#ifndef _TILEWIN_PANEL_POPUP_H
#define _TILEWIN_PANEL_POPUP_H
#include "panel.h"

struct popup;

struct popup_vtable {
	void (*render)(struct popup *p, cairo_t *cr);
	void (*motion)(struct popup *p, double x, double y);
	void (*leave)(struct popup *p);
	void (*button)(struct popup *p, double x, double y, uint32_t button, bool pressed);
	void (*axis)(struct popup *p, double x, double y, int direction);
	void (*key)(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods);
	void (*destroy)(struct popup *p);
};

struct popup {
	enum popup_kind kind;
	struct panel *panel;
	struct panel_output *output;
	struct psurface *surface;
	struct popup *parent, *child;
	const struct popup_vtable *vtable;
	void *data;
	int x, y, width, height;
};

struct popup *popup_create(struct panel *panel, enum popup_kind kind,
	struct popup *parent, struct panel_output *output, int x, int y, int width,
	int height, const struct popup_vtable *vtable, void *data);
void popup_destroy(struct popup *p);
void popup_close_later(struct panel *panel);
void popup_set_dirty(struct popup *p);
/* Opens a (sub)menu at output-local coordinates. */
struct popup *menu_open_at(struct panel *panel, struct popup *parent,
	struct panel_output *output, list_t *items, bool owns, int x, int y,
	bool above, const char *context_id);
void menu_open_start_classic(struct panel *panel, struct panel_output *output,
	list_t *items, int x, int y);
cairo_t *popup_scratch_cairo(void);
/* Background, border and shadow shared by menus and flyouts. */
void popup_draw_frame(struct panel *panel, cairo_t *cr, int width, int height,
	int margin, const char *prefix);
int popup_shadow_margin(struct panel *panel);
double popup_radius(struct panel *panel, const char *prefix);
list_t *power_menu_items(struct panel *panel);
list_t *theme_menu_items(struct panel *panel);

#endif
