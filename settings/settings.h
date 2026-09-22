#ifndef _TW_SETTINGS_H
#define _TW_SETTINGS_H
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>
#include "confdoc.h"
#include "list.h"

struct tw_desktop_entry;
struct theme_page;
struct wallpaper_page;
struct taskbar_page;
struct menus_page;
struct launcher_page;
struct keyboard_page;
struct mouse_page;
struct screen_page;
struct sound_page;
struct bluetooth_page;
struct apps_page;
struct account_page;
struct animations_page;
struct window_page;
struct datetime_page;

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
	struct about_page *about_page;
	struct launcher_page *launcher_page;
	struct keyboard_page *keyboard_page;
	struct mouse_page *mouse_page;
	struct screen_page *screen_page;
	struct sound_page *sound_page;
	struct bluetooth_page *bluetooth_page;
	struct apps_page *apps_page;
	struct account_page *account_page;
	struct animations_page *animations_page;
	struct window_page *window_page;
	struct datetime_page *datetime_page;
	GtkWidget *sidebar, *search, *results, *results_scroll;
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
/* A setting found by the search of the settings window. */
struct ui_search_entry {
	char *page, *page_title, *group, *title, *subtitle, *keywords;
	GtkWidget *widget; // the row or group, NULL for the page itself or once it is gone
	int score;
};
/* Rows and groups created from now on belong to this page (NULL stops indexing). */
void ui_index_page(const char *name, const char *title, const char *keywords);
/* Entries matching every word of the query, best first. Borrowed, free the array. */
GPtrArray *ui_search(const char *query);
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
/* Every installed app that is meant to be shown, cached. Borrowed. */
list_t *ui_all_apps(void);
const struct tw_desktop_entry *ui_find_app(const char *id);
GtkWidget *ui_app_picker(const char *label, void (*callback)(const char *id, gpointer data),
		gpointer data);
/*
 * A small window to pick an icon: a name of the icon theme or an image file,
 * with a preview. clear_label adds a button that reports no icon (NULL for
 * none). apply is called with the chosen icon, or NULL when it was cleared.
 */
/* Asks for a program: a command to type, or an executable picked from disk. */
void ui_command_dialog(GtkWindow *parent, const char *title, const char *description,
	const char *current, void (*apply)(const char *command, gpointer data),
	gpointer data);

void ui_icon_dialog(GtkWindow *parent, const char *title, const char *description,
		const char *current, const char *fallback, const char *clear_label,
		void (*apply)(const char *icon, gpointer data), gpointer data);
char *ui_display_value(const char *value);
char *ui_input_value(const char *text);

struct app_list {
	GtkWidget *list;
	GPtrArray *ids; // char *
	void (*changed)(struct app_list *list, gpointer data);
	gpointer data;
	guint rebuild_id;
	/* Optional button on every row, e.g. to change an entry's icon. */
	const char *extra_icon, *extra_tooltip;
	void (*extra)(struct app_list *list, guint index, gpointer data);
	/* Optional icon shown for a row instead of the app's own one. */
	const char *(*row_icon)(struct app_list *list, guint index, gpointer data);
};
struct app_list *ui_app_list_new(GtkWidget *content, const char *title,
		const char *description, void (*changed)(struct app_list *, gpointer), gpointer data);
void ui_app_list_set(struct app_list *list, GPtrArray *ids);
/* Redraws the rows, e.g. after an entry's icon changed. */
void ui_app_list_refresh(struct app_list *list);

/* ~/.config/tileWin/wallpapers/<basename>.<ext>, NULL if there is none. */
char *ui_wallpaper_dropin(const char *basename);
void ui_wallpaper_remove_dropins(const char *basename);
/* Renders a `wallpaper` value, or the theme's wallpaper if it is NULL or "theme". */
/* Below the redraw, so a window that is waiting to be painted is painted first;
 * the pages that are not on screen yet come after the pictures of the one that
 * is. */
#define UI_PRIORITY_WALLPAPER (G_PRIORITY_DEFAULT_IDLE)
#define UI_PRIORITY_LAZY_PAGE (G_PRIORITY_DEFAULT_IDLE + 10)

/* Puts the wallpaper of a theme into an empty picture once the window is up. */
void ui_wallpaper_picture_fill(GtkWidget *picture, const char *wallpaper, const char *theme,
	int width, int height);
/* Lets the queue above run: called after the window has been painted once. */
void ui_wallpaper_fills_start(void);

GdkTexture *ui_wallpaper_texture(const char *wallpaper, const char *theme, int width,
		int height);

/* pages */
GtkWidget *theme_page_new(struct settings *s);
void theme_page_refresh(struct settings *s);
GtkWidget *wallpaper_page_new(struct settings *s);
void wallpaper_page_refresh(struct settings *s);
GtkWidget *taskbar_page_new(struct settings *s);
void taskbar_page_refresh(struct settings *s);
/* The right-click menus of the taskbar, put at the end of the Taskbar page. */
void menus_section_attach(struct settings *s, GtkWidget *content);
GtkWidget *startmenu_page_new(struct settings *s);
GtkWidget *about_page_new(struct settings *s);
void menus_page_refresh(struct settings *s);
GtkWidget *launcher_page_new(struct settings *s);
void launcher_page_refresh(struct settings *s);
GtkWidget *keyboard_page_new(struct settings *s);
void keyboard_page_refresh(struct settings *s);
GtkWidget *mouse_page_new(struct settings *s);
void mouse_page_refresh(struct settings *s);
GtkWidget *screen_page_new(struct settings *s);
void screen_page_refresh(struct settings *s);
GtkWidget *sound_page_new(struct settings *s);
void sound_page_refresh(struct settings *s);
GtkWidget *bluetooth_page_new(struct settings *s);
void bluetooth_page_refresh(struct settings *s);
GtkWidget *apps_page_new(struct settings *s);
void apps_page_refresh(struct settings *s);
GtkWidget *account_page_new(struct settings *s);
void account_page_refresh(struct settings *s);
GtkWidget *animations_page_new(struct settings *s);
void animations_page_refresh(struct settings *s);
GtkWidget *window_page_new(struct settings *s);
GtkWidget *datetime_page_new(struct settings *s);
void datetime_page_refresh(struct settings *s);
void window_page_refresh(struct settings *s);

#endif
