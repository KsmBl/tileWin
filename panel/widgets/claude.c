/*
 * The claude widget: the usage limits of a Claude Code account, the 5-hour and
 * the weekly window, on the taskbar and on the desktop.
 *
 *   widget claude {
 *       account ~/.claude                 # a Claude Code folder; the one with the
 *                                         # newest values by default
 *       show usage_5h reset_5h usage_7d   # what, in this order (tw_claude_items)
 *   }
 *
 * The values come from tilewin-claude-usage, which Claude Code runs as its status
 * line once the folder is connected: Claude Code itself shows them, tileWin never
 * touches a login of Claude's. A folder that is not connected says so, and a
 * click connects it. On the desktop "show" of its desktop_widgets entry counts
 * (more by default), and the style "list" gives each item a line of its own.
 *
 * What it shows:
 *   usage   the share of the window used, with a bar
 *   reset   the time until the window starts over
 *   left    the time until it is used up at the pace since it started, or that
 *           it lasts until it resets
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "draw.h"
#include "panel.h"
#include "tw_claude_usage.h"
#include "tw_theme.h"

#define RELOAD_MS 5000
#define MAX_ITEMS 12
#define BAR_W 34
#define GAP 12

struct claude_state {
	struct tw_claude_usage usage;
	bool have; // values written down at least once
	struct loop_timer *timer;
};

/* ---------- the values ---------- */

/* The folder to show: the one set, else the one Claude Code showed values for last. */
static char *chosen_dir(struct widget *w) {
	const char *set = widget_conf(w, "account", NULL);
	if (set && *set) {
		return tw_claude_normalize_dir(set);
	}
	list_t *all = tw_claude_usage_all();
	char *dir = all->length > 0 ?
		strdup(((struct tw_claude_usage *)all->items[0])->dir) : tw_claude_default_dir();
	tw_claude_usage_list_free(all);
	return dir;
}

static void reload(struct widget *w) {
	struct claude_state *c = w->data;
	tw_claude_usage_finish(&c->usage);
	char *dir = chosen_dir(w);
	c->have = dir && tw_claude_usage_read(dir, &c->usage);
	if (!c->usage.dir && dir) {
		c->usage.dir = strdup(dir);
	}
	free(dir);
}

static void tick(void *data) {
	struct widget *w = data;
	struct claude_state *c = w->data;
	c->timer = NULL;
	if (!w->active) {
		return;
	}
	reload(w);
	widget_set_dirty(w); // the times count down also without new values
	c->timer = loop_add_timer_lazy(w->panel->loop, RELOAD_MS, tick, w);
}

/* ---------- what to show ---------- */

static int show_list(struct widget *w, const struct tw_claude_item **items) {
	const char *show = NULL;
	char joined[512] = "";
	if (w->desk) {
		// the desktop has its own list, more by default, apart from the taskbar's
		struct twconf_node *node = twconf_child(w->desk, "show");
		for (int i = 0; node && i < node->argc; i++) {
			strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
			strncat(joined, node->argv[i], sizeof(joined) - strlen(joined) - 1);
		}
		show = *joined ? joined : TW_CLAUDE_SHOW_DESKTOP;
	} else {
		show = widget_conf(w, "show", TW_CLAUDE_SHOW_TASKBAR);
	}
	char *copy = strdup(show), *save = NULL;
	int count = 0;
	for (char *id = strtok_r(copy, " ,\t", &save); id && count < MAX_ITEMS;
			id = strtok_r(NULL, " ,\t", &save)) {
		const struct tw_claude_item *item = tw_claude_item_find(id);
		if (item) {
			items[count++] = item;
		}
	}
	free(copy);
	return count;
}

static const struct tw_claude_window *window_of(struct claude_state *c,
		const struct tw_claude_item *item, long *length) {
	*length = item->seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	return item->seven_day ? &c->usage.seven_day : &c->usage.five_hour;
}

/*
 * The value of an item as text, brief for the taskbar; the share for a bar,
 * -1 when it has none.
 */
static double item_value(struct claude_state *c, const struct tw_claude_item *item, bool brief,
		char *out, size_t size) {
	long length;
	const struct tw_claude_window *win = window_of(c, item, &length);
	time_t now = time(NULL);
	double used = c->have ? tw_claude_window_now(win, now) : -1;
	if (used < 0) {
		snprintf(out, size, "–");
		return item->kind == TW_CLAUDE_USAGE ? 0 : -1;
	}
	char span[48];
	switch (item->kind) {
	case TW_CLAUDE_USAGE:
		snprintf(out, size, "%.0f%%", used);
		return used;
	case TW_CLAUDE_RESET:
		if (win->resets_at <= now) {
			snprintf(out, size, brief ? "reset" : "has reset");
		} else {
			tw_claude_format_span(win->resets_at - now, brief, span, sizeof(span));
			snprintf(out, size, brief ? "↻ %s" : "in %s", span);
		}
		return -1;
	case TW_CLAUDE_LEFT: {
		bool known;
		long left = tw_claude_window_left(win, length, now, &known);
		if (left < 0) {
			snprintf(out, size, brief ? "lasts" : "until it resets");
		} else if (left == 0) {
			snprintf(out, size, brief ? "used up" : "used up");
		} else {
			tw_claude_format_span(left, brief, span, sizeof(span));
			snprintf(out, size, brief ? "≈%s left" : "about %s", span);
		}
		return -1;
	}
	}
	return -1;
}

static uint32_t level_color(struct widget *w, double used, uint32_t normal) {
	if (used >= widget_conf_int(w, "critical", 95)) {
		return widget_conf_color(w, "critical_fg", 0xe04040ff);
	}
	if (used >= widget_conf_int(w, "warning", 80)) {
		return widget_conf_color(w, "warning_fg", 0xf0a030ff);
	}
	return normal;
}

/* ---------- options of each place ---------- */

/*
 * The look of each place has its own options: on the desktop only those of its
 * desktop_widgets entry count, so the taskbar's bar width or names do not
 * change the card, and the other way round.
 */
static const char *place_conf(struct widget *w, const char *key, const char *fallback) {
	if (w->desk) {
		struct twconf_node *node = twconf_child(w->desk, key);
		return node && node->argc > 0 ? node->argv[0] : fallback;
	}
	return widget_conf(w, key, fallback);
}

static bool place_bool(struct widget *w, const char *key, bool fallback) {
	const char *v = place_conf(w, key, NULL);
	if (!v) {
		return fallback;
	}
	return strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

static int place_int(struct widget *w, const char *key, int fallback, int low, int high) {
	const char *v = place_conf(w, key, NULL);
	int n = v ? atoi(v) : fallback;
	return n < low ? low : n > high ? high : n;
}

/* The color of the bars: bar_color, else the accent of the theme. */
static uint32_t bar_color(struct widget *w, uint32_t accent) {
	const char *v = place_conf(w, "bar_color", NULL);
	uint32_t color;
	if (v && tw_parse_color(v, &color)) {
		return color;
	}
	return accent;
}

static void fill_rgba(cairo_t *cr, uint32_t c, double alpha) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, alpha);
}

/*
 * How bars look in the theme: smooth, or in blocks as the progress bars of
 * Windows 95 (in a sunken field) and XP were, as the flyouts and the other
 * desktop widgets draw them.
 */
enum bar_look { BAR_SMOOTH, BAR_BLOCKS, BAR_BLOCKS_SUNKEN };

/* A bar of the share used, on its track. */
static void draw_share_bar(cairo_t *cr, double x, double y, double w, double h, double share,
		uint32_t track, double track_alpha, uint32_t fill, double radius, enum bar_look look,
		bool dark) {
	if (look != BAR_SMOOTH) {
		// as the Bar style of the other widgets draws it in these themes: a white field
		// (dark in the dark scheme), sunken in Windows 95, framed and round in XP, and
		// the share in whole blocks two apart
		if (look == BAR_BLOCKS_SUNKEN) {
			pd_rect(cr, x, y, w, h, dark ? 0x2c2c2cff : 0xffffffff);
			pd_bevel(cr, x, y, w, h, true);
		} else {
			cairo_new_path(cr);
			pd_rounded(cr, x + 0.5, y + 0.5, w - 1, h - 1, h > 8 ? 3 : 1);
			pd_color(cr, dark ? 0x0a1a40ff : 0xffffffff);
			cairo_fill_preserve(cr);
			pd_color(cr, dark ? 0x3c78e6ff : 0x686868ff);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
		double inset = h >= 10 ? 3 : 2;
		double bh = h - 2 * inset;
		double chunk = fmax(h * (look == BAR_BLOCKS_SUNKEN ? 0.55 : 0.45), 4), gap = 2;
		double bx = x + inset;
		double end = bx + (w - 2 * inset) * (share < 0 ? 0 : share > 100 ? 100 : share) / 100.0;
		while (bh > 0 && bx + chunk <= end + 0.5) {
			cairo_rectangle(cr, bx, y + inset, chunk, bh);
			if (look == BAR_BLOCKS) {
				// the glossy blocks of XP: lighter at the top
				cairo_pattern_t *g = cairo_pattern_create_linear(0, y + inset, 0, y + inset + bh);
				cairo_pattern_add_color_stop_rgb(g, 0, ((fill >> 24 & 0xff) + 255) / 510.0,
					((fill >> 16 & 0xff) + 255) / 510.0, ((fill >> 8 & 0xff) + 255) / 510.0);
				cairo_pattern_add_color_stop_rgb(g, 1, (fill >> 24 & 0xff) / 255.0,
					(fill >> 16 & 0xff) / 255.0, (fill >> 8 & 0xff) / 255.0);
				cairo_set_source(cr, g);
				cairo_fill(cr);
				cairo_pattern_destroy(g);
			} else {
				fill_rgba(cr, fill, 1);
				cairo_fill(cr);
			}
			bx += chunk + gap;
		}
		return;
	}
	double r = radius > h / 2 ? h / 2 : radius;
	pd_rounded(cr, x, y, w, h, r);
	fill_rgba(cr, track, track_alpha);
	cairo_fill(cr);
	if (share > 0) {
		double fw = w * share / 100.0;
		pd_rounded(cr, x, y, fw < 2 * r ? 2 * r : fw, h, r);
		fill_rgba(cr, fill, 1);
		cairo_fill(cr);
	}
}

/* The long name of an item, as the desktop and the flyout say it. */
static const char *long_name(const struct tw_claude_item *item, bool brief) {
	if (brief) {
		return item->label;
	}
	switch (item->kind) {
	case TW_CLAUDE_USAGE:
		return item->seven_day ? "Weekly limit" : "5-hour limit";
	case TW_CLAUDE_RESET:
		return item->seven_day ? "Weekly limit resets" : "5-hour limit resets";
	case TW_CLAUDE_LEFT:
		return item->seven_day ? "Weekly limit lasts" : "5-hour limit lasts";
	}
	return item->label;
}

/* ---------- the taskbar ---------- */

static const char *not_connected_text = "Claude: click to connect";

static bool show_connect(struct claude_state *c) {
	return !c->have && !c->usage.connected;
}

/* The taskbar look: bars yes|no, bar_width, labels short|none. */
static int taskbar_bar_width(struct widget *w) {
	return place_bool(w, "bars", true) ? place_int(w, "bar_width", BAR_W, 8, 200) : 0;
}

static bool taskbar_labels(struct widget *w) {
	const char *v = place_conf(w, "labels", "short");
	return strcmp(v, "none") != 0 && strcmp(v, "no") != 0;
}

/* The width of one item: its name, a bar for a usage, its value. */
static int item_width(struct widget *w, struct render_ctx *ctx, const struct tw_claude_item *item) {
	struct claude_state *c = w->data;
	const char *font = bar_font(ctx->panel);
	char value[64];
	double share = item_value(c, item, true, value, sizeof(value));
	int width = render_text_width(ctx, font, value);
	if (taskbar_labels(w)) {
		width += render_text_width(ctx, font, item->label) + 4;
	}
	int bar = taskbar_bar_width(w);
	if (share >= 0 && bar > 0) {
		width += bar + 4;
	}
	return width;
}

static int claude_measure(struct widget *w, struct render_ctx *ctx) {
	struct claude_state *c = w->data;
	if (show_connect(c)) {
		return render_text_width(ctx, bar_font(ctx->panel), not_connected_text) + 16;
	}
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	int width = 16;
	for (int i = 0; i < count; i++) {
		width += item_width(w, ctx, items[i]) + (i > 0 ? GAP : 0);
	}
	return count > 0 ? width : 0;
}

static void claude_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct claude_state *c = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	const char *font = bar_font(ctx->panel);
	uint32_t fg = widget_fg(ctx->panel, "claude");
	uint32_t dim = (fg & 0xffffff00) | 0xa0;
	// the color of the bars of the flyouts (navy in Windows 95), else the taskbar's accent
	uint32_t accent = bar_color(w, tw_theme_color(ctx->panel->theme, "flyout.accent",
		tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x0078d4ff)) | 0xff);
	double x = b.x + 8, end = b.x + b.width - 8;
	if (show_connect(c)) {
		pd_text(ctx->cairo, font, not_connected_text, x, b.y, end - x, b.height, dim, PD_LEFT);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
		return;
	}
	int bar = taskbar_bar_width(w);
	bool blocks = ctx->style == PSV_CLASSIC || ctx->style == PSV_LUNA;
	int bar_h = place_int(w, "bar_height", blocks ? 10 : 6, 2, 24); // blocks need room
	bool labels = taskbar_labels(w);
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	for (int i = 0; i < count && x < end; i++) {
		char value[64];
		double share = item_value(c, items[i], true, value, sizeof(value));
		if (labels) {
			int lw = render_text_width(ctx, font, items[i]->label);
			pd_text(ctx->cairo, font, items[i]->label, x, b.y, lw, b.height, dim, PD_LEFT);
			x += lw + 4;
		}
		if (share >= 0 && bar > 0) {
			enum bar_look look = ctx->style == PSV_CLASSIC ? BAR_BLOCKS_SUNKEN :
				ctx->style == PSV_LUNA ? BAR_BLOCKS : BAR_SMOOTH;
			draw_share_bar(ctx->cairo, x, b.y + (b.height - bar_h) / 2.0, bar, bar_h, share,
				fg, 0.22, level_color(w, share, accent), 1, look, false);
			x += bar + 4;
		}
		int vw = render_text_width(ctx, font, value);
		uint32_t color = share >= 0 ? level_color(w, share, fg) : fg;
		pd_text(ctx->cairo, font, value, x, b.y, end - x < vw ? end - x : vw, b.height, color,
			PD_LEFT);
		x += vw + GAP;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- the desktop: list, bars and rings ---------- */

/*
 * The desktop look, from its desktop_widgets entry: title yes|no (the account
 * at the top), updated yes|no (how old the values are), bars yes|no,
 * bar_height (pixels at a cell of 100), bar_color, text_size (pixels at a
 * cell of 100), labels long|short.
 */
struct desk_look {
	char font[224], bold[224], small[224];
	double sc, pad, text, bar_h;
	bool title, updated, bars, brief;
	uint32_t fill;
	enum bar_look look;
};

static void desk_look(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		struct desk_look *l) {
	l->sc = ctx->scale;
	l->pad = 12 * l->sc;
	int size = place_int(w, "text_size", 13, 8, 40);
	l->text = size * l->sc;
	snprintf(l->font, sizeof(l->font), "%s %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->bold, sizeof(l->bold), "%s Bold %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->small, sizeof(l->small), "%s %dpx", pal->family, (int)(l->text * 0.85 + 0.5));
	l->title = place_bool(w, "title", true);
	l->updated = place_bool(w, "updated", true);
	l->bars = place_bool(w, "bars", true);
	l->bar_h = place_int(w, "bar_height", 0, 0, 60) * l->sc; // 0: from the room there is
	l->brief = strcmp(place_conf(w, "labels", "long"), "short") == 0;
	l->fill = bar_color(w, pal->accent);
	l->look = pal->look == GADGET_CLASSIC ? BAR_BLOCKS_SUNKEN :
		pal->look == GADGET_LUNA ? BAR_BLOCKS : BAR_SMOOTH;
}

/* The account at the top; returns where the rest starts. */
static double desk_title(struct claude_state *c, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l) {
	double y = l->pad;
	if (!l->title) {
		return y;
	}
	char title[160];
	snprintf(title, sizeof(title), "Claude Code%s%s", c->usage.account ? " · " : "",
		c->usage.account ? c->usage.account : "");
	pd_text(ctx->cairo, l->bold, title, l->pad, y, ctx->width - 2 * l->pad, l->text * 1.4,
		pal->fg, PD_LEFT);
	return y + l->text * 1.8;
}

/* "As of ... ago" at the bottom; returns the room it takes. */
static double desk_foot(struct claude_state *c, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l) {
	if (!l->updated || !c->have || !c->usage.updated) {
		return 0;
	}
	double h = l->text * 1.3;
	char when[64];
	long ago = (long)(time(NULL) - c->usage.updated);
	if (ago < 90) {
		snprintf(when, sizeof(when), "As of just now");
	} else {
		char span[48];
		tw_claude_format_span(ago, false, span, sizeof(span));
		snprintf(when, sizeof(when), "As of %s ago", span);
	}
	pd_text(ctx->cairo, l->small, when, l->pad, ctx->height - l->pad - h,
		ctx->width - 2 * l->pad, h, pal->dim, PD_LEFT);
	return h;
}

/* A line for each item, a bar under each usage. */
static void desk_list(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l, double y, double bottom) {
	struct claude_state *c = w->data;
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	double row = count > 0 ? (bottom - y) / count : 0;
	for (int i = 0; i < count; i++) {
		char value[64];
		double share = item_value(c, items[i], l->brief, value, sizeof(value));
		bool bar = share >= 0 && l->bars;
		double bh = l->bar_h > 0 ? l->bar_h :
			fmax(row * 0.18, (l->look == BAR_SMOOTH ? 4 : 15) * l->sc); // blocks need room
		double line = bar ? fmin(row - bh - 2 * l->sc, row * 0.62) : row;
		pd_text(cr, l->font, long_name(items[i], l->brief), l->pad, y, ctx->width * 0.6, line,
			pal->dim, PD_LEFT);
		uint32_t color = share >= 0 ? level_color(w, share, pal->fg) : pal->fg;
		pd_text(cr, l->bold, value, l->pad, y, ctx->width - 2 * l->pad, line, color, PD_RIGHT);
		if (bar) {
			draw_share_bar(cr, l->pad, y + line, ctx->width - 2 * l->pad, bh, share, pal->track,
				(pal->track & 0xff) / 255.0, level_color(w, share, l->fill), 2 * l->sc, l->look,
				pal->dark);
		}
		y += row;
	}
}

/*
 * A big bar for each window with a usage ticked, its other ticked items (when
 * it resets, how long it lasts) in a line under it.
 */
static void desk_bars(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l, double y, double bottom) {
	struct claude_state *c = w->data;
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	int windows = 0;
	bool seven[2] = { false, false };
	for (int i = 0; i < count && windows < 2; i++) {
		if (items[i]->kind == TW_CLAUDE_USAGE) {
			seven[windows++] = items[i]->seven_day;
		}
	}
	if (windows == 0) {
		desk_list(w, ctx, pal, l, y, bottom); // nothing to make a bar of: the lines
		return;
	}
	double block = (bottom - y) / windows;
	for (int k = 0; k < windows; k++) {
		const struct tw_claude_item *usage = tw_claude_item_find(seven[k] ? "usage_7d" : "usage_5h");
		char value[64];
		double share = item_value(c, usage, false, value, sizeof(value));
		double head = l->text * 1.4;
		pd_text(cr, l->font, long_name(usage, l->brief), l->pad, y, ctx->width * 0.6, head,
			pal->dim, PD_LEFT);
		pd_text(cr, l->bold, value, l->pad, y, ctx->width - 2 * l->pad, head,
			level_color(w, share, pal->fg), PD_RIGHT);
		double bh = l->bar_h > 0 ? l->bar_h : fmax(block * 0.28, 8 * l->sc);
		if (l->bars) {
			draw_share_bar(cr, l->pad, y + head + 2 * l->sc, ctx->width - 2 * l->pad, bh, share,
				pal->track, (pal->track & 0xff) / 255.0, level_color(w, share, l->fill),
				3 * l->sc, l->look, pal->dark);
		}
		// the window's other items, after one another
		GString *more = g_string_new(NULL);
		for (int i = 0; i < count; i++) {
			if (items[i]->seven_day != seven[k] || items[i]->kind == TW_CLAUDE_USAGE) {
				continue;
			}
			char v[64];
			item_value(c, items[i], l->brief, v, sizeof(v));
			g_string_append_printf(more, "%s%s %s", more->len ? " · " : "",
				items[i]->kind == TW_CLAUDE_RESET ? "resets" : "lasts", v);
		}
		if (more->len) {
			pd_text(cr, l->small, more->str, l->pad, y + head + bh + 4 * l->sc,
				ctx->width - 2 * l->pad, l->text * 1.3, pal->dim, PD_LEFT);
		}
		g_string_free(more, TRUE);
		y += block;
	}
}

/* A ring for each window with a usage ticked, side by side, its other items under it. */
static void desk_rings(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l, double y, double bottom) {
	struct claude_state *c = w->data;
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	int windows = 0;
	bool seven[2] = { false, false };
	for (int i = 0; i < count && windows < 2; i++) {
		if (items[i]->kind == TW_CLAUDE_USAGE) {
			seven[windows++] = items[i]->seven_day;
		}
	}
	if (windows == 0) {
		desk_list(w, ctx, pal, l, y, bottom);
		return;
	}
	double col = (ctx->width - 2 * l->pad) / windows;
	for (int k = 0; k < windows; k++) {
		const struct tw_claude_item *usage = tw_claude_item_find(seven[k] ? "usage_7d" : "usage_5h");
		char value[64];
		double share = item_value(c, usage, false, value, sizeof(value));
		int extras = 0;
		for (int i = 0; i < count; i++) {
			extras += items[i]->seven_day == seven[k] && items[i]->kind != TW_CLAUDE_USAGE;
		}
		double below = l->text * 1.3 * (1 + extras);
		double room = fmin(col - 8 * l->sc, bottom - y - below);
		double r = room / 2 - 2 * l->sc;
		if (r < 8 * l->sc) {
			r = 8 * l->sc;
		}
		double cx = l->pad + col * k + col / 2, cy = y + r + 2 * l->sc;
		double thick = l->bar_h > 0 ? l->bar_h : fmax(r * 0.2, 4 * l->sc);
		cairo_set_line_width(cr, thick);
		if (l->look != BAR_SMOOTH) {
			// in blocks, as the ring of the other widgets in these themes
			int blocks = 20;
			double gap = 0.035;
			for (int b = 0; b < blocks; b++) {
				cairo_new_path(cr);
				cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2 + 2 * M_PI * b / blocks + gap,
					-M_PI / 2 + 2 * M_PI * (b + 1) / blocks - gap);
				bool on = l->bars && (b + 0.5) / blocks <= share / 100.0;
				fill_rgba(cr, on ? level_color(w, share, l->fill) : pal->track,
					on ? 1 : (pal->track & 0xff) / 255.0);
				cairo_stroke(cr);
			}
		} else {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, 0, 2 * M_PI);
			fill_rgba(cr, pal->track, (pal->track & 0xff) / 255.0);
			cairo_stroke(cr);
		}
		if (share > 0 && l->bars && l->look == BAR_SMOOTH) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2, -M_PI / 2 + 2 * M_PI * share / 100.0);
			fill_rgba(cr, level_color(w, share, l->fill), 1);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		}
		pd_text(cr, l->bold, value, cx - r, cy - l->text, 2 * r, 2 * l->text,
			level_color(w, share, pal->fg), PD_CENTER);
		double ty = cy + r + 4 * l->sc;
		pd_text(cr, l->small, long_name(usage, true), cx - col / 2, ty, col, l->text * 1.3,
			pal->dim, PD_CENTER);
		for (int i = 0; i < count; i++) {
			if (items[i]->seven_day != seven[k] || items[i]->kind == TW_CLAUDE_USAGE) {
				continue;
			}
			char v[64], line[96];
			item_value(c, items[i], false, v, sizeof(v)); // "in 2 d 23 h", "about 31 min"
			snprintf(line, sizeof(line), "%s %s",
				items[i]->kind == TW_CLAUDE_RESET ? "resets" : "lasts", v);
			ty += l->text * 1.3;
			pd_text(cr, l->small, line, cx - col / 2, ty, col, l->text * 1.3, pal->dim, PD_CENTER);
		}
	}
}

static bool claude_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	const char *style = ctx->style ? ctx->style : "";
	bool list = strcmp(style, "list") == 0, bars = strcmp(style, "bar") == 0,
		rings = strcmp(style, "ring") == 0;
	if (!list && !bars && !rings) {
		return false; // compact and tile: the taskbar look
	}
	struct claude_state *c = w->data;
	struct desk_look l;
	desk_look(w, ctx, pal, &l);
	double y = desk_title(c, ctx, pal, &l);
	if (show_connect(c)) {
		pd_text(ctx->cairo, l.font, "Not connected: click to let Claude Code show its limits "
			"here", l.pad, y, ctx->width - 2 * l.pad, 40 * l.sc, pal->dim, PD_LEFT);
		return true;
	}
	double bottom = ctx->height - l.pad - desk_foot(c, ctx, pal, &l);
	if (list) {
		desk_list(w, ctx, pal, &l, y, bottom);
	} else if (bars) {
		desk_bars(w, ctx, pal, &l, y, bottom);
	} else {
		desk_rings(w, ctx, pal, &l, y, bottom);
	}
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static void append_window(GString *t, struct claude_state *c, bool seven_day) {
	const struct tw_claude_window *win = seven_day ? &c->usage.seven_day : &c->usage.five_hour;
	long length = seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	time_t now = time(NULL);
	double used = tw_claude_window_now(win, now);
	if (used < 0) {
		return;
	}
	g_string_append_printf(t, "\n%s: %.0f%% used", seven_day ? "Weekly limit" : "5-hour limit",
		used);
	if (win->resets_at > now) {
		char span[48], when[48];
		struct tm tm;
		localtime_r(&win->resets_at, &tm);
		strftime(when, sizeof(when), win->resets_at - now < 24 * 3600 ? "%H:%M" : "%a %H:%M", &tm);
		tw_claude_format_span(win->resets_at - now, false, span, sizeof(span));
		g_string_append_printf(t, ", resets at %s (in %s)", when, span);
	}
	bool known;
	long left = tw_claude_window_left(win, length, now, &known);
	if (known && left > 0) {
		char span[48];
		tw_claude_format_span(left, false, span, sizeof(span));
		g_string_append_printf(t, "\n    at this pace used up in about %s", span);
	} else if (known && left < 0) {
		g_string_append(t, "\n    at this pace it lasts until it resets");
	}
}

static char *claude_tooltip(struct widget *w, struct hotspot *hs) {
	struct claude_state *c = w->data;
	GString *t = g_string_new("Claude Code");
	if (c->usage.account) {
		g_string_append_printf(t, " · %s", c->usage.account);
	}
	if (show_connect(c)) {
		g_string_append_printf(t, "\n%s does not show its usage limits to tileWin yet: "
			"click to connect it (tileWin goes before its status line).",
			c->usage.dir ? c->usage.dir : "This Claude Code");
		return g_string_free(t, FALSE);
	}
	if (!c->have) {
		g_string_append(t, "\nConnected: the limits come with the next answer of Claude Code.");
		return g_string_free(t, FALSE);
	}
	append_window(t, c, false);
	append_window(t, c, true);
	long ago = (long)(time(NULL) - c->usage.updated);
	char span[48];
	tw_claude_format_span(ago, false, span, sizeof(span));
	g_string_append_printf(t, "\nAs Claude Code showed them %s ago", span);
	return g_string_free(t, FALSE);
}

static bool claude_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct claude_state *c = w->data;
	if (button != BTN_LEFT) {
		return false;
	}
	if (!show_connect(c)) {
		// the limits in full, in a flyout in the look of the theme
		struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
		anchor.right_align = true;
		info_flyout_toggle(w, anchor);
		return true;
	}
	if (!c->usage.dir) {
		return false;
	}
	char *quoted = g_shell_quote(c->usage.dir);
	char *cmd = g_strdup_printf("tilewin-claude-usage connect %s", quoted);
	proc_spawn(cmd);
	g_free(cmd);
	g_free(quoted);
	return true;
}

static bool claude_sample(struct widget *w, struct widget_sample *out) {
	struct claude_state *c = w->data;
	if (!c->have) {
		return false;
	}
	time_t now = time(NULL);
	double five = tw_claude_window_now(&c->usage.five_hour, now);
	double seven = tw_claude_window_now(&c->usage.seven_day, now);
	out->percent = (int)(five >= 0 ? five : seven >= 0 ? seven : 0);
	snprintf(out->value, sizeof(out->value), "%.0f%%", five >= 0 ? five : 0);
	if (seven >= 0) {
		snprintf(out->detail, sizeof(out->detail), "Week %.0f%%", seven);
	}
	out->warning = out->percent >= widget_conf_int(w, "warning", 80);
	out->critical = out->percent >= widget_conf_int(w, "critical", 95);
	return true;
}

/* What the widget shows, for its flyout; NULL when nothing was written down yet. */
const struct tw_claude_usage *claude_widget_usage(struct widget *w) {
	struct claude_state *c = w && w->impl == &widget_claude ? w->data : NULL;
	if (c) {
		reload(w); // the newest, as the flyout opens or reads again
	}
	return c && c->usage.dir ? &c->usage : NULL;
}

/* ---------- life ---------- */

static void claude_set_active(struct widget *w, bool active) {
	struct claude_state *c = w->data;
	if (active && !c->timer) {
		tick(w);
	} else if (!active && c->timer) {
		loop_remove_timer(w->panel->loop, c->timer);
		c->timer = NULL;
	}
}

static void claude_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct claude_state));
	reload(w);
}

static void claude_destroy(struct widget *w) {
	struct claude_state *c = w->data;
	if (c->timer) {
		loop_remove_timer(w->panel->loop, c->timer);
	}
	tw_claude_usage_finish(&c->usage);
	free(c);
}

const struct widget_impl widget_claude = {
	.type = "claude",
	.init = claude_init,
	.destroy = claude_destroy,
	.measure = claude_measure,
	.render = claude_render,
	.click = claude_click,
	.tooltip = claude_tooltip,
	.set_active = claude_set_active,
	.sample = claude_sample,
	.draw_gadget = claude_draw_gadget,
};
