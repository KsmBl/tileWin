#ifndef _TILEWIN_PANEL_FLYOUT_H
#define _TILEWIN_PANEL_FLYOUT_H
#include "panel.h"

/* Colors and fonts of flyouts (network, volume, quick settings, notifications). */
struct fly_style {
	enum pstyle style;
	bool dark; // dark menu background: overlays are light instead of dark
	uint32_t fg, dim, accent, hover, track, line, error;
	uint32_t field_bg, field_fg, button_bg, button_hover, button_border;
	const char *font, *bold;
	char big[128];
};

void fly_style_init(struct fly_style *st, struct panel *panel);
void fill_hover(cairo_t *cr, const struct fly_style *st, struct pbox b);
void draw_line(cairo_t *cr, const struct fly_style *st, struct popup *p, int y);
/* 0-100 for a pointer x on a slider box. */
int slider_value(struct pbox b, double x);
void draw_slider(cairo_t *cr, const struct fly_style *st, struct pbox b, int value);
/* A 40x20 on/off switch. */
void draw_switch(cairo_t *cr, const struct fly_style *st, int x, int y, bool on);
void draw_button(cairo_t *cr, const struct fly_style *st, struct pbox b,
	const char *label, bool primary, bool hover);

#endif
