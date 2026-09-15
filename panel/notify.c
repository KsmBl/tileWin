/*
 * Notifications, like on Windows.
 *
 * The panel is the org.freedesktop.Notifications service. When another
 * notification daemon owns the name, it waits in the queue and takes over when
 * that one quits. New notifications pop up next to the notification area: as
 * toasts (Windows 10 and 11) or as balloons (95, XP, 7; theme key
 * notifications.style toast|balloon). The Action Center ("panel
 * notifications", Win+N or the notifications button of the taskbar) lists the
 * history, which is kept in ~/.local/state/tileWin/notifications.json. Do not
 * disturb ("panel dnd", ~/.local/state/tileWin/do-not-disturb) only lets
 * urgent notifications pop up; the others still go to the Action Center.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <inttypes.h>
#include <json.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "config.h"
#if HAVE_TRAY
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif
#endif
#include "draw.h"
#include "flyout.h"
#include "log.h"
#include "panel.h"
#include "popup.h"
#include "tw_desktop.h"
#include "tw_paths.h"

#define MAX_HISTORY 50
#define MAX_TOASTS 3
#define TOAST_MS 6000
#define TOAST_HOVER_MS 3000
#define TOAST_WIDTH 364
#define BALLOON_WIDTH 300
#define BALLOON_TAIL 14
#define GAP 10
#define CENTER_WIDTH 380
#define CENTER_HEADER 56
#define CENTER_FOOTER 52
#define CENTER_EMPTY 120
#define CENTER_PAD 16
#define MAX_ACTIONS 3

#define BUS_PATH "/org/freedesktop/Notifications"
#define BUS_NAME "org.freedesktop.Notifications"

enum close_reason {
	CLOSED_EXPIRED = 1,
	CLOSED_DISMISSED = 2,
	CLOSED_BY_APP = 3,
};

struct n_action {
	char *key, *label;
};

struct notification {
	uint32_t id;
	char *app_name, *app_icon, *desktop_entry, *image_path;
	char *summary, *body;
	list_t *actions; // struct n_action *
	cairo_surface_t *image;
	uint8_t urgency; // 0 low, 1 normal, 2 critical
	int32_t timeout; // ms; -1 default, 0 never
	bool transient;
	bool stale; // from an earlier session: its app can't be told anything
	time_t time;

	// pop-up
	bool pending; // waits for a free place
	struct psurface *toast;
	struct loop_timer *timer;
	bool hover;
	double px, py;
	struct pbox close_box, body_box;
	struct pbox action_boxes[MAX_ACTIONS];
	int action_count;
};

enum op_kind {
	OP_HIDE,
	OP_ACTIVATE,
	OP_ACTION,
};

struct op {
	enum op_kind kind;
	uint32_t id;
	int action;
};

static struct {
	struct panel *panel;
#if HAVE_TRAY
	sd_bus *bus;
#endif
	int bus_fd;
	uint32_t next_id;
	list_t *items; // struct notification *, oldest first
	int unread;
	bool dnd;
	struct loop_timer *save_timer;
	struct loop_timer *ops_timer;
	struct op ops[16];
	int op_count;
} nt = { .bus_fd = -1 };

static void center_changed(void);
static void toasts_update(void);

/* ---------- helpers ---------- */

static char *nonempty_dup(const char *s) {
	return s && *s ? strdup(s) : NULL;
}

/* Notification bodies often contain simple markup even when it isn't asked for. */
static char *plain_text(const char *s) {
	if (!s) {
		return strdup("");
	}
	static const struct {
		const char *entity;
		char c;
	} entities[] = {
		{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
		{ "&apos;", '\'' }, { "&#39;", '\'' },
	};
	char *out = malloc(strlen(s) + 1);
	char *o = out;
	for (const char *p = s; *p;) {
		if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '/')) {
			const char *end = strchr(p, '>');
			if (end) {
				p = end + 1;
				continue;
			}
		}
		if (*p == '&') {
			bool matched = false;
			for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
				size_t len = strlen(entities[i].entity);
				if (strncmp(p, entities[i].entity, len) == 0) {
					*o++ = entities[i].c;
					p += len;
					matched = true;
					break;
				}
			}
			if (matched) {
				continue;
			}
		}
		*o++ = *p++;
	}
	*o = '\0';
	return out;
}

static char *state_file(const char *name) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = malloc(strlen(dir) + strlen(name) + 2);
	sprintf(path, "%s/%s", dir, name);
	free(dir);
	return path;
}

static struct notification *find(uint32_t id) {
	for (int i = 0; nt.items && i < nt.items->length; i++) {
		struct notification *n = nt.items->items[i];
		if (n->id == id) {
			return n;
		}
	}
	return NULL;
}

static bool balloon_style(void) {
	const char *style = tw_theme_str(nt.panel->theme, "notifications.style", NULL);
	if (style) {
		return strcmp(style, "balloon") == 0;
	}
	enum pstyle ps = panel_style(nt.panel);
	return ps == PS_CLASSIC || ps == PS_LUNA || ps == PS_AERO;
}

static bool bar_bottom(void) {
	struct panel *panel = nt.panel;
	return panel->config ? panel->config->layouts[panel->layout].bottom : true;
}

static const char *group_key(const struct notification *n) {
	return n->desktop_entry ? n->desktop_entry : n->app_name ? n->app_name : "";
}

static const char *group_title(const struct notification *n) {
	if (n->app_name) {
		return n->app_name;
	}
	const char *name = n->desktop_entry ? apps_display_name(n->desktop_entry) : NULL;
	return name && *name ? name : "Other";
}

static cairo_surface_t *notification_icon(struct notification *n, int size, bool app_only) {
	if (n->image && !app_only) {
		return n->image;
	}
	const char *names[] = { app_only ? NULL : n->image_path, n->app_icon };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *name = names[i];
		if (!name) {
			continue;
		}
		if (strncmp(name, "file://", 7) == 0) {
			name += 7;
		}
		cairo_surface_t *icon = apps_icon(nt.panel, name, size);
		if (icon) {
			return icon;
		}
	}
	const char *ids[] = { n->desktop_entry, n->app_name };
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		struct tw_desktop_entry *entry = ids[i] ? apps_find(ids[i]) : NULL;
		if (entry && entry->icon) {
			cairo_surface_t *icon = apps_icon(nt.panel, entry->icon, size);
			if (icon) {
				return icon;
			}
		}
	}
	return NULL;
}

/* The buttons of a notification: its actions except "default". */
static struct n_action *visible_action(const struct notification *n, int index) {
	for (int i = 0; n->actions && i < n->actions->length; i++) {
		struct n_action *a = n->actions->items[i];
		if (strcmp(a->key, "default") != 0 && index-- == 0) {
			return a;
		}
	}
	return NULL;
}

static int visible_action_count(const struct notification *n) {
	if (n->stale) {
		return 0;
	}
	int count = 0;
	while (count < MAX_ACTIONS && visible_action(n, count)) {
		count++;
	}
	return count;
}

static bool has_default_action(const struct notification *n) {
	for (int i = 0; n->actions && i < n->actions->length; i++) {
		struct n_action *a = n->actions->items[i];
		if (strcmp(a->key, "default") == 0) {
			return true;
		}
	}
	return false;
}

static void free_actions(list_t *actions) {
	for (int i = 0; actions && i < actions->length; i++) {
		struct n_action *a = actions->items[i];
		free(a->key);
		free(a->label);
		free(a);
	}
	list_free(actions);
}

static void toast_destroy(struct notification *n) {
	if (n->timer) {
		loop_remove_timer(nt.panel->loop, n->timer);
		n->timer = NULL;
	}
	if (n->toast) {
		struct psurface *s = n->toast;
		n->toast = NULL;
		psurface_destroy(s);
	}
	n->hover = false;
}

static void notification_free(struct notification *n) {
	toast_destroy(n);
	free(n->app_name);
	free(n->app_icon);
	free(n->desktop_entry);
	free(n->image_path);
	free(n->summary);
	free(n->body);
	free_actions(n->actions);
	if (n->image) {
		cairo_surface_destroy(n->image);
	}
	free(n);
}

static void draw_x(cairo_t *cr, struct pbox b, uint32_t color, double size) {
	double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0, r = size / 2;
	cairo_new_path(cr);
	cairo_move_to(cr, cx - r, cy - r);
	cairo_line_to(cr, cx + r, cy + r);
	cairo_move_to(cr, cx + r, cy - r);
	cairo_line_to(cr, cx - r, cy + r);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.3);
	cairo_stroke(cr);
}

static void format_time(time_t t, char *buf, size_t size) {
	time_t now = time(NULL);
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) {
		strftime(buf, size, "%H:%M", &a);
	} else {
		strftime(buf, size, "%x", &a);
	}
}

/* ---------- history file and do not disturb ---------- */

static void save_now(void *data) {
	nt.save_timer = NULL;
	char *path = state_file("notifications.json");
	if (!path) {
		return;
	}
	json_object *array = json_object_new_array();
	for (int i = 0; i < nt.items->length; i++) {
		struct notification *n = nt.items->items[i];
		if (n->transient) {
			continue;
		}
		json_object *obj = json_object_new_object();
		const char *fields[][2] = {
			{ "app_name", n->app_name }, { "app_icon", n->app_icon },
			{ "desktop_entry", n->desktop_entry }, { "image_path", n->image_path },
			{ "summary", n->summary }, { "body", n->body },
		};
		for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
			if (fields[f][1]) {
				json_object_object_add(obj, fields[f][0],
					json_object_new_string(fields[f][1]));
			}
		}
		json_object_object_add(obj, "time", json_object_new_int64(n->time));
		json_object_object_add(obj, "urgency", json_object_new_int(n->urgency));
		json_object_array_add(array, obj);
	}
	tw_write_string(path, json_object_to_json_string_ext(array, JSON_C_TO_STRING_PLAIN));
	json_object_put(array);
	free(path);
}

static void save_later(void) {
	if (!nt.save_timer) {
		nt.save_timer = loop_add_timer(nt.panel->loop, 1000, save_now, NULL);
	}
}

static const char *json_string(json_object *obj, const char *key) {
	json_object *value;
	return json_object_object_get_ex(obj, key, &value) &&
		json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

static void load_history(void) {
	char *path = state_file("notifications.json");
	json_object *array = path ? json_object_from_file(path) : NULL;
	free(path);
	if (!array || !json_object_is_type(array, json_type_array)) {
		json_object_put(array);
		return;
	}
	size_t length = json_object_array_length(array);
	size_t start = length > MAX_HISTORY ? length - MAX_HISTORY : 0;
	for (size_t i = start; i < length; i++) {
		json_object *obj = json_object_array_get_idx(array, i);
		struct notification *n = calloc(1, sizeof(*n));
		n->id = ++nt.next_id;
		n->stale = true;
		n->app_name = nonempty_dup(json_string(obj, "app_name"));
		n->app_icon = nonempty_dup(json_string(obj, "app_icon"));
		n->desktop_entry = nonempty_dup(json_string(obj, "desktop_entry"));
		n->image_path = nonempty_dup(json_string(obj, "image_path"));
		n->summary = strdup(json_string(obj, "summary") ? json_string(obj, "summary") : "");
		n->body = strdup(json_string(obj, "body") ? json_string(obj, "body") : "");
		json_object *value;
		if (json_object_object_get_ex(obj, "time", &value)) {
			n->time = json_object_get_int64(value);
		}
		if (json_object_object_get_ex(obj, "urgency", &value)) {
			n->urgency = json_object_get_int(value);
		}
		list_add(nt.items, n);
	}
	json_object_put(array);
}

bool notify_dnd(void) {
	return nt.dnd;
}

void notify_set_dnd(struct panel *panel, bool on) {
	nt.dnd = on;
	char *path = state_file("do-not-disturb");
	if (path) {
		tw_write_string(path, on ? "on\n" : "off\n");
		free(path);
	}
	if (on) {
		for (int i = 0; i < nt.items->length; i++) {
			struct notification *n = nt.items->items[i];
			if (n->urgency < 2) {
				n->pending = false;
				toast_destroy(n);
			}
		}
		toasts_update();
	}
	panel_set_dirty(panel);
	center_changed();
	quicksettings_redraw(panel);
}

/* ---------- bus ---------- */

static void emit_closed(struct notification *n, enum close_reason reason) {
#if HAVE_TRAY
	if (nt.bus && !n->stale) {
		sd_bus_emit_signal(nt.bus, BUS_PATH, BUS_NAME, "NotificationClosed", "uu",
			n->id, (uint32_t)reason);
		sd_bus_flush(nt.bus);
	}
#endif
}

static void emit_action(struct notification *n, const char *key) {
#if HAVE_TRAY
	if (nt.bus && !n->stale) {
		sd_bus_emit_signal(nt.bus, BUS_PATH, BUS_NAME, "ActionInvoked", "us", n->id, key);
		sd_bus_flush(nt.bus);
	}
#endif
}

/* ---------- list changes ---------- */

static void remove_item(struct notification *n, enum close_reason reason) {
	for (int i = 0; i < nt.items->length; i++) {
		if (nt.items->items[i] == n) {
			list_del(nt.items, i);
			break;
		}
	}
	emit_closed(n, reason);
	notification_free(n);
	save_later();
	toasts_update();
	center_changed();
	panel_set_dirty(nt.panel);
}

static void focus_app(struct notification *n) {
	const char *names[] = { n->desktop_entry, n->app_name };
	list_t *windows = nt.panel->state.windows;
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		for (int i = 0; names[k] && windows && i < windows->length; i++) {
			struct pwindow *win = windows->items[i];
			const char *app_id = win->app_id;
			if (app_id && (strcasecmp(app_id, names[k]) == 0 ||
					strcasestr(names[k], app_id) != NULL)) {
				ipc_panel_commandf(nt.panel, win->minimized ?
					"[con_id=%" PRId64 "] minimize disable, focus" :
					"[con_id=%" PRId64 "] focus", win->id);
				return;
			}
		}
	}
}

/* Clicked: the app shows it, and it leaves the list like on Windows. */
static void activate(struct notification *n) {
	if (has_default_action(n)) {
		emit_action(n, "default");
	}
	focus_app(n);
	remove_item(n, CLOSED_DISMISSED);
}

static void invoke_action(struct notification *n, int index) {
	struct n_action *a = visible_action(n, index);
	if (!a) {
		return;
	}
	emit_action(n, a->key);
	remove_item(n, CLOSED_DISMISSED);
}

static void clear_all(void) {
	while (nt.items->length > 0) {
		struct notification *n = nt.items->items[0];
		list_del(nt.items, 0);
		emit_closed(n, CLOSED_DISMISSED);
		notification_free(n);
	}
	nt.unread = 0;
	save_later();
	center_changed();
	panel_set_dirty(nt.panel);
}

static void clear_group(const char *key) {
	for (int i = nt.items->length - 1; i >= 0; i--) {
		struct notification *n = nt.items->items[i];
		if (strcmp(group_key(n), key) == 0) {
			list_del(nt.items, i);
			emit_closed(n, CLOSED_DISMISSED);
			notification_free(n);
		}
	}
	save_later();
	toasts_update();
	center_changed();
	panel_set_dirty(nt.panel);
}

/* ---------- pop-ups ---------- */

struct ncolors {
	uint32_t bg, fg, dim, border, accent, button, button_hover;
	double radius;
	bool balloon;
};

static void toast_colors(struct ncolors *c) {
	const struct tw_theme *t = nt.panel->theme;
	enum pstyle style = panel_style(nt.panel);
	c->balloon = balloon_style();
	c->accent = tw_theme_color(t, "taskbar.indicator", 0x0078d4ff);
	if (c->balloon) {
		bool aero = style == PS_AERO;
		c->bg = aero ? 0xfbfbfbff : 0xffffe1ff;
		c->fg = 0x000000ff;
		c->dim = 0x303030ff;
		c->border = aero ? 0x767676ff : 0x000000ff;
		c->accent = 0x0033ccff;
		c->radius = style == PS_CLASSIC ? 4 : aero ? 5 : 8;
	} else if (style == PS_FLUENT) {
		c->bg = t->dark ? 0x2c2c2cf8 : 0xf3f3f3f8;
		c->fg = t->dark ? 0xffffffff : 0x1b1b1bff;
		c->dim = t->dark ? 0xffffffb0 : 0x1b1b1bb0;
		c->border = t->dark ? 0xffffff1c : 0x0000001c;
		c->accent = t->dark ? 0x60cdffff : 0x005fb8ff;
		c->radius = 8;
	} else {
		c->bg = 0x1f1f1ff4;
		c->fg = 0xffffffff;
		c->dim = 0xffffffb0;
		c->border = 0x3c3c3cff;
		c->radius = 0;
	}
	bool dark_bg = ((c->bg >> 24 & 0xff) * 299 + (c->bg >> 16 & 0xff) * 587 +
		(c->bg >> 8 & 0xff) * 114) / 1000 < 128;
	c->button = dark_bg ? 0xffffff1f : 0x0000000f;
	c->button_hover = dark_bg ? 0xffffff38 : 0x00000024;
	c->bg = tw_theme_color(t, "notifications.bg", c->bg);
	c->fg = tw_theme_color(t, "notifications.fg", c->fg);
	c->border = tw_theme_color(t, "notifications.border", c->border);
}

static bool toast_hovered(struct notification *n, struct pbox b) {
	return n->hover && pbox_contains(&b, n->px, n->py);
}

/* Lays a pop-up out and draws it if cr belongs to its surface; returns the height. */
static int toast_layout(struct notification *n, cairo_t *cr, int W, bool draw) {
	struct ncolors c;
	toast_colors(&c);
	const char *font = tw_theme_str(nt.panel->theme, "menu.font", bar_font(nt.panel));
	const char *bold = bar_bold_font(nt.panel);
	int actions = visible_action_count(n);
	n->action_count = actions;

	if (c.balloon) {
		int pad = 10, top = bar_bottom() ? 0 : BALLOON_TAIL;
		int tx = pad + 22, tw = W - tx - pad - 18;
		int y = top + pad;
		int sh = pd_text_wrapped(cr, bold, n->summary, tx, y, tw, 2, c.fg, false);
		if (sh < 16) {
			sh = 16;
		}
		int bh = pd_text_wrapped(cr, font, n->body, pad, y + sh + 6, W - 2 * pad, 5, c.fg,
			false);
		int content = y + sh + (bh ? bh + 6 : 0) + pad + (actions ? actions * 20 : 0);
		int H = content + (top ? 0 : BALLOON_TAIL);
		if (draw) {
			double bx = 0.5, by = top + 0.5, bw = W - 1, bhh = content - top - 1;
			double tail = W - 44; // points at the notification area
			cairo_new_path(cr);
			pd_rounded(cr, bx, by, bw, bhh, c.radius);
			cairo_new_sub_path(cr);
			if (top) {
				cairo_move_to(cr, tail, by + 1);
				cairo_line_to(cr, tail + 14, 0.5);
				cairo_line_to(cr, tail + 18, by + 1);
			} else {
				cairo_move_to(cr, tail, by + bhh - 1);
				cairo_line_to(cr, tail + 14, H - 0.5);
				cairo_line_to(cr, tail + 18, by + bhh - 1);
			}
			cairo_close_path(cr);
			cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
			pd_color(cr, c.bg);
			cairo_fill_preserve(cr);
			pd_color(cr, c.border);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			// hide the border between the bubble and its tail
			pd_rect(cr, tail + 1, top ? by : by + bhh - 1.5, 16.5, 2, c.bg);

			pd_icon(cr, notification_icon(n, 16, false), pad, y, 16);
			pd_text_wrapped(cr, bold, n->summary, tx, y, tw, 2, c.fg, true);
			n->close_box = (struct pbox){ W - pad - 16, y, 16, 16 };
			if (toast_hovered(n, n->close_box)) {
				pd_rect(cr, n->close_box.x, n->close_box.y, 16, 16, c.button_hover);
			}
			cairo_new_path(cr);
			cairo_rectangle(cr, n->close_box.x + 0.5, n->close_box.y + 0.5, 15, 15);
			pd_color(cr, c.dim);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			draw_x(cr, n->close_box, c.fg, 7);
			pd_text_wrapped(cr, font, n->body, pad, y + sh + 6, W - 2 * pad, 5, c.fg, true);
			int ay = y + sh + (bh ? bh + 6 : 0);
			for (int i = 0; i < actions; i++) {
				struct n_action *a = visible_action(n, i);
				int lw = 0;
				pd_text_size(cr, font, a->label, &lw, NULL);
				n->action_boxes[i] = (struct pbox){ pad, ay + i * 20, lw + 4, 20 };
				pd_text(cr, font, a->label, pad, ay + i * 20, W - 2 * pad, 20, c.accent,
					PD_LEFT);
				pd_rect(cr, pad, ay + i * 20 + 16, lw, 1, c.accent);
			}
			n->body_box = (struct pbox){ 0, top, W, content - top };
		}
		return H;
	}

	int pad = 16, header = 36;
	bool big = n->image || n->image_path;
	int tx = pad + (big ? 60 : 0), tw = W - tx - pad;
	int y = header + 2;
	int sh = pd_text_wrapped(cr, bold, n->summary, tx, y, tw, 2, c.fg, false);
	int bh = pd_text_wrapped(cr, font, n->body, tx, y + sh + 2, tw, 4, c.dim, false);
	int text_bottom = y + sh + (bh ? bh + 2 : 0);
	int bottom = big && y + 48 > text_bottom ? y + 48 : text_bottom;
	int H = bottom + 16 + (actions ? 32 + 12 : 0);
	if (draw) {
		cairo_new_path(cr);
		pd_rounded(cr, 0.5, 0.5, W - 1, H - 1, c.radius);
		pd_color(cr, c.bg);
		cairo_fill_preserve(cr);
		pd_color(cr, c.border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_icon(cr, notification_icon(n, 16, true), pad, 12, 16);
		pd_text(cr, font, group_title(n), pad + 24, 8, W - pad - 72, 24, c.dim, PD_LEFT);
		n->close_box = (struct pbox){ W - 40, 6, 32, 28 };
		if (n->hover) {
			if (toast_hovered(n, n->close_box)) {
				cairo_new_path(cr);
				pd_rounded(cr, n->close_box.x, n->close_box.y, 32, 28, c.radius ? 4 : 0);
				pd_color(cr, c.button_hover);
				cairo_fill(cr);
			}
			draw_x(cr, n->close_box, c.fg, 9);
		}
		if (big) {
			pd_icon(cr, notification_icon(n, 48, false), pad, y, 48);
		}
		pd_text_wrapped(cr, bold, n->summary, tx, y, tw, 2, c.fg, true);
		pd_text_wrapped(cr, font, n->body, tx, y + sh + 2, tw, 4, c.dim, true);
		n->body_box = (struct pbox){ 0, 0, W, bottom + 16 };
		int bw = actions ? (W - 2 * pad - (actions - 1) * 8) / actions : 0;
		for (int i = 0; i < actions; i++) {
			struct pbox b = { pad + i * (bw + 8), bottom + 12, bw, 32 };
			n->action_boxes[i] = b;
			cairo_new_path(cr);
			pd_rounded(cr, b.x, b.y, b.width, b.height, c.radius ? 4 : 0);
			pd_color(cr, toast_hovered(n, b) ? c.button_hover : c.button);
			cairo_fill(cr);
			pd_text(cr, font, visible_action(n, i)->label, b.x + 4, b.y, b.width - 8, b.height,
				c.fg, PD_CENTER);
		}
	}
	return H;
}

static int toast_width(void) {
	return balloon_style() ? BALLOON_WIDTH : TOAST_WIDTH;
}

static int toast_height(struct notification *n) {
	return toast_layout(n, popup_scratch_cairo(), toast_width(), false);
}

static void toast_render(struct psurface *s, cairo_t *cr) {
	struct notification *n = s->data;
	toast_layout(n, cr, s->width, true);
}

static void defer(enum op_kind kind, uint32_t id, int action);

static void toast_timeout(void *data) {
	struct notification *n = data;
	n->timer = NULL;
	defer(OP_HIDE, n->id, -1);
}

static int toast_ms(struct notification *n) {
	if (n->urgency >= 2 || n->timeout == 0) {
		return 0; // stays until closed
	}
	if (n->timeout < 0) {
		return TOAST_MS;
	}
	return n->timeout < 1000 ? 1000 : n->timeout > 60000 ? 60000 : n->timeout;
}

static void toast_start_timer(struct notification *n, int ms) {
	if (n->timer) {
		loop_remove_timer(nt.panel->loop, n->timer);
		n->timer = NULL;
	}
	if (ms > 0) {
		n->timer = loop_add_timer(nt.panel->loop, ms, toast_timeout, n);
	}
}

static void toast_motion(struct psurface *s, double x, double y) {
	struct notification *n = s->data;
	n->px = x;
	n->py = y;
	if (!n->hover) {
		n->hover = true;
		toast_start_timer(n, 0);
	}
	psurface_set_dirty(s);
}

static void toast_leave(struct psurface *s) {
	struct notification *n = s->data;
	n->hover = false;
	if (toast_ms(n) > 0) {
		toast_start_timer(n, TOAST_HOVER_MS);
	}
	psurface_set_dirty(s);
}

static void toast_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	struct notification *n = s->data;
	if (!pressed) {
		return;
	}
	if (button != BTN_LEFT || pbox_contains(&n->close_box, x, y)) {
		defer(OP_HIDE, n->id, -1);
		return;
	}
	for (int i = 0; i < n->action_count; i++) {
		if (pbox_contains(&n->action_boxes[i], x, y)) {
			defer(OP_ACTION, n->id, i);
			return;
		}
	}
	if (pbox_contains(&n->body_box, x, y)) {
		defer(OP_ACTIVATE, n->id, -1);
	}
}

static void toast_closed(struct psurface *s) {
	struct notification *n = s->data;
	if (n->toast == s) {
		n->toast = NULL;
	}
	psurface_destroy(s);
	toasts_update();
}

static const struct psurface_impl toast_impl = {
	.render = toast_render,
	.pointer_motion = toast_motion,
	.pointer_leave = toast_leave,
	.pointer_button = toast_button,
	.closed = toast_closed,
};

/* Newest at the taskbar, older ones pushed away from it. */
static void toasts_restack(void) {
	bool bottom = bar_bottom();
	int offset = 0;
	for (int i = nt.items->length - 1; i >= 0; i--) {
		struct notification *n = nt.items->items[i];
		if (!n->toast) {
			continue;
		}
		struct psurface *s = n->toast;
		int bar = s->output && s->output->bar ? s->output->bar->height : 0;
		int margin = bar + GAP + offset;
		bool balloon = balloon_style();
		zwlr_layer_surface_v1_set_anchor(s->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			(bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
		zwlr_layer_surface_v1_set_margin(s->layer_surface, bottom ? 0 : margin,
			balloon ? 4 : GAP, bottom ? margin : 0, 0);
		wl_surface_commit(s->surface);
		offset += s->req_height + GAP;
	}
}

static void toast_show(struct notification *n) {
	struct panel *panel = nt.panel;
	struct panel_output *output = panel_focused_output(panel);
	if (!output || !panel->compositor) {
		return;
	}
	struct psurface *s = psurface_create(panel, output, &toast_impl, n,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-notification");
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	psurface_set_size(s, toast_width(), toast_height(n));
	n->toast = s;
	toasts_restack();
	toast_start_timer(n, toast_ms(n));
}

/* Shows waiting notifications when there is room, oldest first. */
static void toasts_update(void) {
	if (!nt.items) {
		return;
	}
	int max = balloon_style() ? 1 : MAX_TOASTS;
	int shown = 0;
	for (int i = 0; i < nt.items->length; i++) {
		shown += ((struct notification *)nt.items->items[i])->toast != NULL;
	}
	for (int i = 0; i < nt.items->length && shown < max; i++) {
		struct notification *n = nt.items->items[i];
		if (n->pending && !n->toast) {
			n->pending = false;
			toast_show(n);
			shown++;
		}
	}
	toasts_restack();
}

static void toast_hide(struct notification *n) {
	toast_destroy(n);
	n->pending = false;
	if (n->transient) {
		remove_item(n, CLOSED_EXPIRED);
		return;
	}
	toasts_update();
}

static void run_ops(void *data) {
	nt.ops_timer = NULL;
	struct op ops[16];
	int count = nt.op_count;
	memcpy(ops, nt.ops, sizeof(ops));
	nt.op_count = 0;
	for (int i = 0; i < count; i++) {
		struct notification *n = find(ops[i].id);
		if (!n) {
			continue;
		}
		switch (ops[i].kind) {
		case OP_HIDE:
			toast_hide(n);
			break;
		case OP_ACTIVATE:
			activate(n);
			break;
		case OP_ACTION:
			invoke_action(n, ops[i].action);
			break;
		}
	}
}

/* Pop-ups must not be destroyed inside their own input handlers. */
static void defer(enum op_kind kind, uint32_t id, int action) {
	if (nt.op_count < (int)(sizeof(nt.ops) / sizeof(nt.ops[0]))) {
		nt.ops[nt.op_count++] = (struct op){ kind, id, action };
	}
	if (!nt.ops_timer) {
		nt.ops_timer = loop_add_timer(nt.panel->loop, 0, run_ops, NULL);
	}
}

void notify_theme_changed(struct panel *panel) {
	for (int i = 0; nt.items && i < nt.items->length; i++) {
		struct notification *n = nt.items->items[i];
		if (n->toast) {
			psurface_set_size(n->toast, toast_width(), toast_height(n));
			psurface_set_dirty(n->toast);
		}
	}
	toasts_update();
}

/* ---------- adding ---------- */

static void trim_history(void) {
	int count = 0;
	for (int i = nt.items->length - 1; i >= 0; i--) {
		struct notification *n = nt.items->items[i];
		if (++count > MAX_HISTORY && !n->toast) {
			list_del(nt.items, i);
			emit_closed(n, CLOSED_EXPIRED);
			notification_free(n);
		}
	}
}

/* Takes the fields over; replaces the notification with that id if it exists. */
static uint32_t add_notification(uint32_t replaces, struct notification *fields) {
	struct notification *n = replaces ? find(replaces) : NULL;
	if (n && !n->stale) {
		free(n->app_name);
		free(n->app_icon);
		free(n->desktop_entry);
		free(n->image_path);
		free(n->summary);
		free(n->body);
		free_actions(n->actions);
		if (n->image) {
			cairo_surface_destroy(n->image);
		}
		n->app_name = fields->app_name;
		n->app_icon = fields->app_icon;
		n->desktop_entry = fields->desktop_entry;
		n->image_path = fields->image_path;
		n->summary = fields->summary;
		n->body = fields->body;
		n->actions = fields->actions;
		n->image = fields->image;
		n->urgency = fields->urgency;
		n->timeout = fields->timeout;
		n->transient = fields->transient;
		n->time = time(NULL);
		free(fields);
		if (n->toast) {
			psurface_set_size(n->toast, toast_width(), toast_height(n));
			psurface_set_dirty(n->toast);
			toast_start_timer(n, n->hover ? 0 : toast_ms(n));
		} else if (!nt.dnd || n->urgency >= 2) {
			n->pending = true;
		}
	} else {
		n = fields;
		n->id = ++nt.next_id;
		if (nt.next_id == 0) {
			n->id = nt.next_id = 1;
		}
		n->time = time(NULL);
		n->pending = !nt.dnd || n->urgency >= 2;
		if (!n->transient && !popup_is_open(nt.panel, POPUP_NOTIFICATIONS)) {
			nt.unread++;
		}
		list_add(nt.items, n);
		trim_history();
	}
	if (popup_is_open(nt.panel, POPUP_NOTIFICATIONS)) {
		n->pending = false; // the Action Center shows it already
	}
	uint32_t id = n->id;
	toasts_update();
	save_later();
	center_changed();
	panel_set_dirty(nt.panel);
	return id;
}

#if HAVE_TRAY

static cairo_surface_t *image_from_data(int width, int height, int rowstride, bool alpha,
		int bits, int channels, const uint8_t *data, size_t len) {
	if (width <= 0 || height <= 0 || width > 2048 || height > 2048 || bits != 8 ||
			(channels != 3 && channels != 4) || rowstride < width * channels ||
			len < (size_t)((height - 1) * rowstride + width * channels)) {
		return NULL;
	}
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	unsigned char *dst = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const uint8_t *p = data + y * rowstride + x * channels;
			uint32_t a = alpha && channels == 4 ? p[3] : 255;
			uint32_t pixel = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 |
				(p[2] * a / 255);
			memcpy(dst + y * stride + x * 4, &pixel, 4);
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

static int read_hints(sd_bus_message *m, struct notification *n) {
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
			return r;
		}
		char type;
		const char *contents;
		if ((r = sd_bus_message_peek_type(m, &type, &contents)) < 0) {
			return r;
		}
		if (strcmp(key, "urgency") == 0 && strcmp(contents, "y") == 0) {
			r = sd_bus_message_read(m, "v", "y", &n->urgency);
		} else if ((strcmp(key, "image-path") == 0 || strcmp(key, "image_path") == 0) &&
				strcmp(contents, "s") == 0) {
			const char *value;
			r = sd_bus_message_read(m, "v", "s", &value);
			if (r >= 0) {
				free(n->image_path);
				n->image_path = nonempty_dup(value);
			}
		} else if (strcmp(key, "desktop-entry") == 0 && strcmp(contents, "s") == 0) {
			const char *value;
			r = sd_bus_message_read(m, "v", "s", &value);
			if (r >= 0) {
				free(n->desktop_entry);
				n->desktop_entry = nonempty_dup(value);
			}
		} else if (strcmp(key, "transient") == 0 && strcmp(contents, "b") == 0) {
			int value;
			r = sd_bus_message_read(m, "v", "b", &value);
			n->transient = r >= 0 && value;
		} else if ((strcmp(key, "image-data") == 0 || strcmp(key, "image_data") == 0 ||
				strcmp(key, "icon_data") == 0) && strcmp(contents, "(iiibiiay)") == 0) {
			if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0 ||
					(r = sd_bus_message_enter_container(m, 'r', "iiibiiay")) < 0) {
				return r;
			}
			int32_t width, height, rowstride, bits, channels;
			int alpha;
			const void *data;
			size_t len;
			if ((r = sd_bus_message_read(m, "iiibii", &width, &height, &rowstride, &alpha,
					&bits, &channels)) < 0 ||
					(r = sd_bus_message_read_array(m, 'y', &data, &len)) < 0) {
				return r;
			}
			if (!n->image) {
				n->image = image_from_data(width, height, rowstride, alpha, bits, channels,
					data, len);
			}
			if ((r = sd_bus_message_exit_container(m)) < 0 ||
					(r = sd_bus_message_exit_container(m)) < 0) {
				return r;
			}
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) {
			return r;
		}
	}
	if (r < 0) {
		return r;
	}
	return sd_bus_message_exit_container(m);
}

static int method_notify(sd_bus_message *m, void *userdata, sd_bus_error *error) {
	const char *app_name, *app_icon, *summary, *body;
	uint32_t replaces;
	int r = sd_bus_message_read(m, "susss", &app_name, &replaces, &app_icon, &summary, &body);
	if (r < 0) {
		return r;
	}
	char **actions = NULL;
	if ((r = sd_bus_message_read_strv(m, &actions)) < 0) {
		return r;
	}
	struct notification *n = calloc(1, sizeof(*n));
	n->urgency = 1;
	n->app_name = nonempty_dup(app_name);
	n->app_icon = nonempty_dup(app_icon);
	n->summary = plain_text(summary);
	n->body = plain_text(body);
	n->actions = create_list();
	for (int i = 0; actions && actions[i] && actions[i + 1]; i += 2) {
		struct n_action *a = calloc(1, sizeof(*a));
		a->key = strdup(actions[i]);
		a->label = plain_text(actions[i + 1]);
		list_add(n->actions, a);
	}
	for (int i = 0; actions && actions[i]; i++) {
		free(actions[i]);
	}
	free(actions);
	int32_t timeout = -1;
	if ((r = read_hints(m, n)) < 0 || (r = sd_bus_message_read(m, "i", &timeout)) < 0) {
		notification_free(n);
		return r;
	}
	n->timeout = timeout;
	uint32_t id = add_notification(replaces, n);
	return sd_bus_reply_method_return(m, "u", id);
}

static int method_close(sd_bus_message *m, void *userdata, sd_bus_error *error) {
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0) {
		return r;
	}
	struct notification *n = find(id);
	if (n && !n->stale) {
		remove_item(n, CLOSED_BY_APP);
	}
	return sd_bus_reply_method_return(m, "");
}

static int method_capabilities(sd_bus_message *m, void *userdata, sd_bus_error *error) {
	static const char *const caps[] = { "body", "actions", "icon-static", "persistence", NULL };
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0) {
		r = sd_bus_message_append_strv(reply, (char **)caps);
	}
	if (r >= 0) {
		r = sd_bus_send(NULL, reply, NULL);
	}
	sd_bus_message_unref(reply);
	return r;
}

static int method_server_information(sd_bus_message *m, void *userdata, sd_bus_error *error) {
	return sd_bus_reply_method_return(m, "ssss", "tileWin", "tileWin", SWAY_VERSION, "1.2");
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", method_capabilities,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", method_server_information,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END,
};

static void bus_in(int fd, short mask, void *data) {
	int r;
	while ((r = sd_bus_process(nt.bus, NULL)) > 0) {
		// keep processing
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Notifications: bus error: %s", strerror(-r));
		loop_remove_fd(nt.panel->loop, nt.bus_fd);
		sd_bus_flush_close_unref(nt.bus);
		nt.bus = NULL;
		nt.bus_fd = -1;
	}
}

static void bus_init(void) {
	sd_bus *bus = NULL;
	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		sway_log(SWAY_INFO, "Notifications: no user bus: %s", strerror(-r));
		return;
	}
	r = sd_bus_add_object_vtable(bus, NULL, BUS_PATH, BUS_NAME, vtable, NULL);
	if (r >= 0) {
		r = sd_bus_request_name(bus, BUS_NAME, SD_BUS_NAME_QUEUE);
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Notifications: can't provide %s: %s", BUS_NAME, strerror(-r));
		sd_bus_flush_close_unref(bus);
		return;
	} else if (r == 0) {
		sway_log(SWAY_INFO, "Notifications: another notification service is running, "
			"waiting for it to quit");
	}
	nt.bus = bus;
	nt.bus_fd = sd_bus_get_fd(bus);
	loop_add_fd(nt.panel->loop, nt.bus_fd, POLLIN, bus_in, NULL);
	bus_in(nt.bus_fd, 0, NULL);
}

#endif

/* ---------- Action Center ---------- */

enum crow_kind {
	CROW_GROUP,
	CROW_ITEM,
};

struct crow {
	enum crow_kind kind;
	uint32_t id;
	char *group;
	struct pbox box, close;
	struct pbox actions[MAX_ACTIONS];
	int action_count;
};

struct center {
	struct panel *panel;
	struct popup *popup;
	struct popup_anchor anchor;
	bool side; // Windows 10: the whole height at the screen edge
	double px, py;
	bool inside;
	int scroll, content;
	list_t *rows; // struct crow *
	struct pbox clear_all, dnd, list;
};

static struct center *center_get(void) {
	struct popup *p = nt.panel ? nt.panel->popup : NULL;
	return p && p->kind == POPUP_NOTIFICATIONS ? p->data : NULL;
}

static bool center_hovered(struct center *c, struct pbox b) {
	return c->inside && pbox_contains(&b, c->px, c->py);
}

static void rows_free(struct center *c) {
	for (int i = 0; c->rows && i < c->rows->length; i++) {
		struct crow *r = c->rows->items[i];
		free(r->group);
		free(r);
	}
	list_free(c->rows);
	c->rows = NULL;
}

#define ITEM_PAD 10

static int item_height(struct notification *n, cairo_t *cr, const struct fly_style *st, int w) {
	int sh = pd_text_wrapped(cr, st->bold, n->summary, 0, 0, w - 64, 2, st->fg, false);
	int bh = pd_text_wrapped(cr, st->font, n->body, 0, 0, w, 3, st->fg, false);
	if (sh < 18) {
		sh = 18;
	}
	int actions = visible_action_count(n);
	return ITEM_PAD + sh + (bh ? bh + 2 : 0) + (actions ? 36 : 0) + ITEM_PAD;
}

/* The notification list from y; draws and records rows when draw is set. */
static int center_list(struct center *c, cairo_t *cr, const struct fly_style *st, int x,
		int y0, int w, bool draw) {
	if (draw) {
		rows_free(c);
		c->rows = create_list();
	}
	int y = y0;
	list_t *groups = create_list();
	for (int i = nt.items->length - 1; i >= 0; i--) {
		struct notification *n = nt.items->items[i];
		bool seen = false;
		for (int k = 0; k < groups->length && !seen; k++) {
			seen = strcmp(groups->items[k], group_key(n)) == 0;
		}
		if (!seen) {
			list_add(groups, (char *)group_key(n));
		}
	}
	bool visible_list = pbox_contains(&c->list, c->px, c->py);
	for (int g = 0; g < groups->length; g++) {
		const char *key = groups->items[g];
		struct notification *newest = NULL;
		for (int i = nt.items->length - 1; i >= 0 && !newest; i--) {
			struct notification *n = nt.items->items[i];
			if (strcmp(group_key(n), key) == 0) {
				newest = n;
			}
		}
		if (draw) {
			struct crow *r = calloc(1, sizeof(*r));
			r->kind = CROW_GROUP;
			r->group = strdup(key);
			r->box = (struct pbox){ x - 8, y, w + 16, 34 };
			r->close = (struct pbox){ x + w - 26, y + 5, 26, 24 };
			list_add(c->rows, r);
			pd_icon(cr, notification_icon(newest, 16, true), x, y + 9, 16);
			pd_text(cr, st->bold, group_title(newest), x + 24, y, w - 60, 34, st->fg, PD_LEFT);
			if (visible_list && center_hovered(c, r->box)) {
				if (center_hovered(c, r->close)) {
					fill_hover(cr, st, r->close);
				}
				draw_x(cr, r->close, st->fg, 8);
			}
		}
		y += 34;
		for (int i = nt.items->length - 1; i >= 0; i--) {
			struct notification *n = nt.items->items[i];
			if (strcmp(group_key(n), key) != 0) {
				continue;
			}
			int h = item_height(n, cr, st, w);
			if (draw) {
				struct crow *r = calloc(1, sizeof(*r));
				r->kind = CROW_ITEM;
				r->id = n->id;
				r->box = (struct pbox){ x - 8, y, w + 16, h };
				r->close = (struct pbox){ x + w - 26, y + 6, 26, 24 };
				list_add(c->rows, r);
				bool hover = visible_list && center_hovered(c, r->box);
				if (hover) {
					fill_hover(cr, st, r->box);
				}
				int sh = pd_text_wrapped(cr, st->bold, n->summary, x, y + ITEM_PAD, w - 64, 2,
					st->fg, true);
				if (sh < 18) {
					sh = 18;
				}
				int bh = pd_text_wrapped(cr, st->font, n->body, x, y + ITEM_PAD + sh + 2, w, 3,
					st->dim, true);
				if (hover) {
					if (center_hovered(c, r->close)) {
						fill_hover(cr, st, r->close);
					}
					draw_x(cr, r->close, st->fg, 8);
				} else {
					char when[32];
					format_time(n->time, when, sizeof(when));
					pd_text(cr, st->font, when, x + w - 64, y + ITEM_PAD, 64, 18, st->dim,
						PD_RIGHT);
				}
				int actions = visible_action_count(n);
				int by = y + ITEM_PAD + sh + (bh ? bh + 2 : 0) + 6;
				int bw = actions ? (w - (actions - 1) * 8) / actions : 0;
				for (int k = 0; k < actions; k++) {
					struct pbox b = { x + k * (bw + 8), by, bw, 28 };
					r->actions[k] = b;
					draw_button(cr, st, b, visible_action(n, k)->label, false,
						visible_list && center_hovered(c, b));
				}
				r->action_count = actions;
			}
			y += h + 4;
		}
		y += 8;
	}
	list_free(groups);
	return y - y0;
}

static void center_geometry(struct center *c, int *x, int *y, int *width, int *height) {
	struct panel *panel = c->panel;
	struct panel_output *o = c->anchor.output;
	int M = popup_shadow_margin(panel);
	int bar = bar_height(panel);
	*width = CENTER_WIDTH + 2 * M;
	if (c->side) {
		*x = o->width - *width;
		*y = bar_bottom() ? 0 : bar;
		*height = o->height - bar;
		return;
	}
	struct fly_style st;
	fly_style_init(&st, panel);
	int content = center_list(c, popup_scratch_cairo(), &st, 0, 0,
		CENTER_WIDTH - 2 * CENTER_PAD, false);
	int max = o->height - bar - 24;
	*height = CENTER_HEADER + CENTER_FOOTER + (content > 0 ? content : CENTER_EMPTY) + 2 * M;
	if (*height > max) {
		*height = max;
	}
	*x = c->anchor.right_align ? c->anchor.x - *width + M : c->anchor.x - M;
	*y = c->anchor.above ? c->anchor.y - *height : c->anchor.y;
}

static void center_render(struct popup *p, cairo_t *cr) {
	struct center *c = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x = M + CENTER_PAD, w = W - 2 * M - 2 * CENTER_PAD;
	pd_text(cr, st.big, "Notifications", x, M + 8, w - 90, CENTER_HEADER - 16, st.fg, PD_LEFT);
	c->clear_all = (struct pbox){ 0 };
	if (nt.items->length > 0) {
		int tw = 0;
		pd_text_size(cr, st.font, "Clear all", &tw, NULL);
		c->clear_all = (struct pbox){ x + w - tw - 12, M + 14, tw + 12, 28 };
		if (center_hovered(c, c->clear_all)) {
			fill_hover(cr, &st, c->clear_all);
		}
		pd_text(cr, st.font, "Clear all", c->clear_all.x, c->clear_all.y, c->clear_all.width,
			c->clear_all.height, st.style == PS_CLASSIC ? 0x0000ffff : st.accent, PD_CENTER);
	}

	int top = M + CENTER_HEADER, bottom = H - M - CENTER_FOOTER;
	c->list = (struct pbox){ M, top, W - 2 * M, bottom - top };
	c->content = center_list(c, cr, &st, x, 0, w, false);
	int max_scroll = c->content - c->list.height;
	if (c->scroll > max_scroll) {
		c->scroll = max_scroll;
	}
	if (c->scroll < 0) {
		c->scroll = 0;
	}
	cairo_save(cr);
	cairo_rectangle(cr, c->list.x, c->list.y, c->list.width, c->list.height);
	cairo_clip(cr);
	center_list(c, cr, &st, x, top - c->scroll, w, true);
	cairo_restore(cr);
	if (nt.items->length == 0) {
		pd_text(cr, st.font, "No new notifications", x, top, w,
			c->list.height < CENTER_EMPTY ? c->list.height : CENTER_EMPTY, st.dim, PD_CENTER);
	}

	draw_line(cr, &st, p, bottom);
	c->dnd = (struct pbox){ M, bottom + 1, W - 2 * M, CENTER_FOOTER - 1 };
	if (center_hovered(c, c->dnd)) {
		fill_hover(cr, &st, c->dnd);
	}
	pd_text(cr, st.font, "Do not disturb", x, bottom, w - 56, CENTER_FOOTER, st.fg, PD_LEFT);
	draw_switch(cr, &st, x + w - 40, bottom + (CENTER_FOOTER - 20) / 2, nt.dnd);
}

static void center_changed(void) {
	struct center *c = center_get();
	if (!c) {
		return;
	}
	if (!c->side) {
		int x, y, width, height;
		center_geometry(c, &x, &y, &width, &height);
		if (height != c->popup->height || y != c->popup->y) {
			popup_move_resize(c->popup, x, y, width, height);
		}
	}
	popup_set_dirty(c->popup);
}

static void center_motion(struct popup *p, double x, double y) {
	struct center *c = p->data;
	c->px = x;
	c->py = y;
	c->inside = true;
	popup_set_dirty(p);
}

static void center_leave(struct popup *p) {
	struct center *c = p->data;
	c->inside = false;
	popup_set_dirty(p);
}

static void center_axis(struct popup *p, double x, double y, int direction) {
	struct center *c = p->data;
	c->scroll += direction < 0 ? -48 : 48;
	popup_set_dirty(p);
}

static void center_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct center *c = p->data;
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	} else if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
		c->scroll += sym == XKB_KEY_Up ? -48 : 48;
		popup_set_dirty(p);
	}
}

static void center_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct center *c = p->data;
	if (!pressed || button != BTN_LEFT) {
		return;
	}
	if (c->clear_all.width > 0 && pbox_contains(&c->clear_all, x, y)) {
		clear_all();
		return;
	}
	if (pbox_contains(&c->dnd, x, y)) {
		notify_set_dnd(p->panel, !nt.dnd);
		return;
	}
	if (!pbox_contains(&c->list, x, y)) {
		return;
	}
	// copy the row: the rows are rebuilt when the list changes
	bool found = false, close = false;
	enum crow_kind kind = CROW_ITEM;
	uint32_t id = 0;
	char *group = NULL;
	int action = -1;
	for (int i = 0; c->rows && i < c->rows->length && !found; i++) {
		struct crow *r = c->rows->items[i];
		if (!pbox_contains(&r->box, x, y)) {
			continue;
		}
		found = true;
		kind = r->kind;
		id = r->id;
		group = r->group ? strdup(r->group) : NULL;
		close = pbox_contains(&r->close, x, y);
		for (int k = 0; k < r->action_count; k++) {
			if (pbox_contains(&r->actions[k], x, y)) {
				action = k;
			}
		}
	}
	if (!found) {
		return;
	}
	if (kind == CROW_GROUP) {
		if (close && group) {
			clear_group(group);
		}
	} else {
		struct notification *n = find(id);
		if (n && close) {
			remove_item(n, CLOSED_DISMISSED);
		} else if (n && action >= 0) {
			invoke_action(n, action);
		} else if (n) {
			activate(n);
			popup_close_later(p->panel);
		}
	}
	free(group);
}

static void center_destroy(struct popup *p) {
	struct center *c = p->data;
	rows_free(c);
	free(c);
}

static const struct popup_vtable center_vtable = {
	.render = center_render,
	.motion = center_motion,
	.leave = center_leave,
	.button = center_button,
	.axis = center_axis,
	.key = center_key,
	.destroy = center_destroy,
};

static void center_open(struct panel *panel, struct popup_anchor anchor) {
	if (!anchor.output || !nt.items) {
		return;
	}
	struct center *c = calloc(1, sizeof(*c));
	c->panel = panel;
	c->anchor = anchor;
	const char *kind = tw_theme_str(panel->theme, "notifications.center", NULL);
	c->side = kind ? strcmp(kind, "side") == 0 : panel_style(panel) == PS_FLAT;
	int x, y, width, height;
	center_geometry(c, &x, &y, &width, &height);
	c->popup = popup_create(panel, POPUP_NOTIFICATIONS, NULL, anchor.output, x, y, width,
		height, &center_vtable, c);
	if (!c->popup) {
		free(c);
		return;
	}
	// everything shows up in the list now
	for (int i = 0; i < nt.items->length; i++) {
		struct notification *n = nt.items->items[i];
		n->pending = false;
		toast_destroy(n);
	}
	nt.unread = 0;
	panel_set_dirty(panel);
}

void notify_handle_command(struct panel *panel, int argc, char **argv) {
	if (!nt.items || argc < 1) {
		return;
	}
	const char *action = argc > 1 ? argv[1] : "toggle";
	if (strcmp(argv[0], "dnd") == 0) {
		bool on = strcmp(action, "on") == 0 ? true :
			strcmp(action, "off") == 0 ? false : !nt.dnd;
		notify_set_dnd(panel, on);
		return;
	}
	bool open = popup_is_open(panel, POPUP_NOTIFICATIONS);
	if (strcmp(action, "clear") == 0) {
		clear_all();
	} else if (strcmp(action, "close") == 0 || (strcmp(action, "toggle") == 0 && open)) {
		if (open) {
			popup_close_all(panel);
		}
	} else if (!open) {
		struct panel_output *output = panel_focused_output(panel);
		if (output) {
			center_open(panel, flyout_anchor(panel, output));
		}
	}
}

/* ---------- taskbar button ---------- */

static int bell_size(struct render_ctx *ctx) {
	return ctx->height < 32 ? 16 : 18;
}

static bool bell_shown(struct widget *w, struct render_ctx *ctx) {
	bool modern = ctx->style == PSV_FLAT || ctx->style == PSV_FLUENT;
	return widget_conf_bool(w, "always", modern) || nt.unread > 0 || nt.dnd;
}

static int bell_measure(struct widget *w, struct render_ctx *ctx) {
	if (!bell_shown(w, ctx)) {
		return 0;
	}
	int pad = tw_theme_int(ctx->panel->theme, "panel.item_padding", 6);
	return bell_size(ctx) + 2 * pad + (nt.unread > 0 ? 6 : 0);
}

void notify_draw_bell(cairo_t *cr, double x, double y, double s, uint32_t color, bool dnd) {
	double cx = x + s / 2;
	cairo_new_path(cr);
	cairo_move_to(cr, x + s * 0.16, y + s * 0.74);
	cairo_curve_to(cr, x + s * 0.26, y + s * 0.64, x + s * 0.26, y + s * 0.56,
		x + s * 0.26, y + s * 0.44);
	cairo_curve_to(cr, x + s * 0.26, y + s * 0.24, x + s * 0.38, y + s * 0.12, cx, y + s * 0.12);
	cairo_curve_to(cr, x + s * 0.62, y + s * 0.12, x + s * 0.74, y + s * 0.24,
		x + s * 0.74, y + s * 0.44);
	cairo_curve_to(cr, x + s * 0.74, y + s * 0.56, x + s * 0.74, y + s * 0.64,
		x + s * 0.84, y + s * 0.74);
	cairo_close_path(cr);
	pd_color(cr, color);
	cairo_set_line_width(cr, s >= 18 ? 1.5 : 1.3);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, y + s * 0.8, s * 0.1, 0, M_PI);
	cairo_stroke(cr);
	if (dnd) {
		cairo_new_path(cr);
		cairo_move_to(cr, x + s * 0.08, y + s * 0.08);
		cairo_line_to(cr, x + s * 0.92, y + s * 0.92);
		cairo_stroke(cr);
	}
}

static void bell_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	if (b.width <= 0) {
		return;
	}
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, popup_is_open(ctx->panel, POPUP_NOTIFICATIONS),
			render_hover(ctx, b), render_pressed(ctx, b));
	}
	int size = bell_size(ctx);
	int pad = tw_theme_int(ctx->panel->theme, "panel.item_padding", 6);
	double gx = b.x + pad, gy = b.y + (b.height - size) / 2.0;
	notify_draw_bell(ctx->cairo, gx, gy, size, widget_fg(ctx->panel, "notifications"), nt.dnd);
	if (nt.unread > 0) {
		char number[8];
		snprintf(number, sizeof(number), nt.unread > 9 ? "9+" : "%d", nt.unread);
		const char *bold = bar_bold_font(ctx->panel);
		const char *space = strrchr(bold, ' ');
		char font[128];
		snprintf(font, sizeof(font), "%.*s 7", space ? (int)(space - bold) : (int)strlen(bold),
			bold);
		double r = 7, cx = gx + size, cy = gy + 3;
		cairo_new_path(ctx->cairo);
		cairo_arc(ctx->cairo, cx, cy, r, 0, 2 * M_PI);
		pd_color(ctx->cairo, tw_theme_color(ctx->panel->theme, "notifications.badge",
			tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x0078d4ff)));
		cairo_fill(ctx->cairo);
		pd_text(ctx->cairo, font, number, cx - r - 2, cy - r, 2 * r + 4, 2 * r, 0xffffffff,
			PD_CENTER);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool bell_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	if (popup_is_open(w->panel, POPUP_NOTIFICATIONS)) {
		popup_close_all(w->panel);
		return true;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	center_open(w->panel, anchor);
	return true;
}

static char *bell_tooltip(struct widget *w, struct hotspot *hs) {
	char text[96];
	if (nt.unread > 0) {
		snprintf(text, sizeof(text), nt.unread == 1 ? "1 new notification" :
			"%d new notifications", nt.unread);
	} else {
		snprintf(text, sizeof(text), "No new notifications");
	}
	if (nt.dnd) {
		strncat(text, "\nDo not disturb is on", sizeof(text) - strlen(text) - 1);
	}
	return strdup(text);
}

const struct widget_impl widget_notifications = {
	.type = "notifications",
	.measure = bell_measure,
	.render = bell_render,
	.click = bell_click,
	.tooltip = bell_tooltip,
};

/* ---------- start and stop ---------- */

void notify_init(struct panel *panel) {
	nt.panel = panel;
	nt.items = create_list();
	char *path = state_file("do-not-disturb");
	char *value = path ? tw_read_first_line(path) : NULL;
	nt.dnd = value && strcmp(value, "on") == 0;
	free(value);
	free(path);
	load_history();
#if HAVE_TRAY
	bus_init();
#endif
}

void notify_fini(struct panel *panel) {
	if (!nt.items) {
		return;
	}
	if (nt.save_timer) {
		loop_remove_timer(panel->loop, nt.save_timer);
		save_now(NULL);
	}
	if (nt.ops_timer) {
		loop_remove_timer(panel->loop, nt.ops_timer);
		nt.ops_timer = NULL;
	}
#if HAVE_TRAY
	if (nt.bus) {
		loop_remove_fd(panel->loop, nt.bus_fd);
		sd_bus_flush_close_unref(nt.bus);
		nt.bus = NULL;
	}
#endif
	for (int i = 0; i < nt.items->length; i++) {
		notification_free(nt.items->items[i]);
	}
	list_free(nt.items);
	nt.items = NULL;
}
