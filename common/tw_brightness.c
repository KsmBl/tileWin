#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "tw_brightness.h"
#include "tw_paths.h"

static int clamp_boost(int boost) {
	return boost < 100 ? 100 : boost > TW_BOOST_MAX ? TW_BOOST_MAX : boost;
}

double tw_boost_curve(double v, int boost) {
	v = isnan(v) || v < 0 ? 0 : v > 1 ? 1 : v;
	boost = clamp_boost(boost);
	if (boost == 100) {
		return v;
	}
	// a power curve keeps black black and white white and lifts what is between:
	// at the full boost a middle grey of 0.5 becomes 0.65, a dark 0.1 becomes 0.24
	double gamma = 1 + (TW_BOOST_GAMMA - 1) * (boost - 100) / (double)(TW_BOOST_MAX - 100);
	double out = pow(v, 1 / gamma);
	return out < 0 ? 0 : out > 1 ? 1 : out;
}

void tw_gamma_ramp(uint16_t *table, uint32_t size, const double rgb[3], int boost) {
	for (int c = 0; c < 3; c++) {
		double f = rgb[c] < 0 ? 0 : rgb[c] > 1 ? 1 : rgb[c];
		for (uint32_t i = 0; i < size; i++) {
			double v = size > 1 ? (double)i / (size - 1) : 1;
			table[c * size + i] = (uint16_t)lround(tw_boost_curve(v, boost) * f * 65535.0);
		}
	}
}

char *tw_boost_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = malloc(strlen(dir) + 32);
	if (path) {
		sprintf(path, "%s/brightness-boost", dir);
	}
	free(dir);
	return path;
}

int tw_boost_read(void) {
	char *path = tw_boost_path();
	char *value = path ? tw_read_first_line(path) : NULL;
	int boost = value ? clamp_boost(atoi(value)) : 100;
	free(value);
	free(path);
	return boost;
}

bool tw_boost_write(int boost) {
	boost = clamp_boost(boost);
	if (tw_boost_read() == boost) {
		return true;
	}
	char *path = tw_boost_path();
	char text[16];
	snprintf(text, sizeof(text), "%d\n", boost);
	bool ok = path && tw_write_string(path, text);
	free(path);
	return ok;
}

bool tw_boost_available(void) {
	// tilewin-nightlight holds this lock while it runs (single_instance there)
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return false;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/tilewin-nightlight-%s.lock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	bool held = flock(fd, LOCK_SH | LOCK_NB) != 0;
	close(fd); // lets go of the lock again when it could be taken here
	return held;
}

int tw_brightness_shown(int backlight) {
	if (backlight < 100) {
		return backlight;
	}
	return tw_boost_available() ? tw_boost_read() : backlight;
}

int tw_brightness_split(int percent) {
	if (percent > 100 && tw_boost_available()) {
		tw_boost_write(percent);
		return 100;
	}
	tw_boost_write(100);
	return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}
