#ifndef _TW_SETTINGS_H
#define _TW_SETTINGS_H
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>
#include "confdoc.h"

struct tw_desktop_entry;
struct theme_page;
struct wallpaper_page;
struct taskbar_page;
struct menus_page;
struct launcher_page;
struct keyboard_page;
struct mouse_page;
struct screen_page;

struct settings {
	GtkApplication *app;
	GtkWindow *window;
	GtkStack *stack;
	GtkLabel *status;
	struct confdoc *common;  // ~/.config/tileWin/common.conf (sway syntax)
	struct confdoc *taskbar; // ~/.config/tileWin/taskbar.conf
	struct confdoc *windowmode, *tilemode; // key bindings
	guint common_timer, taskbar_timer, modes_timer;
	bool reload_after_save;
	GFileMonitor *monitor;
	struct theme_page *theme_page;
	struct wallpaper_page *wallpaper_page;
	struct taskbar_page *taskbar_page;
	struct menus_page *menus_page;
	struct launcher_page *launcher_page;
	struct keyboard_page *keyboard_page;
	struct mouse_page *mouse_page;
	struct screen_page *screen_page;
};

/* ipc.c: talks to the running tileWin, if any */
char *tw_ipc_request(uint32_t type, const char *payload);
bool tw_ipc_available(void);
bool tw_ipc_command(const char *command, char **error);
/* A field of the get_tilewin reply. Newly allocated, NULL if not running. */
char *tw_ipc_state(const char *key);

/* main.c */
void settings_status(struct settings *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
/* Runs a compositor command if tileWin is running and reports failures. */
bool settings_command(struct settings *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
/* Schedule saving the documents; reload also reloads the compositor config. */
void settings_common_changed(struct settings *s, bool reload);
void settings_taskbar_changed(struct settings *s);
/* A mode config (windowmode.conf or tilemode.conf) changed: save and reload. */
void settings_mode_changed(struct settings *s, struct confdoc *doc);
void settings_refresh(struct settings *s);
char *settings_current_theme(void);
char *settings_current_mode(void);
/* Makes the settings window itself light or dark. */
void settings_apply_color_scheme(bool dark);

/* ui.c */
GtkWidget *ui_page(const char *title, const char *description, GtkWidget **content);
/* A heading and a framed list; returns the list. Its parent is the group box. */
GtkWidget *ui_group(GtkWidget *content, const char *title, const char *description);
GtkWidget *ui_row(GtkWidget *group, const char *title, const char *subtitle,
		GtkWidget *control);
GtkWidget *ui_row_box(GtkWidget *row);
GtkWidget *ui_icon_button(const char *icon, const char *tooltip, bool sensitive,
		GCallback callback, gpointer data);
void ui_closure_free(gpointer data, GClosure *closure);
GtkWidget *ui_app_icon(const char *icon, int size);
const struct tw_desktop_entry *ui_find_app(const char *id);
GtkWidget *ui_app_picker(const char *label, void (*callback)(const char *id, gpointer data),
		gpointer data);
char *ui_display_value(const char *value);
char *ui_input_value(const char *text);

struct app_list {
	GtkWidget *list;
	GPtrArray *ids; // char *
	void (*changed)(struct app_list *list, gpointer data);
	gpointer data;
	guint rebuild_id;
};
struct app_list *ui_app_list_new(GtkWidget *content, const char *title,
		const char *description, void (*changed)(struct app_list *, gpointer), gpointer data);
void ui_app_list_set(struct app_list *list, GPtrArray *ids);

/* ~/.config/tileWin/wallpapers/<basename>.<ext>, NULL if there is none. */
char *ui_wallpaper_dropin(const char *basename);
void ui_wallpaper_remove_dropins(const char *basename);
/* Renders a `wallpaper` value, or the theme's wallpaper if it is NULL or "theme". */
GdkTexture *ui_wallpaper_texture(const char *wallpaper, const char *theme, int width,
		int height);

/* pages */
GtkWidget *theme_page_new(struct settings *s);
void theme_page_refresh(struct settings *s);
GtkWidget *wallpaper_page_new(struct settings *s);
void wallpaper_page_refresh(struct settings *s);
GtkWidget *taskbar_page_new(struct settings *s);
void taskbar_page_refresh(struct settings *s);
GtkWidget *menus_page_new(struct settings *s);
void menus_page_refresh(struct settings *s);
GtkWidget *launcher_page_new(struct settings *s);
void launcher_page_refresh(struct settings *s);
GtkWidget *keyboard_page_new(struct settings *s);
void keyboard_page_refresh(struct settings *s);
GtkWidget *mouse_page_new(struct settings *s);
void mouse_page_refresh(struct settings *s);
GtkWidget *screen_page_new(struct settings *s);
void screen_page_refresh(struct settings *s);

#endif
