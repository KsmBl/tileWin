#define _GNU_SOURCE
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../dm/config.h"
#include "../dm/sessions.h"
#include "list.h"
#include "settings.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "tw_theme.h"

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

/*
 * The Login screen page: a picture of the login screen on top that follows every
 * change, tabs below it, and something to choose wherever there are choices. A
 * change only counts after Save, in a bar that is only there when there is
 * something to save; the helper checks the file before it replaces the old one.
 *
 * What can be set comes from the table in dm/config.c. A setting no tab asks for
 * by name lands in Advanced, so a new one always has a row.
 *
 * For tests: TILEWIN_DM_APPLY is a helper run as it is, without pkexec,
 * TILEWIN_DM_CONF is the file to read, TILEWIN_DM_STATE the shared folder.
 */
#define THUMB_W 304
#define THUMB_H 171

struct login_page {
	struct settings *s;
	GtkWidget **controls; // as dm_options: an entry, a switch, a spin button or a scale
	bool *placed;
	GtkWidget *service_switch, *service_label;
	GtkWidget *preview, *bar, *bar_label, *env_view, *picture_hint, *autologin_hint;
	GtkWidget *wallpaper_kind, *wallpaper_color, *wallpaper_pick;
	GtkWidget *accent_kind, *accent_color;
	GPtrArray *user_rows; // GtkSwitch *, with data "user"
	char *saved;
	bool updating, enabled;
	char *render_file;
	cairo_surface_t *shot;
	guint render_timer;
	bool rendering, render_again;
};

static const char *conf_path(void) {
	const char *path = g_getenv("TILEWIN_DM_CONF");
	return path && *path ? path : DM_CONFIG_PATH;
}

static char *helper_path(void) {
	const char *test = g_getenv("TILEWIN_DM_APPLY");
	return g_strdup(test && *test ? test : TILEWIN_LIBEXECDIR "/tilewin-dm-apply");
}

static int option_index(const char *key) {
	for (size_t i = 0; i < dm_option_count; i++) {
		if (strcmp(dm_options[i].key, key) == 0) {
			return (int)i;
		}
	}
	g_error("no setting %s", key);
	return -1;
}

/* ---------- values ---------- */

static char *control_value(GtkWidget *control) {
	if (GTK_IS_SWITCH(control)) {
		return g_strdup(gtk_switch_get_active(GTK_SWITCH(control)) ? "yes" : "no");
	}
	if (GTK_IS_RANGE(control)) {
		return g_strdup_printf("%d", (int)round(gtk_range_get_value(GTK_RANGE(control))));
	}
	if (GTK_IS_SPIN_BUTTON(control)) {
		return g_strdup_printf("%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)));
	}
	return g_strdup(gtk_editable_get_text(GTK_EDITABLE(control)));
}

static void control_set(GtkWidget *control, const char *value) {
	if (GTK_IS_SWITCH(control)) {
		gtk_switch_set_active(GTK_SWITCH(control), strcmp(value, "yes") == 0 ||
			strcmp(value, "true") == 0 || strcmp(value, "on") == 0 || strcmp(value, "1") == 0);
	} else if (GTK_IS_RANGE(control)) {
		gtk_range_set_value(GTK_RANGE(control), atoi(value));
	} else if (GTK_IS_SPIN_BUTTON(control)) {
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(control), atoi(value));
	} else {
		gtk_editable_set_text(GTK_EDITABLE(control), value);
	}
}

/* The value of a setting as it is on the page. Newly allocated. */
static char *value_of(struct login_page *p, const char *key) {
	char *value = control_value(p->controls[option_index(key)]);
	// the color and font buttons of the settings call "the theme's" empty
	if (!*value && (strcmp(key, "accent") == 0 || strcmp(key, "font") == 0)) {
		g_free(value);
		value = g_strdup("theme");
	}
	return value;
}

/* The file as it would be: what differs from the defaults, and the env lines. */
static char *config_text(struct login_page *p) {
	GString *out = g_string_new("# The display manager of tileWin, written by its settings page.\n"
		"# See docs/display-manager.md for what each line means.\n\n");
	for (size_t i = 0; i < dm_option_count; i++) {
		const struct dm_option *o = &dm_options[i];
		char *value = value_of(p, o->key);
		value[strcspn(value, "\r\n")] = '\0'; // a line ends a value
		if (strcmp(value, o->fallback) != 0) {
			g_string_append_printf(out, o->type == DM_TEXT ? "%s \"%s\"\n" : "%s %s\n", o->key, value);
		}
		g_free(value);
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->env_view));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	char *env = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	char **lines = g_strsplit(env, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *line = g_strstrip(lines[i]);
		if (*line) {
			g_string_append_printf(out, "env %s\n", line);
		}
	}
	g_strfreev(lines);
	g_free(env);
	return g_string_free(out, FALSE);
}

/* ---------- the look the login screen will have ---------- */

static const char *state_dir(void) {
	const char *dir = g_getenv("TILEWIN_DM_STATE");
	return dir && *dir ? dir : "/var/lib/tilewin-dm/shared";
}

static char *read_word(const char *dir, const char *name) {
	char *path = g_build_filename(dir, name, NULL);
	char *word = tw_read_first_line(path);
	g_free(path);
	return word;
}

/* The theme after "follow": the one of who logged in last, or as a start your own. */
static char *effective_theme(struct login_page *p) {
	char *theme = value_of(p, "theme");
	if (strcmp(theme, "follow") != 0) {
		return theme;
	}
	g_free(theme);
	char *word = read_word(state_dir(), "last-theme");
	if (word && *word) {
		theme = g_strdup(word);
	} else {
		char *own = tw_theme_current_name();
		theme = g_strdup(own);
		free(own);
	}
	free(word);
	return theme;
}

static char *effective_scheme(struct login_page *p) {
	char *scheme = value_of(p, "color_scheme");
	if (strcmp(scheme, "follow") != 0) {
		return scheme;
	}
	g_free(scheme);
	char *word = read_word(state_dir(), "last-scheme");
	if (word && strcmp(word, "dark") == 0) {
		scheme = g_strdup("dark");
	} else if (word && strcmp(word, "light") == 0) {
		scheme = g_strdup("light");
	} else {
		scheme = g_strdup(tw_color_scheme_is_dark() ? "dark" : "light");
	}
	free(word);
	return scheme;
}

/*
 * The files of the pictures of the login screen, in the runtime folder: named
 * after this process, as two settings apps at once (or the tests, side by
 * side) would draw into each other's otherwise. They go when the app does.
 */
static char *runtime_file_of(const char *name, int pid) {
	char *base = g_strdup_printf("tilewin-dm-%s-%d", name, pid);
	char *path = g_build_filename(g_get_user_runtime_dir(), base, NULL);
	g_free(base);
	return path;
}

static char *runtime_file(const char *name) {
	return runtime_file_of(name, (int)getpid());
}

/* The files of that process: the files first, then the folders (remove() takes both). */
static void remove_runtime_files_of(int pid) {
	static const char *const names[] = { "render.png", "render.conf", "preview.conf",
		"look/tileWin/current-theme", "look/tileWin/color-scheme", "look/tileWin", "look" };
	char *look = runtime_file_of("look", pid);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char *path = strncmp(names[i], "look/", 5) == 0 ?
			g_build_filename(look, names[i] + 5, NULL) : runtime_file_of(names[i], pid);
		remove(path);
		g_free(path);
	}
	g_free(look);
}

static void remove_runtime_files(void) {
	remove_runtime_files_of((int)getpid());
}

/* Those of settings apps that ended without cleaning up (killed, crashed). */
static void remove_stale_runtime_files(void) {
	GDir *dir = g_dir_open(g_get_user_runtime_dir(), 0, NULL);
	const char *name;
	while (dir && (name = g_dir_read_name(dir))) {
		const char *dash = strrchr(name, '-');
		if (!g_str_has_prefix(name, "tilewin-dm-") || !dash || !g_ascii_isdigit(dash[1])) {
			continue;
		}
		int pid = atoi(dash + 1);
		char proc[32];
		snprintf(proc, sizeof(proc), "/proc/%d", pid);
		if (pid > 0 && pid != (int)getpid() && access(proc, F_OK) != 0) {
			remove_runtime_files_of(pid);
		}
	}
	if (dir) {
		g_dir_close(dir);
	}
}

/*
 * A config folder like the one the daemon writes for the greeter: the theme and
 * the scheme of the login screen, and none of your own settings. Newly allocated.
 */
static char *write_look_folder(struct login_page *p) {
	char *dir = runtime_file("look");
	char *inner = g_build_filename(dir, "tileWin", NULL);
	g_mkdir_with_parents(inner, 0700);
	char *theme = effective_theme(p), *scheme = effective_scheme(p);
	char *path = g_build_filename(inner, "current-theme", NULL);
	char *line = g_strdup_printf("%s\n", theme);
	g_file_set_contents(path, line, -1, NULL);
	g_free(path);
	g_free(line);
	path = g_build_filename(inner, "color-scheme", NULL);
	line = g_strdup_printf("%s\n", scheme);
	g_file_set_contents(path, line, -1, NULL);
	g_free(path);
	g_free(line);
	g_free(theme);
	g_free(scheme);
	g_free(inner);
	return dir;
}

/* ---------- the picture of the login screen ---------- */

/*
 * The greeter itself draws it, without a screen, so the picture is the real thing
 * and not a likeness. A change only asks for it a moment later, and what comes in
 * during a drawing is drawn right after it.
 */
static void render_start(struct login_page *p);

static void render_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct login_page *p = data;
	g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL);
	g_object_unref(source);
	p->rendering = false;
	cairo_surface_t *shot = cairo_image_surface_create_from_png(p->render_file);
	if (cairo_surface_status(shot) == CAIRO_STATUS_SUCCESS) {
		if (p->shot) {
			cairo_surface_destroy(p->shot);
		}
		p->shot = shot;
		gtk_widget_queue_draw(p->preview);
	} else {
		cairo_surface_destroy(shot);
	}
	if (p->render_again) {
		p->render_again = false;
		render_start(p);
	}
}

static void render_start(struct login_page *p) {
	if (p->rendering) {
		p->render_again = true;
		return;
	}
	char *text = config_text(p);
	char *conf = runtime_file("render.conf");
	if (!p->render_file) {
		p->render_file = runtime_file("render.png");
	}
	g_file_set_contents(conf, text, -1, NULL);
	char *look = write_look_folder(p);
	char size[32];
	snprintf(size, sizeof(size), "%dx%d", THUMB_W * 2, THUMB_H * 2);
	GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", look, TRUE);
	GSubprocess *proc = g_subprocess_launcher_spawn(launcher, NULL, "tilewin-dm-greeter", "--render",
		p->render_file, "--size", size, "--config", conf, NULL);
	if (proc) {
		p->rendering = true;
		g_subprocess_wait_check_async(proc, NULL, render_done, p);
	}
	g_object_unref(launcher);
	g_free(look);
	g_free(conf);
	g_free(text);
}

/* A fixed size whatever the picture is, or the page would move with it. */
static void draw_preview(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
	struct login_page *p = data;
	if (!p->shot) {
		cairo_set_source_rgb(cr, 0.1, 0.1, 0.12);
		cairo_paint(cr);
		return;
	}
	cairo_scale(cr, (double)w / cairo_image_surface_get_width(p->shot),
		(double)h / cairo_image_surface_get_height(p->shot));
	cairo_set_source_surface(cr, p->shot, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
	cairo_paint(cr);
}

static gboolean render_later(gpointer data) {
	struct login_page *p = data;
	p->render_timer = 0;
	render_start(p);
	return G_SOURCE_REMOVE;
}

/* ---------- saving ---------- */

static void update_page(struct login_page *p) {
	if (p->updating) {
		return;
	}
	if (p->render_timer) {
		g_source_remove(p->render_timer);
	}
	p->render_timer = g_timeout_add(90, render_later, p);
	char *text = config_text(p);
	gtk_revealer_set_reveal_child(GTK_REVEALER(p->bar), g_strcmp0(text, p->saved) != 0);
	g_free(text);

	char *user = value_of(p, "autologin_user");
	gtk_widget_set_visible(p->autologin_hint, *user != '\0');
	g_free(user);

	// a picture only the owner can read would not reach the login screen
	char *wallpaper = value_of(p, "wallpaper");
	bool blocked = false;
	if (wallpaper[0] == '/') {
		struct stat st;
		char *path = g_strdup(wallpaper);
		blocked = stat(path, &st) != 0 || !(st.st_mode & S_IROTH);
		for (char *slash = strrchr(path, '/'); !blocked && slash && slash != path;
				slash = strrchr(path, '/')) {
			*slash = '\0';
			blocked = stat(path, &st) != 0 || !(st.st_mode & S_IXOTH);
		}
		g_free(path);
	}
	gtk_widget_set_visible(p->picture_hint, blocked);
	g_free(wallpaper);
}

static void on_any_change(GtkWidget *widget, gpointer data) {
	update_page(data);
}

static void on_any_notify(GObject *object, GParamSpec *pspec, gpointer data) {
	update_page(data);
}

static void sync_user_rows(struct login_page *p);
static void sync_wallpaper(struct login_page *p);
static void sync_accent(struct login_page *p);

static void set_all(struct login_page *p, const struct dm_config *config) {
	p->updating = true;
	for (size_t i = 0; i < dm_option_count; i++) {
		const char *value = config ? config->values[i] : dm_options[i].fallback;
		const char *key = dm_options[i].key;
		if ((strcmp(key, "accent") == 0 || strcmp(key, "font") == 0) && strcmp(value, "theme") == 0) {
			value = "";
		}
		control_set(p->controls[i], value);
	}
	GString *env = g_string_new(NULL);
	for (size_t i = 0; config && i < config->env_count; i++) {
		g_string_append_printf(env, "%s\n", config->env[i]);
	}
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->env_view)), env->str, -1);
	g_string_free(env, TRUE);
	sync_user_rows(p);
	sync_wallpaper(p);
	sync_accent(p);
	p->updating = false;
}

static void load_values(struct login_page *p) {
	struct dm_config *config = dm_config_load(conf_path());
	set_all(p, config);
	dm_config_free(config);
	g_free(p->saved);
	p->saved = config_text(p);
	update_page(p);
}

struct call {
	struct login_page *p;
	char *done_text;
	char *input;
	void (*done)(struct login_page *p, bool ok, const char *output, const char *input);
};

static void helper_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct call *call = data;
	char *out = NULL, *err = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, NULL) &&
		g_subprocess_get_successful(G_SUBPROCESS(source));
	if (!ok) {
		int status = g_subprocess_get_if_exited(G_SUBPROCESS(source)) ?
			g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
		char *text = g_strdup_printf("It did not work: %s", err && *err ? g_strstrip(err) :
			status == 126 || status == 127 ? "not authorized" : "the helper failed");
		settings_problem(call->p->s, text, NULL, NULL, NULL);
		g_free(text);
	} else if (call->done_text) {
		settings_status(call->p->s, "%s", call->done_text);
	}
	if (call->done) {
		call->done(call->p, ok, out, call->input);
	}
	g_free(out);
	g_free(err);
	g_free(call->done_text);
	g_free(call->input);
	g_free(call);
	g_object_unref(source);
}

static void run_helper(struct login_page *p, const char *what, const char *input, const char *done_text,
		void (*done)(struct login_page *, bool, const char *, const char *), bool as_root) {
	char *helper = helper_path();
	const char *argv[4];
	int n = 0;
	const char *test = g_getenv("TILEWIN_DM_APPLY");
	if (as_root && (!test || !*test)) {
		argv[n++] = "pkexec";
	}
	argv[n++] = helper;
	argv[n++] = what;
	argv[n] = NULL;
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_free(helper);
	if (!proc) {
		char *text = g_strdup_printf("It did not work: %s", error->message);
		settings_problem(p->s, text, NULL, NULL, NULL);
		g_free(text);
		g_error_free(error);
		return;
	}
	struct call *call = g_new0(struct call, 1);
	call->p = p;
	call->done_text = g_strdup(done_text);
	call->input = g_strdup(input);
	call->done = done;
	g_subprocess_communicate_utf8_async(proc, input ? input : "", NULL, helper_finished, call);
}

static void saved_done(struct login_page *p, bool ok, const char *output, const char *input) {
	if (ok) {
		g_free(p->saved);
		p->saved = g_strdup(input);
		update_page(p);
	}
}

static void on_save(GtkButton *button, gpointer data) {
	struct login_page *p = data;
	char *text = config_text(p);
	settings_problem_clear(p->s);
	run_helper(p, "write", text, "Saved. The login screen takes it the next time it starts.",
		saved_done, true);
	g_free(text);
}

static void on_discard(GtkButton *button, gpointer data) {
	load_values(data);
}

static void on_reset(GtkButton *button, gpointer data) {
	struct login_page *p = data;
	set_all(p, NULL);
	update_page(p);
}

static void on_try(GtkButton *button, gpointer data) {
	struct login_page *p = data;
	char *text = config_text(p);
	char *conf = runtime_file("preview.conf");
	GError *error = NULL;
	if (g_file_set_contents(conf, text, -1, &error)) {
		char *look = write_look_folder(p);
		char **env = g_environ_setenv(g_get_environ(), "XDG_CONFIG_HOME", look, TRUE);
		const char *argv[] = { "tilewin-dm-greeter", "--preview", "--config", conf, NULL };
		g_spawn_async(NULL, (char **)argv, env, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
		g_strfreev(env);
		g_free(look);
	}
	if (error) {
		char *problem = g_strdup_printf("No preview: %s", error->message);
		settings_problem(p->s, problem, NULL, NULL, NULL);
		g_free(problem);
		g_error_free(error);
	}
	g_free(conf);
	g_free(text);
}

/* ---------- in use or not ---------- */

static void show_service(struct login_page *p) {
	bool was = p->updating;
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->service_switch), p->enabled);
	gtk_switch_set_state(GTK_SWITCH(p->service_switch), p->enabled);
	gtk_label_set_text(GTK_LABEL(p->service_label), p->enabled ?
		"In use: you log in here when the computer starts" :
		"Not in use: another display manager starts with the computer");
	p->updating = was;
}

static void service_read(struct login_page *p, bool ok, const char *output, const char *input) {
	p->enabled = ok && output && g_str_has_prefix(output, "enabled");
	show_service(p);
}

static void service_changed(struct login_page *p, bool ok, const char *output, const char *input) {
	if (ok) {
		p->enabled = !p->enabled;
	}
	show_service(p);
}

static void on_service_answer(GObject *source, GAsyncResult *result, gpointer data) {
	struct login_page *p = data;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	if (choice == 1) {
		run_helper(p, p->enabled ? "disable" : "enable", NULL,
			p->enabled ? "The display manager used before is back at the next start of the computer." :
			"This login screen comes up at the next start of the computer.",
			service_changed, true);
	} else {
		show_service(p);
	}
}

static gboolean on_service_switch(GtkSwitch *widget, gboolean active, gpointer data) {
	struct login_page *p = data;
	if (p->updating || active == p->enabled) {
		return TRUE;
	}
	// this decides whether anyone can log in tomorrow: not on a click alone
	GtkAlertDialog *dialog = gtk_alert_dialog_new(active ?
		"Use this as the display manager of the computer?" : "Stop using this display manager?");
	gtk_alert_dialog_set_detail(dialog, active ?
		"It takes over from your current one at the next start of the computer. If it should "
		"not come up, Ctrl+Alt+F2 gets you to a text console, where "
		"\"sudo " TILEWIN_LIBEXECDIR "/tilewin-dm-apply disable\" puts the old one back." :
		"The display manager used before comes back at the next start of the computer.");
	gtk_alert_dialog_set_buttons(dialog, (const char *const[]){ "Cancel",
		active ? "Use it" : "Stop using it", NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, p->s->window, NULL, on_service_answer, p);
	g_object_unref(dialog);
	return TRUE;
}

/* ---------- choosing ---------- */

/* An entry nobody sees, which holds the value of what is shown instead. */
static GtkWidget *store_new(struct login_page *p, int index) {
	GtkWidget *entry = gtk_entry_new();
	g_signal_connect(entry, "changed", G_CALLBACK(on_any_change), p);
	p->controls[index] = entry;
	return entry;
}

static void on_segment(GtkToggleButton *button, gpointer data) {
	if (gtk_toggle_button_get_active(button)) {
		gtk_editable_set_text(GTK_EDITABLE(data), g_object_get_data(G_OBJECT(button), "value"));
	}
}

static void on_segment_entry(GtkEditable *entry, gpointer data) {
	const char *text = gtk_editable_get_text(entry);
	for (GtkWidget *b = gtk_widget_get_first_child(data); b; b = gtk_widget_get_next_sibling(b)) {
		if (GTK_IS_TOGGLE_BUTTON(b) && strcmp(text, g_object_get_data(G_OBJECT(b), "value")) == 0 &&
				!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b))) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
		}
	}
}

/* Buttons side by side, for a few choices that are all worth seeing at once. */
static GtkWidget *segments_new(struct login_page *p, int index, const char *const *labels) {
	GtkWidget *entry = store_new(p, index);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(box, "linked");
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	GtkWidget *first = NULL;
	const char *const *choices = dm_options[index].choices;
	for (int i = 0; choices[i]; i++) {
		GtkWidget *button = gtk_toggle_button_new_with_label(labels[i]);
		g_object_set_data(G_OBJECT(button), "value", (gpointer)choices[i]);
		if (first) {
			gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(first));
		} else {
			first = button;
		}
		g_signal_connect(button, "toggled", G_CALLBACK(on_segment), entry);
		gtk_box_append(GTK_BOX(box), button);
	}
	g_signal_connect(entry, "changed", G_CALLBACK(on_segment_entry), box);
	gtk_widget_set_visible(entry, FALSE);
	gtk_box_append(GTK_BOX(box), entry);
	return box;
}

/* Adds a row for a setting, with its words from the table. */
static GtkWidget *row_for(struct login_page *p, GtkWidget *group, const char *key, GtkWidget *control) {
	int index = option_index(key);
	p->placed[index] = true;
	// the table explains how to write a value; with something to choose that says nothing
	static const char *const chosen[] = { "theme", "color_scheme", "accent", "font", "card_style",
		"default_session", "dim", "card_width", "clock_format", "date_format", "keyboard_layout",
		"cursor_theme", "screen_scale", "screen_off_after", "autologin_session", "autologin_user", "repeat_delay", NULL };
	const char *help = dm_options[index].help;
	for (int i = 0; chosen[i]; i++) {
		if (strcmp(chosen[i], key) == 0) {
			help = NULL;
		}
	}
	return ui_row(group, dm_options[index].label, help, control);
}

static void add_switch(struct login_page *p, GtkWidget *group, const char *key) {
	int index = option_index(key);
	GtkWidget *control = gtk_switch_new();
	g_signal_connect(control, "notify::active", G_CALLBACK(on_any_notify), p);
	p->controls[index] = control;
	row_for(p, group, key, control);
}

static void add_scale(struct login_page *p, GtkWidget *group, const char *key) {
	int index = option_index(key);
	GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, dm_options[index].min,
		dm_options[index].max, 1);
	gtk_widget_set_size_request(scale, 240, -1);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
	g_signal_connect(scale, "value-changed", G_CALLBACK(on_any_change), p);
	p->controls[index] = scale;
	row_for(p, group, key, scale);
}

static void add_spin(struct login_page *p, GtkWidget *group, const char *key) {
	int index = option_index(key);
	GtkWidget *spin = gtk_spin_button_new_with_range(dm_options[index].min, dm_options[index].max, 1);
	g_signal_connect(spin, "value-changed", G_CALLBACK(on_any_change), p);
	p->controls[index] = spin;
	row_for(p, group, key, spin);
}

static void add_text(struct login_page *p, GtkWidget *group, const char *key) {
	GtkWidget *entry = store_new(p, option_index(key));
	gtk_widget_set_size_request(entry, 280, -1);
	row_for(p, group, key, entry);
}

/* A dropdown of values and words, with Custom… to type something else. */
static void add_presets(struct login_page *p, GtkWidget *group, const char *key, GPtrArray *values,
		GPtrArray *labels) {
	GtkWidget *entry = store_new(p, option_index(key));
	gtk_widget_set_size_request(entry, 200, -1);
	row_for(p, group, key, ui_presets(entry, values, labels));
}

static void offer(GPtrArray *values, GPtrArray *labels, const char *value, const char *label) {
	g_ptr_array_add(values, g_strdup(value));
	// a dropdown is as wide as its longest word, and the window with it
	g_ptr_array_add(labels, g_utf8_strlen(label, -1) > 24 ?
		g_strdup_printf("%.*s…", (int)(g_utf8_offset_to_pointer(label, 23) - label), label) :
		g_strdup(label));
}

static GPtrArray *strings_new(void) {
	return g_ptr_array_new_with_free_func(g_free);
}

/* The people of this computer, as the greeter lists them. */
static void add_people(GPtrArray *values, GPtrArray *labels) {
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent())) {
		if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && pw->pw_shell && !strstr(pw->pw_shell, "nologin") &&
				!strstr(pw->pw_shell, "/false")) {
			char *full = pw->pw_gecos && *pw->pw_gecos ? g_strndup(pw->pw_gecos, strcspn(pw->pw_gecos, ",")) : NULL;
			offer(values, labels, pw->pw_name, full && *full ? full : pw->pw_name);
			g_free(full);
		}
	}
	endpwent();
}

static void add_sessions(GPtrArray *values, GPtrArray *labels) {
	struct dm_session_list *list = dm_sessions_read();
	for (size_t i = 0; i < list->count; i++) {
		offer(values, labels, list->items[i].id, list->items[i].name);
	}
	dm_sessions_free(list);
}

/* Each format with what it looks like right now as its name. */
static void add_time_formats(struct login_page *p, GtkWidget *group, const char *key,
		const char *const *formats) {
	GPtrArray *values = strings_new(), *labels = strings_new();
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	for (int i = 0; formats[i]; i++) {
		char text[128] = "";
		if (*formats[i]) {
			strftime(text, sizeof(text), formats[i], &tm);
		}
		offer(values, labels, formats[i], *formats[i] ? text : "None");
	}
	add_presets(p, group, key, values, labels);
}

/* ---------- the wallpaper ---------- */

static void sync_wallpaper(struct login_page *p) {
	char *value = value_of(p, "wallpaper");
	guint kind = value[0] == '#' ? 1 : strcmp(value, "theme") == 0 ? 0 : 2;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->wallpaper_kind), kind);
	gtk_widget_set_visible(p->wallpaper_color, kind == 1);
	gtk_widget_set_visible(p->wallpaper_pick, kind == 2);
	if (kind == 1) {
		GdkRGBA rgba;
		if (gdk_rgba_parse(&rgba, value)) {
			gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(p->wallpaper_color), &rgba);
		}
	}
	if (kind == 2) {
		char *name = g_path_get_basename(value);
		gtk_button_set_label(GTK_BUTTON(p->wallpaper_pick), name);
		g_free(name);
	}
	g_free(value);
}

static void set_wallpaper(struct login_page *p, const char *value) {
	gtk_editable_set_text(GTK_EDITABLE(p->controls[option_index("wallpaper")]), value);
	bool was = p->updating;
	p->updating = true;
	sync_wallpaper(p);
	p->updating = was;
	update_page(p);
}

static void on_wallpaper_color(GObject *object, GParamSpec *pspec, gpointer data) {
	struct login_page *p = data;
	if (p->updating) {
		return;
	}
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
	char text[16];
	snprintf(text, sizeof(text), "#%02x%02x%02x", (int)round(c->red * 255),
		(int)round(c->green * 255), (int)round(c->blue * 255));
	set_wallpaper(p, text);
}

static void on_wallpaper_file(GObject *source, GAsyncResult *result, gpointer data) {
	struct login_page *p = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (file) {
		char *path = g_file_get_path(file);
		if (path) {
			set_wallpaper(p, path);
		}
		g_free(path);
		g_object_unref(file);
	} else {
		bool was = p->updating;
		p->updating = true;
		sync_wallpaper(p);
		p->updating = was;
	}
}

static void pick_picture(struct login_page *p) {
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "A picture for the login screen");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Pictures");
	gtk_file_filter_add_mime_type(filter, "image/*");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_open(dialog, p->s->window, NULL, on_wallpaper_file, p);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void on_wallpaper_pick(GtkButton *button, gpointer data) {
	pick_picture(data);
}

static void on_wallpaper_kind(GObject *object, GParamSpec *pspec, gpointer data) {
	struct login_page *p = data;
	if (p->updating) {
		return;
	}
	switch (gtk_drop_down_get_selected(GTK_DROP_DOWN(object))) {
	case 0:
		set_wallpaper(p, "theme");
		break;
	case 1:
		set_wallpaper(p, "#204070");
		break;
	default:
		pick_picture(p);
		break;
	}
}

static void add_wallpaper(struct login_page *p, GtkWidget *group) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	p->wallpaper_kind = gtk_drop_down_new_from_strings((const char *const[]){
		"The theme's", "A color", "A picture", NULL });
	g_signal_connect(p->wallpaper_kind, "notify::selected", G_CALLBACK(on_wallpaper_kind), p);
	gtk_box_append(GTK_BOX(box), p->wallpaper_kind);
	p->wallpaper_color = gtk_color_dialog_button_new(gtk_color_dialog_new());
	g_signal_connect(p->wallpaper_color, "notify::rgba", G_CALLBACK(on_wallpaper_color), p);
	gtk_box_append(GTK_BOX(box), p->wallpaper_color);
	p->wallpaper_pick = gtk_button_new_with_label("Choose…");
	g_signal_connect(p->wallpaper_pick, "clicked", G_CALLBACK(on_wallpaper_pick), p);
	gtk_box_append(GTK_BOX(box), p->wallpaper_pick);
	GtkWidget *entry = store_new(p, option_index("wallpaper"));
	gtk_widget_set_visible(entry, FALSE);
	gtk_box_append(GTK_BOX(box), entry);
	int index = option_index("wallpaper");
	p->placed[index] = true;
	ui_row(group, dm_options[index].label, NULL, box);
	p->picture_hint = ui_row(group, NULL, "The login screen runs as a user of its own and cannot read "
		"this picture, so it would show the theme's. Keep it in a folder everyone can read, such as "
		"/usr/share/backgrounds.", NULL);
	gtk_widget_set_visible(p->picture_hint, FALSE);
}

/* ---------- the accent ---------- */

static void sync_accent(struct login_page *p) {
	char *value = value_of(p, "accent");
	bool own = value[0] == '#';
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->accent_kind), own);
	gtk_widget_set_visible(p->accent_color, own);
	GdkRGBA rgba;
	if (own && gdk_rgba_parse(&rgba, value)) {
		gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(p->accent_color), &rgba);
	}
	g_free(value);
}

static void set_accent(struct login_page *p, const char *value) {
	gtk_editable_set_text(GTK_EDITABLE(p->controls[option_index("accent")]), value);
	bool was = p->updating;
	p->updating = true;
	sync_accent(p);
	p->updating = was;
	update_page(p);
}

static void on_accent_kind(GObject *object, GParamSpec *pspec, gpointer data) {
	struct login_page *p = data;
	if (!p->updating) {
		set_accent(p, gtk_drop_down_get_selected(GTK_DROP_DOWN(object)) ? "#0078d4" : "");
	}
}

static void on_accent_color(GObject *object, GParamSpec *pspec, gpointer data) {
	struct login_page *p = data;
	if (p->updating) {
		return;
	}
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
	char text[16];
	snprintf(text, sizeof(text), "#%02x%02x%02x", (int)round(c->red * 255),
		(int)round(c->green * 255), (int)round(c->blue * 255));
	set_accent(p, text);
}

static void add_accent(struct login_page *p, GtkWidget *group) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	p->accent_kind = gtk_drop_down_new_from_strings((const char *const[]){
		"The theme's", "A color", NULL });
	g_signal_connect(p->accent_kind, "notify::selected", G_CALLBACK(on_accent_kind), p);
	gtk_box_append(GTK_BOX(box), p->accent_kind);
	p->accent_color = gtk_color_dialog_button_new(gtk_color_dialog_new());
	g_signal_connect(p->accent_color, "notify::rgba", G_CALLBACK(on_accent_color), p);
	gtk_box_append(GTK_BOX(box), p->accent_color);
	GtkWidget *entry = store_new(p, option_index("accent"));
	gtk_widget_set_visible(entry, FALSE);
	gtk_box_append(GTK_BOX(box), entry);
	row_for(p, group, "accent", box);
}

/* ---------- who is listed ---------- */

static void sync_user_rows(struct login_page *p) {
	char *hidden = value_of(p, "hide_users");
	for (guint i = 0; i < p->user_rows->len; i++) {
		GtkWidget *control = p->user_rows->pdata[i];
		gtk_switch_set_active(GTK_SWITCH(control),
			!dm_word_in_list(hidden, g_object_get_data(G_OBJECT(control), "user")));
	}
	g_free(hidden);
}

static void on_user_listed(GObject *object, GParamSpec *pspec, gpointer data) {
	struct login_page *p = data;
	if (p->updating) {
		return;
	}
	// who is not on this computer's list stays as the file has them
	char *old = value_of(p, "hide_users");
	GString *out = g_string_new(NULL);
	char **words = g_strsplit_set(old, " \t,", -1);
	for (int i = 0; words[i]; i++) {
		bool known = false;
		for (guint j = 0; j < p->user_rows->len; j++) {
			known |= strcmp(words[i], g_object_get_data(G_OBJECT(p->user_rows->pdata[j]), "user")) == 0;
		}
		if (*words[i] && !known) {
			g_string_append_printf(out, "%s%s", out->len ? " " : "", words[i]);
		}
	}
	g_strfreev(words);
	g_free(old);
	for (guint j = 0; j < p->user_rows->len; j++) {
		GtkWidget *control = p->user_rows->pdata[j];
		if (!gtk_switch_get_active(GTK_SWITCH(control))) {
			g_string_append_printf(out, "%s%s", out->len ? " " : "",
				(const char *)g_object_get_data(G_OBJECT(control), "user"));
		}
	}
	gtk_editable_set_text(GTK_EDITABLE(p->controls[option_index("hide_users")]), out->str);
	g_string_free(out, TRUE);
}

/* ---------- the tabs ---------- */

static GtkWidget *tab_new(GtkWidget *stack, const char *name, const char *title) {
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(box, 32);
	gtk_widget_set_margin_end(box, 32);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	gtk_stack_add_titled(GTK_STACK(stack), scroll, name, title);
	return box;
}

static void build_look(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Colors and picture", NULL);
	GPtrArray *values = strings_new(), *labels = strings_new();
	offer(values, labels, "follow", "Of who logged in last");
	list_t *names = tw_theme_list();
	for (int i = 0; names && i < names->length; i++) {
		const char *name = names->items[i];
		char *error = NULL;
		struct tw_theme *theme = tw_theme_load(name, &error);
		free(error);
		offer(values, labels, name, theme && theme->title ? theme->title : name);
		if (theme) {
			tw_theme_free(theme);
		}
	}
	if (names) {
		list_free_items_and_destroy(names);
	}
	add_presets(p, group, "theme", values, labels);
	row_for(p, group, "color_scheme", segments_new(p, option_index("color_scheme"),
		(const char *const[]){ "Follow", "Light", "Dark", NULL }));
	add_wallpaper(p, group);
	add_scale(p, group, "dim");
	add_accent(p, group);
	GtkWidget *font = store_new(p, option_index("font"));
	row_for(p, group, "font", ui_font_field(font));

	group = ui_group(tab, "The login screen itself", NULL);
	row_for(p, group, "card_position", segments_new(p, option_index("card_position"),
		(const char *const[]){ "Center", "Left", "Right", NULL }));
	row_for(p, group, "card_style", segments_new(p, option_index("card_style"),
		(const char *const[]){ "As the theme", "A window", "Plain", NULL }));
	add_scale(p, group, "card_width");
	add_text(p, group, "card_title");
	add_text(p, group, "greeting");
	add_switch(p, group, "card_on_all_screens");
}

static void build_login(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Who and what comes first", NULL);
	GPtrArray *values = strings_new(), *labels = strings_new();
	offer(values, labels, "", "Nobody in particular");
	add_people(values, labels);
	add_presets(p, group, "default_user", values, labels);
	add_switch(p, group, "remember_user");
	values = strings_new();
	labels = strings_new();
	add_sessions(values, labels);
	add_presets(p, group, "default_session", values, labels);
	add_switch(p, group, "remember_session");

	group = ui_group(tab, "Automatic login", NULL);
	values = strings_new();
	labels = strings_new();
	offer(values, labels, "", "Off");
	add_people(values, labels);
	add_presets(p, group, "autologin_user", values, labels);
	p->autologin_hint = ui_row(group, NULL, "Anyone who starts this computer is logged in as that user, "
		"without a password.", NULL);
	gtk_widget_add_css_class(p->autologin_hint, "tw-warning");
	gtk_widget_set_visible(p->autologin_hint, FALSE);
	values = strings_new();
	labels = strings_new();
	offer(values, labels, "", "The one that comes first");
	add_sessions(values, labels);
	add_presets(p, group, "autologin_session", values, labels);
	add_switch(p, group, "autologin_once");

	group = ui_group(tab, "The password", NULL);
	row_for(p, group, "password_style", segments_new(p, option_index("password_style"),
		(const char *const[]){ "Dots", "Stars", "Nothing", NULL }));
	add_switch(p, group, "password_reveal");
	add_switch(p, group, "show_caps_lock");
}

static void build_shown(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Clock", NULL);
	row_for(p, group, "clock", segments_new(p, option_index("clock"),
		(const char *const[]){ "As the theme", "Top", "Left", "Right", "None", NULL }));
	add_time_formats(p, group, "clock_format", (const char *const[]){ "%H:%M", "%H:%M:%S",
		"%I:%M %p", NULL });
	add_time_formats(p, group, "date_format", (const char *const[]){ "%A, %e %B", "%e %B %Y",
		"%Y-%m-%d", "%m/%d/%Y", "", NULL });

	group = ui_group(tab, "Users", NULL);
	add_switch(p, group, "show_users");
	add_switch(p, group, "show_full_names");
	add_switch(p, group, "show_avatars");
	GPtrArray *names = strings_new(), *full = strings_new();
	add_people(names, full);
	if (names->len > 0) {
		GtkWidget *people = ui_group(tab, "Who is listed", NULL);
		for (guint i = 0; i < names->len; i++) {
			GtkWidget *control = gtk_switch_new();
			g_object_set_data_full(G_OBJECT(control), "user", g_strdup(names->pdata[i]), g_free);
			g_signal_connect(control, "notify::active", G_CALLBACK(on_user_listed), p);
			g_ptr_array_add(p->user_rows, control);
			ui_row(people, full->pdata[i], names->pdata[i], control);
		}
	}
	g_ptr_array_unref(names);
	g_ptr_array_unref(full);
	GtkWidget *hidden = gtk_entry_new();
	g_object_ref_sink(hidden); // never shown: the switches above are how it is set
	g_signal_connect(hidden, "changed", G_CALLBACK(on_any_change), p);
	p->controls[option_index("hide_users")] = hidden;
	p->placed[option_index("hide_users")] = true;

	group = ui_group(tab, "Also on the card", NULL);
	add_switch(p, group, "show_sessions");
	add_switch(p, group, "show_layout");
}

static void build_power(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Power buttons", NULL);
	add_switch(p, group, "show_power");
	add_switch(p, group, "allow_poweroff");
	add_switch(p, group, "allow_reboot");
	add_switch(p, group, "allow_suspend");
	add_switch(p, group, "allow_hibernate");

	group = ui_group(tab, "Screen", NULL);
	GPtrArray *times = strings_new(), *names = strings_new();
	offer(times, names, "60", "After 1 minute");
	offer(times, names, "300", "After 5 minutes");
	offer(times, names, "600", "After 10 minutes");
	offer(times, names, "1800", "After 30 minutes");
	offer(times, names, "0", "Never");
	add_presets(p, group, "screen_off_after", times, names);
	GPtrArray *values = strings_new(), *labels = strings_new();
	offer(values, labels, "auto", "As the screen says");
	offer(values, labels, "1", "100%");
	offer(values, labels, "1.25", "125%");
	offer(values, labels, "1.5", "150%");
	offer(values, labels, "2", "200%");
	add_presets(p, group, "screen_scale", values, labels);
}

/* The keyboard layouts X knows, by the name people know them by. */
static void add_layouts(GPtrArray *values, GPtrArray *labels) {
	char *text = NULL;
	if (!g_file_get_contents("/usr/share/X11/xkb/rules/evdev.lst", &text, NULL, NULL)) {
		return;
	}
	char **lines = g_strsplit(text, "\n", -1);
	bool in_layouts = false;
	for (int i = 0; lines[i]; i++) {
		if (lines[i][0] == '!') {
			in_layouts = strcmp(lines[i], "! layout") == 0;
		} else if (in_layouts) {
			char *line = g_strstrip(lines[i]);
			size_t len = strcspn(line, " \t");
			if (len && line[len]) {
				char *code = g_strndup(line, len);
				offer(values, labels, code, g_strstrip(line + len));
				g_free(code);
			}
		}
	}
	g_strfreev(lines);
	g_free(text);
}

static void add_cursor_themes(GPtrArray *values, GPtrArray *labels, const char *dir) {
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	while (d && (name = g_dir_read_name(d))) {
		char *cursors = g_build_filename(dir, name, "cursors", NULL);
		bool known = false;
		for (guint i = 0; i < values->len; i++) {
			known |= strcmp(values->pdata[i], name) == 0;
		}
		if (!known && g_file_test(cursors, G_FILE_TEST_IS_DIR)) {
			offer(values, labels, name, name);
		}
		g_free(cursors);
	}
	if (d) {
		g_dir_close(d);
	}
}

static void build_input(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Keyboard", NULL);
	GPtrArray *values = strings_new(), *labels = strings_new();
	offer(values, labels, "", "The default");
	add_layouts(values, labels);
	add_presets(p, group, "keyboard_layout", values, labels);
	add_text(p, group, "keyboard_variant");
	add_text(p, group, "keyboard_options");
	add_switch(p, group, "numlock");
	add_scale(p, group, "repeat_delay");
	add_scale(p, group, "repeat_rate");

	group = ui_group(tab, "Pointer", NULL);
	add_switch(p, group, "tap_to_click");
	values = strings_new();
	labels = strings_new();
	offer(values, labels, "", "The default");
	add_cursor_themes(values, labels, "/usr/share/icons");
	add_cursor_themes(values, labels, "/usr/local/share/icons");
	add_presets(p, group, "cursor_theme", values, labels);
	add_scale(p, group, "cursor_size");
	add_switch(p, group, "hide_cursor");
}

static void build_advanced(struct login_page *p, GtkWidget *tab) {
	GtkWidget *group = ui_group(tab, "Everything else", NULL);
	add_switch(p, group, "allow_root");
	add_spin(p, group, "fail_delay");
	add_spin(p, group, "clear_password_after");
	add_spin(p, group, "vt");
	add_spin(p, group, "min_uid");
	add_spin(p, group, "max_uid");
	add_text(p, group, "session_path");
	// a new setting nobody placed yet still gets a row
	for (size_t i = 0; i < dm_option_count; i++) {
		if (p->placed[i]) {
			continue;
		}
		switch (dm_options[i].type) {
		case DM_BOOL:
			add_switch(p, group, dm_options[i].key);
			break;
		case DM_INT:
			add_spin(p, group, dm_options[i].key);
			break;
		default:
			add_text(p, group, dm_options[i].key);
			break;
		}
	}

	group = ui_group(tab, "Environment of the sessions", "KEY=value, one on a line");
	p->env_view = gtk_text_view_new();
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(p->env_view), TRUE);
	gtk_widget_set_size_request(p->env_view, -1, 90);
	gtk_widget_set_margin_start(p->env_view, 8);
	gtk_widget_set_margin_end(p->env_view, 8);
	gtk_widget_set_margin_top(p->env_view, 8);
	gtk_widget_set_margin_bottom(p->env_view, 8);
	g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->env_view)), "changed",
		G_CALLBACK(update_page), p);
	gtk_list_box_append(GTK_LIST_BOX(group), p->env_view);

	group = ui_group(tab, "Start over", NULL);
	GtkWidget *reset = gtk_button_new_with_label("Reset everything");
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), p);
	ui_row(group, "All settings of this page back to how they came",
		"Nothing counts until you press Save", reset);
}

static void on_tab_toggled(GtkToggleButton *button, gpointer stack) {
	if (gtk_toggle_button_get_active(button)) {
		gtk_stack_set_visible_child_name(GTK_STACK(stack), g_object_get_data(G_OBJECT(button), "tab"));
	}
}

/* The search can bring a tab in front too: its button goes along. */
static void on_tab_shown(GObject *stack, GParamSpec *pspec, gpointer bar) {
	const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
	for (GtkWidget *b = gtk_widget_get_first_child(bar); b; b = gtk_widget_get_next_sibling(b)) {
		if (g_strcmp0(g_object_get_data(G_OBJECT(b), "tab"), name) == 0) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
		}
	}
}

/*
 * Linked buttons. GtkStackSwitcher makes them all as wide as the widest, and
 * together they would ask the whole settings window to be wider.
 */
static GtkWidget *tabs_bar_new(GtkWidget *stack) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bar, "linked");
	gtk_widget_set_margin_start(bar, 32);
	gtk_widget_set_margin_end(bar, 32);
	gtk_widget_set_margin_bottom(bar, 2);
	GtkWidget *first = NULL;
	GListModel *pages = G_LIST_MODEL(gtk_stack_get_pages(GTK_STACK(stack)));
	for (guint i = 0; i < g_list_model_get_n_items(pages); i++) {
		GtkStackPage *page = g_list_model_get_item(pages, i);
		GtkWidget *button = gtk_toggle_button_new_with_label(gtk_stack_page_get_title(page));
		gtk_widget_set_hexpand(button, TRUE);
		g_object_set_data_full(G_OBJECT(button), "tab", g_strdup(gtk_stack_page_get_name(page)), g_free);
		if (first) {
			gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(first));
		} else {
			first = button;
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
		}
		g_signal_connect(button, "toggled", G_CALLBACK(on_tab_toggled), stack);
		gtk_box_append(GTK_BOX(bar), button);
		g_object_unref(page);
	}
	g_object_unref(pages);
	g_signal_connect(stack, "notify::visible-child-name", G_CALLBACK(on_tab_shown), bar);
	return bar;
}

GtkWidget *login_page_new(struct settings *s) {
	struct login_page *p = g_new0(struct login_page, 1);
	static bool cleanup_set;
	if (!cleanup_set) {
		atexit(remove_runtime_files);
		remove_stale_runtime_files();
		cleanup_set = true;
	}
	p->s = s;
	p->controls = g_new0(GtkWidget *, dm_option_count);
	p->placed = g_new0(bool, dm_option_count);
	p->user_rows = g_ptr_array_new();

	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	// on top, and never scrolled away: the picture and whether it is in use
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
	gtk_widget_set_margin_start(head, 32);
	gtk_widget_set_margin_end(head, 32);
	gtk_widget_set_margin_top(head, 20);
	gtk_widget_set_margin_bottom(head, 12);
	p->preview = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->preview), THUMB_W);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->preview), THUMB_H);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->preview), draw_preview, p, NULL);
	gtk_widget_set_overflow(p->preview, GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(p->preview, "tw-thumb");
	gtk_widget_set_valign(p->preview, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(head), p->preview);

	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_hexpand(side, TRUE);
	GtkWidget *title = gtk_label_new("Login screen");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_widget_add_css_class(title, "tw-title");
	gtk_box_append(GTK_BOX(side), title);
	GtkWidget *about = gtk_label_new("Where you log in, in the look of your theme. The picture "
		"follows what you change here.");
	gtk_label_set_xalign(GTK_LABEL(about), 0);
	gtk_label_set_wrap(GTK_LABEL(about), TRUE);
	// a long line must not ask the whole settings window to be wider
	gtk_label_set_max_width_chars(GTK_LABEL(about), 30);
	gtk_widget_add_css_class(about, "dim-label");
	gtk_box_append(GTK_BOX(side), about);

	GtkWidget *service = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_set_margin_top(service, 6);
	p->service_switch = gtk_switch_new();
	gtk_widget_set_valign(p->service_switch, GTK_ALIGN_CENTER);
	g_signal_connect(p->service_switch, "state-set", G_CALLBACK(on_service_switch), p);
	gtk_box_append(GTK_BOX(service), p->service_switch);
	p->service_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->service_label), 0);
	gtk_label_set_wrap(GTK_LABEL(p->service_label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(p->service_label), 26);
	gtk_box_append(GTK_BOX(service), p->service_label);
	gtk_box_append(GTK_BOX(side), service);

	GtkWidget *try_it = gtk_button_new_with_label("Try it on the whole screen");
	gtk_widget_set_tooltip_text(try_it, "Checks no password. Esc ends it.");
	gtk_widget_set_halign(try_it, GTK_ALIGN_START);
	gtk_widget_set_margin_top(try_it, 6);
	g_signal_connect(try_it, "clicked", G_CALLBACK(on_try), p);
	gtk_box_append(GTK_BOX(side), try_it);
	gtk_box_append(GTK_BOX(head), side);
	gtk_box_append(GTK_BOX(page), head);

	// the tabs
	GtkWidget *stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_widget_set_vexpand(stack, TRUE);
	build_look(p, tab_new(stack, "look", "Look"));
	build_login(p, tab_new(stack, "login", "Logging in"));
	build_shown(p, tab_new(stack, "shown", "Shown"));
	build_power(p, tab_new(stack, "power", "Power"));
	build_input(p, tab_new(stack, "input", "Input"));
	build_advanced(p, tab_new(stack, "advanced", "Advanced"));
	gtk_box_append(GTK_BOX(page), tabs_bar_new(stack));
	gtk_box_append(GTK_BOX(page), stack);

	// only there when there is something to save
	p->bar = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(p->bar), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(bar, 32);
	gtk_widget_set_margin_end(bar, 32);
	gtk_widget_set_margin_top(bar, 8);
	gtk_widget_set_margin_bottom(bar, 8);
	p->bar_label = gtk_label_new("You have changes that are not saved");
	gtk_label_set_xalign(GTK_LABEL(p->bar_label), 0);
	gtk_widget_set_hexpand(p->bar_label, TRUE);
	gtk_box_append(GTK_BOX(bar), p->bar_label);
	GtkWidget *discard = gtk_button_new_with_label("Discard");
	g_signal_connect(discard, "clicked", G_CALLBACK(on_discard), p);
	gtk_box_append(GTK_BOX(bar), discard);
	GtkWidget *save = gtk_button_new_with_label("Save");
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), p);
	gtk_box_append(GTK_BOX(bar), save);
	gtk_revealer_set_child(GTK_REVEALER(p->bar), bar);
	gtk_box_append(GTK_BOX(page), p->bar);

	load_values(p);
	run_helper(p, "status", NULL, NULL, service_read, false);
	return page;
}
