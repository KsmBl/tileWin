#ifndef _TW_GRID_H
#define _TW_GRID_H

/*
 * The grid "arrange optimal" lays n windows out in over an area of width w and
 * height h: rows of cols windows, the last row with the rest of them sharing
 * its width. Chosen so that the window furthest from width:height = target is
 * as close to it as can be (then the windows on average), so 4 windows on a
 * wide screen are 2x2 rather than 3 and one stretched across the bottom.
 */
#define TW_GRID_ASPECT 1.3

void tw_grid_choose(int n, double w, double h, double target, int *cols, int *rows);

#endif
