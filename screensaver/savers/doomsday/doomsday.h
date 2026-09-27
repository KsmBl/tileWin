#ifndef _TW_DOOMSDAY_H
#define _TW_DOOMSDAY_H
#include "saver_util.h"

/* Doomsday and its kinds, and what the tests look at in them. */

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
	bool inside;                     // no piece off the sides, lost, or sunk in
	long glass, wood, stone;         // samples of the screen made of each, now
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
