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
extern const struct saver saver_blank;

/* Blank. */

static void *blank_create(int width, int height, const struct saver_options *options) {
	return calloc(1, 1);
}

static void blank_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	// black, which saver_run_draw has painted already
}

const struct saver saver_blank = {
	.name = "blank",
	.title = "Blank",
	.description = "A black screen",
	.create = blank_create,
	.draw = blank_draw,
	.destroy = free,
};

TILEWIN_SAVER(.saver = &saver_blank, .order = 10, .shot_seconds = 1);
