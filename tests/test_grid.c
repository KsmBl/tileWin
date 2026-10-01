/*
 * The grid of "arrange optimal": windows close to 1.3:1, so 4 windows on a
 * wide screen make 2x2, not three and one stretched across the bottom.
 */
#include <stdio.h>
#include "tw_grid.h"

static int failures;

static void check(int n, double w, double h, int want_cols, int want_rows) {
	int cols, rows;
	tw_grid_choose(n, w, h, TW_GRID_ASPECT, &cols, &rows);
	if (cols != want_cols || rows != want_rows) {
		fprintf(stderr, "FAIL: %d windows on %.0fx%.0f: %dx%d, wanted %dx%d\n", n, w, h,
			cols, rows, want_cols, want_rows);
		failures++;
	}
}

int main(void) {
	// a 1280x720 screen without its taskbar, and a full HD one
	double sizes[][2] = { { 1280, 680 }, { 1920, 1040 } };
	for (int i = 0; i < 2; i++) {
		double w = sizes[i][0], h = sizes[i][1];
		check(1, w, h, 1, 1);
		check(2, w, h, 2, 1);
		check(3, w, h, 3, 1);
		check(4, w, h, 2, 2);
		check(5, w, h, 3, 2);
		check(6, w, h, 3, 2);
		check(8, w, h, 4, 2);
		check(9, w, h, 3, 3);
	}
	check(0, 1280, 680, 1, 1);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all grids as wanted\n");
	return 0;
}
