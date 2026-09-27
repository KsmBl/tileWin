/*
 * Decay and Jungle, the kinds of Doomsday that take their time, run at their
 * fastest pace on a picture in memory with the windows they bring in the
 * preview, and looked at from inside and at what they drew.
 *
 * Decay: it ages from the start, the windows are glass in frames of wood or
 * stone, cracks show before anything falls, the windows and the taskbar come
 * off in pieces that knock about, break and come to rest (never off the
 * sides or through the bottom, also at the fastest speed), and at the end all
 * is down, no glass, wood or stone is left, the rubble is buried in dust, and
 * the screen is the colour of dust.
 *
 * Jungle: it grows on and on (vines, leaves, flowers that open and close,
 * the giant fronds), smoothly, a little every frame and not in jumps, moss covers the screen, the plants cover much of it,
 * nights come unless turned off, the butterflies stay about, and at the end
 * the screen is green.
 *
 * Both start and end at once too, and at tiny sizes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

struct run {
	const struct saver *saver;
	int w, h;
	void *state;
	cairo_surface_t *surface;
	cairo_t *cr;
	const char *settings[8];
	struct saver_options options;
};

static void start(struct run *r, const struct saver *saver, int w, int h, const char *const *set,
		int n) {
	*r = (struct run){ .saver = saver, .w = w, .h = h };
	memcpy(r->settings, set, sizeof(char *) * n);
	r->options = (struct saver_options){ .speed = 1, .photo_seconds = 8, .settings = r->settings,
		.setting_count = n };
	r->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	r->cr = cairo_create(r->surface);
	r->state = saver->create(w, h, &r->options);
}

/* Runs on for that many seconds in frames of dt seconds. */
static void run_dt(struct run *r, double seconds, double dt) {
	for (int i = 0; i < (int)(seconds / dt); i++) {
		r->saver->draw(r->state, r->cr, r->w, r->h, dt);
	}
	cairo_surface_flush(r->surface);
}

static void run(struct run *r, double seconds) {
	run_dt(r, seconds, 1 / 15.0);
}

static void finish(struct run *r) {
	r->saver->destroy(r->state);
	cairo_destroy(r->cr);
	cairo_surface_destroy(r->surface);
}

/* The mean colour of the picture, and how far its pixels are from it on average. */
static void colours(struct run *r, double mean[3], double *spread) {
	unsigned char *data = cairo_image_surface_get_data(r->surface);
	int stride = cairo_image_surface_get_stride(r->surface);
	double sum[3] = { 0 };
	long n = 0;
	for (int y = 0; y < r->h; y += 2) {
		for (int x = 0; x < r->w; x += 2) {
			uint32_t p = *(uint32_t *)(data + y * stride + x * 4);
			sum[0] += (p >> 16) & 255;
			sum[1] += (p >> 8) & 255;
			sum[2] += p & 255;
			n++;
		}
	}
	for (int k = 0; k < 3; k++) {
		mean[k] = sum[k] / n;
	}
	double dev = 0;
	for (int y = 0; y < r->h; y += 2) {
		for (int x = 0; x < r->w; x += 2) {
			uint32_t p = *(uint32_t *)(data + y * stride + x * 4);
			dev += fabs(((p >> 16) & 255) - mean[0]) + fabs(((p >> 8) & 255) - mean[1]) +
				fabs((p & 255) - mean[2]);
		}
	}
	*spread = dev / n / 3;
}

static void test_decay(void) {
	static const char *const set[] = { "doomsday_decay_pace", "fast" };
	struct run r;
	struct decay_stats st;
	start(&r, &saver_decay, 480, 300, set, 2);
	double mean[3], spread_before;
	run(&r, 0.5);
	colours(&r, mean, &spread_before);
	run(&r, 9.5);
	saver_decay_stats(r.state, &st);
	printf("decay, 10 s: %d pieces, %d cracks, %.0f%% not aged\n", st.pieces, st.cracks,
		st.untouched * 100);
	check(st.pieces > 10 && st.cracks > 10, "the windows are not broken into pieces");
	check(st.glass > 0 && st.wood + st.stone > 0, "the windows are not glass in frames of wood or stone");
	check(st.fallen == 0, "pieces fall at once");
	check(st.untouched < 0.97, "nothing ages at first");
	bool inside = st.inside;
	int most_flying = 0, fallen_at_90 = 0;
	for (int k = 0; k < 19; k++) {
		run(&r, 10);
		saver_decay_stats(r.state, &st);
		inside = inside && st.inside;
		most_flying = st.flying > most_flying ? st.flying : most_flying;
		if (k == 7) {
			fallen_at_90 = st.fallen;
		}
	}
	printf("decay, 200 s: %d of %d pieces fell (%d at 90 s), %d shattered, %d at rest, %d in "
		"the air (%d at most); %d standing; rubble %.1f, dust %.1f; %.1f%% not aged\n",
		st.fallen, st.pieces, fallen_at_90, st.shattered, st.resting, st.flying, most_flying,
		st.standing, st.rubble, st.dust, st.untouched * 100);
	check(inside, "a piece left the screen at a side or went astray");
	check(fallen_at_90 > 0 && fallen_at_90 < st.pieces, "the windows do not fall apart bit by bit");
	check(most_flying > 1, "pieces do not fall");
	check(st.fallen == st.pieces && st.standing == 0, "not everything fell apart in the end");
	check(st.flying <= 2, "the pieces do not come to rest");
	check(st.rubble > 0 && st.dust > st.rubble, "the rubble is not buried in dust");
	check(st.untouched < 0.01, "not everything aged");
	check(st.shattered > 0, "nothing broke when it landed");
	check(st.glass == 0 && st.wood == 0 && st.stone == 0, "glass, wood or stone is left at the end");
	double spread;
	colours(&r, mean, &spread);
	printf("decay, the end: mean colour %.0f %.0f %.0f, spread %.1f (at the start %.1f)\n",
		mean[0], mean[1], mean[2], spread, spread_before);
	check(mean[0] >= mean[2] && mean[0] - mean[2] < 60 && spread < spread_before * 0.6,
		"the end is not plain dust");
	finish(&r);
}

/* Decay at the fastest speed: frames of a twelfth of a second of it. */
static void test_decay_fast(void) {
	static const char *const set[] = { "doomsday_decay_pace", "fast" };
	struct run r;
	struct decay_stats st;
	start(&r, &saver_decay, 480, 300, set, 2);
	bool inside = true;
	for (int k = 0; k < 20; k++) {
		run_dt(&r, 10, 2.5 / 30);
		saver_decay_stats(r.state, &st);
		inside = inside && st.inside;
	}
	printf("decay at 2.5 times the speed: %d of %d pieces fell, %d shattered, %d in the air\n",
		st.fallen, st.pieces, st.shattered, st.flying);
	check(inside, "at the fastest speed a piece left the screen or sank through the bottom");
	check(st.fallen == st.pieces && st.flying <= 2, "at the fastest speed the pieces do not come to rest");
	finish(&r);
}

/* How many pixels of the picture changed since the copy in last, which is brought up to date. */
static long changed(struct run *r, uint32_t *last) {
	unsigned char *data = cairo_image_surface_get_data(r->surface);
	int stride = cairo_image_surface_get_stride(r->surface);
	long n = 0;
	for (int y = 0; y < r->h; y++) {
		uint32_t *row = (uint32_t *)(data + y * stride);
		for (int x = 0; x < r->w; x++) {
			n += row[x] != last[y * r->w + x];
			last[y * r->w + x] = row[x];
		}
	}
	return n;
}

static void test_jungle(void) {
	static const char *const set[] = { "doomsday_jungle_pace", "fast" };
	struct run r;
	struct jungle_stats st;
	start(&r, &saver_jungle, 480, 300, set, 2);
	run(&r, 1);
	saver_jungle_stats(r.state, &st);
	check(st.leaves == 0 && st.moss < 0.02, "the jungle is there at once");
	long leaves = 0;
	double length = 0, most_night = 0;
	long blooms = 0;
	int most_open = 0;
	bool inside = true, growing = true;
	for (int k = 0; k < 18; k++) {
		run(&r, 10);
		saver_jungle_stats(r.state, &st);
		if (k > 0 && k < 10) {
			growing = growing && st.leaves > leaves && st.length > length;
		}
		leaves = st.leaves;
		length = st.length;
		inside = inside && st.inside;
		most_night = st.night > most_night ? st.night : most_night;
		most_open = st.open > most_open ? st.open : most_open;
		blooms = st.blooms;
	}
	printf("jungle, 181 s: %d vines %.0f long, %ld leaves, %d flowers opened %ld times (%d open "
		"at once at most), %d fronds; moss %.0f%%, plants over %.0f%%; darkest night %.2f\n",
		st.vines, st.length, st.leaves, st.flowers, blooms, most_open, st.fronds, st.moss * 100,
		st.covered * 100, most_night);
	check(growing, "the jungle does not keep growing");
	check(st.flowers > 0 && blooms > st.flowers / 2 && most_open > 0, "no flowers open");
	check(st.fronds == 9, "the giant fronds do not all come");
	check(st.moss > 0.95, "the moss does not cover the screen");
	check(st.covered > 0.25, "the plants do not cover much of the screen");
	check(most_night > 0.5, "no night falls");
	check(inside, "a butterfly got lost");
	double mean[3], spread;
	colours(&r, mean, &spread);
	printf("jungle, the end: mean colour %.0f %.0f %.0f\n", mean[0], mean[1], mean[2]);
	check(mean[1] > mean[0] && mean[1] > mean[2], "the end is not green");
	finish(&r);

	// without nights and creatures; and growing smoothly: about as much changes every
	// frame, not all at once every few frames
	static const char *const calm[] = { "doomsday_jungle_pace", "fast", "doomsday_jungle_nights",
		"no", "doomsday_jungle_creatures", "no" };
	start(&r, &saver_jungle, 320, 200, calm, 6);
	most_night = 0;
	for (int k = 0; k < 4; k++) {
		run(&r, 10);
		saver_jungle_stats(r.state, &st);
		most_night = st.night > most_night ? st.night : most_night;
	}
	uint32_t *last = calloc((size_t)r.w * r.h, sizeof(uint32_t));
	run_dt(&r, 1 / 30.0, 1 / 30.0);
	changed(&r, last);
	long counts[24], total = 0, most = 0;
	for (int k = 0; k < 24; k++) {
		run_dt(&r, 1 / 30.0, 1 / 30.0);
		counts[k] = changed(&r, last);
		total += counts[k];
		most = counts[k] > most ? counts[k] : most;
	}
	long quiet = 0;
	for (int k = 0; k < 24; k++) {
		quiet += counts[k] < total / 24 / 4;
	}
	printf("jungle, a second of frames: %ld pixels change a frame on average, %ld at most, %ld "
		"frames with next to nothing\n", total / 24, most, quiet);
	check(most < total / 24 * 4 && quiet == 0, "the jungle grows in jumps, not smoothly");
	free(last);
	for (int k = 0; k < 4; k++) {
		run(&r, 10);
		saver_jungle_stats(r.state, &st);
		most_night = st.night > most_night ? st.night : most_night;
	}
	check(most_night == 0, "night falls with nights turned off");
	finish(&r);
}

int main(void) {
	test_decay();
	test_decay_fast();
	test_jungle();
	// started and ended at once, and at tiny sizes
	static const char *const fast[] = { "doomsday_decay_pace", "fast", "doomsday_jungle_pace",
		"fast" };
	const struct saver *kinds[] = { &saver_decay, &saver_jungle };
	for (int k = 0; k < 2; k++) {
		struct run r;
		start(&r, kinds[k], 640, 400, fast, 4);
		finish(&r);
		int sizes[][2] = { { 64, 40 }, { 8, 8 }, { 1, 1 } };
		for (int i = 0; i < 3; i++) {
			start(&r, kinds[k], sizes[i][0], sizes[i][1], fast, 4);
			run(&r, 160);
			finish(&r);
		}
	}
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
