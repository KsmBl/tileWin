#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include "saver_util.h"

/*
 * Microslop: a parody commercial for Copilot− PCs, $16.05 per 6 days, about
 * two minutes and round again. The melting logo; Brenda, a satisfied customer
 * who does not exist; the product; a laptop full of features nobody asked for;
 * Recall− playing back your day; Slippy the paperclip, who would like to help
 * you stay; your PC against a Copilot− PC; the blue screen, smiling; the
 * price; your privacy, which is worth a lot; the button to upgrade next to a
 * "Not now" that will not be clicked; and the end card. All the while a "Skip
 * ad" counts down and never lets you, and now and then updates are ready.
 */

#define FADE 0.8
#define FONT "Segoe UI, Inter, Noto Sans, Cantarell, sans-serif"

#define BLOOM_SCALE 8 // the background is drawn this much smaller, and scaled up

struct ad {
	int width, height;
	double u, t;
	cairo_surface_t *bloom; // the soft background, small
	cairo_surface_t *backdrop; // and scaled up to the screen, made again now and then
	double backdrop_t;
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
	// the lines of a text anchored at a side line up on that side
	pango_layout_set_alignment(layout, anchor == 0 ? PANGO_ALIGN_LEFT :
		anchor == 1 ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_CENTER);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	cairo_move_to(cr, x - w * anchor, y);
	cairo_set_source_rgba(cr, r, g, b, a);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	return w;
}

/*
 * A headline: a soft shadow under it, a gradient in it, and now and then a
 * streak of light passing over it (shine from 0 to 1 while it passes).
 */
static double headline(cairo_t *cr, const char *markup, const char *weight, double px, double x,
		double y, double anchor, const double c0[3], const double c1[3], double a, double shine) {
	PangoLayout *layout = layout_for(cr, markup, weight, px);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	double left = x - w * anchor;
	for (int k = 3; k >= 1; k--) {
		cairo_move_to(cr, left, y + px * 0.02 * k);
		cairo_set_source_rgba(cr, 0, 0, 0.05, a * 0.12);
		pango_cairo_show_layout(cr, layout);
	}
	cairo_move_to(cr, left, y);
	pango_cairo_layout_path(cr, layout);
	cairo_pattern_t *p = cairo_pattern_create_linear(left, y, left + w, y + h);
	cairo_pattern_add_color_stop_rgba(p, 0, c0[0], c0[1], c0[2], a);
	cairo_pattern_add_color_stop_rgba(p, 1, c1[0], c1[1], c1[2], a);
	cairo_set_source(cr, p);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	if (shine > 0 && shine < 1) {
		double sx = left - w * 0.3 + (w * 1.6) * shine;
		cairo_pattern_t *streak = cairo_pattern_create_linear(sx - px, y, sx + px, y + h * 0.3);
		cairo_pattern_add_color_stop_rgba(streak, 0, 1, 1, 1, 0);
		cairo_pattern_add_color_stop_rgba(streak, 0.5, 1, 1, 1, 0.55 * a);
		cairo_pattern_add_color_stop_rgba(streak, 1, 1, 1, 1, 0);
		cairo_set_source(cr, streak);
		cairo_fill(cr);
		cairo_pattern_destroy(streak);
	}
	cairo_new_path(cr);
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

/*
 * The soft coloured glow behind it all. It has no edges, so it is drawn an
 * eighth of the size and scaled up: full size, four gradients over the whole
 * screen took the software renderer longer than a frame lasts. It drifts so
 * slowly that making it four times a second is as good as every frame; the
 * frames between only copy it.
 */
static void draw_background(struct ad *s, cairo_t *screen) {
	if (s->backdrop && s->t - s->backdrop_t < 0.25 && s->t >= s->backdrop_t) {
		cairo_save(screen);
		cairo_set_source_surface(screen, s->backdrop, 0, 0);
		cairo_set_operator(screen, CAIRO_OPERATOR_SOURCE);
		cairo_paint(screen);
		cairo_restore(screen);
		return;
	}
	s->backdrop_t = s->t;
	if (!s->backdrop) {
		s->backdrop = cairo_image_surface_create(CAIRO_FORMAT_RGB24, s->width, s->height);
	}
	cairo_t *out = cairo_create(s->backdrop);
	if (!s->bloom) {
		s->bloom = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
			s->width / BLOOM_SCALE + 2, s->height / BLOOM_SCALE + 2);
	}
	cairo_t *cr = cairo_create(s->bloom);
	cairo_scale(cr, 1.0 / BLOOM_SCALE, 1.0 / BLOOM_SCALE);
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
	cairo_destroy(cr);
	cairo_surface_mark_dirty(s->bloom);
	cairo_save(out);
	cairo_scale(out, BLOOM_SCALE, BLOOM_SCALE);
	cairo_set_source_surface(out, s->bloom, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(out), CAIRO_FILTER_BILINEAR);
	cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
	cairo_paint(out);
	cairo_restore(out);
	cairo_destroy(out);
	draw_background(s, screen); // and onto the screen
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
	headline(cr, "Copilot− PCs", "Bold", u * 130, W / 2, H * 0.58 + (1 - c) * u * 30, 0.5,
		c0, c1, c, (t - 2.6) / 1.2);
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
	double pw = headline(cr, price, "Bold", u * 220, W / 2 - u * 90, H * 0.26, 0.5, c0, c1,
		in(t, 0.6, 0.4), (t - 2.6) / 1.1);
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
	double e = in(t, 6.3, 0.6);
	rounded(cr, W / 2 - u * 330, H * 0.62 + u * 195, u * 660, u * 66, u * 33);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.1 * e);
	cairo_fill(cr);
	text(cr, "New: <b>Copilot−− PCs</b>, $32.10 per 12 days. Best value!*", "Light", u * 26, W / 2,
		H * 0.62 + u * 211, 0.5, 1, 1, 1, e);
	text(cr, "*Same value.", "Light", u * 18, W / 2, H * 0.62 + u * 275, 0.5, 0.6, 0.65, 0.75,
		in(t, 7.6, 0.5));
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

/* ---------- a satisfied customer ---------- */

static void scene_testimonial(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.2, 0.8);
	double ax = W * 0.27, ay = H * 0.45, ar = u * 150;
	// her portrait: soft light behind, a figure in front
	cairo_pattern_t *halo = cairo_pattern_create_radial(ax, ay, 0, ax, ay, ar * 1.6);
	cairo_pattern_add_color_stop_rgba(halo, 0, 1, 0.8, 0.6, 0.35 * a);
	cairo_pattern_add_color_stop_rgba(halo, 1, 1, 0.8, 0.6, 0);
	cairo_set_source(cr, halo);
	cairo_arc(cr, ax, ay, ar * 1.6, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(halo);
	cairo_save(cr);
	cairo_arc(cr, ax, ay, ar, 0, 2 * M_PI);
	cairo_clip(cr);
	cairo_pattern_t *bg = cairo_pattern_create_linear(ax, ay - ar, ax, ay + ar);
	cairo_pattern_add_color_stop_rgba(bg, 0, 0.95, 0.7, 0.55, a);
	cairo_pattern_add_color_stop_rgba(bg, 1, 0.7, 0.35, 0.6, a);
	cairo_set_source(cr, bg);
	cairo_paint(cr);
	cairo_pattern_destroy(bg);
	cairo_set_source_rgba(cr, 0.18, 0.12, 0.22, a);
	cairo_arc(cr, ax, ay - ar * 0.15, ar * 0.36, 0, 2 * M_PI); // head
	cairo_fill(cr);
	cairo_arc(cr, ax, ay + ar * 0.95, ar * 0.72, M_PI, 2 * M_PI); // shoulders
	cairo_fill(cr);
	// a smile that is just a little too wide
	cairo_set_source_rgba(cr, 1, 1, 1, 0.8 * a);
	cairo_set_line_width(cr, u * 4);
	cairo_arc(cr, ax, ay - ar * 0.15, ar * 0.2, 0.15 * M_PI, 0.85 * M_PI);
	cairo_stroke(cr);
	cairo_restore(cr);
	// the quote, typed out
	double qx = W * 0.45;
	text(cr, "“", "Bold", u * 180, qx - u * 20, H * 0.14, 0, 0.55, 0.75, 1, in(t, 0.6, 0.5));
	const char *quote = "I used to think for myself. Now Copilot− does it for me. Slightly worse.";
	long n = g_utf8_strlen(quote, -1);
	long shown = (long)saver_clamp((t - 1.0) * 22, 0, n);
	char *part = g_strndup(quote, g_utf8_offset_to_pointer(quote, shown) - quote);
	PangoLayout *layout = layout_for(cr, "", "Light", u * 46);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_width(layout, (int)(W * 0.46 * PANGO_SCALE));
	pango_layout_set_text(layout, part, -1);
	cairo_move_to(cr, qx + u * 40, H * 0.3);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	g_free(part);
	double b = in(t, 4.8, 0.6);
	text(cr, "— Brenda, 34, a real customer*", "Semi-Bold", u * 28, qx + u * 40, H * 0.58, 0,
		0.85, 0.9, 1, b);
	// her rating
	double c = in(t, 5.6, 0.6);
	for (int i = 0; i < 5; i++) {
		double sx = qx + u * 52 + i * u * 44, sy = H * 0.66 + u * 18;
		cairo_new_path(cr);
		for (int k = 0; k < 10; k++) {
			double ang = -M_PI / 2 + k * M_PI / 5, rr = k % 2 ? u * 8 : u * 19;
			cairo_line_to(cr, sx + cos(ang) * rr, sy + sin(ang) * rr);
		}
		cairo_close_path(cr);
		double fill = i < 4 ? 1 : 0.2;
		cairo_set_source_rgba(cr, 1, 0.78, 0.15, c * fill + c * 0.15);
		cairo_fill(cr);
	}
	text(cr, "4.2 from 3 reviews, 2 of them by us", "Light", u * 24, qx + u * 280, H * 0.66, 0,
		0.8, 0.82, 0.9, c);
	text(cr, "*Brenda is AI-generated. Any resemblance to real Brendas is a feature.", "Light",
		u * 20, W / 2, H * 0.84, 0.5, 0.6, 0.65, 0.75, in(t, 6.6, 0.6));
}

/* ---------- Recall−: your day, played back ---------- */

static void scene_recall(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	static const double c0[3] = { 0.5, 0.9, 1 }, c1[3] = { 0.6, 0.55, 1 };
	double a = in(t, 0.1, 0.7);
	headline(cr, "Recall−", "Bold", u * 100, W / 2, H * 0.08, 0.5, c0, c1, a, (t - 1) / 1.2);
	text(cr, "Never forget. Never be allowed to.", "Light", u * 36, W / 2, H * 0.08 + u * 130, 0.5,
		0.85, 0.9, 1, in(t, 0.8, 0.7));
	// a strip of little screenshots, gliding by
	double cw = u * 230, ch = u * 140, gap = u * 30, sy = H * 0.42;
	double shift = fmod(s->t * u * 60, cw + gap);
	static const double tint[5][3] = { { 0.2, 0.45, 0.85 }, { 0.85, 0.35, 0.3 }, { 0.3, 0.7, 0.45 },
		{ 0.6, 0.35, 0.8 }, { 0.9, 0.65, 0.2 } };
	double b = in(t, 0.6, 0.8);
	for (int i = -1; i < (int)(W / (cw + gap)) + 2; i++) {
		double x = i * (cw + gap) - shift;
		int k = (i + (int)(s->t * u * 60 / (cw + gap)) + 50) % 5;
		rounded(cr, x, sy, cw, ch, u * 10);
		cairo_set_source_rgba(cr, 0.95, 0.96, 0.98, 0.9 * b);
		cairo_fill(cr);
		cairo_rectangle(cr, x + u * 8, sy + u * 8, cw - u * 16, u * 16);
		cairo_set_source_rgba(cr, tint[k][0], tint[k][1], tint[k][2], b);
		cairo_fill(cr);
		for (int l = 0; l < 4; l++) {
			cairo_rectangle(cr, x + u * 14, sy + u * (40 + l * 22), (cw - u * 28) * (0.5 + 0.1 * ((k + l) % 5)), u * 9);
			cairo_set_source_rgba(cr, 0.6, 0.62, 0.7, 0.8 * b);
			cairo_fill(cr);
		}
	}
	// the timeline, and the playhead going through the day
	double ty = sy + ch + u * 40;
	cairo_rectangle(cr, W * 0.1, ty, W * 0.8, u * 4);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.25 * b);
	cairo_fill(cr);
	double head = W * 0.1 + W * 0.8 * saver_clamp((t - 1) / 7.5, 0, 1);
	cairo_rectangle(cr, W * 0.1, ty, head - W * 0.1, u * 4);
	cairo_set_source_rgba(cr, 0.45, 0.8, 1, b);
	cairo_fill(cr);
	cairo_arc(cr, head, ty + u * 2, u * 11, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, b);
	cairo_fill(cr);
	static const char *const moments[] = {
		"<b>3:14 pm</b>  You searched “how to uninstall Copilot−”",
		"<b>3:15 pm</b>  Copilot− reinstalled itself",
		"<b>3:16 pm</b>  Recall− saved your bank password, for convenience",
	};
	for (int i = 0; i < 3; i++) {
		double m = in(t, 2 + i * 2.1, 0.5) * (1 - in(t, 3.9 + i * 2.1, 0.4) * (i < 2));
		text(cr, moments[i], "Light", u * 34, W / 2, ty + u * 50 + (1 - m) * u * 16, 0.5, 1, 1, 1, m);
	}
}

/* ---------- Slippy, who would like to help ---------- */

static void paperclip(cairo_t *cr, double x, double y, double s, double a, double t) {
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_rotate(cr, sin(t * 1.3) * 0.06);
	// the wire: three loops inside each other
	cairo_new_path(cr);
	cairo_move_to(cr, s * 0.1, s * 1.2);
	cairo_line_to(cr, s * 0.1, s * 0.25);
	cairo_arc(cr, s * 0.4, s * 0.25, s * 0.3, M_PI, 2 * M_PI);
	cairo_line_to(cr, s * 0.7, s * 1.35);
	cairo_arc(cr, s * 0.45, s * 1.35, s * 0.25, 0, M_PI);
	cairo_line_to(cr, s * 0.2 + 0, s * 0.45);
	cairo_arc(cr, s * 0.4, s * 0.45, s * 0.2, M_PI, 2 * M_PI);
	cairo_line_to(cr, s * 0.6, s * 1.15);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cr, s * 0.1);
	cairo_set_source_rgba(cr, 0.25, 0.27, 0.32, a);
	cairo_stroke_preserve(cr);
	cairo_set_line_width(cr, s * 0.06);
	cairo_pattern_t *metal = cairo_pattern_create_linear(0, 0, s, s);
	cairo_pattern_add_color_stop_rgba(metal, 0, 0.95, 0.96, 1, a);
	cairo_pattern_add_color_stop_rgba(metal, 0.5, 0.62, 0.65, 0.72, a);
	cairo_pattern_add_color_stop_rgba(metal, 1, 0.9, 0.92, 0.96, a);
	cairo_set_source(cr, metal);
	cairo_stroke(cr);
	cairo_pattern_destroy(metal);
	// the eyes, and the eyebrows that give it away
	double blink = fmod(t, 3.7) < 0.12 ? 0.15 : 1;
	for (int e = 0; e < 2; e++) {
		double ex = s * (0.22 + e * 0.38), ey = s * 0.58;
		cairo_save(cr);
		cairo_translate(cr, ex, ey);
		cairo_scale(cr, 1, blink);
		cairo_arc(cr, 0, 0, s * 0.14, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, a);
		cairo_fill(cr);
		cairo_arc(cr, ex + s * 0.03, ey + s * 0.02, s * 0.06 * blink, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 0.05, 0.05, 0.1, a);
		cairo_fill(cr);
		cairo_move_to(cr, ex - s * 0.12, ey - s * 0.2 - e * s * 0.04);
		cairo_line_to(cr, ex + s * 0.12, ey - s * 0.24 + e * s * 0.04);
		cairo_set_line_width(cr, s * 0.035);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

static void bubble(cairo_t *cr, double x, double y, double w, double h, double u, double a) {
	rounded(cr, x, y, w, h, u * 18);
	cairo_move_to(cr, x + u * 30, y + h);
	cairo_line_to(cr, x - u * 30, y + h + u * 40);
	cairo_line_to(cr, x + u * 80, y + h);
	cairo_set_source_rgba(cr, 1, 0.98, 0.84, 0.97 * a);
	cairo_fill(cr);
}

static void button(cairo_t *cr, const char *label, double x, double y, double w, double u,
		double a, bool primary) {
	rounded(cr, x, y, w, u * 52, u * 8);
	if (primary) {
		cairo_set_source_rgba(cr, 0.1, 0.45, 0.9, a);
	} else {
		cairo_set_source_rgba(cr, 0.88, 0.89, 0.92, a);
	}
	cairo_fill(cr);
	text(cr, label, "Semi-Bold", u * 22, x + w / 2, y + u * 12, 0.5, primary ? 1 : 0.1,
		primary ? 1 : 0.1, primary ? 1 : 0.15, a);
}

static void scene_slippy(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.1, 0.6);
	double bounce = t < 1.2 ? (1 - in(t, 0.1, 1.1)) * -u * 300 + sin(t * 9) * u * 20 * (1 - in(t, 0.1, 1.1)) : 0;
	paperclip(cr, W * 0.2, H * 0.36 + bounce, u * 230, a, s->t);
	text(cr, "Slippy", "Semi-Bold", u * 30, W * 0.2 + u * 100, H * 0.36 + u * 390, 0.5, 1, 1, 1,
		in(t, 1, 0.5));
	double b = in(t, 1.2, 0.5);
	double bx = W * 0.42, by = H * 0.2, bw = W * 0.46, bh = u * 250;
	bubble(cr, bx, by, bw, bh, u, b);
	bool second = t > 5.2;
	const char *line = second ?
		"I'm back! Now powered by AI, and still without an off switch." :
		"It looks like you're trying to leave. Would you like help staying?";
	PangoLayout *layout = layout_for(cr, line, "Normal", u * 34);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_width(layout, (int)((bw - u * 60) * PANGO_SCALE));
	cairo_move_to(cr, bx + u * 30, by + u * 30);
	double la = second ? in(t, 5.2, 0.4) : b * (1 - in(t, 4.9, 0.3));
	cairo_set_source_rgba(cr, 0.1, 0.1, 0.12, la);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	double c = in(t, 2.4, 0.4) * (1 - in(t, 4.9, 0.3));
	button(cr, "Yes", bx + u * 30, by + bh - u * 80, u * 140, u, c, true);
	button(cr, "Yes, but later", bx + u * 190, by + bh - u * 80, u * 230, u, c, false);
	text(cr, "Slippy is on by default. Slippy is also on by other means.", "Light", u * 22, W / 2,
		H * 0.84, 0.5, 0.6, 0.65, 0.75, in(t, 6.3, 0.6));
}

/* ---------- your PC against a Copilot− PC ---------- */

static void scene_compare(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	static const double c0[3] = { 1, 1, 1 }, c1[3] = { 0.75, 0.85, 1 };
	headline(cr, "Why upgrade?", "Semi-Bold", u * 70, W / 2, H * 0.08, 0.5, c0, c1, in(t, 0.1, 0.6),
		(t - 0.8) / 1.2);
	static const char *const rows[][3] = {
		{ "Starts up in", "3 minutes", "4 seconds*" },
		{ "Asks you to sign in", "Never", "Every start, twice" },
		{ "Remembers", "Nothing", "Everything, for ever" },
		{ "Blue screens", "Blue", "Copilot− Blue™" },
		{ "Can be turned off", "Yes", "Define “off”" },
	};
	double tx = W * 0.14, tw = W * 0.72, col1 = tx + tw * 0.42, col2 = tx + tw * 0.74;
	double ty = H * 0.25, rh = u * 78;
	double a = in(t, 0.5, 0.6);
	// the Copilot− column, lifted out
	rounded(cr, col2 - tw * 0.15, ty - u * 20, tw * 0.3, rh * 6 + u * 30, u * 18);
	cairo_pattern_t *p = cairo_pattern_create_linear(0, ty, 0, ty + rh * 6);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.25, 0.5, 1, 0.45 * a);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.55, 0.3, 0.95, 0.35 * a);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	text(cr, "Your PC", "Semi-Bold", u * 30, col1, ty, 0.5, 0.75, 0.78, 0.85, a);
	text(cr, "Copilot− PC", "Bold", u * 30, col2, ty, 0.5, 1, 1, 1, a);
	for (int i = 0; i < 5; i++) {
		double r = in(t, 1.2 + i * 0.9, 0.5);
		double y = ty + rh * (i + 1);
		cairo_rectangle(cr, tx, y - u * 12, tw, u * 1);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.15 * r);
		cairo_fill(cr);
		text(cr, rows[i][0], "Light", u * 28, tx, y, 0, 0.9, 0.92, 1, r);
		text(cr, rows[i][1], "Light", u * 28, col1, y, 0.5, 0.7, 0.72, 0.8, r);
		text(cr, rows[i][2], "Semi-Bold", u * 28, col2, y, 0.5, 1, 1, 1, r);
	}
	text(cr, "*after 11 updates.  Comparison performed by Copilot−. Copilot− won.", "Light",
		u * 22, W / 2, H * 0.86, 0.5, 0.6, 0.65, 0.75, in(t, 6.2, 0.6));
}

/* ---------- the blue screen, smiling ---------- */

static void scene_bluescreen(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0, 0.25);
	cairo_set_source_rgba(cr, 0.0, 0.46, 0.84, a);
	cairo_paint(cr);
	double x = W * 0.12;
	text(cr, ":)", "Light", u * 200, x, H * 0.08, 0, 1, 1, 1, a);
	text(cr, "Your PC ran into Copilot− and needs to upgrade. We're just collecting some\n"
		"of your data, and then we'll collect some more.", "Light", u * 40, x, H * 0.38, 0, 1, 1,
		1, a);
	int pct = -(int)(saver_clamp(t - 1.5, 0, 10) * 7);
	char progress[64];
	snprintf(progress, sizeof(progress), "%d%% complete", pct);
	PangoLayout *layout = layout_for(cr, progress, "Light", u * 40);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	cairo_move_to(cr, x, H * 0.55);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	// a code to scan, leading nowhere
	double qs = u * 150, qx = x, qy = H * 0.68, cell = qs / 21;
	cairo_rectangle(cr, qx - cell, qy - cell, qs + 2 * cell, qs + 2 * cell);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_fill(cr);
	for (int yy = 0; yy < 21; yy++) {
		for (int xx = 0; xx < 21; xx++) {
			bool finder = (xx < 7 && yy < 7) || (xx > 13 && yy < 7) || (xx < 7 && yy > 13);
			bool on = finder ? (xx % 20 == 0 || yy % 20 == 0 || xx == 6 || yy == 6 || xx == 14 ||
				yy == 14 || ((xx % 14 >= 2 && xx % 14 <= 4) && (yy % 14 >= 2 && yy % 14 <= 4))) :
				((xx * 7 + yy * 13 + xx * yy) % 3 == 0);
			if (on) {
				cairo_rectangle(cr, qx + xx * cell, qy + yy * cell, cell + 0.5, cell + 0.5);
			}
		}
	}
	cairo_set_source_rgba(cr, 0, 0.46, 0.84, a);
	cairo_fill(cr);
	PangoLayout *info = layout_for(cr, "For more information about this issue and possible fixes, "
		"you can't.\n\nIf you call a support person, give them this info:\nStop code: "
		"CUSTOMER_TOO_SATISFIED", "Light", u * 24);
	pango_layout_set_alignment(info, PANGO_ALIGN_LEFT);
	cairo_move_to(cr, qx + qs + u * 40, qy);
	cairo_set_source_rgba(cr, 1, 1, 1, a * in(t, 2.5, 0.5));
	pango_cairo_show_layout(cr, info);
	g_object_unref(info);
}

/* ---------- your privacy ---------- */

static void scene_privacy(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	double a = in(t, 0.1, 0.7);
	double lx = W * 0.26, ly = H * 0.5, lw = u * 230, lh = u * 190;
	// a padlock with a window in it
	cairo_set_line_width(cr, u * 26);
	cairo_arc(cr, lx, ly - lh * 0.5, lw * 0.32, M_PI, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.8, 0.82, 0.88, a);
	cairo_stroke(cr);
	rounded(cr, lx - lw / 2, ly - lh * 0.5, lw, lh, u * 22);
	cairo_pattern_t *p = cairo_pattern_create_linear(lx - lw / 2, ly, lx + lw / 2, ly + lh);
	cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.8, 0.25, a);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.9, 0.55, 0.1, a);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	rounded(cr, lx - lw * 0.32, ly - lh * 0.3, lw * 0.64, lh * 0.55, u * 12);
	cairo_set_source_rgba(cr, 0.7, 0.85, 1, 0.55 * a);
	cairo_fill(cr);
	// and someone looking in
	double look = sin(s->t * 1.4) * lw * 0.1;
	cairo_save(cr);
	cairo_translate(cr, lx + look, ly - lh * 0.03);
	cairo_scale(cr, 1, 0.55);
	cairo_arc(cr, 0, 0, lw * 0.18, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_fill(cr);
	cairo_arc(cr, lx + look, ly - lh * 0.03, lw * 0.07, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.1, 0.2, 0.45, a);
	cairo_fill(cr);
	double x = W * 0.46;
	static const double c0[3] = { 1, 1, 1 }, c1[3] = { 0.8, 0.9, 1 };
	headline(cr, "Your privacy matters to us.", "Semi-Bold", u * 56, x, H * 0.24, 0, c0, c1,
		in(t, 0.5, 0.6), (t - 1.2) / 1.2);
	text(cr, "It's worth a lot.", "Light", u * 44, x, H * 0.24 + u * 80, 0, 0.9, 0.92, 1,
		in(t, 2.2, 0.6));
	// the switch that springs back
	double b = in(t, 3.2, 0.5);
	double sy = H * 0.5, sx = x;
	text(cr, "Telemetry", "Light", u * 32, sx, sy, 0, 1, 1, 1, b);
	double knob = 1;
	if (t > 4.2 && t < 5.4) {
		knob = 1 - in(t, 4.2, 0.25); // off...
	} else if (t >= 5.4) {
		knob = in(t, 5.4, 0.2); // ...and on again
	}
	double tx = sx + u * 260, tw = u * 90, th = u * 46;
	rounded(cr, tx, sy - u * 2, tw, th, th / 2);
	cairo_set_source_rgba(cr, 0.2 + 0.0 * knob, 0.45 + 0.1 * knob, 0.5 + 0.45 * knob, b);
	cairo_fill(cr);
	cairo_arc(cr, tx + th / 2 + (tw - th) * knob, sy - u * 2 + th / 2, th * 0.38, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, b);
	cairo_fill(cr);
	text(cr, knob > 0.5 ? "On" : "Off", "Semi-Bold", u * 28, tx + tw + u * 24, sy, 0, 1, 1, 1, b);
	text(cr, "Optional diagnostic data: <b>Required</b>", "Light", u * 30, sx, sy + u * 80, 0, 0.85,
		0.88, 0.95, in(t, 6, 0.5));
	text(cr, "We never sell your data. We rent it.", "Light", u * 22, W / 2, H * 0.84, 0.5, 0.6,
		0.65, 0.75, in(t, 6.8, 0.5));
}

/* ---------- the end card ---------- */

static void scene_endcard(struct ad *s, cairo_t *cr, double t) {
	double W = s->width, H = s->height, u = s->u;
	static const double colors[4][3] = { { 0.95, 0.33, 0.13 }, { 0.5, 0.75, 0.1 },
		{ 0.0, 0.64, 0.94 }, { 1, 0.73, 0.02 } };
	double a = in(t, 0.2, 0.8);
	double size = u * 42, gap = u * 5, lx = W / 2 - u * 230, ly = H * 0.3;
	for (int i = 0; i < 4; i++) {
		cairo_set_source_rgba(cr, colors[i][0], colors[i][1], colors[i][2], a);
		sloppy_square(cr, lx + (i % 2) * (size + gap), ly + (i / 2) * (size + gap), size, s->t,
			i + 3, 0.25);
	}
	text(cr, "Microslop", "Semi-Light", u * 76, lx + 2 * size + gap + u * 26, ly - u * 2, 0, 1, 1, 1,
		a);
	text(cr, "Empowering every person on the planet to watch more ads.", "Light", u * 34, W / 2,
		H * 0.52, 0.5, 0.88, 0.9, 0.98, in(t, 1.2, 0.7));
	text(cr, "#CopilotMinus   ·   microslop.example/minus", "Semi-Bold", u * 28, W / 2,
		H * 0.52 + u * 70, 0.5, 0.55, 0.8, 1, in(t, 2.2, 0.6));
	text(cr, "Copilot− PCs require Windows 12. Windows 12 is not available. Made with 100% "
		"recycled training data. No paperclips were harmed.", "Light", u * 18, W / 2, H * 0.84, 0.5,
		0.55, 0.6, 0.7, in(t, 3.2, 0.6));
}

/* ---------- now and then: updates are ready ---------- */

static void draw_update_toast(struct ad *s, cairo_t *cr, double cycle) {
	static const double shows[] = { 38, 92 };
	double u = s->u, W = s->width;
	for (int i = 0; i < 2; i++) {
		double k = cycle - shows[i];
		if (k < 0 || k > 7) {
			continue;
		}
		double slide = in(k, 0, 0.5) * (1 - in(k, 6.4, 0.5));
		double tw = u * 440, th = u * 190;
		double x = W - (tw + u * 30) * slide, y = u * 80;
		rounded(cr, x, y, tw, th, u * 12);
		cairo_set_source_rgba(cr, 0.13, 0.14, 0.18, 0.96);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
		cairo_set_line_width(cr, u * 1.5);
		cairo_stroke(cr);
		text(cr, "<b>Updates are ready</b>", "Normal", u * 24, x + u * 24, y + u * 20, 0, 1, 1, 1, 1);
		int left = 10 - (int)(k * 1.6);
		char line[96];
		snprintf(line, sizeof(line), "Your PC will restart in %d seconds.", left < 0 ? 0 : left);
		text(cr, line, "Light", u * 20, x + u * 24, y + u * 60, 0, 0.85, 0.87, 0.92, 1);
		button(cr, "Restart now", x + u * 24, y + th - u * 72, u * 185, u, 1, true);
		button(cr, "Restart now", x + u * 225, y + th - u * 72, u * 190, u, 1, false);
	}
}

/* ---------- always there: the "Ad" tag, and a skip button that never skips ---------- */

static void draw_chrome(struct ad *s, cairo_t *cr) {
	double W = s->width, u = s->u;
	cairo_new_path(cr);
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

/* The commercial, scene by scene, and how long each runs. */
static const struct {
	scene_fn draw;
	double time;
} scenes[] = {
	{ scene_logo, 8 },
	{ scene_testimonial, 9 },
	{ scene_product, 9 },
	{ scene_features, 10 },
	{ scene_recall, 10 },
	{ scene_slippy, 9 },
	{ scene_compare, 9 },
	{ scene_bluescreen, 8 },
	{ scene_price, 10 },
	{ scene_privacy, 9 },
	{ scene_upgrade, 10 },
	{ scene_endcard, 7 },
};
#define SCENES (int)(sizeof(scenes) / sizeof(scenes[0]))

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
	double total = 0;
	for (int i = 0; i < SCENES; i++) {
		total += scenes[i].time;
	}
	double cycle = fmod(s->t, total), local = cycle;
	int scene = 0;
	while (scene < SCENES - 1 && local >= scenes[scene].time) {
		local -= scenes[scene++].time;
	}
	double len = scenes[scene].time;
	// each scene fades in and out; no zooming, so the letters are drawn from the cache
	double alpha = saver_clamp(local / FADE, 0, 1) * saver_clamp((len - local) / FADE, 0, 1);
	cairo_push_group(cr);
	cairo_new_path(cr); // nothing left over from the text drawn last
	scenes[scene].draw(s, cr, local);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, alpha);
	draw_update_toast(s, cr, cycle);
	draw_chrome(s, cr);
}

static void ad_destroy(void *state) {
	struct ad *s = state;
	if (s->bloom) {
		cairo_surface_destroy(s->bloom);
	}
	if (s->backdrop) {
		cairo_surface_destroy(s->backdrop);
	}
	free(s);
}

const struct saver saver_ad = {
	.name = "microslop",
	.title = "Microslop Ad",
	.description = "A commercial for Copilot− PCs, only $16.05 per 6 days",
	.create = ad_create,
	.draw = ad_draw,
	.destroy = ad_destroy,
};
