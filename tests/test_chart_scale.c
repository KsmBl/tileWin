/*
 * The scale of the charts (panel/draw.c): its top is the highest reading
 * rounded up to 1, 2 or 5 of its power of ten, so its labels are round
 * numbers, and rates are labelled in B/s, kB/s, MB/s and GB/s without
 * trailing ".0". A chart drawn with it has its three lines and labels.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "draw.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}

static void ceiling(double v, double want) {
	char what[96];
	snprintf(what, sizeof(what), "%g rounds up to %g (got %g)", v, want, pd_nice_ceiling(v));
	check(fabs(pd_nice_ceiling(v) - want) < want * 1e-9, what);
}

static void rate(double v, const char *want) {
	char got[32], what[96];
	pd_format_axis_rate(got, sizeof(got), v);
	snprintf(what, sizeof(what), "%g B/s reads \"%s\" (got \"%s\")", v, want, got);
	check(strcmp(got, want) == 0, what);
}

static void percent(char *out, size_t size, double v) {
	snprintf(out, size, "%.0f%%", v);
}

int main(void) {
	ceiling(0, 1);
	ceiling(-3, 1);
	ceiling(1, 1);
	ceiling(1.2, 2);
	ceiling(3, 5);
	ceiling(7, 10);
	ceiling(10, 10);
	ceiling(25000, 50000);
	ceiling(38000, 50000);
	ceiling(4200000, 5000000);
	ceiling(0.03, 0.05);

	rate(0, "0");
	rate(500, "500 B/s");
	rate(1000, "1 kB/s");
	rate(25000, "25 kB/s");
	rate(2500000, "2.5 MB/s");
	rate(5000000, "5 MB/s");
	rate(1e9, "1 GB/s");

	// a chart's scale, drawn: lines at half and the top, something at the labels
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 100);
	cairo_t *cr = cairo_create(surface);
	pd_chart_axis(cr, "Sans 9px", 0, 0, 200, 100, 100, percent, 0xff0000ff, 0x00ff00ff);
	cairo_surface_flush(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	// ARGB32 in memory, little endian: B G R A
	int red_half = 0, red_top = 0, green = 0;
	for (int x = 0; x < 200; x++) {
		red_half += data[50 * stride + 4 * x + 2] > 200;
		red_top += data[0 * stride + 4 * x + 2] > 200;
	}
	for (int i = 0; i < stride * 100; i += 4) {
		green += data[i + 1] > 150 && data[i + 2] < 100;
	}
	check(red_half > 150, "the line at half the scale is drawn");
	check(red_top > 150, "the line at its top is drawn");
	check(green > 20, "its labels are drawn");
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
