/*
 * A headless Chromium driven over the DevTools protocol on a pipe
 * (--remote-debugging-pipe): commands go in on its fd 3, answers and events
 * come out of its fd 4, each a JSON message ended by a NUL. No WebSocket and
 * no port that another program could reach.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <json.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "accept_js.h"
#include "portal.h"

struct cdp {
	pid_t pid;
	int in, out; // we write to in, read from out
	char *buf;
	size_t len, cap;
	int next_id;
	char session[128];
};

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
	}
}

char *portal_find_chromium(const char *configured) {
	if (configured && configured[0]) {
		if (strcmp(configured, "none") == 0) {
			return NULL;
		}
		if (strchr(configured, '/')) {
			return access(configured, X_OK) == 0 ? strdup(configured) : NULL;
		}
	}
	static const char *const names[] = { "chromium", "chromium-browser", "google-chrome-stable",
		"google-chrome", "brave", "brave-browser", "microsoft-edge-stable", "vivaldi-stable" };
	const char *path = getenv("PATH");
	if (!path) {
		return NULL;
	}
	size_t count = sizeof(names) / sizeof(names[0]);
	for (size_t i = configured && configured[0] ? 0 : 0; i < count + 1; i++) {
		const char *name = i == 0 ? (configured && configured[0] ? configured : NULL) : names[i - 1];
		if (!name) {
			continue;
		}
		char *copy = strdup(path);
		char *save = NULL;
		for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", dir, name);
			if (access(full, X_OK) == 0) {
				free(copy);
				return strdup(full);
			}
		}
		free(copy);
	}
	return NULL;
}

static bool cdp_send(struct cdp *c, json_object *msg) {
	const char *text = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
	size_t n = strlen(text) + 1; // with its NUL
	const char *p = text;
	while (n > 0) {
		ssize_t w = write(c->in, p, n);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

/* The next message, or NULL when none came before the deadline. */
static json_object *cdp_read(struct cdp *c, long long deadline) {
	while (true) {
		char *nul = c->len ? memchr(c->buf, '\0', c->len) : NULL;
		if (nul) {
			json_object *msg = json_tokener_parse(c->buf);
			size_t used = (size_t)(nul - c->buf) + 1;
			memmove(c->buf, c->buf + used, c->len - used);
			c->len -= used;
			if (msg) {
				return msg;
			}
			continue;
		}
		long long left = deadline - now_ms();
		if (left <= 0) {
			return NULL;
		}
		struct pollfd pfd = { c->out, POLLIN, 0 };
		int r = poll(&pfd, 1, (int)(left > 1000 ? 1000 : left));
		if (r < 0 && errno != EINTR) {
			return NULL;
		}
		if (r <= 0) {
			continue;
		}
		if (c->len + 65536 > c->cap) {
			c->cap = c->cap ? c->cap * 2 : 131072;
			if (c->cap > 64 * 1024 * 1024) {
				return NULL; // nothing a login page sends is this big
			}
			c->buf = realloc(c->buf, c->cap);
		}
		ssize_t n = read(c->out, c->buf + c->len, c->cap - c->len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return NULL; // Chromium is gone
		}
		c->len += (size_t)n;
	}
}

/* Sends a command and waits for its answer; its "result", or NULL. */
static json_object *cdp_call(struct cdp *c, const char *method, json_object *params,
		bool in_session, int timeout_ms) {
	int id = ++c->next_id;
	json_object *msg = json_object_new_object();
	json_object_object_add(msg, "id", json_object_new_int(id));
	json_object_object_add(msg, "method", json_object_new_string(method));
	json_object_object_add(msg, "params", params ? params : json_object_new_object());
	if (in_session && c->session[0]) {
		json_object_object_add(msg, "sessionId", json_object_new_string(c->session));
	}
	bool sent = cdp_send(c, msg);
	json_object_put(msg);
	if (!sent) {
		return NULL;
	}
	long long deadline = now_ms() + timeout_ms;
	while (true) {
		json_object *answer = cdp_read(c, deadline);
		if (!answer) {
			return NULL;
		}
		json_object *jid;
		if (json_object_object_get_ex(answer, "id", &jid) && json_object_get_int(jid) == id) {
			json_object *result = NULL, *error;
			if (json_object_object_get_ex(answer, "error", &error)) {
				portal_log("chromium: %s: %s", method, json_object_to_json_string(error));
			} else if (json_object_object_get_ex(answer, "result", &result)) {
				json_object_get(result);
			}
			json_object_put(answer);
			return result;
		}
		json_object_put(answer); // an event
	}
}

/* Runs a script in the page; its string value, newly allocated, or NULL. */
static char *cdp_eval(struct cdp *c, const char *script, int timeout_ms) {
	json_object *params = json_object_new_object();
	json_object_object_add(params, "expression", json_object_new_string(script));
	json_object_object_add(params, "returnByValue", json_object_new_boolean(true));
	json_object_object_add(params, "awaitPromise", json_object_new_boolean(true));
	json_object *result = cdp_call(c, "Runtime.evaluate", params, true, timeout_ms);
	char *value = NULL;
	json_object *inner, *v;
	if (result && json_object_object_get_ex(result, "result", &inner) &&
			json_object_object_get_ex(inner, "value", &v) &&
			json_object_is_type(v, json_type_string)) {
		value = strdup(json_object_get_string(v));
	}
	if (result) {
		json_object_put(result);
	}
	return value;
}

/* Waits until the page has loaded, and a little longer for its scripts. */
static void wait_loaded(struct cdp *c, int timeout_ms) {
	long long deadline = now_ms() + timeout_ms;
	while (now_ms() < deadline) {
		char *state = cdp_eval(c, "document.readyState", 3000);
		bool done = state && strcmp(state, "complete") == 0;
		free(state);
		if (done) {
			break;
		}
		sleep_ms(300);
	}
	sleep_ms(1200);
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
	return remove(path);
}

static bool cdp_start(struct cdp *c, const char *browser, char *profile) {
	int to[2], from[2];
	if (pipe2(to, O_CLOEXEC) < 0) {
		return false;
	}
	if (pipe2(from, O_CLOEXEC) < 0) {
		close(to[0]);
		close(to[1]);
		return false;
	}
	char user_data[PATH_MAX + 32];
	snprintf(user_data, sizeof(user_data), "--user-data-dir=%s", profile);
	pid_t pid = fork();
	if (pid == 0) {
		// its fd 3 reads what we write, its fd 4 writes what we read
		int in = dup(to[0]), out = dup(from[1]);
		dup2(in, 3);
		dup2(out, 4);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			if (!getenv("TILEWIN_PORTAL_DEBUG")) {
				dup2(null, 2);
			}
		}
		execl(browser, browser, "--headless=new", "--remote-debugging-pipe", user_data,
			"--no-first-run", "--no-default-browser-check", "--disable-extensions",
			"--disable-sync", "--disable-background-networking", "--disable-component-update",
			"--disable-default-apps", "--mute-audio", "--window-size=1280,900",
			// login pages often have certificates of their own
			"--ignore-certificate-errors", "about:blank", (char *)NULL);
		_exit(127);
	}
	close(to[0]);
	close(from[1]);
	if (pid < 0) {
		close(to[1]);
		close(from[0]);
		return false;
	}
	c->pid = pid;
	c->in = to[1];
	c->out = from[0];
	return true;
}

static void cdp_stop(struct cdp *c) {
	if (c->pid > 0) {
		cdp_call(c, "Browser.close", NULL, false, 2000);
		close(c->in);
		close(c->out);
		for (int i = 0; i < 30; i++) {
			if (waitpid(c->pid, NULL, WNOHANG) == c->pid) {
				c->pid = 0;
				break;
			}
			sleep_ms(100);
		}
		if (c->pid > 0) {
			kill(c->pid, SIGKILL);
			waitpid(c->pid, NULL, 0);
		}
	}
	free(c->buf);
}

enum portal_outcome portal_chromium(const char *browser, const char *url,
		portal_online_fn online, void *data, char **page) {
	char profile[] = "/tmp/tilewin-portal-XXXXXX";
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char profile_rt[PATH_MAX];
	char *dir = NULL;
	if (runtime) {
		snprintf(profile_rt, sizeof(profile_rt), "%s/tilewin-portal-XXXXXX", runtime);
		dir = mkdtemp(profile_rt);
	}
	if (!dir) {
		dir = mkdtemp(profile);
	}
	if (!dir) {
		return PORTAL_FAILED;
	}
	struct cdp c = { 0 };
	enum portal_outcome outcome = PORTAL_FAILED;
	portal_log("chromium: %s opens %s", browser, url);
	if (!cdp_start(&c, browser, dir)) {
		nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
		return PORTAL_FAILED;
	}
	json_object *params = json_object_new_object();
	json_object_object_add(params, "url", json_object_new_string("about:blank"));
	json_object *target = cdp_call(&c, "Target.createTarget", params, false, 20000);
	json_object *id;
	if (!target || !json_object_object_get_ex(target, "targetId", &id)) {
		portal_log("chromium: it did not start");
		goto done;
	}
	params = json_object_new_object();
	json_object_object_add(params, "targetId", json_object_new_string(json_object_get_string(id)));
	json_object_object_add(params, "flatten", json_object_new_boolean(true));
	json_object *attached = cdp_call(&c, "Target.attachToTarget", params, false, 10000);
	json_object *session;
	if (!attached || !json_object_object_get_ex(attached, "sessionId", &session)) {
		if (attached) {
			json_object_put(attached);
		}
		goto done;
	}
	snprintf(c.session, sizeof(c.session), "%s", json_object_get_string(session));
	json_object_put(attached);
	params = json_object_new_object();
	json_object_object_add(params, "url", json_object_new_string(url));
	json_object *nav = cdp_call(&c, "Page.navigate", params, true, 20000);
	if (nav) {
		json_object_put(nav);
	}
	wait_loaded(&c, 15000);
	int idle = 0;
	for (int round = 0; round < 8 && idle < 2; round++) {
		char *result = cdp_eval(&c, portal_accept_js, 10000);
		portal_log("chromium: round %d: %s", round + 1, result ? result : "(no answer)");
		if (result && strncmp(result, "needs-input:", 12) == 0) {
			*page = strdup(result + 12);
			free(result);
			outcome = PORTAL_NEEDS_YOU;
			goto done;
		}
		bool clicked = result && strncmp(result, "clicked:", 8) == 0;
		free(result);
		idle = clicked ? 0 : idle + 1;
		// what was pressed may load another page, or ask the network in the background
		sleep_ms(clicked ? 2500 : 2000);
		if (online(data)) {
			outcome = PORTAL_ACCEPTED;
			goto done;
		}
		wait_loaded(&c, 10000);
	}
	char *where = cdp_eval(&c, "location.href", 3000);
	if (where) {
		*page = where;
	}
done:
	if (target) {
		json_object_put(target);
	}
	cdp_stop(&c);
	nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return outcome;
}
