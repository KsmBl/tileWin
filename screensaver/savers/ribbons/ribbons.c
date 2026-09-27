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
extern const struct saver saver_ribbons;

/* Ribbons. */

#define RIBBONS 5
#define RIBBON_POINTS 150

struct ribbon {
	double x[RIBBON_POINTS], y[RIBBON_POINTS], z[RIBBON_POINTS], twist[RIBBON_POINTS];
	int head, count;
	double f[6], p[6]; // the frequencies and phases of its path
	double hue;
};

struct ribbons {
	struct ribbon r[RIBBONS];
	int count;
	double t, since_point, u;
};

static void *ribbons_create(int width, int height, const struct saver_options *options) {
	struct ribbons *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	static const int counts[] = { RIBBONS, 3, 1 };
	s->count = counts[saver_choice(options, &saver_ribbons, "ribbons")];
	for (int i = 0; i < s->count; i++) {
		struct ribbon *r = &s->r[i];
		for (int k = 0; k < 6; k++) {
			r->f[k] = saver_between(0.35, 0.9) * (k % 2 ? 1 : -1);
			r->p[k] = saver_between(0, 2 * M_PI);
		}
		r->hue = i / (double)RIBBONS + saver_random() * 0.1;
	}
	return s;
}

static void ribbon_head(struct ribbon *r, double t, double *x, double *y, double *z,
		double *twist) {
	*x = sin(t * r->f[0] + r->p[0]) * 0.75 + sin(t * r->f[1] * 2.3 + r->p[1]) * 0.25;
	*y = sin(t * r->f[2] + r->p[2]) * 0.7 + sin(t * r->f[3] * 1.9 + r->p[3]) * 0.3;
	*z = sin(t * r->f[4] + r->p[4]);
	*twist = t * (1 + r->f[5]) + r->p[5];
}

static void ribbons_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct ribbons *s = state;
	s->t += dt;
	s->since_point += dt;
	bool add = s->since_point >= 1 / 30.0;
	if (add) {
		s->since_point = 0;
	}
	for (int i = 0; i < s->count; i++) {
		struct ribbon *r = &s->r[i];
		r->hue += dt * 0.02;
		if (add || r->count == 0) {
			ribbon_head(r, s->t, &r->x[r->head], &r->y[r->head], &r->z[r->head],
				&r->twist[r->head]);
			r->head = (r->head + 1) % RIBBON_POINTS;
			r->count = r->count < RIBBON_POINTS ? r->count + 1 : RIBBON_POINTS;
		}
		double px[RIBBON_POINTS], py[RIBBON_POINTS], half[RIBBON_POINTS];
		for (int k = 0; k < r->count; k++) {
			int slot = (r->head - r->count + k + RIBBON_POINTS) % RIBBON_POINTS;
			double persp = 3 / (3 + r->z[slot] + 1);
			px[k] = width / 2.0 + r->x[slot] * width * 0.48 * persp;
			py[k] = height / 2.0 + r->y[slot] * height * 0.48 * persp;
			half[k] = s->u * 46 * persp * cos(r->twist[slot]);
		}
		for (int k = 0; k + 1 < r->count; k++) {
			double dx = px[k + 1] - px[k], dy = py[k + 1] - py[k];
			double len = hypot(dx, dy);
			if (len < 0.01) {
				continue;
			}
			// across the path; how wide it looks is how far it is turned towards us
			double nx = -dy / len, ny = dx / len;
			cairo_move_to(cr, px[k] + nx * half[k], py[k] + ny * half[k]);
			cairo_line_to(cr, px[k + 1] + nx * half[k + 1], py[k + 1] + ny * half[k + 1]);
			cairo_line_to(cr, px[k + 1] - nx * half[k + 1], py[k + 1] - ny * half[k + 1]);
			cairo_line_to(cr, px[k] - nx * half[k], py[k] - ny * half[k]);
			cairo_close_path(cr);
			double age = (k + 1) / (double)r->count; // 1 at the head
			double facing = fabs(half[k]) / (s->u * 46 + 0.001);
			bool back = half[k] < 0;
			// lit where it faces us, its back a deeper color; the tail fades out
			saver_set_hsva(cr, r->hue + age * 0.08, back ? 0.95 : 0.6,
				0.3 + 0.7 * facing, 0.15 + 0.85 * age);
			cairo_fill_preserve(cr);
			cairo_set_line_width(cr, 0.8); // closes the hairline between the pieces
			cairo_stroke(cr);
		}
	}
}

static const char *const ribbons_values[] = { "five", "three", "one", NULL };
static const char *const ribbons_labels[] = { "Five", "Three", "One", NULL };
static const struct saver_option ribbons_options[] = {
	{ "ribbons", "Ribbons", NULL, SAVER_CHOICE, ribbons_values, ribbons_labels, false, NULL },
	{ 0 },
};

const struct saver saver_ribbons = {
	.name = "ribbons",
	.title = "Ribbons",
	.description = "Glowing ribbons that twist and weave through the dark",
	.create = ribbons_create,
	.draw = ribbons_draw,
	.options = ribbons_options,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_ribbons, .order = 40, .shot_seconds = 6);
