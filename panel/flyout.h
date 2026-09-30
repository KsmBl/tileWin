#ifndef _TILEWIN_PANEL_FLYOUT_H
#define _TILEWIN_PANEL_FLYOUT_H
#include "panel.h"

/*
 * Colors and fonts of flyouts (network, volume, CPU, memory, quick settings,
 * notifications and the other widget flyouts). They come from the theme's
 * flyout section, and from its menu section where that has none, so the
 * flyouts look like the rest of the theme: a raised grey box on Windows 95,
 * the Luna frame on XP, glass on 7, the dark panes of 8 and 10.
 */
struct fly_style {
	enum pstyle style;
	bool dark; // dark menu background: overlays are light instead of dark
	uint32_t fg, dim, accent, hover, track, line, error;
	uint32_t field_bg, field_fg, button_bg, button_hover, button_border;
	uint32_t link; // the links at the bottom
	uint32_t bar;  // the filled part of usage bars
	uint32_t chart_bg, chart_grid, chart_line; // the charts of the last minute
	double chart_radius;
	int frame; // width of a frame around the content (Luna, glass), 0 for none
	const char *font, *bold;
	char big[128];
};

void fly_style_init(struct fly_style *st, struct panel *panel);
/* The box of a flyout, with its shadow and the theme's frame. */
void fly_draw_frame(struct popup *p, cairo_t *cr, const struct fly_style *st);
/* The background of a chart of the last minute, with its grid lines. */
void fly_draw_chart_bg(cairo_t *cr, const struct fly_style *st, struct pbox g);
/* A usage bar: share (0-1) of it filled in color. */
void fly_draw_bar(cairo_t *cr, const struct fly_style *st, double x, double y, double w,
	double h, double share, uint32_t color);
void fill_hover(cairo_t *cr, const struct fly_style *st, struct pbox b);
void draw_line(cairo_t *cr, const struct fly_style *st, struct popup *p, int y);
/* 0-100 for a pointer x on a slider box. */
int slider_value(struct pbox b, double x);
void draw_slider(cairo_t *cr, const struct fly_style *st, struct pbox b, int value);
/* A 40x20 on/off switch. */
void draw_switch(cairo_t *cr, const struct fly_style *st, int x, int y, bool on);
void draw_button(cairo_t *cr, const struct fly_style *st, struct pbox b,
	const char *label, bool primary, bool hover);
/* The Bluetooth rune in a size x size square. */
void draw_bluetooth_glyph(cairo_t *cr, double x, double y, double size, uint32_t color);

#endif
