/*
 * Lava Lamp: an example of a screen saver of your own. Soft blobs of warm wax
 * rise in a dark lamp, cool at the top and sink again, and melt into each
 * other where they meet.
 *
 * Build it (see the Makefile beside it) and put this directory, with the
 * saver.so it builds, into ~/.local/share/tileWin/screensavers; it is then on
 * the Screen saver page of the settings.
 */
#include <stdlib.h>
#include "tilewin-saver.h"

#define BLOBS 9

struct blob {
	double x, y, r, vy, heat;
};

struct lamp {
	struct blob blobs[BLOBS];
	double t, hue;
};

/* The saver itself, declared first: its settings are read with it. */
extern const struct saver saver_lavalamp;

static void *lamp_create(int width, int height, const struct saver_options *options) {
	struct lamp *s = calloc(1, sizeof(*s));
	double u = saver_unit(width, height);
	for (int i = 0; i < BLOBS; i++) {
		s->blobs[i] = (struct blob){ saver_between(0.3, 0.7) * width, saver_between(0, 1) * height,
			u * saver_between(40, 110), 0, saver_random() };
	}
	// a setting of its own: "lavalamp_colour" in the screensaver block of taskbar.conf
	static const double hues[] = { 0.02, 0.8, 0.45 };
	s->hue = hues[saver_choice(options, &saver_lavalamp, "colour")];
	return s;
}

static void lamp_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct lamp *s = state;
	s->t += dt;
	double u = saver_unit(width, height);
	// the dark glass, lit from below
	cairo_pattern_t *glass = cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgb(glass, 0, 0.03, 0.02, 0.05);
	cairo_pattern_add_color_stop_rgb(glass, 1, 0.22, 0.08, 0.06);
	cairo_set_source(cr, glass);
	cairo_paint(cr);
	cairo_pattern_destroy(glass);
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int i = 0; i < BLOBS; i++) {
		struct blob *b = &s->blobs[i];
		// warmed at the bottom it rises, cooled at the top it sinks
		b->heat += dt * (b->y > height * 0.8 ? 0.25 : b->y < height * 0.2 ? -0.25 : 0);
		b->heat = saver_clamp(b->heat, 0, 1);
		b->vy += (0.5 - b->heat) * u * 60 * dt;
		b->vy *= 1 - 0.6 * dt;
		b->y = saver_clamp(b->y + b->vy * dt, -b->r, height + b->r);
		double x = b->x + sin(s->t * 0.3 + i) * u * 30;
		cairo_pattern_t *p = cairo_pattern_create_radial(x, b->y, 0, x, b->y, b->r);
		double r, g, bl;
		saver_hsv(s->hue + b->heat * 0.06, 0.9, 0.5 + 0.5 * b->heat, &r, &g, &bl);
		cairo_pattern_add_color_stop_rgba(p, 0, r, g, bl, 0.9);
		cairo_pattern_add_color_stop_rgba(p, 0.6, r, g, bl, 0.45);
		cairo_pattern_add_color_stop_rgba(p, 1, r, g, bl, 0);
		cairo_set_source(cr, p);
		cairo_arc(cr, x, b->y, b->r, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

static void lamp_destroy(void *state) {
	free(state);
}

static const char *const colour_values[] = { "red", "purple", "green", NULL };
static const char *const colour_labels[] = { "Red", "Purple", "Green", NULL };
static const struct saver_option lamp_options[] = {
	{ "colour", "Wax", "The colour of the wax", SAVER_CHOICE, colour_values, colour_labels, false,
		NULL },
	{ 0 },
};

const struct saver saver_lavalamp = {
	.name = "lavalamp",
	.title = "Lava Lamp",
	.description = "Warm wax rising and sinking in a dark lamp",
	.options = lamp_options,
	.create = lamp_create,
	.draw = lamp_draw,
	.destroy = lamp_destroy,
};

/* What tileWin looks for in saver.so: this saver, placed after those of tileWin. */
TILEWIN_SAVER(.saver = &saver_lavalamp);
