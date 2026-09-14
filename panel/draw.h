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
void pd_border_sunken_thin(cairo_t *cr, double x, double y, double w, double h);

void pd_text_size(cairo_t *cr, const char *font, const char *text, int *w, int *h);
void pd_text(cairo_t *cr, const char *font, const char *text, double x, double y,
	double w, double h, uint32_t color, enum pd_align align);
void pd_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size);

/* glyphs, drawn inside a size x size square */
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
void pd_glyph_generic_app(cairo_t *cr, double x, double y, double size);

#endif
