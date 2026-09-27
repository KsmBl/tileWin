#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_flying;

/* Flying Windows. */

#define FLAGS 96 // at most: the setting says how many

struct flag {
	double x, y, z, phase;
};

struct flying {
	int count;
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
	static const int counts[] = { 48, 16, 96 };
	s->count = counts[saver_choice(options, &saver_flying, "windows")];
	for (int i = 0; i < s->count; i++) {
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
	for (int i = 0; i < s->count; i++) {
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
	qsort(s->order, s->count, sizeof(int), flag_cmp);
	for (int k = 0; k < s->count; k++) {
		struct flag *f = &s->f[s->order[k]];
		double sx = cx + f->x / f->z * spread, sy = cy + f->y / f->z * spread;
		double alpha = saver_clamp((1 - f->z) * 4, 0, 1); // out of the dark
		flag_draw(cr, sx, sy, s->u * 34 / f->z, f->phase, alpha);
	}
}

static const char *const windows_values[] = { "normal", "few", "many", NULL };
static const char *const windows_labels[] = { "Normal", "Few", "Many", NULL };
static const struct saver_option flying_options[] = {
	{ "windows", "Windows", NULL, SAVER_CHOICE, windows_values, windows_labels, false, NULL },
	{ 0 },
};

const struct saver saver_flying = {
	.name = "flyingwindows",
	.title = "Flying Windows",
	.description = "Waving Windows flags flying towards you, from Windows 3.1",
	.create = flying_create,
	.draw = flying_draw,
	.options = flying_options,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_flying, .order = 90, .shot_seconds = 4);
