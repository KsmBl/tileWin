#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_aurora;

/* Aurora. */

#define AURORA_STARS 260
#define AURORA_LEVELS 8
#define AURORA_MOODS 3
#define AURORA_CURTAINS 3
#define AURORA_HILLS 64

struct aurora {
	double sx[AURORA_STARS], sy[AURORA_STARS], sb[AURORA_STARS], sp[AURORA_STARS];
	double hills[AURORA_HILLS + 1];
	// a curtain in AURORA_LEVELS strengths, calm, busy and very busy
	cairo_pattern_t *curtain[AURORA_MOODS][AURORA_LEVELS];
	double t, load, target, since_cpu;
	unsigned long long cpu_total, cpu_idle;
	int mood;         // 0 follows the processor, 1 always calm, 2 always stormy
	bool stars;
};

/* How busy the processor was since the last look, 0 to 1. */
static void aurora_sample_cpu(struct aurora *a) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return;
	}
	unsigned long long v[8] = { 0 };
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
		&v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 5) {
		return;
	}
	unsigned long long idle = v[3] + v[4], total = 0;
	for (int i = 0; i < 8; i++) {
		total += v[i];
	}
	if (a->cpu_total && total > a->cpu_total) {
		a->target = 1 - (double)(idle - a->cpu_idle) / (total - a->cpu_total);
	}
	a->cpu_total = total;
	a->cpu_idle = idle;
}

static void *aurora_create(int width, int height, const struct saver_options *options) {
	struct aurora *a = calloc(1, sizeof(*a));
	for (int i = 0; i < AURORA_STARS; i++) {
		a->sx[i] = saver_random();
		a->sy[i] = saver_random() * saver_random() * 0.75; // more of them up high
		a->sb[i] = saver_between(0.2, 1);
		a->sp[i] = saver_between(0, 2 * M_PI);
	}
	double p1 = saver_between(0, 6), p2 = saver_between(0, 6), p3 = saver_between(0, 6);
	for (int i = 0; i <= AURORA_HILLS; i++) {
		double x = i / (double)AURORA_HILLS;
		a->hills[i] = 0.84 - 0.07 * sin(x * 5 + p1) - 0.04 * sin(x * 13 + p2) -
			0.015 * sin(x * 37 + p3);
	}
	static const double tops[AURORA_MOODS][3] = {
		{ 0.35, 0.3, 0.95 }, { 0.75, 0.25, 0.85 }, { 0.95, 0.25, 0.4 },
	};
	for (int m = 0; m < AURORA_MOODS; m++) {
		for (int l = 0; l < AURORA_LEVELS; l++) {
			double s = (l + 1) / (double)AURORA_LEVELS;
			cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, 1);
			cairo_pattern_add_color_stop_rgba(p, 0, tops[m][0], tops[m][1], tops[m][2], 0);
			cairo_pattern_add_color_stop_rgba(p, 0.45, tops[m][0] * 0.6, 0.55, tops[m][2] * 0.7,
				0.22 * s);
			cairo_pattern_add_color_stop_rgba(p, 0.86, 0.25, 1, 0.55, 0.8 * s);
			cairo_pattern_add_color_stop_rgba(p, 0.95, 0.65, 1, 0.8, 0.9 * s);
			cairo_pattern_add_color_stop_rgba(p, 1, 0.3, 1, 0.6, 0);
			a->curtain[m][l] = p;
		}
	}
	aurora_sample_cpu(a);
	a->load = a->target = 0.2;
	a->mood = saver_choice(options, &saver_aurora, "mood");
	a->stars = saver_toggle(options, &saver_aurora, "stars");
	return a;
}

static void aurora_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct aurora *a = state;
	a->t += dt;
	a->since_cpu += dt;
	if (a->mood == 1 || a->mood == 2) {
		a->target = a->mood == 1 ? 0.15 : 0.9;
	} else if (a->since_cpu >= 1) {
		a->since_cpu = 0;
		aurora_sample_cpu(a);
	}
	a->load += (a->target - a->load) * saver_clamp(dt * 1.2, 0, 1);
	double load = saver_clamp(a->load, 0, 1);

	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgb(sky, 0, 0.01, 0.015, 0.05);
	cairo_pattern_add_color_stop_rgb(sky, 0.8, 0.02, 0.07, 0.1);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);
	for (int i = 0; a->stars && i < AURORA_STARS; i++) {
		double twinkle = 0.6 + 0.4 * sin(a->t * 2 + a->sp[i]);
		cairo_rectangle(cr, a->sx[i] * width, a->sy[i] * height, 1.2, 1.2);
		cairo_set_source_rgba(cr, 1, 1, 1, a->sb[i] * twinkle * 0.8);
		cairo_fill(cr);
	}

	// the curtains: they fold and sway, faster and brighter when the computer works
	int mood = load < 0.4 ? 0 : load < 0.75 ? 1 : 2;
	double step = fmax(1.5, width / 360.0), tau = 2 * M_PI;
	double sway = a->t * (0.6 + 1.4 * load);
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int c = 0; c < AURORA_CURTAINS; c++) {
		for (double x = 0; x < width; x += step) {
			double u = x / width;
			double base = height * (0.42 + 0.07 * c) +
				height * 0.08 * sin(u * tau * 1.1 + a->t * 0.15 + c * 2) +
				height * 0.04 * sin(u * tau * 3.7 - a->t * 0.33 + c);
			double tall = height * (0.2 + 0.1 * sin(u * tau * 0.8 + a->t * 0.2 + c * 1.3) +
				0.05 * sin(u * tau * 5 + sway * 0.6));
			double bright = (0.6 + 0.4 * sin(u * tau * 7 + sway * 0.9 + c * 3)) *
				(0.5 + 0.5 * sin(u * tau * 2.3 - a->t * 0.4 + c));
			bright *= 0.45 + 0.8 * load;
			int level = (int)(saver_clamp(bright, 0, 0.999) * AURORA_LEVELS);
			if (level <= 0 || tall < 1) {
				continue;
			}
			double top = base - tall;
			cairo_matrix_t m;
			cairo_matrix_init(&m, 1, 0, 0, 1 / tall, 0, -top / tall);
			cairo_pattern_t *p = a->curtain[mood][level - 1];
			cairo_pattern_set_matrix(p, &m);
			cairo_rectangle(cr, x, top, step + 0.6, tall);
			cairo_set_source(cr, p);
			cairo_fill(cr);
		}
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	// the hills in front
	cairo_move_to(cr, 0, height);
	for (int i = 0; i <= AURORA_HILLS; i++) {
		cairo_line_to(cr, i * width / (double)AURORA_HILLS, a->hills[i] * height);
	}
	cairo_line_to(cr, width, height);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.006, 0.012, 0.02);
	cairo_fill(cr);
}

static void aurora_destroy(void *state) {
	struct aurora *a = state;
	for (int m = 0; m < AURORA_MOODS; m++) {
		for (int l = 0; l < AURORA_LEVELS; l++) {
			cairo_pattern_destroy(a->curtain[m][l]);
		}
	}
	free(a);
}

static const char *const mood_values[] = { "cpu", "calm", "stormy", NULL };
static const char *const mood_labels[] = { "Follows the processor", "Always calm", "Always stormy",
	NULL };
static const struct saver_option aurora_options[] = {
	{ "mood", "Northern lights", "Following the processor, they grow wilder the harder it works",
		SAVER_CHOICE, mood_values, mood_labels, false, NULL },
	{ "stars", "Stars", NULL, SAVER_TOGGLE, NULL, NULL, true, NULL },
	{ 0 },
};

const struct saver saver_aurora = {
	.name = "aurora",
	.title = "Aurora",
	.description = "Northern lights over the hills that glow brighter and turn red the "
		"harder the computer works",
	.resolution = 0.5,
	.create = aurora_create,
	.draw = aurora_draw,
	.options = aurora_options,
	.destroy = aurora_destroy,
};

TILEWIN_SAVER(.saver = &saver_aurora, .order = 110, .shot_seconds = 6);
