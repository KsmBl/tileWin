#include <math.h>
#include "tw_grid.h"

void tw_grid_choose(int n, double w, double h, double target, int *cols, int *rows) {
	*cols = n > 0 ? n : 1;
	*rows = 1;
	if (n <= 1 || w <= 0 || h <= 0) {
		return;
	}
	double best_worst = 1e9, best_mean = 1e9;
	for (int c = 1; c <= n; c++) {
		int r = (n + c - 1) / c;
		int last = n - c * (r - 1); // windows in the last row
		double cell_h = h / r;
		// every row but the last has c windows, the last one has `last`
		double full = fabs(log((w / c) / cell_h / target));
		double tail = fabs(log((w / last) / cell_h / target));
		double worst = full > tail ? full : tail;
		double mean = (full * (n - last) + tail * last) / n;
		if (worst < best_worst - 1e-9 ||
				(fabs(worst - best_worst) <= 1e-9 && mean < best_mean - 1e-9)) {
			best_worst = worst;
			best_mean = mean;
			*cols = c;
			*rows = r;
		}
	}
}
