/*
 * Runs every screen saver for a few seconds on a picture in memory, the way
 * the preview of the settings does, and looks at what it drew: something has
 * to be there (except for Blank), it has to change over time (except for
 * Blank and the Word Clock between two minutes), and a saver drawn over the
 * desktop has to leave most of it showing. Run under a sanitizer, this also
 * catches a saver that writes where it should not.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savers.h"

static int failures;

static void fail(const char *saver, const char *what) {
	fprintf(stderr, "FAIL: %s: %s\n", saver, what);
	failures++;
}

/* How many pixels are not black, and how many are not see-through. */
static void measure(cairo_surface_t *surface, long *lit, long *opaque) {
	cairo_surface_flush(surface);
	int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
	int stride = cairo_image_surface_get_stride(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	*lit = *opaque = 0;
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(data + y * stride);
		for (int x = 0; x < w; x++) {
			*lit += (row[x] & 0x00ffffff) != 0;
			*opaque += (row[x] >> 24) == 0xff;
		}
	}
}

int main(void) {
	const int w = 480, h = 300;
	struct saver_options options = { .speed = 1, .text = "Hello", .photo_seconds = 3 };
	for (int i = 0; i < saver_count; i++) {
		const struct saver *saver = savers[i];
		if (saver_find(saver->name) != saver) {
			fail(saver->name, "is not found by its name");
		}
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		cairo_t *cr = cairo_create(surface);
		struct saver_run *run = saver_run_new(saver, w, h, &options);
		long lit_before = 0, opaque = 0, lit_after = 0;
		unsigned char *first = malloc((size_t)cairo_image_surface_get_stride(surface) * h);
		for (int frame = 0; frame < 150; frame++) {
			saver_run_draw(run, cr, 1 / 30.0);
			if (frame == 30) {
				measure(surface, &lit_before, &opaque);
				cairo_surface_flush(surface);
				memcpy(first, cairo_image_surface_get_data(surface),
					(size_t)cairo_image_surface_get_stride(surface) * h);
			}
		}
		measure(surface, &lit_after, &opaque);
		// SAVER_DUMP=<folder> keeps the last picture of each, to look at
		if (getenv("SAVER_DUMP")) {
			char path[512];
			snprintf(path, sizeof(path), "%s/%s.png", getenv("SAVER_DUMP"), saver->name);
			cairo_surface_write_to_png(surface, path);
		}
		bool changed = memcmp(first, cairo_image_surface_get_data(surface),
			(size_t)cairo_image_surface_get_stride(surface) * h) != 0;
		printf("%-14s %6ld pixels lit, %6ld opaque, %s\n", saver->name, lit_after, opaque,
			changed ? "moving" : "still");
		bool blank = strcmp(saver->name, "blank") == 0;
		if (!blank && lit_after < 200) {
			fail(saver->name, "draws next to nothing");
		}
		if (blank && lit_after != 0) {
			fail(saver->name, "is not black");
		}
		if (!blank && strcmp(saver->name, "wordclock") != 0 && !changed) {
			fail(saver->name, "does not move");
		}
		if (saver->transparent && opaque > (long)w * h / 2) {
			fail(saver->name, "hides the desktop it is drawn over");
		}
		if (!saver->transparent && opaque != (long)w * h) {
			fail(saver->name, "lets the desktop show through");
		}
		free(first);
		saver_run_free(run);
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
	}
	if (saver_find("random") == NULL || saver_find("none") != NULL ||
			saver_find("no-such-saver") != NULL) {
		fail("saver_find", "random, none or an unknown name come out wrong");
	}
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
