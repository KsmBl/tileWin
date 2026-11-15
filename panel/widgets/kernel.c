/*
 * The kernel widget: the state of the automatic kernel updates, on the taskbar
 * and on the desktop. A shield in the color of the state (green: up to date or
 * fixed live, the accent: busy, amber: a new kernel waits for the restart, red:
 * something failed, grey: off), what goes on in words and how far it is.
 *
 *   widget kernel {
 *       text full|short|none   # "Building 7.3.6 · 69%", "69%", the shield alone
 *       progress yes|no        # a bar while something goes on
 *       bar_width 40           # its width on the taskbar
 *       hide_idle yes|no       # nothing on the taskbar while all is well
 *   }
 *
 * On the desktop the styles "card" (the shield, what goes on and the rest in
 * lines), "list" (a line for each: running, live fixes, next start, last
 * check), "ring" (how far it is, the shield in the middle) and "bar" draw it
 * their own way. A click opens a flyout with all of it, Check now and Restart.
 * It reads the files tw_kernel_auto.h names, every two seconds while something
 * goes on and every fifteen otherwise.
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
#include "tw_kernel_auto.h"
#include "tw_theme.h"

#define BUSY_MS 2000
#define IDLE_MS 15000

struct kernel_state {
	struct tw_kernel_auto ka;
	struct tw_kernel_auto_summary sum;
	struct loop_timer *timer;
};

static void reload(struct widget *w) {
	struct kernel_state *k = w->data;
	tw_kernel_auto_read(&k->ka);
	tw_kernel_auto_summarize(&k->ka, time(NULL), &k->sum);
}

static bool busy(const struct tw_kernel_auto_summary *s) {
	switch (s->kind) {
	case TW_KA_CHECKING:
	case TW_KA_DOWNLOADING:
	case TW_KA_BUILDING:
	case TW_KA_FIXING:
	case TW_KA_APPLYING:
		return true;
	default:
		return false;
	}
}

static void tick(void *data) {
	struct widget *w = data;
	struct kernel_state *k = w->data;
	k->timer = NULL;
	if (!w->active) {
		return;
	}
	char before[64];
	snprintf(before, sizeof(before), "%s", k->sum.brief);
	enum tw_kernel_auto_kind kind = k->sum.kind;
	reload(w);
	if (kind != k->sum.kind || strcmp(before, k->sum.brief) != 0) {
		widget_set_dirty(w);
	}
	k->timer = loop_add_timer_lazy(w->panel->loop, busy(&k->sum) ? BUSY_MS : IDLE_MS, tick, w);
}

/* The color of the state, from the theme where it has one. */
static uint32_t kind_color(struct widget *w, enum tw_kernel_auto_kind kind, uint32_t accent) {
	switch (kind) {
	case TW_KA_OK:
	case TW_KA_FIXED:
		return widget_conf_color(w, "ok_fg", 0x2e9e4fff);
	case TW_KA_RESTART:
	case TW_KA_WAITING:
	case TW_KA_PENDING:
		return widget_conf_color(w, "warning_fg", 0xe8952aff);
	case TW_KA_ERROR:
		return widget_conf_color(w, "critical_fg", 0xd83b3bff);
	case TW_KA_OFF:
		return 0x8a8a8aff;
	default:
		return accent;
	}
}

/* ---------- the shield ---------- */

static void shield_path(cairo_t *cr, double x, double y, double s) {
	double cx = x + s / 2;
	cairo_new_path(cr);
	cairo_move_to(cr, cx, y + s * 0.04);
	cairo_curve_to(cr, cx + s * 0.18, y + s * 0.14, cx + s * 0.32, y + s * 0.16, x + s * 0.88, y + s * 0.16);
	cairo_curve_to(cr, x + s * 0.88, y + s * 0.58, x + s * 0.74, y + s * 0.82, cx, y + s * 0.97);
	cairo_curve_to(cr, x + s * 0.26, y + s * 0.82, x + s * 0.12, y + s * 0.58, x + s * 0.12, y + s * 0.16);
	cairo_curve_to(cr, cx - s * 0.32, y + s * 0.16, cx - s * 0.18, y + s * 0.14, cx, y + s * 0.04);
	cairo_close_path(cr);
}

/*
 * A shield in the color of the state with its sign in it: a tick, an arrow
 * down, dots while busy, the arrow of a restart, "!" or a bar for off. Flat
 * with a dark edge in Windows 95, with a light top elsewhere.
 */
static void draw_shield(cairo_t *cr, double x, double y, double s, enum tw_kernel_auto_kind kind,
		uint32_t color, bool flat) {
	cairo_save(cr);
	shield_path(cr, x, y, s);
	if (flat) {
		pd_color(cr, color);
		cairo_fill_preserve(cr);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + s);
		double r = (color >> 24 & 0xff) / 255.0, gr = (color >> 16 & 0xff) / 255.0,
			b = (color >> 8 & 0xff) / 255.0;
		cairo_pattern_add_color_stop_rgb(g, 0, r + (1 - r) * 0.35, gr + (1 - gr) * 0.35, b + (1 - b) * 0.35);
		cairo_pattern_add_color_stop_rgb(g, 1, r * 0.85, gr * 0.85, b * 0.85);
		cairo_set_source(cr, g);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(g);
	}
	cairo_set_source_rgba(cr, 0, 0, 0, flat ? 0.75 : 0.35);
	cairo_set_line_width(cr, fmax(1, s / 18));
	cairo_stroke(cr);

	double cx = x + s / 2, cy = y + s * 0.5, u = s / 10;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, fmax(1.4, s / 9));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_new_path(cr);
	switch (kind) {
	case TW_KA_OK:
	case TW_KA_FIXED:
		cairo_move_to(cr, cx - 2.2 * u, cy);
		cairo_line_to(cr, cx - 0.6 * u, cy + 1.7 * u);
		cairo_line_to(cr, cx + 2.4 * u, cy - 1.7 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_DOWNLOADING:
		cairo_move_to(cr, cx, cy - 2.4 * u);
		cairo_line_to(cr, cx, cy + 1.8 * u);
		cairo_move_to(cr, cx - 1.7 * u, cy + 0.3 * u);
		cairo_line_to(cr, cx, cy + 2 * u);
		cairo_line_to(cr, cx + 1.7 * u, cy + 0.3 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_RESTART:
		cairo_arc(cr, cx, cy + 0.2 * u, 2.2 * u, -M_PI * 0.35, M_PI * 1.45);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 0.9 * u, cy - 2.6 * u);
		cairo_line_to(cr, cx + 1.9 * u, cy - 1.6 * u);
		cairo_line_to(cr, cx + 0.6 * u, cy - 1.0 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_ERROR:
		cairo_move_to(cr, cx, cy - 2.4 * u);
		cairo_line_to(cr, cx, cy + 0.6 * u);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy + 2.2 * u, s / 16, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case TW_KA_OFF:
	case TW_KA_WAITING:
	case TW_KA_PENDING:
		cairo_move_to(cr, cx - 2 * u, cy);
		cairo_line_to(cr, cx + 2 * u, cy);
		cairo_stroke(cr);
		break;
	default: // busy: three dots
		for (int i = -1; i <= 1; i++) {
			cairo_new_path(cr);
			cairo_arc(cr, cx + i * 1.9 * u, cy, s / 14, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		break;
	}
	cairo_restore(cr);
}

/* ---------- options of each place ---------- */

static const char *place_conf(struct widget *w, const char *key, const char *fallback) {
	if (w->desk) {
		struct twconf_node *node = twconf_child(w->desk, key);
		if (node && node->argc > 0) {
			return node->argv[0];
		}
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

static uint32_t accent_of(struct panel *panel) {
	return tw_theme_color(panel->theme, "flyout.accent",
		tw_theme_color(panel->theme, "taskbar.indicator", 0x0078d4ff)) | 0xff;
}

/* The text on the taskbar: in full, short ("69%", "7.3.6") or none. */
static void taskbar_text(struct widget *w, char *out, size_t size) {
	struct kernel_state *k = w->data;
	const char *how = place_conf(w, "text", "full");
	if (strcmp(how, "none") == 0) {
		out[0] = '\0';
	} else if (strcmp(how, "short") == 0) {
		if (k->sum.percent >= 0) {
			snprintf(out, size, "%d%%", k->sum.percent);
		} else if (k->sum.kind == TW_KA_RESTART) {
			tw_kernel_auto_release(k->ka.next, out, size);
		} else if (busy(&k->sum)) {
			snprintf(out, size, "…");
		} else {
			out[0] = '\0';
		}
	} else {
		snprintf(out, size, "%s", k->sum.brief);
	}
}

static bool hidden(struct widget *w) {
	struct kernel_state *k = w->data;
	return place_bool(w, "hide_idle", false) &&
		(k->sum.kind == TW_KA_OK || k->sum.kind == TW_KA_OFF);
}

static int progress_width(struct widget *w) {
	struct kernel_state *k = w->data;
	return k->sum.percent >= 0 && place_bool(w, "progress", true) ?
		place_int(w, "bar_width", 40, 10, 200) : 0;
}

/* ---------- the taskbar ---------- */

static int kernel_measure(struct widget *w, struct render_ctx *ctx) {
	if (hidden(w)) {
		return 0;
	}
	char text[64];
	taskbar_text(w, text, sizeof(text));
	int icon = (int)(ctx->height * 0.55);
	int width = 16 + icon;
	if (*text) {
		width += 6 + render_text_width(ctx, bar_font(ctx->panel), text);
	}
	int bar = progress_width(w);
	if (bar > 0) {
		width += 6 + bar;
	}
	return width;
}

static void kernel_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct kernel_state *k = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	uint32_t fg = widget_fg(ctx->panel, "kernel");
	uint32_t accent = accent_of(ctx->panel);
	uint32_t color = kind_color(w, k->sum.kind, accent);
	double icon = floor(b.height * 0.55);
	double x = b.x + 8;
	draw_shield(ctx->cairo, x, b.y + (b.height - icon) / 2, icon, k->sum.kind, color,
		ctx->style == PSV_CLASSIC);
	x += icon + 6;
	char text[64];
	taskbar_text(w, text, sizeof(text));
	if (*text) {
		const char *font = bar_font(ctx->panel);
		int tw = render_text_width(ctx, font, text);
		pd_text(ctx->cairo, font, text, x, b.y, tw, b.height,
			k->sum.kind == TW_KA_ERROR ? color : fg, PD_LEFT);
		x += tw + 6;
	}
	int bar = progress_width(w);
	if (bar > 0) {
		bool blocks = ctx->style == PSV_CLASSIC || ctx->style == PSV_LUNA;
		int bh = place_int(w, "bar_height", blocks ? 10 : 6, 2, 24);
		pd_share_bar(ctx->cairo, x, b.y + (b.height - bh) / 2.0, bar, bh, k->sum.percent, fg, 0.22,
			accent, 1, ctx->style == PSV_CLASSIC ? PD_BAR_BLOCKS_SUNKEN :
			ctx->style == PSV_LUNA ? PD_BAR_BLOCKS : PD_BAR_SMOOTH, false);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- the desktop ---------- */

struct desk_look {
	char font[224], bold[224], small[224], big[224];
	double sc, pad, text;
	enum pd_bar_look look;
	uint32_t color, accent;
};

static void desk_look(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		struct desk_look *l) {
	struct kernel_state *k = w->data;
	l->sc = ctx->scale;
	l->pad = 12 * l->sc;
	l->text = place_int(w, "text_size", 13, 8, 40) * l->sc;
	snprintf(l->font, sizeof(l->font), "%s %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->bold, sizeof(l->bold), "%s Bold %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->small, sizeof(l->small), "%s %dpx", pal->family, (int)(l->text * 0.85 + 0.5));
	snprintf(l->big, sizeof(l->big), "%s Bold %dpx", pal->family, (int)(l->text * 1.25 + 0.5));
	l->look = pal->look == GADGET_CLASSIC ? PD_BAR_BLOCKS_SUNKEN :
		pal->look == GADGET_LUNA ? PD_BAR_BLOCKS : PD_BAR_SMOOTH;
	l->accent = pal->accent;
	l->color = kind_color(w, k->sum.kind, pal->accent);
}

static void desk_bar(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l, double y, double h, int percent, uint32_t fill) {
	pd_share_bar(ctx->cairo, l->pad, y, ctx->width - 2 * l->pad, h, percent, pal->track,
		(pal->track & 0xff) / 255.0, fill, 2 * l->sc, l->look, pal->dark);
}

/* The lines below the state: running, live fixes, next start, last check. */
static int fact_lines(struct kernel_state *k, char lines[4][2][96]) {
	int n = 0;
	char rel[64];
	time_t now = time(NULL);
	if (!k->ka.configured || !k->ka.enabled) {
		return 0;
	}
	tw_kernel_auto_release(k->ka.running, rel, sizeof(rel));
	snprintf(lines[n][0], 96, "Running");
	snprintf(lines[n++][1], 96, "%s%s", rel[0] ? "Linux " : "–", rel);
	snprintf(lines[n][0], 96, "Live fixes");
	if (k->ka.fixes[0]) {
		snprintf(lines[n++][1], 96, "Linux %s", k->ka.fixes);
	} else {
		snprintf(lines[n++][1], 96, k->ka.running_auto ? "none needed" : "–");
	}
	tw_kernel_auto_release(k->ka.next, rel, sizeof(rel));
	snprintf(lines[n][0], 96, "Next start");
	snprintf(lines[n++][1], 96, "%s%s", rel[0] ? "Linux " : "the same", rel);
	snprintf(lines[n][0], 96, "Checked");
	if (k->ka.checked) {
		tw_kernel_auto_ago(k->ka.checked, now, lines[n++][1], 96);
	} else {
		snprintf(lines[n++][1], 96, "not yet");
	}
	return n;
}

/* The shield, what goes on, a sentence more, a bar while busy, and the facts. */
static void desk_card(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l, bool facts) {
	struct kernel_state *k = w->data;
	cairo_t *cr = ctx->cairo;
	double icon = fmin(44 * l->sc, ctx->height * 0.32);
	double y = l->pad;
	draw_shield(cr, l->pad, y, icon, k->sum.kind, l->color, pal->look == GADGET_CLASSIC);
	double tx = l->pad + icon + 10 * l->sc, tw = ctx->width - tx - l->pad;
	pd_text(cr, l->bold, k->sum.title, tx, y, tw, icon * 0.55, pal->fg, PD_LEFT);
	char sub[96];
	if (k->sum.percent >= 0) {
		snprintf(sub, sizeof(sub), "%d%%", k->sum.percent);
	} else {
		snprintf(sub, sizeof(sub), "%s", k->sum.brief);
	}
	pd_text(cr, l->small, sub, tx, y + icon * 0.55, tw, icon * 0.45, pal->dim, PD_LEFT);
	y += icon + 8 * l->sc;
	if (k->sum.percent >= 0) {
		double bh = fmax(l->look == PD_BAR_SMOOTH ? 6 * l->sc : 14 * l->sc, 4);
		desk_bar(ctx, pal, l, y, bh, k->sum.percent, l->accent);
		y += bh + 8 * l->sc;
	}
	double bottom = ctx->height - l->pad;
	char lines[4][2][96];
	int n = facts ? fact_lines(k, lines) : 0;
	double line = l->text * 1.35;
	double facts_h = n * line;
	if (y + facts_h > bottom) {
		n = (int)((bottom - y) / line);
		facts_h = n * line;
	}
	double detail_h = bottom - y - facts_h - (n > 0 ? 6 * l->sc : 0);
	if (detail_h > l->text) {
		int max_lines = (int)(detail_h / (l->text * 1.2));
		pd_text_wrapped(cr, l->small, k->sum.detail, l->pad, y, ctx->width - 2 * l->pad,
			max_lines > 0 ? max_lines : 1, pal->dim, true);
	}
	y = bottom - facts_h;
	for (int i = 0; i < n; i++) {
		pd_text(cr, l->small, lines[i][0], l->pad, y, ctx->width * 0.45, line, pal->dim, PD_LEFT);
		pd_text(cr, l->small, lines[i][1], l->pad, y, ctx->width - 2 * l->pad, line, pal->fg,
			PD_RIGHT);
		y += line;
	}
}

/* A line for the state and one for each fact. */
static void desk_list(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l) {
	struct kernel_state *k = w->data;
	cairo_t *cr = ctx->cairo;
	double y = l->pad, row = l->text * 1.6;
	double icon = l->text * 1.3;
	draw_shield(cr, l->pad, y + (row - icon) / 2, icon, k->sum.kind, l->color,
		pal->look == GADGET_CLASSIC);
	pd_text(cr, l->bold, k->sum.brief, l->pad + icon + 6 * l->sc, y,
		ctx->width - 2 * l->pad - icon, row, pal->fg, PD_LEFT);
	y += row;
	if (k->sum.percent >= 0) {
		double bh = fmax(l->look == PD_BAR_SMOOTH ? 5 * l->sc : 12 * l->sc, 4);
		desk_bar(ctx, pal, l, y, bh, k->sum.percent, l->accent);
		y += bh + 4 * l->sc;
	}
	char lines[4][2][96];
	int n = fact_lines(k, lines);
	double left = ctx->height - l->pad - y;
	double line = n > 0 ? fmin(left / n, row) : row;
	for (int i = 0; i < n && line >= l->text; i++) {
		pd_text(cr, l->font, lines[i][0], l->pad, y, ctx->width * 0.5, line, pal->dim, PD_LEFT);
		pd_text(cr, l->font, lines[i][1], l->pad, y, ctx->width - 2 * l->pad, line, pal->fg,
			PD_RIGHT);
		y += line;
	}
}

/* A ring of how far it is (whole when nothing goes on), the shield in it. */
static void desk_ring(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l) {
	struct kernel_state *k = w->data;
	cairo_t *cr = ctx->cairo;
	double below = l->text * 1.5;
	double r = fmin(ctx->width - 2 * l->pad, ctx->height - 2 * l->pad - below) / 2;
	double cx = ctx->width / 2, cy = l->pad + r;
	double thick = fmax(r * 0.16, 3 * l->sc);
	double share = k->sum.percent >= 0 ? k->sum.percent : busy(&k->sum) ? 0 : 100;
	cairo_set_line_width(cr, thick);
	if (l->look != PD_BAR_SMOOTH) {
		int blocks = 20;
		for (int i = 0; i < blocks; i++) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2 + 2 * M_PI * i / blocks + 0.035,
				-M_PI / 2 + 2 * M_PI * (i + 1) / blocks - 0.035);
			bool on = (i + 0.5) / blocks <= share / 100.0;
			uint32_t c = on ? (k->sum.percent >= 0 ? l->accent : l->color) : pal->track;
			cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
				(c >> 8 & 0xff) / 255.0, on ? 1 : (pal->track & 0xff) / 255.0);
			cairo_stroke(cr);
		}
	} else {
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r - thick / 2, 0, 2 * M_PI);
		uint32_t t = pal->track;
		cairo_set_source_rgba(cr, (t >> 24 & 0xff) / 255.0, (t >> 16 & 0xff) / 255.0,
			(t >> 8 & 0xff) / 255.0, (t & 0xff) / 255.0);
		cairo_stroke(cr);
		if (share > 0) {
			uint32_t c = k->sum.percent >= 0 ? l->accent : l->color;
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2, -M_PI / 2 + 2 * M_PI * share / 100.0);
			pd_color(cr, c);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		}
	}
	double icon = r * 0.95;
	draw_shield(cr, cx - icon / 2, cy - icon / 2, icon, k->sum.kind, l->color,
		pal->look == GADGET_CLASSIC);
	pd_text(cr, l->small, k->sum.brief, l->pad / 2, ctx->height - l->pad / 2 - below,
		ctx->width - l->pad, below, pal->fg, PD_CENTER);
}

/* A wide bar of how far it is, the state above it. */
static void desk_wide(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct desk_look *l) {
	struct kernel_state *k = w->data;
	cairo_t *cr = ctx->cairo;
	double head = l->text * 1.5;
	double icon = head * 0.9;
	double y = l->pad;
	draw_shield(cr, l->pad, y + (head - icon) / 2, icon, k->sum.kind, l->color,
		pal->look == GADGET_CLASSIC);
	pd_text(cr, l->bold, k->sum.brief, l->pad + icon + 6 * l->sc, y,
		ctx->width - 2 * l->pad - icon, head, pal->fg, PD_LEFT);
	double bh = fmax(ctx->height - 2 * l->pad - head - 6 * l->sc, 4);
	bh = fmin(bh, l->look == PD_BAR_SMOOTH ? 10 * l->sc : 18 * l->sc);
	// how far it is in the accent; when nothing goes on, whole in the color of the state
	int share = k->sum.percent >= 0 ? k->sum.percent : busy(&k->sum) ? 0 : 100;
	desk_bar(ctx, pal, l, y + head + 4 * l->sc, bh, share,
		k->sum.percent >= 0 ? l->accent : l->color);
}

static bool kernel_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	const char *style = ctx->style ? ctx->style : "";
	struct desk_look l;
	if (strcmp(style, "card") == 0) {
		desk_look(w, ctx, pal, &l);
		desk_card(w, ctx, pal, &l, place_bool(w, "facts", true));
	} else if (strcmp(style, "list") == 0) {
		desk_look(w, ctx, pal, &l);
		desk_list(w, ctx, pal, &l);
	} else if (strcmp(style, "ring") == 0) {
		desk_look(w, ctx, pal, &l);
		desk_ring(w, ctx, pal, &l);
	} else if (strcmp(style, "bar") == 0) {
		desk_look(w, ctx, pal, &l);
		desk_wide(w, ctx, pal, &l);
	} else {
		return false; // compact and tile: the taskbar look
	}
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static char *kernel_tooltip(struct widget *w, struct hotspot *hs) {
	struct kernel_state *k = w->data;
	reload(w);
	GString *t = g_string_new(k->sum.title);
	if (k->sum.percent >= 0) {
		g_string_append_printf(t, " (%d%%)", k->sum.percent);
	}
	g_string_append_printf(t, "\n%s", k->sum.detail);
	char lines[4][2][96];
	int n = fact_lines(k, lines);
	for (int i = 0; i < n; i++) {
		g_string_append_printf(t, "\n%s: %s", lines[i][0], lines[i][1]);
	}
	return g_string_free(t, FALSE);
}

static bool kernel_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
	return true;
}

static bool kernel_sample(struct widget *w, struct widget_sample *out) {
	struct kernel_state *k = w->data;
	out->percent = k->sum.percent >= 0 ? k->sum.percent : busy(&k->sum) ? 0 : 100;
	snprintf(out->value, sizeof(out->value), "%s", k->sum.brief);
	snprintf(out->detail, sizeof(out->detail), "%s", k->sum.title);
	out->warning = k->sum.kind == TW_KA_RESTART || k->sum.kind == TW_KA_WAITING;
	out->critical = k->sum.kind == TW_KA_ERROR;
	return true;
}

/* What the widget shows, for its flyout: read again now. */
const struct tw_kernel_auto *kernel_widget_state(struct widget *w,
		struct tw_kernel_auto_summary *sum) {
	struct kernel_state *k = w && w->impl == &widget_kernel ? w->data : NULL;
	if (!k) {
		return NULL;
	}
	reload(w);
	if (sum) {
		*sum = k->sum;
	}
	return &k->ka;
}

/* ---------- life ---------- */

static void kernel_set_active(struct widget *w, bool active) {
	struct kernel_state *k = w->data;
	if (active && !k->timer) {
		tick(w);
	} else if (!active && k->timer) {
		loop_remove_timer(w->panel->loop, k->timer);
		k->timer = NULL;
	}
}

static void kernel_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct kernel_state));
	reload(w);
}

static void kernel_destroy(struct widget *w) {
	struct kernel_state *k = w->data;
	if (k->timer) {
		loop_remove_timer(w->panel->loop, k->timer);
	}
	free(k);
}

const struct widget_impl widget_kernel = {
	.type = "kernel",
	.init = kernel_init,
	.destroy = kernel_destroy,
	.measure = kernel_measure,
	.render = kernel_render,
	.click = kernel_click,
	.tooltip = kernel_tooltip,
	.set_active = kernel_set_active,
	.sample = kernel_sample,
	.draw_gadget = kernel_draw_gadget,
};
