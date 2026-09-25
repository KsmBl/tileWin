#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "draw.h"
#include "panel.h"
#include "tw_widgets.h"

/*
 * The desktop styles of the widgets: a chart, a gauge, a ring and a bar for
 * what the widgets measure, and a digital, an analog and a binary clock. Each
 * of them is drawn in the look of the theme (grey and raised for Windows 95,
 * soft blue for XP, glass for 7, a colored tile for 8, flat for 10, rounded
 * for 11) and in its light or dark colors. The colors come from the
 * "desktop_widget" block of the theme, and the look decides them where the
 * theme says nothing.
 *
 * Sizes are relative to the card, so a gadget drawn over more cells of the
 * grid simply grows; ctx->scale is 1 for cells 100 pixels high.
 */

/* ---------- colors ---------- */

static uint32_t alpha(uint32_t color, uint32_t a) {
	return (color & 0xffffff00) | (a & 0xff);
}

static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		double ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

/* The tiles of the Windows 8 start screen, one per widget. */
static const uint32_t metro_tints[] = {
	0x2d89efff, 0x00a300ff, 0x9f00a7ff, 0xda532cff, 0x00aba9ff,
	0x603cbaff, 0xe3a21aff, 0xb91d47ff, 0x1e7145ff, 0x2b5797ff,
};

static uint32_t metro_tint(struct widget *w) {
	unsigned hash = 5381;
	for (const char *c = w && w->name ? w->name : ""; *c; c++) {
		hash = hash * 33 + (unsigned char)*c;
	}
	return metro_tints[hash % (sizeof(metro_tints) / sizeof(metro_tints[0]))];
}

struct look_colors {
	uint32_t bg, bg2, fg, dim, accent, accent2, border, track, grid, face;
	double radius;
};

/* light, then dark */
static const struct look_colors look_defaults[][2] = {
	[GADGET_CLASSIC] = {
		{ 0xc0c0c0ff, 0xc0c0c0ff, 0x000000ff, 0x404040ff, 0x000080ff, 0x00ff00ff,
			0x000000ff, 0x808080ff, 0x008000ff, 0x000000ff, 0 },
		{ 0x3c3c3cff, 0x3c3c3cff, 0xe0e0e0ff, 0xa0a0a0ff, 0x4a7ac0ff, 0x00ff00ff,
			0x000000ff, 0x202020ff, 0x008000ff, 0x000000ff, 0 },
	},
	[GADGET_LUNA] = {
		{ 0xfbfdffff, 0xd3e2f7ff, 0x000000ff, 0x4b5f86ff, 0x316ac5ff, 0x3ca33cff,
			0x0054e3ff, 0xc5d4ecff, 0x008040ff, 0x000000ff, 6 },
		{ 0x23407aff, 0x0f2350ff, 0xffffffff, 0xb6c8ecff, 0x7aa7f5ff, 0x5fd35fff,
			0x3c78e6ff, 0xffffff30, 0x008040ff, 0x000000ff, 6 },
	},
	[GADGET_AERO] = {
		{ 0xffffffc8, 0xdfeaf6d8, 0x1e395bff, 0x4f6b8dff, 0x2f8fe6ff, 0x36c236ff,
			0x0000005a, 0x1e395b30, 0x1e395b20, 0x0b1a2be8, 6 },
		{ 0x3a4f66b0, 0x0b1622d0, 0xffffffff, 0xc3d3e6ff, 0x6fc3ffff, 0x7de26bff,
			0x000000a0, 0xffffff30, 0xffffff20, 0x050b12e8, 6 },
	},
	[GADGET_METRO] = {
		{ 0x2d89efff, 0x2d89efff, 0xffffffff, 0xffffffb8, 0xffffffff, 0xffffffff,
			0x00000000, 0xffffff40, 0xffffff26, 0x00000030, 0 },
		{ 0x2d89efff, 0x2d89efff, 0xffffffff, 0xffffffb8, 0xffffffff, 0xffffffff,
			0x00000000, 0xffffff40, 0xffffff26, 0x00000030, 0 },
	},
	[GADGET_FLAT] = {
		{ 0xf2f2f2e6, 0xf2f2f2e6, 0x000000ff, 0x5d5d5dff, 0x0078d7ff, 0x0078d7ff,
			0x0000001f, 0x00000026, 0x00000014, 0x00000008, 0 },
		{ 0x1f1f1fe6, 0x1f1f1fe6, 0xffffffff, 0xa6a6a6ff, 0x3a96ddff, 0x3a96ddff,
			0xffffff1f, 0xffffff2a, 0xffffff14, 0xffffff08, 0 },
	},
	[GADGET_FLUENT] = {
		{ 0xf3f3f3d9, 0xf3f3f3d9, 0x1b1b1bff, 0x5f5f5fff, 0x005fb8ff, 0x0f7b0fff,
			0x0000001a, 0x0000001f, 0x00000012, 0x00000008, 8 },
		{ 0x202020d9, 0x202020d9, 0xffffffff, 0xa0a0a0ff, 0x60cdffff, 0x6ccb5fff,
			0xffffff1a, 0xffffff24, 0xffffff12, 0xffffff08, 8 },
	},
};

static enum gadget_look look_of(const struct tw_theme *t) {
	static const struct {
		const char *name;
		enum gadget_look look;
	} names[] = {
		{ "classic", GADGET_CLASSIC }, { "win95", GADGET_CLASSIC },
		{ "luna", GADGET_LUNA }, { "winxp", GADGET_LUNA },
		{ "aero", GADGET_AERO }, { "win7", GADGET_AERO },
		{ "metro", GADGET_METRO }, { "win8", GADGET_METRO },
		{ "flat", GADGET_FLAT }, { "win10", GADGET_FLAT },
		{ "fluent", GADGET_FLUENT }, { "win11", GADGET_FLUENT },
	};
	const char *name = tw_theme_str(t, "desktop_widget.look", t->style);
	for (size_t i = 0; name && i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(name, names[i].name) == 0) {
			return names[i].look;
		}
	}
	return GADGET_FLAT;
}

void gadget_palette(struct panel *panel, struct widget *w, bool card,
		struct gadget_palette *pal) {
	const struct tw_theme *t = panel->theme;
	memset(pal, 0, sizeof(*pal));
	pal->look = look_of(t);
	pal->dark = t->dark;
	const struct look_colors *d = &look_defaults[pal->look][pal->dark];
	uint32_t tint = pal->look == GADGET_METRO ? metro_tint(w) : d->bg;
	if (pal->look == GADGET_METRO && pal->dark) {
		tint = mix(tint, 0x000000ff, 0.18); // a little deeper on a dark desktop
	}
	pal->bg = tw_theme_color(t, "desktop_widget.bg", tint);
	pal->bg2 = tw_theme_color(t, "desktop_widget.bg2",
		pal->look == GADGET_METRO ? pal->bg : d->bg2);
	pal->fg = tw_theme_color(t, "desktop_widget.fg", d->fg);
	pal->dim = tw_theme_color(t, "desktop_widget.dim", d->dim);
	pal->accent = tw_theme_color(t, "desktop_widget.accent", d->accent);
	pal->accent2 = tw_theme_color(t, "desktop_widget.accent2", d->accent2);
	pal->border = tw_theme_color(t, "desktop_widget.border", d->border);
	pal->track = tw_theme_color(t, "desktop_widget.track", d->track);
	pal->grid = tw_theme_color(t, "desktop_widget.grid", d->grid);
	pal->face = tw_theme_color(t, "desktop_widget.face", d->face);
	pal->warning = tw_theme_color(t, "desktop_widget.warning",
		pal->dark || pal->look == GADGET_METRO ? 0xfbbf24ff : 0xc98a00ff);
	pal->critical = tw_theme_color(t, "desktop_widget.critical",
		pal->look == GADGET_METRO ? 0xffd0d0ff : 0xe81123ff);
	pal->radius = tw_theme_int(t, "desktop_widget.radius", (int)d->radius);
	pal->shadow = pal->look == GADGET_AERO && pal->dark;
	if (!card) {
		// straight on the wallpaper: white with a shadow, like the icon labels
		pal->fg = 0xffffffff;
		pal->dim = 0xffffffc8;
		pal->track = 0xffffff40;
		pal->grid = 0xffffff26;
		pal->shadow = true;
		if (pal->look == GADGET_METRO) {
			pal->accent = pal->accent2 = metro_tint(w);
		}
	}
	// the font of the taskbar, at the sizes the gadget asks for
	const char *font = tw_theme_str(t, "desktop_widget.font", bar_font(panel));
	snprintf(pal->family, sizeof(pal->family), "%s", font);
	char *space = strrchr(pal->family, ' ');
	if (space && strspn(space + 1, "0123456789.px") == strlen(space + 1) && space[1]) {
		*space = '\0';
	}
}

/* ---------- small helpers ---------- */

static void text(cairo_t *cr, const struct gadget_palette *pal, bool bold, double px,
		const char *str, double x, double y, double w, double h, uint32_t color,
		enum pd_align align) {
	if (!str || !*str || px < 4) {
		return;
	}
	char font[224];
	snprintf(font, sizeof(font), "%s%s %dpx", pal->family, bold ? " Bold" : "",
		(int)lround(px));
	if (pal->shadow) {
		pd_text(cr, font, str, x + 1, y + 1, w, h, 0x00000090, align);
	}
	pd_text(cr, font, str, x, y, w, h, color, align);
}

static double text_width(cairo_t *cr, const struct gadget_palette *pal, bool bold, double px,
		const char *str) {
	char font[224];
	snprintf(font, sizeof(font), "%s%s %dpx", pal->family, bold ? " Bold" : "",
		(int)lround(px));
	int w = 0;
	pd_text_size(cr, font, str, &w, NULL);
	return w;
}

/* The raised or sunken edge of Windows 95, in the colors of the theme. */
static void bevel(cairo_t *cr, const struct tw_theme *t, double x, double y, double w,
		double h, bool sunken) {
	uint32_t hi = tw_theme_color(t, "decoration.highlight", 0xffffffff);
	uint32_t light = tw_theme_color(t, "decoration.light", 0xdfdfdfff);
	uint32_t shadow = tw_theme_color(t, "decoration.shadow", 0x808080ff);
	uint32_t dark = tw_theme_color(t, "decoration.dark", 0x000000ff);
	uint32_t o_tl = sunken ? shadow : light, o_br = sunken ? hi : dark;
	uint32_t i_tl = sunken ? dark : hi, i_br = sunken ? light : shadow;
	x = floor(x);
	y = floor(y);
	w = floor(w);
	h = floor(h);
	cairo_save(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	pd_rect(cr, x, y, w - 1, 1, o_tl);
	pd_rect(cr, x, y, 1, h - 1, o_tl);
	pd_rect(cr, x + w - 1, y, 1, h, o_br);
	pd_rect(cr, x, y + h - 1, w, 1, o_br);
	pd_rect(cr, x + 1, y + 1, w - 3, 1, i_tl);
	pd_rect(cr, x + 1, y + 1, 1, h - 3, i_tl);
	pd_rect(cr, x + w - 2, y + 1, 1, h - 2, i_br);
	pd_rect(cr, x + 1, y + h - 2, w - 2, 1, i_br);
	cairo_restore(cr);
}

static void vertical_gradient(cairo_t *cr, double y, double h, uint32_t top, uint32_t bottom) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(0, y, 0, y + h);
	cairo_pattern_add_color_stop_rgba(pattern, 0, (top >> 24 & 0xff) / 255.0,
		(top >> 16 & 0xff) / 255.0, (top >> 8 & 0xff) / 255.0, (top & 0xff) / 255.0);
	cairo_pattern_add_color_stop_rgba(pattern, 1, (bottom >> 24 & 0xff) / 255.0,
		(bottom >> 16 & 0xff) / 255.0, (bottom >> 8 & 0xff) / 255.0, (bottom & 0xff) / 255.0);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
}

static void circle(cairo_t *cr, double cx, double cy, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
}

/* The color a reading is drawn in: the accent, or the warning colors. */
static uint32_t value_color(const struct gadget_palette *pal, const struct widget_sample *s,
		uint32_t normal) {
	return s->critical ? pal->critical : s->warning ? pal->warning : normal;
}

static double clamp01(double v) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* ---------- the card ---------- */

void gadget_draw_card(struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	cairo_t *cr = ctx->cairo;
	double w = ctx->width, h = ctx->height;
	double r = fmin(pal->radius, h / 2);
	switch (pal->look) {
	case GADGET_CLASSIC:
		pd_rect(cr, 0, 0, w, h, pal->bg);
		bevel(cr, ctx->panel->theme, 0, 0, w, h, ctx->pressed);
		return;
	case GADGET_LUNA:
		cairo_new_path(cr);
		pd_rounded(cr, 0.75, 0.75, w - 1.5, h - 1.5, r);
		vertical_gradient(cr, 0, h, pal->bg, pal->bg2);
		cairo_fill_preserve(cr);
		pd_color(cr, pal->border);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_new_path(cr);
		pd_rounded(cr, 2.5, 2.5, w - 5, h - 5, fmax(r - 2, 0));
		pd_color(cr, pal->dark ? 0xffffff20 : 0xffffffa0);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	case GADGET_AERO:
		cairo_new_path(cr);
		pd_rounded(cr, 0.5, 0.5, w - 1, h - 1, r);
		vertical_gradient(cr, 0, h, pal->bg, pal->bg2);
		cairo_fill_preserve(cr);
		cairo_save(cr);
		cairo_clip_preserve(cr);
		// the glossy upper half of the glass
		cairo_rectangle(cr, 0, 0, w, h * 0.48);
		vertical_gradient(cr, 0, h * 0.48, 0xffffff48, 0xffffff10);
		cairo_fill(cr);
		cairo_restore(cr);
		pd_color(cr, pal->border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_new_path(cr);
		pd_rounded(cr, 1.5, 1.5, w - 3, h - 3, fmax(r - 1, 0));
		pd_color(cr, 0xffffff58);
		cairo_stroke(cr);
		break;
	case GADGET_METRO:
		pd_rect(cr, 0, 0, w, h, pal->bg);
		break;
	case GADGET_FLAT:
		pd_rect(cr, 0, 0, w, h, pal->bg);
		cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
		pd_color(cr, pal->border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	case GADGET_FLUENT:
		cairo_new_path(cr);
		pd_rounded(cr, 0.5, 0.5, w - 1, h - 1, r);
		pd_color(cr, pal->bg);
		cairo_fill_preserve(cr);
		pd_color(cr, pal->border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	}
	if (ctx->hover) {
		cairo_new_path(cr);
		pd_rounded(cr, 0.5, 0.5, w - 1, h - 1, pal->look == GADGET_METRO ? 0 : r);
		pd_color(cr, ctx->pressed ? alpha(pal->fg, 0x18) : alpha(pal->fg, 0x0c));
		cairo_fill(cr);
	}
}

double gadget_draw_caption(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const char *caption) {
	double sc = ctx->scale, size = 12 * sc, pad = 8 * sc;
	double h = size * 1.6;
	// Windows 8 put the name of a tile at its lower left, the others center it
	text(ctx->cairo, pal, false, size, caption, pad, ctx->height - h - pad / 2,
		ctx->width - 2 * pad, h, pal->look == GADGET_METRO ? pal->fg : pal->dim,
		pal->look == GADGET_METRO ? PD_LEFT : PD_CENTER);
	return h + pad / 2;
}

/* ---------- chart ---------- */

static void draw_chart(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct widget_sample *s, const char *title) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 10 * sc;
	double header = s->detail[0] ? 38 * sc : 28 * sc;
	text(cr, pal, false, 12 * sc, title, pad, pad, w * 0.55, 16 * sc, pal->dim, PD_LEFT);
	if (s->detail[0]) {
		text(cr, pal, false, 11 * sc, s->detail, pad, pad + 16 * sc, w - 2 * pad, 16 * sc,
			pal->dim, PD_LEFT);
	}
	bool classic = pal->look == GADGET_CLASSIC || pal->look == GADGET_LUNA;
	uint32_t line = value_color(pal, s, classic ? pal->accent2 : pal->accent);
	text(cr, pal, true, 20 * sc, s->value, w * 0.4, pad - 3 * sc, w * 0.6 - pad, 26 * sc,
		value_color(pal, s, pal->look == GADGET_METRO ? pal->fg :
			classic ? pal->fg : pal->accent), PD_RIGHT);

	double cx = pad, cy = pad + header, cw = w - 2 * pad, ch = h - cy - pad;
	if (ch < 8 || cw < 8) {
		return;
	}
	// the face behind the chart
	switch (pal->look) {
	case GADGET_CLASSIC:
		pd_rect(cr, cx, cy, cw, ch, pal->face);
		bevel(cr, ctx->panel->theme, cx - 2, cy - 2, cw + 4, ch + 4, true);
		break;
	case GADGET_LUNA:
		pd_rect(cr, cx, cy, cw, ch, pal->face);
		cairo_rectangle(cr, cx - 0.5, cy - 0.5, cw + 1, ch + 1);
		pd_color(cr, pal->dark ? 0x00000080 : 0x7f9db9ff);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	case GADGET_AERO:
		cairo_new_path(cr);
		pd_rounded(cr, cx, cy, cw, ch, 3);
		pd_color(cr, pal->face);
		cairo_fill(cr);
		break;
	case GADGET_METRO:
		break;
	case GADGET_FLAT:
		pd_rect(cr, cx, cy, cw, ch, pal->face);
		break;
	case GADGET_FLUENT:
		cairo_new_path(cr);
		pd_rounded(cr, cx, cy, cw, ch, 4);
		pd_color(cr, pal->face);
		cairo_fill(cr);
		break;
	}
	cairo_save(cr);
	cairo_rectangle(cr, cx, cy, cw, ch);
	cairo_clip(cr);
	// the grid: squares like the task manager, or a few quiet lines
	cairo_set_line_width(cr, 1);
	if (classic || pal->look == GADGET_AERO) {
		uint32_t grid = pal->look == GADGET_AERO ? 0xffffff1c : pal->grid;
		double step = fmax(10 * sc, 6);
		for (double gx = cx + cw; gx > cx; gx -= step) {
			cairo_move_to(cr, floor(gx) + 0.5, cy);
			cairo_line_to(cr, floor(gx) + 0.5, cy + ch);
		}
		for (double gy = cy + ch; gy > cy; gy -= step) {
			cairo_move_to(cr, cx, floor(gy) + 0.5);
			cairo_line_to(cr, cx + cw, floor(gy) + 0.5);
		}
		pd_color(cr, grid);
		cairo_stroke(cr);
	} else {
		for (int i = 1; i < 4; i++) {
			double gy = floor(cy + ch * i / 4.0) + 0.5;
			cairo_move_to(cr, cx, gy);
			cairo_line_to(cr, cx + cw, gy);
		}
		pd_color(cr, pal->grid);
		cairo_stroke(cr);
	}
	// the measurements, oldest at the left
	double inset = classic ? 1 : 0;
	double step = cw / (WIDGET_HISTORY - 1);
	cairo_new_path(cr);
	for (int i = 0; i < WIDGET_HISTORY; i++) {
		int v = s->has_history ? s->history[i] : (i == WIDGET_HISTORY - 1 ? s->percent : 0);
		double x = cx + i * step, y = cy + ch - inset - (ch - 2 * inset) * clamp01(v / 100.0);
		if (i == 0) {
			cairo_move_to(cr, x, y);
		} else {
			cairo_line_to(cr, x, y);
		}
	}
	cairo_path_t *path = cairo_copy_path(cr);
	if (!classic) {
		cairo_line_to(cr, cx + cw, cy + ch);
		cairo_line_to(cr, cx, cy + ch);
		cairo_close_path(cr);
		if (pal->look == GADGET_AERO) {
			vertical_gradient(cr, cy, ch, alpha(line, 0x90), alpha(line, 0x10));
		} else {
			pd_color(cr, alpha(line, pal->look == GADGET_METRO ? 0x50 : 0x40));
		}
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	cairo_append_path(cr, path);
	cairo_path_destroy(path);
	pd_color(cr, line);
	cairo_set_line_width(cr, fmax(1.5 * sc, 1));
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* ---------- gauge ---------- */

#define GAUGE_START (150 * M_PI / 180)
#define GAUGE_SWEEP (240 * M_PI / 180)

static void draw_gauge(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct widget_sample *s, const char *title) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 8 * sc, caption = 18 * sc;
	double r = fmin(w - 2 * pad, h - 2 * pad - caption) / 2;
	if (r < 8) {
		return;
	}
	double cx = w / 2, cy = pad + r;
	double t = clamp01(s->percent / 100.0);
	double angle = GAUGE_START + GAUGE_SWEEP * t;
	bool dial = pal->look == GADGET_CLASSIC || pal->look == GADGET_LUNA ||
		pal->look == GADGET_AERO;
	uint32_t ink = pal->fg;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	if (dial) {
		// a round face with a rim, like the meters of the Windows 7 gadgets
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		if (pal->look == GADGET_AERO) {
			cairo_pattern_t *face = cairo_pattern_create_radial(cx, cy - r * 0.3, r * 0.1,
				cx, cy, r);
			cairo_pattern_add_color_stop_rgba(face, 0, 0.22, 0.28, 0.34, 0.95);
			cairo_pattern_add_color_stop_rgba(face, 1, (pal->face >> 24 & 0xff) / 255.0,
				(pal->face >> 16 & 0xff) / 255.0, (pal->face >> 8 & 0xff) / 255.0,
				(pal->face & 0xff) / 255.0);
			cairo_set_source(cr, face);
			cairo_pattern_destroy(face);
			ink = 0xffffffff;
		} else if (pal->look == GADGET_LUNA) {
			vertical_gradient(cr, cy - r, 2 * r, pal->dark ? 0x2a4a8aff : 0xffffffff,
				pal->dark ? 0x0a1a40ff : 0xdce7f7ff);
		} else {
			pd_color(cr, pal->dark ? 0x2c2c2cff : 0xdfdfdfff);
		}
		cairo_fill(cr);
		// the rim
		double rim = fmax(r * 0.06, 2);
		cairo_new_path(cr);
		circle(cr, cx, cy, r - rim / 2);
		cairo_set_line_width(cr, rim);
		if (pal->look == GADGET_AERO) {
			cairo_pattern_t *chrome = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
			cairo_pattern_add_color_stop_rgba(chrome, 0, 1, 1, 1, 0.95);
			cairo_pattern_add_color_stop_rgba(chrome, 0.5, 0.45, 0.5, 0.55, 0.95);
			cairo_pattern_add_color_stop_rgba(chrome, 1, 0.9, 0.92, 0.95, 0.95);
			cairo_set_source(cr, chrome);
			cairo_pattern_destroy(chrome);
			cairo_stroke(cr);
		} else if (pal->look == GADGET_LUNA) {
			pd_color(cr, pal->border);
			cairo_stroke(cr);
		} else {
			// Windows 95: light at the upper left, shadow at the lower right
			cairo_arc(cr, cx, cy, r - rim / 2, M_PI * 0.75, M_PI * 1.75);
			pd_color(cr, pal->dark ? 0x6e6e6eff : 0xffffffff);
			cairo_stroke(cr);
			cairo_arc(cr, cx, cy, r - rim / 2, M_PI * 1.75, M_PI * 2.75);
			pd_color(cr, pal->dark ? 0x000000ff : 0x808080ff);
			cairo_stroke(cr);
		}
		// the red part at the end of the scale
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r * 0.8, GAUGE_START + GAUGE_SWEEP * 0.8,
			GAUGE_START + GAUGE_SWEEP);
		cairo_set_line_width(cr, fmax(r * 0.05, 1.5));
		pd_color(cr, alpha(pal->critical, 0xc0));
		cairo_stroke(cr);
		// ticks, and the numbers when there is room for them
		for (int i = 0; i <= 20; i++) {
			double a = GAUGE_START + GAUGE_SWEEP * i / 20.0;
			double len = i % 2 == 0 ? r * 0.13 : r * 0.07;
			double outer = r * 0.86;
			cairo_move_to(cr, cx + cos(a) * outer, cy + sin(a) * outer);
			cairo_line_to(cr, cx + cos(a) * (outer - len), cy + sin(a) * (outer - len));
			cairo_set_line_width(cr, i % 2 == 0 ? fmax(r * 0.025, 1.2) : fmax(r * 0.012, 0.8));
			pd_color(cr, alpha(ink, i % 2 == 0 ? 0xff : 0xb0));
			cairo_stroke(cr);
			if (i % 4 == 0 && r >= 48) {
				char label[8];
				snprintf(label, sizeof(label), "%d", i * 5);
				double lr = outer - len - r * 0.17, size = r * 0.13;
				text(cr, pal, false, size, label, cx + cos(a) * lr - size * 1.5,
					cy + sin(a) * lr - size, size * 3, size * 2, alpha(ink, 0xd0), PD_CENTER);
			}
		}
	} else {
		// an arc that fills up, the way the flat styles draw progress
		double lw = r * (pal->look == GADGET_METRO ? 0.12 : 0.14);
		cairo_set_line_cap(cr, pal->look == GADGET_FLUENT ? CAIRO_LINE_CAP_ROUND :
			CAIRO_LINE_CAP_BUTT);
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r - lw / 2, GAUGE_START, GAUGE_START + GAUGE_SWEEP);
		cairo_set_line_width(cr, lw);
		pd_color(cr, pal->track);
		cairo_stroke(cr);
		if (t > 0) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - lw / 2, GAUGE_START, angle);
			pd_color(cr, value_color(pal, s, pal->accent));
			cairo_stroke(cr);
		}
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	}
	// the needle
	double needle = dial ? r * 0.78 : r * 0.62;
	uint32_t needle_color = pal->look == GADGET_AERO ? 0xff3a2aff :
		pal->look == GADGET_LUNA ? 0xc03030ff : pal->look == GADGET_CLASSIC ?
		(pal->dark ? 0xe0e0e0ff : 0x000000ff) : pal->fg;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, cx - cos(angle) * r * 0.12, cy - sin(angle) * r * 0.12);
	cairo_line_to(cr, cx + cos(angle) * needle, cy + sin(angle) * needle);
	cairo_set_line_width(cr, fmax(r * (dial ? 0.04 : 0.03), 1.5));
	if (pal->shadow || pal->look == GADGET_AERO) {
		cairo_save(cr);
		cairo_translate(cr, 1, 1.5);
		pd_color(cr, 0x00000070);
		cairo_stroke_preserve(cr);
		cairo_restore(cr);
	}
	pd_color(cr, needle_color);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_new_path(cr);
	circle(cr, cx, cy, fmax(r * 0.08, 2.5));
	pd_color(cr, pal->look == GADGET_AERO ? 0xd0d8e0ff : needle_color);
	cairo_fill(cr);
	// the reading in the lower part of the dial, the name under it
	double size = r * 0.24;
	text(cr, pal, true, size, s->value, cx - r, cy + r * 0.28, 2 * r, size * 1.5,
		value_color(pal, s, ink), PD_CENTER);
	text(cr, pal, false, 12 * sc, title, pad, h - pad - caption, w - 2 * pad, caption,
		pal->dim, PD_CENTER);
}

/* ---------- ring ---------- */

static void draw_ring(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct widget_sample *s, const char *title) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 10 * sc;
	double r = fmin(w, h) / 2 - pad;
	if (r < 8) {
		return;
	}
	double cx = w / 2, cy = h / 2;
	double t = clamp01(s->percent / 100.0);
	double lw = r * (pal->look == GADGET_CLASSIC ? 0.22 : pal->look == GADGET_FLUENT ? 0.13 :
		0.16);
	double rr = r - lw / 2;
	uint32_t fill = value_color(pal, s, pal->look == GADGET_LUNA ||
		pal->look == GADGET_AERO ? pal->accent2 : pal->accent);
	cairo_set_line_width(cr, lw);
	if (pal->look == GADGET_CLASSIC || pal->look == GADGET_LUNA) {
		// in blocks, like the progress bars of those days
		int blocks = 20;
		double gap = 0.035;
		for (int i = 0; i < blocks; i++) {
			double a0 = -M_PI / 2 + 2 * M_PI * i / blocks + gap;
			double a1 = -M_PI / 2 + 2 * M_PI * (i + 1) / blocks - gap;
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, rr, a0, a1);
			pd_color(cr, (i + 0.5) / blocks <= t ? fill : pal->track);
			cairo_stroke(cr);
		}
	} else {
		cairo_new_path(cr);
		circle(cr, cx, cy, rr);
		pd_color(cr, pal->track);
		cairo_stroke(cr);
		if (t > 0) {
			if (pal->look == GADGET_AERO) {
				// a glow under the glass
				cairo_new_path(cr);
				cairo_arc(cr, cx, cy, rr, -M_PI / 2, -M_PI / 2 + 2 * M_PI * t);
				cairo_set_line_width(cr, lw * 1.6);
				pd_color(cr, alpha(fill, 0x40));
				cairo_stroke(cr);
				cairo_set_line_width(cr, lw);
			}
			cairo_set_line_cap(cr, pal->look == GADGET_FLUENT ? CAIRO_LINE_CAP_ROUND :
				CAIRO_LINE_CAP_BUTT);
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, rr, -M_PI / 2, -M_PI / 2 + 2 * M_PI * t);
			pd_color(cr, fill);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		}
	}
	double inner = r - lw;
	double size = inner * 0.5;
	// a long reading ("↓ 1.2 MB/s") is made smaller until it fits in the ring
	double tw = text_width(cr, pal, true, size, s->value);
	if (tw > inner * 1.7 && tw > 0) {
		size *= inner * 1.7 / tw;
	}
	text(cr, pal, true, size, s->value, cx - inner, cy - size * 0.85, 2 * inner, size * 1.4,
		pal->fg, PD_CENTER);
	text(cr, pal, false, fmax(inner * 0.2, 9 * sc), title, cx - inner * 0.85,
		cy + size * 0.5, inner * 1.7, inner * 0.36, pal->dim, PD_CENTER);
}

/* ---------- bar ---------- */

static void draw_bar(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const struct widget_sample *s, const char *title) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 10 * sc;
	double t = clamp01(s->percent / 100.0);
	double header = 22 * sc;
	text(cr, pal, false, 12 * sc, title, pad, pad, w * 0.6 - pad, header, pal->dim, PD_LEFT);
	text(cr, pal, true, 15 * sc, s->value, w * 0.4, pad, w * 0.6 - pad, header,
		value_color(pal, s, pal->fg), PD_RIGHT);
	double bx = pad, bw = w - 2 * pad;
	double bh = pal->look == GADGET_FLUENT ? fmax(6 * sc, 3) : fmax(16 * sc, 6);
	double space = h - pad - header - pad;
	bh = fmin(bh, fmax(space, 3));
	double by = pad + header + (space - bh) / 2;
	if (s->detail[0] && space > bh + 16 * sc) {
		by = pad + header + 4 * sc;
		text(cr, pal, false, 11 * sc, s->detail, pad, by + bh + 2 * sc, bw, 16 * sc, pal->dim,
			PD_LEFT);
	}
	uint32_t fill = value_color(pal, s, pal->look == GADGET_LUNA ||
		pal->look == GADGET_AERO ? pal->accent2 : pal->accent);
	switch (pal->look) {
	case GADGET_CLASSIC: {
		pd_rect(cr, bx, by, bw, bh, pal->dark ? 0x2c2c2cff : 0xffffffff);
		bevel(cr, ctx->panel->theme, bx, by, bw, bh, true);
		double chunk = fmax(bh * 0.55, 4), gap = 2, x = bx + 3;
		double end = bx + 3 + (bw - 6) * t;
		while (x + chunk <= end + 0.5) {
			pd_rect(cr, x, by + 3, chunk, bh - 6, fill);
			x += chunk + gap;
		}
		break;
	}
	case GADGET_LUNA: {
		cairo_new_path(cr);
		pd_rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 3);
		pd_color(cr, pal->dark ? 0x0a1a40ff : 0xffffffff);
		cairo_fill_preserve(cr);
		pd_color(cr, pal->dark ? 0x3c78e6ff : 0x686868ff);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		double chunk = fmax(bh * 0.45, 4), gap = 2, x = bx + 3;
		double end = bx + 3 + (bw - 6) * t;
		while (x + chunk <= end + 0.5) {
			cairo_rectangle(cr, x, by + 3, chunk, bh - 6);
			vertical_gradient(cr, by + 3, bh - 6, mix(fill, 0xffffffff, 0.55), fill);
			cairo_fill(cr);
			x += chunk + gap;
		}
		break;
	}
	case GADGET_AERO:
		cairo_new_path(cr);
		pd_rounded(cr, bx, by, bw, bh, 2);
		vertical_gradient(cr, by, bh, pal->dark ? 0x202020d0 : 0xe6e6e6ff,
			pal->dark ? 0x101010d0 : 0xccccccff);
		cairo_fill(cr);
		if (t > 0) {
			cairo_new_path(cr);
			pd_rounded(cr, bx + 1, by + 1, (bw - 2) * t, bh - 2, 1.5);
			vertical_gradient(cr, by, bh, mix(fill, 0xffffffff, 0.35), mix(fill, 0x000000ff, 0.15));
			cairo_fill(cr);
			cairo_rectangle(cr, bx + 1, by + 1, (bw - 2) * t, (bh - 2) / 2);
			pd_color(cr, 0xffffff40);
			cairo_fill(cr);
		}
		cairo_new_path(cr);
		pd_rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 2);
		pd_color(cr, pal->dark ? 0x00000090 : 0x0000005a);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	case GADGET_METRO:
	case GADGET_FLAT:
		pd_rect(cr, bx, by, bw, bh, pal->track);
		pd_rect(cr, bx, by, bw * t, bh, fill);
		break;
	case GADGET_FLUENT:
		cairo_new_path(cr);
		pd_rounded(cr, bx, by, bw, bh, bh / 2);
		pd_color(cr, pal->track);
		cairo_fill(cr);
		if (t > 0) {
			cairo_new_path(cr);
			pd_rounded(cr, bx, by, fmax(bw * t, bh), bh, bh / 2);
			pd_color(cr, fill);
			cairo_fill(cr);
		}
		break;
	}
}

/* ---------- clocks ---------- */

static bool twelve_hours(struct widget *w) {
	const char *format = widget_conf(w, "format",
		tw_theme_str(w->panel->theme, "clock.format", "%H:%M"));
	return strstr(format, "%I") || strstr(format, "%l") || strstr(format, "%p") ||
		strstr(format, "%r");
}

static void now(struct tm *tm) {
	time_t t = time(NULL);
	localtime_r(&t, tm);
}

static void draw_digital(struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 12 * sc;
	struct tm tm;
	now(&tm);
	bool seconds = widget_conf_bool(ctx->widget, "seconds", false);
	char time_text[32], suffix[16] = "", date[96];
	bool twelve = twelve_hours(ctx->widget);
	strftime(time_text, sizeof(time_text), twelve ? "%l:%M" : "%H:%M", &tm);
	if (seconds) {
		strftime(suffix, sizeof(suffix), ":%S", &tm);
	}
	if (twelve) {
		size_t len = strlen(suffix);
		strftime(suffix + len, sizeof(suffix) - len, " %p", &tm);
	}
	strftime(date, sizeof(date), "%A, %e %B", &tm);
	const char *time_start = time_text + strspn(time_text, " ");
	double size = fmin(h * 0.42, (w - 2 * pad) / (strlen(time_start) * 0.62 +
		strlen(suffix) * 0.3 + 0.2));
	double small = size * 0.42;
	double date_size = fmax(12 * sc, h * 0.11);
	double block = size * 1.2 + date_size * 1.5;
	double y = (h - block) / 2;
	bool left = pal->look == GADGET_METRO || pal->look == GADGET_FLAT ||
		pal->look == GADGET_FLUENT;
	double tw = text_width(cr, pal, pal->look != GADGET_FLUENT, size, time_start);
	double sw = suffix[0] ? text_width(cr, pal, false, small, suffix) + 2 : 0;
	double x = left ? pad : (w - tw - sw) / 2;
	if (pal->look == GADGET_CLASSIC) {
		// in a sunken field, the way the clock of Windows 95 sat in the taskbar
		double fw = tw + sw + 16 * sc, fh = size * 1.25;
		double fx = (w - fw) / 2, fy = y - size * 0.05;
		pd_rect(cr, fx, fy, fw, fh, pal->dark ? 0x2c2c2cff : 0xffffffff);
		bevel(cr, ctx->panel->theme, fx, fy, fw, fh, true);
	}
	uint32_t color = pal->look == GADGET_LUNA && !pal->dark ? 0x0a246aff : pal->fg;
	text(cr, pal, pal->look != GADGET_FLUENT, size, time_start, x, y, tw + 4, size * 1.2,
		color, PD_LEFT);
	if (suffix[0]) {
		text(cr, pal, false, small, suffix, x + tw + 2, y + size * 0.52, sw + 4, small * 1.3,
			pal->dim, PD_LEFT);
	}
	text(cr, pal, false, date_size, date, pad, y + size * 1.2, w - 2 * pad, date_size * 1.5,
		pal->dim, left ? PD_LEFT : PD_CENTER);
}

static void hand(cairo_t *cr, double cx, double cy, double angle, double back, double length,
		double width, uint32_t color, bool shadow) {
	double dx = sin(angle), dy = -cos(angle);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, width);
	if (shadow) {
		cairo_move_to(cr, cx - dx * back + 1, cy - dy * back + 1.5);
		cairo_line_to(cr, cx + dx * length + 1, cy + dy * length + 1.5);
		pd_color(cr, 0x00000050);
		cairo_stroke(cr);
	}
	cairo_move_to(cr, cx - dx * back, cy - dy * back);
	cairo_line_to(cr, cx + dx * length, cy + dy * length);
	pd_color(cr, color);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void draw_analog(struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 8 * sc;
	double r = fmin(w, h) / 2 - pad;
	if (r < 10) {
		return;
	}
	double cx = w / 2, cy = h / 2;
	struct tm tm;
	now(&tm);
	bool seconds = widget_conf_bool(ctx->widget, "seconds", true);
	uint32_t ink = pal->fg, second_color = pal->accent;
	bool numbers = r >= 40 && pal->look != GADGET_METRO && pal->look != GADGET_FLUENT;
	switch (pal->look) {
	case GADGET_CLASSIC:
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		pd_color(cr, pal->dark ? 0x2c2c2cff : 0xdfdfdfff);
		cairo_fill(cr);
		cairo_set_line_width(cr, fmax(r * 0.05, 2));
		cairo_arc(cr, cx, cy, r, M_PI * 0.75, M_PI * 1.75);
		pd_color(cr, pal->dark ? 0x6e6e6eff : 0xffffffff);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy, r, M_PI * 1.75, M_PI * 2.75);
		pd_color(cr, pal->dark ? 0x000000ff : 0x808080ff);
		cairo_stroke(cr);
		second_color = pal->dark ? 0xff5050ff : 0x800000ff;
		break;
	case GADGET_LUNA:
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		vertical_gradient(cr, cy - r, 2 * r, pal->dark ? 0x2a4a8aff : 0xffffffff,
			pal->dark ? 0x0a1a40ff : 0xdce7f7ff);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, fmax(r * 0.07, 2));
		pd_color(cr, pal->border);
		cairo_stroke(cr);
		ink = pal->dark ? 0xffffffff : 0x0a246aff;
		second_color = 0xd03a1aff;
		break;
	case GADGET_AERO: {
		// the white clock with a chrome rim of the Windows 7 gadget
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		cairo_pattern_t *face = cairo_pattern_create_radial(cx, cy - r * 0.4, r * 0.1,
			cx, cy, r);
		if (pal->dark) {
			cairo_pattern_add_color_stop_rgba(face, 0, 0.25, 0.3, 0.36, 0.97);
			cairo_pattern_add_color_stop_rgba(face, 1, 0.04, 0.07, 0.1, 0.97);
			ink = 0xffffffff;
		} else {
			cairo_pattern_add_color_stop_rgba(face, 0, 1, 1, 1, 0.98);
			cairo_pattern_add_color_stop_rgba(face, 1, 0.86, 0.9, 0.94, 0.98);
			ink = 0x1a1a1aff;
		}
		cairo_set_source(cr, face);
		cairo_pattern_destroy(face);
		cairo_fill(cr);
		double rim = fmax(r * 0.08, 2);
		cairo_new_path(cr);
		circle(cr, cx, cy, r - rim / 2);
		cairo_pattern_t *chrome = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
		cairo_pattern_add_color_stop_rgba(chrome, 0, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(chrome, 0.5, 0.45, 0.5, 0.55, 1);
		cairo_pattern_add_color_stop_rgba(chrome, 1, 0.9, 0.92, 0.95, 1);
		cairo_set_source(cr, chrome);
		cairo_pattern_destroy(chrome);
		cairo_set_line_width(cr, rim);
		cairo_stroke(cr);
		second_color = 0xe0301eff;
		break;
	}
	case GADGET_METRO:
		ink = pal->fg;
		second_color = pal->fg;
		break;
	case GADGET_FLAT:
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		pd_color(cr, pal->face);
		cairo_fill(cr);
		break;
	case GADGET_FLUENT:
		cairo_new_path(cr);
		circle(cr, cx, cy, r);
		pd_color(cr, pal->face);
		cairo_fill_preserve(cr);
		pd_color(cr, pal->border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	}
	// the marks of the hours and minutes
	for (int i = 0; i < 60; i++) {
		bool hour = i % 5 == 0;
		if (!hour && (r < 30 || pal->look == GADGET_METRO)) {
			continue;
		}
		double a = i * M_PI / 30;
		double outer = r * (pal->look == GADGET_AERO || pal->look == GADGET_LUNA ? 0.86 : 0.92);
		if (pal->look == GADGET_METRO || pal->look == GADGET_FLUENT) {
			// dots, the flat way
			cairo_new_path(cr);
			circle(cr, cx + sin(a) * outer * 0.96, cy - cos(a) * outer * 0.96,
				fmax(r * (i % 15 == 0 ? 0.035 : 0.022), 1.2));
			pd_color(cr, alpha(ink, i % 15 == 0 ? 0xff : 0xa0));
			cairo_fill(cr);
			continue;
		}
		double len = hour ? r * 0.12 : r * 0.05;
		cairo_move_to(cr, cx + sin(a) * outer, cy - cos(a) * outer);
		cairo_line_to(cr, cx + sin(a) * (outer - len), cy - cos(a) * (outer - len));
		cairo_set_line_width(cr, hour ? fmax(r * 0.03, 1.5) : fmax(r * 0.012, 0.8));
		pd_color(cr, alpha(ink, hour ? 0xff : 0x90));
		cairo_stroke(cr);
	}
	if (numbers) {
		for (int i = 1; i <= 12; i++) {
			if (r < 60 && i % 3 != 0) {
				continue;
			}
			char label[4];
			snprintf(label, sizeof(label), "%d", i);
			double a = i * M_PI / 6, lr = r * 0.62, size = r * 0.16;
			text(cr, pal, pal->look != GADGET_CLASSIC, size, label,
				cx + sin(a) * lr - size * 1.5, cy - cos(a) * lr - size, size * 3, size * 2,
				ink, PD_CENTER);
		}
	}
	double minutes = tm.tm_min + tm.tm_sec / 60.0;
	double hours = tm.tm_hour % 12 + minutes / 60.0;
	bool shadow = pal->look == GADGET_AERO || pal->shadow;
	bool thin = pal->look == GADGET_FLAT || pal->look == GADGET_FLUENT ||
		pal->look == GADGET_METRO;
	hand(cr, cx, cy, hours * M_PI / 6, r * 0.1, r * 0.5, fmax(r * (thin ? 0.05 : 0.07), 2),
		ink, shadow);
	hand(cr, cx, cy, minutes * M_PI / 30, r * 0.1, r * 0.75, fmax(r * (thin ? 0.035 : 0.05), 1.5),
		ink, shadow);
	if (seconds) {
		hand(cr, cx, cy, tm.tm_sec * M_PI / 30, r * 0.18, r * 0.82, fmax(r * 0.018, 1),
			second_color, shadow);
	}
	cairo_new_path(cr);
	circle(cr, cx, cy, fmax(r * 0.05, 2.5));
	pd_color(cr, seconds ? second_color : ink);
	cairo_fill(cr);
}

static void draw_binary(struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, w = ctx->width, h = ctx->height;
	double pad = 10 * sc;
	struct tm tm;
	now(&tm);
	bool seconds = widget_conf_bool(ctx->widget, "seconds", true);
	int hour = tm.tm_hour;
	if (twelve_hours(ctx->widget)) {
		hour = hour % 12 ? hour % 12 : 12;
	}
	int digits[6] = { hour / 10, hour % 10, tm.tm_min / 10, tm.tm_min % 10,
		tm.tm_sec / 10, tm.tm_sec % 10 };
	int bits[6] = { 2, 4, 3, 4, 3, 4 }; // the tens never need more
	int columns = seconds ? 6 : 4;
	double label = 16 * sc;
	double area_x = pad, area_w = w - 2 * pad;
	double area_y = pad, area_h = h - 2 * pad - label;
	if (pal->look == GADGET_CLASSIC) {
		// LEDs on a black panel, sunk into the grey
		pd_rect(cr, area_x, area_y, area_w, area_h, pal->face);
		bevel(cr, ctx->panel->theme, area_x - 2, area_y - 2, area_w + 4, area_h + 4, true);
		area_x += 4;
		area_w -= 8;
		area_y += 4;
		area_h -= 8;
	}
	// pairs of columns, a little apart from each other
	double group_gap = area_w * 0.06;
	double col_w = (area_w - group_gap * (double)(columns / 2 - 1)) / columns;
	double row_h = area_h / 4;
	double d = fmin(col_w, row_h) * 0.72;
	bool round = pal->look == GADGET_AERO || pal->look == GADGET_LUNA ||
		pal->look == GADGET_FLUENT;
	uint32_t on = pal->look == GADGET_CLASSIC ? 0xff2020ff : pal->look == GADGET_LUNA ?
		pal->accent2 : pal->accent;
	uint32_t off = pal->look == GADGET_CLASSIC ? 0x400000ff : pal->track;
	for (int c = 0; c < columns; c++) {
		double x = area_x + c * col_w + (double)(c / 2) * group_gap + col_w / 2;
		for (int b = 0; b < bits[c]; b++) {
			double y = area_y + (3 - b) * row_h + row_h / 2;
			bool lit = digits[c] & (1 << b);
			cairo_new_path(cr);
			if (round) {
				circle(cr, x, y, d / 2);
			} else {
				cairo_rectangle(cr, x - d / 2, y - d / 2, d, d);
			}
			if (lit && pal->look == GADGET_AERO) {
				cairo_pattern_t *glow = cairo_pattern_create_radial(x - d * 0.15, y - d * 0.15,
					d * 0.05, x, y, d / 2);
				cairo_pattern_add_color_stop_rgba(glow, 0, 1, 1, 1, 1);
				cairo_pattern_add_color_stop_rgba(glow, 1, (on >> 24 & 0xff) / 255.0,
					(on >> 16 & 0xff) / 255.0, (on >> 8 & 0xff) / 255.0, 1);
				cairo_set_source(cr, glow);
				cairo_pattern_destroy(glow);
			} else {
				pd_color(cr, lit ? on : off);
			}
			cairo_fill(cr);
		}
		char digit[4];
		snprintf(digit, sizeof(digit), "%d", digits[c]);
		text(cr, pal, false, 11 * sc, digit, x - col_w / 2, h - pad - label, col_w, label,
			pal->dim, PD_CENTER);
	}
}

/* ---------- entry point ---------- */

static const char *title_of(struct widget *w) {
	const struct tw_widget_info *info = tw_widget_find(w->name);
	const char *colon = strchr(w->name, ':');
	if (info && strcmp(info->type, "custom") == 0 && colon) {
		return colon + 1;
	}
	return info ? info->title : w->name;
}

bool gadget_draw(struct gadget_ctx *ctx) {
	const char *style = ctx->style ? ctx->style : "compact";
	if (strcmp(style, "compact") == 0 || strcmp(style, "tile") == 0) {
		return false;
	}
	struct gadget_palette pal;
	gadget_palette(ctx->panel, ctx->widget, ctx->card, &pal);
	if (ctx->card) {
		gadget_draw_card(ctx, &pal);
	}
	if (strcmp(style, "digital") == 0) {
		draw_digital(ctx, &pal);
		return true;
	}
	if (strcmp(style, "analog") == 0) {
		draw_analog(ctx, &pal);
		return true;
	}
	if (strcmp(style, "binary") == 0) {
		draw_binary(ctx, &pal);
		return true;
	}
	struct widget_sample sample = { .percent = -1 };
	const char *title = title_of(ctx->widget);
	if (!ctx->widget->impl->sample || !ctx->widget->impl->sample(ctx->widget, &sample)) {
		// nothing to show, e.g. no battery: say so instead of an empty chart
		text(ctx->cairo, &pal, false, 12 * ctx->scale, title, 0, ctx->height / 2 - 20 * ctx->scale,
			ctx->width, 20 * ctx->scale, pal.dim, PD_CENTER);
		text(ctx->cairo, &pal, true, 14 * ctx->scale, "Not available", 0, ctx->height / 2,
			ctx->width, 22 * ctx->scale, pal.fg, PD_CENTER);
		return true;
	}
	if (strcmp(style, "chart") == 0) {
		draw_chart(ctx, &pal, &sample, title);
	} else if (strcmp(style, "gauge") == 0) {
		draw_gauge(ctx, &pal, &sample, title);
	} else if (strcmp(style, "ring") == 0) {
		draw_ring(ctx, &pal, &sample, title);
	} else {
		draw_bar(ctx, &pal, &sample, title);
	}
	return true;
}
