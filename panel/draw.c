#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cairo_util.h"
#include "draw.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void pd_color(cairo_t *cr, uint32_t color) {
	cairo_set_source_u32(cr, color);
}

void pd_rect(cairo_t *cr, double x, double y, double w, double h, uint32_t color) {
	if (w <= 0 || h <= 0 || (color & 0xff) == 0) {
		return;
	}
	cairo_set_source_u32(cr, color);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

void pd_rounded4(cairo_t *cr, double x, double y, double w, double h,
		double tl, double tr, double br, double bl) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
	cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
	cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
	cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void pd_rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > h / 2) {
		r = h / 2;
	}
	if (r > w / 2) {
		r = w / 2;
	}
	pd_rounded4(cr, x, y, w, h, r, r, r, r);
}

cairo_pattern_t *pd_gradient(const char *stops, double x0, double y0, double x1, double y1) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(x0, y0, x1, y1);
	char *copy = strdup(stops);
	char *save = NULL;
	int count = 0;
	for (char *tok = strtok_r(copy, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
		char *colon = strchr(tok, ':');
		double offset = count > 0 ? 1 : 0;
		const char *color_str = tok;
		if (colon) {
			*colon = '\0';
			offset = strtod(tok, NULL);
			color_str = colon + 1;
		}
		uint32_t c;
		if (tw_parse_color(color_str, &c)) {
			cairo_pattern_add_color_stop_rgba(pattern, offset,
				(c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
				(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
			count++;
		}
	}
	free(copy);
	return pattern;
}

bool pd_source(cairo_t *cr, const struct tw_theme *t, const char *base,
		double y, double h, uint32_t fallback) {
	char key[128];
	snprintf(key, sizeof(key), "%s_gradient", base);
	const char *stops = tw_theme_str(t, key, NULL);
	if (stops) {
		cairo_pattern_t *p = pd_gradient(stops, 0, y, 0, y + h);
		cairo_set_source(cr, p);
		cairo_pattern_destroy(p);
		return true;
	}
	uint32_t color = tw_theme_color(t, base, fallback);
	cairo_set_source_u32(cr, color);
	return (color & 0xff) != 0;
}

void pd_fill(cairo_t *cr, const struct tw_theme *t, const char *base,
		double y, double h, uint32_t fallback) {
	if (pd_source(cr, t, base, y, h, fallback)) {
		cairo_fill(cr);
	} else {
		cairo_new_path(cr);
	}
}

void pd_bevel(cairo_t *cr, double x, double y, double w, double h, bool sunken) {
	uint32_t hi = 0xffffffff, light = 0xdfdfdfff, shadow = 0x808080ff, dark = 0x000000ff;
	uint32_t o_tl = sunken ? dark : hi, o_br = sunken ? hi : dark;
	uint32_t i_tl = sunken ? shadow : light, i_br = sunken ? light : shadow;
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

void pd_border_sunken_thin(cairo_t *cr, double x, double y, double w, double h) {
	cairo_save(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	pd_rect(cr, x, y, w - 1, 1, 0x808080ff);
	pd_rect(cr, x, y, 1, h - 1, 0x808080ff);
	pd_rect(cr, x + w - 1, y, 1, h, 0xffffffff);
	pd_rect(cr, x, y + h - 1, w, 1, 0xffffffff);
	cairo_restore(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *font, const char *text,
		double width) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text ? text : "", -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	if (width > 0) {
		pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
	}
	return layout;
}

void pd_text_size(cairo_t *cr, const char *font, const char *text, int *w, int *h) {
	PangoLayout *layout = make_layout(cr, font, text, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (w) {
		*w = tw;
	}
	if (h) {
		*h = th;
	}
	g_object_unref(layout);
}

void pd_text(cairo_t *cr, const char *font, const char *text, double x, double y,
		double w, double h, uint32_t color, enum pd_align align) {
	if (!text || !*text || w <= 2) {
		return;
	}
	PangoLayout *layout = make_layout(cr, font, text, w);
	pango_layout_set_alignment(layout, align == PD_CENTER ? PANGO_ALIGN_CENTER :
		align == PD_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_set_source_u32(cr, color);
	cairo_move_to(cr, x, y + floor((h - th) / 2.0));
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

int pd_text_wrapped(cairo_t *cr, const char *font, const char *text, double x, double y,
		double w, int max_lines, uint32_t color, bool draw) {
	if (!text || !*text || w <= 2) {
		return 0;
	}
	PangoLayout *layout = make_layout(cr, font, text, w);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	if (max_lines > 0) {
		pango_layout_set_height(layout, -max_lines);
	}
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (draw) {
		cairo_set_source_u32(cr, color);
		cairo_move_to(cr, x, y);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
	return th;
}

void pd_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size) {
	if (!icon) {
		pd_glyph_generic_app(cr, x, y, size);
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

/* ---------- glyphs ---------- */

void pd_glyph_windows(cairo_t *cr, double x, double y, double size, uint32_t c1,
		uint32_t c2, uint32_t c3, uint32_t c4, bool wavy) {
	cairo_new_path(cr);
	double gap = size * 0.08;
	double half = (size - gap) / 2;
	uint32_t colors[4] = { c1, c2, c3, c4 };
	for (int i = 0; i < 4; i++) {
		double px = x + (i % 2) * (half + gap);
		double py = y + (i / 2) * (half + gap);
		cairo_set_source_u32(cr, colors[i]);
		if (wavy) {
			double skew = half * 0.12;
			cairo_move_to(cr, px, py + skew);
			cairo_curve_to(cr, px + half * 0.35, py - skew, px + half * 0.65, py + skew * 2,
				px + half, py);
			cairo_line_to(cr, px + half, py + half - skew);
			cairo_curve_to(cr, px + half * 0.65, py + half + skew, px + half * 0.35,
				py + half - skew * 2, px, py + half);
			cairo_close_path(cr);
		} else {
			cairo_rectangle(cr, px, py, half, half);
		}
		cairo_fill(cr);
	}
}

void pd_glyph_speaker(cairo_t *cr, double x, double y, double s, int level,
		bool muted, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_move_to(cr, x + s * 0.12, y + s * 0.38);
	cairo_line_to(cr, x + s * 0.3, y + s * 0.38);
	cairo_line_to(cr, x + s * 0.52, y + s * 0.18);
	cairo_line_to(cr, x + s * 0.52, y + s * 0.82);
	cairo_line_to(cr, x + s * 0.3, y + s * 0.62);
	cairo_line_to(cr, x + s * 0.12, y + s * 0.62);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, s * 0.07);
	if (muted) {
		cairo_move_to(cr, x + s * 0.64, y + s * 0.38);
		cairo_line_to(cr, x + s * 0.88, y + s * 0.62);
		cairo_move_to(cr, x + s * 0.88, y + s * 0.38);
		cairo_line_to(cr, x + s * 0.64, y + s * 0.62);
		cairo_stroke(cr);
	} else {
		int waves = level > 66 ? 3 : level > 33 ? 2 : level > 0 ? 1 : 0;
		for (int i = 0; i < waves; i++) {
			double r = s * (0.16 + 0.12 * i);
			cairo_arc(cr, x + s * 0.52, y + s * 0.5, r, -M_PI / 4, M_PI / 4);
			cairo_stroke(cr);
		}
	}
	cairo_restore(cr);
}

void pd_glyph_battery(cairo_t *cr, double x, double y, double s, int percent,
		bool charging, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.07);
	double bx = x + s * 0.1, by = y + s * 0.3, bw = s * 0.72, bh = s * 0.4;
	cairo_rectangle(cr, bx, by, bw, bh);
	cairo_stroke(cr);
	cairo_rectangle(cr, bx + bw, by + bh * 0.3, s * 0.08, bh * 0.4);
	cairo_fill(cr);
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}
	double inner = (bw - s * 0.14) * percent / 100.0;
	if (percent <= 15 && !charging) {
		cairo_set_source_u32(cr, 0xe81123ff);
	}
	cairo_rectangle(cr, bx + s * 0.07, by + s * 0.07, inner, bh - s * 0.14);
	cairo_fill(cr);
	if (charging) {
		cairo_set_source_u32(cr, color);
		cairo_move_to(cr, x + s * 0.5, y + s * 0.12);
		cairo_line_to(cr, x + s * 0.36, y + s * 0.52);
		cairo_line_to(cr, x + s * 0.48, y + s * 0.52);
		cairo_line_to(cr, x + s * 0.42, y + s * 0.88);
		cairo_line_to(cr, x + s * 0.62, y + s * 0.44);
		cairo_line_to(cr, x + s * 0.5, y + s * 0.44);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

void pd_glyph_network(cairo_t *cr, double x, double y, double s, int bars,
		bool wireless, bool connected, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	uint32_t dim = (color & 0xffffff00) | 0x50;
	if (wireless) {
		cairo_set_line_width(cr, s * 0.09);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		double cx = x + s * 0.5, cy = y + s * 0.82;
		for (int i = 0; i < 3; i++) {
			cairo_set_source_u32(cr, connected && bars > i + 1 ? color : dim);
			cairo_arc(cr, cx, cy, s * (0.26 + 0.2 * i), -M_PI * 0.75, -M_PI * 0.25);
			cairo_stroke(cr);
		}
		cairo_set_source_u32(cr, connected ? color : dim);
		cairo_arc(cr, cx, cy, s * 0.07, 0, 2 * M_PI);
		cairo_fill(cr);
	} else {
		cairo_set_source_u32(cr, connected ? color : dim);
		cairo_set_line_width(cr, s * 0.07);
		cairo_rectangle(cr, x + s * 0.15, y + s * 0.15, s * 0.7, s * 0.48);
		cairo_stroke(cr);
		cairo_rectangle(cr, x + s * 0.44, y + s * 0.63, s * 0.12, s * 0.12);
		cairo_rectangle(cr, x + s * 0.28, y + s * 0.75, s * 0.44, s * 0.07);
		cairo_fill(cr);
	}
	if (!connected) {
		cairo_set_source_u32(cr, 0xe81123ff);
		cairo_set_line_width(cr, s * 0.09);
		cairo_move_to(cr, x + s * 0.62, y + s * 0.62);
		cairo_line_to(cr, x + s * 0.9, y + s * 0.9);
		cairo_move_to(cr, x + s * 0.9, y + s * 0.62);
		cairo_line_to(cr, x + s * 0.62, y + s * 0.9);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

void pd_glyph_search(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.09);
	cairo_arc(cr, x + s * 0.58, y + s * 0.42, s * 0.26, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, x + s * 0.39, y + s * 0.61);
	cairo_line_to(cr, x + s * 0.14, y + s * 0.86);
	cairo_stroke(cr);
	cairo_restore(cr);
}

void pd_glyph_arrow(cairo_t *cr, double x, double y, double s, int direction,
		uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_translate(cr, x + s / 2, y + s / 2);
	cairo_rotate(cr, direction * M_PI / 2);
	cairo_move_to(cr, -s * 0.15, -s * 0.3);
	cairo_line_to(cr, s * 0.2, 0);
	cairo_line_to(cr, -s * 0.15, s * 0.3);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_restore(cr);
}

void pd_glyph_tile(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.08);
	cairo_rectangle(cr, x + s * 0.12, y + s * 0.16, s * 0.76, s * 0.68);
	cairo_move_to(cr, x + s * 0.5, y + s * 0.16);
	cairo_line_to(cr, x + s * 0.5, y + s * 0.84);
	cairo_move_to(cr, x + s * 0.5, y + s * 0.5);
	cairo_line_to(cr, x + s * 0.88, y + s * 0.5);
	cairo_stroke(cr);
	cairo_restore(cr);
}

void pd_glyph_overlap(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.08);
	cairo_rectangle(cr, x + s * 0.12, y + s * 0.3, s * 0.5, s * 0.5);
	cairo_stroke(cr);
	cairo_rectangle(cr, x + s * 0.38, y + s * 0.14, s * 0.5, s * 0.5);
	cairo_stroke(cr);
	cairo_rectangle(cr, x + s * 0.38, y + s * 0.14, s * 0.5, s * 0.12);
	cairo_fill(cr);
	cairo_restore(cr);
}

void pd_glyph_power(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.09);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_arc(cr, x + s * 0.5, y + s * 0.55, s * 0.3, -M_PI * 0.3, M_PI * 1.3);
	cairo_stroke(cr);
	cairo_move_to(cr, x + s * 0.5, y + s * 0.12);
	cairo_line_to(cr, x + s * 0.5, y + s * 0.5);
	cairo_stroke(cr);
	cairo_restore(cr);
}

void pd_glyph_brightness(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_arc(cr, x + s / 2, y + s / 2, s * 0.18, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, s * 0.07);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int i = 0; i < 8; i++) {
		double a = i * M_PI / 4;
		cairo_move_to(cr, x + s / 2 + cos(a) * s * 0.3, y + s / 2 + sin(a) * s * 0.3);
		cairo_line_to(cr, x + s / 2 + cos(a) * s * 0.42, y + s / 2 + sin(a) * s * 0.42);
	}
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* A drive with a lamp that lights up while it is busy. */
void pd_glyph_disk(cairo_t *cr, double x, double y, double s, bool active, uint32_t color) {
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_set_source_u32(cr, color);
	cairo_set_line_width(cr, s * 0.08);
	pd_rounded(cr, x + s * 0.12, y + s * 0.26, s * 0.76, s * 0.48, s * 0.08);
	cairo_stroke(cr);
	// the platter
	cairo_new_path(cr);
	cairo_arc(cr, x + s * 0.40, y + s * 0.50, s * 0.13, 0, 2 * M_PI);
	cairo_stroke(cr);
	// the lamp
	cairo_new_path(cr);
	cairo_arc(cr, x + s * 0.72, y + s * 0.50, s * 0.07, 0, 2 * M_PI);
	if (active) {
		cairo_fill(cr);
	} else {
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

void pd_glyph_generic_app(cairo_t *cr, double x, double y, double size) {
	cairo_new_path(cr);
	double s = size / 16.0;
	pd_rect(cr, x + 1 * s, y + 2 * s, 14 * s, 12 * s, 0x505a64ff);
	pd_rect(cr, x + 2 * s, y + 3 * s, 12 * s, 2 * s, 0x2f8fe0ff);
	pd_rect(cr, x + 2 * s, y + 6 * s, 12 * s, 7 * s, 0xf0f0f0ff);
}
