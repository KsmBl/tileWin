#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_bubbles;

/* Bubbles. */

#define BUBBLES_MAX 32

struct bubble {
	double x, y, vx, vy, r, hue, spin;
};

static const char *const amount_values[] = { "normal", "few", "many", NULL };
static const char *const amount_labels[] = { "Normal", "Few", "Many", NULL };
static const char *const size_values[] = { "normal", "small", "large", NULL };
static const char *const size_labels[] = { "Normal", "Small", "Large", NULL };

struct bubbles {
	int count;
	struct bubble b[BUBBLES_MAX];
};

static void *bubbles_create(int width, int height, const struct saver_options *options) {
	struct bubbles *s = calloc(1, sizeof(*s));
	double u = saver_unit(width, height);
	static const double amounts[] = { 1, 0.5, 1.8 }, sizes[] = { 1, 0.65, 1.4 };
	double amount = amounts[saver_choice(options, &saver_bubbles, "count")];
	double size = sizes[saver_choice(options, &saver_bubbles, "size")];
	s->count = (int)saver_clamp(width * (double)height / (1920.0 * 1080.0) * 12 * amount, 3,
		BUBBLES_MAX);
	for (int i = 0; i < s->count; i++) {
		struct bubble *b = &s->b[i];
		b->r = u * saver_between(70, 150) * size;
		// a place of its own, so they do not start inside each other
		for (int tries = 0; tries < 50; tries++) {
			b->x = saver_between(b->r, width - b->r);
			b->y = saver_between(b->r, height - b->r);
			bool free_spot = true;
			for (int j = 0; j < i && free_spot; j++) {
				free_spot = hypot(b->x - s->b[j].x, b->y - s->b[j].y) > b->r + s->b[j].r;
			}
			if (free_spot) {
				break;
			}
		}
		double angle = saver_between(0, 2 * M_PI), speed = u * saver_between(60, 150);
		b->vx = cos(angle) * speed;
		b->vy = sin(angle) * speed;
		b->hue = saver_random();
		b->spin = saver_between(0, 2 * M_PI);
	}
	return s;
}

static void bubbles_move(struct bubbles *s, int width, int height, double dt) {
	for (int i = 0; i < s->count; i++) {
		struct bubble *b = &s->b[i];
		b->x += b->vx * dt;
		b->y += b->vy * dt;
		b->hue += dt * 0.03;
		b->spin += dt * 0.5;
		if ((b->x < b->r && b->vx < 0) || (b->x > width - b->r && b->vx > 0)) {
			b->vx = -b->vx;
		}
		if ((b->y < b->r && b->vy < 0) || (b->y > height - b->r && b->vy > 0)) {
			b->vy = -b->vy;
		}
	}
	// they bounce off each other like balls, the bigger ones heavier
	for (int i = 0; i < s->count; i++) {
		for (int j = i + 1; j < s->count; j++) {
			struct bubble *a = &s->b[i], *b = &s->b[j];
			double dx = b->x - a->x, dy = b->y - a->y, d = hypot(dx, dy);
			if (d <= 0 || d >= a->r + b->r) {
				continue;
			}
			double nx = dx / d, ny = dy / d;
			double approach = (a->vx - b->vx) * nx + (a->vy - b->vy) * ny;
			double ma = a->r * a->r, mb = b->r * b->r;
			if (approach > 0) {
				double impulse = 2 * approach / (ma + mb);
				a->vx -= impulse * mb * nx;
				a->vy -= impulse * mb * ny;
				b->vx += impulse * ma * nx;
				b->vy += impulse * ma * ny;
			}
			double overlap = (a->r + b->r - d) / 2;
			a->x -= nx * overlap;
			a->y -= ny * overlap;
			b->x += nx * overlap;
			b->y += ny * overlap;
		}
	}
}

static void bubble_draw(cairo_t *cr, const struct bubble *b) {
	double r = b->r, x = b->x, y = b->y;
	double tr, tg, tb;
	saver_hsv(b->hue, 0.45, 1, &tr, &tg, &tb);
	// the thin film: clear inside, tinted and bright towards the edge
	cairo_pattern_t *body = cairo_pattern_create_radial(x - r * 0.2, y - r * 0.25, 0, x, y, r);
	cairo_pattern_add_color_stop_rgba(body, 0, 1, 1, 1, 0.03);
	cairo_pattern_add_color_stop_rgba(body, 0.72, tr, tg, tb, 0.05);
	cairo_pattern_add_color_stop_rgba(body, 0.92, tr, tg, tb, 0.2);
	cairo_pattern_add_color_stop_rgba(body, 1, 1, 1, 1, 0.42);
	cairo_arc(cr, x, y, r, 0, 2 * M_PI);
	cairo_set_source(cr, body);
	cairo_fill(cr);
	cairo_pattern_destroy(body);
	// the rainbow sheen along the rim, turning slowly
	double cx = cos(b->spin) * r, cy = sin(b->spin) * r;
	cairo_pattern_t *sheen = cairo_pattern_create_linear(x - cx, y - cy, x + cx, y + cy);
	for (int i = 0; i <= 6; i++) {
		double sr, sg, sb;
		saver_hsv(b->hue + i / 6.0, 0.8, 1, &sr, &sg, &sb);
		cairo_pattern_add_color_stop_rgba(sheen, i / 6.0, sr, sg, sb, 0.34);
	}
	cairo_arc(cr, x, y, r * 0.955, 0, 2 * M_PI);
	cairo_set_line_width(cr, r * 0.07);
	cairo_set_source(cr, sheen);
	cairo_stroke(cr);
	cairo_pattern_destroy(sheen);
	// the window it reflects, up at the left, and a small glint below
	cairo_save(cr);
	cairo_translate(cr, x - r * 0.38, y - r * 0.42);
	cairo_rotate(cr, -0.6);
	cairo_scale(cr, 1, 0.55);
	cairo_pattern_t *glare = cairo_pattern_create_radial(0, 0, 0, 0, 0, r * 0.3);
	cairo_pattern_add_color_stop_rgba(glare, 0, 1, 1, 1, 0.85);
	cairo_pattern_add_color_stop_rgba(glare, 1, 1, 1, 1, 0);
	cairo_arc(cr, 0, 0, r * 0.3, 0, 2 * M_PI);
	cairo_set_source(cr, glare);
	cairo_fill(cr);
	cairo_pattern_destroy(glare);
	cairo_restore(cr);
	cairo_pattern_t *glint = cairo_pattern_create_radial(x + r * 0.45, y + r * 0.52, 0,
		x + r * 0.45, y + r * 0.52, r * 0.12);
	cairo_pattern_add_color_stop_rgba(glint, 0, 1, 1, 1, 0.4);
	cairo_pattern_add_color_stop_rgba(glint, 1, 1, 1, 1, 0);
	cairo_arc(cr, x + r * 0.45, y + r * 0.52, r * 0.12, 0, 2 * M_PI);
	cairo_set_source(cr, glint);
	cairo_fill(cr);
	cairo_pattern_destroy(glint);
}

static void bubbles_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct bubbles *s = state;
	bubbles_move(s, width, height, dt);
	for (int i = 0; i < s->count; i++) {
		bubble_draw(cr, &s->b[i]);
	}
}

static const struct saver_option bubbles_options[] = {
	{ "count", "Bubbles", NULL, SAVER_CHOICE, amount_values, amount_labels, false, NULL },
	{ "size", "Size", NULL, SAVER_CHOICE, size_values, size_labels, false, NULL },
	{ 0 },
};

const struct saver saver_bubbles = {
	.name = "bubbles",
	.title = "Bubbles",
	.description = "Soap bubbles drifting over the desktop and bouncing off each other",
	.transparent = true,
	.create = bubbles_create,
	.draw = bubbles_draw,
	.options = bubbles_options,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_bubbles, .order = 20, .shot_seconds = 5);
