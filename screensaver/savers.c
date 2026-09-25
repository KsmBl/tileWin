#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "saver_util.h"
#include "stringop.h"
#include "tw_paths.h"
#include "twconf.h"

/*
 * The list of screen savers and what runs one: the Windows 7 ones first, then
 * three from older Windows, then three of tileWin's own.
 */

const struct saver *const savers[] = {
	&saver_blank,
	&saver_bubbles,
	&saver_mystify,
	&saver_ribbons,
	&saver_text3d,
	&saver_photos,
	&saver_starfield,
	&saver_pipes,
	&saver_flying,
	&saver_maze,
	&saver_aurora,
	&saver_wordclock,
	&saver_tiling,
	&saver_diggers,
	&saver_matrix,
};

const int saver_count = sizeof(savers) / sizeof(savers[0]);

static uint64_t random_state;

double saver_random(void) {
	if (!random_state) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		random_state = (uint64_t)ts.tv_nsec * 2654435761u + (uint64_t)ts.tv_sec + 1;
	}
	// xorshift64*
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;
	return ((random_state * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}

void saver_hsv(double h, double s, double v, double *r, double *g, double *b) {
	h = fmod(h, 1.0);
	if (h < 0) {
		h += 1;
	}
	double i = floor(h * 6), f = h * 6 - i;
	double p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
	switch ((int)i % 6) {
	case 0: *r = v; *g = t; *b = p; break;
	case 1: *r = q; *g = v; *b = p; break;
	case 2: *r = p; *g = v; *b = t; break;
	case 3: *r = p; *g = q; *b = v; break;
	case 4: *r = t; *g = p; *b = v; break;
	default: *r = v; *g = p; *b = q; break;
	}
}

void saver_set_hsva(cairo_t *cr, double h, double s, double v, double a) {
	double r, g, b;
	saver_hsv(h, s, v, &r, &g, &b);
	cairo_set_source_rgba(cr, r, g, b, a);
}

const struct saver *saver_find(const char *name) {
	if (!name || !*name || strcasecmp(name, "none") == 0) {
		return NULL;
	}
	if (strcasecmp(name, "random") == 0) {
		// Blank is no surprise worth waiting for
		return savers[1 + (int)(saver_random() * (saver_count - 1))];
	}
	for (int i = 0; i < saver_count; i++) {
		if (strcasecmp(savers[i]->name, name) == 0) {
			return savers[i];
		}
	}
	return NULL;
}

/* ---------- running one ---------- */

struct saver_run {
	const struct saver *saver;
	void *state;
	int width, height;         // of the area
	int inner_w, inner_h;      // what the saver draws on
	cairo_surface_t *inner;    // for a saver drawn smaller and scaled up
	double speed;
};

struct saver_run *saver_run_new(const struct saver *saver, int width, int height,
		const struct saver_options *options) {
	struct saver_run *run = calloc(1, sizeof(*run));
	run->saver = saver;
	run->width = width;
	run->height = height;
	run->speed = options && options->speed > 0 ? saver_clamp(options->speed, 0.25, 4) : 1;
	double res = saver->resolution > 0 && saver->resolution < 1 ? saver->resolution : 1;
	run->inner_w = (int)ceil(width * res);
	run->inner_h = (int)ceil(height * res);
	if (res < 1) {
		run->inner = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, run->inner_w, run->inner_h);
	}
	struct saver_options defaults = { .speed = 1, .photo_seconds = 8 };
	run->state = saver->create(run->inner_w, run->inner_h, options ? options : &defaults);
	return run;
}

void saver_run_draw(struct saver_run *run, cairo_t *cr, double seconds) {
	double dt = saver_clamp(seconds, 0, 0.25) * run->speed; // a stall is no leap
	cairo_t *target = cr;
	if (run->inner) {
		target = cairo_create(run->inner);
	}
	cairo_save(target);
	cairo_set_operator(target, CAIRO_OPERATOR_SOURCE);
	if (run->saver->transparent) {
		cairo_set_source_rgba(target, 0, 0, 0, 0);
	} else {
		cairo_set_source_rgb(target, 0, 0, 0);
	}
	cairo_paint(target);
	cairo_restore(target);
	cairo_save(target);
	run->saver->draw(run->state, target, run->inner_w, run->inner_h, dt);
	cairo_restore(target);
	if (run->inner) {
		cairo_destroy(target);
		cairo_surface_flush(run->inner);
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_scale(cr, (double)run->width / run->inner_w, (double)run->height / run->inner_h);
		cairo_set_source_surface(cr, run->inner, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
}

void saver_run_free(struct saver_run *run) {
	if (!run) {
		return;
	}
	run->saver->destroy(run->state);
	if (run->inner) {
		cairo_surface_destroy(run->inner);
	}
	free(run);
}

/* ---------- options ---------- */

char *saver_options_load(struct saver_options *options) {
	*options = (struct saver_options){ .speed = 1, .photo_seconds = 8 };
	char *dir = tw_config_dir();
	char *path = dir ? format_str("%s/taskbar.conf", dir) : NULL;
	free(dir);
	char *error = NULL;
	struct twconf_node *root = path ? twconf_parse_file(path, &error) : NULL;
	free(error);
	free(path);
	struct twconf_node *block = root ? twconf_child(root, "screensaver") : NULL;
	char *name = NULL;
	if (block) {
		const char *value = twconf_value(block, "name");
		name = value ? strdup(value) : NULL;
		value = twconf_value(block, "speed");
		if (value) {
			options->speed = saver_clamp(strtod(value, NULL), 0.25, 4);
		}
		value = twconf_value(block, "text");
		options->text = value ? strdup(value) : NULL;
		value = twconf_value(block, "photos");
		options->photos = value ? tw_expand_home(value) : NULL;
		value = twconf_value(block, "photo_seconds");
		if (value && atoi(value) >= 2) {
			options->photo_seconds = atoi(value);
		}
	}
	twconf_free(root);
	return name;
}

void saver_options_finish(struct saver_options *options) {
	free((char *)options->text);
	free((char *)options->photos);
	options->text = options->photos = NULL;
}
