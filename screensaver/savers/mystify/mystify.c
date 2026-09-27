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
extern const struct saver saver_mystify;

/* Mystify. */

#define MYSTIFY_CORNERS 4
#define MYSTIFY_ECHOES 28 // at most: the setting says how many

struct shape {
	double x[MYSTIFY_CORNERS], y[MYSTIFY_CORNERS], vx[MYSTIFY_CORNERS], vy[MYSTIFY_CORNERS];
	double ex[MYSTIFY_ECHOES][MYSTIFY_CORNERS], ey[MYSTIFY_ECHOES][MYSTIFY_CORNERS];
	int echoes, head;
	double hue;
};

struct mystify {
	struct shape shapes[2];
	int shape_count, trail;
	double since_echo;
	double u;
};

static void *mystify_create(int width, int height, const struct saver_options *options) {
	struct mystify *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	static const int trails[] = { 14, 5, 28 };
	s->shape_count = saver_choice(options, &saver_mystify, "shapes") == 1 ? 1 : 2;
	s->trail = trails[saver_choice(options, &saver_mystify, "trail")];
	for (int k = 0; k < s->shape_count; k++) {
		struct shape *p = &s->shapes[k];
		p->hue = k * 0.5 + saver_random() * 0.2;
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			p->x[i] = saver_between(0, width);
			p->y[i] = saver_between(0, height);
			double angle = saver_between(0, 2 * M_PI), speed = s->u * saver_between(160, 380);
			p->vx[i] = cos(angle) * speed;
			p->vy[i] = sin(angle) * speed;
		}
	}
	return s;
}

static void mystify_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct mystify *s = state;
	s->since_echo += dt;
	bool echo = s->since_echo >= 0.045;
	if (echo) {
		s->since_echo = 0;
	}
	for (int k = 0; k < s->shape_count; k++) {
		struct shape *p = &s->shapes[k];
		p->hue += dt * 0.04;
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			p->x[i] += p->vx[i] * dt;
			p->y[i] += p->vy[i] * dt;
			if ((p->x[i] < 0 && p->vx[i] < 0) || (p->x[i] > width && p->vx[i] > 0)) {
				p->vx[i] = -p->vx[i];
			}
			if ((p->y[i] < 0 && p->vy[i] < 0) || (p->y[i] > height && p->vy[i] > 0)) {
				p->vy[i] = -p->vy[i];
			}
		}
		if (echo) {
			memcpy(p->ex[p->head], p->x, sizeof(p->x));
			memcpy(p->ey[p->head], p->y, sizeof(p->y));
			p->head = (p->head + 1) % MYSTIFY_ECHOES;
			p->echoes = p->echoes < s->trail ? p->echoes + 1 : s->trail;
		}
		cairo_set_line_width(cr, fmax(1.2, 1.6 * s->u));
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		// the echoes fade behind the shape, oldest first
		for (int e = 0; e < p->echoes; e++) {
			int slot = (p->head - p->echoes + e + MYSTIFY_ECHOES) % MYSTIFY_ECHOES;
			for (int i = 0; i < MYSTIFY_CORNERS; i++) {
				if (i == 0) {
					cairo_move_to(cr, p->ex[slot][i], p->ey[slot][i]);
				} else {
					cairo_line_to(cr, p->ex[slot][i], p->ey[slot][i]);
				}
			}
			cairo_close_path(cr);
			saver_set_hsva(cr, p->hue, 1, 1, 0.15 + 0.6 * (e + 1) / s->trail);
			cairo_stroke(cr);
		}
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			if (i == 0) {
				cairo_move_to(cr, p->x[i], p->y[i]);
			} else {
				cairo_line_to(cr, p->x[i], p->y[i]);
			}
		}
		cairo_close_path(cr);
		saver_set_hsva(cr, p->hue, 0.9, 1, 1);
		cairo_stroke(cr);
	}
}

static const char *const shapes_values[] = { "two", "one", NULL };
static const char *const shapes_labels[] = { "Two", "One", NULL };
static const char *const trail_values[] = { "normal", "short", "long", NULL };
static const char *const trail_labels[] = { "Normal", "Short", "Long", NULL };
static const struct saver_option mystify_options[] = {
	{ "shapes", "Shapes", NULL, SAVER_CHOICE, shapes_values, shapes_labels, false, NULL },
	{ "trail", "Trail", "How many echoes follow each shape", SAVER_CHOICE, trail_values,
		trail_labels, false, NULL },
	{ 0 },
};

const struct saver saver_mystify = {
	.name = "mystify",
	.title = "Mystify",
	.description = "Two shapes of lines that bounce around and leave echoes of color",
	.create = mystify_create,
	.draw = mystify_draw,
	.options = mystify_options,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_mystify, .order = 30, .shot_seconds = 5);
