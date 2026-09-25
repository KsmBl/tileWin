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
 *       clock { x 40; y 40; scale 2.5 }
 *       cpu { x -40; y 40; style graph; output all }
 *   }
 *
 * and each of them is the same widget the taskbar shows: the same code draws
 * it, the "widget <name>" block of taskbar.conf sets it up for both places,
 * and whatever its entry here says on top of that (a style, a format) is only
 * for the desktop. A desktop widget is laid out the way it would be on a
 * taskbar DESK_BASE pixels high and then drawn bigger, "scale" times, on a
 * surface of its own over the wallpaper and under the windows, with a
 * translucent card behind it.
 *
 * Dragging a widget moves it. Where it was put is kept in
 * ~/.local/state/tileWin/desktop-widgets, together with the place the config
 * gave it then, so changing the place in the config (or in the settings) still
 * wins over an older drag.
 */

#define DESK_BASE 40      // taskbar height the widget is laid out for
#define DESK_PAD 6        // around the widget, inside the card, before scaling
#define DESK_DRAG 6       // pixels the pointer moves before a press becomes a drag
#define DESK_SPACING 16   // between widgets placed one below the other by default

struct desk_surface {
	struct deskwidget *dw;
	struct panel_output *output;
	struct psurface *surface;
	int x, y;            // top left corner on the output, when dragged or placed
	bool placed;         // x and y hold where it was dragged to
	double px, py;       // pointer, surface coordinates
	bool inside, pressed, dragging;
	double press_x, press_y;
	int drag_x, drag_y; // where it was when the drag began
	struct wl_list link; // deskwidget::surfaces
};

struct deskwidget {
	struct panel *panel;
	struct widget *widget;
	struct twconf_node *conf; // its entry in desktop_widgets
	int index;                // place in the block, for the default position
	double scale;
	bool card;
	struct wl_list surfaces;  // desk_surface::link
};

static bool is_desk_surface(struct psurface *s);

/* ---------- where widgets were dragged to ---------- */

struct saved_place {
	char *key; // "<widget> <output>"
	int x, y;  // where it was put
	int config_x, config_y; // what the config said when it was put there
};

static list_t *saved; // struct saved_place *

static char *saved_path(void) {
	char *dir = tw_state_dir();
	char *path = dir ? format_str("%s/desktop-widgets", dir) : NULL;
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
		int offset = 0;
		if (sscanf(line, "%d %d %d %d %n", &place.x, &place.y, &place.config_x,
				&place.config_y, &offset) >= 4 && offset > 0 && line[offset]) {
			struct saved_place *copy = malloc(sizeof(*copy));
			*copy = place;
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
		char *next = format_str("%s%d %d %d %d %s\n", content, place->x, place->y,
			place->config_x, place->config_y, place->key);
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
			free(place);
			list_del(saved, i);
			saved_write();
			return;
		}
	}
}

/* ---------- size and place ---------- */

static int config_int(struct deskwidget *dw, const char *key, int fallback) {
	const char *value = dw->conf ? twconf_value(dw->conf, key) : NULL;
	return value && *value ? atoi(value) : fallback;
}

/* The place the config gives it; unset, the widgets line up along the top right. */
static void config_place(struct deskwidget *dw, int *x, int *y) {
	*x = config_int(dw, "x", -24);
	*y = config_int(dw, "y", 24 + dw->index * (int)(DESK_BASE * dw->scale + DESK_SPACING));
}

static void render_ctx_init(struct render_ctx *ctx, struct desk_surface *ds, cairo_t *cr) {
	struct panel *panel = ds->dw->panel;
	double k = ds->dw->scale;
	*ctx = (struct render_ctx){
		.panel = panel,
		.surface = ds->surface,
		.output = ds->output,
		.cairo = cr,
		.height = DESK_BASE,
		// white text on a dark card, whatever the taskbar of the theme looks like
		.style = PSV_FLAT,
		.pointer_inside = ds->inside && !ds->dragging,
		.pressed = ds->pressed && !ds->dragging,
		.px = ds->px / k - DESK_PAD,
		.py = ds->py / k - DESK_PAD,
	};
}

/* The width the widget takes when laid out, 0 when it has nothing to show. */
static int measure(struct desk_surface *ds, cairo_t *cr) {
	struct widget *w = ds->dw->widget;
	if (!w->impl->measure) {
		return 0;
	}
	struct render_ctx ctx;
	render_ctx_init(&ctx, ds, cr);
	// text is measured at the size it is drawn at: hinted bigger, it runs wider
	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_scale(cr, ds->dw->scale * (ds->output ? ds->output->scale : 1),
		ds->dw->scale * (ds->output ? ds->output->scale : 1));
	ds->dw->panel->desktop_pass = true;
	int width = w->impl->measure(w, &ctx);
	ds->dw->panel->desktop_pass = false;
	cairo_restore(cr);
	if (width < 0) {
		width = widget_conf_int(w, "width", 240); // one that fills the rest of a taskbar
	}
	return width > 0 ? width + 2 : 0;
}

static void surface_size(struct desk_surface *ds, int width, int *sw, int *sh) {
	double k = ds->dw->scale;
	*sw = width > 0 ? (int)ceil((width + 2 * DESK_PAD) * k) : 1;
	*sh = width > 0 ? (int)ceil((DESK_BASE + 2 * DESK_PAD) * k) : 1;
}

/* Puts the surface where it belongs: dragged there, or where the config says. */
static void apply_place(struct desk_surface *ds) {
	struct zwlr_layer_surface_v1 *layer = ds->surface->layer_surface;
	if (ds->placed) {
		zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
		zwlr_layer_surface_v1_set_margin(layer, ds->y, 0, 0, ds->x);
		return;
	}
	int x, y;
	config_place(ds->dw, &x, &y);
	uint32_t anchor = (x < 0 ? ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) |
		(y < 0 ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
	zwlr_layer_surface_v1_set_anchor(layer, anchor);
	zwlr_layer_surface_v1_set_margin(layer, y < 0 ? 0 : y, x < 0 ? -x : 0, y < 0 ? -y : 0,
		x < 0 ? 0 : x);
}

/* The top and bottom of what the taskbar leaves free: margins count from there. */
static void usable_edges(struct desk_surface *ds, int *top, int *bottom) {
	struct panel *panel = ds->dw->panel;
	*top = 0;
	*bottom = ds->output ? ds->output->height : 0;
	if (ds->output && ds->output->bar && panel->config) {
		if (panel->config->layouts[panel->layout].bottom) {
			*bottom -= ds->output->bar->height;
		} else {
			*top += ds->output->bar->height;
		}
	}
}

/* Top left corner of the surface on its output. */
static void surface_origin(struct desk_surface *ds, int *ox, int *oy) {
	int top, bottom;
	usable_edges(ds, &top, &bottom);
	if (ds->placed) {
		*ox = ds->x;
		*oy = top + ds->y;
		return;
	}
	int x, y;
	config_place(ds->dw, &x, &y);
	int width = ds->output ? ds->output->width : 0;
	*ox = x < 0 ? width + x - ds->surface->width : x;
	*oy = y < 0 ? bottom + y - ds->surface->height : top + y;
}

/* ---------- drawing ---------- */

static void draw_card(struct desk_surface *ds, cairo_t *cr) {
	const struct tw_theme *t = ds->dw->panel->theme;
	double k = ds->dw->scale;
	double w = ds->surface->width, h = ds->surface->height;
	double r = tw_theme_int(t, "desktop_widget.radius", 8) * k;
	cairo_new_path(cr);
	pd_rounded(cr, 0.5, 0.5, w - 1, h - 1, fmin(r, h / 2));
	pd_color(cr, tw_theme_color(t, "desktop_widget.bg", 0x0000006b));
	cairo_fill_preserve(cr);
	pd_color(cr, tw_theme_color(t, "desktop_widget.border", 0xffffff26));
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void desk_render(struct psurface *s, cairo_t *cr) {
	struct desk_surface *ds = s->data;
	struct deskwidget *dw = ds->dw;
	struct widget *w = dw->widget;
	int width = measure(ds, cr);
	int sw, sh;
	surface_size(ds, width, &sw, &sh);
	if (sw != s->req_width || sh != s->req_height) {
		// the widget grew or shrank: draw again once the new size is there
		psurface_set_size(s, sw, sh);
		wl_surface_commit(s->surface);
		return;
	}
	if (width <= 0) {
		return;
	}
	if (dw->card) {
		draw_card(ds, cr);
	}
	double k = dw->scale;
	struct render_ctx ctx;
	render_ctx_init(&ctx, ds, cr);
	cairo_save(cr);
	cairo_scale(cr, k, k);
	cairo_translate(cr, DESK_PAD, DESK_PAD);
	cairo_rectangle(cr, 0, 0, width, DESK_BASE);
	cairo_clip(cr);
	dw->panel->desktop_pass = true;
	if (w->impl->render) {
		w->impl->render(w, &ctx, (struct pbox){ 0, 0, width, DESK_BASE });
	}
	dw->panel->desktop_pass = false;
	cairo_restore(cr);
}

/* ---------- pointer ---------- */

/* The pointer in the coordinates the widget drew in. */
static void widget_point(struct desk_surface *ds, double x, double y, double *wx, double *wy) {
	*wx = x / ds->dw->scale - DESK_PAD;
	*wy = y / ds->dw->scale - DESK_PAD;
}

static struct hotspot *hotspot_at(struct desk_surface *ds, double x, double y) {
	double wx, wy;
	widget_point(ds, x, y, &wx, &wy);
	return psurface_hotspot_at(ds->surface, wx, wy);
}

static void desk_motion(struct psurface *s, double x, double y) {
	struct desk_surface *ds = s->data;
	if (ds->pressed && !ds->dragging &&
			hypot(x - ds->press_x, y - ds->press_y) >= DESK_DRAG) {
		// a press that moves becomes a drag; the widget goes along
		ds->dragging = true;
		tooltip_cancel(s->panel);
		int ox, oy, top, bottom;
		surface_origin(ds, &ox, &oy);
		usable_edges(ds, &top, &bottom);
		ds->x = ds->drag_x = ox;
		ds->y = ds->drag_y = oy - top;
		ds->placed = true;
	}
	if (ds->dragging) {
		// while the button is down the compositor goes on measuring the pointer from
		// where the surface was when it was pressed, however far it has moved since
		ds->x = ds->drag_x + (int)(x - ds->press_x);
		ds->y = ds->drag_y + (int)(y - ds->press_y);
		int max_x = ds->output ? ds->output->width - 16 : ds->x;
		int max_y = ds->output ? ds->output->height - 16 : ds->y;
		ds->x = ds->x < 16 - s->width ? 16 - s->width : ds->x > max_x ? max_x : ds->x;
		ds->y = ds->y < 0 ? 0 : ds->y > max_y ? max_y : ds->y;
		apply_place(ds);
		wl_surface_commit(s->surface);
		return;
	}
	struct hotspot *before = ds->inside ? hotspot_at(ds, ds->px, ds->py) : NULL;
	ds->px = x;
	ds->py = y;
	ds->inside = true;
	struct hotspot *after = hotspot_at(ds, x, y);
	if (before != after || !after) {
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
	char *key = place_key(ds);
	struct saved_place *place = saved_find(key);
	if (!place) {
		place = calloc(1, sizeof(*place));
		place->key = key;
		list_add(saved, place);
	} else {
		free(key);
	}
	place->x = ds->x;
	place->y = ds->y;
	config_place(ds->dw, &place->config_x, &place->config_y);
	saved_write();
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
		"exec tilewin-settings --page taskbar"));
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
		desk_menu(ds, wx, wy);
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
	double k = ds->dw->scale;
	int ox, oy;
	surface_origin(ds, &ox, &oy);
	*out = (struct pbox){
		ox + (int)((box.x + DESK_PAD) * k), oy + (int)((box.y + DESK_PAD) * k),
		(int)(box.width * k), (int)(box.height * k),
	};
	return true;
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
	int config_x, config_y;
	config_place(dw, &config_x, &config_y);
	if (place && place->config_x == config_x && place->config_y == config_y) {
		ds->placed = true;
		ds->x = place->x;
		ds->y = place->y;
	} else if (place) {
		saved_forget(key); // the config moved it since: the config wins
	}
	free(key);
	int sw, sh;
	surface_size(ds, measure(ds, popup_scratch_cairo()), &sw, &sh);
	psurface_set_size(s, sw, sh);
	apply_place(ds);
	wl_surface_commit(s->surface);
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
				apply_place(ds);
				wl_surface_commit(ds->surface->surface);
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
		dw->index = config->desk_widgets->length;
		const char *scale = twconf_value(entry, "scale");
		dw->scale = scale ? strtod(scale, NULL) : 2;
		if (!(dw->scale >= 0.5 && dw->scale <= 8)) {
			dw->scale = 2;
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
