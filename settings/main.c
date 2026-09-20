#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_paths.h"
#include "tw_theme.h"

static const char css[] =
	".tw-title { font-size: 1.8em; font-weight: 800; }\n"
	".tw-heading { font-weight: bold; }\n"
	".tw-caption { font-size: 0.9em; }\n"
	"list.tw-group { border: 1px solid alpha(currentColor, 0.15); border-radius: 8px; }\n"
	"list.tw-group > row:first-child { border-top-left-radius: 8px; border-top-right-radius: 8px; }\n"
	"list.tw-group > row:last-child { border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }\n"
	".tw-thumb { border-radius: 6px; }\n"
	"flowboxchild.tw-card { padding: 8px; border-radius: 10px; }\n"
	"flowboxchild.tw-card:selected { background: alpha(@theme_selected_bg_color, 0.3); }\n"
	".tw-status { padding: 6px 12px; }\n"
	"image.tw-avatar { border-radius: 9999px; }\n"
	".tw-found { background-color: alpha(@theme_selected_bg_color, 0.28); }\n";

static struct settings settings;

void settings_status(struct settings *s, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *msg = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (s->status) {
		gtk_label_set_text(s->status, msg);
	} else {
		g_printerr("%s\n", msg);
	}
	g_free(msg);
}

bool settings_command(struct settings *s, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *command = g_strdup_vprintf(fmt, args);
	va_end(args);
	char *error = NULL;
	bool ok = tw_ipc_command(command, &error);
	if (!ok && tw_ipc_available()) {
		settings_status(s, "tileWin rejected \"%s\": %s", command, error ? error : "unknown error");
	}
	g_free(error);
	g_free(command);
	return ok;
}

char *settings_current_theme(void) {
	char *name = tw_ipc_state("theme");
	if (name && *name) {
		return name;
	}
	g_free(name);
	char *saved = tw_theme_current_name();
	name = g_strdup(saved ? saved : TW_DEFAULT_THEME);
	free(saved);
	return name;
}

char *settings_current_mode(void) {
	char *mode = tw_ipc_state("mode");
	if (mode && *mode) {
		return mode;
	}
	g_free(mode);
	char *dir = tw_state_dir();
	char *path = dir ? g_build_filename(dir, "mode", NULL) : NULL;
	free(dir);
	char *line = path ? tw_read_first_line(path) : NULL;
	g_free(path);
	mode = g_strdup(line && strcmp(line, "tile") == 0 ? "tile" : "window");
	free(line);
	return mode;
}

void settings_apply_color_scheme(bool dark) {
	GtkSettings *gtk = gtk_settings_get_default();
	if (!gtk) {
		return;
	}
#if GTK_CHECK_VERSION(4, 20, 0)
	g_object_set(gtk, "gtk-interface-color-scheme",
		dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#endif
	g_object_set(gtk, "gtk-application-prefer-dark-theme", dark, NULL);
}

static void copy_default_if_missing(const char *filename) {
	char *dir = tw_config_dir();
	char *path = g_build_filename(dir, filename, NULL);
	free(dir);
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		char *source = g_build_filename(tw_data_dir(), "config", filename, NULL);
		char *contents = NULL;
		if (g_file_get_contents(source, &contents, NULL, NULL)) {
			tw_write_string(path, contents);
		}
		g_free(contents);
		g_free(source);
	}
	g_free(path);
}

static gboolean save_common(gpointer data) {
	struct settings *s = data;
	s->common_timer = 0;
	// the installed mode configs include the installed common.conf, so the
	// user's common.conf is only used together with the user's mode configs
	copy_default_if_missing("tilemode.conf");
	copy_default_if_missing("windowmode.conf");
	char *error = NULL;
	if (!confdoc_save(s->common, &error)) {
		settings_status(s, "%s", error);
		g_free(error);
		return G_SOURCE_REMOVE;
	}
	settings_status(s, "Saved %s", s->common->path);
	if (s->reload_after_save) {
		s->reload_after_save = false;
		if (settings_command(s, "reload")) {
			settings_status(s, "Saved %s and reloaded tileWin", s->common->path);
		}
	}
	return G_SOURCE_REMOVE;
}

static gboolean save_taskbar(gpointer data) {
	struct settings *s = data;
	s->taskbar_timer = 0;
	char *error = NULL;
	if (!confdoc_save(s->taskbar, &error)) {
		settings_status(s, "%s", error);
		g_free(error);
	} else {
		settings_status(s, "Saved %s", s->taskbar->path);
	}
	return G_SOURCE_REMOVE;
}

void settings_common_changed(struct settings *s, bool reload) {
	if (reload) {
		s->reload_after_save = true;
	}
	if (s->common_timer) {
		g_source_remove(s->common_timer);
	}
	s->common_timer = g_timeout_add(400, save_common, s);
}

static gboolean save_modes(gpointer data) {
	struct settings *s = data;
	s->modes_timer = 0;
	char *error = NULL;
	struct confdoc *docs[] = { s->windowmode, s->tilemode };
	for (size_t i = 0; i < G_N_ELEMENTS(docs); i++) {
		if (docs[i]->dirty && !confdoc_save(docs[i], &error)) {
			settings_status(s, "%s", error);
			g_free(error);
			return G_SOURCE_REMOVE;
		}
		docs[i]->dirty = false;
	}
	copy_default_if_missing("common.conf");
	if (settings_command(s, "reload")) {
		settings_status(s, "Saved and reloaded tileWin");
	} else {
		settings_status(s, "Saved");
	}
	return G_SOURCE_REMOVE;
}

void settings_mode_changed(struct settings *s, struct confdoc *doc) {
	doc->dirty = true;
	if (s->modes_timer) {
		g_source_remove(s->modes_timer);
	}
	s->modes_timer = g_timeout_add(900, save_modes, s);
}

void settings_taskbar_changed(struct settings *s) {
	if (s->taskbar_timer) {
		g_source_remove(s->taskbar_timer);
	}
	s->taskbar_timer = g_timeout_add(400, save_taskbar, s);
}

void settings_refresh(struct settings *s) {
	theme_page_refresh(s);
	wallpaper_page_refresh(s);
	taskbar_page_refresh(s);
	menus_page_refresh(s);
	launcher_page_refresh(s);
	keyboard_page_refresh(s);
	mouse_page_refresh(s);
	animations_page_refresh(s);
	window_page_refresh(s);
	screen_page_refresh(s);
	sound_page_refresh(s);
	datetime_page_refresh(s);
	bluetooth_page_refresh(s);
	apps_page_refresh(s);
	account_page_refresh(s);
}

static void flush_saves(struct settings *s) {
	if (s->common_timer) {
		g_source_remove(s->common_timer);
		save_common(s);
	}
	if (s->taskbar_timer) {
		g_source_remove(s->taskbar_timer);
		save_taskbar(s);
	}
	if (s->modes_timer) {
		g_source_remove(s->modes_timer);
		save_modes(s);
	}
}

static bool same_path(GFile *file, const char *path) {
	if (!file) {
		return false;
	}
	char *p = g_file_get_path(file);
	bool same = g_strcmp0(p, path) == 0;
	g_free(p);
	return same;
}

static void check_external_change(struct settings *s, struct confdoc *doc, guint pending) {
	if (pending) {
		return; // our unsaved edits win
	}
	char *contents = NULL;
	if (g_file_get_contents(doc->path, &contents, NULL, NULL) &&
			strcmp(contents, doc->text->str) != 0) {
		confdoc_set_text(doc, contents);
		settings_refresh(s);
		settings_status(s, "%s changed on disk and was reloaded", doc->path);
	}
	g_free(contents);
}

static void on_config_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other,
		GFileMonitorEvent event, gpointer data) {
	struct settings *s = data;
	if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
			event != G_FILE_MONITOR_EVENT_CREATED &&
			event != G_FILE_MONITOR_EVENT_RENAMED &&
			event != G_FILE_MONITOR_EVENT_MOVED_IN) {
		return;
	}
	if (same_path(file, s->common->path) || same_path(other, s->common->path)) {
		check_external_change(s, s->common, s->common_timer);
	}
	if (same_path(file, s->taskbar->path) || same_path(other, s->taskbar->path)) {
		check_external_change(s, s->taskbar, s->taskbar_timer);
	}
	if (same_path(file, s->windowmode->path) || same_path(other, s->windowmode->path)) {
		check_external_change(s, s->windowmode, s->modes_timer);
	}
	if (same_path(file, s->tilemode->path) || same_path(other, s->tilemode->path)) {
		check_external_change(s, s->tilemode, s->modes_timer);
	}
}

/* ---------- search ---------- */

static gboolean unmark_found(gpointer data) {
	GtkWidget **widget = data;
	if (*widget) {
		gtk_widget_remove_css_class(*widget, "tw-found");
		g_object_remove_weak_pointer(G_OBJECT(*widget), (gpointer *)widget);
	}
	g_free(widget);
	return G_SOURCE_REMOVE;
}

static gboolean show_found(gpointer data) {
	GtkWidget **widget = data;
	if (!*widget) {
		g_free(widget);
		return G_SOURCE_REMOVE;
	}
	if (!gtk_widget_grab_focus(*widget)) {
		gtk_widget_child_focus(*widget, GTK_DIR_TAB_FORWARD);
	}
	gtk_widget_add_css_class(*widget, "tw-found");
	g_timeout_add(1600, unmark_found, widget);
	return G_SOURCE_REMOVE;
}

/* ---------- pages are built when they are needed ---------- */

struct lazy_page {
	struct settings *s;
	const char *name, *title, *keywords;
	GtkWidget *(*create)(struct settings *s);
	GtkWidget *holder;
	bool built;
};

static GPtrArray *lazy_pages;

static void lazy_build(struct lazy_page *lp) {
	if (lp->built) {
		return;
	}
	lp->built = true;
	ui_index_page(lp->name, lp->title, lp->keywords);
	GtkWidget *page = lp->create(lp->s);
	// the stack used to hand the page the whole area by itself; inside the
	// box that holds its place it has to ask for the room
	gtk_widget_set_hexpand(page, TRUE);
	gtk_widget_set_vexpand(page, TRUE);
	gtk_box_append(GTK_BOX(lp->holder), page);
	ui_index_page(NULL, NULL, NULL);
}

static void lazy_build_named(const char *name) {
	for (guint i = 0; lazy_pages && name && i < lazy_pages->len; i++) {
		struct lazy_page *lp = lazy_pages->pdata[i];
		if (strcmp(lp->name, name) == 0) {
			lazy_build(lp);
			return;
		}
	}
}

/* One page per turn of the loop, so the window is up and usable meanwhile. */
static gboolean lazy_build_next(gpointer data) {
	for (guint i = 0; lazy_pages && i < lazy_pages->len; i++) {
		struct lazy_page *lp = lazy_pages->pdata[i];
		if (!lp->built) {
			lazy_build(lp);
			return lazy_pages->len > i + 1 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
		}
	}
	return G_SOURCE_REMOVE;
}

static void on_visible_page(GObject *stack, GParamSpec *pspec, gpointer data) {
	lazy_build_named(gtk_stack_get_visible_child_name(GTK_STACK(stack)));
}

/* Everything has to exist before the search can look through it. */
static void lazy_build_all(void) {
	for (guint i = 0; lazy_pages && i < lazy_pages->len; i++) {
		lazy_build(lazy_pages->pdata[i]);
	}
}

static void show_entry(struct settings *s, struct ui_search_entry *e) {
	lazy_build_named(e->page);
	gtk_stack_set_visible_child_name(s->stack, e->page);
	if (e->widget) {
		GtkWidget **widget = g_new(GtkWidget *, 1);
		*widget = e->widget;
		g_object_add_weak_pointer(G_OBJECT(*widget), (gpointer *)widget);
		// after the page is shown, so it can scroll to the row
		g_timeout_add(80, show_found, widget);
	}
}

static void on_result_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct settings *s = data;
	struct ui_search_entry *e = g_object_get_data(G_OBJECT(row), "entry");
	if (e) {
		show_entry(s, e);
	}
}

static void on_search_activate(GtkSearchEntry *entry, gpointer data) {
	struct settings *s = data;
	GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->results), 0);
	if (first) {
		on_result_activated(GTK_LIST_BOX(s->results), first, s);
	}
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
	struct settings *s = data;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
	bool active = text && *text;
	gtk_widget_set_visible(s->sidebar, !active);
	gtk_widget_set_visible(s->results_scroll, active);
	gtk_list_box_remove_all(GTK_LIST_BOX(s->results));
	if (!active) {
		return;
	}
	lazy_build_all(); // the search looks through every page
	GPtrArray *hits = ui_search(text);
	for (guint i = 0; i < hits->len && i < 60; i++) {
		struct ui_search_entry *e = hits->pdata[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		GtkWidget *title = gtk_label_new(e->title);
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(box), title);
		char *where = e->widget && e->group && strcmp(e->group, e->title) != 0 ?
			g_strdup_printf("%s › %s", e->page_title, e->group) : g_strdup(e->page_title);
		GtkWidget *caption = gtk_label_new(where);
		g_free(where);
		gtk_label_set_xalign(GTK_LABEL(caption), 0);
		gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class(caption, "dim-label");
		gtk_widget_add_css_class(caption, "tw-caption");
		gtk_box_append(GTK_BOX(box), caption);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		g_object_set_data(G_OBJECT(row), "entry", e);
		gtk_list_box_append(GTK_LIST_BOX(s->results), row);
	}
	if (hits->len == 0) {
		GtkWidget *none = gtk_label_new("No settings found");
		gtk_widget_add_css_class(none, "dim-label");
		gtk_widget_set_margin_top(none, 12);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), none);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(s->results), row);
	}
	g_ptr_array_free(hits, TRUE);
}

static gboolean on_find_shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
	struct settings *s = data;
	gtk_widget_grab_focus(s->search);
	return TRUE;
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
	flush_saves(data);
	return FALSE;
}

static void build_window(struct settings *s) {
	GtkWidget *window = gtk_application_window_new(s->app);
	s->window = GTK_WINDOW(window);
	gtk_window_set_title(s->window, "tileWin Settings");
	gtk_window_set_default_size(s->window, 1000, 740);
	gtk_window_set_icon_name(s->window, "preferences-desktop");

	GtkWidget *stack = gtk_stack_new();
	s->stack = GTK_STACK(stack);
	gtk_stack_set_transition_type(s->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_widget_set_hexpand(stack, TRUE);
	gtk_widget_set_vexpand(stack, TRUE);

	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(right), stack);
	gtk_box_append(GTK_BOX(right), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	GtkWidget *status = gtk_label_new("");
	s->status = GTK_LABEL(status);
	gtk_label_set_xalign(s->status, 0);
	gtk_label_set_ellipsize(s->status, PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(status, "dim-label");
	gtk_widget_add_css_class(status, "tw-status");
	gtk_box_append(GTK_BOX(right), status);

	static const struct {
		const char *name, *title, *keywords;
		GtkWidget *(*create)(struct settings *s);
	} pages[] = {
		{ "theme", "Theme", "appearance look style dark light colors mode", theme_page_new },
		{ "wallpaper", "Wallpaper", "background desktop picture", wallpaper_page_new },
		{ "animations", "Animations", "effects motion speed fade zoom slide minimize maximize "
			"open close desktop switch", animations_page_new },
		{ "windows", "Window behavior", "snap stick drag move together group modifier stretch "
			"double click focus follows mouse attention activation", window_page_new },
		{ "screen", "Screen", "display monitor resolution refresh scale rotation brightness "
			"night light sleep lock lid power", screen_page_new },
		{ "sound", "Sound", "volume audio speakers headphones microphone mute", sound_page_new },
		{ "datetime", "Date & time", "clock calendar time zone timezone ntp hour format "
			"12 24 seconds automatic", datetime_page_new },
		{ "bluetooth", "Bluetooth", "headphones mouse keyboard pair devices wireless",
			bluetooth_page_new },
		{ "taskbar", "Taskbar", "panel bar widgets tray clock notifications", taskbar_page_new },
		{ "menus", "Menus", "start menu right click context pinned", menus_page_new },
		{ "launcher", "Launcher & apps", "run search applications", launcher_page_new },
		{ "keyboard", "Keyboard", "shortcuts keys bindings layout hotkeys", keyboard_page_new },
		{ "mouse", "Mouse & touchpad", "pointer cursor touchpad scrolling tap", mouse_page_new },
		{ "apps", "Apps", "default browser email startup autostart programs", apps_page_new },
		{ "account", "Account", "user picture photo avatar profile name", account_page_new },
	};
	lazy_pages = g_ptr_array_new_with_free_func(g_free);
	for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
		struct lazy_page *lp = g_new0(struct lazy_page, 1);
		lp->s = s;
		lp->name = pages[i].name;
		lp->title = pages[i].title;
		lp->keywords = pages[i].keywords;
		lp->create = pages[i].create;
		lp->holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_vexpand(lp->holder, TRUE);
		gtk_stack_add_titled(s->stack, lp->holder, pages[i].name, pages[i].title);
		g_ptr_array_add(lazy_pages, lp);
	}
	// the one that is shown first, then the rest while the window is already up
	lazy_build(lazy_pages->pdata[0]);
	g_signal_connect(s->stack, "notify::visible-child-name", G_CALLBACK(on_visible_page), s);
	g_idle_add(lazy_build_next, NULL);

	GtkWidget *sidebar = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), s->stack);
	gtk_widget_set_size_request(sidebar, 190, -1);
	gtk_widget_set_vexpand(sidebar, TRUE);
	s->sidebar = sidebar;

	// search across all settings: typing anywhere in the window starts it
	GtkWidget *search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Find a setting");
	gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(search), window);
	gtk_widget_set_margin_start(search, 8);
	gtk_widget_set_margin_end(search, 8);
	gtk_widget_set_margin_top(search, 8);
	gtk_widget_set_margin_bottom(search, 4);
	g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), s);
	g_signal_connect(search, "activate", G_CALLBACK(on_search_activate), s);
	s->search = search;
	s->results = gtk_list_box_new();
	gtk_widget_add_css_class(s->results, "navigation-sidebar");
	g_signal_connect(s->results, "row-activated", G_CALLBACK(on_result_activated), s);
	s->results_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->results_scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s->results_scroll), s->results);
	gtk_widget_set_vexpand(s->results_scroll, TRUE);
	gtk_widget_set_size_request(s->results_scroll, 190, -1);
	gtk_widget_set_visible(s->results_scroll, FALSE);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(left), search);
	gtk_box_append(GTK_BOX(left), sidebar);
	gtk_box_append(GTK_BOX(left), s->results_scroll);
	GtkEventController *keys = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(
		gtk_shortcut_trigger_parse_string("<Control>f"),
		gtk_callback_action_new(on_find_shortcut, s, NULL)));
	gtk_widget_add_controller(window, keys);

	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(hbox), left);
	gtk_box_append(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_box_append(GTK_BOX(hbox), right);
	gtk_window_set_child(s->window, hbox);
	g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), s);

	if (s->common->error) {
		settings_status(s, "common.conf has a syntax error (%s); please check it by hand",
			s->common->error);
	} else if (s->taskbar->error) {
		settings_status(s, "taskbar.conf has a syntax error (%s); please check it by hand",
			s->taskbar->error);
	} else if (!tw_ipc_available()) {
		settings_status(s, "tileWin is not running: changes are saved and apply on the next start");
	}
}

static void on_startup(GApplication *app, gpointer data) {
	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
	settings_apply_color_scheme(tw_color_scheme_is_dark());
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cmdline, gpointer data) {
	struct settings *s = data;
	if (!s->window) {
		build_window(s);
	}
	GVariantDict *options = g_application_command_line_get_options_dict(cmdline);
	const char *page = NULL;
	if (g_variant_dict_lookup(options, "page", "&s", &page)) {
		if (gtk_stack_get_child_by_name(s->stack, page)) {
			lazy_build_named(page);
			gtk_stack_set_visible_child_name(s->stack, page);
		} else {
			g_application_command_line_printerr(cmdline,
				"Unknown page '%s' (theme, wallpaper, animations, windows, screen, sound, datetime, bluetooth, taskbar, menus, launcher, keyboard, mouse, apps, account)\n", page);
		}
	}
	gtk_window_present(s->window);
	return 0;
}

/* ---------- --pick-file: a file chooser for the taskbar's Run dialog ---------- */

struct pick_request {
	GMainLoop *loop;
	char *path;
};

static void on_pick_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct pick_request *r = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (file) {
		r->path = g_file_get_path(file);
		g_object_unref(file);
	}
	g_main_loop_quit(r->loop);
}

/*
 * The panel has no toolkit of its own, so it asks us for a file and reads the
 * path from our output. Runs without the application, so it never hands over
 * to a settings window that is already open.
 */
static int pick_file(const char *title) {
	gtk_init();
	struct pick_request r = { g_main_loop_new(NULL, FALSE), NULL };
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, title && *title ? title : "Pick a file");
	gtk_file_dialog_open(dialog, NULL, NULL, on_pick_finished, &r);
	g_object_unref(dialog);
	g_main_loop_run(r.loop);
	g_main_loop_unref(r.loop);
	if (!r.path) {
		return 1;
	}
	printf("%s\n", r.path);
	g_free(r.path);
	return 0;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pick-file") == 0) {
			return pick_file(i + 1 < argc ? argv[i + 1] : NULL);
		}
	}
	struct settings *s = &settings;
	s->common = confdoc_open("common.conf");
	s->taskbar = confdoc_open("taskbar.conf");
	s->windowmode = confdoc_open("windowmode.conf");
	s->tilemode = confdoc_open("tilemode.conf");

	s->app = gtk_application_new("org.tilewin.Settings", G_APPLICATION_HANDLES_COMMAND_LINE);
	g_application_add_main_option(G_APPLICATION(s->app), "page", 'p', 0, G_OPTION_ARG_STRING,
		"Page to open: theme, wallpaper, animations, windows, screen, sound, bluetooth, taskbar, menus, launcher, keyboard, mouse, apps or account", "PAGE");
	g_signal_connect(s->app, "startup", G_CALLBACK(on_startup), s);
	g_signal_connect(s->app, "command-line", G_CALLBACK(on_command_line), s);

	char *dir = tw_config_dir();
	if (dir) {
		tw_mkdir_p(dir);
		GFile *file = g_file_new_for_path(dir);
		s->monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
		if (s->monitor) {
			g_signal_connect(s->monitor, "changed", G_CALLBACK(on_config_dir_changed), s);
		}
		g_object_unref(file);
		free(dir);
	}

	int status = g_application_run(G_APPLICATION(s->app), argc, argv);
	flush_saves(s);
	g_clear_object(&s->monitor);
	g_object_unref(s->app);
	confdoc_free(s->common);
	confdoc_free(s->taskbar);
	confdoc_free(s->windowmode);
	confdoc_free(s->tilemode);
	return status;
}
