#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Screen savers of older Windows: the Starfield Simulation of Windows 3.1 and
 * 95 and the Flying Windows of Windows 3.1. 3D Pipes and 3D Maze have files
 * of their own.
 */

/* ================= Starfield ================= */

#define STARS_MAX 900

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
	f->count = (int)saver_clamp(width * (double)height / 3000, 150, STARS_MAX);
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

const struct saver saver_starfield = {
	.name = "starfield",
	.title = "Starfield",
	.description = "Flying through the stars, from Windows 3.1 and 95",
	.create = starfield_create,
	.draw = starfield_draw,
	.destroy = free,
};

/* ================= Flying Windows ================= */

#define FLAGS 48

struct flag {
	double x, y, z, phase;
};

struct flying {
	struct flag f[FLAGS];
	int order[FLAGS];
	double u;
};

static void flag_reset(struct flag *f, bool anywhere) {
	f->x = saver_between(-1, 1);
	f->y = saver_between(-1, 1);
	f->z = anywhere ? saver_between(0.1, 1) : 1;
	f->phase = saver_between(0, 2 * M_PI);
}

static void *flying_create(int width, int height, const struct saver_options *options) {
	struct flying *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	for (int i = 0; i < FLAGS; i++) {
		flag_reset(&s->f[i], true);
		s->order[i] = i;
	}
	return s;
}

static struct flying *sorting;

static int flag_cmp(const void *a, const void *b) {
	double za = sorting->f[*(const int *)a].z, zb = sorting->f[*(const int *)b].z;
	return za < zb ? 1 : za > zb ? -1 : 0;
}

/* The waving four-color flag of Windows 3.1, size across, centered at x, y. */
static void flag_draw(cairo_t *cr, double x, double y, double size, double phase,
		double alpha) {
	static const double colors[4][3] = {
		{ 0.93, 0.26, 0.16 }, { 0.3, 0.69, 0.31 }, { 0.13, 0.49, 0.9 }, { 0.98, 0.75, 0.1 },
	};
	double half = size / 2, gap = size * 0.05;
	for (int pane = 0; pane < 4; pane++) {
		double x0 = pane % 2 ? gap / 2 : -half, x1 = pane % 2 ? half : -gap / 2;
		double y0 = pane < 2 ? -half : gap / 2, y1 = pane < 2 ? -gap / 2 : half;
		double w0 = sin(phase + x0 / size * 4) * size * 0.08;
		double w1 = sin(phase + x1 / size * 4) * size * 0.08;
		// the flag leans back a little as it waves, as the original did
		double lean = size * 0.12;
		cairo_move_to(cr, x + x0 + lean * (y0 / size), y + y0 + w0);
		cairo_line_to(cr, x + x1 + lean * (y0 / size), y + y0 + w1);
		cairo_line_to(cr, x + x1 + lean * (y1 / size), y + y1 + w1);
		cairo_line_to(cr, x + x0 + lean * (y1 / size), y + y1 + w0);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, colors[pane][0], colors[pane][1], colors[pane][2], alpha);
		cairo_fill(cr);
	}
}

static void flying_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct flying *s = state;
	double cx = width / 2.0, cy = height / 2.0, spread = fmax(width, height) * 0.55;
	for (int i = 0; i < FLAGS; i++) {
		struct flag *f = &s->f[i];
		f->z -= dt * 0.22;
		f->phase += dt * 5;
		double sx = cx + f->x / f->z * spread, sy = cy + f->y / f->z * spread;
		double size = s->u * 34 / f->z;
		if (f->z <= 0.04 || sx < -size || sx > width + size || sy < -size || sy > height + size) {
			flag_reset(f, false);
		}
	}
	sorting = s;
	qsort(s->order, FLAGS, sizeof(int), flag_cmp);
	for (int k = 0; k < FLAGS; k++) {
		struct flag *f = &s->f[s->order[k]];
		double sx = cx + f->x / f->z * spread, sy = cy + f->y / f->z * spread;
		double alpha = saver_clamp((1 - f->z) * 4, 0, 1); // out of the dark
		flag_draw(cr, sx, sy, s->u * 34 / f->z, f->phase, alpha);
	}
}

const struct saver saver_flying = {
	.name = "flyingwindows",
	.title = "Flying Windows",
	.description = "Waving Windows flags flying towards you, from Windows 3.1",
	.create = flying_create,
	.draw = flying_draw,
	.destroy = free,
};
