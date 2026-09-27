#ifndef _TW_SAVER_NOISE_H
#define _TW_SAVER_NOISE_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Noise and pixel helpers of the savers that work on the picture of the
 * screen pixel by pixel (Blizzard, Decay, Jungle).
 */

static inline uint32_t hash2(int x, int y, uint32_t seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/* Value noise, repeating every period cells sideways (0 for never). */
static inline float noise(float x, float y, int period, uint32_t seed) {
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = xi, x1 = xi + 1;
	if (period > 0) {
		x0 = ((xi % period) + period) % period;
		x1 = (x0 + 1) % period;
	}
	float a = (hash2(x0, yi, seed) & 0xffff) / 65535.0f;
	float b = (hash2(x1, yi, seed) & 0xffff) / 65535.0f;
	float c = (hash2(x0, yi + 1, seed) & 0xffff) / 65535.0f;
	float d = (hash2(x1, yi + 1, seed) & 0xffff) / 65535.0f;
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static inline float fbm(float x, float y, int period, uint32_t seed, int octaves) {
	float n = 0, amp = 0.5f, total = 0;
	for (int o = 0; o < octaves; o++) {
		n += noise(x, y, period, seed + o * 7) * amp;
		total += amp;
		x *= 2;
		y *= 2;
		period *= 2;
		amp *= 0.5f;
	}
	return n / total;
}

/* Noise worked out on a coarse grid and read between its points: for every pixel, cheaply. */
struct grid {
	int gw, gh, step;
	float *v;
};

static inline void grid_make(struct grid *g, int width, int height, int step, float cell, uint32_t seed) {
	g->step = step;
	g->gw = width / step + 2;
	g->gh = height / step + 2;
	g->v = malloc(sizeof(float) * g->gw * g->gh);
	for (int y = 0; y < g->gh; y++) {
		for (int x = 0; x < g->gw; x++) {
			g->v[y * g->gw + x] = fbm(x * step / cell, y * step / cell, 0, seed, 4);
		}
	}
}

static inline float grid_at(const struct grid *g, int x, int y) {
	int gx = x / g->step, gy = y / g->step;
	float fx = (float)(x % g->step) / g->step, fy = (float)(y % g->step) / g->step;
	const float *r0 = g->v + gy * g->gw + gx, *r1 = r0 + g->gw;
	return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
}

static inline float smooth(float edge0, float edge1, float x) {
	float k = (x - edge0) / (edge1 - edge0);
	k = k < 0 ? 0 : k > 1 ? 1 : k;
	return k * k * (3 - 2 * k);
}

static inline uint32_t mix_px(uint32_t a, uint32_t b, int k) {
	// k from 0 (a) to 256 (b), each channel at once in two lanes
	uint32_t rb = ((a & 0xff00ff) * (256 - k) + (b & 0xff00ff) * k) >> 8 & 0xff00ff;
	uint32_t g = ((a & 0x00ff00) * (256 - k) + (b & 0x00ff00) * k) >> 8 & 0x00ff00;
	return 0xff000000u | rb | g;
}

#endif
