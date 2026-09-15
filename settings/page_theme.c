#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "settings.h"
#include "tw_paths.h"
#include "tw_theme.h"

struct theme_page {
	struct settings *s;
	bool updating;
	GtkWidget *flow;
	GtkWidget *mode_dd;
	GtkWidget *target_dd; // which mode's theme the cards change
	GtkWidget *summary;
	GtkWidget *dark_switch;
	GtkWidget *icons_switch;
};

static const char *target_mode(struct theme_page *p) {
	return gtk_drop_down_get_selected(GTK_DROP_DOWN(p->target_dd)) == 1 ? "tile" : "window";
}

static char *mode_theme(const char *mode) {
	char *key = g_strdup_printf("theme_%s", mode);
	char *name = tw_ipc_state(key);
	g_free(key);
	if (name && *name) {
		return name;
	}
	g_free(name);
	char *saved = tw_theme_mode_name(mode);
	name = g_strdup(saved);
	free(saved);
	return name;
}

static char *theme_title(const char *name) {
	char *error = NULL;
	struct tw_theme *theme = tw_theme_load(name, &error);
	free(error);
	char *title = g_strdup(theme && theme->title ? theme->title : name);
	tw_theme_free(theme);
	return title;
}

static void update_summary(struct theme_page *p, const char *window_theme,
		const char *tile_theme) {
	char *window_title = theme_title(window_theme);
	char *tile_title = theme_title(tile_theme);
	char *text = g_strdup_printf("Window mode uses %s, tile mode uses %s. Switching the "
		"mode also switches to its theme.", window_title, tile_title);
	gtk_label_set_text(GTK_LABEL(p->summary), text);
	g_free(text);
	g_free(window_title);
	g_free(tile_title);
}

/* Selects the card of the theme the chosen mode uses. */
static void select_target_theme(struct theme_page *p) {
	char *window_theme = mode_theme("window");
	char *tile_theme = mode_theme("tile");
	const char *wanted = strcmp(target_mode(p), "tile") == 0 ? tile_theme : window_theme;
	gtk_flow_box_unselect_all(GTK_FLOW_BOX(p->flow));
	for (int i = 0;; i++) {
		GtkFlowBoxChild *child = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(p->flow), i);
		if (!child) {
			break;
		}
		const char *name = g_object_get_data(G_OBJECT(child), "theme");
		if (name && strcmp(name, wanted) == 0) {
			gtk_flow_box_select_child(GTK_FLOW_BOX(p->flow), child);
		}
	}
	update_summary(p, window_theme, tile_theme);
	g_free(window_theme);
	g_free(tile_theme);
}

static gboolean on_dark_switch(GtkSwitch *widget, gboolean active, gpointer data) {
	struct theme_page *p = data;
	if (p->updating) {
		return FALSE;
	}
	const char *scheme = active ? "dark" : "light";
	if (tw_ipc_available()) {
		if (settings_command(p->s, "color_scheme %s", scheme)) {
			settings_status(p->s, "Switched to the %s color scheme", scheme);
		}
	} else {
		tw_color_scheme_save(active);
		const char *argv[] = { "tilewin-color-scheme", scheme, NULL };
		g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
		settings_status(p->s, "Apps use the %s color scheme; tileWin follows when it starts",
			scheme);
	}
	settings_apply_color_scheme(active);
	return FALSE;
}

static char *app_icons_path(void) {
	char *dir = tw_config_dir();
	char *path = dir ? g_build_filename(dir, "app-icons", NULL) : NULL;
	free(dir);
	return path;
}

static bool app_icons_enabled(void) {
	char *path = app_icons_path();
	char *value = path ? tw_read_first_line(path) : NULL;
	bool enabled = !value || strcmp(value, "no") != 0;
	free(value);
	g_free(path);
	return enabled;
}

static gboolean on_icons_switch(GtkSwitch *widget, gboolean active, gpointer data) {
	struct theme_page *p = data;
	if (p->updating) {
		return FALSE;
	}
	char *path = app_icons_path();
	if (!path || !tw_write_string(path, active ? "yes\n" : "no\n")) {
		g_free(path);
		return FALSE;
	}
	g_free(path);
	if (tw_ipc_available()) {
		// a reload runs tilewin-app-icons with the current theme
		settings_command(p->s, "reload");
	} else if (!active) {
		const char *argv[] = { "tilewin-app-icons", "restore", NULL };
		g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
	}
	settings_status(p->s, active ? "Apps use the icons of the theme" :
		"Apps use your own icon theme again");
	return FALSE;
}

static void on_theme_activated(GtkFlowBox *flow, GtkFlowBoxChild *child, gpointer data) {
	struct theme_page *p = data;
	if (p->updating) {
		return;
	}
	const char *name = g_object_get_data(G_OBJECT(child), "theme");
	const char *title = g_object_get_data(G_OBJECT(child), "title");
	const char *mode = target_mode(p);
	if (tw_ipc_available()) {
		if (settings_command(p->s, "theme %s %s", name, mode)) {
			settings_status(p->s, "The %s mode theme is now %s", mode, title);
		}
	} else {
		char *active = settings_current_mode();
		bool ok = tw_theme_save_mode(mode, name);
		if (ok && strcmp(active, mode) == 0) {
			ok = tw_theme_save_current(name);
		}
		g_free(active);
		if (ok) {
			settings_status(p->s, "The %s mode theme is now %s; it applies when tileWin starts",
				mode, title);
		}
	}
	p->updating = true;
	select_target_theme(p);
	p->updating = false;
	wallpaper_page_refresh(p->s);
}

static void on_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct theme_page *p = data;
	if (p->updating) {
		return;
	}
	const char *mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) == 1 ?
		"tile" : "window";
	if (tw_ipc_available()) {
		if (settings_command(p->s, "wm_mode %s", mode)) {
			settings_status(p->s, "Switched to %s mode", mode);
		}
		return;
	}
	char *dir = tw_state_dir();
	char *path = dir ? g_build_filename(dir, "mode", NULL) : NULL;
	char *content = g_strdup_printf("%s\n", mode);
	if (path && tw_write_string(path, content)) {
		settings_status(p->s, "tileWin will start in %s mode", mode);
	}
	g_free(content);
	g_free(path);
	free(dir);
}

static void on_target_changed(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct theme_page *p = data;
	if (p->updating) {
		return;
	}
	p->updating = true;
	select_target_theme(p);
	p->updating = false;
}

void theme_page_refresh(struct settings *s) {
	struct theme_page *p = s->theme_page;
	if (!p) {
		return;
	}
	p->updating = true;
	char *mode = settings_current_mode();
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), strcmp(mode, "tile") == 0);
	g_free(mode);
	char *scheme = tw_ipc_state("color_scheme");
	gtk_switch_set_active(GTK_SWITCH(p->dark_switch),
		scheme ? strcmp(scheme, "dark") == 0 : tw_color_scheme_is_dark());
	g_free(scheme);
	gtk_switch_set_active(GTK_SWITCH(p->icons_switch), app_icons_enabled());

	gtk_flow_box_remove_all(GTK_FLOW_BOX(p->flow));
	list_t *names = tw_theme_list();
	for (int i = 0; names && i < names->length; i++) {
		const char *name = names->items[i];
		char *error = NULL;
		struct tw_theme *theme = tw_theme_load(name, &error);
		free(error);
		const char *title = theme && theme->title ? theme->title : name;

		GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GdkTexture *texture = ui_wallpaper_texture(NULL, name, 240, 150);
		GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
		g_object_unref(texture);
		gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
		gtk_widget_set_size_request(picture, 240, 150);
		gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
		gtk_widget_add_css_class(picture, "tw-thumb");
		gtk_box_append(GTK_BOX(card), picture);
		GtkWidget *label = gtk_label_new(title);
		gtk_widget_add_css_class(label, "tw-heading");
		gtk_box_append(GTK_BOX(card), label);
		GtkWidget *sub = gtk_label_new(name);
		gtk_widget_add_css_class(sub, "dim-label");
		gtk_widget_add_css_class(sub, "tw-caption");
		gtk_box_append(GTK_BOX(card), sub);

		GtkWidget *child = gtk_flow_box_child_new();
		gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), card);
		gtk_widget_add_css_class(child, "tw-card");
		g_object_set_data_full(G_OBJECT(child), "theme", g_strdup(name), g_free);
		g_object_set_data_full(G_OBJECT(child), "title", g_strdup(title), g_free);
		gtk_flow_box_append(GTK_FLOW_BOX(p->flow), child);
		if (theme) {
			tw_theme_free(theme);
		}
	}
	if (names) {
		list_free_items_and_destroy(names);
	}
	select_target_theme(p);
	p->updating = false;
}

GtkWidget *theme_page_new(struct settings *s) {
	struct theme_page *p = g_new0(struct theme_page, 1);
	p->s = s;
	s->theme_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Theme",
		"The theme sets the look of window borders, the taskbar, menus and the default wallpaper.",
		&content);

	GtkWidget *group = ui_group(content, "Window manager", NULL);
	p->mode_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Window mode", "Tile mode", NULL });
	ui_row(group, "Mode",
		"Window mode works like Windows, tile mode like sway. Super+Shift+W also switches.",
		p->mode_dd);
	g_signal_connect(p->mode_dd, "notify::selected", G_CALLBACK(on_mode_changed), p);
	p->dark_switch = gtk_switch_new();
	g_signal_connect(p->dark_switch, "state-set", G_CALLBACK(on_dark_switch), p);
	ui_row(group, "Dark mode",
		"Dark title bars, taskbar menus and flyouts. GTK, GNOME and KDE apps switch too.",
		p->dark_switch);
	p->icons_switch = gtk_switch_new();
	g_signal_connect(p->icons_switch, "state-set", G_CALLBACK(on_icons_switch), p);
	ui_row(group, "Theme icons in apps",
		"File managers like Thunar and Dolphin, file dialogs and other apps use the icons of "
		"the theme, e.g. Windows XP folders. Turned off, they use your own icon theme again.",
		p->icons_switch);

	p->target_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Window mode", "Tile mode", NULL });
	char *active = settings_current_mode();
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->target_dd), strcmp(active, "tile") == 0);
	g_free(active);
	ui_row(group, "Theme for",
		"Window mode and tile mode each have their own theme. Pick a mode, then a theme below.",
		p->target_dd);
	g_signal_connect(p->target_dd, "notify::selected", G_CALLBACK(on_target_changed), p);

	GtkWidget *heading = gtk_label_new("Themes");
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_widget_add_css_class(heading, "tw-heading");
	gtk_widget_set_margin_top(heading, 14);
	gtk_box_append(GTK_BOX(content), heading);
	char *user_dir = tw_config_dir();
	char *hint = g_strdup_printf("Your own themes go into %s/themes/<name>/theme.conf.",
		user_dir ? user_dir : "~/.config/tileWin");
	free(user_dir);
	GtkWidget *hint_label = gtk_label_new(hint);
	g_free(hint);
	gtk_label_set_xalign(GTK_LABEL(hint_label), 0);
	gtk_label_set_wrap(GTK_LABEL(hint_label), TRUE);
	gtk_widget_add_css_class(hint_label, "dim-label");
	gtk_box_append(GTK_BOX(content), hint_label);
	p->summary = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->summary), 0);
	gtk_label_set_wrap(GTK_LABEL(p->summary), TRUE);
	gtk_box_append(GTK_BOX(content), p->summary);

	p->flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(p->flow), GTK_SELECTION_SINGLE);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(p->flow), TRUE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p->flow), TRUE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(p->flow), 4);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(p->flow), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(p->flow), 8);
	gtk_box_append(GTK_BOX(content), p->flow);
	g_signal_connect(p->flow, "child-activated", G_CALLBACK(on_theme_activated), p);

	theme_page_refresh(s);
	return page;
}
