/*
 * The Blizzard of Doomsday, run on a picture in memory with the windows it
 * brings in the preview, and looked at from inside (saver_blizzard_stats) and
 * at what it drew:
 *  - snow piles up on the tops of the windows and on the taskbar, never
 *    deeper than there is room for and only where the surface shows, and the
 *    top of the deepest heap is white on the screen;
 *  - a drift rises at the bottom unless that is turned off;
 *  - frost comes only inside the windows and the taskbar, and not at all with
 *    it turned off; icicles grow; the storm builds up;
 *  - light snow leaves less than heavy snow;
 *  - it starts and ends at once (while the frost is still worked out), at
 *    tiny sizes too, and draws a frame in reasonable time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "saver_util.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

struct run {
	int w, h;
	void *state;
	cairo_surface_t *surface;
	cairo_t *cr;
	const char *settings[6];
	struct saver_options options;
};

static void start(struct run *r, int w, int h, const char *snow, const char *frost,
		const char *bury) {
	*r = (struct run){ .w = w, .h = h };
	const char *s[] = { "doomsday_blizzard_snow", snow, "doomsday_blizzard_frost", frost,
		"doomsday_blizzard_bury", bury };
	memcpy(r->settings, s, sizeof(s));
	r->options = (struct saver_options){ .speed = 1, .photo_seconds = 8,
		.settings = r->settings, .setting_count = 6 };
	r->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	r->cr = cairo_create(r->surface);
	r->state = saver_blizzard.create(w, h, &r->options);
}

/* Runs on for that many seconds in frames of a fifteenth; the stats after. */
static void run(struct run *r, double seconds, struct blizzard_stats *stats) {
	for (int i = 0; i < (int)(seconds * 15); i++) {
		saver_blizzard.draw(r->state, r->cr, r->w, r->h, 1 / 15.0);
	}
	cairo_surface_flush(r->surface);
	saver_blizzard_stats(r->state, stats);
}

static void finish(struct run *r) {
	saver_blizzard.destroy(r->state);
	cairo_destroy(r->cr);
	cairo_surface_destroy(r->surface);
}

/* How light the drawing is around a point, 0 to 255. */
static double light_at(struct run *r, int x, int y) {
	unsigned char *data = cairo_image_surface_get_data(r->surface);
	int stride = cairo_image_surface_get_stride(r->surface);
	double sum = 0;
	int n = 0;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			int px = x + dx, py = y + dy;
			if (px < 0 || py < 0 || px >= r->w || py >= r->h) {
				continue;
			}
			uint32_t p = *(uint32_t *)(data + py * stride + px * 4);
			sum += (((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3.0;
			n++;
		}
	}
	return n ? sum / n : 0;
}

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
	struct blizzard_stats st, early;
	struct run heavy;
	start(&heavy, 480, 300, "heavy", "yes", "yes");
	run(&heavy, 1, &early);
	check(early.ledges > 0, "no ledges for snow on the windows and the taskbar");
	check(early.frost == 0, "frost at once, before it had time to grow");
	bool fits = early.fits;
	for (int k = 0; k < 10; k++) {
		run(&heavy, 10, &st);
		fits = fits && st.fits;
	}
	printf("heavy, 101 s: %d ledges, snow %.0f on windows (%d columns, deepest %.1f), "
		"%.0f on the taskbar, %.0f drifted; %d icicles %.0f long; frost %.3f, %ld outside; "
		"storm %.2f\n", st.ledges, st.windows, st.window_columns, st.deepest, st.taskbar,
		st.ground, st.icicles, st.icicle_length, st.frost, st.frost_outside, st.storm);
	check(fits, "snow deeper than its room, or where the surface does not show");
	check(st.windows > early.windows * 3 && st.window_columns > 0, "no snow piling up on the windows");
	check(st.taskbar > early.taskbar * 3, "no snow piling up on the taskbar");
	check(st.ground > 0, "no drift at the bottom");
	check(st.icicles > 0 && st.icicle_length > 0, "no icicles growing");
	check(st.frost > 0.01, "no frost on the windows");
	check(st.frost_outside == 0, "frost outside the windows and the taskbar");
	check(st.storm > early.storm, "the storm does not build up");
	double white = light_at(&heavy, (int)st.deepest_x, (int)(st.deepest_y + st.deepest * 0.4));
	printf("the deepest heap, %.1f deep, is %.0f light\n", st.deepest, white);
	check(st.deepest > 2 && white > 190, "the snow on the windows is not seen");
	double heavy_depth = st.windows / (st.window_columns ? st.window_columns : 1);
	finish(&heavy);

	struct run light;
	start(&light, 480, 300, "light", "no", "no");
	run(&light, 101, &st);
	double light_depth = st.windows / (st.window_columns ? st.window_columns : 1);
	printf("light, 101 s: %.2f deep on the windows against %.2f heavy; frost %.3f, drift %.0f\n",
		light_depth, heavy_depth, st.frost, st.ground);
	check(st.fits, "light snow deeper than its room");
	check(light_depth < heavy_depth, "light snow leaves as much as heavy snow");
	check(st.frost == 0, "frost with the frost turned off");
	check(st.ground == 0, "a drift with the drift turned off");
	finish(&light);

	// started and ended at once, while the frost is still being worked out
	for (int i = 0; i < 3; i++) {
		struct run quick;
		start(&quick, 640, 400, "whiteout", "yes", "yes");
		finish(&quick);
	}
	// and at tiny sizes
	int sizes[][2] = { { 64, 40 }, { 8, 8 }, { 1, 1 } };
	for (int i = 0; i < 3; i++) {
		struct run tiny;
		start(&tiny, sizes[i][0], sizes[i][1], "whiteout", "yes", "yes");
		run(&tiny, 40, &st);
		check(st.fits, "snow deeper than its room at a tiny size");
		finish(&tiny);
	}

	// a frame in reasonable time (generous: this may be a debug build)
	struct run timed;
	start(&timed, 960, 540, "heavy", "yes", "yes");
	run(&timed, 20, &st);
	double t0 = now();
	for (int i = 0; i < 30; i++) {
		saver_blizzard.draw(timed.state, timed.cr, timed.w, timed.h, 1 / 30.0);
	}
	double ms = (now() - t0) / 30 * 1000;
	printf("960x540: %.1f ms a frame\n", ms);
	check(ms < 200, "a frame takes far too long");
	finish(&timed);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
