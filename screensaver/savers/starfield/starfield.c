#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_starfield;

/* Starfield. */

#define STARS_MAX 1800

struct star {
	double x, y, z, px, py; // px, py: where it was drawn last, for the streak
	bool drawn;
};

struct starfield {
	int count;
	struct star s[STARS_MAX];
	double u;
};

static void star_reset(struct star *s, bool anywhere) {
	s->x = saver_between(-1, 1);
	s->y = saver_between(-1, 1);
	s->z = anywhere ? saver_between(0.05, 1) : 1;
	s->drawn = false;
}

static void *starfield_create(int width, int height, const struct saver_options *options) {
	struct starfield *f = calloc(1, sizeof(*f));
	f->u = saver_unit(width, height);
	static const double densities[] = { 1, 0.45, 2 };
	double density = densities[saver_choice(options, &saver_starfield, "stars")];
	f->count = (int)saver_clamp(width * (double)height / 3000 * density, 60, STARS_MAX);
	for (int i = 0; i < f->count; i++) {
		star_reset(&f->s[i], true);
	}
	return f;
}

static void starfield_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct starfield *f = state;
	double cx = width / 2.0, cy = height / 2.0, spread = fmax(width, height) * 0.5;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int i = 0; i < f->count; i++) {
		struct star *s = &f->s[i];
		s->z -= dt * 0.32;
		double sx = cx + s->x / s->z * spread, sy = cy + s->y / s->z * spread;
		if (s->z <= 0.02 || sx < -20 || sx > width + 20 || sy < -20 || sy > height + 20) {
			star_reset(s, false);
			continue;
		}
		double near = 1 - s->z;
		cairo_set_line_width(cr, fmax(0.8, f->u * 3.2 * near));
		cairo_move_to(cr, s->drawn ? s->px : sx, s->drawn ? s->py : sy);
		cairo_line_to(cr, sx, sy);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.25 + 0.75 * near);
		cairo_stroke(cr);
		s->px = sx;
		s->py = sy;
		s->drawn = true;
	}
}

static const char *const stars_values[] = { "normal", "sparse", "dense", NULL };
static const char *const stars_labels[] = { "Normal", "Sparse", "Dense", NULL };
static const struct saver_option starfield_options[] = {
	{ "stars", "Stars", NULL, SAVER_CHOICE, stars_values, stars_labels, false, NULL },
	{ 0 },
};

const struct saver saver_starfield = {
	.name = "starfield",
	.title = "Starfield",
	.description = "Flying through the stars, from Windows 3.1 and 95",
	.create = starfield_create,
	.draw = starfield_draw,
	.options = starfield_options,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_starfield, .order = 70, .shot_seconds = 4);
