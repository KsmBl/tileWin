/*
 * Runs every screen saver for a few seconds on a picture in memory, the way
 * the preview of the settings does, and looks at what it drew: something has
 * to be there (except for Blank), it has to change over time (except for
 * Blank and the Word Clock between two minutes), and a saver drawn over the
 * desktop has to leave most of it showing. Run under a sanitizer, this also
 * catches a saver that writes where it should not. Each kind of Doomsday is run
 * the same way. Then the settings of the savers: that their tables make sense,
 * that they are read as written (and by the names they had before), and that
 * the kinds of Doomsday are found by their old names but not listed.
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

/* Runs a saver for a while and looks at what it drew; name is what it is called here. */
static void try_saver(const struct saver *saver, const char *name, struct saver_options options) {
	const int w = 480, h = 300;
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
		snprintf(path, sizeof(path), "%s/%s.png", getenv("SAVER_DUMP"), name);
		cairo_surface_write_to_png(surface, path);
	}
	bool changed = memcmp(first, cairo_image_surface_get_data(surface),
		(size_t)cairo_image_surface_get_stride(surface) * h) != 0;
	printf("%-22s %6ld pixels lit, %6ld opaque, %s\n", name, lit_after, opaque,
		changed ? "moving" : "still");
	bool blank = strcmp(saver->name, "blank") == 0;
	if (!blank && lit_after < 200) {
		fail(name, "draws next to nothing");
	}
	if (blank && lit_after != 0) {
		fail(name, "is not black");
	}
	if (!blank && strcmp(saver->name, "wordclock") != 0 && !changed) {
		fail(name, "does not move");
	}
	if (saver->transparent && opaque > (long)w * h / 2) {
		fail(name, "hides the desktop it is drawn over");
	}
	if (!saver->transparent && opaque != (long)w * h) {
		fail(name, "lets the desktop show through");
	}
	free(first);
	saver_run_free(run);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

/* That the settings of a saver make sense: known keys, labels for each value, "when" valid. */
static void check_options(const struct saver *saver) {
	for (const struct saver_option *o = saver->options; o && o->key; o++) {
		for (const struct saver_option *other = saver->options; other != o; other++) {
			if (strcmp(other->key, o->key) == 0) {
				fail(saver->name, "has a setting twice");
			}
		}
		if (!o->label || !*o->label) {
			fail(saver->name, "has a setting without a label");
		}
		if (o->type == SAVER_CHOICE) {
			int values = 0, labels = 0;
			while (o->values && o->values[values]) {
				values++;
			}
			while (o->labels && o->labels[labels]) {
				labels++;
			}
			if (values < 2 || values != labels) {
				fail(saver->name, "has a choice without values, or not a label for each");
			}
		} else if (o->values || o->labels) {
			fail(saver->name, "has a toggle with values");
		}
		if (o->when) {
			const char *eq = strchr(o->when, '=');
			bool found = false;
			for (const struct saver_option *c = saver->options; eq && c->key; c++) {
				if (c->type == SAVER_CHOICE && strlen(c->key) == (size_t)(eq - o->when) &&
						strncmp(c->key, o->when, eq - o->when) == 0) {
					for (int v = 0; c->values[v]; v++) {
						found = found || strcmp(c->values[v], eq + 1) == 0;
					}
				}
			}
			if (!found) {
				fail(saver->name, "has a setting shown with a choice that is not there");
			}
		}
	}
}

/* That taskbar.conf, where the settings are explained, names each saver and each setting. */
static void check_documented(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		fail(path, "cannot be read");
		return;
	}
	static char text[1 << 18];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = 0;
	fclose(f);
	for (int i = 0; i < saver_count; i++) {
		char word[96];
		snprintf(word, sizeof(word), " %s,", savers[i]->name);
		if (!strstr(text, word)) {
			fail(savers[i]->name, "is not in the list of savers in taskbar.conf");
		}
		for (const struct saver_option *o = savers[i]->options; o && o->key; o++) {
			snprintf(word, sizeof(word), "#     %s_%s ", savers[i]->name, o->key);
			if (!strstr(text, word)) {
				fail(word + 6, "is not explained in taskbar.conf");
			}
		}
	}
}

int main(int argc, char **argv) {
	if (argc > 1) {
		check_documented(argv[1]);
	}
	struct saver_options options = { .speed = 1, .text = "Hello", .photo_seconds = 3 };
	for (int i = 0; i < saver_count; i++) {
		const struct saver *saver = savers[i];
		if (saver_find(saver->name) != saver || saver_listed(saver->name) != saver) {
			fail(saver->name, "is not found by its name");
		}
		check_options(saver);
		try_saver(saver, saver->name, options);
	}

	// Doomsday: each of its kinds, found by their old names, but not in the list
	static const char *const kinds[] = { "hellfire", "thunderstorm", "blizzard" };
	const struct saver *doomsday = saver_find("doomsday");
	if (!doomsday) {
		fail("doomsday", "is not there");
		return 1;
	}
	for (int k = 0; k < 3; k++) {
		const char *settings[] = { "doomsday_kind", kinds[k] };
		struct saver_options o = options;
		o.settings = settings;
		o.setting_count = 2;
		char name[64];
		snprintf(name, sizeof(name), "doomsday (%s)", kinds[k]);
		try_saver(doomsday, name, o);
		const struct saver *old = saver_find(kinds[k]);
		if (!old || strcmp(old->name, kinds[k]) != 0 || saver_listed(kinds[k]) != doomsday) {
			fail(kinds[k], "is not found by its old name, or not shown as Doomsday");
		}
		for (int i = 0; i < saver_count; i++) {
			if (strcmp(savers[i]->name, kinds[k]) == 0) {
				fail(kinds[k], "is in the list next to Doomsday");
			}
		}
		// the settings of each kind are shown with it, and only with it
		for (const struct saver_option *opt = doomsday->options; opt->key; opt++) {
			bool mine = strncmp(opt->key, kinds[k], strlen(kinds[k])) == 0;
			if (strcmp(opt->key, "kind") != 0 && saver_option_shown(&o, doomsday, opt) != mine) {
				fail(opt->key, "is shown with another kind of Doomsday, or not with its own");
			}
		}
	}

	// the settings read as they are written
	const struct saver *pipes = saver_find("pipes");
	{
		const char *settings[] = { "doomsday_kind", "BLIZZARD", "doomsday_hellfire_flash", "no",
			"doomsday_blizzard_frost", "On", "doomsday_thunderstorm_rain", "no-such-value",
			"thunderstorm_lightning", "often", "doomsday_blizzard_bury", "maybe" };
		struct saver_options o = { .settings = settings, .setting_count = 12 };
		if (saver_choice(&o, doomsday, "kind") != 2) {
			fail("saver_choice", "does not read a value in capitals");
		}
		if (saver_toggle(&o, doomsday, "hellfire_flash") || !saver_toggle(&o, doomsday, "blizzard_frost")) {
			fail("saver_toggle", "does not read no and on");
		}
		if (saver_toggle(&o, doomsday, "blizzard_bury")) {
			fail("saver_toggle", "takes an unknown word for yes");
		}
		if (saver_choice(&o, doomsday, "thunderstorm_rain") != 0) {
			fail("saver_choice", "does not take the default for an unknown value");
		}
		if (saver_choice(&o, doomsday, "thunderstorm_lightning") != 1) {
			fail("saver_choice", "does not read a setting by the name it had before Doomsday");
		}
		if (!saver_toggle(&o, doomsday, "thunderstorm_strikes") ||
				!saver_toggle(NULL, doomsday, "blizzard_frost")) {
			fail("saver_toggle", "does not take the default when nothing is written");
		}
		if (pipes && saver_choice(&o, pipes, "kind") != 0) {
			fail("saver_choice", "reads the setting of another saver");
		}
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
