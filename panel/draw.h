#ifndef _TILEWIN_PANEL_DRAW_H
#define _TILEWIN_PANEL_DRAW_H
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include "cairo_util.h"
#include "tw_theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum pd_align {
	PD_LEFT,
	PD_CENTER,
	PD_RIGHT,
};

void pd_color(cairo_t *cr, uint32_t color);
void pd_rect(cairo_t *cr, double x, double y, double w, double h, uint32_t color);
void pd_rounded(cairo_t *cr, double x, double y, double w, double h, double r);
void pd_rounded4(cairo_t *cr, double x, double y, double w, double h,
	double tl, double tr, double br, double bl);
/* Sets the source from "<base>_gradient" (vertical stops) or "<base>". */
bool pd_source(cairo_t *cr, const struct tw_theme *t, const char *base,
	double y, double h, uint32_t fallback);
cairo_pattern_t *pd_gradient(const char *stops, double x0, double y0, double x1, double y1);
/* Fills the current path with the key's gradient or color. */
void pd_fill(cairo_t *cr, const struct tw_theme *t, const char *base,
	double y, double h, uint32_t fallback);
void pd_bevel(cairo_t *cr, double x, double y, double w, double h, bool sunken);
/* The colors pd_bevel draws with (those of the theme's decoration). */
void pd_set_bevel_colors(uint32_t hi, uint32_t light, uint32_t shadow, uint32_t dark);
void pd_border_sunken_thin(cairo_t *cr, double x, double y, double w, double h);

void pd_text_size(cairo_t *cr, const char *font, const char *text, int *w, int *h);
void pd_text(cairo_t *cr, const char *font, const char *text, double x, double y,
	double w, double h, uint32_t color, enum pd_align align);
/* Word-wrapped text of at most max_lines lines (0: any); returns its height. */
int pd_text_wrapped(cairo_t *cr, const char *font, const char *text, double x, double y,
	double w, int max_lines, uint32_t color, bool draw);
void pd_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size);

/* The top of the scale of a chart: v rounded up to 1, 2 or 5 times a power of ten. */
double pd_nice_ceiling(double v);
/* A label of the scale of a chart of rates: "0", "500 B/s", "2 MB/s", "1.5 GB/s". */
void pd_format_axis_rate(char *out, size_t size, double bytes_per_second);
/*
 * The scale of a chart: lines at 0, half and the top, labelled at the left
 * inside the chart (format writes a label for a value), drawn below the lines
 * of the values. Returns nothing; the chart keeps its whole width.
 */
void pd_chart_axis(cairo_t *cr, const char *font, double x, double y, double w, double h,
	double top, void (*format)(char *out, size_t size, double value), uint32_t line,
	uint32_t label);

/* glyphs, drawn inside a size x size square */
/* The flag of Windows XP: four glossy panels waving, with a shadow. */
void pd_glyph_xp_flag(cairo_t *cr, double x, double y, double size);
void pd_glyph_windows(cairo_t *cr, double x, double y, double size, uint32_t c1,
	uint32_t c2, uint32_t c3, uint32_t c4, bool wavy);
void pd_glyph_speaker(cairo_t *cr, double x, double y, double size, int level,
	bool muted, uint32_t color);
void pd_glyph_battery(cairo_t *cr, double x, double y, double size, int percent,
	bool charging, uint32_t color);
void pd_glyph_network(cairo_t *cr, double x, double y, double size, int bars,
	bool wireless, bool connected, uint32_t color);
void pd_glyph_search(cairo_t *cr, double x, double y, double size, uint32_t color);
void pd_glyph_arrow(cairo_t *cr, double x, double y, double size, int direction,
	uint32_t color);
void pd_glyph_tile(cairo_t *cr, double x, double y, double size, uint32_t color);
void pd_glyph_overlap(cairo_t *cr, double x, double y, double size, uint32_t color);
void pd_glyph_power(cairo_t *cr, double x, double y, double size, uint32_t color);
void pd_glyph_brightness(cairo_t *cr, double x, double y, double size, uint32_t color);
void pd_glyph_disk(cairo_t *cr, double x, double y, double size, bool active,
	uint32_t color);
void pd_glyph_generic_app(cairo_t *cr, double x, double y, double size);

#endif
