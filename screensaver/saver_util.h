#ifndef _TW_SAVER_UTIL_H
#define _TW_SAVER_UTIL_H
#include <cairo.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "savers.h"

/* Helpers the savers share; not part of what the rest of tileWin sees. */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A number in [0, 1). */
double saver_random(void);
/* A number in [low, high). */
static inline double saver_between(double low, double high) {
	return low + (high - low) * saver_random();
}

/* Hue, saturation and value in [0, 1] to red, green and blue. */
void saver_hsv(double h, double s, double v, double *r, double *g, double *b);
void saver_set_hsva(cairo_t *cr, double h, double s, double v, double a);

/* The unit of length: a thousandth of the shorter side. */
static inline double saver_unit(int width, int height) {
	return (width < height ? width : height) / 1000.0;
}

static inline double saver_clamp(double v, double low, double high) {
	return v < low ? low : v > high ? high : v;
}

/* A window on the screen, from tileWin: the title bar is its top title_h pixels. */
struct saver_window {
	double x, y, w, h;
	char title[64];
	double title_h, border;
};

/*
 * The windows tileWin shows on that screen, bottom first, in its coordinates,
 * and its taskbars (bars may be NULL); false without tileWin to ask.
 */
bool saver_tilewin_windows(const char *output, struct saver_window *wins, int max,
	int *count, struct saver_window *bars, int *bar_count);

/* The savers, in the files that draw them. */
extern const struct saver saver_blank, saver_bubbles, saver_mystify, saver_ribbons,
	saver_text3d, saver_photos;
extern const struct saver saver_starfield, saver_pipes, saver_flying;
extern const struct saver saver_aurora, saver_wordclock, saver_tiling, saver_diggers,
	saver_maze, saver_matrix, saver_hellfire;

#endif
