/*
 * Brightness over 100 %: the gamma curve of the boost (monotonic, the same at
 * 100 %, black and white kept, clamped, times the night light's channel
 * factors), and its state file with the split between backlight and boost.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "tw_brightness.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: " __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		failures++; \
	} \
} while (0)

static void curve(void) {
	for (int i = 0; i <= 1000; i++) {
		double v = i / 1000.0;
		CHECK(tw_boost_curve(v, 100) == v, "100 %% changes %.3f", v);
		CHECK(tw_boost_curve(v, 50) == v, "below 100 %% changes %.3f", v);
	}
	for (int boost = 101; boost <= TW_BOOST_MAX; boost++) {
		CHECK(tw_boost_curve(0, boost) == 0, "black is not black at %d", boost);
		CHECK(fabs(tw_boost_curve(1, boost) - 1) < 1e-12, "white is not white at %d", boost);
		double last = -1;
		for (int i = 0; i <= 1000; i++) {
			double v = i / 1000.0, out = tw_boost_curve(v, boost);
			CHECK(out >= last, "not monotonic at %d, %.3f", boost, v);
			CHECK(out >= v - 1e-12, "darker at %d, %.3f", boost, v);
			CHECK(out <= 1, "over 1 at %d, %.3f", boost, v);
			CHECK(out >= tw_boost_curve(v, boost - 1) - 1e-12,
				"%d is darker than %d at %.3f", boost, boost - 1, v);
			last = out;
		}
	}
	// clamped: outside 0-1 and over the maximum
	CHECK(tw_boost_curve(-0.5, 120) == 0, "below 0");
	CHECK(tw_boost_curve(1.5, 120) == 1, "over 1");
	CHECK(tw_boost_curve(NAN, 120) == 0, "NaN");
	CHECK(tw_boost_curve(0.5, 500) == tw_boost_curve(0.5, TW_BOOST_MAX), "over the maximum");
	// a visible lift of the middle tones at the full boost
	double mid = tw_boost_curve(0.5, TW_BOOST_MAX);
	CHECK(mid > 0.6 && mid < 0.7, "middle grey at the full boost is %.3f", mid);
	printf("middle grey 0.5: 115%% %.3f, 130%% %.3f\n", tw_boost_curve(0.5, 115), mid);
}

static void ramp(void) {
	enum { SIZE = 256 };
	uint16_t table[3 * SIZE];
	double neutral[3] = { 1, 1, 1 };
	tw_gamma_ramp(table, SIZE, neutral, 100);
	for (int i = 0; i < SIZE; i++) {
		long want = lround(i / (double)(SIZE - 1) * 65535);
		CHECK(table[i] == want && table[SIZE + i] == want && table[2 * SIZE + i] == want,
			"the neutral table is not the identity at %d", i);
	}
	// the warm colors of the night light, with and without the boost
	double warm[3] = { 1, 0.75, 0.5 };
	uint16_t plain[3 * SIZE];
	tw_gamma_ramp(plain, SIZE, warm, 100);
	tw_gamma_ramp(table, SIZE, warm, 125);
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < SIZE; i++) {
			double v = i / (double)(SIZE - 1);
			long want = lround(tw_boost_curve(v, 125) * warm[c] * 65535);
			CHECK(table[c * SIZE + i] == want, "channel %d at %d: %u, wanted %ld", c, i,
				table[c * SIZE + i], want);
			CHECK(table[c * SIZE + i] >= plain[c * SIZE + i], "boost darker in %d at %d", c, i);
			CHECK(table[c * SIZE + i] <= lround(warm[c] * 65535),
				"channel %d goes over its temperature factor at %d", c, i);
			CHECK(i == 0 || table[c * SIZE + i] >= table[c * SIZE + i - 1],
				"channel %d not monotonic at %d", c, i);
		}
		CHECK(table[c * SIZE + SIZE - 1] == plain[c * SIZE + SIZE - 1],
			"the boost changes white of channel %d", c);
	}
}

static void state(const char *dir) {
	setenv("XDG_STATE_HOME", dir, 1);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	setenv("WAYLAND_DISPLAY", "wayland-test", 1);
	char lock[512], file[512];
	snprintf(lock, sizeof(lock), "%s/tilewin-nightlight-wayland-test.lock", dir);
	snprintf(file, sizeof(file), "%s/tileWin/brightness-boost", dir);

	CHECK(tw_boost_read() == 100, "no file is not 100");
	CHECK(!tw_boost_available(), "available without the night light");
	// without the night light nothing goes over 100
	CHECK(tw_brightness_split(120) == 100, "split without the night light");
	CHECK(tw_boost_read() == 100, "boost written without the night light");
	CHECK(tw_brightness_shown(100) == 100, "shown without the night light");

	// the night light runs: it holds the lock
	int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	CHECK(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0, "can't take the lock");
	CHECK(tw_boost_available(), "not available with the night light");
	CHECK(tw_brightness_split(115) == 100, "the backlight is not full over 100");
	CHECK(tw_boost_read() == 115, "boost not 115 but %d", tw_boost_read());
	CHECK(access(file, R_OK) == 0, "no state file");
	CHECK(tw_brightness_shown(100) == 115, "shown is not 115");
	CHECK(tw_brightness_shown(80) == 80, "shown is not the backlight below full");
	CHECK(tw_brightness_split(500) == 100 && tw_boost_read() == TW_BOOST_MAX, "not clamped");
	CHECK(tw_brightness_split(60) == 60, "the backlight is not 60");
	// the bottom: 0 is shown for the lowest, and the backlight never goes off
	CHECK(tw_brightness_shown(1) == 0, "the lowest backlight is not shown as 0");
	CHECK(tw_brightness_shown(0) == 0, "an off backlight is not shown as 0");
	CHECK(tw_brightness_shown(5) == 5, "5 is not shown as 5");
	CHECK(tw_brightness_split(0) == TW_BACKLIGHT_LOWEST, "0 turns the backlight off");
	CHECK(tw_brightness_split(-5) == TW_BACKLIGHT_LOWEST, "below 0 is not the lowest");
	CHECK(tw_brightness_split(5) == 5, "the backlight is not 5");
	CHECK(tw_boost_read() == 100, "the boost stays below 100");
	tw_boost_write(3);
	CHECK(tw_boost_read() == 100, "boost below 100");
	close(fd);
	CHECK(!tw_boost_available(), "available after the night light ended");
	unlink(lock);
	unlink(file);
	char sub[512];
	snprintf(sub, sizeof(sub), "%s/tileWin", dir);
	rmdir(sub);
}

int main(void) {
	curve();
	ramp();
	char dir[] = "/tmp/tilewin-boost-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	state(dir);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("the brightness boost works as wanted\n");
	return 0;
}
