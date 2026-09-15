#include <math.h>
#include <string.h>
#include <strings.h>
#include <pango/pangocairo.h>
#include "cairo_util.h"
#include "sway/tilewin.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Procedural renderers for the window decorations of the built-in theme
 * styles. Every color and metric can be overridden from theme.conf; the
 * defaults here approximate the respective Windows look.
 */

enum style {
	STYLE_WIN95,
	STYLE_WINXP,
	STYLE_WIN7,
	STYLE_WIN10,
	STYLE_WIN11,
};

enum glyph {
	GLYPH_MINIMIZE,
	GLYPH_MAXIMIZE,
	GLYPH_RESTORE,
	GLYPH_CLOSE,
};

struct metrics {
	int top, side, bottom, grab, radius;
};

static enum style get_style(const struct tw_theme *theme) {
	const char *s = theme && theme->style ? theme->style : "win10";
	if (strcasecmp(s, "win95") == 0 || strcasecmp(s, "classic") == 0) {
		return STYLE_WIN95;
	} else if (strcasecmp(s, "winxp") == 0 || strcasecmp(s, "luna") == 0) {
		return STYLE_WINXP;
	} else if (strcasecmp(s, "win7") == 0 || strcasecmp(s, "aero") == 0) {
		return STYLE_WIN7;
	} else if (strcasecmp(s, "win11") == 0 || strcasecmp(s, "fluent") == 0) {
		return STYLE_WIN11;
	}
	return STYLE_WIN10;
}

static void get_metrics(const struct tw_theme *t, bool maximized, struct metrics *m) {
	static const struct {
		int top, max_top, border, grab, radius;
	} defaults[] = {
		[STYLE_WIN95] = { 23, 19, 4, 2, 0 },
		[STYLE_WINXP] = { 30, 26, 4, 3, 8 },
		[STYLE_WIN7] = { 30, 22, 8, 0, 6 },
		[STYLE_WIN10] = { 31, 30, 1, 6, 0 },
		[STYLE_WIN11] = { 33, 32, 1, 8, 8 },
	};
	enum style s = get_style(t);
	int border = tw_theme_int(t, "decoration.border_width", defaults[s].border);
	if (maximized) {
		m->top = tw_theme_int(t, "decoration.maximized_top_height", defaults[s].max_top);
		m->side = m->bottom = 0;
		m->grab = 0;
		m->radius = 0;
	} else {
		m->top = tw_theme_int(t, "decoration.top_height", defaults[s].top);
		m->side = m->bottom = border;
		m->grab = tw_theme_int(t, "decoration.grab_margin", defaults[s].grab);
		m->radius = tw_theme_int(t, "decoration.corner_radius", defaults[s].radius);
	}
}

void tw_style_insets(const struct tw_theme *theme, bool maximized,
		struct tw_insets *insets, int *grab_margin) {
	struct metrics m;
	get_metrics(theme, maximized, &m);
	insets->top = m.top;
	insets->left = insets->right = m.side;
	insets->bottom = m.bottom;
	if (grab_margin) {
		*grab_margin = m.grab;
	}
}

void tw_style_buttons(const struct tw_theme *t, int width, bool maximized,
		struct tw_buttons *b) {
	struct metrics m;
	get_metrics(t, maximized, &m);
	switch (get_style(t)) {
	case STYLE_WIN95: {
		int bw = tw_theme_int(t, "decoration.button_width", 16);
		int bh = tw_theme_int(t, "decoration.button_height", 14);
		int y = m.side + 2;
		int right = width - m.side - 2;
		b->close = (struct wlr_box){ right - bw, y, bw, bh };
		b->maximize = (struct wlr_box){ b->close.x - 2 - bw, y, bw, bh };
		b->minimize = (struct wlr_box){ b->maximize.x - bw, y, bw, bh };
		break;
	}
	case STYLE_WINXP: {
		int bw = tw_theme_int(t, "decoration.button_width", 21);
		int bh = tw_theme_int(t, "decoration.button_height", 21);
		int y = (m.top - bh) / 2 + (maximized ? 0 : 1);
		int right = width - m.side - 2;
		b->close = (struct wlr_box){ right - bw, y, bw, bh };
		b->maximize = (struct wlr_box){ b->close.x - 2 - bw, y, bw, bh };
		b->minimize = (struct wlr_box){ b->maximize.x - 2 - bw, y, bw, bh };
		break;
	}
	case STYLE_WIN7: {
		int bh = tw_theme_int(t, "decoration.button_height", maximized ? 20 : 19);
		int cw = tw_theme_int(t, "decoration.close_width", 47);
		int bw = tw_theme_int(t, "decoration.button_width", 27);
		int y = maximized ? 0 : 1;
		int right = width - m.side + (maximized ? -2 : 2);
		b->close = (struct wlr_box){ right - cw, y, cw, bh };
		b->maximize = (struct wlr_box){ b->close.x - bw + 1, y, bw - 1, bh };
		b->minimize = (struct wlr_box){ b->maximize.x - bw, y, bw, bh };
		break;
	}
	case STYLE_WIN10:
	case STYLE_WIN11: {
		int bw = tw_theme_int(t, "decoration.button_width", 46);
		int y = maximized ? 0 : m.side;
		int bh = m.top - y;
		int right = width - m.side;
		b->close = (struct wlr_box){ right - bw, y, bw, bh };
		b->maximize = (struct wlr_box){ b->close.x - bw, y, bw, bh };
		b->minimize = (struct wlr_box){ b->maximize.x - bw, y, bw, bh };
		break;
	}
	}
}

/* ---------- drawing helpers ---------- */

static void fill_rect(cairo_t *cr, double x, double y, double w, double h,
		uint32_t color) {
	if (w <= 0 || h <= 0) {
		return;
	}
	cairo_set_source_u32(cr, color);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h,
		double tl, double tr, double br, double bl) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
	cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
	cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
	cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* Parses "0:#rrggbb 0.5:#rrggbbaa ..." into a gradient pattern. */
static cairo_pattern_t *make_gradient(const struct tw_theme *t, const char *key,
		const char *fallback, double x0, double y0, double x1, double y1) {
	const char *spec = tw_theme_str(t, key, fallback);
	cairo_pattern_t *pattern = cairo_pattern_create_linear(x0, y0, x1, y1);
	char *copy = strdup(spec);
	char *save = NULL;
	int stops = 0;
	for (char *tok = strtok_r(copy, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
		char *colon = strchr(tok, ':');
		double offset = 0;
		const char *color_str = tok;
		if (colon) {
			*colon = '\0';
			offset = strtod(tok, NULL);
			color_str = colon + 1;
		} else if (stops > 0) {
			offset = 1;
		}
		uint32_t c;
		if (tw_parse_color(color_str, &c)) {
			cairo_pattern_add_color_stop_rgba(pattern, offset,
				(c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
				(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
			stops++;
		}
	}
	free(copy);
	return pattern;
}

static void fill_gradient(cairo_t *cr, const struct tw_theme *t, const char *key,
		const char *fallback, double x, double y, double w, double h, bool vertical) {
	cairo_pattern_t *p = vertical ?
		make_gradient(t, key, fallback, 0, y, 0, y + h) :
		make_gradient(t, key, fallback, x, 0, x + w, 0);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
}

static PangoLayout *make_layout(cairo_t *cr, const char *font, const char *text,
		double max_width) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text ? text : "", -1);
	pango_layout_set_single_paragraph_mode(layout, true);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	if (max_width > 0) {
		pango_layout_set_width(layout, (int)(max_width * PANGO_SCALE));
	}
	return layout;
}

enum text_effect {
	TEXT_PLAIN,
	TEXT_SHADOW,
	TEXT_GLOW,
};

static void draw_text(cairo_t *cr, const char *font, const char *text,
		double x, double y, double max_width, double height, uint32_t color,
		bool center, enum text_effect effect, uint32_t effect_color) {
	if (!text || !*text || max_width <= 4) {
		return;
	}
	PangoLayout *layout = make_layout(cr, font, text, max_width);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	double tx = center ? x + (max_width - tw) / 2 : x;
	double ty = y + floor((height - th) / 2);
	if (effect == TEXT_SHADOW) {
		cairo_set_source_u32(cr, effect_color);
		cairo_move_to(cr, tx + 1, ty + 1);
		pango_cairo_show_layout(cr, layout);
	} else if (effect == TEXT_GLOW) {
		cairo_set_source_u32(cr, effect_color);
		for (int dx = -2; dx <= 2; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				if (dx == 0 && dy == 0) {
					continue;
				}
				cairo_move_to(cr, tx + dx, ty + dy);
				pango_cairo_show_layout(cr, layout);
			}
		}
	}
	cairo_set_source_u32(cr, color);
	cairo_move_to(cr, tx, ty);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void draw_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y,
		double size) {
	if (!icon) {
		return;
	}
	int iw = cairo_image_surface_get_width(icon);
	int ih = cairo_image_surface_get_height(icon);
	if (iw <= 0 || ih <= 0) {
		return;
	}
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / iw, size / ih);
	cairo_set_source_surface(cr, icon, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* Generic placeholder icon: a tiny window. */
static void draw_generic_icon(cairo_t *cr, double x, double y, double size,
		uint32_t frame, uint32_t title) {
	double s = size / 16.0;
	fill_rect(cr, x + 1 * s, y + 2 * s, 14 * s, 12 * s, frame);
	fill_rect(cr, x + 2 * s, y + 3 * s, 12 * s, 3 * s, title);
	fill_rect(cr, x + 2 * s, y + 6 * s, 12 * s, 7 * s, 0xffffffff);
}

static void line(cairo_t *cr, double x0, double y0, double x1, double y1) {
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
}

/* Vector glyphs centered in a box, used by XP/7/10/11. */
static void draw_vector_glyph(cairo_t *cr, enum glyph glyph, const struct wlr_box *box,
		double size, double weight, uint32_t color, bool rounded) {
	double cx = floor(box->x + box->width / 2.0);
	double cy = floor(box->y + box->height / 2.0);
	double h = size / 2;
	double off = fmod(weight, 2) == 1 ? 0.5 : 0;
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, weight);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	switch (glyph) {
	case GLYPH_MINIMIZE:
		line(cr, cx - h, cy + off, cx + h, cy + off);
		cairo_stroke(cr);
		break;
	case GLYPH_MAXIMIZE:
		if (rounded) {
			rounded_path(cr, cx - h + off, cy - h + off, size - weight, size - weight,
				2, 2, 2, 2);
		} else {
			cairo_rectangle(cr, cx - h + off, cy - h + off, size - weight, size - weight);
		}
		cairo_stroke(cr);
		break;
	case GLYPH_RESTORE: {
		double s = size * 0.8;
		double d = size - s;
		if (rounded) {
			rounded_path(cr, cx - h + off, cy - h + d + off, s - weight, s - weight,
				1.5, 1.5, 1.5, 1.5);
		} else {
			cairo_rectangle(cr, cx - h + off, cy - h + d + off, s - weight, s - weight);
		}
		cairo_stroke(cr);
		cairo_move_to(cr, cx - h + d + off, cy - h + d);
		cairo_line_to(cr, cx - h + d + off, cy - h + off);
		cairo_line_to(cr, cx + h - off, cy - h + off);
		cairo_line_to(cr, cx + h - off, cy + h - d - off);
		cairo_line_to(cr, cx + h - d, cy + h - d - off);
		cairo_stroke(cr);
		break;
	}
	case GLYPH_CLOSE:
		cairo_set_line_cap(cr, rounded ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_SQUARE);
		line(cr, cx - h + 0.5, cy - h + 0.5, cx + h - 0.5, cy + h - 0.5);
		line(cr, cx + h - 0.5, cy - h + 0.5, cx - h + 0.5, cy + h - 0.5);
		cairo_stroke(cr);
		break;
	}
}

static enum glyph max_glyph(const struct tw_frame *f) {
	return f->maximized ? GLYPH_RESTORE : GLYPH_MAXIMIZE;
}

static const char *state_prefix(const struct tw_frame *f) {
	return f->focused ? "decoration.active" : "decoration.inactive";
}

static uint32_t state_color(const struct tw_theme *t, const struct tw_frame *f,
		const char *name, uint32_t active_default, uint32_t inactive_default) {
	char key[128];
	snprintf(key, sizeof(key), "%s.%s", state_prefix(f), name);
	return tw_theme_color(t, key, f->focused ? active_default : inactive_default);
}

static const char *state_str(const struct tw_theme *t, const struct tw_frame *f,
		const char *name, const char *active_default, const char *inactive_default) {
	char key[128];
	snprintf(key, sizeof(key), "%s.%s", state_prefix(f), name);
	return tw_theme_str(t, key, f->focused ? active_default : inactive_default);
}

static double title_max_width(const struct tw_buttons *b, double x) {
	return b->minimize.x - x - 6;
}

/* ---------- Windows 95 ---------- */

static void bevel(cairo_t *cr, const struct wlr_box *b, bool sunken,
		uint32_t face, uint32_t hi, uint32_t light, uint32_t shadow, uint32_t dark) {
	fill_rect(cr, b->x, b->y, b->width, b->height, face);
	uint32_t o_tl = sunken ? dark : hi, o_br = sunken ? hi : dark;
	uint32_t i_tl = sunken ? shadow : light, i_br = sunken ? light : shadow;
	fill_rect(cr, b->x, b->y, b->width - 1, 1, o_tl);
	fill_rect(cr, b->x, b->y, 1, b->height - 1, o_tl);
	fill_rect(cr, b->x + b->width - 1, b->y, 1, b->height, o_br);
	fill_rect(cr, b->x, b->y + b->height - 1, b->width, 1, o_br);
	fill_rect(cr, b->x + 1, b->y + 1, b->width - 3, 1, i_tl);
	fill_rect(cr, b->x + 1, b->y + 1, 1, b->height - 3, i_tl);
	fill_rect(cr, b->x + b->width - 2, b->y + 1, 1, b->height - 2, i_br);
	fill_rect(cr, b->x + 1, b->y + b->height - 2, b->width - 2, 1, i_br);
}

static void draw_95_glyph(cairo_t *cr, enum glyph glyph, double x, double y,
		uint32_t c) {
	switch (glyph) {
	case GLYPH_MINIMIZE:
		fill_rect(cr, x + 4, y + 9, 6, 2, c);
		break;
	case GLYPH_MAXIMIZE:
		fill_rect(cr, x + 3, y + 2, 9, 2, c);
		fill_rect(cr, x + 3, y + 2, 1, 9, c);
		fill_rect(cr, x + 11, y + 2, 1, 9, c);
		fill_rect(cr, x + 3, y + 10, 9, 1, c);
		break;
	case GLYPH_RESTORE:
		fill_rect(cr, x + 5, y + 1, 6, 2, c);
		fill_rect(cr, x + 10, y + 1, 1, 6, c);
		fill_rect(cr, x + 5, y + 1, 1, 3, c);
		fill_rect(cr, x + 9, y + 6, 2, 1, c);
		fill_rect(cr, x + 3, y + 4, 6, 2, c);
		fill_rect(cr, x + 3, y + 4, 1, 6, c);
		fill_rect(cr, x + 8, y + 4, 1, 6, c);
		fill_rect(cr, x + 3, y + 9, 6, 1, c);
		break;
	case GLYPH_CLOSE:
		for (int r = 0; r < 7; r++) {
			fill_rect(cr, x + 4 + r, y + 3 + r, 2, 1, c);
			fill_rect(cr, x + 10 - r, y + 3 + r, 2, 1, c);
		}
		break;
	}
}

static void draw_win95(cairo_t *cr, const struct tw_theme *t,
		const struct tw_frame *f, const struct metrics *m) {
	uint32_t face = tw_theme_color(t, "decoration.face", 0xc0c0c0ff);
	uint32_t hi = tw_theme_color(t, "decoration.highlight", 0xffffffff);
	uint32_t light = tw_theme_color(t, "decoration.light", 0xdfdfdfff);
	uint32_t shadow = tw_theme_color(t, "decoration.shadow", 0x808080ff);
	uint32_t dark = tw_theme_color(t, "decoration.dark", 0x000000ff);
	int W = f->width, H = f->height;

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	if (m->side > 0) {
		struct wlr_box outer = { 0, 0, W, H };
		// the frame uses light as the outer top-left line
		bevel(cr, &outer, false, face, light, hi, shadow, dark);
	} else {
		fill_rect(cr, 0, 0, W, m->top, face);
	}

	int bar_h = tw_theme_int(t, "decoration.title_height", 18);
	int bx = m->side, by = m->side, bw = W - 2 * m->side;
	cairo_rectangle(cr, bx, by, bw, bar_h);
	fill_gradient(cr, t, f->focused ? "decoration.active.title_gradient" :
		"decoration.inactive.title_gradient",
		f->focused ? "0:#000080 1:#1084d0" : "0:#808080 1:#b5b5b5",
		bx, by, bw, bar_h, false);

	struct tw_buttons b;
	tw_style_buttons(t, W, f->maximized, &b);

	double x = bx + 2;
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
	if (f->icon) {
		draw_icon(cr, f->icon, x, by + (bar_h - 16) / 2, 16);
	} else {
		draw_generic_icon(cr, x, by + (bar_h - 16) / 2, 16, face, 0x000080ff);
	}
	x += 19;
	draw_text(cr, tw_theme_str(t, "decoration.title_font", "Tahoma, Noto Sans Bold 8"),
		f->title, x, by, title_max_width(&b, x), bar_h,
		state_color(t, f, "title_fg", 0xffffffff, 0xc0c0c0ff), false, TEXT_PLAIN, 0);

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	const struct {
		const struct wlr_box *box;
		enum tw_hit hit;
		enum glyph glyph;
	} buttons[] = {
		{ &b.minimize, TW_HIT_MINIMIZE, GLYPH_MINIMIZE },
		{ &b.maximize, TW_HIT_MAXIMIZE, max_glyph(f) },
		{ &b.close, TW_HIT_CLOSE, GLYPH_CLOSE },
	};
	for (size_t i = 0; i < 3; i++) {
		bool pressed = f->pressed == buttons[i].hit && f->hover == buttons[i].hit;
		struct wlr_box box = *buttons[i].box;
		bevel(cr, &box, pressed, face, hi, light, shadow, dark);
		int off = pressed ? 1 : 0;
		draw_95_glyph(cr, buttons[i].glyph, box.x + off, box.y + off,
			tw_theme_color(t, "decoration.glyph", 0x000000ff));
	}
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
}

/* ---------- Windows XP (Luna) ---------- */

static void draw_xp_button(cairo_t *cr, const struct tw_theme *t,
		const struct tw_frame *f, const struct wlr_box *b, enum glyph glyph,
		enum tw_hit hit) {
	bool close = hit == TW_HIT_CLOSE;
	bool hover = f->hover == hit;
	bool pressed = hover && f->pressed == hit;
	const char *state = pressed ? "pressed" : hover ? "hover" : "normal";
	char key[128];
	snprintf(key, sizeof(key), "decoration.%s.%s%s", close ? "close" : "button",
		f->focused ? "" : "inactive_", state);

	const char *fallback;
	if (close) {
		fallback = !f->focused ? "0:#d6a79a 1:#c08a7c" :
			pressed ? "0:#b52f0c 1:#d65332" : hover ? "0:#f7a283 0.45:#ec6a45 1:#d3481f" :
			"0:#e9876a 0.45:#d9532e 1:#c2330f";
	} else {
		fallback = !f->focused ? "0:#a6bdf0 1:#8aa6e6" :
			pressed ? "0:#1f4fcb 1:#3a74e8" : hover ? "0:#79b1ff 0.5:#4a8bff 1:#3a73e8" :
			"0:#5c99f9 0.5:#2d6ef0 1:#2758d6";
	}

	rounded_path(cr, b->x + 0.5, b->y + 0.5, b->width - 1, b->height - 1, 3, 3, 3, 3);
	fill_gradient(cr, t, key, fallback, b->x, b->y, b->width, b->height, true);
	rounded_path(cr, b->x + 0.5, b->y + 0.5, b->width - 1, b->height - 1, 3, 3, 3, 3);
	cairo_set_source_u32(cr, f->focused ? 0xffffffff : 0xdce6faff);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	rounded_path(cr, b->x + 1.5, b->y + 1.5, b->width - 3, b->height - 3, 2, 2, 2, 2);
	cairo_set_source_u32(cr, 0xffffff30);
	cairo_stroke(cr);

	uint32_t gc = f->focused ? 0xffffffff : 0xe8eefbff;
	double cx = b->x + b->width / 2.0, cy = b->y + b->height / 2.0;
	cairo_set_source_u32(cr, gc);
	switch (glyph) {
	case GLYPH_MINIMIZE:
		fill_rect(cr, floor(cx - 5), floor(cy + 3), 7, 3, gc);
		break;
	case GLYPH_MAXIMIZE:
		cairo_rectangle(cr, floor(cx - 5) + 1, floor(cy - 5) + 1, 9, 9);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		fill_rect(cr, floor(cx - 5), floor(cy - 5), 11, 3, gc);
		break;
	case GLYPH_RESTORE:
		cairo_rectangle(cr, floor(cx - 1) + 0.5, floor(cy - 5) + 0.5, 6, 6);
		cairo_stroke(cr);
		fill_rect(cr, floor(cx - 1), floor(cy - 5), 7, 2, gc);
		fill_rect(cr, floor(cx - 5), floor(cy - 1), 7, 6, close ? 0 : 0);
		cairo_rectangle(cr, floor(cx - 5) + 0.5, floor(cy - 1) + 0.5, 6, 6);
		cairo_stroke(cr);
		fill_rect(cr, floor(cx - 5), floor(cy - 1), 7, 2, gc);
		break;
	case GLYPH_CLOSE:
		cairo_set_line_width(cr, 2.2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		line(cr, cx - 4, cy - 4, cx + 4, cy + 4);
		line(cr, cx + 4, cy - 4, cx - 4, cy + 4);
		cairo_stroke(cr);
		break;
	}
}

static void draw_winxp(cairo_t *cr, const struct tw_theme *t,
		const struct tw_frame *f, const struct metrics *m) {
	int W = f->width, H = f->height;
	double r = m->radius;

	cairo_save(cr);
	rounded_path(cr, 0, 0, W, H, r, r, 0, 0);
	cairo_clip(cr);

	fill_rect(cr, 0, 0, W, H, state_color(t, f, "frame", 0x0831d9ff, 0x7b97e0ff));
	if (m->side > 0) {
		uint32_t outer = state_color(t, f, "frame_dark", 0x001ea0ff, 0x6582d0ff);
		uint32_t inner = state_color(t, f, "frame_light", 0x166aeeff, 0x9fb5eaff);
		fill_rect(cr, 0, m->top, 1, H - m->top, outer);
		fill_rect(cr, W - 1, m->top, 1, H - m->top, outer);
		fill_rect(cr, 0, H - 1, W, 1, outer);
		fill_rect(cr, m->side - 1, m->top, 1, H - m->top - m->bottom + 1, inner);
		fill_rect(cr, W - m->side, m->top, 1, H - m->top - m->bottom + 1, inner);
		fill_rect(cr, m->side - 1, H - m->bottom, W - 2 * m->side + 2, 1, inner);
	}

	cairo_rectangle(cr, 0, 0, W, m->top);
	fill_gradient(cr, t, f->focused ? "decoration.active.title_gradient" :
		"decoration.inactive.title_gradient",
		f->focused ?
			"0:#3d8df5 0.06:#0a66f7 0.14:#0055e9 0.45:#0058ee 0.72:#005ef4 0.88:#0463f8 0.95:#0851db 1:#0842c1" :
			"0:#a8c1f0 0.1:#7e9ae2 0.5:#7c97e0 0.9:#8ea8e6 1:#7c93d6",
		0, 0, W, m->top, true);
	fill_rect(cr, 0, 0, W, 1, state_color(t, f, "title_highlight", 0x6ca6ffff, 0xb9ccf2ff));
	cairo_restore(cr);

	struct tw_buttons b;
	tw_style_buttons(t, W, f->maximized, &b);

	double x = m->side + 3;
	double icon_y = floor((m->top - 16) / 2.0) + (f->maximized ? 0 : 1);
	if (f->icon) {
		draw_icon(cr, f->icon, x, icon_y, 16);
	} else {
		draw_generic_icon(cr, x, icon_y, 16, 0xece9d8ff, 0x0058eeff);
	}
	x += 21;
	draw_text(cr, tw_theme_str(t, "decoration.title_font",
			"Trebuchet MS, Noto Sans Bold 10"),
		f->title, x, f->maximized ? 0 : 1, title_max_width(&b, x), m->top,
		state_color(t, f, "title_fg", 0xffffffff, 0xd8e4f8ff), false,
		f->focused ? TEXT_SHADOW : TEXT_PLAIN,
		state_color(t, f, "title_shadow", 0x0a1883ff, 0x00000000));

	draw_xp_button(cr, t, f, &b.minimize, GLYPH_MINIMIZE, TW_HIT_MINIMIZE);
	draw_xp_button(cr, t, f, &b.maximize, max_glyph(f), TW_HIT_MAXIMIZE);
	draw_xp_button(cr, t, f, &b.close, GLYPH_CLOSE, TW_HIT_CLOSE);
}

/* ---------- Windows 7 (Aero) ---------- */

static void draw_outlined_glyph(cairo_t *cr, enum glyph glyph, const struct wlr_box *b) {
	double cx = floor(b->x + b->width / 2.0), cy = floor(b->y + b->height / 2.0);
	for (int pass = 0; pass < 2; pass++) {
		uint32_t color = pass == 0 ? 0x1b2a3ab0 : 0xffffffff;
		double grow = pass == 0 ? 1 : 0;
		cairo_set_source_u32(cr, color);
		switch (glyph) {
		case GLYPH_MINIMIZE:
			cairo_rectangle(cr, cx - 5 - grow, cy + 1 - grow, 10 + 2 * grow, 3 + 2 * grow);
			cairo_fill(cr);
			break;
		case GLYPH_MAXIMIZE:
			cairo_set_line_width(cr, 2 + 2 * grow);
			cairo_rectangle(cr, cx - 4.5, cy - 3.5, 9, 7);
			cairo_stroke(cr);
			cairo_rectangle(cr, cx - 5.5 - grow, cy - 4.5 - grow, 11 + 2 * grow, 2 + grow);
			cairo_fill(cr);
			break;
		case GLYPH_RESTORE:
			cairo_set_line_width(cr, 1.6 + 2 * grow);
			cairo_rectangle(cr, cx - 1.5, cy - 5.5, 7, 6);
			cairo_stroke(cr);
			cairo_rectangle(cr, cx - 5.5, cy - 1.5, 7, 6);
			cairo_stroke(cr);
			break;
		case GLYPH_CLOSE:
			cairo_set_line_width(cr, 2.4 + 2 * grow);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			line(cr, cx - 4, cy - 3.5, cx + 4, cy + 4.5);
			line(cr, cx + 4, cy - 3.5, cx - 4, cy + 4.5);
			cairo_stroke(cr);
			break;
		}
	}
}

static void draw_win7(cairo_t *cr, const struct tw_theme *t,
		const struct tw_frame *f, const struct metrics *m) {
	int W = f->width, H = f->height;
	double r = m->radius;

	rounded_path(cr, 0, 0, W, H, r, r, r, r);
	cairo_set_source_u32(cr, state_color(t, f, "glass", 0x8fb4dae8, 0xc6d7e9dc));
	cairo_fill(cr);

	// glossy reflection over the top area
	cairo_save(cr);
	rounded_path(cr, 0, 0, W, H, r, r, r, r);
	cairo_clip(cr);
	cairo_rectangle(cr, 0, 0, W, m->top);
	fill_gradient(cr, t, "decoration.glass_gloss",
		"0:#ffffff80 0.45:#ffffff30 1:#ffffff08", 0, 0, W, m->top, true);
	// diagonal streaks
	cairo_set_source_u32(cr, 0xffffff18);
	double sw = W * 0.05;
	for (int i = 0; i < 3; i++) {
		double sx = W * (0.12 + 0.18 * i);
		cairo_move_to(cr, sx, 0);
		cairo_line_to(cr, sx + sw, 0);
		cairo_line_to(cr, sx + sw - m->top * 1.2, H);
		cairo_line_to(cr, sx - m->top * 1.2, H);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_restore(cr);

	if (m->side > 0) {
		rounded_path(cr, 0.5, 0.5, W - 1, H - 1, r, r, r, r);
		cairo_set_source_u32(cr, state_color(t, f, "frame_outer", 0x000000a0, 0x00000060));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		rounded_path(cr, 1.5, 1.5, W - 3, H - 3, r - 1, r - 1, r - 1, r - 1);
		cairo_set_source_u32(cr, 0xffffff70);
		cairo_stroke(cr);
		// edge around the client area
		cairo_rectangle(cr, m->side - 1.5, m->top - 1.5,
			W - 2 * m->side + 3, H - m->top - m->bottom + 3);
		cairo_set_source_u32(cr, 0xffffff60);
		cairo_stroke(cr);
		cairo_rectangle(cr, m->side - 0.5, m->top - 0.5,
			W - 2 * m->side + 1, H - m->top - m->bottom + 1);
		cairo_set_source_u32(cr, 0x00000070);
		cairo_stroke(cr);
	}

	struct tw_buttons b;
	tw_style_buttons(t, W, f->maximized, &b);

	double x = m->side + 2;
	double text_top = f->maximized ? 0 : m->side / 2.0;
	double text_h = m->top - text_top - (f->maximized ? 0 : 2);
	if (f->icon) {
		draw_icon(cr, f->icon, x, text_top + (text_h - 16) / 2, 16);
	} else {
		draw_generic_icon(cr, x, text_top + (text_h - 16) / 2, 16, 0x6d8fb8ff, 0x3a6ea5ff);
	}
	x += 22;
	draw_text(cr, tw_theme_str(t, "decoration.title_font", "Segoe UI, Noto Sans 9"),
		f->title, x, text_top, title_max_width(&b, x) - 4, text_h,
		state_color(t, f, "title_fg", 0x000000ff, 0x202020c0), false, TEXT_GLOW,
		state_color(t, f, "title_glow", 0xffffff40, 0xffffff30));

	// caption button group
	struct wlr_box group = { b.minimize.x, b.minimize.y,
		b.close.x + b.close.width - b.minimize.x, b.close.height };
	cairo_save(cr);
	rounded_path(cr, group.x + 0.5, group.y - 4, group.width - 1, group.height + 3.5,
		0, 0, 4, 4);
	cairo_clip(cr);
	const struct {
		const struct wlr_box *box;
		enum tw_hit hit;
		enum glyph glyph;
	} buttons[] = {
		{ &b.minimize, TW_HIT_MINIMIZE, GLYPH_MINIMIZE },
		{ &b.maximize, TW_HIT_MAXIMIZE, max_glyph(f) },
		{ &b.close, TW_HIT_CLOSE, GLYPH_CLOSE },
	};
	for (size_t i = 0; i < 3; i++) {
		const struct wlr_box *box = buttons[i].box;
		bool close = buttons[i].hit == TW_HIT_CLOSE;
		bool hover = f->hover == buttons[i].hit;
		bool pressed = hover && f->pressed == buttons[i].hit;
		const char *fallback;
		if (close) {
			fallback = pressed ? "0:#c1766a 0.5:#a22c10 1:#c5654a" :
				hover ? "0:#fbb9a8 0.45:#f0856a 0.5:#e8401a 1:#f39a78" :
				f->focused ? "0:#e8a898 0.45:#d67a64 0.5:#c24a2d 1:#d2785c" :
				"0:#ffffff50 0.5:#ffffff20 1:#ffffff40";
		} else {
			fallback = pressed ? "0:#8ab6d8e0 0.5:#2c6fa8e0 1:#5ab0e0e0" :
				hover ? "0:#cfeaffd0 0.45:#8ccaf5c0 0.5:#3e9ee8c0 1:#88d5ffd0" :
				"0:#ffffff70 0.45:#ffffff38 0.5:#ffffff12 1:#ffffff40";
		}
		char key[96];
		snprintf(key, sizeof(key), "decoration.%s.%s", close ? "close" : "button",
			pressed ? "pressed" : hover ? "hover" : "normal");
		cairo_rectangle(cr, box->x, box->y - 4, box->width, box->height + 4);
		fill_gradient(cr, t, key, fallback, box->x, box->y, box->width, box->height, true);
		if (i > 0) {
			fill_rect(cr, box->x, box->y, 1, box->height, 0x00000060);
		}
		draw_outlined_glyph(cr, buttons[i].glyph, box);
	}
	cairo_restore(cr);
	rounded_path(cr, group.x + 0.5, group.y - 4, group.width - 1, group.height + 3.5,
		0, 0, 4, 4);
	cairo_set_source_u32(cr, 0x000000a0);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* ---------- Windows 10 / 11 ---------- */

static void draw_shadow(cairo_t *cr, int W, int H, int grab, double radius,
		uint32_t color) {
	double base = (color & 0xff) / 255.0;
	for (int i = grab; i >= 1; i--) {
		double a = base * pow(1.0 - (double)(i - 1) / grab, 2);
		rounded_path(cr, -i + 0.5, -i + 0.5, W + 2 * i - 1, H + 2 * i - 1,
			radius + i, radius + i, radius + i, radius + i);
		cairo_set_source_rgba(cr, (color >> 24 & 0xff) / 255.0,
			(color >> 16 & 0xff) / 255.0, (color >> 8 & 0xff) / 255.0, a);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
}

static void draw_modern(cairo_t *cr, const struct tw_theme *t,
		const struct tw_frame *f, const struct metrics *m, bool win11) {
	int W = f->width, H = f->height;
	double r = m->radius;

	if (m->grab > 0) {
		draw_shadow(cr, W, H, m->grab, r, tw_theme_color(t, "decoration.shadow",
			win11 ? 0x00000030 : 0x00000028));
	}

	cairo_save(cr);
	rounded_path(cr, 0, 0, W, H, r, r, r, r);
	cairo_clip(cr);
	fill_rect(cr, 0, 0, W, H, state_color(t, f, "title_bg",
		win11 ? 0xf3f3f3ff : 0xffffffff, win11 ? 0xfafafaff : 0xffffffff));

	struct tw_buttons b;
	tw_style_buttons(t, W, f->maximized, &b);

	const struct {
		const struct wlr_box *box;
		enum tw_hit hit;
		enum glyph glyph;
	} buttons[] = {
		{ &b.minimize, TW_HIT_MINIMIZE, GLYPH_MINIMIZE },
		{ &b.maximize, TW_HIT_MAXIMIZE, max_glyph(f) },
		{ &b.close, TW_HIT_CLOSE, GLYPH_CLOSE },
	};
	uint32_t glyph_color = state_color(t, f, "glyph", 0x000000ff, win11 ? 0x8c8c8cff : 0x999999ff);
	for (size_t i = 0; i < 3; i++) {
		bool close = buttons[i].hit == TW_HIT_CLOSE;
		bool hover = f->hover == buttons[i].hit;
		bool pressed = hover && f->pressed == buttons[i].hit;
		uint32_t bg = 0, fg = glyph_color;
		if (pressed) {
			bg = close ? tw_theme_color(t, "decoration.close.pressed", win11 ? 0xc83c30ff : 0xf1707aff) :
				tw_theme_color(t, "decoration.button.pressed", win11 ? 0x0000000f : 0xccccccff);
			fg = close ? 0xffffffff : glyph_color;
		} else if (hover) {
			bg = close ? tw_theme_color(t, "decoration.close.hover", win11 ? 0xc42b1cff : 0xe81123ff) :
				tw_theme_color(t, "decoration.button.hover", win11 ? 0x0000000f : 0xe5e5e5ff);
			fg = close ? 0xffffffff : 0x000000ff;
		}
		const struct wlr_box *box = buttons[i].box;
		fill_rect(cr, box->x, box->y, box->width, box->height, bg);
		cairo_set_antialias(cr, buttons[i].glyph == GLYPH_CLOSE || win11 ?
			CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);
		draw_vector_glyph(cr, buttons[i].glyph, box, 10, 1, fg, win11);
		cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
	}

	double x = m->side + (win11 ? 12 : 8);
	if (f->icon) {
		draw_icon(cr, f->icon, x, floor((m->top - 16) / 2.0), 16);
	} else {
		draw_generic_icon(cr, x, floor((m->top - 16) / 2.0), 16, 0x767676ff, 0x0078d7ff);
	}
	x += win11 ? 28 : 24;
	bool center = strcmp(tw_theme_str(t, "decoration.title_align", "left"), "center") == 0;
	double tx = center ? b.minimize.x - (W - b.minimize.x) : x;
	(void)tx;
	draw_text(cr, tw_theme_str(t, "decoration.title_font",
			win11 ? "Segoe UI Variable Text, Segoe UI, Noto Sans 9" : "Segoe UI, Noto Sans 9"),
		f->title, x, 0, title_max_width(&b, x), m->top,
		state_color(t, f, "title_fg", 0x000000ff, win11 ? 0x8c8c8cff : 0x999999ff),
		false, TEXT_PLAIN, 0);
	cairo_restore(cr);

	if (m->side > 0) {
		double fw = tw_theme_int(t, "decoration.frame_width", 1);
		if (fw > m->side) {
			fw = m->side;
		}
		rounded_path(cr, fw / 2, fw / 2, W - fw, H - fw, r, r, r, r);
		cairo_set_source_u32(cr, state_color(t, f, "frame",
			win11 ? 0x00000055 : 0x1f6fc5d0, win11 ? 0x00000030 : 0xaaaaaaff));
		cairo_set_line_width(cr, fw);
		cairo_stroke(cr);
	}
	(void)state_str;
}

void tw_style_draw_frame(cairo_t *cr, const struct tw_theme *theme,
		const struct tw_frame *frame) {
	struct metrics m;
	get_metrics(theme, frame->maximized, &m);
	switch (get_style(theme)) {
	case STYLE_WIN95:
		draw_win95(cr, theme, frame, &m);
		break;
	case STYLE_WINXP:
		draw_winxp(cr, theme, frame, &m);
		break;
	case STYLE_WIN7:
		draw_win7(cr, theme, frame, &m);
		break;
	case STYLE_WIN10:
		draw_modern(cr, theme, frame, &m, false);
		break;
	case STYLE_WIN11:
		draw_modern(cr, theme, frame, &m, true);
		break;
	}
}

/* ---------- Alt+Tab switcher ---------- */

#define ALTTAB_CELL 96
#define ALTTAB_PAD 18
#define ALTTAB_TEXT 34

void tw_style_alttab_size(const struct tw_theme *theme, int count, int max_width,
		int *width, int *height, int *columns) {
	int cols = (max_width - 2 * ALTTAB_PAD) / ALTTAB_CELL;
	if (cols < 1) {
		cols = 1;
	}
	if (cols > 8) {
		cols = 8;
	}
	if (cols > count) {
		cols = count;
	}
	int rows = (count + cols - 1) / cols;
	*columns = cols;
	*width = cols * ALTTAB_CELL + 2 * ALTTAB_PAD;
	*height = rows * ALTTAB_CELL + 2 * ALTTAB_PAD + ALTTAB_TEXT;
}

void tw_style_draw_alttab(cairo_t *cr, const struct tw_theme *t,
		const struct tw_alttab_item *items, int count, int selected,
		int width, int height, int columns) {
	enum style style = get_style(t);
	bool classic = style == STYLE_WIN95;
	double radius = tw_theme_int(t, "alttab.radius",
		style == STYLE_WIN11 ? 8 : style == STYLE_WINXP ? 6 : style == STYLE_WIN7 ? 6 : 0);
	uint32_t bg = tw_theme_color(t, "alttab.bg",
		classic ? 0xc0c0c0ff : style == STYLE_WINXP ? 0x1b56c8f0 :
		style == STYLE_WIN7 ? 0x6d93bce8 : 0x1f1f1fe8);
	uint32_t fg = tw_theme_color(t, "alttab.fg", classic ? 0x000000ff : 0xffffffff);
	uint32_t sel = tw_theme_color(t, "alttab.selection",
		classic ? 0x000080ff : style == STYLE_WINXP ? 0xffffff50 : 0xffffff38);
	uint32_t border = tw_theme_color(t, "alttab.border",
		classic ? 0x000000ff : style == STYLE_WIN10 ? 0x00000000 : 0xffffff60);

	if (classic) {
		struct wlr_box box = { 0, 0, width, height };
		bevel(cr, &box, false, bg, 0xdfdfdfff, 0xffffffff, 0x808080ff, 0x000000ff);
	} else {
		rounded_path(cr, 0, 0, width, height, radius, radius, radius, radius);
		cairo_set_source_u32(cr, bg);
		cairo_fill_preserve(cr);
		cairo_set_source_u32(cr, border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}

	const char *font = tw_theme_str(t, "alttab.font",
		classic ? "Tahoma, Noto Sans 8" : "Segoe UI, Noto Sans 10");
	for (int i = 0; i < count; i++) {
		int col = i % columns, row = i / columns;
		double cx = ALTTAB_PAD + col * ALTTAB_CELL;
		double cy = ALTTAB_PAD + row * ALTTAB_CELL;
		if (i == selected) {
			if (classic) {
				cairo_set_source_u32(cr, sel);
				cairo_set_line_width(cr, 2);
				cairo_rectangle(cr, cx + 5, cy + 5, ALTTAB_CELL - 10, ALTTAB_CELL - 10);
				cairo_stroke(cr);
			} else {
				rounded_path(cr, cx + 4, cy + 4, ALTTAB_CELL - 8, ALTTAB_CELL - 8, 4, 4, 4, 4);
				cairo_set_source_u32(cr, sel);
				cairo_fill_preserve(cr);
				cairo_set_source_u32(cr, 0xffffff80);
				cairo_set_line_width(cr, 1);
				cairo_stroke(cr);
			}
		}
		double isize = 48;
		double ix = cx + (ALTTAB_CELL - isize) / 2, iy = cy + (ALTTAB_CELL - isize) / 2;
		if (items[i].icon) {
			draw_icon(cr, items[i].icon, ix, iy, isize);
		} else {
			draw_generic_icon(cr, ix, iy, isize, 0x606060ff, 0x0078d7ff);
		}
	}

	if (selected >= 0 && selected < count) {
		draw_text(cr, font, items[selected].title, ALTTAB_PAD,
			height - ALTTAB_PAD - ALTTAB_TEXT + 6, width - 2 * ALTTAB_PAD,
			ALTTAB_TEXT - 6, fg, true, TEXT_PLAIN, 0);
	}
}

uint32_t tw_style_snap_color(const struct tw_theme *theme) {
	return tw_theme_color(theme, "snap.color",
		get_style(theme) == STYLE_WIN95 ? 0x80808060 : 0xcfe3ff50);
}
