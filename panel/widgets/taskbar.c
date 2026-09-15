#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"
#include "tw_desktop.h"

/* ================= taskbar ================= */

enum hotspot_kind {
	HS_WINDOW = 1,
	HS_GROUP,
};

struct taskbar_entry {
	struct pwindow *window; // first window of the group
	int count;
	bool focused;
	bool urgent;
	bool minimized;
};

struct taskbar_data {
	bool icons_only;
	bool group;
	bool all_workspaces;
	bool all_outputs;
	int button_width;
	bool middle_close;
};

static bool theme_icons_only(struct panel *panel, struct widget *w) {
	struct taskbar_data *d = w->data;
	(void)d;
	const char *conf = widget_conf(w, "icons_only", "theme");
	if (strcasecmp(conf, "theme") == 0) {
		return tw_theme_bool(panel->theme, "taskbar.icons_only", false);
	}
	return twconf_parse_bool(conf, false);
}

static void taskbar_init(struct widget *w) {
	struct taskbar_data *d = calloc(1, sizeof(*d));
	w->data = d;
	d->all_workspaces = strcasecmp(widget_conf(w, "workspaces", "current"), "all") == 0;
	d->all_outputs = strcasecmp(widget_conf(w, "outputs", "current"), "all") == 0;
	d->middle_close = strcasecmp(widget_conf(w, "middle_click", "close"), "close") == 0;
}

static void taskbar_destroy(struct widget *w) {
	free(w->data);
}

static const char *visible_workspace(struct panel *panel, const char *output) {
	list_t *wss = panel->state.workspaces;
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		if (ws->visible && strcmp(ws->output, output) == 0) {
			return ws->name;
		}
	}
	return NULL;
}

/* Builds the list of taskbar entries shown on an output. */
static list_t *collect_entries(struct widget *w, struct panel_output *output) {
	struct panel *panel = w->panel;
	struct taskbar_data *d = w->data;
	bool group = widget_conf_bool(w, "group", theme_icons_only(panel, w));
	const char *ws_name = output && output->name ?
		visible_workspace(panel, output->name) : NULL;
	list_t *entries = create_list();
	list_t *windows = panel->state.windows;
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *win = windows->items[i];
		if (!d->all_outputs && output && output->name && strcmp(win->output, output->name) != 0) {
			continue;
		}
		if (!d->all_workspaces && ws_name && strcmp(win->workspace, ws_name) != 0) {
			continue;
		}
		struct taskbar_entry *entry = NULL;
		if (group && *win->app_id) {
			for (int j = 0; j < entries->length; j++) {
				struct taskbar_entry *e = entries->items[j];
				if (strcmp(e->window->app_id, win->app_id) == 0) {
					entry = e;
					break;
				}
			}
		}
		if (!entry) {
			entry = calloc(1, sizeof(*entry));
			entry->window = win;
			entry->minimized = true;
			list_add(entries, entry);
		}
		entry->count++;
		entry->focused |= win->focused;
		entry->urgent |= win->urgent;
		entry->minimized &= win->minimized;
	}
	return entries;
}

static int button_width(struct widget *w, struct render_ctx *ctx, bool icons_only) {
	const struct tw_theme *t = ctx->panel->theme;
	if (icons_only) {
		int fallback = ctx->style == PSV_AERO ? 60 : ctx->style == PSV_FLUENT ? 44 : 48;
		return widget_conf_int(w, "button_width",
			tw_theme_int(t, "taskbar.button_width", fallback));
	}
	return widget_conf_int(w, "max_width", tw_theme_int(t, "taskbar.button_width", 160));
}

static int taskbar_measure(struct widget *w, struct render_ctx *ctx) {
	bool icons_only = theme_icons_only(ctx->panel, w);
	if (!icons_only && !ctx->centered) {
		return -1;
	}
	list_t *entries = collect_entries(w, ctx->output);
	int count = entries->length;
	list_free_items_and_destroy(entries);
	return count * button_width(w, ctx, icons_only) + (icons_only ? 0 : 4);
}

static void draw_labeled_button(struct widget *w, struct render_ctx *ctx,
		struct taskbar_entry *e, struct pbox b) {
	cairo_t *cr = ctx->cairo;
	struct panel *panel = ctx->panel;
	const struct tw_theme *t = panel->theme;
	bool hover = render_hover(ctx, b);
	bool pressed = render_pressed(ctx, b);
	bool active = e->focused && !e->minimized;
	uint32_t fg = tw_theme_color(t, active ? "taskbar.active_fg" : "taskbar.fg", bar_fg(panel));
	int icon = 16;
	double inner_x = b.x + 6;

	switch (ctx->style) {
	case PSV_CLASSIC: {
		struct pbox bb = { b.x + 1, b.y + 4, b.width - 2, b.height - 6 };
		pd_rect(cr, bb.x, bb.y, bb.width, bb.height, 0xc0c0c0ff);
		if (active || pressed) {
			// checkered light background of a pressed task button
			cairo_save(cr);
			cairo_rectangle(cr, bb.x + 2, bb.y + 2, bb.width - 4, bb.height - 4);
			cairo_clip(cr);
			for (int yy = bb.y; yy < bb.y + bb.height; yy++) {
				for (int xx = bb.x + ((yy - bb.y) % 2); xx < bb.x + bb.width; xx += 2) {
					pd_rect(cr, xx, yy, 1, 1, 0xffffffff);
				}
			}
			cairo_restore(cr);
		}
		pd_bevel(cr, bb.x, bb.y, bb.width, bb.height, active || pressed);
		inner_x = bb.x + 4 + (active || pressed ? 1 : 0);
		b = bb;
		break;
	}
	case PSV_LUNA: {
		const char *key = active || pressed ? "taskbar.active" : hover ? "taskbar.hover" : "taskbar.bg";
		pd_rounded(cr, b.x + 2, b.y + 3, b.width - 4, b.height - 5, 3);
		pd_fill(cr, t, key, b.y + 3, b.height - 5,
			active ? 0x1e52b7ff : hover ? 0x4a8ffbff : 0x3574eeff);
		pd_rounded(cr, b.x + 2.5, b.y + 3.5, b.width - 5, b.height - 6, 3);
		cairo_set_source_u32(cr, active ? 0x00000050 : 0xffffff30);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		inner_x = b.x + 8;
		break;
	}
	default:
		render_item_bg(ctx, b, active, hover, pressed);
		if (e->count > 0 && ctx->style == PSV_FLAT) {
			pd_rect(cr, b.x, b.y + b.height - 2, b.width, 2,
				tw_theme_color(t, "taskbar.indicator", 0x76b9edff));
		}
		break;
	}

	cairo_surface_t *surface = apps_icon_for_window(panel, e->window, icon * ctx->surface->scale);
	pd_icon(cr, surface, inner_x, b.y + (b.height - icon) / 2.0 + (active && ctx->style == PSV_CLASSIC ? 1 : 0), icon);
	double tx = inner_x + icon + 5;
	const char *font = active && ctx->style == PSV_CLASSIC ? bar_bold_font(panel) : bar_font(panel);
	char label[512];
	if (e->count > 1) {
		snprintf(label, sizeof(label), "%s (%d)", apps_display_name(e->window->app_id), e->count);
	} else {
		snprintf(label, sizeof(label), "%s", e->window->title);
	}
	pd_text(cr, font, label, tx, b.y + (active && ctx->style == PSV_CLASSIC ? 1 : 0),
		b.x + b.width - tx - 6, b.height, fg, PD_LEFT);
}

static void draw_icon_button(struct widget *w, struct render_ctx *ctx,
		struct taskbar_entry *e, struct pbox b) {
	cairo_t *cr = ctx->cairo;
	struct panel *panel = ctx->panel;
	const struct tw_theme *t = panel->theme;
	bool hover = render_hover(ctx, b);
	bool pressed = render_pressed(ctx, b);
	bool active = e->focused && !e->minimized;
	int icon = ctx->style == PSV_AERO ? 30 : 24;

	switch (ctx->style) {
	case PSV_AERO:
		pd_rounded(cr, b.x + 2.5, b.y + 2.5, b.width - 5, b.height - 5, 3);
		pd_source(cr, t, active || pressed ? "taskbar.active" : hover ? "taskbar.hover" :
			"taskbar.running", b.y, b.height, 0xffffff20);
		cairo_fill_preserve(cr);
		cairo_set_source_u32(cr, active ? 0xffffff90 : 0xffffff45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		break;
	case PSV_FLUENT: {
		render_item_bg(ctx, b, active, hover, pressed);
		int pill = active ? 16 : 6;
		pd_rounded(cr, b.x + (b.width - pill) / 2.0, b.y + b.height - 6, pill, 3, 1.5);
		cairo_set_source_u32(cr, tw_theme_color(t, active ? "taskbar.indicator" :
			"taskbar.indicator_inactive", active ? 0x005fb8ff : 0x8a8a8aff));
		cairo_fill(cr);
		break;
	}
	default: {
		render_item_bg(ctx, b, active, hover, pressed);
		int inset = active ? 0 : 6;
		pd_rect(cr, b.x + inset, b.y + b.height - 2, b.width - 2 * inset, 2,
			tw_theme_color(t, "taskbar.indicator", 0x76b9edff));
		break;
	}
	}
	if (e->urgent) {
		pd_rect(cr, b.x + 2, b.y + 2, b.width - 4, 3, 0xf7a71fff);
	}
	cairo_surface_t *surface = apps_icon_for_window(panel, e->window, icon * ctx->surface->scale);
	double dy = pressed ? 1 : 0;
	pd_icon(cr, surface, b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0 + dy -
		(ctx->style == PSV_FLUENT ? 2 : 0), icon);
	if (e->count > 1 && ctx->style == PSV_AERO) {
		// stacked look for grouped windows
		pd_rect(cr, b.x + b.width - 4, b.y + 5, 1, b.height - 10, 0xffffff50);
	}
}

static void taskbar_render(struct widget *w, struct render_ctx *ctx, struct pbox box) {
	struct panel *panel = ctx->panel;
	bool icons_only = theme_icons_only(panel, w);
	list_t *entries = collect_entries(w, ctx->output);
	int n = entries->length;
	if (n == 0) {
		list_free(entries);
		return;
	}
	int bw = button_width(w, ctx, icons_only);
	if (!icons_only && n * bw > box.width - 4) {
		bw = (box.width - 4) / n;
	}
	int x = box.x + (icons_only ? 0 : 2);
	for (int i = 0; i < n; i++) {
		struct taskbar_entry *e = entries->items[i];
		struct pbox b = { x, box.y, bw, box.height };
		if (icons_only) {
			draw_icon_button(w, ctx, e, b);
		} else {
			draw_labeled_button(w, ctx, e, b);
		}
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w,
			e->count > 1 ? HS_GROUP : HS_WINDOW, e->window->id, e->window->app_id);
		x += bw;
		free(e);
	}
	list_free(entries);
}

list_t *taskbar_window_menu(struct panel *panel, struct pwindow *win) {
	list_t *items = create_list();
	bool window_mode = panel->layout == LAYOUT_WINDOW;
	char cmd[256];
	struct tw_desktop_entry *entry = apps_find(win->app_id);
	if (entry) {
		char *exec = tw_desktop_exec_command(entry);
		if (exec) {
			char *launch = format_str("exec %s", exec);
			struct menu_item *item = menu_item_new(entry->name, launch);
			item->icon = entry->icon ? strdup(entry->icon) : NULL;
			item->bold = true;
			list_add(items, item);
			list_add(items, menu_item_separator());
			free(launch);
			free(exec);
		}
	}
	if (window_mode) {
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] %s", (long long)win->id,
			win->minimized ? "minimize disable" : "maximize disable");
		struct menu_item *restore = menu_item_new("Restore", cmd);
		restore->disabled = !win->minimized && !win->maximized;
		list_add(items, restore);
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] minimize enable", (long long)win->id);
		struct menu_item *minimize = menu_item_new("Minimize", cmd);
		minimize->disabled = win->minimized;
		list_add(items, minimize);
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] maximize enable", (long long)win->id);
		struct menu_item *maximize = menu_item_new("Maximize", cmd);
		maximize->disabled = win->maximized || !win->floating;
		list_add(items, maximize);
	} else {
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] fullscreen toggle", (long long)win->id);
		list_add(items, menu_item_new("Fullscreen", cmd));
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] floating toggle", (long long)win->id);
		list_add(items, menu_item_new("Toggle floating", cmd));
	}

	struct menu_item *move = menu_item_new("Move to desktop", NULL);
	move->children = create_list();
	list_t *wss = panel->state.workspaces;
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] move container to workspace \"%s\"",
			(long long)win->id, ws->name);
		struct menu_item *item = menu_item_new(ws->name, cmd);
		item->checked = strcmp(ws->name, win->workspace) == 0;
		list_add(move->children, item);
	}
	snprintf(cmd, sizeof(cmd), "[con_id=%lld] move container to workspace next_on_output",
		(long long)win->id);
	list_add(move->children, menu_item_separator());
	list_add(move->children, menu_item_new("Next desktop", cmd));
	list_add(items, move);

	snprintf(cmd, sizeof(cmd), "[con_id=%lld] sticky toggle", (long long)win->id);
	list_add(items, menu_item_new("Show on all desktops", cmd));

	list_t *extra = panel_named_menu(panel, "window");
	if (extra) {
		char id[32];
		snprintf(id, sizeof(id), "%lld", (long long)win->id);
		list_add(items, menu_item_separator());
		for (int i = 0; i < extra->length; i++) {
			struct menu_item *item = extra->items[i];
			if (item->command) {
				char *pos;
				while ((pos = strstr(item->command, "{id}"))) {
					char *next = format_str("%.*s%s%s", (int)(pos - item->command),
						item->command, id, pos + 4);
					free(item->command);
					item->command = next;
				}
			}
			list_add(items, item);
		}
		list_free(extra);
	}

	list_add(items, menu_item_separator());
	snprintf(cmd, sizeof(cmd), "[con_id=%lld] kill", (long long)win->id);
	struct menu_item *close = menu_item_new("Close window", cmd);
	close->bold = !entry;
	list_add(items, close);
	return items;
}

static void activate_window(struct panel *panel, struct pwindow *win) {
	if (win->focused && !win->minimized && panel->layout == LAYOUT_WINDOW) {
		ipc_panel_commandf(panel, "[con_id=%lld] minimize enable", (long long)win->id);
	} else {
		ipc_panel_commandf(panel, "[con_id=%lld] focus", (long long)win->id);
	}
}

static bool taskbar_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct panel *panel = w->panel;
	struct taskbar_data *d = w->data;
	struct pwindow *win = panel_find_window(panel, hs->id);
	if (!win) {
		return false;
	}
	if (button == BTN_LEFT) {
		if (hs->kind == HS_GROUP) {
			list_t *items = create_list();
			for (int i = 0; i < panel->state.windows->length; i++) {
				struct pwindow *o = panel->state.windows->items[i];
				if (strcmp(o->app_id, win->app_id) != 0) {
					continue;
				}
				char cmd[64];
				snprintf(cmd, sizeof(cmd), "[con_id=%lld] focus", (long long)o->id);
				struct menu_item *item = menu_item_new(o->title, cmd);
				item->checked = o->focused;
				list_add(items, item);
			}
			struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x, hs->box.width);
			menu_open(panel, items, true, anchor, NULL);
		} else {
			activate_window(panel, win);
		}
		return true;
	}
	if (button == BTN_MIDDLE) {
		if (d->middle_close) {
			ipc_panel_commandf(panel, "[con_id=%lld] kill", (long long)win->id);
		} else {
			struct tw_desktop_entry *entry = apps_find(win->app_id);
			if (entry) {
				apps_launch(panel, entry);
			}
		}
		return true;
	}
	if (button == BTN_RIGHT) {
		struct popup_anchor anchor = popup_anchor_for_bar(s, (int)x, 0);
		menu_open(panel, taskbar_window_menu(panel, win), true, anchor, NULL);
		return true;
	}
	return false;
}

static char *taskbar_tooltip(struct widget *w, struct hotspot *hs) {
	struct pwindow *win = panel_find_window(w->panel, hs->id);
	if (!win) {
		return NULL;
	}
	if (hs->kind == HS_GROUP) {
		int count = 0;
		for (int i = 0; i < w->panel->state.windows->length; i++) {
			struct pwindow *o = w->panel->state.windows->items[i];
			count += strcmp(o->app_id, win->app_id) == 0;
		}
		return format_str("%s - %d windows", apps_display_name(win->app_id), count);
	}
	return strdup(win->title);
}

bool taskbar_activate_index(struct panel *panel, int index) {
	if (!panel->config) {
		return false;
	}
	struct widget *taskbar = NULL;
	for (int i = 0; i < panel->config->widgets->length; i++) {
		struct widget *w = panel->config->widgets->items[i];
		if (w->impl == &widget_taskbar && w->active) {
			taskbar = w;
			break;
		}
	}
	if (!taskbar) {
		return false;
	}
	list_t *entries = collect_entries(taskbar, panel_focused_output(panel));
	bool ok = false;
	if (index >= 1 && index <= entries->length) {
		struct taskbar_entry *e = entries->items[index - 1];
		activate_window(panel, e->window);
		ok = true;
	}
	list_free_items_and_destroy(entries);
	return ok;
}

const struct widget_impl widget_taskbar = {
	.type = "taskbar",
	.init = taskbar_init,
	.destroy = taskbar_destroy,
	.measure = taskbar_measure,
	.render = taskbar_render,
	.click = taskbar_click,
	.tooltip = taskbar_tooltip,
};

/* ================= start button ================= */

static int start_measure(struct widget *w, struct render_ctx *ctx) {
	const struct tw_theme *t = ctx->panel->theme;
	int fallback;
	switch (ctx->style) {
	case PSV_CLASSIC: {
		const char *label = widget_conf(w, "label", tw_theme_str(t, "start.label", "Start"));
		fallback = render_text_width(ctx, bar_bold_font(ctx->panel), label) + 40;
		break;
	}
	case PSV_LUNA:
		fallback = 100;
		break;
	case PSV_AERO:
		fallback = 54;
		break;
	case PSV_FLUENT:
		fallback = 44;
		break;
	default:
		fallback = 48;
		break;
	}
	return widget_conf_int(w, "width", tw_theme_int(t, "start.width", fallback));
}

static void start_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	cairo_t *cr = ctx->cairo;
	struct panel *panel = ctx->panel;
	const struct tw_theme *t = panel->theme;
	bool hover = render_hover(ctx, b);
	bool open = popup_is_open(panel, POPUP_STARTMENU);
	bool pressed = render_pressed(ctx, b) || open;
	const char *label = widget_conf(w, "label", tw_theme_str(t, "start.label", "Start"));
	uint32_t fg = tw_theme_color(t, "start.fg", bar_fg(panel));

	// "start.icon": a text icon (e.g. a Nerd Font logo) instead of the drawn logo
	const char *icon = tw_theme_str(t, "start.icon", NULL);
	if (icon && ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA && ctx->style != PSV_AERO) {
		render_item_bg(ctx, b, false, hover, pressed);
		pd_text(cr, tw_theme_str(t, "start.icon_font", bar_font(panel)), icon, b.x, b.y,
			b.width, b.height, hover ? tw_theme_color(t, "start.hover_fg", fg) : fg, PD_CENTER);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
		return;
	}

	switch (ctx->style) {
	case PSV_CLASSIC: {
		struct pbox bb = { b.x + 2, b.y + 4, b.width - 3, b.height - 6 };
		pd_rect(cr, bb.x, bb.y, bb.width, bb.height, 0xc0c0c0ff);
		pd_bevel(cr, bb.x, bb.y, bb.width, bb.height, pressed);
		int off = pressed ? 1 : 0;
		pd_glyph_windows(cr, bb.x + 5 + off, bb.y + (bb.height - 14) / 2.0 + off, 14,
			0xff0000ff, 0x00a000ff, 0x0000ffff, 0xffd800ff, true);
		pd_text(cr, bar_bold_font(panel), label, bb.x + 23 + off, bb.y + off,
			bb.width - 25, bb.height, fg, PD_LEFT);
		break;
	}
	case PSV_LUNA: {
		pd_rounded4(cr, b.x, b.y, b.width, b.height, 0, b.height / 2.0, b.height / 2.0, 0);
		pd_fill(cr, t, hover || pressed ? "start.hover" : "start.bg", b.y, b.height, 0x379f37ff);
		cairo_save(cr);
		pd_rounded4(cr, b.x, b.y, b.width, b.height, 0, b.height / 2.0, b.height / 2.0, 0);
		cairo_clip(cr);
		pd_rect(cr, b.x, b.y, b.width, 1, 0xffffff60);
		cairo_restore(cr);
		pd_glyph_windows(cr, b.x + 10, b.y + (b.height - 18) / 2.0, 18,
			0xf35325ff, 0x81bc06ff, 0x05a6f0ff, 0xffba08ff, true);
		const char *font = tw_theme_str(t, "start.font", "Trebuchet MS, Noto Sans Bold Italic 12");
		pd_text(cr, font, label, b.x + 34, b.y + 1, b.width - 36, b.height, 0x00000060, PD_LEFT);
		pd_text(cr, font, label, b.x + 33, b.y, b.width - 36, b.height, fg, PD_LEFT);
		break;
	}
	case PSV_AERO: {
		double r = (b.height - 4) / 2.0;
		double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
		if (hover || pressed) {
			cairo_arc(cr, cx, cy, r + 3, 0, 6.2832);
			cairo_set_source_u32(cr, 0x7fd0ff40);
			cairo_fill(cr);
		}
		cairo_arc(cr, cx, cy, r, 0, 6.2832);
		cairo_pattern_t *p = pd_gradient(tw_theme_str(t, hover || pressed ? "start.hover_gradient" :
			"start.bg_gradient", "0:#8dd1ff 0.45:#2b7fd0 0.5:#0f4f98 1:#3aa2e8"),
			0, cy - r, 0, cy + r);
		cairo_set_source(cr, p);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(p);
		cairo_set_source_u32(cr, 0x0a2a50c0);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		double g = r * 1.05;
		pd_glyph_windows(cr, cx - g / 2, cy - g / 2, g, 0xf35325ff, 0x81bc06ff,
			0x05a6f0ff, 0xffba08ff, true);
		break;
	}
	case PSV_FLUENT: {
		render_item_bg(ctx, b, false, hover, pressed);
		double g = 20;
		pd_glyph_windows(cr, b.x + (b.width - g) / 2, b.y + (b.height - g) / 2 - 1, g,
			0x0078d4ff, 0x0078d4ff, 0x0078d4ff, 0x0078d4ff, false);
		break;
	}
	default: {
		render_item_bg(ctx, b, false, hover, pressed);
		double g = 16;
		uint32_t c = hover ? tw_theme_color(t, "start.hover_fg", 0x0a84ffff) : fg;
		pd_glyph_windows(cr, b.x + (b.width - g) / 2, b.y + (b.height - g) / 2, g,
			c, c, c, c, false);
		break;
	}
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

list_t *start_default_menu(struct panel *panel) {
	list_t *named = panel_named_menu(panel, "start");
	if (named) {
		return named;
	}
	list_t *items = create_list();
	list_add(items, menu_item_new("Terminal", "exec xfce4-terminal"));
	list_add(items, menu_item_new("File Explorer", "exec thunar"));
	list_add(items, menu_item_new("Task Manager", "exec xfce4-terminal -e btop"));
	list_add(items, menu_item_new("Run...", "panel run"));
	list_add(items, menu_item_separator());
	struct menu_item *power = menu_item_new("Shut down or sign out", NULL);
	power->children = create_list();
	list_add(power->children, menu_item_new("Lock", "exec swaylock -f -c 000000"));
	list_add(power->children, menu_item_new("Sign out", "exit"));
	list_add(power->children, menu_item_new("Restart tileWin", "restart"));
	list_add(power->children, menu_item_separator());
	list_add(power->children, menu_item_new("Restart", "exec systemctl reboot"));
	list_add(power->children, menu_item_new("Shut down", "exec systemctl poweroff"));
	list_add(items, power);
	list_add(items, menu_item_new("Desktop", "showdesktop"));
	return items;
}

static bool start_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct panel *panel = w->panel;
	if (button == BTN_LEFT) {
		if (popup_is_open(panel, POPUP_STARTMENU)) {
			popup_close_all(panel);
		} else {
			startmenu_toggle(panel, s->output, false);
		}
		return true;
	}
	if (button == BTN_RIGHT) {
		struct popup_anchor anchor = popup_anchor_for_bar(s, (int)x, 0);
		menu_open(panel, w->menu ? w->menu : start_default_menu(panel), !w->menu, anchor, NULL);
		return true;
	}
	return false;
}

static char *start_tooltip(struct widget *w, struct hotspot *hs) {
	return strdup("Start");
}

const struct widget_impl widget_start = {
	.type = "start",
	.measure = start_measure,
	.render = start_render,
	.click = start_click,
	.tooltip = start_tooltip,
};

/* ================= quick launch ================= */

struct quick_item {
	char *id;      // desktop entry id or command
	char *icon;
	char *label;
};

static void quicklaunch_init(struct widget *w) {
	list_t *items = create_list();
	for (int i = 0; w->conf && i < twconf_count(w->conf); i++) {
		struct twconf_node *child = twconf_at(w->conf, i);
		if (strcmp(child->name, "item") != 0 || child->argc < 1) {
			continue;
		}
		struct quick_item *item = calloc(1, sizeof(*item));
		item->id = strdup(child->argv[0]);
		if (child->argc > 1) {
			item->icon = strdup(child->argv[1]);
		}
		list_add(items, item);
	}
	w->data = items;
}

static void quicklaunch_destroy(struct widget *w) {
	list_t *items = w->data;
	for (int i = 0; items && i < items->length; i++) {
		struct quick_item *item = items->items[i];
		free(item->id);
		free(item->icon);
		free(item->label);
		free(item);
	}
	list_free(items);
}

static int quick_size(struct render_ctx *ctx) {
	return ctx->style == PSV_CLASSIC || ctx->style == PSV_LUNA ? 22 : 40;
}

static int quicklaunch_measure(struct widget *w, struct render_ctx *ctx) {
	list_t *items = w->data;
	return items->length * quick_size(ctx) + 4;
}

static void quicklaunch_render(struct widget *w, struct render_ctx *ctx, struct pbox box) {
	list_t *items = w->data;
	int size = quick_size(ctx);
	int icon = size >= 40 ? 24 : 16;
	int x = box.x + 2;
	for (int i = 0; i < items->length; i++) {
		struct quick_item *item = items->items[i];
		struct pbox b = { x, box.y + (box.height - (size >= 40 ? box.height : size)) / 2,
			size, size >= 40 ? box.height : size };
		bool hover = render_hover(ctx, b);
		bool pressed = render_pressed(ctx, b);
		if (ctx->style == PSV_CLASSIC) {
			if (hover) {
				pd_bevel(ctx->cairo, b.x, b.y, b.width, b.height, pressed);
			}
		} else {
			render_item_bg(ctx, b, false, hover, pressed);
		}
		const char *icon_name = item->icon;
		struct tw_desktop_entry *entry = apps_find(item->id);
		if (!icon_name && entry) {
			icon_name = entry->icon;
		}
		cairo_surface_t *surface = apps_icon(ctx->panel, icon_name ? icon_name : item->id,
			icon * ctx->surface->scale);
		pd_icon(ctx->cairo, surface, b.x + (b.width - icon) / 2.0,
			b.y + (b.height - icon) / 2.0, icon);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, i, NULL);
		x += size;
	}
}

static bool quicklaunch_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	list_t *items = w->data;
	if (button != BTN_LEFT || hs->id < 0 || hs->id >= items->length) {
		return false;
	}
	struct quick_item *item = items->items[hs->id];
	struct tw_desktop_entry *entry = apps_find(item->id);
	if (entry) {
		apps_launch(w->panel, entry);
	} else {
		ipc_panel_commandf(w->panel, "exec %s", item->id);
	}
	return true;
}

static char *quicklaunch_tooltip(struct widget *w, struct hotspot *hs) {
	list_t *items = w->data;
	if (hs->id < 0 || hs->id >= items->length) {
		return NULL;
	}
	struct quick_item *item = items->items[hs->id];
	struct tw_desktop_entry *entry = apps_find(item->id);
	return strdup(entry ? entry->name : item->id);
}

const struct widget_impl widget_quicklaunch = {
	.type = "quicklaunch",
	.init = quicklaunch_init,
	.destroy = quicklaunch_destroy,
	.measure = quicklaunch_measure,
	.render = quicklaunch_render,
	.click = quicklaunch_click,
	.tooltip = quicklaunch_tooltip,
};
