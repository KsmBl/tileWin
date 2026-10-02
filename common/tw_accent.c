#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_accent.h"
#include "tw_paths.h"

static struct {
	bool loaded, from_wallpaper, have;
	uint32_t color;
} acc;

static char *file_in(char *dir, const char *name) {
	if (!dir) {
		return NULL;
	}
	size_t n = strlen(dir) + strlen(name) + 2;
	char *path = malloc(n);
	if (path) {
		snprintf(path, n, "%s/%s", dir, name);
	}
	free(dir);
	return path;
}

static void load(void) {
	if (acc.loaded) {
		return;
	}
	acc.loaded = true;
	char *mode_path = file_in(tw_config_dir(), "accent");
	char *mode = mode_path ? tw_read_first_line(mode_path) : NULL;
	acc.from_wallpaper = mode && strcmp(mode, "wallpaper") == 0;
	free(mode);
	free(mode_path);
	char *cache_path = file_in(tw_cache_dir(), "accent");
	char *value = cache_path ? tw_read_first_line(cache_path) : NULL;
	unsigned int rgb;
	acc.have = value && value[0] == '#' && sscanf(value + 1, "%6x", &rgb) == 1;
	if (acc.have) {
		acc.color = (rgb << 8) | 0xff;
	}
	free(value);
	free(cache_path);
}

void tw_accent_reload(void) {
	acc.loaded = false;
}

bool tw_accent_from_wallpaper(void) {
	load();
	return acc.from_wallpaper;
}

bool tw_accent_set_from_wallpaper(bool enable) {
	char *dir = tw_config_dir();
	if (dir) {
		tw_mkdir_p(dir);
	}
	char *path = file_in(dir, "accent");
	bool ok = path && tw_write_string(path, enable ? "wallpaper\n" : "theme\n");
	free(path);
	tw_accent_reload();
	return ok;
}

bool tw_accent_wallpaper_color(uint32_t *color) {
	load();
	if (acc.have) {
		*color = acc.color;
	}
	return acc.have;
}

bool tw_accent_store(uint32_t color) {
	load();
	color |= 0xff;
	if (acc.have && acc.color == color) {
		return false;
	}
	acc.have = true;
	acc.color = color;
	char *dir = tw_cache_dir();
	if (dir) {
		tw_mkdir_p(dir);
	}
	char *path = file_in(dir, "accent");
	char text[16];
	snprintf(text, sizeof(text), "#%06x\n", color >> 8);
	if (path) {
		tw_write_string(path, text);
	}
	free(path);
	return true;
}

#define BUCKETS 24

bool tw_accent_pick(const unsigned char *data, int width, int height, int stride,
		uint32_t *color) {
	if (!data || width <= 0 || height <= 0) {
		return false;
	}
	double weight[BUCKETS] = { 0 }, sum[BUCKETS][3] = { { 0 } };
	// about a hundred samples across, whatever the size
	int step = width / 96 > 0 ? width / 96 : 1;
	for (int y = 0; y < height; y += step) {
		const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
		for (int x = 0; x < width; x += step) {
			uint32_t p = row[x];
			double a = ((p >> 24) & 0xff) / 255.0;
			if (a < 0.5) {
				continue;
			}
			double r = ((p >> 16) & 0xff) / 255.0 / a, g = ((p >> 8) & 0xff) / 255.0 / a,
				b = (p & 0xff) / 255.0 / a;
			double hi = fmax(r, fmax(g, b)), lo = fmin(r, fmin(g, b));
			double s = hi > 0 ? (hi - lo) / hi : 0;
			if (s < 0.25 || hi < 0.2) {
				continue; // greys, and what is nearly black, carry no color
			}
			double h;
			double d = hi - lo;
			if (hi == r) {
				h = fmod((g - b) / d, 6.0);
			} else if (hi == g) {
				h = (b - r) / d + 2;
			} else {
				h = (r - g) / d + 4;
			}
			h = h < 0 ? h + 6 : h;
			int bucket = (int)(h / 6.0 * BUCKETS) % BUCKETS;
			double w = s * hi;
			weight[bucket] += w;
			sum[bucket][0] += r * w;
			sum[bucket][1] += g * w;
			sum[bucket][2] += b * w;
		}
	}
	// a hue with its neighbours: a wide band of one color wins over specks
	int best = -1;
	double best_score = 0;
	for (int i = 0; i < BUCKETS; i++) {
		double score = weight[i] + 0.5 * (weight[(i + 1) % BUCKETS] +
			weight[(i + BUCKETS - 1) % BUCKETS]);
		if (weight[i] > 0 && score > best_score) {
			best = i;
			best_score = score;
		}
	}
	if (best < 0) {
		return false;
	}
	double r = sum[best][0] / weight[best], g = sum[best][1] / weight[best],
		b = sum[best][2] / weight[best];
	// deep enough for white text on it, and not so dark it looks black
	for (int i = 0; i < 20; i++) {
		double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		if (lum > 0.42) {
			r *= 0.9;
			g *= 0.9;
			b *= 0.9;
		} else if (lum < 0.12) {
			r = r + (1 - r) * 0.08;
			g = g + (1 - g) * 0.08;
			b = b + (1 - b) * 0.08;
		} else {
			break;
		}
	}
	*color = ((uint32_t)lround(r * 255) << 24) | ((uint32_t)lround(g * 255) << 16) |
		((uint32_t)lround(b * 255) << 8) | 0xff;
	return true;
}
