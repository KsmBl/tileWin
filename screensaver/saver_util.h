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

/*
 * The desktop a saver is drawn over: its windows and taskbars, from tileWin or
 * made up without it (the preview), and a picture of it: the screen as it was
 * before the saver, or drawn stand-ins. An RGB24 surface of width by height.
 */
cairo_surface_t *saver_desktop(const struct saver_options *options, int width, int height,
	struct saver_window *wins, int max, int *count, struct saver_window *bars, int *bar_count);

/*
 * The wallpaper of that screen as tileWin draws it, width by height, RGB24;
 * NULL without tileWin to ask (the preview).
 */
cairo_surface_t *saver_wallpaper(const char *output, int width, int height);
/*
 * The desktop with the windows taken off (and the taskbars too with no_bars):
 * where they stood, with margin around each for its shadow, the wallpaper
 * tileWin reports when it is the one on the screen, or else the colour of the
 * desktop around each; in the preview (not real) the stand-in wallpaper.
 */
cairo_surface_t *saver_bare_desktop(const struct saver_options *options, cairo_surface_t *desktop,
	int width, int height, const struct saver_window *wins, int count,
	const struct saver_window *bars, int bar_count, double margin, bool real, bool no_bars);
/* The wallpaper of the windows of its own that savers bring without tileWin. */
void saver_fake_wallpaper(cairo_t *cr, int width, int height);

/* The savers, in the files that draw them. */
extern const struct saver saver_blank, saver_bubbles, saver_mystify, saver_ribbons,
	saver_text3d, saver_photos;
extern const struct saver saver_starfield, saver_pipes, saver_flying;
extern const struct saver saver_aurora, saver_wordclock, saver_tiling, saver_diggers,
	saver_maze, saver_matrix, saver_ad, saver_gravity;
/* Doomsday, and the kinds it runs, which are not in the list themselves. */
extern const struct saver saver_doomsday, saver_hellfire, saver_storm, saver_blizzard,
	saver_decay, saver_jungle;
extern const struct saver *const doomsday_kinds[];
extern const int doomsday_kind_count;

/* What a Blizzard has done so far, for the tests. */
struct blizzard_stats {
	int ledges, icicles;
	int window_columns;              // columns of the tops of windows snow can lie on
	double windows, taskbar, ground; // snow lying on them, pixels of area
	double icicle_length;            // of all together
	double frost;                    // the part of the screen frosted over, 0 to 1
	long frost_outside;              // pixels of frost outside the windows and taskbars
	double storm;                    // how hard it snows now
	bool fits;                       // no heap deeper than its room or where nothing shows
	double deepest, deepest_x, deepest_y; // the deepest snow on a window, and its top
	double plastered;                // the part of the windows and taskbars snowed over
	int cracks;                      // in the glass of the screen
	double dark;                     // how far the light has failed, 0 to 1
};
void saver_blizzard_stats(void *state, struct blizzard_stats *out);

/* What Decay has done so far, for the tests. */
struct decay_stats {
	int pieces, cracks;              // the windows and taskbars were broken into
	int fallen, resting, flying, shattered; // pieces off, at rest in the rubble, in the air
	int standing;                    // windows and taskbars not all fallen yet
	double rubble, dust;             // mean height at the bottom, pixels
	double untouched;                // the part of the screen not aged at all yet
	double progress;                 // to dust, 1 all dust
	bool inside;                     // no piece off the sides or lost
};
void saver_decay_stats(void *state, struct decay_stats *out);

/* What Jungle has grown so far, for the tests. */
struct jungle_stats {
	int vines, growing;              // vines planned or grown, and growing now
	double length;                   // of all the vines together
	long leaves, blooms;             // leaves grown and growing; flowers opened so far
	int flowers, open, fronds;       // flowers, open now, giant fronds out
	double moss;                     // the part of the screen mossed over
	double covered;                  // the part covered by vines and leaves
	double progress, night;          // to all overgrown; how dark it is
	bool inside;                     // no butterfly lost
};
void saver_jungle_stats(void *state, struct jungle_stats *out);

#endif
