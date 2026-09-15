#ifndef _SWAY_TILEWIN_H
#define _SWAY_TILEWIN_H
#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <json.h>
#include <sys/types.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <xkbcommon/xkbcommon.h>
#include "tw_theme.h"

struct sway_config;
struct sway_container;
struct sway_output;
struct sway_seat;
struct sway_view;
struct sway_workspace;
struct wlr_scene_buffer;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_buffer;

#define TW_RESTART_EXIT_CODE 42

enum tw_mode {
	TW_MODE_TILE,
	TW_MODE_WINDOW,
};

enum tw_hit {
	TW_HIT_NONE,
	TW_HIT_CLIENT,
	TW_HIT_TITLE,
	TW_HIT_ICON,
	TW_HIT_MINIMIZE,
	TW_HIT_MAXIMIZE,
	TW_HIT_CLOSE,
	TW_HIT_EDGE,
};

enum tw_snap {
	TW_SNAP_NONE,
	TW_SNAP_LEFT,
	TW_SNAP_RIGHT,
	TW_SNAP_TOP,
	TW_SNAP_TOPLEFT,
	TW_SNAP_TOPRIGHT,
	TW_SNAP_BOTTOMLEFT,
	TW_SNAP_BOTTOMRIGHT,
};

struct tw_insets {
	int top, bottom, left, right;
};

enum tw_deco_strip {
	TW_STRIP_TOP,
	TW_STRIP_BOTTOM,
	TW_STRIP_LEFT,
	TW_STRIP_RIGHT,
	TW_STRIP_COUNT,
};

/* Window-mode data embedded in every sway_container. */
struct tw_container {
	bool auto_floated;          // floated automatically by window mode
	bool has_window_geometry;   // geometry remembered while tiled
	bool window_geometry_maximized;
	enum tw_snap window_geometry_snap;
	struct wlr_box window_geometry;
	enum tw_snap snap;
	struct wlr_box restore_box; // geometry before maximize/snap

	struct wlr_scene_tree *deco_tree;
	struct wlr_scene_buffer *strips[TW_STRIP_COUNT];
	// fills the slot of a snapped or maximized window that is smaller than it
	struct wlr_scene_rect *content_bg;
	int content_bg_width, content_bg_height; // client size the color was sampled at
	enum tw_hit hover, pressed;

	struct {
		bool valid;
		int width, height;
		bool focused, maximized;
		float scale;
		int theme_generation;
	} frame_cache;
	struct {
		bool valid;
		char *title;
		enum tw_hit hover, pressed;
		cairo_surface_t *icon;
	} top_cache;
};

/* Frame description passed to the theme renderers. */
struct tw_frame {
	int width, height; // outer frame size in logical pixels
	bool focused, maximized;
	const char *title;
	cairo_surface_t *icon;
	enum tw_hit hover, pressed;
};

struct tw_buttons {
	struct wlr_box minimize, maximize, close;
};

struct tw_alttab_item {
	const char *title;
	cairo_surface_t *icon;
};

/* mode.c */
extern enum tw_mode tw_mode;
extern struct tw_theme *tw_theme;
extern int tw_theme_generation;

const char *tw_mode_name(enum tw_mode mode);
bool tw_parse_mode(const char *name, enum tw_mode *mode);
bool tw_is_window_mode(void);
void tw_init(const char *mode_override);
void tw_fini(void);
bool tw_request_mode(enum tw_mode mode, char **error);
bool tw_request_theme(const char *name, char **error);
/* Switches between the light and dark variant of the theme and tells apps. */
bool tw_set_color_scheme(bool dark, char **error);
void tw_load_theme_tile_config(struct sway_config *config);
/* Binds the volume, brightness and media keys the config leaves unbound. */
void tw_add_default_bindings(struct sway_config *config);
void tw_after_reload(void);
json_object *tw_describe_state(void);
void tw_wallpaper_update(struct sway_output *output);
void tw_wallpaper_invalidate(void);

/* reload.c */
void reload_config_now(void);

/* ipc-server.c; takes ownership of data (may be NULL) */
void ipc_event_tilewin(const char *change, json_object *data);

/* styles.c */
void tw_style_insets(const struct tw_theme *theme, bool maximized,
		struct tw_insets *insets, int *grab_margin);
void tw_style_buttons(const struct tw_theme *theme, int width, bool maximized,
		struct tw_buttons *buttons);
void tw_style_draw_frame(cairo_t *cairo, const struct tw_theme *theme,
		const struct tw_frame *frame);
void tw_style_alttab_size(const struct tw_theme *theme, int count, int max_width,
		int *width, int *height, int *columns);
void tw_style_draw_alttab(cairo_t *cairo, const struct tw_theme *theme,
		const struct tw_alttab_item *items, int count, int selected,
		int width, int height, int columns);
uint32_t tw_style_snap_color(const struct tw_theme *theme);

/* buffer.c; takes ownership of the surface */
struct wlr_buffer *tw_buffer_from_surface(cairo_surface_t *surface);
void tw_scene_buffer_set_surface(struct wlr_scene_buffer *node,
		cairo_surface_t *surface, int width, int height);

/* icons.c */
cairo_surface_t *tw_icon_for_view(struct sway_view *view, int size);
void tw_icon_cache_clear(void);

/* decoration.c */
void tw_container_update_deco_state(struct sway_container *con);
struct tw_insets tw_deco_insets(bool maximized);
void tw_deco_arrange(struct sway_container *con, int width, int height);
void tw_deco_disable(struct sway_container *con);
void tw_container_destroy(struct sway_container *con);
enum tw_hit tw_deco_hit_test(struct sway_container *con, double lx, double ly,
		enum wlr_edges *edges);
bool tw_handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct sway_container *cont, struct wlr_surface *surface,
		uint32_t button, bool pressed);
bool tw_handle_motion(struct sway_seat *seat, struct sway_container *cont,
		struct wlr_surface *surface);

/* manage.c */
struct wlr_box tw_workarea(struct sway_workspace *ws);
void tw_set_box(struct sway_container *con, const struct wlr_box *box);
void tw_maximize(struct sway_container *con, bool enable);
void tw_minimize(struct sway_container *con, bool enable);
void tw_restore(struct sway_container *con);
bool tw_snap(struct sway_container *con, const char *direction, char **error);
void tw_snap_to(struct sway_container *con, enum tw_snap snap);
/*
 * Snapped and maximized windows keep the size of their slot even when the
 * client commits a smaller size (terminals resize in character cells).
 */
bool tw_container_fills_slot(struct sway_container *con);
/* Positions the content of such a window and fills the rest of the slot. */
void tw_update_content_fill(struct sway_container *con);
void tw_place_new_window(struct sway_container *con);
bool tw_arrange_workspace(struct sway_workspace *ws, const char *how, char **error);
bool tw_show_desktop(struct sway_workspace *ws, char **error);
void tw_convert_to_window_mode(void);
/* Refits maximized and snapped windows after the output's usable area changed. */
void tw_workarea_changed(struct sway_output *output);
void tw_convert_to_tile_mode(void);
struct sway_container *tw_next_focus_candidate(struct sway_seat *seat,
		struct sway_workspace *ws, struct sway_container *exclude);
void tw_view_notify_maximized(struct sway_view *view, bool maximized);
enum tw_snap tw_snap_zone(double lx, double ly);
void tw_snap_preview_update(struct sway_container *con, double lx, double ly);
enum tw_snap tw_snap_preview_finish(void);

/* alttab.c */
bool tw_alttab_active(void);
void tw_alttab_step(struct sway_seat *seat, int direction);
void tw_alttab_commit(void);
void tw_alttab_cancel(void);
void tw_alttab_modifiers(uint32_t modifiers);
bool tw_alttab_handle_key(xkb_keysym_t sym, bool pressed);
void tw_alttab_container_destroyed(struct sway_container *con);

/* taskview.c: Win+Tab task view and desktops */
bool tw_taskview_active(void);
void tw_taskview_open(struct sway_seat *seat);
void tw_taskview_close(void);
void tw_taskview_toggle(struct sway_seat *seat);
void tw_taskview_motion(struct sway_seat *seat);
void tw_taskview_button(struct sway_seat *seat, uint32_t button, bool pressed);
/* Handles a key while the task view is open; true if consumed. */
bool tw_taskview_handle_key(xkb_keysym_t sym, bool pressed, uint32_t modifiers);
void tw_taskview_container_destroyed(struct sway_container *con);
void tw_taskview_workspace_destroyed(struct sway_workspace *ws);
/* Creates a desktop (workspace) that stays when empty. */
struct sway_workspace *tw_desktop_new(struct sway_output *output);
/* Moves the windows of a desktop to its neighbor and removes it. */
bool tw_desktop_close(struct sway_workspace *ws);

/* session.c */
void tw_panel_start(void);
void tw_panel_restart(void);
void tw_panel_stop(void);
void tw_panel_config_reloaded(void);
pid_t tw_panel_pid(void);
bool tw_restart(bool relaunch_apps, char **error);
void tw_session_restore(void);
void tw_session_apply_placement(struct sway_container *con);
/* Windows changed; the session is saved shortly afterwards. */
void tw_session_changed(void);
/* Saves the session now, e.g. when logging out with apps still running. */
void tw_session_save_now(void);
/* Stops saving: a shutdown is terminating the apps together with tileWin. */
void tw_session_freeze(void);
/* SIGTERM: saves once more if no windows are gone yet, then stops saving. */
void tw_session_shutdown(void);
/* Gives D-Bus and systemd services (portals, Thunar) the session environment. */
void tw_session_export_environment(void);

#endif
