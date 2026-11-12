/*
 * tilewin-portal: the login pages of public Wi-Fi ("captive portals"), done
 * without the person having to. When NetworkManager has a network, a plain
 * HTTP page is asked for whose answer is known; a network that is not yet
 * open answers it with its login page instead (a redirect, or a page of its
 * own). That page is then loaded in the background and its way in taken:
 *
 *  - first by tileWin itself: the page is read for its forms and links, the
 *    checkboxes of the terms are ticked (not those of newsletters), and the
 *    button that most says "let me in" in any of ten languages is pressed,
 *    across redirects, meta refreshes and pages of more than one step;
 *  - when that is not enough, as for pages made by scripts, in a headless
 *    Chromium (or Chrome, Brave, Edge, Vivaldi) if one is installed;
 *  - a page that wants something only the person can give (an email, a room
 *    number, a voucher) is opened in their browser instead.
 *
 * A notification says what came of it.
 *
 *   tilewin-portal          watches the networks (tileWin starts it)
 *   tilewin-portal check    checks the network now and does what it would do,
 *                           saying what it did; exit status 0 when online,
 *                           2 when the page wants the person, 1 when it failed,
 *                           3 when there is no network
 *
 * ~/.config/tileWin/portal.conf:
 *   accept yes|no        take the way in of login pages (yes), or only say there is one
 *   check_url URL        the page asked for (http, not https)
 *   check_text TEXT      what it says when the internet is there
 *   browser NAME|PATH|none   the headless browser for pages made by scripts
 *
 * The login pages often have certificates of their own, so their certificates
 * are not checked: nothing is sent to them but the forms of their own pages.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "accept.h"
#include "html.h"
#include "portal.h"
#include "config.h"
#include "tw_paths.h"
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif

#define DEFAULT_CHECK_URL "http://nmcheck.gnome.org/check_network_status.txt"
#define DEFAULT_CHECK_TEXT "NetworkManager is online"
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
	"Chrome/131.0.0.0 Safari/537.36"
#define MAX_STEPS 10
#define MAX_BODY (4 * 1024 * 1024)

struct config {
	bool accept;
	char check_url[1024];
	char check_text[256];
	char browser[1024];
};

static struct config conf;
static bool verbose;

void portal_log(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "tilewin-portal: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static void sleep_ms(int ms) {
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
	}
}

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- the config ---------- */

static void read_config(void) {
	conf.accept = true;
	snprintf(conf.check_url, sizeof(conf.check_url), "%s", DEFAULT_CHECK_URL);
	snprintf(conf.check_text, sizeof(conf.check_text), "%s", DEFAULT_CHECK_TEXT);
	conf.browser[0] = '\0';
	char *dir = tw_config_dir();
	if (dir) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/portal.conf", dir);
		FILE *f = fopen(path, "r");
		char line[2048];
		while (f && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			char *key = line;
			while (isspace((unsigned char)*key)) {
				key++;
			}
			if (*key == '#' || !*key) {
				continue;
			}
			char *value = key + strcspn(key, " \t");
			if (*value) {
				*value++ = '\0';
				while (isspace((unsigned char)*value)) {
					value++;
				}
			}
			if (strcmp(key, "accept") == 0) {
				conf.accept = strcmp(value, "no") != 0 && strcmp(value, "false") != 0 &&
					strcmp(value, "off") != 0;
			} else if (strcmp(key, "check_url") == 0 && *value) {
				snprintf(conf.check_url, sizeof(conf.check_url), "%s", value);
			} else if (strcmp(key, "check_text") == 0 && *value) {
				snprintf(conf.check_text, sizeof(conf.check_text), "%s", value);
			} else if (strcmp(key, "browser") == 0) {
				snprintf(conf.browser, sizeof(conf.browser), "%s", value);
			}
		}
		if (f) {
			fclose(f);
		}
		free(dir);
	}
	// for the tests
	const char *env;
	if ((env = getenv("TILEWIN_PORTAL_CHECK_URL"))) {
		snprintf(conf.check_url, sizeof(conf.check_url), "%s", env);
	}
	if ((env = getenv("TILEWIN_PORTAL_CHECK_TEXT"))) {
		snprintf(conf.check_text, sizeof(conf.check_text), "%s", env);
	}
	if ((env = getenv("TILEWIN_PORTAL_BROWSER"))) {
		snprintf(conf.browser, sizeof(conf.browser), "%s", env);
	}
}

/* ---------- HTTP ---------- */

struct response {
	long code;
	char *url;      // where it ended, after the redirects
	char *location; // a redirect not followed
	char *body;
	size_t len;
	bool failed;
};

static size_t on_body(char *data, size_t size, size_t n, void *user) {
	struct response *r = user;
	size_t bytes = size * n;
	if (r->len + bytes > MAX_BODY) {
		return 0;
	}
	r->body = realloc(r->body, r->len + bytes + 1);
	memcpy(r->body + r->len, data, bytes);
	r->len += bytes;
	r->body[r->len] = '\0';
	return bytes;
}

static void response_clear(struct response *r) {
	free(r->url);
	free(r->location);
	free(r->body);
	memset(r, 0, sizeof(*r));
}

static void setup(CURL *curl, struct response *r) {
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
}

static void finish(CURL *curl, CURLcode rc, struct response *r) {
	r->failed = rc != CURLE_OK;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->code);
	char *url = NULL;
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
	r->url = url ? strdup(url) : NULL;
	char *location = NULL;
	curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
	r->location = location ? strdup(location) : NULL;
	if (!r->body) {
		r->body = strdup("");
	}
}

/* ---------- is the internet there ---------- */

enum check { CHECK_ONLINE, CHECK_PORTAL, CHECK_NONE };

/* Asks the check URL, following nothing; *portal gets the login page's address. */
static enum check check_now(char **portal) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		return CHECK_NONE;
	}
	struct response r = { 0 };
	setup(curl, &r);
	curl_easy_setopt(curl, CURLOPT_URL, conf.check_url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
	// no cache of a proxy on the way may answer for the network
	struct curl_slist *headers = curl_slist_append(NULL, "Cache-Control: no-cache");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	finish(curl, curl_easy_perform(curl), &r);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	enum check result;
	if (r.failed) {
		result = CHECK_NONE;
	} else if ((r.code == 200 && strstr(r.body, conf.check_text)) ||
			(r.code == 204 && !conf.check_text[0])) {
		result = CHECK_ONLINE;
	} else if (r.code >= 300 && r.code < 400 && r.location) {
		result = CHECK_PORTAL;
		if (portal) {
			*portal = strdup(r.location);
		}
	} else {
		// a page of its own instead of the one asked for, or 511
		result = CHECK_PORTAL;
		if (portal) {
			*portal = strdup(conf.check_url);
		}
	}
	response_clear(&r);
	return result;
}

static bool online_now(void *data) {
	return check_now(NULL) == CHECK_ONLINE;
}

/* Waits up to ms for the internet to come: some networks take a moment. */
static bool wait_online(int ms) {
	long long deadline = now_ms() + ms;
	do {
		if (online_now(NULL)) {
			return true;
		}
		sleep_ms(700);
	} while (now_ms() < deadline);
	return false;
}

/* ---------- the way in, by tileWin itself ---------- */

/* GET, or a form sent; follows redirects. Keeps the cookies of the handle. */
static void request(CURL *curl, const struct portal_action *a, const char *url,
		struct response *r) {
	curl_easy_reset(curl);
	setup(curl, r);
	curl_easy_setopt(curl, CURLOPT_COOKIEFILE, ""); // the cookies stay in the handle
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	char *target = NULL;
	char *body = NULL;
	curl_mime *mime = NULL;
	if (a && a->step == PORTAL_SUBMIT) {
		// the pairs, encoded as a form does
		size_t size = 1;
		char *encoded = calloc(1, 1);
		for (int i = 0; i < a->npairs; i++) {
			char *n = curl_easy_escape(curl, a->names[i], 0);
			char *v = curl_easy_escape(curl, a->values[i], 0);
			size += strlen(n) + strlen(v) + 2;
			encoded = realloc(encoded, size);
			if (i) {
				strcat(encoded, "&");
			}
			strcat(encoded, n);
			strcat(encoded, "=");
			strcat(encoded, v);
			curl_free(n);
			curl_free(v);
		}
		if (a->post && a->multipart) {
			mime = curl_mime_init(curl);
			for (int i = 0; i < a->npairs; i++) {
				curl_mimepart *part = curl_mime_addpart(mime);
				curl_mime_name(part, a->names[i]);
				curl_mime_data(part, a->values[i], CURL_ZERO_TERMINATED);
			}
			curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
			free(encoded);
		} else if (a->post) {
			body = encoded;
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		} else {
			// GET: the pairs replace the query of the action
			CURLU *u = curl_url();
			curl_url_set(u, CURLUPART_URL, url, 0);
			curl_url_set(u, CURLUPART_QUERY, encoded[0] ? encoded : NULL, 0);
			char *full = NULL;
			if (curl_url_get(u, CURLUPART_URL, &full, 0) == CURLUE_OK) {
				target = strdup(full);
				curl_free(full);
			}
			curl_url_cleanup(u);
			free(encoded);
		}
	}
	curl_easy_setopt(curl, CURLOPT_URL, target ? target : url);
	finish(curl, curl_easy_perform(curl), r);
	free(target);
	free(body);
	curl_mime_free(mime);
}

/* Whether that step was taken before: a page that comes back the same. */
static bool seen_before(char **seen, int nseen, const char *key) {
	for (int i = 0; i < nseen; i++) {
		if (strcmp(seen[i], key) == 0) {
			return true;
		}
	}
	return false;
}

static enum portal_outcome accept_builtin(const char *start, char **page_out, bool *scripted) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		return PORTAL_FAILED;
	}
	enum portal_outcome outcome = PORTAL_FAILED;
	struct response r = { 0 };
	request(curl, NULL, start, &r);
	char *seen[MAX_STEPS * 2];
	int nseen = 0;
	*scripted = false;
	for (int step = 0; step < MAX_STEPS; step++) {
		if (r.failed && !r.len) {
			portal_log("the page %s did not load", r.url ? r.url : start);
			break;
		}
		const char *base = r.url ? r.url : start;
		free(*page_out);
		*page_out = strdup(base);
		struct html_page *page = html_parse(r.body);
		struct portal_action a = portal_decide(page, base);
		if (verbose) {
			portal_log("page %s (\"%s\"): %d forms, %d links: %s", base, page->title,
				page->nforms, page->nlinks, a.what ? a.what : "nothing to do");
		}
		// a page with scripts and nothing to take: for the browser
		*scripted = a.step == PORTAL_NOTHING && strcasestr(r.body, "<script");
		html_free(page);
		if (a.step == PORTAL_NOTHING) {
			portal_action_clear(&a);
			break;
		}
		if (a.step == PORTAL_NEEDS_INPUT) {
			portal_log("%s wants something typed in", base);
			outcome = PORTAL_NEEDS_YOU;
			portal_action_clear(&a);
			break;
		}
		char key[2048];
		snprintf(key, sizeof(key), "%s %s", a.url, a.what ? a.what : "");
		if (seen_before(seen, nseen, key)) {
			portal_log("the same page came back: %s", base);
			*scripted = true; // its button may well work by a script
			portal_action_clear(&a);
			break;
		}
		seen[nseen++] = strdup(key);
		portal_log("step %d: %s (%s %s)", step + 1, a.what ? a.what : "?",
			a.step == PORTAL_SUBMIT ? (a.post ? "POST" : "GET") : "GET", a.url);
		char *url = strdup(a.url);
		response_clear(&r);
		request(curl, &a, url, &r);
		free(url);
		portal_action_clear(&a);
		if (online_now(NULL)) {
			outcome = PORTAL_ACCEPTED;
			break;
		}
	}
	if (outcome == PORTAL_FAILED && nseen > 0 && wait_online(4000)) {
		outcome = PORTAL_ACCEPTED;
	}
	for (int i = 0; i < nseen; i++) {
		free(seen[i]);
	}
	response_clear(&r);
	curl_easy_cleanup(curl);
	return outcome;
}

/* ---------- opening a page for the person ---------- */

static void open_in_browser(const char *url) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			int null = open("/dev/null", O_RDWR);
			if (null >= 0) {
				dup2(null, 0);
				dup2(null, 1);
			}
			execlp("xdg-open", "xdg-open", url, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

/* ---------- notifications ---------- */

#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
static sd_bus *user_bus;
static uint32_t open_id;    // the notification whose "Open the page" opens open_url
static char *open_url;

static void notify(const char *summary, const char *body, const char *url) {
	if (!user_bus && sd_bus_open_user(&user_bus) < 0) {
		user_bus = NULL;
		return;
	}
	sd_bus_message *m = NULL, *reply = NULL;
	if (sd_bus_message_new_method_call(user_bus, &m, "org.freedesktop.Notifications",
			"/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify") < 0) {
		return;
	}
	sd_bus_message_append(m, "susss", "Network", (uint32_t)0, "network-wireless", summary, body);
	sd_bus_message_open_container(m, 'a', "s");
	if (url) {
		sd_bus_message_append(m, "ss", "open", "Open the page");
	}
	sd_bus_message_close_container(m);
	sd_bus_message_open_container(m, 'a', "{sv}");
	sd_bus_message_close_container(m);
	sd_bus_message_append(m, "i", (int32_t)-1);
	if (sd_bus_call(user_bus, m, 2000, NULL, &reply) >= 0) {
		uint32_t id = 0;
		sd_bus_message_read(reply, "u", &id);
		if (url) {
			open_id = id;
			free(open_url);
			open_url = strdup(url);
		}
	}
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
}

static int on_action(sd_bus_message *m, void *data, sd_bus_error *error) {
	uint32_t id;
	const char *key;
	if (sd_bus_message_read(m, "us", &id, &key) >= 0 && id == open_id && open_url &&
			strcmp(key, "open") == 0) {
		open_in_browser(open_url);
	}
	return 0;
}
#else
static void notify(const char *summary, const char *body, const char *url) {
}
#endif

/* ---------- one check ---------- */

/* Checks the network and takes its login page; names it after network. */
static enum portal_outcome run_check(const char *network, bool tell, bool open) {
	read_config();
	char *portal = NULL;
	enum check state = check_now(&portal);
	if (state != CHECK_PORTAL) {
		free(portal);
		return state == CHECK_ONLINE ? PORTAL_ONLINE : PORTAL_NO_NETWORK;
	}
	portal_log("%s has a login page: %s", network ? network : "the network", portal);
	char title[256];
	snprintf(title, sizeof(title), "%s", network && network[0] ? network : "Wi-Fi");
	if (!conf.accept) {
		if (tell) {
			notify(title, "This network wants you to log in on its page.", portal);
		}
		free(portal);
		return PORTAL_NEEDS_YOU;
	}
	char *page = NULL;
	bool scripted = false;
	enum portal_outcome outcome = accept_builtin(portal, &page, &scripted);
	if (outcome == PORTAL_FAILED) {
		char *browser = portal_find_chromium(conf.browser);
		if (browser) {
			char *where = NULL;
			outcome = portal_chromium(browser, page ? page : portal, online_now, NULL, &where);
			if (where) {
				free(page);
				page = where;
			}
			free(browser);
		} else if (scripted) {
			portal_log("the page is made by scripts, and no Chromium is installed for it");
		}
	}
	const char *show = page ? page : portal;
	switch (outcome) {
	case PORTAL_ACCEPTED:
		portal_log("accepted: the internet is there");
		if (tell) {
			notify(title, "Accepted the login page of this network: you are online.", NULL);
		}
		break;
	case PORTAL_NEEDS_YOU:
		portal_log("opening %s for you", show);
		if (open) {
			open_in_browser(show);
		}
		if (tell) {
			notify(title, "The login page of this network wants something from you, so it "
				"was opened.", show);
		}
		break;
	default:
		// the person has to do it then: the page is there for them
		portal_log("could not get past %s: opening it for you", show);
		if (open) {
			open_in_browser(show);
		}
		if (tell) {
			notify(title, "The login page of this network could not be accepted by itself, "
				"so it was opened.", show);
		}
		break;
	}
	free(page);
	free(portal);
	return outcome;
}

/* ---------- watching NetworkManager ---------- */

#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
#define NM_CONNECTED_LOCAL 50

static sd_bus *system_bus;
static long long check_at;      // when to check, 0 for not
static char handled[512];       // the connection checked last, "" after a disconnect
static int nm_state;

static char *nm_string(const char *path, const char *iface, const char *prop, char type) {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	char *value = NULL;
	if (type == 'o') {
		sd_bus_message *reply = NULL;
		if (sd_bus_get_property(system_bus, "org.freedesktop.NetworkManager", path, iface, prop,
				&error, &reply, "o") >= 0) {
			const char *v;
			if (sd_bus_message_read(reply, "o", &v) >= 0) {
				value = strdup(v);
			}
		}
		sd_bus_message_unref(reply);
	} else {
		sd_bus_get_property_string(system_bus, "org.freedesktop.NetworkManager", path, iface,
			prop, &error, &value);
	}
	sd_bus_error_free(&error);
	return value;
}

/* What NetworkManager has now: a check soon for a connection not checked yet. */
static void nm_changed(void) {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	uint32_t state = 0;
	sd_bus_get_property_trivial(system_bus, "org.freedesktop.NetworkManager",
		"/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", "State", &error,
		'u', &state);
	sd_bus_error_free(&error);
	nm_state = (int)state;
	if (nm_state < NM_CONNECTED_LOCAL) {
		handled[0] = '\0';
		check_at = 0;
		return;
	}
	char *primary = nm_string("/org/freedesktop/NetworkManager",
		"org.freedesktop.NetworkManager", "PrimaryConnection", 'o');
	if (primary && strcmp(primary, "/") != 0 && strcmp(primary, handled) != 0) {
		snprintf(handled, sizeof(handled), "%s", primary);
		check_at = now_ms() + 2000; // let DHCP and DNS settle
	}
	free(primary);
}

static int on_nm_signal(sd_bus_message *m, void *data, sd_bus_error *error) {
	nm_changed();
	return 0;
}

static void check_connection(void) {
	char *name = handled[0] ? nm_string(handled,
		"org.freedesktop.NetworkManager.Connection.Active", "Id", 's') : NULL;
	enum portal_outcome o = run_check(name, true, true);
	if (o == PORTAL_NO_NETWORK) {
		// no answer yet: once more a little later, then it is left
		static int retries;
		if (retries++ < 3) {
			check_at = now_ms() + 5000;
		} else {
			retries = 0;
		}
	}
	free(name);
}
#endif

static bool single_instance(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return true;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/tilewin-portal-%s.lock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	return fd < 0 || flock(fd, LOCK_EX | LOCK_NB) == 0; // the fd stays open
}

static int watch(void) {
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	if (!single_instance()) {
		return 0;
	}
	// it goes with the session: the Wayland display going is the sign
	struct wl_display *display = wl_display_connect(NULL);
	if (sd_bus_open_system(&system_bus) < 0) {
		portal_log("no system bus: nothing to watch");
		return 1;
	}
	sd_bus_match_signal(system_bus, NULL, "org.freedesktop.NetworkManager",
		"/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", "StateChanged",
		on_nm_signal, NULL);
	sd_bus_match_signal(system_bus, NULL, "org.freedesktop.NetworkManager",
		"/org/freedesktop/NetworkManager", "org.freedesktop.DBus.Properties",
		"PropertiesChanged", on_nm_signal, NULL);
	if (sd_bus_open_user(&user_bus) >= 0) {
		sd_bus_match_signal(user_bus, NULL, NULL, "/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", "ActionInvoked", on_action, NULL);
	} else {
		user_bus = NULL;
	}
	nm_changed();
	while (true) {
		while (sd_bus_process(system_bus, NULL) > 0) {
		}
		while (user_bus && sd_bus_process(user_bus, NULL) > 0) {
		}
		if (check_at && now_ms() >= check_at) {
			check_at = 0;
			check_connection();
			continue;
		}
		struct pollfd fds[3] = {
			{ sd_bus_get_fd(system_bus), POLLIN, 0 },
			{ user_bus ? sd_bus_get_fd(user_bus) : -1, POLLIN, 0 },
			{ display ? wl_display_get_fd(display) : -1, POLLIN, 0 },
		};
		int timeout = check_at ? (int)(check_at - now_ms()) : -1;
		if (check_at && timeout < 0) {
			timeout = 0;
		}
		if (display) {
			wl_display_flush(display);
		}
		if (poll(fds, 3, timeout) < 0 && errno != EINTR) {
			break;
		}
		if (fds[2].revents & (POLLHUP | POLLERR)) {
			break;
		}
		if ((fds[2].revents & POLLIN) && wl_display_dispatch(display) < 0) {
			break;
		}
	}
	return 0;
#else
	portal_log("built without sd-bus: it cannot watch NetworkManager");
	return 1;
#endif
}

int main(int argc, char **argv) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	verbose = getenv("TILEWIN_PORTAL_DEBUG") != NULL;
	if (argc >= 2 && strcmp(argv[1], "check") == 0) {
		verbose = true;
		enum portal_outcome o = run_check(argc >= 3 ? argv[2] : NULL,
			getenv("TILEWIN_PORTAL_NOTIFY") != NULL, true);
		static const char *const said[] = { "online", "accepted", "needs-you", "failed",
			"no-network" };
		printf("%s\n", said[o]);
		return o == PORTAL_ONLINE || o == PORTAL_ACCEPTED ? 0 :
			o == PORTAL_NEEDS_YOU ? 2 : o == PORTAL_NO_NETWORK ? 3 : 1;
	}
	if (argc >= 2) {
		fprintf(stderr, "usage: %s [check]\n", argv[0]);
		return 2;
	}
	return watch();
}
