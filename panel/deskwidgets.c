#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "popup.h"
#include "stringop.h"
#include "tw_paths.h"
#include "tw_widgets.h"

/*
 * Widgets on the desktop. taskbar.conf lists them in a block of their own,
 *
 *   desktop_widgets {
 *       clock { style analog; column -1; row 0 }
 *       cpu { style chart; size 3x2; output all }
 *   }
 *
 * and each of them is the same widget the taskbar shows: the "widget <name>"
 * block of taskbar.conf sets it up for both places, and whatever its entry
 * here says on top of that (a format, a color) is only for the desktop.
 *
 * The widgets sit in the grid of the desktop icons and take whole cells of it:
 * "column" and "row" say where (negative numbers count from the right and the
 * bottom), "size" how many cells, and the style decides when it does not.
 * Widgets without a place line up along the right edge, and the icons flow
 * around all of them. The "compact" style draws the widget the way the taskbar
 * does, laid out for a taskbar DESK_BASE pixels high and drawn bigger; the
 * others (gadgets.c) draw what the widget measures their own way. Each widget
 * has a surface of its own over the wallpaper and under the windows.
 *
 * Dragging a widget moves it from cell to cell. Where it was put is kept in
 * ~/.local/state/tileWin/desktop-widget-cells, together with the place the
 * config gave it then, so changing the place in the config (or in the
 * settings) still wins over an older drag.
 */

#define DESK_BASE 40    // taskbar height the compact style is laid out for
#define DESK_PAD 6      // around a compact widget, inside the card, before scaling
#define DESK_DRAG 6     // pixels the pointer moves before a press becomes a drag
#define DESK_INSET 4    // between a card and the edges of its cells

struct desk_surface {
	struct deskwidget *dw;
	struct panel_output *output;
	struct psurface *surface;
	int col, row;         // the cell of its upper left corner
	int columns, rows;    // how many cells it covers
	bool placed;          // dragged to col and row by hand
	int content_width;    // compact and tile: the width the widget measured
	// where the widget draws: surface = offset + (widget + pad) * k
	double k, ox, oy, pad;
	double px, py;        // pointer, surface coordinates
	bool inside, pressed, dragging;
	double press_x, press_y;
	int drag_x, drag_y;   // where the card was when the drag began, in pixels
	int drag_col, drag_row;
	bool drag_placed;
	struct wl_list link;  // deskwidget::surfaces
};

struct deskwidget {
	struct panel *panel;
	struct widget *widget;
	struct twconf_node *conf; // its entry in desktop_widgets
	const struct tw_widget_style *style;
	int want_columns, want_rows; // from "size"; 0 lets the style decide
	bool card;
	struct wl_list surfaces;  // desk_surface::link
};

static bool is_desk_surface(struct psurface *s);
static void layout_output(struct panel *panel, struct panel_output *output);

static bool compact(struct deskwidget *dw) {
	return !dw->style || strcmp(dw->style->name, "compact") == 0;
}

static bool tile(struct deskwidget *dw) {
	return dw->style && strcmp(dw->style->name, "tile") == 0;
}

/* ---------- where widgets were dragged to ---------- */

struct saved_place {
	char *key;       // "<widget> <output>"
	int col, row;    // where it was put
	char *config;    // what the config said when it was put there
};

static list_t *saved; // struct saved_place *

static char *saved_path(void) {
	char *dir = tw_state_dir();
	char *path = dir ? format_str("%s/desktop-widget-cells", dir) : NULL;
	free(dir);
	return path;
}

static void saved_load(void) {
	if (saved) {
		return;
	}
	saved = create_list();
	char *path = saved_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	char line[512];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		struct saved_place place = { 0 };
		char config[256];
		int offset = 0;
		if (sscanf(line, "%d %d %255s %n", &place.col, &place.row, config, &offset) >= 3 &&
				offset > 0 && line[offset]) {
			struct saved_place *copy = malloc(sizeof(*copy));
			*copy = place;
			copy->config = strdup(config);
			copy->key = strdup(line + offset);
			list_add(saved, copy);
		}
	}
	if (f) {
		fclose(f);
	}
}

static void saved_write(void) {
	char *path = saved_path();
	if (!path || !saved) {
		free(path);
		return;
	}
	char *content = strdup("");
	for (int i = 0; i < saved->length; i++) {
		struct saved_place *place = saved->items[i];
		char *next = format_str("%s%d %d %s %s\n", content, place->col, place->row,
			place->config, place->key);
		free(content);
		content = next;
	}
	char *dir = tw_state_dir();
	if (dir) {
		tw_mkdir_p(dir);
		free(dir);
	}
	tw_write_string(path, content);
	free(content);
	free(path);
}

static char *place_key(struct desk_surface *ds) {
	return format_str("%s %s", ds->dw->widget->name,
		ds->output && ds->output->name ? ds->output->name : "?");
}

static struct saved_place *saved_find(const char *key) {
	saved_load();
	for (int i = 0; i < saved->length; i++) {
		struct saved_place *place = saved->items[i];
		if (strcmp(place->key, key) == 0) {
			return place;
		}
	}
	return NULL;
}

static void saved_forget(const char *key) {
	saved_load();
	for (int i = 0; i < saved->length; i++) {
		struct saved_place *place = saved->items[i];
		if (strcmp(place->key, key) == 0) {
			free(place->key);
			free(place->config);
			free(place);
			list_del(saved, i);
			saved_write();
			return;
		}
	}
}

/* ---------- size and place ---------- */

static const char *conf_value(struct deskwidget *dw, const char *key) {
	const char *value = dw->conf ? twconf_value(dw->conf, key) : NULL;
	return value && *value ? value : NULL;
}

/* What the config says about the place, as one word: an older drag is kept while it holds. */
static char *config_signature(struct deskwidget *dw) {
	static const char *const keys[] = { "column", "row", "x", "y" };
	char *signature = strdup("");
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const char *value = conf_value(dw, keys[i]);
		char *next = format_str("%s%s%s", signature, i ? "," : "", value ? value : "");
		free(signature);
		signature = next;
	}
	for (char *c = signature; *c; c++) {
		if (*c == ' ' || *c == '\t') {
			*c = '_';
		}
	}
	return signature;
}

/*
 * The cell the config puts a widget of columns x rows cells in; false when it
 * gives none. "column" and "row" count cells, negative ones from the right
 * and the bottom; "x" and "y" are pixels, from before the widgets had cells.
 */
static bool config_cell(struct deskwidget *dw, const struct desk_grid *g, int columns,
		int rows, int *col, int *row) {
	const char *column = conf_value(dw, "column"), *line = conf_value(dw, "row");
	const char *x = conf_value(dw, "x"), *y = conf_value(dw, "y");
	if (!column && !line && !x && !y) {
		return false;
	}
	int c = -1, r = 0;
	if (column) {
		c = atoi(column);
	} else if (x) {
		int px = atoi(x);
		c = px >= 0 ? (int)lround((double)(px - g->margin) / g->cell_w) :
			-1 - (int)lround((double)(-px - g->margin) / g->cell_w);
	}
	if (line) {
		r = atoi(line);
	} else if (y) {
		int py = atoi(y);
		r = py >= 0 ? (int)lround((double)(py - g->margin) / g->cell_h) :
			-1 - (int)lround((double)(-py - g->margin) / g->cell_h);
	}
	*col = c >= 0 ? c : g->columns + c - columns + 1;
	*row = r >= 0 ? r : g->rows + r - rows + 1;
	return true;
}

static void render_ctx_init(struct render_ctx *ctx, struct desk_surface *ds, cairo_t *cr) {
	struct panel *panel = ds->dw->panel;
	double k = ds->k > 0 ? ds->k : 1;
	*ctx = (struct render_ctx){
		.panel = panel,
		.surface = ds->surface,
		.output = ds->output,
		.cairo = cr,
		.height = DESK_BASE,
		// the card has colors of its own, whatever the taskbar of the theme looks like
		.style = PSV_FLAT,
		.pointer_inside = ds->inside && !ds->dragging,
		.pressed = ds->pressed && !ds->dragging,
		.px = (ds->px - ds->ox) / k - ds->pad,
		.py = (ds->py - ds->oy) / k - ds->pad,
	};
}

/* The width the widget takes when laid out at scale k, 0 when it has nothing to show. */
static int measure(struct desk_surface *ds, cairo_t *cr, double k) {
	struct widget *w = ds->dw->widget;
	if (!w->impl->measure) {
		return 0;
	}
	struct render_ctx ctx;
	render_ctx_init(&ctx, ds, cr);
	// text is measured at the size it is drawn at: hinted bigger, it runs wider
	cairo_save(cr);
	cairo_identity_matrix(cr);
	double scale = k * (ds->output ? ds->output->scale : 1);
	cairo_scale(cr, scale, scale);
	ds->dw->panel->desktop_pass = true;
	ds->dw->panel->desktop_card = ds->dw->card;
	int width = w->impl->measure(w, &ctx);
	ds->dw->panel->desktop_pass = false;
	cairo_restore(cr);
	if (width < 0) {
		width = widget_conf_int(w, "width", 240); // one that fills the rest of a taskbar
	}
	return width > 0 ? width + 2 : 0;
}

/* How big the text of a card is: 1 for cells 100 pixels high. */
static double card_scale(const struct desk_grid *g) {
	return g->cell_h / 100.0;
}

static double caption_height(const struct desk_grid *g) {
	double sc = card_scale(g);
	return 12 * sc * 1.6 + 4 * sc;
}

/* How much bigger than on the taskbar a compact widget is drawn in rows of cells. */
static double compact_scale(const struct desk_grid *g, int rows) {
	double k = (rows * g->cell_h - 2.0 * DESK_INSET) / (DESK_BASE + 2 * DESK_PAD);
	return k < 0.5 ? 0.5 : k > 4 ? 4 : k;
}

/* The cells a widget takes, from its style, its "size" and what it measured. */
static void decide_size(struct desk_surface *ds, const struct desk_grid *g) {
	struct deskwidget *dw = ds->dw;
	int rows = dw->want_rows ? dw->want_rows : dw->style && dw->style->rows ?
		dw->style->rows : 1;
	int columns = dw->want_columns;
	if (!columns && compact(dw)) {
		double k = compact_scale(g, rows);
		double needed = (ds->content_width + 2 * DESK_PAD) * k + 2 * DESK_INSET;
		columns = (int)ceil(needed / g->cell_w);
	} else if (!columns) {
		columns = dw->style && dw->style->columns ? dw->style->columns : 2;
	}
	ds->columns = columns < 1 ? 1 : columns > g->columns ? g->columns : columns;
	ds->rows = rows < 1 ? 1 : rows > g->rows ? g->rows : rows;
}

static void card_size(struct desk_surface *ds, const struct desk_grid *g, int *w, int *h) {
	*w = ds->columns * g->cell_w - 2 * DESK_INSET;
	*h = ds->rows * g->cell_h - 2 * DESK_INSET;
	*w = *w < 8 ? 8 : *w;
	*h = *h < 8 ? 8 : *h;
}

/* Where the widget draws inside the card: scaled and centered, or 1:1 for a gadget. */
static void decide_transform(struct desk_surface *ds, const struct desk_grid *g) {
	struct deskwidget *dw = ds->dw;
	int sw, sh;
	card_size(ds, g, &sw, &sh);
	if (!compact(dw) && !tile(dw)) {
		ds->k = 1;
		ds->ox = ds->oy = ds->pad = 0;
		return;
	}
	double width = ds->content_width + 2 * DESK_PAD, height = DESK_BASE + 2 * DESK_PAD;
	double area_h = tile(dw) ? sh - caption_height(g) : sh;
	// a tile does not blow the widget up much more than a compact card of one row
	double k = compact(dw) ? compact_scale(g, ds->rows) :
		fmin(area_h / height, compact_scale(g, 1) * 1.25);
	if (width * k > sw) {
		k = sw / width;
	}
	ds->k = k > 0.2 ? k : 0.2;
	ds->pad = DESK_PAD;
	ds->ox = (sw - width * ds->k) / 2;
	ds->oy = (area_h - height * ds->k) / 2;
}

/* Puts the surface on its cells, counted from what the taskbar leaves free. */
static void apply_place(struct desk_surface *ds, const struct desk_grid *g) {
	struct zwlr_layer_surface_v1 *layer = ds->surface->layer_surface;
	zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(layer, g->margin + ds->row * g->cell_h + DESK_INSET, 0, 0,
		g->margin + ds->col * g->cell_w + DESK_INSET);
}

/* The top of what the taskbar leaves free: the grid counts from there. */
static int usable_top(struct desk_surface *ds) {
	struct panel *panel = ds->dw->panel;
	if (ds->output && ds->output->bar && panel->config &&
			!panel->config->layouts[panel->layout].bottom) {
		return ds->output->bar->height;
	}
	return 0;
}

/* Top left corner of the card on its output. */
static void surface_origin(struct desk_surface *ds, int *ox, int *oy) {
	struct desk_grid g;
	desktop_grid(ds->output, &g);
	*ox = g.margin + ds->col * g.cell_w + DESK_INSET;
	*oy = usable_top(ds) + g.margin + ds->row * g.cell_h + DESK_INSET;
}

/* ---------- drawing ---------- */

static void desk_render(struct psurface *s, cairo_t *cr) {
	struct desk_surface *ds = s->data;
	struct deskwidget *dw = ds->dw;
	struct widget *w = dw->widget;
	struct desk_grid g;
	desktop_grid(ds->output, &g);
	if (compact(dw) || tile(dw)) {
		int width = measure(ds, cr, ds->k > 0 ? ds->k : compact_scale(&g, ds->rows));
		if (width != ds->content_width) {
			// it grew or shrank: it may need other cells, and the others may move
			ds->content_width = width;
			layout_output(dw->panel, ds->output);
			if (s->req_width != s->width || s->req_height != s->height) {
				return; // drawn again once the new size is there
			}
		}
	}
	int sw, sh;
	card_size(ds, &g, &sw, &sh);
	if (sw != s->req_width || sh != s->req_height) {
		psurface_set_size(s, sw, sh);
		wl_surface_commit(s->surface);
		return;
	}
	struct gadget_ctx gadget = {
		.panel = dw->panel,
		.widget = w,
		.style = dw->style ? dw->style->name : "compact",
		.cairo = cr,
		.width = s->width,
		.height = s->height,
		.scale = card_scale(&g),
		.card = dw->card,
		.hover = ds->inside && !ds->dragging,
		.pressed = ds->pressed && !ds->dragging,
	};
	if (gadget_draw(&gadget)) {
		// the whole card is the widget: clicks and tooltips as on the taskbar
		psurface_add_hotspot(s, 0, 0, s->width, s->height, w, 0, 0, NULL);
		return;
	}
	struct gadget_palette pal;
	gadget_palette(dw->panel, w, dw->card, &pal);
	if (dw->card) {
		gadget.hover = false; // the widget shows its own hover
		gadget_draw_card(&gadget, &pal);
	}
	if (tile(dw)) {
		const struct tw_widget_info *info = tw_widget_find(w->name);
		const char *colon = strchr(w->name, ':');
		gadget_draw_caption(&gadget, &pal, info && strcmp(info->type, "custom") == 0 && colon ?
			colon + 1 : info ? info->title : w->name);
	}
	if (ds->content_width <= 0) {
		return;
	}
	struct render_ctx ctx;
	render_ctx_init(&ctx, ds, cr);
	cairo_save(cr);
	cairo_translate(cr, ds->ox, ds->oy);
	cairo_scale(cr, ds->k, ds->k);
	cairo_translate(cr, DESK_PAD, DESK_PAD);
	cairo_rectangle(cr, 0, 0, ds->content_width, DESK_BASE);
	cairo_clip(cr);
	dw->panel->desktop_pass = true;
	dw->panel->desktop_card = dw->card;
	if (w->impl->render) {
		w->impl->render(w, &ctx, (struct pbox){ 0, 0, ds->content_width, DESK_BASE });
	}
	dw->panel->desktop_pass = false;
	cairo_restore(cr);
}

/* ---------- pointer ---------- */

/* The pointer in the coordinates the widget drew in. */
static void widget_point(struct desk_surface *ds, double x, double y, double *wx, double *wy) {
	double k = ds->k > 0 ? ds->k : 1;
	*wx = (x - ds->ox) / k - ds->pad;
	*wy = (y - ds->oy) / k - ds->pad;
}

static struct hotspot *hotspot_at(struct desk_surface *ds, double x, double y) {
	double wx, wy;
	widget_point(ds, x, y, &wx, &wy);
	return psurface_hotspot_at(ds->surface, wx, wy);
}

static bool overlaps(struct desk_surface *a, int col, int row, struct desk_surface *b) {
	return col < b->col + b->columns && b->col < col + a->columns &&
		row < b->row + b->rows && b->row < row + a->rows;
}

/* Another widget on the same screen in the way of a at col and row. */
static bool cells_taken(struct desk_surface *a, int col, int row) {
	struct panel *panel = a->dw->panel;
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct desk_surface *b;
		wl_list_for_each(b, &dw->surfaces, link) {
			if (b != a && b->output == a->output && overlaps(a, col, row, b)) {
				return true;
			}
		}
	}
	return false;
}

static void desk_motion(struct psurface *s, double x, double y) {
	struct desk_surface *ds = s->data;
	if (ds->pressed && !ds->dragging &&
			hypot(x - ds->press_x, y - ds->press_y) >= DESK_DRAG) {
		// a press that moves becomes a drag; the widget goes along, cell by cell
		ds->dragging = true;
		tooltip_cancel(s->panel);
		struct desk_grid g;
		desktop_grid(ds->output, &g);
		ds->drag_x = g.margin + ds->col * g.cell_w;
		ds->drag_y = g.margin + ds->row * g.cell_h;
		ds->drag_col = ds->col;
		ds->drag_row = ds->row;
		ds->drag_placed = ds->placed;
		psurface_set_dirty(s);
	}
	if (ds->dragging) {
		// while the button is down the compositor goes on measuring the pointer from
		// where the surface was when it was pressed, however far it has moved since
		struct desk_grid g;
		desktop_grid(ds->output, &g);
		double left = ds->drag_x + (x - ds->press_x) - g.margin;
		double top = ds->drag_y + (y - ds->press_y) - g.margin;
		int col = (int)lround(left / g.cell_w), row = (int)lround(top / g.cell_h);
		int max_col = g.columns - ds->columns, max_row = g.rows - ds->rows;
		col = col > max_col ? max_col : col < 0 ? 0 : col;
		row = row > max_row ? max_row : row < 0 ? 0 : row;
		if (col != ds->col || row != ds->row) {
			ds->col = col;
			ds->row = row;
			ds->placed = true;
			apply_place(ds, &g);
			wl_surface_commit(s->surface);
		}
		return;
	}
	struct hotspot *before = ds->inside ? hotspot_at(ds, ds->px, ds->py) : NULL;
	bool entered = !ds->inside;
	ds->px = x;
	ds->py = y;
	ds->inside = true;
	struct hotspot *after = hotspot_at(ds, x, y);
	if (before != after || !after || entered) {
		// drawing again frees the hotspots, so the tooltip gets a copy
		struct hotspot hovered = after ? *after : (struct hotspot){ 0 };
		char *str = after && after->str ? strdup(after->str) : NULL;
		hovered.str = str;
		psurface_set_dirty(s);
		if (hovered.widget) {
			tooltip_schedule(s->panel, s, &hovered);
		} else {
			tooltip_cancel(s->panel);
		}
		free(str);
	}
}

static void desk_leave(struct psurface *s) {
	struct desk_surface *ds = s->data;
	ds->inside = false;
	if (!ds->dragging) {
		ds->pressed = false;
	}
	tooltip_cancel(s->panel);
	psurface_set_dirty(s);
}

static void end_drag(struct desk_surface *ds) {
	ds->dragging = false;
	ds->pressed = false;
	if (cells_taken(ds, ds->col, ds->row)) {
		// another widget sits there: back to where it came from
		ds->col = ds->drag_col;
		ds->row = ds->drag_row;
		ds->placed = ds->drag_placed;
	} else if (ds->col != ds->drag_col || ds->row != ds->drag_row) {
		char *key = place_key(ds);
		struct saved_place *place = saved_find(key);
		if (!place) {
			place = calloc(1, sizeof(*place));
			place->key = key;
			list_add(saved, place);
		} else {
			free(key);
			free(place->config);
		}
		place->col = ds->col;
		place->row = ds->row;
		place->config = config_signature(ds->dw);
		saved_write();
	}
	layout_output(ds->dw->panel, ds->output);
	desktop_widgets_moved(ds->dw->panel);
	psurface_set_dirty(ds->surface);
}

static void desk_menu(struct desk_surface *ds, double x, double y) {
	struct panel *panel = ds->dw->panel;
	struct widget *w = ds->dw->widget;
	struct popup_anchor anchor = popup_anchor_for_bar(ds->surface, (int)x, 0);
	if (w->menu) {
		menu_open(panel, w->menu, false, anchor, NULL);
		return;
	}
	list_t *items = create_list();
	char *cmd = format_str("panel desktop widget reset %s %s", w->name,
		ds->output && ds->output->name ? ds->output->name : "?");
	struct menu_item *reset = menu_item_new("Put back in its place", cmd);
	free(cmd);
	reset->disabled = !ds->placed;
	list_add(items, reset);
	list_add(items, menu_item_new("Desktop widget settings",
		"exec tilewin-settings --page desktop"));
	list_add(items, menu_item_separator());
	list_add(items, menu_item_new("Refresh", "panel desktop refresh"));
	menu_items_default_icons(items);
	menu_open(panel, items, true, anchor, NULL);
}

static void desk_button(struct psurface *s, double x, double y, uint32_t button, bool pressed) {
	struct desk_surface *ds = s->data;
	if (button == BTN_LEFT && pressed) {
		ds->pressed = true;
		ds->press_x = x;
		ds->press_y = y;
		tooltip_cancel(s->panel);
		psurface_set_dirty(s);
		return;
	}
	if (button == BTN_LEFT && ds->dragging) {
		end_drag(ds);
		return;
	}
	ds->pressed = false;
	psurface_set_dirty(s);
	if (pressed) {
		return;
	}
	double wx, wy;
	widget_point(ds, x, y, &wx, &wy);
	struct hotspot *hs = psurface_hotspot_at(s, wx, wy);
	if (hs && hs->widget && widget_handle_button(s->panel, s, hs, button, wx, wy)) {
		return;
	}
	if (button == BTN_RIGHT) {
		desk_menu(ds, x, y);
	}
}

static void desk_axis(struct psurface *s, double x, double y, int direction) {
	struct desk_surface *ds = s->data;
	struct hotspot *hs = hotspot_at(ds, x, y);
	if (hs && hs->widget) {
		widget_handle_scroll(s->panel, s, hs, direction);
	}
}

static void surface_destroy(struct desk_surface *ds) {
	wl_list_remove(&ds->link);
	if (ds->surface) {
		struct psurface *s = ds->surface;
		ds->surface = NULL;
		psurface_destroy(s);
	}
	free(ds);
}

static void desk_closed(struct psurface *s) {
	surface_destroy(s->data);
}

static const struct psurface_impl desk_impl = {
	.render = desk_render,
	.pointer_motion = desk_motion,
	.pointer_leave = desk_leave,
	.pointer_button = desk_button,
	.pointer_axis = desk_axis,
	.closed = desk_closed,
};

static bool is_desk_surface(struct psurface *s) {
	return s && s->impl == &desk_impl;
}

bool deskwidget_place(struct psurface *s, struct pbox box, struct pbox *out) {
	if (!is_desk_surface(s)) {
		return false;
	}
	struct desk_surface *ds = s->data;
	double k = ds->k > 0 ? ds->k : 1;
	int ox, oy;
	surface_origin(ds, &ox, &oy);
	*out = (struct pbox){
		ox + (int)(ds->ox + (box.x + ds->pad) * k), oy + (int)(ds->oy + (box.y + ds->pad) * k),
		(int)(box.width * k), (int)(box.height * k),
	};
	return true;
}

/* ---------- the grid ---------- */

/*
 * The free cells closest to col and row where on->items[i] fits: two widgets
 * never cover each other, whatever the config says. Only the widgets marked
 * done have their cells yet.
 */
static void free_cells_near(list_t *on, bool *done, int i, const struct desk_grid *g,
		int *col, int *row) {
	struct desk_surface *ds = on->items[i];
	int max_col = g->columns - ds->columns, max_row = g->rows - ds->rows;
	max_col = max_col < 0 ? 0 : max_col;
	max_row = max_row < 0 ? 0 : max_row;
	int want_col = *col > max_col ? max_col : *col < 0 ? 0 : *col;
	int want_row = *row > max_row ? max_row : *row < 0 ? 0 : *row;
	*col = want_col;
	*row = want_row;
	int best = -1;
	for (int c = 0; c <= max_col; c++) {
		for (int r = 0; r <= max_row; r++) {
			bool free_cells = true;
			for (int j = 0; j < on->length && free_cells; j++) {
				if (j != i && done[j] && overlaps(ds, c, r, on->items[j])) {
					free_cells = false;
				}
			}
			// closest first; on a tie the one nearer the right edge and the top
			int distance = (c - want_col) * (c - want_col) + (r - want_row) * (r - want_row);
			if (free_cells && (best < 0 || distance < best ||
					(distance == best && (c > *col || (c == *col && r < *row))))) {
				best = distance;
				*col = c;
				*row = r;
			}
		}
	}
}

/*
 * Gives every widget on the screen its cells: first the ones dragged somewhere
 * or put somewhere by the config, then the rest, as close to the upper right
 * corner as they fit. None covers another; the icons flow around all of them.
 */
static void layout_output(struct panel *panel, struct panel_output *output) {
	if (!panel->config || !panel->config->desk_widgets || !output) {
		return;
	}
	struct desk_grid g;
	desktop_grid(output, &g);
	list_t *on = create_list(); // struct desk_surface *, in the order of the config
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct desk_surface *ds;
		wl_list_for_each(ds, &dw->surfaces, link) {
			if (ds->output == output) {
				list_add(on, ds);
			}
		}
	}
	bool *done = calloc(on->length ? on->length : 1, sizeof(bool));
	// first the ones dragged somewhere or placed by the config, then the rest
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < on->length; i++) {
			struct desk_surface *ds = on->items[i];
			if (pass == 0) {
				decide_size(ds, &g);
			}
			int col = g.columns - ds->columns, row = 0;
			if (done[i] || (pass == 0 && !ds->placed &&
					!config_cell(ds->dw, &g, ds->columns, ds->rows, &col, &row))) {
				continue;
			}
			if (pass == 0 && ds->placed) {
				col = ds->col;
				row = ds->row;
			}
			free_cells_near(on, done, i, &g, &col, &row);
			ds->col = col;
			ds->row = row;
			done[i] = true;
		}
	}
	for (int i = 0; i < on->length; i++) {
		struct desk_surface *ds = on->items[i];
		decide_transform(ds, &g);
		int sw, sh;
		card_size(ds, &g, &sw, &sh);
		if (sw != ds->surface->req_width || sh != ds->surface->req_height) {
			psurface_set_size(ds->surface, sw, sh);
		}
		apply_place(ds, &g);
		wl_surface_commit(ds->surface->surface);
		psurface_set_dirty(ds->surface);
	}
	free(done);
	list_free(on);
}

bool deskwidgets_cover(struct panel_output *output, int col, int row) {
	struct panel *panel = output ? output->panel : NULL;
	if (!panel || !panel->config || !panel->config->desk_widgets) {
		return false;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct desk_surface *ds;
		wl_list_for_each(ds, &dw->surfaces, link) {
			if (ds->output == output && col >= ds->col && col < ds->col + ds->columns &&
					row >= ds->row && row < ds->row + ds->rows) {
				return true;
			}
		}
	}
	return false;
}

void deskwidgets_grid_changed(struct panel *panel) {
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		if (output->ready) {
			layout_output(panel, output);
		}
	}
}

/* ---------- surfaces on the screens ---------- */

static bool wanted_on(struct deskwidget *dw, struct panel_output *output) {
	const char *where = dw->conf ? twconf_value(dw->conf, "output") : NULL;
	if (where && strcasecmp(where, "all") == 0) {
		return true;
	}
	if (where && strcasecmp(where, "main") != 0) {
		return output->name && strcmp(output->name, where) == 0;
	}
	const char *main_output = dw->panel->state.main_output;
	if (main_output) {
		return output->name && strcmp(output->name, main_output) == 0;
	}
	// no main display known yet: the first screen
	struct panel_output *first;
	wl_list_for_each(first, &dw->panel->outputs, link) {
		if (first->ready) {
			return first == output;
		}
	}
	return false;
}

static void surface_create(struct deskwidget *dw, struct panel_output *output) {
	struct panel *panel = dw->panel;
	struct desk_surface *ds = calloc(1, sizeof(*ds));
	ds->dw = dw;
	ds->output = output;
	wl_list_insert(&dw->surfaces, &ds->link);
	struct psurface *s = psurface_create(panel, output, &desk_impl, ds,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "tilewin-desktop-widget");
	ds->surface = s;
	char *key = place_key(ds);
	struct saved_place *place = saved_find(key);
	char *signature = config_signature(dw);
	if (place && strcmp(place->config, signature) == 0) {
		ds->placed = true;
		ds->col = place->col;
		ds->row = place->row;
	} else if (place) {
		saved_forget(key); // the config moved it since: the config wins
	}
	free(signature);
	free(key);
	if (compact(dw) || tile(dw)) {
		struct desk_grid g;
		desktop_grid(output, &g);
		ds->content_width = measure(ds, popup_scratch_cairo(), compact_scale(&g, 1));
	}
	// layout_output gives it its cells and its size, and commits it
}

static struct desk_surface *surface_on(struct deskwidget *dw, struct panel_output *output) {
	struct desk_surface *ds;
	wl_list_for_each(ds, &dw->surfaces, link) {
		if (ds->output == output) {
			return ds;
		}
	}
	return NULL;
}

void deskwidgets_update(struct panel *panel) {
	if (!panel->config || !panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct panel_output *output;
		wl_list_for_each(output, &panel->outputs, link) {
			struct desk_surface *ds = surface_on(dw, output);
			bool wanted = output->ready && wanted_on(dw, output);
			if (wanted && !ds) {
				surface_create(dw, output);
			} else if (!wanted && ds) {
				surface_destroy(ds);
			}
		}
	}
	// the screens or the taskbar may have changed size: every widget finds its cells again
	deskwidgets_grid_changed(panel);
	desktop_widgets_moved(panel);
}

void deskwidgets_output_gone(struct panel_output *output) {
	struct panel *panel = output->panel;
	if (!panel->config || !panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct desk_surface *ds = surface_on(panel->config->desk_widgets->items[i], output);
		if (ds) {
			surface_destroy(ds);
		}
	}
}

void deskwidgets_set_dirty(struct panel *panel) {
	if (!panel->config || !panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct desk_surface *ds;
		wl_list_for_each(ds, &dw->surfaces, link) {
			psurface_set_dirty(ds->surface);
		}
	}
}

void deskwidgets_state_changed(struct panel *panel) {
	if (!panel->config || !panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		if (dw->widget->impl->state_changed) {
			dw->widget->impl->state_changed(dw->widget);
		}
	}
}

void deskwidgets_widget_dirty(struct widget *w) {
	struct panel *panel = w->panel;
	if (!panel->config || !panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		if (dw->widget != w) {
			continue;
		}
		struct desk_surface *ds;
		wl_list_for_each(ds, &dw->surfaces, link) {
			psurface_set_dirty(ds->surface);
		}
	}
}

/* "panel desktop widget reset <widget> <output>": back to where the config puts it. */
void deskwidgets_handle_command(struct panel *panel, int argc, char **argv) {
	if (argc < 3 || strcmp(argv[0], "reset") != 0 || !panel->config ||
			!panel->config->desk_widgets) {
		return;
	}
	for (int i = 0; i < panel->config->desk_widgets->length; i++) {
		struct deskwidget *dw = panel->config->desk_widgets->items[i];
		struct desk_surface *ds;
		wl_list_for_each(ds, &dw->surfaces, link) {
			if (strcmp(dw->widget->name, argv[1]) == 0 && ds->output && ds->output->name &&
					strcmp(ds->output->name, argv[2]) == 0) {
				char *key = place_key(ds);
				saved_forget(key);
				free(key);
				ds->placed = false;
				layout_output(panel, ds->output);
				desktop_widgets_moved(panel);
				return;
			}
		}
	}
}

/* ---------- the widgets of the config ---------- */

void deskwidgets_load(struct panel *panel, struct panel_config *config) {
	struct twconf_node *block = twconf_child(config->root, "desktop_widgets");
	config->desk_widgets = create_list();
	for (int i = 0; block && i < twconf_count(block); i++) {
		struct twconf_node *entry = twconf_at(block, i);
		const struct tw_widget_info *info = tw_widget_find(entry->name);
		if (!info || !(info->flags & TW_WIDGET_DESKTOP)) {
			sway_log(SWAY_ERROR, "'%s' cannot go on the desktop", entry->name);
			continue;
		}
		// set up the way the taskbar sets it up, with the desktop's own words on top
		struct twconf_node *def = NULL;
		for (int j = 0; j < twconf_count(config->root); j++) {
			struct twconf_node *child = twconf_at(config->root, j);
			if (strcmp(child->name, "widget") == 0 && child->argc >= 1 &&
					strcmp(child->argv[0], entry->name) == 0) {
				def = child;
			}
		}
		struct widget *w = widget_create_with(panel, entry->name, def, entry);
		if (!w) {
			sway_log(SWAY_ERROR, "Unknown desktop widget '%s'", entry->name);
			continue;
		}
		struct deskwidget *dw = calloc(1, sizeof(*dw));
		dw->panel = panel;
		dw->widget = w;
		dw->conf = entry;
		dw->style = tw_widget_style_find(info, twconf_value(entry, "style"));
		const char *size = twconf_value(entry, "size");
		if (size && sscanf(size, "%dx%d", &dw->want_columns, &dw->want_rows) != 2) {
			sway_log(SWAY_ERROR, "Desktop widget '%s': size is columns x rows, e.g. 3x2",
				entry->name);
			dw->want_columns = dw->want_rows = 0;
		}
		dw->card = twconf_parse_bool(twconf_value(entry, "background"), true);
		wl_list_init(&dw->surfaces);
		// always on show, unlike taskbar widgets that wait for their layout
		w->active = true;
		if (w->impl->set_active) {
			w->impl->set_active(w, true);
		}
		list_add(config->desk_widgets, dw);
	}
}

void deskwidgets_free(struct panel_config *config) {
	if (!config->desk_widgets) {
		return;
	}
	for (int i = 0; i < config->desk_widgets->length; i++) {
		struct deskwidget *dw = config->desk_widgets->items[i];
		struct desk_surface *ds, *tmp;
		wl_list_for_each_safe(ds, tmp, &dw->surfaces, link) {
			surface_destroy(ds);
		}
		widget_destroy(dw->widget);
		free(dw);
	}
	list_free(config->desk_widgets);
	config->desk_widgets = NULL;
}
