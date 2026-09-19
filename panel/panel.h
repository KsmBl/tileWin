#ifndef _TILEWIN_PANEL_H
#define _TILEWIN_PANEL_H
#include <cairo.h>
#include <json.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "cursor-shape-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "list.h"
#include "loop.h"
#include "pool-buffer.h"
#include "tw_theme.h"
#include "twconf.h"
#include "viewporter-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct panel;
struct panel_output;
struct psurface;
struct widget;
struct popup;
struct tw_desktop_entry;

struct pbox {
	int x, y, width, height;
};

static inline bool pbox_contains(const struct pbox *b, double x, double y) {
	return x >= b->x && y >= b->y && x < b->x + b->width && y < b->y + b->height;
}

/* ---------- surfaces ---------- */

struct hotspot {
	struct pbox box;
	struct widget *widget;
	int kind;
	int64_t id;
	char *str;
};

struct psurface_impl {
	void (*render)(struct psurface *s, cairo_t *cairo);
	void (*pointer_motion)(struct psurface *s, double x, double y);
	void (*pointer_leave)(struct psurface *s);
	void (*pointer_button)(struct psurface *s, double x, double y,
		uint32_t button, bool pressed);
	void (*pointer_axis)(struct psurface *s, double x, double y, int direction);
	void (*key)(struct psurface *s, xkb_keysym_t sym, const char *utf8,
		uint32_t modifiers);
	void (*keyboard_leave)(struct psurface *s);
	void (*configured)(struct psurface *s);
	void (*closed)(struct psurface *s);
};

struct psurface {
	struct panel *panel;
	struct panel_output *output;
	const struct psurface_impl *impl;
	void *data;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_viewport *viewport;
	int width, height; // logical size configured by the compositor
	int req_width, req_height;
	int scale;
	struct pool_buffer buffers[2];
	bool configured, dirty, frame_pending, catcher;
	list_t *hotspots; // struct hotspot *
	struct hotspot *hover;
	struct wl_list link; // panel::surfaces
};

struct psurface *psurface_create(struct panel *panel, struct panel_output *output,
	const struct psurface_impl *impl, void *data,
	enum zwlr_layer_shell_v1_layer layer, const char *name_space);
/* A transparent full-output surface that reports clicks outside popups. */
struct psurface *psurface_create_catcher(struct panel *panel,
	struct panel_output *output, const struct psurface_impl *impl, void *data);
void psurface_destroy(struct psurface *s);
void psurface_set_size(struct psurface *s, int width, int height);
void psurface_set_dirty(struct psurface *s);
void psurface_render(struct psurface *s);
void psurface_add_hotspot(struct psurface *s, int x, int y, int width, int height,
	struct widget *widget, int kind, int64_t id, const char *str);
struct hotspot *psurface_hotspot_at(struct psurface *s, double x, double y);
void psurface_clear_hotspots(struct psurface *s);

/* ---------- outputs and seats ---------- */

struct panel_output {
	struct panel *panel;
	struct wl_output *wl_output;
	struct zxdg_output_v1 *xdg_output;
	uint32_t wl_name;
	char *name;
	int scale;
	enum wl_output_subpixel subpixel;
	int x, y, width, height;
	bool ready;
	struct psurface *bar;
	struct psurface *desktop, *desktop_bg; // desktop icons and desktop input surface
	struct wl_list link; // panel::outputs
};

struct panel_seat {
	struct panel *panel;
	struct wl_seat *wl_seat;
	uint32_t wl_name;

	struct wl_pointer *pointer;
	struct psurface *pointer_focus;
	double px, py;
	uint32_t enter_serial;
	struct wl_surface *cursor_surface;
	struct wl_cursor_theme *cursor_theme;
	double axis_accum;

	struct wl_keyboard *keyboard;
	struct psurface *keyboard_focus;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	int32_t repeat_rate, repeat_delay;
	struct loop_timer *repeat_timer;
	uint32_t repeat_key;

	struct wl_list link; // panel::seats
};

extern const struct wl_seat_listener panel_seat_listener;
void panel_seat_destroy(struct panel_seat *seat);
/* Ctrl (1) and Shift (2) of a seat whose keyboard is on one of our surfaces. */
uint32_t panel_modifiers(struct panel *panel);

/* ---------- compositor state ---------- */

struct pwindow {
	int64_t id;
	char *app_id;
	char *title;
	char *workspace;
	char *output;
	char *identifier; // foreign toplevel identifier
	int pid;
	bool focused, urgent, minimized, maximized, floating, above;
	int order; // creation order
};

struct pworkspace {
	char *name;
	int num;
	char *output;
	bool focused, visible, urgent;
};

struct panel_state {
	list_t *windows;    // struct pwindow *
	list_t *workspaces; // struct pworkspace *
	char *mode;         // "tile" or "window"
	char *focused_output;
	char *focused_workspace;
	char *keyboard_layout;
	int64_t focused_window;
	int double_click_ms; // "double_click_time" of the compositor
};

/* ---------- config ---------- */

enum layout_index {
	LAYOUT_TILE = 0,
	LAYOUT_WINDOW = 1,
};

struct layout_config {
	bool bottom;
	int height; // 0 = theme default
	list_t *left, *center, *right; // struct widget *
};

struct menu_item {
	char *label;
	char *command;
	char *icon;
	list_t *children; // struct menu_item *, submenu if non-NULL
	bool separator, disabled, checked, bold;
};

struct panel_config {
	struct twconf_node *root;
	char *font;
	list_t *outputs; // char *, NULL for all
	char *terminal;
	struct layout_config layouts[2];
	list_t *widgets; // struct widget * (all instances)
	struct twconf_node *startmenu;
	int tooltip_delay;
	bool desktop_icons;
	// the desktop grid: the picture, the cell it sits in and the outer margin
	int desktop_icon_size, desktop_cell_width, desktop_cell_height, desktop_margin;
};

/* ---------- widgets ---------- */

struct render_ctx {
	struct panel *panel;
	struct psurface *surface;
	struct panel_output *output;
	cairo_t *cairo;
	int height;
	enum pstyle_value {
		PSV_CLASSIC,
		PSV_LUNA,
		PSV_AERO,
		PSV_FLAT,
		PSV_FLUENT,
	} style;
	bool bottom;
	bool centered;      // measuring for a centered group
	bool pointer_inside;
	bool pressed;
	double px, py;
};

/* bar.c render helpers */
const char *bar_font(struct panel *panel);
const char *bar_bold_font(struct panel *panel);
uint32_t bar_fg(struct panel *panel);
/* Text color of a widget type: theme key "<type>.fg", else the panel color. */
uint32_t widget_fg(struct panel *panel, const char *type);
bool render_hover(struct render_ctx *ctx, struct pbox box);
bool render_pressed(struct render_ctx *ctx, struct pbox box);
int render_text_width(struct render_ctx *ctx, const char *font, const char *text);
/* Background of a clickable item in the current style. */
void render_item_bg(struct render_ctx *ctx, struct pbox box, bool active,
	bool hover, bool pressed);
list_t *panel_named_menu(struct panel *panel, const char *name);
list_t *taskbar_window_menu(struct panel *panel, struct pwindow *win);
bool taskbar_activate_index(struct panel *panel, int index);
list_t *start_default_menu(struct panel *panel);

struct widget_impl {
	const char *type;
	void (*init)(struct widget *w);
	void (*destroy)(struct widget *w);
	/* Width for this output; return -1 to take all remaining space. */
	int (*measure)(struct widget *w, struct render_ctx *ctx);
	void (*render)(struct widget *w, struct render_ctx *ctx, struct pbox box);
	/* Returns true when the click was consumed. */
	bool (*click)(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y);
	bool (*scroll)(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction);
	char *(*tooltip)(struct widget *w, struct hotspot *hs);
	void (*set_active)(struct widget *w, bool active);
	void (*state_changed)(struct widget *w);
};

struct widget {
	const struct widget_impl *impl;
	struct panel *panel;
	char *name;
	struct twconf_node *conf; // may be NULL
	void *data;
	bool active;
	char *on_click, *on_middle_click, *on_right_click;
	char *on_scroll_up, *on_scroll_down;
	list_t *menu; // struct menu_item *, NULL if none
};

const struct widget_impl *widget_impl_find(const char *type);
struct widget *widget_create(struct panel *panel, const char *name,
	struct twconf_node *conf);
void widget_destroy(struct widget *w);
const char *widget_conf(struct widget *w, const char *key, const char *fallback);
int widget_conf_int(struct widget *w, const char *key, int fallback);
bool widget_conf_bool(struct widget *w, const char *key, bool fallback);
uint32_t widget_conf_color(struct widget *w, const char *key, uint32_t fallback);

extern const struct widget_impl widget_start;
extern const struct widget_impl widget_taskbar;
extern const struct widget_impl widget_quicklaunch;
extern const struct widget_impl widget_workspaces;
extern const struct widget_impl widget_title;
extern const struct widget_impl widget_tray;
extern const struct widget_impl widget_clock;
extern const struct widget_impl widget_volume;
extern const struct widget_impl widget_battery;
extern const struct widget_impl widget_network;
extern const struct widget_impl widget_cpu;
extern const struct widget_impl widget_memory;
extern const struct widget_impl widget_disk;
extern const struct widget_impl widget_gpu;
extern const struct widget_impl widget_net;
extern const struct widget_impl widget_storage;
extern const struct widget_impl widget_power;
extern const struct widget_impl widget_brightness;
extern const struct widget_impl widget_keyboard;
extern const struct widget_impl widget_modeswitch;
extern const struct widget_impl widget_showdesktop;
extern const struct widget_impl widget_search;
extern const struct widget_impl widget_separator;
extern const struct widget_impl widget_spacer;
extern const struct widget_impl widget_custom;
extern const struct widget_impl widget_notifications;

/* ---------- panel ---------- */

struct panel {
	bool running;
	struct loop *loop;
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wp_viewporter *viewporter;
	struct ext_data_control_manager_v1 *data_control;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard;
	struct ext_foreign_toplevel_list_v1 *toplevel_list;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_capture;
	struct ext_image_copy_capture_manager_v1 *copy_capture;
	struct zwlr_screencopy_manager_v1 *screencopy;
	uint32_t screencopy_version;
	struct wl_list outputs;  // panel_output::link
	struct wl_list seats;    // panel_seat::link
	struct wl_list surfaces; // psurface::link

	char *socket_path;
	int ipc_cmd_fd;
	int ipc_event_fd;
	struct panel_state state;
	struct loop_timer *tree_timer;

	char *config_path;
	struct panel_config *config;
	struct tw_theme *theme;
	enum layout_index layout;
	int inotify_fd;
	int config_watch, theme_watch, desktop_watch;
	struct loop_timer *reload_timer;

	struct popup *popup;
	struct psurface *tooltip;
	struct loop_timer *tooltip_timer;
	struct psurface *tooltip_source;
	struct hotspot tooltip_hotspot;
	bool tooltip_pending;
};

/* panel style derived from the theme */
enum pstyle {
	PS_CLASSIC,
	PS_LUNA,
	PS_AERO,
	PS_FLAT,
	PS_FLUENT,
};
enum pstyle panel_style(struct panel *panel);

/* main.c */
void panel_set_dirty(struct panel *panel);
void panel_request_reload(struct panel *panel);
struct panel_output *panel_focused_output(struct panel *panel);
struct panel_seat *panel_first_seat(struct panel *panel);

/* wayland.c */
bool panel_wayland_init(struct panel *panel);
void panel_wayland_fini(struct panel *panel);
void panel_outputs_update_bars(struct panel *panel);

/* bar.c */
void bar_create(struct panel_output *output);
void bar_destroy(struct panel_output *output);
int bar_height(struct panel *panel);
void bar_run_command(struct panel *panel, const char *command, const char *context_id);
void bar_handle_panel_command(struct panel *panel, const char *args);
void bar_hide_tooltip(struct panel *panel);

/* ipc.c */
bool ipc_panel_init(struct panel *panel);
void ipc_panel_fini(struct panel *panel);
void ipc_panel_readable(struct panel *panel);
bool ipc_panel_command(struct panel *panel, const char *command);
bool ipc_panel_commandf(struct panel *panel, const char *fmt, ...);
void ipc_panel_refresh_tree(struct panel *panel);
void ipc_panel_refresh_workspaces(struct panel *panel);
void ipc_panel_refresh_inputs(struct panel *panel);
struct pwindow *panel_find_window(struct panel *panel, int64_t id);

/* config.c */
struct panel_config *panel_config_load(struct panel *panel, const char *path);
void panel_config_free(struct panel_config *config);
char *panel_default_config_path(void);
list_t *menu_items_parse(struct twconf_node *node);
void menu_items_free(list_t *items);
struct menu_item *menu_item_new(const char *label, const char *command);
struct menu_item *menu_item_separator(void);
/* Icon for a menu entry without one, from its command or submenu label; NULL if none. */
char *menu_default_icon(const char *label, const char *command);
/* Gives the entries of a menu (not its submenus) their default icons. */
void menu_items_default_icons(list_t *items);

/* popup.c */
enum popup_kind {
	POPUP_MENU,
	POPUP_STARTMENU,
	POPUP_RUN,
	POPUP_CALENDAR,
	POPUP_LAUNCHER,
	POPUP_NETWORK,
	POPUP_VOLUME,
	POPUP_POWER,
	POPUP_CPU,
	POPUP_MEMORY,
	POPUP_DIALOG,
	POPUP_SHUTDOWN,
	POPUP_NOTIFICATIONS,
	POPUP_QUICKSETTINGS,
	POPUP_BLUETOOTH,
	POPUP_SNIP,
	POPUP_CLIPBOARD,
};
struct popup_anchor {
	struct panel_output *output;
	int x, y;         // output-local position of the anchor point
	bool above;       // open above the anchor (bottom bar)
	bool right_align; // align the right edge to x
};
void popup_close_all(struct panel *panel);
bool popup_is_open(struct panel *panel, enum popup_kind kind);
void menu_open(struct panel *panel, list_t *items, bool owns_items,
	struct popup_anchor anchor, const char *context_id);
void startmenu_toggle(struct panel *panel, struct panel_output *output, bool search);
/* shutdown.c: the shut down dialog of the theme */
void shutdown_dialog_open(struct panel *panel, struct panel_output *output, bool logoff);
void rundialog_open(struct panel *panel, struct panel_output *output);
/* The same with the box already filled, e.g. with a file that was browsed to. */
void rundialog_open_with(struct panel *panel, struct panel_output *output,
		const char *prefill);
/* flyouts.c: the clock and calendar flyout of the clock widget */
void calendar_toggle(struct panel *panel, struct popup_anchor anchor, const char *settings);
void launcher_toggle(struct panel *panel, struct panel_output *output);
struct popup_anchor popup_anchor_for_bar(struct psurface *bar, int x, int width);
/* A menu entry of a tray icon was clicked ("panel tray_event <service> <path> <id>"). */
void tray_handle_event_command(struct panel *panel, const char *service,
	const char *path, int id);

/* desktop.c */
char *desktop_directory(void);
void desktop_create(struct panel_output *output);
void desktop_destroy(struct panel_output *output);
void desktop_dir_changed(struct panel *panel);
void desktop_handle_command(struct panel *panel, int argc, char **argv);

/* flyouts.c */
#define TW_DATETIME_SETTINGS "exec tilewin-settings --page datetime"
#define TW_NETWORK_SETTINGS "exec sh -c 'command -v nm-connection-editor >/dev/null && exec nm-connection-editor || exec xfce4-terminal -e nmtui'"
/* Anchor at the tray end of the taskbar on an output. */
struct popup_anchor flyout_anchor(struct panel *panel, struct panel_output *output);
void flyout_network_toggle(struct panel *panel, struct popup_anchor anchor, const char *settings);
void flyout_volume_toggle(struct panel *panel, struct popup_anchor anchor, const char *mixer);
void flyout_power_toggle(struct panel *panel, struct popup_anchor anchor, const char *settings);
/* task_manager: command of the footer link, NULL for btop/htop/top in the terminal */
void flyout_cpu_toggle(struct panel *panel, struct popup_anchor anchor, const char *task_manager);
void flyout_memory_toggle(struct panel *panel, struct popup_anchor anchor,
		const char *task_manager);
/* Called when PulseAudio reports a change, refreshes an open volume flyout. */
void flyout_volume_changed(struct panel *panel);

/* notify.c: the notification service, pop-ups and the Action Center */
void notify_init(struct panel *panel);
void notify_fini(struct panel *panel);
void notify_theme_changed(struct panel *panel);
/* "notifications [toggle|open|close|clear]" and "dnd [toggle|on|off]" */
void notify_handle_command(struct panel *panel, int argc, char **argv);
bool notify_dnd(void);
void notify_set_dnd(struct panel *panel, bool on);
/* The bell of the notifications button, crossed out for do not disturb. */
void notify_draw_bell(cairo_t *cr, double x, double y, double size, uint32_t color,
	bool crossed);

/* thumbnails.c: window previews above taskbar buttons */
void thumbnails_list_bound(struct panel *panel);
/* Shows previews instead of a tooltip; false if the hotspot has none. */
bool thumbnails_show(struct panel *panel, struct psurface *bar, struct hotspot *hs);
void thumbnails_hide_later(struct panel *panel);
bool thumbnails_visible(void);
void thumbnails_fini(struct panel *panel);

/* clipboard.c: clipboard history (Win+V) */
void clipboard_init(struct panel *panel);
void clipboard_fini(struct panel *panel);
void clipboard_toggle(struct panel *panel, struct panel_output *output);
/* Puts text on the clipboard. */
void clipboard_copy_text(struct panel *panel, const char *text);

/* snip.c: the snipping toolbar (Win+Shift+S) */
void snip_toolbar_toggle(struct panel *panel, struct panel_output *output);

/* quicksettings.c: Wi-Fi, Bluetooth, sound, brightness and more in one flyout */
void quicksettings_toggle(struct panel *panel, struct popup_anchor anchor);
/* The sound changed: reads the state again. */
void quicksettings_changed(struct panel *panel);
/* Bluetooth or do not disturb changed: only draws again. */
void quicksettings_redraw(struct panel *panel);

/* bluetooth.c: BlueZ on the system bus */
struct bt_device {
	char *path, *name, *address, *icon;
	bool paired, connected, trusted;
	bool has_rssi;
	int rssi;
	bool busy; // connecting or pairing
};
void bt_init(struct panel *panel);
void bt_fini(void);
bool bt_available(void); // bluetoothd runs and there is an adapter
bool bt_powered(void);
bool bt_discovering(void);
list_t *bt_devices(void); // struct bt_device *, may be NULL
/* The last error or pairing code to show, NULL if none. */
const char *bt_status(void);
void bt_set_powered(bool on);
void bt_set_discovery(bool on);
void bt_connect(const char *path, bool connect);
void bt_pair(const char *path);
void bt_remove(const char *path);
/* flyouts.c */
void flyout_bluetooth_toggle(struct panel *panel, struct popup_anchor anchor);
void flyout_bluetooth_changed(struct panel *panel);

/* tooltip.c */
void tooltip_schedule(struct panel *panel, struct psurface *s, struct hotspot *hs);
void tooltip_cancel(struct panel *panel);

/* proc.c */
struct proc;
typedef void (*proc_line_fn)(void *data, const char *line);
typedef void (*proc_done_fn)(void *data, const char *output);
struct proc *proc_run(struct panel *panel, const char *command, bool listen,
	proc_line_fn on_line, proc_done_fn on_done, void *data);
void proc_cancel(struct proc *proc);
void proc_spawn(const char *command);

/* apps.c */
list_t *apps_get(void);         // struct tw_desktop_entry *, cached
void apps_launch(struct panel *panel, const struct tw_desktop_entry *entry);
struct tw_desktop_entry *apps_find(const char *id_or_app_id);
cairo_surface_t *apps_icon_for_window(struct panel *panel, struct pwindow *win, int size);
cairo_surface_t *apps_icon(struct panel *panel, const char *name, int size);
/* osd.c: volume, brightness and media popup ("panel osd volume 45 0") */
void osd_handle_command(struct panel *panel, int argc, char **argv);

/* trayicons.c: the theme's notification area icons, or drawn glyphs */
void ti_speaker(struct panel *panel, cairo_t *cr, double x, double y, double size,
	int volume, bool muted, uint32_t color);
void ti_network(struct panel *panel, cairo_t *cr, double x, double y, double size,
	int bars, bool wireless, bool connected, uint32_t color);
void ti_battery(struct panel *panel, cairo_t *cr, double x, double y, double size,
	int percent, bool charging, uint32_t color);
void ti_brightness(struct panel *panel, cairo_t *cr, double x, double y, double size,
	uint32_t color);
void ti_disk(struct panel *panel, cairo_t *cr, double x, double y, double size,
	bool active, uint32_t color);
/* Forgets loaded icons, e.g. after a theme switch. */
void apps_icon_cache_clear(void);
const char *apps_display_name(const char *app_id);
/* 0 = no match, higher is better. */
int apps_match_score(const struct tw_desktop_entry *entry, const char *query);

#endif
