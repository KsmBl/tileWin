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
#include "list.h"
#include "tw_theme.h"

struct sway_config;
struct sway_container;
struct sway_cursor;
struct sway_output;
struct sway_seat;
struct sway_view;
struct sway_workspace;
struct wlr_scene_buffer;
struct wlr_scene_node;
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

/* The animations that can be turned off or styled one by one. */
enum tw_anim_kind {
	TW_ANIM_OPEN,
	TW_ANIM_CLOSE,
	TW_ANIM_MINIMIZE,
	TW_ANIM_MAXIMIZE, // also snapping and stretching
	TW_ANIM_DESKTOP,
	TW_ANIM_KIND_COUNT,
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
	bool above; // kept over the other floating windows
	struct wlr_box restore_box; // geometry before maximize/snap
	// double-clicking a frame side: the size before, and after stretching
	int expand_axis; // 0: none, 1: width, 2: height
	struct wlr_box expand_restore, expanded;

	struct wlr_scene_tree *deco_tree;
	struct wlr_scene_buffer *strips[TW_STRIP_COUNT];
	// fills the slot of a snapped or maximized window that is smaller than it
	struct wlr_scene_rect *content_bg;
	int content_bg_width, content_bg_height; // client size the color was sampled at
	bool content_translucent; // the client is see-through: it gets no fill
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
	// the screen the window was on before that screen went away, so it can
	// go back there when the screen returns (screens.c)
	struct {
		char *output, *id; // name, and "make model serial"
		struct wlr_box box, restore; // from the top left corner of that screen
		int width, height; // of that screen
		bool maximized;
		enum tw_snap snap;
	} home;

	struct {
		bool active; // fading in after opening
		bool hidden; // a copy of the window is animated instead
		float alpha;
		double dy;
	} anim;
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
/* Sets the theme of a mode; applied now if it is the active mode. */
bool tw_set_mode_theme(enum tw_mode mode, const char *name, char **error);
/* Switches between the light and dark variant of the theme and tells apps. */
bool tw_set_color_scheme(bool dark, char **error);
void tw_load_theme_tile_config(struct sway_config *config);
/* Binds the volume, brightness and media keys the config leaves unbound. */
void tw_add_default_bindings(struct sway_config *config);
/* Points $taskmanager at a task manager that is actually installed. */
void tw_fix_task_manager(struct sway_config *config);
void tw_after_reload(void);
/* Tells the taskbar the new "double_click_time" so it uses it too. */
void tw_double_click_time_changed(void);
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
/* "alttab { style flip3d }": fly through the windows as a 3D stack. */
bool tw_style_alttab_flip(const struct tw_theme *theme);
uint32_t tw_style_alttab_wash(const struct tw_theme *theme);
void tw_style_draw_alttab_title(cairo_t *cairo, const struct tw_theme *theme,
		const char *title, int width, int height);
uint32_t tw_style_snap_color(const struct tw_theme *theme);

/* buffer.c; takes ownership of the surface */
struct wlr_buffer *tw_buffer_from_surface(cairo_surface_t *surface);
void tw_scene_buffer_set_surface(struct wlr_scene_buffer *node,
		cairo_surface_t *surface, int width, int height);
/*
 * Copies the buffers of a window into parent, scaled, with its content origin
 * at x, y. Different scales for the axes squeeze the copy, which is how the
 * Alt+Tab stack makes a window look tilted.
 */
void tw_snapshot_view(struct wlr_scene_tree *parent, struct sway_view *view,
		double scale_x, double scale_y, double x, double y);
/* The same for any scene node, e.g. the wallpaper. */
void tw_snapshot_tree(struct wlr_scene_tree *parent, struct wlr_scene_node *source,
		double scale, double x, double y);

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
/* Moves and shrinks box so it lies inside area. */
struct wlr_box tw_fit_box(struct wlr_box box, struct wlr_box area);
/* The part of area a window snapped that way takes. */
struct wlr_box tw_snap_box(struct wlr_box area, enum tw_snap snap);
void tw_set_box(struct sway_container *con, const struct wlr_box *box);
void tw_maximize(struct sway_container *con, bool enable);
void tw_minimize(struct sway_container *con, bool enable);
void tw_restore(struct sway_container *con);
bool tw_snap(struct sway_container *con, const char *direction, char **error);
void tw_snap_to(struct sway_container *con, enum tw_snap snap);
/* Ends the snapped state of a window that was moved away, keeping its size. */
void tw_unsnap_in_place(struct sway_container *con);
/* Puts the windows marked "always on top" back over the other floating ones. */
void tw_raise_above_windows(struct sway_workspace *ws);
/*
 * Snapped and maximized windows keep the size of their slot even when the
 * client commits a smaller size (terminals resize in character cells).
 */
bool tw_container_fills_slot(struct sway_container *con);
/* Resizes a floating window to its natural size without moving it away. */
void tw_floating_resize_in_place(struct sway_container *con);
/* Positions the content of such a window and fills the rest of the slot. */
void tw_update_content_fill(struct sway_container *con);
void tw_place_new_window(struct sway_container *con);
bool tw_arrange_workspace(struct sway_workspace *ws, const char *how, char **error);
bool tw_show_desktop(struct sway_workspace *ws, char **error);
void tw_convert_to_window_mode(void);
/* Refits maximized and snapped windows after the output's usable area changed. */
void tw_workarea_changed(struct sway_output *output);
void tw_convert_to_tile_mode(void);
/* Gives every open window the border style of the current mode's config. */
void tw_reset_borders(void);
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
/* Moves a desktop one place left (-1) or right (1) in the desktop order. */
bool tw_desktop_move(struct sway_workspace *ws, int direction);
/* The name given to a desktop, or NULL when it only has its number. */
const char *tw_desktop_label(struct sway_workspace *ws);
/* Gives a desktop a name, or takes it away again with NULL or "". */
bool tw_desktop_rename(struct sway_workspace *ws, const char *label);

/* screens.c: several screens in window mode */
/*
 * A floating window went from the workspace box old to new (another screen, or
 * its screen changed): maximized and snapped windows fill the new slot, others
 * are kept on the screen, and the size to restore to comes along.
 */
void tw_floating_screen_changed(struct sway_container *con, const struct wlr_box *old,
		const struct wlr_box *new);
/* Moves a window to the desktop shown on another screen, keeping its state. */
void tw_move_to_screen(struct sway_container *con, struct sway_output *output);
/* Win+Left/Right on a window snapped at an edge next to another screen. */
bool tw_snap_across(struct sway_container *con, int direction);
/* Around handing the workspaces of a screen that goes away to other screens. */
void tw_screen_leaving(struct sway_output *output);
void tw_screen_left(struct sway_output *output);
/* A screen came (back): the windows that lived there return to it. */
void tw_screen_added(struct sway_output *output);
/* The user put the window somewhere: it no longer goes back to an old screen. */
void tw_forget_home(struct sway_container *con);
/* The main display ("main_output"): desktop icons and the main taskbar. */
struct sway_output *tw_main_output(void);
void tw_main_output_changed(void);

/* pointer.c: effects around the mouse pointer */
/* A key of the keyboard; tapping Ctrl on its own shows where the pointer is. */
void tw_pointer_key(xkb_keysym_t sym, bool pressed);
/* A mouse button was pressed, so Ctrl is part of a click and not a tap. */
void tw_pointer_cancel_tap(void);
/* The pointer moved; it leaves copies of itself behind ("pointer_trail"). */
void tw_pointer_moved(struct sway_cursor *cursor);
void tw_pointer_trail_changed(void);
void tw_pointer_fini(void);

/* session.c */
void tw_panel_start(void);
/* Starts tilewin-nightlight, which keeps the night light colors. */
void tw_nightlight_start(void);
/* animate.c: window and desktop animations ("animations", "animation_speed") */
void tw_animate_open(struct sway_container *con);
void tw_animate_close(struct sway_container *con);
void tw_animate_minimize(struct sway_container *con, bool minimize);
/* After the pending geometry of a floating window changed (maximize, snap). */
void tw_animate_resize(struct sway_container *con);
/* Sets a window sliding on after it was let go of while still moving. */
void tw_animate_glide(struct sway_container *con, double vx, double vy);
/* Before switching to the workspace: slides the old one out and the new one in. */
void tw_animate_workspace_switch(struct sway_workspace *ws);
float tw_animate_container_alpha(struct sway_container *con);
double tw_animate_container_dy(struct sway_container *con);
int tw_animate_workspace_dx(struct sway_workspace *ws);
int tw_animate_workspace_dy(struct sway_workspace *ws);
bool tw_animate_hides(struct sway_container *con);
void tw_animate_container_destroyed(struct sway_container *con);
void tw_animate_workspace_destroyed(struct sway_workspace *ws);
/* Before the compositor shuts down, while the scene still exists. */
/* Keeps running animations above the windows; called before each repaint. */
void tw_animate_raise(void);
void tw_animate_shutdown(void);
void tw_animate_fini(void);
bool tw_animation_parse_kind(const char *name, int *kind);
/* Index of a style of that kind of animation, -1 if there is none of that name. */
int tw_animation_parse_style(int kind, const char *name);
const char *const *tw_animation_styles(int kind);

/* explode.c */
struct tw_explosion;
/* Starts making the fire images in a thread, if not done yet. */
void tw_explosion_prepare(void);
/* A window blowing up, NULL if the images are not ready yet. */
struct tw_explosion *tw_explosion_create(struct sway_container *con,
		struct wlr_scene_tree *parent);
/* Shows it ms into the explosion and adds the screen shake; false once it is over. */
bool tw_explosion_update(struct tw_explosion *explosion, double ms, double *shake_x,
		double *shake_y);
void tw_explosion_raise(struct tw_explosion *explosion);
void tw_explosion_destroy(struct tw_explosion *explosion);
void tw_explosion_release(void);
/* Runs the XDG autostart entries (once, when tileWin starts). */
void tw_xdg_autostart(void);

/* stick.c */
void tw_stick_move(struct sway_container *con, list_t *exclude, double *x, double *y);
void tw_stick_resize(struct sway_container *con, enum wlr_edges edges,
		const struct wlr_box *ref, double *grow_width, double *grow_height);
list_t *tw_stick_group(struct sway_container *con);
bool tw_stick_group_modifier_held(struct sway_seat *seat);
bool tw_expand(struct sway_container *con, enum wlr_edges edge);
void tw_panel_restart(void);
void tw_panel_stop(void);
void tw_panel_config_reloaded(void);
/* Sends "panel <args>" to the taskbar, e.g. "close" to close its menus. */
void tw_panel_command(const char *args);
pid_t tw_panel_pid(void);
/*
 * Restarts tileWin. Unless the compositor binary itself has changed (or
 * force_exec asks for it), this keeps the session and every window open and
 * only starts the taskbar and the config afresh.
 */
bool tw_restart(bool relaunch_apps, bool force_exec, char **error);
/* Remembers the file tileWin was started from, to notice a new one later. */
void tw_record_binary(void);
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

/* power.c: idle timeouts, the laptop lid, the power key and locking before sleep */
enum tw_idle_stage {
	TW_IDLE_DIM,
	TW_IDLE_SCREEN_OFF,
	TW_IDLE_LOCK,
	TW_IDLE_SLEEP,
	TW_IDLE_SCREENSAVER, // tilewin-screensaver until input comes
	TW_IDLE_STAGES,
};
/* What closing the lid or pressing the power key does. */
enum tw_power_action {
	TW_POWER_DEFAULT, // leave it to logind
	TW_POWER_NOTHING,
	TW_POWER_SLEEP, // stand by: suspend to RAM
	TW_POWER_HIBERNATE, // suspend to disk
	TW_POWER_HYBRID_SLEEP, // both: stand by, and wake from disk if the power ran out
	TW_POWER_LOCK,
	TW_POWER_SCREEN_OFF,
	TW_POWER_SHUTDOWN,
};
bool tw_idle_stage_parse(const char *name, enum tw_idle_stage *stage);
bool tw_power_action_parse(const char *name, enum tw_power_action *action);
/* Input happened (cheap, called for every event). */
void tw_power_activity(void);
/* An app keeps the screen on (idle inhibitor) or stopped doing so. */
void tw_power_set_inhibited(bool inhibited);
void tw_power_lid(bool closed);
/* The power key went down or up; true when tileWin handles it and the key goes no further. */
bool tw_power_key(bool pressed);
/* The config was loaded or an idle_timeout, lid_action, power_key_action or
 * lock_on_sleep command ran. */
void tw_power_config_changed(void);
/* The screen saver now ("screensaver start") or no more. */
void tw_screensaver_start(void);
void tw_screensaver_stop(void);
void tw_power_fini(void);

#endif
