#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "draw.h"
#include "panel.h"
#include "twconf.h"

/*
 * Minimized windows as icons on the desktop, as Windows 3 kept them: a row
 * along the bottom of the screen the window is on, each its app's icon with
 * the title under it. A double click restores the window, the right button
 * opens its menu. The theme turns it on ("desktop { minimized_icons yes }",
 * as Windows 3 does), "minimized_icons yes|no" in taskbar.conf decides
 * whatever the theme says.
 */

#define CELL_W 84
#define CELL_H 74
#define ICON 32
#define STRIP_H (CELL_H + 8)
#define DOUBLE_CLICK_MS 400

enum { MINI_HS_WINDOW = 1 };

struct strip {
	struct panel_output *output;
	struct psurface *surface;
	int64_t selected;
	int64_t last_click_id;
	int64_t last_click_ms;
};

static list_t *strips; // struct strip *

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool enabled(struct panel *panel) {
	const char *set = panel->config ? twconf_value(panel->config->root, "minimized_icons") :
		NULL;
	if (set && strcmp(set, "theme") != 0) {
		return twconf_parse_bool(set, false);
	}
	return tw_theme_bool(panel->theme, "desktop.minimized_icons", false);
}

/* The minimized windows of a screen, in the order they came. */
static list_t *windows_of(struct panel *panel, struct panel_output *output) {
	list_t *out = create_list();
	for (int i = 0; panel->state.windows && i < panel->state.windows->length; i++) {
		struct pwindow *w = panel->state.windows->items[i];
		if (w->minimized && w->output && output->name &&
				strcmp(w->output, output->name) == 0) {
			list_add(out, w);
		}
	}
	return out;
}

static void draw_title(cairo_t *cr, const char *font, const char *text, double x, double y,
		double width, bool selected, uint32_t hl_bg, uint32_t hl_fg) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_height(layout, -2);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_text(layout, text ? text : "", -1);
	if (selected) {
		// the title of the selected icon on the highlight, as Windows 3 had it
		PangoRectangle ink, logical;
		pango_layout_get_pixel_extents(layout, &ink, &logical);
		pd_rect(cr, x + logical.x - 2, y, logical.width + 4, logical.height, hl_bg);
		cairo_move_to(cr, x, y);
		pd_color(cr, hl_fg);
		pango_cairo_show_layout(cr, layout);
	} else {
		// white with a shadow stays readable on any wallpaper
		cairo_move_to(cr, x + 1, y + 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
		pango_cairo_show_layout(cr, layout);
		cairo_move_to(cr, x, y);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
}

static void strip_render(struct psurface *s, cairo_t *cr) {
	struct strip *strip = s->data;
	struct panel *panel = s->panel;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	list_t *windows = windows_of(panel, strip->output);
	uint32_t hl_bg = tw_theme_color(panel->theme, "menu.hl_bg", 0x000080ff);
	uint32_t hl_fg = tw_theme_color(panel->theme, "menu.hl_fg", 0xffffffff);
	for (int i = 0; i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		double x = 4 + i * CELL_W, y = 4;
		cairo_surface_t *icon = apps_icon_for_window(panel, w, ICON * s->scale);
		pd_icon(cr, icon, x + (CELL_W - ICON) / 2.0, y + 2, ICON);
		draw_title(cr, bar_font(panel), w->title && *w->title ? w->title :
			apps_display_name(w->app_id), x + 2, y + ICON + 6, CELL_W - 4,
			w->id == strip->selected, hl_bg, hl_fg);
		psurface_add_hotspot(s, x, y, CELL_W, CELL_H, NULL, MINI_HS_WINDOW, w->id, NULL);
	}
	list_free(windows);
}

static void strip_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	struct strip *strip = s->data;
	struct panel *panel = s->panel;
	if (!pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(s, x, y);
	struct pwindow *w = hs && hs->kind == MINI_HS_WINDOW ? panel_find_window(panel, hs->id) :
		NULL;
	if (!w) {
		strip->selected = 0;
		psurface_set_dirty(s);
		return;
	}
	if (button == BTN_RIGHT) {
		struct popup_anchor anchor = {
			.output = strip->output,
			.x = (int)x,
			.y = strip->output->height - s->height + (int)y,
			.above = true,
		};
		strip->selected = w->id;
		psurface_set_dirty(s);
		menu_open(panel, taskbar_window_menu(panel, w), true, anchor, NULL);
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	int64_t now = now_ms();
	int64_t double_click = panel->state.double_click_ms > 0 ? panel->state.double_click_ms :
		DOUBLE_CLICK_MS;
	if (strip->last_click_id == w->id && now - strip->last_click_ms < double_click) {
		strip->last_click_id = 0;
		ipc_panel_commandf(panel, "[con_id=%lld] focus", (long long)w->id);
		return;
	}
	strip->last_click_id = w->id;
	strip->last_click_ms = now;
	strip->selected = w->id;
	psurface_set_dirty(s);
}

static const struct psurface_impl strip_impl = {
	.render = strip_render,
	.pointer_button = strip_button,
};

static struct strip *strip_of(struct panel_output *output) {
	for (int i = 0; strips && i < strips->length; i++) {
		struct strip *strip = strips->items[i];
		if (strip->output == output) {
			return strip;
		}
	}
	return NULL;
}

static void strip_destroy(struct strip *strip) {
	if (strip->surface) {
		psurface_destroy(strip->surface);
	}
	list_del(strips, list_find(strips, strip));
	free(strip);
}

void minicons_update(struct panel *panel) {
	if (!panel->theme || !panel->outputs.next) {
		return; // the first window list comes before the screens are known
	}
	if (!strips) {
		strips = create_list();
	}
	bool on = enabled(panel);
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		if (!output->ready) {
			continue;
		}
		list_t *windows = windows_of(panel, output);
		int count = windows->length;
		list_free(windows);
		struct strip *strip = strip_of(output);
		if (!on || count == 0) {
			if (strip) {
				strip_destroy(strip);
			}
			continue;
		}
		if (!strip) {
			strip = calloc(1, sizeof(*strip));
			if (!strip) {
				continue;
			}
			strip->output = output;
			struct psurface *s = psurface_create(panel, output, &strip_impl, strip,
				ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "tilewin-minimized-icons");
			// only as wide as its icons: the desktop beside them takes clicks as before
			zwlr_layer_surface_v1_set_anchor(s->layer_surface,
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
			strip->surface = s;
			list_add(strips, strip);
		}
		int width = 8 + count * CELL_W;
		if (strip->surface->req_height != STRIP_H || strip->surface->req_width != width) {
			psurface_set_size(strip->surface, width, STRIP_H);
			wl_surface_commit(strip->surface->surface);
		}
		psurface_set_dirty(strip->surface);
	}
}

void minicons_output_removed(struct panel_output *output) {
	struct strip *strip = strip_of(output);
	if (strip) {
		strip_destroy(strip);
	}
}
