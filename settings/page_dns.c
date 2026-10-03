#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include "settings.h"

/*
 * DNS page: the DNS service of tileWin (dns/main.c). Its settings are in
 * /etc/tileWin/dns.conf, which everyone may read and only root may write, so
 * every change goes through tilewin-dns-apply with pkexec (polkit lets
 * administrators at the computer do that without a password). What the
 * service is doing comes from /run/tilewin-dns: the servers it asks with
 * their times, today's counts, the block lists and the names it prefetches.
 *
 * For tests: TILEWIN_DNS_APPLY is a helper run as it is, without pkexec,
 * TILEWIN_DNS_CONF the config, TILEWIN_DNS_RUN the status directory and
 * TILEWIN_NM_CONF_DIR NetworkManager's conf.d.
 */

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

const struct dns_preset dns_server_presets[] = {
	{ "1.1.1.1", "Cloudflare" },
	{ "1.0.0.1", "Cloudflare" },
	{ "2606:4700:4700::1111", "Cloudflare (IPv6)" },
	{ "9.9.9.9", "Quad9" },
	{ "149.112.112.112", "Quad9" },
	{ "2620:fe::fe", "Quad9 (IPv6)" },
	{ "8.8.8.8", "Google" },
	{ "8.8.4.4", "Google" },
	{ "2001:4860:4860::8888", "Google (IPv6)" },
	{ "208.67.222.222", "OpenDNS" },
	{ "208.67.220.220", "OpenDNS" },
	{ "94.140.14.14", "AdGuard, blocks ads" },
	{ "94.140.15.15", "AdGuard, blocks ads" },
	{ "194.242.2.2", "Mullvad" },
	{ "84.200.69.80", "DNS.WATCH" },
	{ "76.76.2.0", "Control D" },
	{ "185.228.168.9", "CleanBrowsing, family filter" },
	{ "4.2.2.1", "Level3" },
	{ NULL, NULL },
};

static const struct dns_preset list_presets[] = {
	{ "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts",
		"StevenBlack's hosts, the default of Pi-hole" },
	{ "https://small.oisd.nl/", "OISD small" },
	{ "https://big.oisd.nl/", "OISD big" },
	{ "https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/adblock/light.txt",
		"HaGeZi Light" },
	{ "https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/adblock/multi.txt",
		"HaGeZi Normal" },
	{ "https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/adblock/pro.txt",
		"HaGeZi Pro" },
	{ "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt",
		"AdGuard DNS filter" },
	{ "https://raw.githubusercontent.com/StevenBlack/hosts/master/alternates/gambling-only/hosts",
		"StevenBlack's gambling sites" },
	{ NULL, NULL },
};

const char *dns_preset_name(const struct dns_preset *presets, const char *value) {
	for (const struct dns_preset *p = presets ? presets : dns_server_presets; p->value; p++) {
		if (strcmp(p->value, value) == 0) {
			return p->name;
		}
	}
	return NULL;
}

/* ---------- the config ---------- */

struct dnsconf {
	GPtrArray *servers; // the servers for all networks, also while they are not used
	bool use_servers;
	int fastest, test_interval;
	bool cache;
	int cache_min, cache_max, cache_size;
	bool prefetch;
	int prefetch_percent, prefetch_days;
	GPtrArray *prefetch_add, *prefetch_skip;
	bool blocking, nxdomain;
	GPtrArray *lists, *allow, *block;
};

const char *dns_conf_path(void) {
	const char *path = g_getenv("TILEWIN_DNS_CONF");
	return path && *path ? path : "/etc/tileWin/dns.conf";
}

static const char *run_dir(void) {
	const char *dir = g_getenv("TILEWIN_DNS_RUN");
	return dir && *dir ? dir : "/run/tilewin-dns";
}

static void conf_defaults(struct dnsconf *c) {
	GPtrArray **arrays[] = { &c->servers, &c->prefetch_add, &c->prefetch_skip, &c->lists,
		&c->allow, &c->block };
	for (size_t i = 0; i < G_N_ELEMENTS(arrays); i++) {
		if (*arrays[i]) {
			g_ptr_array_set_size(*arrays[i], 0);
		} else {
			*arrays[i] = g_ptr_array_new_with_free_func(g_free);
		}
	}
	c->use_servers = false;
	c->fastest = 2;
	c->test_interval = 3600;
	c->cache = true;
	c->cache_min = 0;
	c->cache_max = 86400;
	c->cache_size = 10000;
	c->prefetch = true;
	c->prefetch_percent = 5;
	c->prefetch_days = 7;
	c->blocking = true;
	c->nxdomain = false;
}

static bool has(GPtrArray *a, const char *value) {
	for (guint i = 0; i < a->len; i++) {
		if (strcmp(a->pdata[i], value) == 0) {
			return true;
		}
	}
	return false;
}

static void add_words(GPtrArray *a, char **words) {
	for (int i = 1; words[i]; i++) {
		if (*words[i] && !has(a, words[i])) {
			g_ptr_array_add(a, g_strdup(words[i]));
		}
	}
}

static int word_number(char **words, int fallback) {
	return words[1] ? atoi(words[1]) : fallback;
}

static bool word_yes(char **words) {
	return words[1] && (strcmp(words[1], "yes") == 0 || strcmp(words[1], "on") == 0);
}

/* The servers of a config that is not in use are kept in a comment. */
#define UNUSED_SERVERS "#unused-servers"

static void conf_read(struct dnsconf *c) {
	conf_defaults(c);
	char *text = NULL;
	if (!g_file_get_contents(dns_conf_path(), &text, NULL, NULL)) {
		return;
	}
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *line = g_strstrip(lines[i]);
		char **words = g_strsplit_set(line, " \t", -1);
		// splitting on each blank leaves empty words between two of them
		int n = 0;
		for (int k = 0; words[k]; k++) {
			if (*words[k]) {
				words[n++] = words[k];
			} else {
				g_free(words[k]);
			}
		}
		words[n] = NULL;
		const char *key = words[0];
		if (!key) {
			// empty
		} else if (strcmp(key, "servers") == 0 || strcmp(key, "server") == 0) {
			add_words(c->servers, words);
			c->use_servers = c->servers->len > 0;
		} else if (strcmp(key, UNUSED_SERVERS) == 0) {
			add_words(c->servers, words);
		} else if (strcmp(key, "fastest") == 0) {
			c->fastest = word_number(words, 2);
		} else if (strcmp(key, "test_interval") == 0) {
			c->test_interval = word_number(words, 3600);
		} else if (strcmp(key, "cache") == 0) {
			c->cache = word_yes(words);
		} else if (strcmp(key, "cache_min") == 0) {
			c->cache_min = word_number(words, 0);
		} else if (strcmp(key, "cache_max") == 0) {
			c->cache_max = word_number(words, 86400);
		} else if (strcmp(key, "cache_size") == 0) {
			c->cache_size = word_number(words, 10000);
		} else if (strcmp(key, "prefetch") == 0) {
			c->prefetch = word_yes(words);
		} else if (strcmp(key, "prefetch_percent") == 0) {
			c->prefetch_percent = word_number(words, 5);
		} else if (strcmp(key, "prefetch_days") == 0) {
			c->prefetch_days = word_number(words, 7);
		} else if (strcmp(key, "prefetch_add") == 0) {
			add_words(c->prefetch_add, words);
		} else if (strcmp(key, "prefetch_skip") == 0) {
			add_words(c->prefetch_skip, words);
		} else if (strcmp(key, "blocking") == 0) {
			c->blocking = word_yes(words);
		} else if (strcmp(key, "block_answer") == 0) {
			c->nxdomain = words[1] && strcmp(words[1], "nxdomain") == 0;
		} else if (strcmp(key, "blocklist") == 0) {
			add_words(c->lists, words);
		} else if (strcmp(key, "allow") == 0) {
			add_words(c->allow, words);
		} else if (strcmp(key, "block") == 0) {
			add_words(c->block, words);
		}
		g_strfreev(words);
	}
	g_strfreev(lines);
	g_free(text);
}

static void write_list(GString *out, const char *key, GPtrArray *a) {
	for (guint i = 0; i < a->len; i++) {
		g_string_append_printf(out, "%s %s\n", key, (char *)a->pdata[i]);
	}
}

static char *conf_text(const struct dnsconf *c) {
	GString *out = g_string_new("# The DNS service of tileWin, written by its settings app.\n"
		"# See docs/configuration.md for what each line means.\n\n");
	if (c->servers->len > 0) {
		g_string_append(out, c->use_servers ? "servers" : UNUSED_SERVERS);
		for (guint i = 0; i < c->servers->len; i++) {
			g_string_append_printf(out, " %s", (char *)c->servers->pdata[i]);
		}
		g_string_append_c(out, '\n');
	}
	g_string_append_printf(out, "fastest %d\ntest_interval %d\n\n", c->fastest,
		c->test_interval);
	g_string_append_printf(out, "cache %s\ncache_min %d\ncache_max %d\ncache_size %d\n\n",
		c->cache ? "yes" : "no", c->cache_min, c->cache_max, c->cache_size);
	g_string_append_printf(out, "prefetch %s\nprefetch_percent %d\nprefetch_days %d\n",
		c->prefetch ? "yes" : "no", c->prefetch_percent, c->prefetch_days);
	write_list(out, "prefetch_add", c->prefetch_add);
	write_list(out, "prefetch_skip", c->prefetch_skip);
	g_string_append_printf(out, "\nblocking %s\nblock_answer %s\n", c->blocking ? "yes" : "no",
		c->nxdomain ? "nxdomain" : "null");
	write_list(out, "blocklist", c->lists);
	write_list(out, "allow", c->allow);
	write_list(out, "block", c->block);
	return g_string_free(out, FALSE);
}

/* ---------- the helper ---------- */

static char *helper_path(void) {
	const char *test = g_getenv("TILEWIN_DNS_APPLY");
	if (test && *test) {
		return g_strdup(test);
	}
	return g_build_filename(TILEWIN_LIBEXECDIR, "tilewin-dns-apply", NULL);
}

bool dns_service_installed(void) {
	char *path = helper_path();
	bool ok = g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
	g_free(path);
	return ok;
}

/* Whether DNS is sent through the service: the file the helper gives NetworkManager. */
bool dns_service_enabled(void) {
	const char *dir = g_getenv("TILEWIN_NM_CONF_DIR");
	char *path = g_build_filename(dir && *dir ? dir : "/etc/NetworkManager/conf.d",
		"zz-tilewin-dns.conf", NULL);
	bool on = g_file_test(path, G_FILE_TEST_EXISTS);
	g_free(path);
	return on;
}

/*
 * The servers NetworkManager has for all networks in files of its own (not
 * the one of the service), "" if none. While the service is off they win
 * over the servers of single networks.
 */
char *dns_networkmanager_global(void) {
	const char *dir = g_getenv("TILEWIN_NM_CONF_DIR");
	GDir *d = g_dir_open(dir && *dir ? dir : "/etc/NetworkManager/conf.d", 0, NULL);
	GString *out = g_string_new(NULL);
	const char *name;
	while (d && (name = g_dir_read_name(d))) {
		if (strcmp(name, "zz-tilewin-dns.conf") == 0 || !g_str_has_suffix(name, ".conf")) {
			continue;
		}
		char *path = g_build_filename(dir && *dir ? dir : "/etc/NetworkManager/conf.d", name,
			NULL);
		GKeyFile *kf = g_key_file_new();
		if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
			char *servers = g_key_file_get_string(kf, "global-dns-domain-*", "servers", NULL);
			if (servers && *servers) {
				g_string_append_printf(out, "%s%s (%s)", out->len ? "; " : "", servers, name);
			}
			g_free(servers);
		}
		g_key_file_free(kf);
		g_free(path);
	}
	if (d) {
		g_dir_close(d);
	}
	return g_string_free(out, FALSE);
}

struct dns_page;

struct helper_call {
	struct dns_page *p;
	char *what;
	void (*done)(struct dns_page *p, bool ok, const char *output);
};

static void set_message(struct dns_page *p, const char *text);
static void refresh_status(struct dns_page *p);

static void helper_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct helper_call *call = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		&error) && g_subprocess_get_successful(G_SUBPROCESS(source));
	if (!ok) {
		int status = g_subprocess_get_if_exited(G_SUBPROCESS(source)) ?
			g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
		// 126/127: pkexec was refused or found no agent to ask
		char *text = g_strdup_printf("Could not %s: %s", call->what,
			error ? error->message : err && *err ? g_strstrip(err) :
			status == 126 || status == 127 ? "not authorized" : "the helper failed");
		set_message(call->p, text);
		g_free(text);
	}
	if (call->done) {
		call->done(call->p, ok, out);
	}
	refresh_status(call->p);
	g_clear_error(&error);
	g_free(out);
	g_free(err);
	g_free(call->what);
	g_free(call);
}

/* Runs the helper with args (and input on stdin); what is said when it fails. */
static void run_helper(struct dns_page *p, const char *what, const char *input,
		void (*done)(struct dns_page *, bool, const char *), const char *arg1, const char *arg2) {
	char *helper = helper_path();
	const char *argv[6];
	int n = 0;
	const char *test = g_getenv("TILEWIN_DNS_APPLY");
	if (!test || !*test) {
		argv[n++] = "pkexec";
	}
	argv[n++] = helper;
	argv[n++] = arg1;
	if (arg2) {
		argv[n++] = arg2;
	}
	argv[n] = NULL;
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_free(helper);
	if (!proc) {
		char *text = g_strdup_printf("Could not %s: %s", what, error->message);
		set_message(p, text);
		g_free(text);
		g_error_free(error);
		if (done) {
			done(p, false, NULL);
		}
		return;
	}
	struct helper_call *call = g_new0(struct helper_call, 1);
	call->p = p;
	call->what = g_strdup(what);
	call->done = done;
	g_subprocess_communicate_utf8_async(proc, input ? input : "", NULL, helper_finished, call);
	g_object_unref(proc);
}

/* ---------- the page ---------- */

/* A list of words the page edits: servers, block lists, names. */
struct word_list {
	struct dns_page *p;
	GtkWidget *list, *add_row, *pick, *entry_row, *entry, *empty_row;
	GPtrArray **values;                // in the config
	const struct dns_preset *presets;  // to pick from, NULL: only typed
	GPtrArray *picks;                  // the values behind the dropdown's items
	bool (*valid)(const char *text);
	void (*describe)(struct word_list *w, const char *value, char **title, char **subtitle);
	const char *remove_tooltip;
	GPtrArray *rows;
	char *signature; // what the rows show, to leave them be when it is the same
	bool syncing;
};

struct dns_page {
	struct settings *s;
	bool updating, busy;
	struct dnsconf c;
	guint save_timer, status_timer;
	GtkWidget *service_switch, *service_sub, *today_label, *message, *nm_note;
	GtkWidget *mode_dd, *fastest_dd, *fastest_row, *interval_dd, *interval_row, *test_row;
	GtkWidget *servers_box;
	GtkWidget *cache_switch, *min_dd, *max_dd, *size_dd, *min_row, *max_row, *size_row;
	GtkWidget *prefetch_switch, *percent_dd, *days_dd, *percent_row, *days_row;
	GtkWidget *prefetched_list;
	GPtrArray *prefetched_rows;
	GtkWidget *blocking_switch, *answer_dd, *answer_row, *update_row;
	struct word_list *servers, *lists, *always, *never, *allow, *block;
	GHashTable *server_ms;  // server -> ms (-1 untested)
	GHashTable *in_use;     // server -> "use" or "backup"
	GHashTable *list_info;  // url -> "count mtime state"
	char *prefetch_mtime;
	bool running;
};

static void set_message(struct dns_page *p, const char *text) {
	gtk_label_set_text(GTK_LABEL(p->message), text ? text : "");
	gtk_widget_set_visible(p->message, text && *text);
}

static GtkWidget *subtitle_of(GtkWidget *row) {
	GtkWidget *labels = gtk_widget_get_first_child(ui_row_box(row));
	return gtk_widget_get_last_child(labels);
}

static void saved(struct dns_page *p, bool ok, const char *out) {
	if (ok) {
		settings_status(p->s, "Saved %s", dns_conf_path());
	}
}

static gboolean save_now(gpointer data) {
	struct dns_page *p = data;
	p->save_timer = 0;
	char *text = conf_text(&p->c);
	run_helper(p, "save the DNS settings", text, saved, "config", NULL);
	g_free(text);
	return G_SOURCE_REMOVE;
}

static void sync_widgets(struct dns_page *p);

static void changed(struct dns_page *p) {
	if (p->updating) {
		return;
	}
	if (p->save_timer) {
		g_source_remove(p->save_timer);
	}
	p->save_timer = g_timeout_add(700, save_now, p);
	sync_widgets(p);
}

/* ---------- lists of words ---------- */

static void word_list_sync(struct word_list *w);

static void on_remove_word(GtkButton *button, gpointer data) {
	struct word_list *w = data;
	const char *value = g_object_get_data(G_OBJECT(button), "value");
	GPtrArray *a = *w->values;
	for (guint i = 0; i < a->len; i++) {
		if (strcmp(a->pdata[i], value) == 0) {
			g_ptr_array_remove_index(a, i);
			break;
		}
	}
	word_list_sync(w);
	changed(w->p);
}

static void add_word(struct word_list *w, const char *value) {
	char *v = g_strstrip(g_strdup(value));
	if (*v && !has(*w->values, v)) {
		g_ptr_array_add(*w->values, v);
		word_list_sync(w);
		changed(w->p);
	} else {
		g_free(v);
	}
}

/* What a pick does, once the dropdown is done with the click: its list is
 * made anew, which must not happen while its popup is still handling it. */
static gboolean picked(gpointer data) {
	struct word_list *w = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->pick));
	if (sel == 0 || sel == GTK_INVALID_LIST_POSITION || sel > w->picks->len) {
		return G_SOURCE_REMOVE;
	}
	char *value = g_strdup(w->picks->pdata[sel - 1]);
	w->syncing = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(w->pick), 0);
	w->syncing = false;
	if (strcmp(value, "") == 0) {
		gtk_widget_set_visible(w->entry_row, true); // "Other…": type it
		gtk_widget_grab_focus(w->entry);
	} else {
		add_word(w, value);
	}
	g_free(value);
	return G_SOURCE_REMOVE;
}

static void on_pick(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct word_list *w = data;
	if (!w->syncing && gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) != 0) {
		g_idle_add(picked, w);
	}
}

static void on_add_typed(GtkWidget *widget, gpointer data) {
	struct word_list *w = data;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(w->entry));
	char *v = g_strstrip(g_ascii_strdown(text, -1));
	if (w->presets == list_presets) {
		g_free(v);
		v = g_strstrip(g_strdup(text)); // addresses keep their case
	}
	if (!w->valid(v)) {
		gtk_widget_add_css_class(w->entry, "error");
	} else {
		gtk_widget_remove_css_class(w->entry, "error");
		add_word(w, v);
		gtk_editable_set_text(GTK_EDITABLE(w->entry), "");
		if (w->pick) {
			gtk_widget_set_visible(w->entry_row, false);
		}
	}
	g_free(v);
}

static void word_list_sync(struct word_list *w) {
	// the status is read every three seconds: rows that would come out the
	// same stay, so nothing moves under the pointer
	GString *sig = g_string_new(NULL);
	for (guint i = 0; i < (*w->values)->len; i++) {
		char *title = NULL, *subtitle = NULL;
		w->describe(w, (*w->values)->pdata[i], &title, &subtitle);
		g_string_append_printf(sig, "%s\n%s\n%s\n", (char *)(*w->values)->pdata[i], title,
			subtitle ? subtitle : "");
		g_free(title);
		g_free(subtitle);
	}
	if (w->signature && strcmp(w->signature, sig->str) == 0) {
		g_string_free(sig, TRUE);
		return;
	}
	g_free(w->signature);
	w->signature = g_string_free(sig, FALSE);
	for (guint i = 0; i < w->rows->len; i++) {
		gtk_list_box_remove(GTK_LIST_BOX(w->list), w->rows->pdata[i]);
	}
	g_ptr_array_set_size(w->rows, 0);
	GPtrArray *a = *w->values;
	for (guint i = 0; i < a->len; i++) {
		const char *value = a->pdata[i];
		char *title = NULL, *subtitle = NULL;
		w->describe(w, value, &title, &subtitle);
		GtkWidget *remove = gtk_button_new_from_icon_name("list-remove-symbolic");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_tooltip_text(remove, w->remove_tooltip);
		g_object_set_data_full(G_OBJECT(remove), "value", g_strdup(value), g_free);
		g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_word), w);
		GtkWidget *row = ui_row(w->list, title, subtitle, remove);
		// in its place, above the rows to add more
		g_object_ref(row);
		gtk_list_box_remove(GTK_LIST_BOX(w->list), row);
		gtk_list_box_insert(GTK_LIST_BOX(w->list), row, (int)i);
		g_object_unref(row);
		g_ptr_array_add(w->rows, row);
		g_free(title);
		g_free(subtitle);
	}
	gtk_widget_set_visible(w->empty_row, a->len == 0);
	if (w->pick) {
		// what can still be picked: the presets that are not in the list yet
		GtkStringList *items = gtk_string_list_new(NULL);
		g_ptr_array_set_size(w->picks, 0);
		gtk_string_list_append(items, w->presets == list_presets ? "Add a list…" :
			"Add a server…");
		for (const struct dns_preset *pr = w->presets; pr->value; pr++) {
			if (!has(a, pr->value)) {
				char *label = w->presets == list_presets ? g_strdup(pr->name) :
					g_strdup_printf("%s  (%s)", pr->value, pr->name);
				gtk_string_list_append(items, label);
				g_free(label);
				g_ptr_array_add(w->picks, g_strdup(pr->value));
			}
		}
		gtk_string_list_append(items, "Other address…");
		g_ptr_array_add(w->picks, g_strdup(""));
		w->syncing = true;
		if (ui_drop_down_set_strings(GTK_DROP_DOWN(w->pick), items)) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(w->pick), 0);
		}
		w->syncing = false;
		g_object_unref(items);
	}
}

static struct word_list *word_list_new(struct dns_page *p, GtkWidget *content, const char *title,
		const char *description, GPtrArray **values, const struct dns_preset *presets,
		const char *empty, const char *placeholder, const char *remove_tooltip,
		bool (*valid)(const char *),
		void (*describe)(struct word_list *, const char *, char **, char **)) {
	struct word_list *w = g_new0(struct word_list, 1);
	w->p = p;
	w->values = values;
	w->presets = presets;
	w->valid = valid;
	w->describe = describe;
	w->remove_tooltip = remove_tooltip;
	w->rows = g_ptr_array_new();
	w->picks = g_ptr_array_new_with_free_func(g_free);
	w->list = ui_group(content, title, description);
	w->empty_row = ui_row(w->list, empty, NULL, NULL);
	gtk_widget_add_css_class(gtk_widget_get_first_child(gtk_widget_get_first_child(
		ui_row_box(w->empty_row))), "dim-label");
	if (presets) {
		w->pick = gtk_drop_down_new(NULL, NULL);
		g_signal_connect(w->pick, "notify::selected", G_CALLBACK(on_pick), w);
		w->add_row = ui_row(w->list, NULL, NULL, w->pick);
	}
	w->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(w->entry), placeholder);
	gtk_widget_set_hexpand(w->entry, TRUE);
	g_signal_connect(w->entry, "activate", G_CALLBACK(on_add_typed), w);
	GtkWidget *add = gtk_button_new_with_label("Add");
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_typed), w);
	w->entry_row = ui_row(w->list, NULL, NULL, NULL);
	gtk_box_append(GTK_BOX(ui_row_box(w->entry_row)), w->entry);
	gtk_box_append(GTK_BOX(ui_row_box(w->entry_row)), add);
	if (presets) {
		gtk_widget_set_visible(w->entry_row, false);
	}
	word_list_sync(w);
	return w;
}

static bool valid_address(const char *text) {
	GInetAddress *a = g_inet_address_new_from_string(text);
	if (a) {
		g_object_unref(a);
	}
	return a != NULL;
}

bool dns_valid_name(const char *text) {
	if (!strchr(text, '.') || strlen(text) > 253 || text[0] == '.') {
		return false;
	}
	for (const char *c = text; *c; c++) {
		if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_' && *c != '.') {
			return false;
		}
	}
	return true;
}

static bool valid_url(const char *text) {
	return (g_str_has_prefix(text, "https://") || g_str_has_prefix(text, "http://")) &&
		strlen(text) > 10 && !strpbrk(text, " \t");
}

static void describe_server(struct word_list *w, const char *value, char **title,
		char **subtitle) {
	struct dns_page *p = w->p;
	const char *name = dns_preset_name(dns_server_presets, value);
	*title = name ? g_strdup_printf("%s  —  %s", value, name) : g_strdup(value);
	const char *ms = p->server_ms ? g_hash_table_lookup(p->server_ms, value) : NULL;
	const char *use = p->in_use ? g_hash_table_lookup(p->in_use, value) : NULL;
	GString *sub = g_string_new(NULL);
	if (ms && atoi(ms) >= 5000) {
		g_string_append(sub, "Does not answer");
	} else if (ms && atoi(ms) >= 0) {
		g_string_append_printf(sub, "Answers in %s ms", ms);
	} else if (p->running && p->c.use_servers) {
		g_string_append(sub, "Not timed yet");
	}
	if (use && p->c.use_servers) {
		g_string_append_printf(sub, "%s%s", sub->len ? " · " : "",
			strcmp(use, "use") == 0 ? "asked now" : "asked when the others do not answer");
	}
	*subtitle = sub->len ? g_string_free(sub, FALSE) : (g_string_free(sub, TRUE), NULL);
}

static char *ago(time_t when) {
	long s = (long)(time(NULL) - when);
	if (s < 120) {
		return g_strdup("just now");
	}
	if (s < 7200) {
		return g_strdup_printf("%ld minutes ago", s / 60);
	}
	if (s < 172800) {
		return g_strdup_printf("%ld hours ago", s / 3600);
	}
	return g_strdup_printf("%ld days ago", s / 86400);
}

static void describe_list(struct word_list *w, const char *value, char **title, char **subtitle) {
	const char *name = dns_preset_name(list_presets, value);
	if (name) {
		*title = g_strdup(name);
	} else if (g_str_has_prefix(value, "file:")) {
		char *base = g_path_get_basename(value + 5);
		*title = g_strdup_printf("%s (imported)", base);
		g_free(base);
	} else {
		*title = g_strdup(value);
	}
	const char *info = w->p->list_info ? g_hash_table_lookup(w->p->list_info, value) : NULL;
	if (!info) {
		*subtitle = g_strdup(w->p->running ? "Waiting to be downloaded" :
			"Used once the DNS service is on");
		return;
	}
	int count = 0;
	long mtime = 0;
	char state[32] = "";
	sscanf(info, "%d %ld %31s", &count, &mtime, state);
	if (strcmp(state, "download-failed") == 0) {
		*subtitle = g_strdup("Could not be downloaded; tried again in an hour");
	} else if (strcmp(state, "not-downloaded") == 0) {
		*subtitle = g_strdup("Being downloaded");
	} else if (strcmp(state, "missing") == 0) {
		*subtitle = g_strdup("The file is gone");
	} else {
		char *when = ago((time_t)mtime);
		char *num = g_strdup_printf("%'d", count);
		*subtitle = g_strdup_printf("%s names · updated %s", num, when);
		g_free(num);
		g_free(when);
	}
}

static void describe_name(struct word_list *w, const char *value, char **title, char **subtitle) {
	*title = g_strdup(value);
	*subtitle = NULL;
}

/* ---------- choices ---------- */

static const int fastest_values[] = { 1, 2, 3, 4 };
static const char *const fastest_labels[] = { "The fastest one", "The 2 fastest",
	"The 3 fastest", "The 4 fastest", NULL };
static const int interval_values[] = { 900, 1800, 3600, 3 * 3600, 6 * 3600, 86400 };
static const char *const interval_labels[] = { "Every 15 minutes", "Every 30 minutes",
	"Once an hour", "Every 3 hours", "Every 6 hours", "Once a day", NULL };
static const int min_values[] = { 0, 60, 300, 900, 1800, 3600, 6 * 3600, 86400 };
static const char *const min_labels[] = { "As long as the answer says", "1 minute",
	"5 minutes", "15 minutes", "30 minutes", "1 hour", "6 hours", "1 day", NULL };
static const int max_values[] = { 60, 300, 900, 3600, 6 * 3600, 86400, 7 * 86400, 0 };
static const char *const max_labels[] = { "1 minute", "5 minutes", "15 minutes", "1 hour",
	"6 hours", "1 day", "1 week", "As long as the answer says", NULL };
static const int size_values[] = { 1000, 5000, 10000, 50000, 100000 };
static const char *const size_labels[] = { "1,000 answers", "5,000 answers",
	"10,000 answers", "50,000 answers", "100,000 answers", NULL };
static const int percent_values[] = { 1, 2, 5, 10, 20, 50 };
static const char *const percent_labels[] = { "The top 1%", "The top 2%", "The top 5%",
	"The top 10%", "The top 20%", "The top half", NULL };
static const int days_values[] = { 1, 3, 7, 14, 30 };
static const char *const days_labels[] = { "Today", "3 days", "7 days", "14 days",
	"30 days", NULL };
static const char *const mode_labels[] = { "Those of each network",
	"These servers, in this order", "The fastest of these servers", NULL };
static const char *const answer_labels[] = { "0.0.0.0, as Pi-hole does",
	"\"No such name\" (NXDOMAIN)", NULL };

/* The item of a dropdown of numbers for value: the same, or the nearest below. */
static guint index_of(const int *values, size_t n, int value) {
	guint best = 0;
	for (size_t i = 0; i < n; i++) {
		if (values[i] == value) {
			return (guint)i;
		}
		if (values[i] < value && values[i] >= values[best]) {
			best = (guint)i;
		}
	}
	return best;
}

#define SET_DD(dd, values, value) gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), \
	index_of(values, G_N_ELEMENTS(values), value))
#define GET_DD(dd, values) values[MIN(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)), \
	G_N_ELEMENTS(values) - 1)]

static void on_choice(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct dns_page *p = data;
	if (p->updating) {
		return;
	}
	struct dnsconf *c = &p->c;
	guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->mode_dd));
	c->use_servers = mode > 0;
	c->fastest = mode == 2 ? GET_DD(p->fastest_dd, fastest_values) : 0;
	c->test_interval = GET_DD(p->interval_dd, interval_values);
	c->cache_min = GET_DD(p->min_dd, min_values);
	c->cache_max = GET_DD(p->max_dd, max_values);
	c->cache_size = GET_DD(p->size_dd, size_values);
	c->prefetch_percent = GET_DD(p->percent_dd, percent_values);
	c->prefetch_days = GET_DD(p->days_dd, days_values);
	c->nxdomain = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->answer_dd)) == 1;
	if (c->cache_max && c->cache_max < c->cache_min) {
		c->cache_max = c->cache_min; // at most is never shorter than at least
		p->updating = true;
		SET_DD(p->max_dd, max_values, c->cache_max);
		p->updating = false;
	}
	changed(p);
}

static void on_switch(GObject *sw, GParamSpec *pspec, gpointer data) {
	struct dns_page *p = data;
	if (p->updating) {
		return;
	}
	p->c.cache = gtk_switch_get_active(GTK_SWITCH(p->cache_switch));
	p->c.prefetch = gtk_switch_get_active(GTK_SWITCH(p->prefetch_switch));
	p->c.blocking = gtk_switch_get_active(GTK_SWITCH(p->blocking_switch));
	changed(p);
}

static void sync_widgets(struct dns_page *p) {
	struct dnsconf *c = &p->c;
	gtk_widget_set_visible(p->fastest_row, c->use_servers && c->fastest > 0);
	gtk_widget_set_visible(p->interval_row, c->use_servers && c->fastest > 0);
	gtk_widget_set_visible(p->test_row, c->use_servers && c->fastest > 0 && p->running);
	gtk_widget_set_visible(p->servers_box, c->use_servers);
	gtk_widget_set_sensitive(p->min_row, c->cache);
	gtk_widget_set_sensitive(p->max_row, c->cache);
	gtk_widget_set_sensitive(p->size_row, c->cache);
	gtk_widget_set_sensitive(p->prefetch_switch, c->cache);
	gtk_widget_set_sensitive(p->percent_row, c->cache && c->prefetch);
	gtk_widget_set_sensitive(p->days_row, c->cache && c->prefetch);
	gtk_widget_set_sensitive(p->answer_row, c->blocking);
	gtk_widget_set_visible(p->update_row, p->running && c->lists->len > 0);
}

/* ---------- the service ---------- */

static void service_done(struct dns_page *p, bool ok, const char *out) {
	p->busy = false;
	gtk_widget_set_sensitive(p->service_switch, dns_service_installed());
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->service_switch), dns_service_enabled());
	p->updating = false;
}

static void config_then_enable(struct dns_page *p, bool ok, const char *out) {
	if (!ok) {
		service_done(p, ok, out);
		return;
	}
	run_helper(p, "start the DNS service", NULL, service_done, "enable", NULL);
}

static gboolean on_service_switch(GtkSwitch *sw, gboolean on, gpointer data) {
	struct dns_page *p = data;
	if (p->updating) {
		return FALSE; // set by the page itself: the switch follows
	}
	if (p->busy) {
		return TRUE;
	}
	p->busy = true;
	gtk_widget_set_sensitive(p->service_switch, false);
	set_message(p, NULL);
	if (on) {
		// the settings first, so the service starts with them
		if (p->save_timer) {
			g_source_remove(p->save_timer);
			p->save_timer = 0;
		}
		char *text = conf_text(&p->c);
		run_helper(p, "save the DNS settings", text, config_then_enable, "config", NULL);
		g_free(text);
	} else {
		run_helper(p, "stop the DNS service", NULL, service_done, "disable", NULL);
	}
	gtk_switch_set_state(sw, on);
	return TRUE;
}

static void on_test(GtkButton *button, gpointer data) {
	struct dns_page *p = data;
	run_helper(p, "time the servers", NULL, NULL, "test", NULL);
	settings_status(p->s, "Timing the servers…");
}

static void on_update_lists(GtkButton *button, gpointer data) {
	struct dns_page *p = data;
	run_helper(p, "update the block lists", NULL, NULL, "update-lists", NULL);
	settings_status(p->s, "Downloading the block lists…");
}

/* ---------- importing a list ---------- */

struct import {
	struct dns_page *p;
	char *contents;
};

static void imported(struct dns_page *p, bool ok, const char *out) {
	if (ok && out && *out) {
		char *path = g_strstrip(g_strdup(out));
		char *value = g_strdup_printf("file:%s", path);
		add_word(p->lists, value);
		g_free(value);
		g_free(path);
	}
}

static void on_import_picked(GObject *source, GAsyncResult *result, gpointer data) {
	struct dns_page *p = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file) {
		return;
	}
	char *contents = NULL;
	gsize len = 0;
	GError *error = NULL;
	if (!g_file_load_contents(file, NULL, &contents, &len, NULL, &error)) {
		set_message(p, error->message);
		g_error_free(error);
		g_object_unref(file);
		return;
	}
	char *base = g_file_get_basename(file);
	// the helper takes letters, digits, '.', '_' and '-' only
	for (char *c = base; *c; c++) {
		if (!g_ascii_isalnum(*c) && *c != '.' && *c != '_' && *c != '-') {
			*c = '_';
		}
	}
	if (strlen(base) > 60) {
		base[60] = '\0';
	}
	if (g_str_has_suffix(base, ".list")) {
		base[strlen(base) - 5] = '\0';
	}
	if (!g_utf8_validate(contents, len, NULL)) {
		set_message(p, "That file is no text: a block list has one name per line");
	} else {
		run_helper(p, "import the block list", contents, imported, "import", base);
	}
	g_free(base);
	g_free(contents);
	g_object_unref(file);
}

static void on_import(GtkButton *button, gpointer data) {
	struct dns_page *p = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Import a block list");
	gtk_file_dialog_open(dialog, p->s->window, NULL, on_import_picked, p);
	g_object_unref(dialog);
}

/* ---------- what the service is doing ---------- */

static void on_never_prefetch(GtkButton *button, gpointer data) {
	struct dns_page *p = data;
	add_word(p->never, g_object_get_data(G_OBJECT(button), "name"));
}

static void refresh_prefetched(struct dns_page *p) {
	char *path = g_build_filename(run_dir(), "prefetch", NULL);
	GStatBuf st;
	char *stamp = g_stat(path, &st) == 0 ? g_strdup_printf("%ld", (long)st.st_mtime) :
		g_strdup("");
	if (g_strcmp0(stamp, p->prefetch_mtime) == 0 && p->prefetched_rows->len > 0) {
		g_free(stamp);
		g_free(path);
		return;
	}
	g_free(p->prefetch_mtime);
	p->prefetch_mtime = stamp;
	for (guint i = 0; i < p->prefetched_rows->len; i++) {
		gtk_list_box_remove(GTK_LIST_BOX(p->prefetched_list), p->prefetched_rows->pdata[i]);
	}
	g_ptr_array_set_size(p->prefetched_rows, 0);
	char *text = NULL;
	GError *error = NULL;
	if (!g_file_get_contents(path, &text, NULL, &error)) {
		const char *why = !p->running ? "Shown while the DNS service is on" :
			g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_ACCES) ?
			"Only administrators (the group wheel) may see which names are asked for" :
			"Not known yet";
		g_ptr_array_add(p->prefetched_rows, ui_row(p->prefetched_list, why, NULL, NULL));
		g_clear_error(&error);
		g_free(path);
		return;
	}
	g_free(path);
	// one row per name, its types together
	GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
	char **lines = g_strsplit(text, "\n", -1);
	int shown = 0, total = 0;
	for (int i = 0; lines[i]; i++) {
		char name[1100];
		unsigned count = 0, type = 0;
		if (sscanf(lines[i], "%u %1099s %u", &count, name, &type) != 3 ||
				g_hash_table_contains(seen, name)) {
			continue;
		}
		g_hash_table_add(seen, g_strdup(name));
		total++;
		if (shown >= 40) {
			continue;
		}
		shown++;
		char *sub = count ? g_strdup_printf("Asked for %u times", count) :
			g_strdup("Always prefetched");
		GtkWidget *never = gtk_button_new_with_label("Never");
		gtk_widget_add_css_class(never, "flat");
		gtk_widget_set_tooltip_text(never, "Never prefetch this name");
		g_object_set_data_full(G_OBJECT(never), "name", g_strdup(name), g_free);
		g_signal_connect(never, "clicked", G_CALLBACK(on_never_prefetch), p);
		g_ptr_array_add(p->prefetched_rows, ui_row(p->prefetched_list, name, sub, never));
		g_free(sub);
	}
	if (total == 0) {
		g_ptr_array_add(p->prefetched_rows, ui_row(p->prefetched_list,
			"None yet: names are counted as apps ask for them", NULL, NULL));
	} else if (total > shown) {
		char *more = g_strdup_printf("And %d more", total - shown);
		g_ptr_array_add(p->prefetched_rows, ui_row(p->prefetched_list, more, NULL, NULL));
		g_free(more);
	}
	g_hash_table_destroy(seen);
	g_strfreev(lines);
	g_free(text);
}

static bool service_running(const char *status) {
	const char *pid_line = status ? strstr(status, "pid ") : NULL;
	if (!pid_line) {
		return false;
	}
	pid_t pid = (pid_t)atoi(pid_line + 4);
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void refresh_status(struct dns_page *p) {
	char *path = g_build_filename(run_dir(), "status", NULL);
	char *status = NULL;
	g_file_get_contents(path, &status, NULL, NULL);
	g_free(path);
	p->running = service_running(status);
	g_hash_table_remove_all(p->server_ms);
	g_hash_table_remove_all(p->in_use);
	g_hash_table_remove_all(p->list_info);
	unsigned long queries = 0, cached = 0, blocked = 0, prefetched = 0;
	GString *asking = g_string_new(NULL);
	char source[32] = "";
	char **lines = g_strsplit(status ? status : "", "\n", -1);
	for (int i = 0; p->running && lines[i]; i++) {
		char a[512], b[512];
		int n;
		if (sscanf(lines[i], "server %511s %d", a, &n) == 2) {
			g_hash_table_insert(p->server_ms, g_strdup(a), g_strdup_printf("%d", n));
		} else if (sscanf(lines[i], "use %511s", a) == 1) {
			g_hash_table_insert(p->in_use, g_strdup(a), g_strdup("use"));
			g_string_append_printf(asking, "%s%s", asking->len ? ", " : "", a);
		} else if (sscanf(lines[i], "backup %511s", a) == 1) {
			g_hash_table_insert(p->in_use, g_strdup(a), g_strdup("backup"));
		} else if (sscanf(lines[i], "today %lu %lu %lu %lu", &queries, &cached, &blocked,
				&prefetched) == 4) {
			// read
		} else if (sscanf(lines[i], "source %31s", source) == 1) {
			// read
		} else if (strncmp(lines[i], "list ", 5) == 0) {
			int count;
			long mtime;
			if (sscanf(lines[i], "list %d %ld %511s %511s", &count, &mtime, a, b) == 4) {
				g_hash_table_insert(p->list_info, g_strdup(b),
					g_strdup_printf("%d %ld %s", count, mtime, a));
			}
		}
	}
	g_strfreev(lines);
	g_free(status);

	bool enabled = dns_service_enabled();
	char *sub;
	if (!dns_service_installed()) {
		sub = g_strdup("Not installed: install tileWin with install.sh to use it");
	} else if (p->running) {
		const char *from = strcmp(source, "network-own") == 0 ? " (this network's own servers)" :
			strcmp(source, "network") == 0 ? " (the servers of the network)" :
			strcmp(source, "fallback") == 0 ? " (the network has none)" : "";
		sub = g_strdup_printf("On: asking %s%s", asking->len ? asking->str : "nobody yet", from);
	} else if (enabled) {
		sub = g_strdup("On, but not running: see journalctl -u tilewin-dnsd");
	} else {
		sub = g_strdup("Off: NetworkManager and its servers answer as before");
	}
	gtk_label_set_text(GTK_LABEL(p->service_sub), sub);
	g_free(sub);
	g_string_free(asking, TRUE);
	if (!p->busy) {
		p->updating = true;
		gtk_switch_set_active(GTK_SWITCH(p->service_switch), enabled);
		gtk_switch_set_state(GTK_SWITCH(p->service_switch), enabled);
		p->updating = false;
		gtk_widget_set_sensitive(p->service_switch, dns_service_installed());
	}
	if (p->running) {
		char *today = g_strdup_printf("Today: %lu queries, %lu%% from the cache, %lu blocked, "
			"%lu prefetched", queries, queries ? cached * 100 / queries : 0, blocked, prefetched);
		gtk_label_set_text(GTK_LABEL(p->today_label), today);
		g_free(today);
	}
	gtk_widget_set_visible(p->today_label, p->running);
	char *global = dns_networkmanager_global();
	if (*global) {
		char *note = g_strdup_printf(enabled ?
			"NetworkManager has servers for all networks of its own: %s. While the DNS service "
			"is on, it goes to the service instead, which asks the servers chosen here." :
			"NetworkManager has servers for all networks of its own: %s. They are asked while "
			"the DNS service is off.", global);
		gtk_label_set_text(GTK_LABEL(p->nm_note), note);
		g_free(note);
	}
	gtk_widget_set_visible(p->nm_note, *global);
	g_free(global);
	word_list_sync(p->servers);
	word_list_sync(p->lists);
	refresh_prefetched(p);
	sync_widgets(p);
}

static gboolean status_tick(gpointer data) {
	struct dns_page *p = data;
	if (gtk_widget_get_mapped(p->service_switch)) {
		refresh_status(p);
	}
	return G_SOURCE_CONTINUE;
}

void dns_page_refresh(struct settings *s) {
	struct dns_page *p = s->dns_page;
	if (!p || p->save_timer) {
		return; // unsaved changes win
	}
	conf_read(&p->c);
	p->updating = true;
	struct dnsconf *c = &p->c;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), !c->use_servers ? 0 :
		c->fastest > 0 ? 2 : 1);
	SET_DD(p->fastest_dd, fastest_values, c->fastest > 0 ? c->fastest : 2);
	SET_DD(p->interval_dd, interval_values, c->test_interval);
	gtk_switch_set_active(GTK_SWITCH(p->cache_switch), c->cache);
	SET_DD(p->min_dd, min_values, c->cache_min);
	SET_DD(p->max_dd, max_values, c->cache_max == 0 ? 0 : c->cache_max);
	if (c->cache_max == 0) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->max_dd), G_N_ELEMENTS(max_values) - 1);
	}
	SET_DD(p->size_dd, size_values, c->cache_size);
	gtk_switch_set_active(GTK_SWITCH(p->prefetch_switch), c->prefetch);
	SET_DD(p->percent_dd, percent_values, c->prefetch_percent);
	SET_DD(p->days_dd, days_values, c->prefetch_days);
	gtk_switch_set_active(GTK_SWITCH(p->blocking_switch), c->blocking);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->answer_dd), c->nxdomain ? 1 : 0);
	p->updating = false;
	struct word_list *lists[] = { p->servers, p->lists, p->always, p->never, p->allow, p->block };
	for (size_t i = 0; i < G_N_ELEMENTS(lists); i++) {
		word_list_sync(lists[i]);
	}
	refresh_status(p);
}

static GtkWidget *dropdown(struct dns_page *p, const char *const *labels) {
	GtkWidget *dd = gtk_drop_down_new_from_strings(labels);
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_choice), p);
	return dd;
}

static GtkWidget *a_switch(struct dns_page *p) {
	GtkWidget *sw = gtk_switch_new();
	g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch), p);
	return sw;
}

static GtkWidget *button_row(GtkWidget *group, const char *title, const char *subtitle,
		const char *label, GCallback callback, gpointer data) {
	GtkWidget *button = gtk_button_new_with_label(label);
	g_signal_connect(button, "clicked", callback, data);
	return ui_row(group, title, subtitle, button);
}

GtkWidget *dns_page_new(struct settings *s) {
	struct dns_page *p = g_new0(struct dns_page, 1);
	p->s = s;
	p->server_ms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	p->in_use = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	p->list_info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	p->prefetched_rows = g_ptr_array_new();
	conf_read(&p->c);
	GtkWidget *content;
	GtkWidget *page = ui_page("DNS",
		"Which servers turn names like example.com into addresses, how long their answers "
		"are kept, which are fetched ahead of time, and which names are blocked.", &content);

	GtkWidget *service = ui_group(content, "DNS service",
		"tileWin's own DNS service does all of this page; while it is off, NetworkManager "
		"answers as before.");
	p->service_switch = gtk_switch_new();
	g_signal_connect(p->service_switch, "state-set", G_CALLBACK(on_service_switch), p);
	GtkWidget *row = ui_row(service, "Use the tileWin DNS service", " ", p->service_switch);
	p->service_sub = subtitle_of(row);
	p->today_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->today_label), 0);
	gtk_widget_add_css_class(p->today_label, "dim-label");
	gtk_widget_set_margin_start(p->today_label, 12);
	gtk_widget_set_margin_top(p->today_label, 4);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(service)), p->today_label);
	p->message = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->message), 0);
	gtk_label_set_wrap(GTK_LABEL(p->message), TRUE);
	gtk_widget_add_css_class(p->message, "error");
	gtk_widget_set_margin_start(p->message, 12);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(service)), p->message);
	gtk_widget_set_visible(p->message, false);
	p->nm_note = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->nm_note), 0);
	gtk_label_set_wrap(GTK_LABEL(p->nm_note), TRUE);
	gtk_widget_add_css_class(p->nm_note, "dim-label");
	gtk_widget_add_css_class(p->nm_note, "tw-caption");
	gtk_widget_set_margin_start(p->nm_note, 12);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(service)), p->nm_note);

	GtkWidget *all = ui_group(content, "Servers for all networks",
		"Networks with servers of their own (below) keep those.");
	p->mode_dd = dropdown(p, mode_labels);
	ui_row(all, "Ask", NULL, p->mode_dd);
	p->fastest_dd = dropdown(p, fastest_labels);
	p->fastest_row = ui_row(all, "Use",
		"They are asked at the same time, and the first answer counts", p->fastest_dd);
	p->interval_dd = dropdown(p, interval_labels);
	p->interval_row = ui_row(all, "Find the fastest",
		"Each server is timed with three questions; the others stand by in case the "
		"fastest stop answering", p->interval_dd);
	p->test_row = button_row(all, "Time the servers now", NULL, "Time them",
		G_CALLBACK(on_test), p);
	p->servers = word_list_new(p, content, NULL, NULL, &p->c.servers, dns_server_presets,
		"No servers yet: add some below", "An address, e.g. 192.168.1.1 or 2606:4700::1111",
		"Remove this server", valid_address, describe_server);
	p->servers_box = gtk_widget_get_parent(p->servers->list);
	network_dns_section_attach(s, content);

	GtkWidget *cache = ui_group(content, "Cache",
		"Answers are kept for a while, so the same name is answered at once the next time.");
	p->cache_switch = a_switch(p);
	ui_row(cache, "Keep answers", NULL, p->cache_switch);
	p->min_dd = dropdown(p, min_labels);
	p->min_row = ui_row(cache, "Keep an answer at least",
		"Longer than an answer says saves asking, but a moved site is found later",
		p->min_dd);
	p->max_dd = dropdown(p, max_labels);
	p->max_row = ui_row(cache, "Keep an answer at most", NULL, p->max_dd);
	p->size_dd = dropdown(p, size_labels);
	p->size_row = ui_row(cache, "Keep up to", "The answers asked for longest ago make room",
		p->size_dd);

	GtkWidget *prefetch = ui_group(content, "Prefetch",
		"The answers for the names asked for most are fetched again before they run out, so "
		"they are at hand at once, also after nobody asked for them for a while. Only the "
		"questions of apps are counted for this, never the prefetches themselves, so a name "
		"does not stay at the top just because it is prefetched.");
	p->prefetch_switch = a_switch(p);
	ui_row(prefetch, "Prefetch answers", NULL, p->prefetch_switch);
	p->percent_dd = dropdown(p, percent_labels);
	p->percent_row = ui_row(prefetch, "Prefetch the names asked for most",
		"Of all names asked for, by how often", p->percent_dd);
	p->days_dd = dropdown(p, days_labels);
	p->days_row = ui_row(prefetch, "Counted over the last", NULL, p->days_dd);
	p->always = word_list_new(p, content, "Always prefetch", NULL, &p->c.prefetch_add, NULL,
		"No names", "A name, e.g. example.com", "Remove", dns_valid_name, describe_name);
	p->never = word_list_new(p, content, "Never prefetch",
		"Also the names below them, e.g. example.com covers mail.example.com.",
		&p->c.prefetch_skip, NULL, "No names", "A name, e.g. example.com", "Remove",
		dns_valid_name, describe_name);
	p->prefetched_list = ui_group(content, "Prefetched now", NULL);

	GtkWidget *block = ui_group(content, "Block lists",
		"Names of ads, trackers and malware are not looked up at all, as on a Pi-hole. The "
		"lists are downloaded once a day.");
	p->blocking_switch = a_switch(p);
	ui_row(block, "Block the names of the lists", NULL, p->blocking_switch);
	p->answer_dd = dropdown(p, answer_labels);
	p->answer_row = ui_row(block, "Answer for a blocked name", NULL, p->answer_dd);
	p->update_row = button_row(block, "Download the lists again now", NULL, "Update",
		G_CALLBACK(on_update_lists), p);
	button_row(block, "Import a list from a file",
		"A hosts file, one name per line, or adblock rules (||name^)", "Import…",
		G_CALLBACK(on_import), p);
	p->lists = word_list_new(p, content, NULL, NULL, &p->c.lists, list_presets,
		"No lists", "The address of a list, https://…", "Remove this list", valid_url,
		describe_list);
	p->block = word_list_new(p, content, "Blocked by hand",
		"With the names below them.", &p->c.block, NULL, "No names",
		"A name, e.g. ads.example.com", "Unblock", dns_valid_name, describe_name);
	p->allow = word_list_new(p, content, "Never blocked",
		"Names a list blocks that you need, with the names below them.", &p->c.allow, NULL,
		"No names", "A name, e.g. example.com", "Remove", dns_valid_name, describe_name);

	s->dns_page = p;
	dns_page_refresh(s);
	p->status_timer = g_timeout_add_seconds(3, status_tick, p);
	return page;
}
