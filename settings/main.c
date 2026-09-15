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
	".tw-status { padding: 6px 12px; }\n";

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
		settings_status(s, "Saved the shortcuts and reloaded tileWin");
	} else {
		settings_status(s, "Saved the shortcuts");
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
	screen_page_refresh(s);
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

	gtk_stack_add_titled(s->stack, theme_page_new(s), "theme", "Theme");
	gtk_stack_add_titled(s->stack, wallpaper_page_new(s), "wallpaper", "Wallpaper");
	gtk_stack_add_titled(s->stack, screen_page_new(s), "screen", "Screen");
	gtk_stack_add_titled(s->stack, taskbar_page_new(s), "taskbar", "Taskbar");
	gtk_stack_add_titled(s->stack, menus_page_new(s), "menus", "Menus");
	gtk_stack_add_titled(s->stack, launcher_page_new(s), "launcher", "Launcher & apps");
	gtk_stack_add_titled(s->stack, keyboard_page_new(s), "keyboard", "Keyboard");
	gtk_stack_add_titled(s->stack, mouse_page_new(s), "mouse", "Mouse & touchpad");

	GtkWidget *sidebar = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), s->stack);
	gtk_widget_set_size_request(sidebar, 190, -1);

	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(hbox), sidebar);
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
			gtk_stack_set_visible_child_name(s->stack, page);
		} else {
			g_application_command_line_printerr(cmdline,
				"Unknown page '%s' (theme, wallpaper, taskbar, menus, launcher, keyboard, mouse)\n", page);
		}
	}
	gtk_window_present(s->window);
	return 0;
}

int main(int argc, char **argv) {
	struct settings *s = &settings;
	s->common = confdoc_open("common.conf");
	s->taskbar = confdoc_open("taskbar.conf");
	s->windowmode = confdoc_open("windowmode.conf");
	s->tilemode = confdoc_open("tilemode.conf");

	s->app = gtk_application_new("org.tilewin.Settings", G_APPLICATION_HANDLES_COMMAND_LINE);
	g_application_add_main_option(G_APPLICATION(s->app), "page", 'p', 0, G_OPTION_ARG_STRING,
		"Page to open: theme, wallpaper, taskbar, menus, launcher, keyboard or mouse", "PAGE");
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
