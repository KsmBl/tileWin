#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"

/*
 * Workspaces: the virtual desktops of the screen side by side as small
 * screens, as KDE Plasma's pager shows them, each with its windows where they
 * are, as outlines with their app icons. The desktop shown is outlined in the
 * color of the taskbar's indicator. A click goes to a desktop, the wheel walks
 * through them, the right button moves, makes and closes desktops, and resting
 * on one shows a preview of it with its windows as they look (thumbnails.c).
 *
 *   widget workspaces { labels yes; icons yes }
 *
 * The theme's "workspaces" block colors them: fg, active_bg (the desktop
 * shown), urgent_bg, radius (rounded screens) and margin (around them all).
 * Windows only tell where they are when the window tree is read, so while the
 * widget is on the taskbar the tree is read again every second.
 */

#define REFRESH_MS 1000
#define GAP 4

static struct loop_timer *refresh_timer;
static int pagers; // widgets there are, which want the tree read again

static void refresh_fired(void *data) {
	struct panel *panel = data;
	refresh_timer = NULL;
	if (pagers <= 0) {
		return;
	}
	if (!panel->state.battery_saver) {
		ipc_panel_refresh_tree(panel);
	}
	refresh_timer = loop_add_timer(panel->loop, REFRESH_MS, refresh_fired, panel);
}

static void workspaces_init(struct widget *w) {
	if (pagers++ == 0 && !refresh_timer) {
		refresh_timer = loop_add_timer(w->panel->loop, REFRESH_MS, refresh_fired, w->panel);
	}
}

static void workspaces_destroy(struct widget *w) {
	if (--pagers <= 0) {
		pagers = 0;
		if (refresh_timer) {
			loop_remove_timer(w->panel->loop, refresh_timer);
			refresh_timer = NULL;
		}
	}
}

/* The size of one small screen: as high as the taskbar allows, as wide as the screen is. */
static void desk_size(struct render_ctx *ctx, int *w, int *h) {
	*h = ctx->height - 8;
	*h = *h < 12 ? 12 : *h;
	double aspect = ctx->output->height > 0 ?
		(double)ctx->output->width / ctx->output->height : 16.0 / 9;
	*w = (int)(*h * aspect + 0.5);
}

static int desks_on(struct render_ctx *ctx) {
	int n = 0;
	list_t *wss = ctx->panel->state.workspaces;
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		n += ctx->output->name && strcmp(ws->output, ctx->output->name) == 0;
	}
	return n;
}

static int workspaces_measure(struct widget *w, struct render_ctx *ctx) {
	int n = desks_on(ctx);
	if (n == 0) {
		return 0;
	}
	int dw, dh;
	desk_size(ctx, &dw, &dh);
	int margin = tw_theme_int(ctx->panel->theme, "workspaces.margin", 0);
	return n * dw + (n + 1) * GAP + 2 * margin;
}

static const char *desk_label(struct pworkspace *ws) {
	const char *colon = strchr(ws->name, ':');
	return colon && colon[1] ? colon + 1 : ws->name;
}

static void workspaces_render(struct widget *w, struct render_ctx *ctx, struct pbox box) {
	cairo_t *cr = ctx->cairo;
	struct panel *panel = ctx->panel;
	const struct tw_theme *t = panel->theme;
	uint32_t fg = widget_fg(panel, "workspaces");
	uint32_t accent = tw_theme_color(t, "taskbar.indicator", 0x76b9edff);
	bool labels = widget_conf_bool(w, "labels", true);
	bool icons = widget_conf_bool(w, "icons", true);
	int dw, dh;
	desk_size(ctx, &dw, &dh);
	double scale = (double)dw / (ctx->output->width > 0 ? ctx->output->width : 1);
	int x = box.x + GAP + tw_theme_int(t, "workspaces.margin", 0);
	int y = box.y + (box.height - dh) / 2;
	double radius = tw_theme_double(t, "workspaces.radius", 0);
	radius = radius > dh / 3.0 ? dh / 3.0 : radius;
	list_t *wss = panel->state.workspaces;
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		if (!ctx->output->name || strcmp(ws->output, ctx->output->name) != 0) {
			continue;
		}
		struct pbox d = { x, y, dw, dh };
		bool hover = render_hover(ctx, d);
		// the screen, lighter when the pointer is on it
		cairo_new_path(cr);
		pd_rounded(cr, d.x, d.y, d.width, d.height, radius);
		pd_color(cr, ws->visible ? tw_theme_color(t, "workspaces.active_bg",
			(fg & 0xffffff00) | (hover ? 0x30 : 0x18)) : (fg & 0xffffff00) | (hover ? 0x30 : 0x18));
		cairo_fill(cr);
		if (labels) {
			pd_text(cr, bar_font(panel), desk_label(ws), d.x, d.y, d.width, d.height,
				(fg & 0xffffff00) | 0x70, PD_CENTER);
		}
		// its windows, the ones drawn last on top as they are stacked
		cairo_save(cr);
		cairo_new_path(cr);
		pd_rounded(cr, d.x, d.y, d.width, d.height, radius);
		cairo_clip(cr);
		list_t *windows = panel->state.windows;
		for (int j = 0; windows && j < windows->length; j++) {
			struct pwindow *win = windows->items[j];
			if (win->minimized || !win->workspace || strcmp(win->workspace, ws->name) != 0 ||
					win->width <= 0 || win->height <= 0) {
				continue;
			}
			double wx = d.x + win->x * scale, wy = d.y + win->y * scale;
			double ww = win->width * scale, wh = win->height * scale;
			ww = ww < 3 ? 3 : ww;
			wh = wh < 3 ? 3 : wh;
			pd_rect(cr, wx, wy, ww, wh, (fg & 0xffffff00) | (win->focused ? 0x70 : 0x48));
			cairo_rectangle(cr, wx + 0.5, wy + 0.5, ww - 1, wh - 1);
			pd_color(cr, (fg & 0xffffff00) | 0xc0);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			double is = wh - 4 < ww - 4 ? wh - 4 : ww - 4;
			is = is > 16 ? 16 : is;
			if (icons && is >= 8) {
				cairo_surface_t *icon = apps_icon_for_window(panel, win,
					(int)(is * ctx->surface->scale));
				pd_icon(cr, icon, wx + (ww - is) / 2, wy + (wh - is) / 2, is);
			}
		}
		cairo_restore(cr);
		// the desktop shown: outlined in the indicator's color
		cairo_new_path(cr);
		pd_rounded(cr, d.x + 0.5, d.y + 0.5, d.width - 1, d.height - 1, radius);
		pd_color(cr, ws->visible ? accent : (fg & 0xffffff00) | 0x60);
		cairo_set_line_width(cr, ws->visible ? 2 : 1);
		cairo_stroke(cr);
		if (ws->urgent) {
			cairo_new_path(cr);
			pd_rounded(cr, d.x, d.y, d.width, d.height, radius);
			pd_color(cr, tw_theme_color(t, "workspaces.urgent_bg", 0xe8112360));
			cairo_fill(cr);
		}
		psurface_add_hotspot(ctx->surface, d.x, d.y, d.width, d.height, w, 0, i, ws->name);
		x += dw + GAP;
	}
}

static bool workspaces_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (!hs->str) {
		return false;
	}
	if (button == BTN_RIGHT) {
		// the entries act on the desktop that was clicked, not the current one
		list_t *items = create_list();
		char *cmd = format_str("workspace \"%s\", desktop move left", hs->str);
		list_add(items, menu_item_new("Move desktop left", cmd));
		free(cmd);
		cmd = format_str("workspace \"%s\", desktop move right", hs->str);
		list_add(items, menu_item_new("Move desktop right", cmd));
		free(cmd);
		list_add(items, menu_item_separator());
		list_add(items, menu_item_new("New desktop", "desktop new"));
		cmd = format_str("workspace \"%s\", desktop close", hs->str);
		list_add(items, menu_item_new("Close desktop", cmd));
		free(cmd);
		menu_items_default_icons(items);
		menu_open(w->panel, items, true, popup_anchor_for_bar(s, (int)x, 0), NULL);
		return true;
	}
	if (button != BTN_LEFT) {
		return false;
	}
	ipc_panel_commandf(w->panel, "workspace \"%s\"", hs->str);
	return true;
}

static bool workspaces_scroll(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction) {
	ipc_panel_command(w->panel, direction < 0 ? "workspace prev_on_output" :
		"workspace next_on_output");
	return true;
}

/* "Desktop 2" or its name, and the titles of its windows. */
static char *workspaces_tooltip(struct widget *w, struct hotspot *hs) {
	if (!hs->str) {
		return NULL;
	}
	const char *colon = strchr(hs->str, ':');
	char *text = colon && colon[1] ? strdup(colon + 1) : format_str("Desktop %s", hs->str);
	list_t *windows = w->panel->state.windows;
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *win = windows->items[i];
		if (win->workspace && strcmp(win->workspace, hs->str) == 0) {
			char *more = format_str("%s\n%s", text, win->title && *win->title ? win->title :
				win->app_id);
			free(text);
			text = more;
		}
	}
	return text;
}

const struct widget_impl widget_workspaces = {
	.type = "workspaces",
	.init = workspaces_init,
	.destroy = workspaces_destroy,
	.measure = workspaces_measure,
	.render = workspaces_render,
	.click = workspaces_click,
	.scroll = workspaces_scroll,
	.tooltip = workspaces_tooltip,
};
