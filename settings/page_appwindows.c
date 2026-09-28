#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_desktop.h"

/*
 * App windows page: what tileWin does about the apps behind the windows, all
 * in common.conf and applied at once over IPC:
 *  - apps that stop answering are shown as "(Not Responding)" (not_responding);
 *  - minimized apps stop after a while (pause_minimized, _sound, _except);
 *  - apps open where their window was last (remember_windows, _except);
 *  - window rules: for_window lines for one app with one thing to do, which
 *    this page writes and reads; other for_window lines are left alone.
 */

static const char *const pause_values[] = { "off", "10", "30", "60", "300", "900", "1800", NULL };
static const char *const pause_labels[] = { "Never", "After 10 seconds", "After 30 seconds",
	"After a minute", "After 5 minutes", "After 15 minutes", "After 30 minutes", NULL };
static const char *const sound_values[] = { "keep", "pause", NULL };
static const char *const sound_labels[] = { "Keep running", "Stop them as well", NULL };

/* What a rule does; %d is the desktop. */
static const struct {
	const char *label, *command;
} actions[] = {
	{ "Keep it on top of other windows", "always_on_top enable" },
	{ "Open it maximized", "maximize enable" },
	{ "Open it minimized", "minimize enable" },
	{ "Open it full screen", "fullscreen enable" },
	{ "Open it on desktop", "move container to workspace number %d" },
	{ "Let it float in tile mode", "floating enable" },
	{ "Make it see-through", "opacity 0.85" },
};
#define DESKTOP_ACTION 4

struct appwin_page {
	struct settings *s;
	bool updating;
	GtkWidget *not_responding, *pause_after, *pause_sound, *remember;
	struct app_list *pause_except, *remember_except;
	GtkWidget *rules;       // the list of rules
	GtkWidget *rule_app;    // entry: app id
	GtkWidget *open_apps;   // dropdown of the apps with a window open
	GtkWidget *rule_action, *rule_desktop;
	GPtrArray *open;        // struct tw_open_app *
};

struct rule {
	char *id;
	bool x11;
	int action;
	int desktop;
	char *raw; // the arguments as written, to find the line again
};

static void rule_free(struct rule *r) {
	g_free(r->id);
	g_free(r->raw);
	g_free(r);
}

/* "[app_id="^firefox$"] maximize enable" and the like; false for other lines. */
static struct rule *parse_rule(const char *raw) {
	const char *p = raw;
	while (*p == ' ') {
		p++;
	}
	bool x11;
	if (g_str_has_prefix(p, "[app_id=\"")) {
		x11 = false;
		p += strlen("[app_id=\"");
	} else if (g_str_has_prefix(p, "[class=\"")) {
		x11 = true;
		p += strlen("[class=\"");
	} else {
		return NULL;
	}
	const char *end = strstr(p, "\"]");
	if (!end || end == p) {
		return NULL;
	}
	char *id = g_strndup(p, end - p);
	if (id[0] == '^') {
		memmove(id, id + 1, strlen(id));
	}
	size_t n = strlen(id);
	if (n && id[n - 1] == '$') {
		id[n - 1] = 0;
	}
	char *command = g_strstrip(g_strdup(end + 2));
	struct rule *r = NULL;
	for (size_t i = 0; i < G_N_ELEMENTS(actions) && !r; i++) {
		int desktop = 0;
		if (i == DESKTOP_ACTION) {
			if (sscanf(command, "move container to workspace number %d", &desktop) == 1 &&
					desktop > 0) {
				r = g_new0(struct rule, 1);
				r->desktop = desktop;
			}
		} else if (strcmp(command, actions[i].command) == 0) {
			r = g_new0(struct rule, 1);
		}
		if (r) {
			r->action = (int)i;
		}
	}
	g_free(command);
	if (!r || !*id || strpbrk(id, "\"[]")) {
		g_free(id);
		if (r) {
			g_free(r);
		}
		return NULL;
	}
	r->id = id;
	r->x11 = x11;
	r->raw = g_strdup(raw);
	return r;
}

static GPtrArray *read_rules(struct confdoc *doc) {
	GPtrArray *rules = g_ptr_array_new_with_free_func((GDestroyNotify)rule_free);
	for (guint i = 0; doc->root->children && i < doc->root->children->len; i++) {
		struct cstmt *st = doc->root->children->pdata[i];
		if (strcmp(st->name, "for_window") != 0) {
			continue;
		}
		char *raw = cstmt_raw_args(doc, st);
		struct rule *r = raw ? parse_rule(raw) : NULL;
		if (r) {
			g_ptr_array_add(rules, r);
		}
		free(raw);
	}
	return rules;
}

static const char *app_name(struct appwin_page *p, const char *id) {
	char *desktop = g_strdup_printf("%s.desktop", id);
	const struct tw_desktop_entry *e = ui_find_app(desktop);
	g_free(desktop);
	if (e && e->name) {
		return e->name;
	}
	for (guint i = 0; p->open && i < p->open->len; i++) {
		struct tw_open_app *app = p->open->pdata[i];
		if (strcmp(app->id, id) == 0) {
			return app->title;
		}
	}
	return id;
}

/* ---------- plain settings ---------- */

static void apply(struct appwin_page *p, const char *key, const char *value) {
	struct confdoc *d = p->s->common;
	confdoc_set(d, d->root, key, NULL, value);
	settings_common_changed(p->s, false);
	settings_command(p->s, "%s %s", key, value);
}

static void on_switch(GObject *object, GParamSpec *pspec, gpointer data) {
	struct appwin_page *p = data;
	if (p->updating) {
		return;
	}
	bool on = gtk_switch_get_active(GTK_SWITCH(object));
	apply(p, GTK_WIDGET(object) == p->not_responding ? "not_responding" : "remember_windows",
		on ? "enable" : "disable");
}

static void on_choice(GObject *object, GParamSpec *pspec, gpointer data) {
	struct appwin_page *p = data;
	if (p->updating) {
		return;
	}
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
	if (GTK_WIDGET(object) == p->pause_after) {
		apply(p, "pause_minimized", pause_values[sel]);
	} else {
		apply(p, "pause_minimized_sound", sound_values[sel]);
	}
}

static void write_list(struct appwin_page *p, const char *key, GPtrArray *ids) {
	struct confdoc *d = p->s->common;
	GString *args = g_string_new(NULL);
	for (guint i = 0; i < ids->len; i++) {
		char *q = conf_quote(ids->pdata[i]);
		g_string_append_printf(args, "%s%s", i ? " " : "", q);
		free(q);
	}
	confdoc_set(d, d->root, key, NULL, ids->len ? args->str : NULL);
	settings_common_changed(p->s, false);
	settings_command(p->s, "%s none%s%s", key, ids->len ? " " : "", args->str);
	g_string_free(args, TRUE);
}

static void on_pause_except(struct app_list *list, gpointer data) {
	write_list(data, "pause_minimized_except", list->ids);
}

static void on_remember_except(struct app_list *list, gpointer data) {
	write_list(data, "remember_windows_except", list->ids);
}

/* ---------- rules ---------- */

static void rebuild_rules(struct appwin_page *p);

static void on_remove_rule(GtkButton *button, gpointer data) {
	struct appwin_page *p = g_object_get_data(G_OBJECT(button), "page");
	const char *raw = data;
	struct confdoc *d = p->s->common;
	for (guint i = 0; d->root->children && i < d->root->children->len; i++) {
		struct cstmt *st = d->root->children->pdata[i];
		if (strcmp(st->name, "for_window") != 0) {
			continue;
		}
		char *other = cstmt_raw_args(d, st);
		bool same = other && strcmp(other, raw) == 0;
		free(other);
		if (same) {
			confdoc_remove(d, st);
			break;
		}
	}
	settings_common_changed(p->s, true); // tileWin reads rules when it loads its config
	rebuild_rules(p);
}

static void rebuild_rules(struct appwin_page *p) {
	gtk_list_box_remove_all(GTK_LIST_BOX(p->rules));
	GPtrArray *rules = read_rules(p->s->common);
	for (guint i = 0; i < rules->len; i++) {
		struct rule *r = rules->pdata[i];
		char *what = r->action == DESKTOP_ACTION ?
			g_strdup_printf("%s %d", actions[r->action].label, r->desktop) :
			g_strdup(actions[r->action].label);
		char *sub = g_strdup_printf("%s (%s%s)", what, r->x11 ? "X11 class " : "", r->id);
		// the line as written goes with the button, to find it again
		GtkWidget *remove = ui_icon_button("list-remove-symbolic", "Remove this rule", true,
			G_CALLBACK(on_remove_rule), g_strdup(r->raw));
		g_object_set_data(G_OBJECT(remove), "page", p);
		GtkWidget *row = ui_row(p->rules, app_name(p, r->id), sub, remove);
		char *desktop = g_strdup_printf("%s.desktop", r->id);
		const struct tw_desktop_entry *e = ui_find_app(desktop);
		g_free(desktop);
		gtk_box_prepend(GTK_BOX(ui_row_box(row)), ui_app_icon(e ? e->icon : r->id, 32));
		g_free(what);
		g_free(sub);
	}
	if (rules->len == 0) {
		ui_row(p->rules, NULL, "No rules yet", NULL);
	}
	g_ptr_array_free(rules, TRUE);
}

static void on_open_app(GObject *object, GParamSpec *pspec, gpointer data) {
	struct appwin_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
	if (sel == 0 || sel == GTK_INVALID_LIST_POSITION || !p->open || sel > p->open->len) {
		return;
	}
	struct tw_open_app *app = p->open->pdata[sel - 1];
	gtk_editable_set_text(GTK_EDITABLE(p->rule_app), app->id);
}

static void on_action(GObject *object, GParamSpec *pspec, gpointer data) {
	struct appwin_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
	gtk_widget_set_visible(p->rule_desktop, sel == DESKTOP_ACTION);
}

static void on_add_rule(GtkButton *button, gpointer data) {
	struct appwin_page *p = data;
	char *id = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->rule_app))));
	if (!*id || strpbrk(id, "\"[] ")) {
		settings_status(p->s, "Type the app id of the app (or pick an open one) first");
		g_free(id);
		return;
	}
	bool x11 = false;
	for (guint i = 0; p->open && i < p->open->len; i++) {
		struct tw_open_app *app = p->open->pdata[i];
		if (strcmp(app->id, id) == 0) {
			x11 = app->x11;
		}
	}
	guint action = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->rule_action));
	if (action >= G_N_ELEMENTS(actions)) {
		g_free(id);
		return;
	}
	char *command = action == DESKTOP_ACTION ?
		g_strdup_printf(actions[action].command,
			gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->rule_desktop))) :
		g_strdup(actions[action].command);
	char *line = g_strdup_printf("for_window [%s=\"^%s$\"] %s", x11 ? "class" : "app_id", id,
		command);
	struct confdoc *d = p->s->common;
	confdoc_append(d, d->root, line);
	settings_common_changed(p->s, true);
	gtk_editable_set_text(GTK_EDITABLE(p->rule_app), "");
	rebuild_rules(p);
	g_free(line);
	g_free(command);
	g_free(id);
}

static void fill_open_apps(struct appwin_page *p) {
	if (p->open) {
		g_ptr_array_free(p->open, TRUE);
	}
	p->open = tw_ipc_open_apps();
	GtkStringList *names = gtk_string_list_new(NULL);
	gtk_string_list_append(names, "Pick an open app…");
	for (guint i = 0; i < p->open->len; i++) {
		struct tw_open_app *app = p->open->pdata[i];
		char *label = g_strdup_printf("%s (%s)", app->title, app->id);
		gtk_string_list_append(names, label);
		g_free(label);
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->open_apps), G_LIST_MODEL(names));
	g_object_unref(names);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->open_apps), 0);
}

/* ---------- reading ---------- */

static GPtrArray *read_list(struct confdoc *d, const char *key) {
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; d->root->children && i < d->root->children->len; i++) {
		struct cstmt *st = d->root->children->pdata[i];
		if (strcmp(st->name, key) != 0) {
			continue;
		}
		for (int a = 0; a < cstmt_argc(st); a++) {
			const char *id = cstmt_arg(st, a);
			if (g_ascii_strcasecmp(id, "none") == 0) {
				g_ptr_array_set_size(ids, 0);
			} else {
				g_ptr_array_add(ids, g_strdup(id));
			}
		}
	}
	return ids;
}

static bool read_bool(struct confdoc *d, const char *key, bool fallback) {
	const char *v = cstmt_arg(confdoc_child(d->root, key, NULL), 0);
	if (!v) {
		return fallback;
	}
	return !(g_ascii_strcasecmp(v, "disable") == 0 || g_ascii_strcasecmp(v, "no") == 0 ||
		g_ascii_strcasecmp(v, "false") == 0 || g_ascii_strcasecmp(v, "off") == 0);
}

/* "30", "30s", "5m", "1h" in seconds, 0 for off. */
static int read_seconds(const char *v) {
	if (!v) {
		return 0;
	}
	char *end;
	long n = strtol(v, &end, 10);
	if (end == v) {
		return 0;
	}
	if (g_ascii_strcasecmp(end, "m") == 0 || g_ascii_strcasecmp(end, "min") == 0) {
		n *= 60;
	} else if (g_ascii_strcasecmp(end, "h") == 0) {
		n *= 3600;
	}
	return (int)n;
}

void appwin_page_refresh(struct settings *s) {
	struct appwin_page *p = s->appwin_page;
	if (!p) {
		return;
	}
	struct confdoc *d = s->common;
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->not_responding), read_bool(d, "not_responding", true));
	gtk_switch_set_active(GTK_SWITCH(p->remember), read_bool(d, "remember_windows", true));
	int seconds = read_seconds(cstmt_arg(confdoc_child(d->root, "pause_minimized", NULL), 0));
	guint sel = 0;
	for (guint i = 1; seconds > 0 && pause_values[i]; i++) {
		sel = i; // the nearest that is not shorter
		if (atoi(pause_values[i]) >= seconds) {
			break;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->pause_after), sel);
	const char *sound = cstmt_arg(confdoc_child(d->root, "pause_minimized_sound", NULL), 0);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->pause_sound),
		sound && g_ascii_strcasecmp(sound, "pause") == 0 ? 1 : 0);
	GPtrArray *ids = read_list(d, "pause_minimized_except");
	ui_app_list_set(p->pause_except, ids);
	g_ptr_array_free(ids, TRUE);
	ids = read_list(d, "remember_windows_except");
	ui_app_list_set(p->remember_except, ids);
	g_ptr_array_free(ids, TRUE);
	rebuild_rules(p);
	p->updating = false;
}

static GtkWidget *choice(const char *const *labels, struct appwin_page *p) {
	GtkWidget *dd = gtk_drop_down_new_from_strings(labels);
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_choice), p);
	return dd;
}

GtkWidget *appwin_page_new(struct settings *s) {
	struct appwin_page *p = g_new0(struct appwin_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("App windows",
		"What happens to the apps behind the windows: when they stop answering, while they "
		"are minimized, and where their windows open.", &content);

	GtkWidget *hung = ui_group(content, "Apps that stop responding", NULL);
	p->not_responding = gtk_switch_new();
	g_signal_connect(p->not_responding, "notify::active", G_CALLBACK(on_switch), p);
	ui_row(hung, "Show when an app does not respond",
		"An app that does not answer within five seconds after you click, type or close it "
		"gets \"(Not Responding)\" in its title and turns pale; closing it then offers to end "
		"it", p->not_responding);

	GtkWidget *paused = ui_group(content, "Minimized apps",
		"An app all of whose windows are minimized is stopped after a while, so it takes no "
		"processor time or battery until you open one of its windows again. Terminals, apps "
		"with a window shown and apps that keep the screen on keep running.");
	p->pause_after = choice(pause_labels, p);
	ui_row(paused, "Stop minimized apps", "Together with the programs they started",
		p->pause_after);
	p->pause_sound = choice(sound_labels, p);
	ui_row(paused, "Minimized apps playing sound", "Music or a call in a minimized app",
		p->pause_sound);
	p->pause_except = ui_app_list_new(content, "Never stop these apps",
		"For apps that must keep working while minimized, such as downloads or backups.",
		on_pause_except, p);

	GtkWidget *places = ui_group(content, "Where windows open", NULL);
	p->remember = gtk_switch_new();
	g_signal_connect(p->remember, "notify::active", G_CALLBACK(on_switch), p);
	ui_row(places, "Open apps where they were closed",
		"The first window of an app opens on the screen, at the place and in the size its "
		"window had when it was closed last, maximized or snapped if it was", p->remember);
	p->remember_except = ui_app_list_new(content, "Apps that open where they like",
		"These open in the middle of the screen as new windows do.", on_remember_except, p);

	p->rules = ui_group(content, "Window rules",
		"What happens to the windows of an app when they open. The rules are for_window lines "
		"in common.conf and apply to windows opened after they were made.");
	GtkWidget *add = ui_group(content, NULL, NULL);
	p->open_apps = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->open_apps, "notify::selected", G_CALLBACK(on_open_app), p);
	p->rule_app = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->rule_app), "App id, e.g. firefox");
	gtk_widget_set_size_request(p->rule_app, 220, -1);
	GtkWidget *pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(pick), p->open_apps);
	gtk_box_append(GTK_BOX(pick), p->rule_app);
	ui_row(add, "App", "Pick one that is open, or type its app id", pick);

	GtkStringList *labels = gtk_string_list_new(NULL);
	for (size_t i = 0; i < G_N_ELEMENTS(actions); i++) {
		gtk_string_list_append(labels, actions[i].label);
	}
	p->rule_action = gtk_drop_down_new(G_LIST_MODEL(labels), NULL);
	g_signal_connect(p->rule_action, "notify::selected", G_CALLBACK(on_action), p);
	p->rule_desktop = gtk_spin_button_new_with_range(1, 20, 1);
	gtk_widget_set_visible(p->rule_desktop, false);
	GtkWidget *button = gtk_button_new_with_label("Add rule");
	g_signal_connect(button, "clicked", G_CALLBACK(on_add_rule), p);
	GtkWidget *what = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(what), p->rule_action);
	gtk_box_append(GTK_BOX(what), p->rule_desktop);
	gtk_box_append(GTK_BOX(what), button);
	ui_row(add, "Rule", NULL, what);
	fill_open_apps(p);

	s->appwin_page = p;
	appwin_page_refresh(s);
	return page;
}
