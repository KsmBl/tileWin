#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include "saver_util.h"

/*
 * Microslop: a parody commercial for Copilot− PCs, $16.05 per 6 days. Five
 * scenes, fading into each other and round again: the melting logo, the
 * product, a laptop full of features nobody asked for, the price, and the
 * button to upgrade next to a "Not now" that will not be clicked. All the
 * while a "Skip ad" in the corner counts down and never lets you.
 */

#define SCENE_TIME 9.0
#define SCENES 5
#define FADE 0.8
#define FONT "Segoe UI, Inter, Noto Sans, Cantarell, sans-serif"

struct ad {
	int width, height;
	double u, t;
};

/* ---------- helpers ---------- */

static double ease(double v) {
	v = saver_clamp(v, 0, 1);
	return 1 - pow(1 - v, 3);
}

/* How far in (0..1) something is that starts at start and takes len, eased. */
static double in(double t, double start, double len) {
	return ease((t - start) / len);
}

static PangoLayout *layout_for(cairo_t *cr, const char *text, const char *weight, double px) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	char *font = g_strdup_printf("%s %s %dpx", FONT, weight, (int)fmax(4, px));
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	g_free(font);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER); // lines of a badge centred
	pango_layout_set_markup(layout, text, -1);
	return layout;
}

/* Text anchored at x (0 left, 0.5 middle, 1 right of it) with its top at y; returns its width. */
static double text(cairo_t *cr, const char *markup, const char *weight, double px, double x,
		double y, double anchor, double r, double g, double b, double a) {
	PangoLayout *layout = layout_for(cr, markup, weight, px);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	cairo_move_to(cr, x - w * anchor, y);
	cairo_set_source_rgba(cr, r, g, b, a);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	return w;
}

/* Text filled with a gradient from one colour to the other, left to right. */
static double gradient_text(cairo_t *cr, const char *markup, const char *weight, double px,
		double x, double y, double anchor, const double c0[3], const double c1[3], double a) {
	PangoLayout *layout = layout_for(cr, markup, weight, px);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	double left = x - w * anchor;
	cairo_move_to(cr, left, y);
	pango_cairo_layout_path(cr, layout);
	cairo_pattern_t *p = cairo_pattern_create_linear(left, y, left + w, y + h);
	cairo_pattern_add_color_stop_rgba(p, 0, c0[0], c0[1], c0[2], a);
	cairo_pattern_add_color_stop_rgba(p, 1, c1[0], c1[1], c1[2], a);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	g_object_unref(layout);
	return w;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	r = fmin(r, fmin(w, h) / 2);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* ---------- the background: the soft coloured bloom every ad has ---------- */

static void draw_background(struct ad *s, cairo_t *cr) {
	double W = s->width, H = s->height, t = s->t;
	cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, W, H);
	cairo_pattern_add_color_stop_rgb(bg, 0, 0.03, 0.05, 0.14);
	cairo_pattern_add_color_stop_rgb(bg, 1, 0.07, 0.03, 0.16);
	cairo_set_source(cr, bg);
	cairo_paint(cr);
	cairo_pattern_destroy(bg);
	static const double blobs[4][5] = {
		{ 0.25, 0.35, 0.10, 0.45, 0.95 }, { 0.75, 0.3, 0.55, 0.25, 0.9 },
		{ 0.6, 0.8, 0.1, 0.75, 0.8 }, { 0.2, 0.85, 0.85, 0.35, 0.6 },
	};
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int i = 0; i < 4; i++) {
		double x = W * (blobs[i][0] + 0.08 * sin(t * 0.13 + i * 1.7));
		double y = H * (blobs[i][1] + 0.08 * cos(t * 0.11 + i * 2.3));
		double r = fmax(W, H) * (0.45 + 0.05 * sin(t * 0.2 + i));
		cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 0, x, y, r);
		cairo_pattern_add_color_stop_rgba(p, 0, blobs[i][2], blobs[i][3], blobs[i][4], 0.33);
		cairo_pattern_add_color_stop_rgba(p, 1, blobs[i][2], blobs[i][3], blobs[i][4], 0);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

/* ---------- scene 1: the logo, melting ---------- */

/* A square that does not quite hold its shape, dripping from its lower edge. */
static void sloppy_square(cairo_t *cr, double x, double y, double size, double t, int seed,
		double melt) {
	int n = 24;
	cairo_new_path(cr);
	for (int k = 0; k <= 4 * n; k++) {
		int side = k / n % 4;
		double f = (double)(k % n) / n;
		double px, py;
		switch (side) {
		case 0: px = x + f * size; py = y; break;
		case 1: px = x + size; py = y + f * size; break;
		case 2: px = x + size - f * size; py = y + size; break;
		default: px = x; py = y + size - f * size; break;
		}
		double wob = sin(k * 0.7 + t * 1.6 + seed * 2.1) * size * 0.018 +
			sin(k * 0.23 + t * 0.9 + seed) * size * 0.025;
		px += wob;
		py += wob * 0.8;
		if (side == 2) {
			py += melt * size * 0.12 * (0.5 + 0.5 * sin(f * 9 + seed)); // sagging
		}
		if (k == 0) {
			cairo_move_to(cr, px, py);
		} else {
			cairo_line_to(cr, px, py);
		}
	}
	cairo_close_path(cr);
	cairo_fill(cr);
	// drips running down, lengthening
	for (int d = 0; d < 2; d++) {
		double dx = x + size * (0.25 + 0.5 * d + 0.1 * sin(seed + d));
		double len = size * melt * (0.25 + 0.35 * fmod(seed * 0.37 + d * 0.61, 1));
		double top = y + size * (1 + melt * 0.1);
		double r = size * 0.05;
		cairo_move_to(cr, dx - r, top - r);
		cairo_line_to(cr, dx - r * 0.7, top + len);
		cairo_arc_negative(cr, dx, top + len, r * 0.7 * 1.3, M_PI, 0);
		cairo_line_to(cr, dx + r, top - r);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
}

static void scene_logo(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double size = u * 120, gap = u * 12;
	double total = size * 2 + gap;
	double lx = W / 2 - total - u * 40, ly = H / 2 - total / 2 - u * 40;
	static const double colors[4][3] = { { 0.95, 0.33, 0.13 }, { 0.5, 0.75, 0.1 },
		{ 0.0, 0.64, 0.94 }, { 1, 0.73, 0.02 } };
	double melt = saver_clamp((t - 2.5) / 6, 0, 1);
	for (int i = 0; i < 4; i++) {
		double a = in(t, 0.2 + i * 0.18, 0.6);
		double drop = (1 - a) * -u * 60;
		cairo_set_source_rgba(cr, colors[i][0], colors[i][1], colors[i][2], a);
		sloppy_square(cr, lx + (i % 2) * (size + gap), ly + (i / 2) * (size + gap) + drop, size,
			s->t, i + 3, melt);
	}
	double a = in(t, 1.1, 0.8);
	text(cr, "Microslop", "Semi-Light", u * 150, lx + total + u * 50 - (1 - a) * u * 40,
		ly + total / 2 - u * 100, 0, 1, 1, 1, a);
	double b = in(t, 2.4, 0.8);
	text(cr, "Where do you want to be sent today?", "Light", u * 38, W / 2, ly + total + u * 110,
		0.5, 0.85, 0.88, 0.95, b);
}

/* ---------- scene 2: Copilot− PCs ---------- */

/* An abstract swirl of two ribbons, with the minus that makes it what it is. */
static void swirl_icon(cairo_t *cr, double cx, double cy, double r, double t, double a) {
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_rotate(cr, sin(t * 0.8) * 0.15);
	static const double ribbons[2][6] = { { 0.1, 0.8, 0.85, 0.2, 0.35, 0.95 },
		{ 0.7, 0.25, 0.95, 1, 0.55, 0.2 } };
	for (int i = 0; i < 2; i++) {
		cairo_save(cr);
		cairo_rotate(cr, i * M_PI);
		cairo_new_path(cr);
		cairo_arc(cr, r * 0.28, 0, r * 0.62, -M_PI * 0.55, M_PI * 0.45);
		cairo_arc_negative(cr, r * 0.28, 0, r * 0.28, M_PI * 0.45, -M_PI * 0.55);
		cairo_close_path(cr);
		cairo_pattern_t *p = cairo_pattern_create_linear(-r, -r, r, r);
		cairo_pattern_add_color_stop_rgba(p, 0, ribbons[i][0], ribbons[i][1], ribbons[i][2], a);
		cairo_pattern_add_color_stop_rgba(p, 1, ribbons[i][3], ribbons[i][4], ribbons[i][5], a);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		cairo_restore(cr);
	}
	cairo_restore(cr);
	// the minus badge
	double br = r * 0.36, bx = cx + r * 0.72, by = cy + r * 0.62;
	cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.9, 0.12, 0.2, a);
	cairo_fill(cr);
	cairo_rectangle(cr, bx - br * 0.55, by - br * 0.13, br * 1.1, br * 0.26);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_fill(cr);
}

static void scene_product(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.2, 0.7);
	text(cr, "Introducing", "Light", u * 44, W / 2, H * 0.2 - (1 - a) * u * 20, 0.5, 0.85, 0.9,
		1, a);
	double b = in(t, 0.8, 1.0);
	swirl_icon(cr, W / 2, H * 0.43, u * 120 * (0.7 + 0.3 * b), s->t, b);
	double c = in(t, 1.6, 0.9);
	static const double c0[3] = { 0.35, 0.8, 1 }, c1[3] = { 0.85, 0.5, 1 };
	gradient_text(cr, "Copilot− PCs", "Bold", u * 130, W / 2, H * 0.58 + (1 - c) * u * 30, 0.5,
		c0, c1, c);
	double d = in(t, 3.0, 0.9);
	text(cr, "All the AI you didn't ask for. <b>Now with less.</b>", "Light", u * 40, W / 2,
		H * 0.58 + u * 180, 0.5, 1, 1, 1, d);
	double e = in(t, 4.6, 0.9);
	text(cr, "Copilot− is Copilot, minus the parts that worked.", "Light", u * 26, W / 2,
		H * 0.58 + u * 245, 0.5, 0.7, 0.75, 0.85, e);
}

/* ---------- scene 3: the laptop and its features ---------- */

static void laptop(struct ad *s, cairo_t *cr, double x, double y, double w, double t) {
	double u = s->u, h = w * 0.62;
	// the screen
	rounded(cr, x, y, w, h, u * 14);
	cairo_set_source_rgb(cr, 0.12, 0.13, 0.16);
	cairo_fill(cr);
	double sx = x + w * 0.035, sy = y + w * 0.035, sw = w * 0.93, sh = h - w * 0.07;
	cairo_pattern_t *wall = cairo_pattern_create_linear(sx, sy, sx + sw, sy + sh);
	cairo_pattern_add_color_stop_rgb(wall, 0, 0.1, 0.3, 0.75);
	cairo_pattern_add_color_stop_rgb(wall, 1, 0.45, 0.2, 0.7);
	cairo_rectangle(cr, sx, sy, sw, sh);
	cairo_set_source(cr, wall);
	cairo_fill(cr);
	cairo_pattern_destroy(wall);
	// a start menu of nothing but ads
	double mw = sw * 0.46, mh = sh * 0.78, mx = sx + sw * 0.27, my = sy + sh * 0.08;
	rounded(cr, mx, my, mw, mh, u * 8);
	cairo_set_source_rgba(cr, 0.96, 0.96, 0.98, 0.95);
	cairo_fill(cr);
	static const double tile[6][3] = { { 0.9, 0.3, 0.2 }, { 0.2, 0.6, 0.9 }, { 0.95, 0.7, 0.1 },
		{ 0.3, 0.75, 0.4 }, { 0.7, 0.3, 0.8 }, { 0.2, 0.2, 0.25 } };
	for (int i = 0; i < 12; i++) {
		double tw = mw * 0.2, th = mh * 0.2;
		double tx = mx + mw * 0.06 + (i % 4) * mw * 0.225, ty = my + mh * 0.1 + (i / 4) * mh * 0.26;
		rounded(cr, tx, ty, tw, th, u * 4);
		const double *c = tile[(i * 5 + (int)(t * 1.5)) % 6];
		cairo_set_source_rgb(cr, c[0], c[1], c[2]);
		cairo_fill(cr);
		text(cr, "AD", "Bold", th * 0.4, tx + tw / 2, ty + th * 0.26, 0.5, 1, 1, 1, 0.9);
	}
	// the base and the keyboard, with the key that matters
	double by = y + h;
	cairo_move_to(cr, x - w * 0.06, by + w * 0.06);
	cairo_line_to(cr, x + w * 1.06, by + w * 0.06);
	cairo_line_to(cr, x + w, by);
	cairo_line_to(cr, x, by);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.72, 0.74, 0.78);
	cairo_fill(cr);
	for (int k = 0; k < 14; k++) {
		double kx = x + w * (0.05 + k * 0.065), kw = w * 0.05;
		bool special = k == 9;
		cairo_rectangle(cr, kx, by + w * 0.012, kw, w * 0.018);
		if (special) {
			double glow = 0.6 + 0.4 * sin(t * 5);
			cairo_set_source_rgba(cr, 0.4, 0.8, 1, glow);
		} else {
			cairo_set_source_rgb(cr, 0.3, 0.31, 0.34);
		}
		cairo_fill(cr);
	}
	double kx = x + w * (0.05 + 9 * 0.065);
	text(cr, "C−", "Bold", w * 0.018, kx + w * 0.025, by + w * 0.011, 0.5, 0.05, 0.1, 0.2, 1);
}

static void scene_features(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.1, 0.9);
	laptop(s, cr, W * 0.07 - (1 - a) * u * 200, H * 0.26, W * 0.4, s->t);
	static const char *const features[] = {
		"<b>Recall−</b> remembers everything. Especially your passwords.",
		"<b>40 TOPS</b> of confidently wrong answers",
		"<b>Ads</b> in the Start menu, the lock screen and Notepad",
		"<b>Updates</b> timed for the middle of your presentation",
		"<b>Local account?</b> Never heard of it.",
	};
	text(cr, "What's new", "Semi-Light", u * 54, W * 0.53, H * 0.2, 0, 1, 1, 1, in(t, 0.5, 0.7));
	for (int i = 0; i < 5; i++) {
		double f = in(t, 1.2 + i * 1.1, 0.7);
		double y = H * 0.33 + i * u * 72;
		double x = W * 0.53 + (1 - f) * u * 80;
		cairo_arc(cr, x + u * 10, y + u * 17, u * 7, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 0.4, 0.8, 1, f);
		cairo_fill(cr);
		text(cr, features[i], "Light", u * 28, x + u * 32, y, 0, 1, 1, 1, f);
	}
}

/* ---------- scene 4: the price ---------- */

static void starburst(cairo_t *cr, double cx, double cy, double r, double spin, double a) {
	int points = 16;
	cairo_new_path(cr);
	for (int k = 0; k < points * 2; k++) {
		double ang = spin + k * M_PI / points;
		double rr = k % 2 ? r * 0.8 : r;
		cairo_line_to(cr, cx + cos(ang) * rr, cy + sin(ang) * rr);
	}
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 1, 0.8, 0.1, a);
	cairo_fill(cr);
}

static void scene_price(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.1, 0.6);
	text(cr, "Only", "Light", u * 48, W / 2, H * 0.17, 0.5, 0.85, 0.9, 1, a);
	// the price rolls up to what it is
	double roll = in(t, 0.6, 1.8);
	char price[64];
	snprintf(price, sizeof(price), "$%.2f", 16.05 * roll);
	static const double c0[3] = { 1, 1, 1 }, c1[3] = { 0.7, 0.85, 1 };
	double pw = gradient_text(cr, price, "Bold", u * 220, W / 2 - u * 90, H * 0.26, 0.5, c0, c1,
		in(t, 0.6, 0.4));
	text(cr, "/ 6 days*", "Semi-Light", u * 64, W / 2 - u * 90 + pw / 2 + u * 20, H * 0.26 + u * 150,
		0, 0.85, 0.9, 1, in(t, 2.2, 0.6));
	// the old price, crossed out: a whole cent less
	double c = in(t, 2.8, 0.6);
	double ow = text(cr, "$16.04", "Light", u * 40, W / 2 - u * 330, H * 0.22, 0.5, 0.75, 0.75,
		0.8, c);
	cairo_move_to(cr, W / 2 - u * 330 - ow / 2 - u * 4, H * 0.22 + u * 28);
	cairo_line_to(cr, W / 2 - u * 330 - (ow / 2 + u * 4) + (ow + u * 8) * c, H * 0.22 + u * 22);
	cairo_set_source_rgba(cr, 0.95, 0.2, 0.25, c);
	cairo_set_line_width(cr, u * 4);
	cairo_stroke(cr);
	// the badge
	double b = in(t, 1.5, 0.5);
	double bx = W / 2 + u * 420, by = H * 0.3;
	starburst(cr, bx, by, u * 95 * b * (1 + 0.05 * sin(s->t * 6)), s->t * 0.8, b);
	text(cr, "LIMITED\nTIME", "Heavy", u * 30, bx, by - u * 36, 0.5, 0.6, 0.1, 0.05, b);
	double d = in(t, 3.4, 0.7);
	text(cr, "That's just <b>$976.38</b> a year!", "Light", u * 44, W / 2, H * 0.62, 0.5, 1, 1, 1, d);
	// an offer that ends soon, every time
	int left = 299 - (int)fmod(s->t * 7, 300);
	char timer[64];
	snprintf(timer, sizeof(timer), "Offer ends in <b>00:%02d:%02d</b>", left / 60, left % 60);
	text(cr, timer, "Light", u * 30, W / 2, H * 0.62 + u * 80, 0.5, 1, 0.75, 0.3, in(t, 4.2, 0.6));
	text(cr, "*Billed every 6 days, in advance. Also in arrears.", "Light", u * 20, W / 2,
		H * 0.62 + u * 140, 0.5, 0.6, 0.65, 0.75, in(t, 5, 0.6));
}

/* ---------- scene 5: the call to action ---------- */

static void scene_upgrade(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.1, 0.8);
	text(cr, "Upgrade now.", "Semi-Light", u * 90, W / 2, H * 0.2, 0.5, 1, 1, 1, a);
	text(cr, "You were going to anyway.", "Light", u * 38, W / 2, H * 0.2 + u * 115, 0.5, 0.8, 0.85,
		0.95, in(t, 1, 0.8));
	// the button, pulsing
	double b = in(t, 1.6, 0.6);
	double pulse = 1 + 0.04 * sin(s->t * 4);
	double bw = u * 360 * pulse, bh = u * 90 * pulse, bx = W / 2 - bw / 2, by = H * 0.48;
	rounded(cr, bx, by, bw, bh, bh / 2);
	cairo_pattern_t *p = cairo_pattern_create_linear(bx, by, bx + bw, by + bh);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.1, 0.55, 1, b);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.55, 0.3, 1, b);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	text(cr, "Get Copilot−", "Semi-Bold", u * 40, W / 2, by + bh / 2 - u * 26, 0.5, 1, 1, 1, b);
	// "Not now": small, grey, and never where the pointer is
	double c = in(t, 2.4, 0.6);
	double chase = saver_clamp((t - 3.5) / 4, 0, 1);
	double nx = W / 2 + sin(chase * 17) * u * 260 * chase, ny = by + bh + u * 70 + cos(chase * 11) * u * 40 * chase;
	text(cr, "Not now", "Light", u * 20, nx, ny, 0.5, 0.55, 0.58, 0.65, c * 0.8);
	if (t > 3.5) {
		// a pointer, forever just too late
		double px = W / 2 + sin((chase - 0.08) * 17) * u * 260 * chase + u * 30;
		double py = ny + u * 30;
		cairo_move_to(cr, px, py);
		cairo_line_to(cr, px, py + u * 34);
		cairo_line_to(cr, px + u * 9, py + u * 26);
		cairo_line_to(cr, px + u * 16, py + u * 40);
		cairo_line_to(cr, px + u * 22, py + u * 37);
		cairo_line_to(cr, px + u * 15, py + u * 24);
		cairo_line_to(cr, px + u * 25, py + u * 24);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, u * 1.5);
		cairo_stroke(cr);
	}
	// the fine print, going by far too fast
	static const char *const fine =
		"Copilot− features require a Microslop account, an internet connection, a TPM 4.0 chip "
		"and consent you already gave. Price excludes the price. Your data may be used to "
		"improve your data. Not valid anywhere. Cancel by fax between 3:00 and 3:05 am on the "
		"sixth day. Copilot− may produce inaccurate information about people, places, facts and "
		"this offer. Recall− cannot be turned off because it was never on, as far as you know. "
		"The Copilot− key may be remapped to Copilot− at any time. Results not typical. "
		"No refunds, but plenty of reminders.";
	PangoLayout *layout = layout_for(cr, fine, "Light", u * 16);
	int fw, fh;
	pango_layout_get_pixel_size(layout, &fw, &fh);
	double fx = W - fmod(s->t * u * 900, fw + W);
	cairo_move_to(cr, fx, H - u * 60);
	cairo_set_source_rgba(cr, 0.7, 0.72, 0.8, in(t, 0.5, 0.5) * 0.8);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

/* ---------- always there: the "Ad" tag, and a skip button that never skips ---------- */

static void draw_chrome(struct ad *s, cairo_t *cr) {
	double W = s->width, u = s->u;
	rounded(cr, u * 24, u * 24, u * 56, u * 30, u * 6);
	cairo_set_source_rgba(cr, 1, 0.8, 0.1, 0.95);
	cairo_fill(cr);
	text(cr, "Ad", "Bold", u * 20, u * 52, u * 26, 0.5, 0.1, 0.1, 0.1, 1);
	int n = 5 - (int)fmod(s->t, 6);
	char skip[64];
	if (n > 0) {
		snprintf(skip, sizeof(skip), "Skip ad in %d", n);
	} else {
		snprintf(skip, sizeof(skip), "Skip ad in 5");
	}
	double bw = u * 190, bh = u * 50, bx = W - bw - u * 30, by = s->height - bh - u * 110;
	rounded(cr, bx, by, bw, bh, u * 6);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
	cairo_set_line_width(cr, u * 1.5);
	cairo_stroke(cr);
	text(cr, skip, "Semi-Bold", u * 22, bx + bw / 2, by + u * 12, 0.5, 1, 1, 1, 0.9);
}

/* ---------- the saver ---------- */

typedef void (*scene_fn)(struct ad *s, cairo_t *cr, double t);
static const scene_fn scenes[SCENES] = { scene_logo, scene_product, scene_features, scene_price,
	scene_upgrade };

static void *ad_create(int width, int height, const struct saver_options *options) {
	struct ad *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->u = saver_unit(width, height) * (width > height * 1.2 ? 1 : 0.8);
	return s;
}

static void ad_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct ad *s = state;
	s->t += dt;
	draw_background(s, cr);
	double cycle = fmod(s->t, SCENE_TIME * SCENES);
	int scene = (int)(cycle / SCENE_TIME);
	double local = cycle - scene * SCENE_TIME;
	// each scene fades in and out, over the one before for a moment
	double alpha = saver_clamp(local / FADE, 0, 1) * saver_clamp((SCENE_TIME - local) / FADE, 0, 1);
	cairo_push_group(cr);
	scenes[scene](s, cr, local);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, alpha);
	draw_chrome(s, cr);
}

static void ad_destroy(void *state) {
	free(state);
}

const struct saver saver_ad = {
	.name = "microslop",
	.title = "Microslop Ad",
	.description = "A commercial for Copilot− PCs, only $16.05 per 6 days",
	.create = ad_create,
	.draw = ad_draw,
	.destroy = ad_destroy,
};
