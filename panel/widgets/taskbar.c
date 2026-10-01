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
	HS_SOUND, // the speaker of a window playing sound
	HS_GROUP_SOUND,
};

struct taskbar_entry {
	struct pwindow *window; // first window of the group
	int count;
	bool focused;
	bool urgent;
	bool minimized;
	struct pbox sound; // its speaker, while it plays sound
};

struct taskbar_data {
	bool icons_only;
	bool group;
	bool all_workspaces;
	// current: the windows of the screen the bar is on; all: every window on
	// every bar; main: every window on the main display's bar, as on Windows
	enum { OUTPUTS_CURRENT, OUTPUTS_ALL, OUTPUTS_MAIN } outputs;
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
	const char *outputs = widget_conf(w, "outputs", "current");
	d->outputs = strcasecmp(outputs, "all") == 0 ? OUTPUTS_ALL :
		strcasecmp(outputs, "main") == 0 ? OUTPUTS_MAIN : OUTPUTS_CURRENT;
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
		bool here = !output || !output->name || strcmp(win->output, output->name) == 0;
		if (!here && !(d->outputs == OUTPUTS_ALL || (d->outputs == OUTPUTS_MAIN &&
				panel->state.main_output &&
				strcmp(output->name, panel->state.main_output) == 0))) {
			continue;
		}
		// a window of another screen counts as current on the desktop that screen shows
		const char *shown = here ? ws_name : visible_workspace(panel, win->output);
		if (!d->all_workspaces && shown && strcmp(win->workspace, shown) != 0) {
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

/*
 * What the app reports (badges.c): a download or copy filling its button, as
 * on Windows 7, in the theme's colors (blocks in the classic look).
 */
static void draw_progress(struct render_ctx *ctx, struct taskbar_entry *e, struct pbox b) {
	struct app_badge badge;
	if (!badges_for_app(e->window->app_id, &badge) || badge.progress < 0) {
		return;
	}
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	if (ctx->style == PSV_CLASSIC) {
		uint32_t color = tw_theme_color(t, "taskbar.progress", 0x000080ff);
		double x = b.x + 4, w = (b.width - 8) * badge.progress, y = b.y + b.height - 6;
		for (double bx = 0; bx + 1 < w; bx += 6) {
			pd_rect(cr, x + bx, y, fmin(4, w - bx), 3, color);
		}
		return;
	}
	uint32_t color = tw_theme_color(t, "taskbar.progress", 0x06b025a0);
	pd_rounded(cr, b.x + 2, b.y + 3, (b.width - 4) * badge.progress, b.height - 6,
		ctx->style == PSV_FLUENT || ctx->style == PSV_LUNA ? 3 : 0);
	pd_color(cr, color);
	cairo_fill(cr);
}

/* The number an app shows (unread mail, messages) on the corner of its icon. */
static void draw_badge(struct render_ctx *ctx, struct taskbar_entry *e, double ix, double iy,
		int icon) {
	struct app_badge badge;
	if (!badges_for_app(e->window->app_id, &badge) || badge.count <= 0) {
		return;
	}
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	char text[16];
	if (badge.count > 99) {
		snprintf(text, sizeof(text), "99+");
	} else {
		snprintf(text, sizeof(text), "%lld", (long long)badge.count);
	}
	const char *font = tw_theme_str(t, "taskbar.badge_font", "Noto Sans Bold 7");
	int tw = 0, th = 0;
	pd_text_size(cr, font, text, &tw, &th);
	double h = icon >= 24 ? 14 : 12, w = fmax(h, tw + 6);
	double x = ix + icon - w / 2 - 1, y = iy - 3;
	uint32_t bg = tw_theme_color(t, "taskbar.badge_bg", ctx->style == PSV_CLASSIC ?
		0x800000ff : 0xd13438ff);
	uint32_t fg = tw_theme_color(t, "taskbar.badge_fg", 0xffffffff);
	if (ctx->style == PSV_CLASSIC) {
		pd_rect(cr, x, y, w, h, bg);
		cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
		pd_color(cr, 0x000000ff);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		pd_rounded(cr, x, y, w, h, h / 2);
		pd_color(cr, bg);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000060);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	pd_text(cr, font, text, x, y, w, h, fg, PD_CENTER);
}

/*
 * A speaker on the button of a window playing sound (struck through when it is
 * muted): a click mutes or unmutes it, the wheel changes its volume. Returns
 * its box, or a zero box for none.
 */
static struct pbox draw_sound(struct render_ctx *ctx, struct taskbar_entry *e, double x,
		double y, double size, bool plate) {
	struct app_sound sound;
	if (!appsound_for_window(ctx->panel, e->window, e->count > 1, &sound)) {
		return (struct pbox){ 0 };
	}
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	uint32_t fg = tw_theme_color(t, "taskbar.fg", bar_fg(ctx->panel));
	if (plate) {
		// on top of the icon: a disc in the color of the taskbar behind it
		uint32_t bg = tw_theme_color(t, "taskbar.sound_bg", ctx->style == PSV_CLASSIC ?
			0xc0c0c0ff : tw_theme_color(t, "panel.bg", 0x202020ff) | 0xff);
		cairo_arc(cr, x + size / 2, y + size / 2, size / 2 + 1, 0, 2 * M_PI);
		pd_color(cr, bg);
		cairo_fill(cr);
	}
	ti_speaker(ctx->panel, cr, x + 1, y + 1, size - 2, sound.muted ? 0 : sound.volume,
		sound.muted, fg);
	return (struct pbox){ (int)x - 2, (int)y - 2, (int)size + 4, (int)size + 4 };
}

static bool badge_urgent(struct taskbar_entry *e) {
	struct app_badge badge;
	return badges_for_app(e->window->app_id, &badge) && badge.urgent;
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
		// the face and the checkers in the theme's colors, which the dark
		// scheme makes dark: its light text stays readable
		struct pbox bb = { b.x + 1, b.y + 4, b.width - 2, b.height - 6 };
		pd_rect(cr, bb.x, bb.y, bb.width, bb.height,
			tw_theme_color(t, "taskbar.bg", tw_theme_color(t, "panel.bg", 0xc0c0c0ff)));
		if (active || pressed) {
			// checkered light background of a pressed task button
			uint32_t check = tw_theme_color(t, "decoration.highlight", 0xffffffff);
			cairo_save(cr);
			cairo_rectangle(cr, bb.x + 2, bb.y + 2, bb.width - 4, bb.height - 4);
			cairo_clip(cr);
			for (int yy = bb.y; yy < bb.y + bb.height; yy++) {
				for (int xx = bb.x + ((yy - bb.y) % 2); xx < bb.x + bb.width; xx += 2) {
					pd_rect(cr, xx, yy, 1, 1, check);
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
	draw_progress(ctx, e, b);

	cairo_surface_t *surface = apps_icon_for_window(panel, e->window, icon * ctx->surface->scale);
	double icon_y = b.y + (b.height - icon) / 2.0 + (active && ctx->style == PSV_CLASSIC ? 1 : 0);
	pd_icon(cr, surface, inner_x, icon_y, icon);
	draw_badge(ctx, e, inner_x, icon_y, icon);
	double tx = inner_x + icon + 5;
	const char *font = active && ctx->style == PSV_CLASSIC ? bar_bold_font(panel) : bar_font(panel);
	char label[512];
	if (e->count > 1) {
		snprintf(label, sizeof(label), "%s (%d)", apps_display_name(e->window->app_id), e->count);
	} else {
		snprintf(label, sizeof(label), "%s", e->window->title);
	}
	double end = b.x + b.width - 6;
	struct pbox speaker = draw_sound(ctx, e, end - 14, b.y + (b.height - 14) / 2.0, 14, false);
	if (speaker.width > 0) {
		end -= 18;
		e->sound = speaker;
	}
	pd_text(cr, font, label, tx, b.y + (active && ctx->style == PSV_CLASSIC ? 1 : 0),
		end - tx, b.height, fg, PD_LEFT);
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
	draw_progress(ctx, e, b);
	if (e->urgent || badge_urgent(e)) {
		pd_rect(cr, b.x + 2, b.y + 2, b.width - 4, 3, 0xf7a71fff);
	}
	cairo_surface_t *surface = apps_icon_for_window(panel, e->window, icon * ctx->surface->scale);
	double dy = pressed ? 1 : 0;
	double ix = b.x + (b.width - icon) / 2.0;
	double iy = b.y + (b.height - icon) / 2.0 + dy - (ctx->style == PSV_FLUENT ? 2 : 0);
	pd_icon(cr, surface, ix, iy, icon);
	draw_badge(ctx, e, ix, iy, icon);
	e->sound = draw_sound(ctx, e, ix + icon - 8, iy + icon - 8, 12, true);
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
		if (e->sound.width > 0) {
			psurface_add_hotspot(ctx->surface, e->sound.x, e->sound.y, e->sound.width,
				e->sound.height, w, e->count > 1 ? HS_GROUP_SOUND : HS_SOUND, e->window->id,
				e->window->app_id);
		}
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
	jumplist_add(entry, items);
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

	if (window_mode) {
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] always_on_top toggle", (long long)win->id);
		struct menu_item *above = menu_item_new("Always on top", cmd);
		above->checked = win->above;
		above->disabled = !win->floating;
		list_add(items, above);
	}

	// see-through windows, like the transparency menu of AquaSnap
	struct menu_item *transparency = menu_item_new("Set transparency", NULL);
	transparency->children = create_list();
	for (int percent = 0; percent <= 90; percent += 10) {
		// written from whole numbers, so no decimal comma can creep in
		int value = 100 - percent;
		snprintf(cmd, sizeof(cmd), "[con_id=%lld] opacity %d.%02d", (long long)win->id,
			value / 100, value % 100);
		char label[32];
		if (percent == 0) {
			snprintf(label, sizeof(label), "Opaque");
		} else {
			snprintf(label, sizeof(label), "%d%%", percent);
		}
		list_add(transparency->children, menu_item_new(label, cmd));
	}
	list_add(items, transparency);

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
	menu_items_default_icons(items);
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
	if (hs->kind == HS_SOUND || hs->kind == HS_GROUP_SOUND) {
		if (button == BTN_LEFT) {
			appsound_toggle_mute(panel, win, hs->kind == HS_GROUP_SOUND);
		}
		return true;
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

/* The wheel on the speaker of a button: that app's volume, 5 at a time. */
static bool taskbar_scroll(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction) {
	if (hs->kind != HS_SOUND && hs->kind != HS_GROUP_SOUND) {
		return false;
	}
	struct pwindow *win = panel_find_window(w->panel, hs->id);
	if (win) {
		appsound_change_volume(w->panel, win, hs->kind == HS_GROUP_SOUND,
			direction < 0 ? 5 : -5);
	}
	return true;
}

static char *taskbar_tooltip(struct widget *w, struct hotspot *hs) {
	struct pwindow *win = panel_find_window(w->panel, hs->id);
	if (!win) {
		return NULL;
	}
	if (hs->kind == HS_SOUND || hs->kind == HS_GROUP_SOUND) {
		struct app_sound sound;
		bool muted = appsound_for_window(w->panel, win, hs->kind == HS_GROUP_SOUND, &sound) &&
			sound.muted;
		return format_str("%s: %s (scroll for the volume)", apps_display_name(win->app_id),
			muted ? "click to unmute" : "click to mute");
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
	.scroll = taskbar_scroll,
	.tooltip = taskbar_tooltip,
};

/* ================= start button ================= */

static int start_measure(struct widget *w, struct render_ctx *ctx) {
	const struct tw_theme *t = ctx->panel->theme;
	int fallback;
	switch (ctx->style) {
	case PSV_CLASSIC: {
		const char *label = widget_conf(w, "label", tw_theme_str(t, "start.label", "Start"));
		// the theme's width is the least: the label must fit in the font there is
		int needed = render_text_width(ctx, bar_bold_font(ctx->panel), label) + 32;
		int width = tw_theme_int(t, "start.width", needed);
		return widget_conf_int(w, "width", width > needed ? width : needed);
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
		pd_rect(cr, bb.x, bb.y, bb.width, bb.height,
			tw_theme_color(t, "start.bg", tw_theme_color(t, "panel.bg", 0xc0c0c0ff)));
		pd_bevel(cr, bb.x, bb.y, bb.width, bb.height, pressed);
		int off = pressed ? 1 : 0;
		pd_glyph_windows(cr, bb.x + 5 + off, bb.y + (bb.height - 14) / 2.0 + off, 14,
			0xff0000ff, 0x00a000ff, 0x0000ffff, 0xffd800ff, true);
		pd_text(cr, bar_bold_font(panel), label, bb.x + 23 + off, bb.y + off,
			bb.width - 25, bb.height, fg, PD_LEFT);
		break;
	}
	case PSV_LUNA: {
		// the green pill of Windows XP: square on the left, rounded on the right,
		// a glossy band near its top, lit on the left and shaded towards the right
		double r = b.height * 0.42;
		const char *key = pressed && tw_theme_str(t, "start.pressed_gradient", NULL) ?
			"start.pressed" : hover || pressed ? "start.hover" : "start.bg";
		pd_rounded4(cr, b.x, b.y, b.width, b.height, 0, r, r, 0);
		pd_fill(cr, t, key, b.y, b.height, 0x379f37ff);
		cairo_save(cr);
		pd_rounded4(cr, b.x, b.y, b.width, b.height, 0, r, r, 0);
		cairo_clip(cr);
		cairo_pattern_t *side = cairo_pattern_create_linear(b.x, 0, b.x + b.width, 0);
		cairo_pattern_add_color_stop_rgba(side, 0, 1, 1, 1, 0.18);
		cairo_pattern_add_color_stop_rgba(side, 0.05, 1, 1, 1, 0.04);
		cairo_pattern_add_color_stop_rgba(side, 0.15, 1, 1, 1, 0);
		cairo_pattern_add_color_stop_rgba(side, 0.75, 0, 0, 0, 0);
		cairo_pattern_add_color_stop_rgba(side, 1, 0, 0, 0, 0.22);
		cairo_set_source(cr, side);
		cairo_paint(cr);
		cairo_pattern_destroy(side);
		cairo_restore(cr);
		// the dark rim around the rounded end
		cairo_new_path(cr);
		cairo_arc(cr, b.x + b.width - r - 0.5, b.y + r, r, -M_PI / 2, 0);
		cairo_arc(cr, b.x + b.width - r - 0.5, b.y + b.height - r, r, 0, M_PI / 2);
		cairo_set_source_u32(cr, 0x0f3a0f80);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		int off = pressed ? 1 : 0;
		double flag = b.height * 0.58;
		pd_glyph_xp_flag(cr, b.x + 9 + off, b.y + (b.height - flag) / 2.0 + off, flag);
		const char *font = tw_theme_str(t, "start.font", "Trebuchet MS, Noto Sans Bold Italic 13");
		double tx = b.x + 13 + flag + off;
		// a soft dark green shadow under the white letters, as on XP
		pd_text(cr, font, label, tx + 1, b.y + 2 + off, b.width - (tx - b.x), b.height,
			0x0a3a0a50, PD_LEFT);
		pd_text(cr, font, label, tx + 1, b.y + 1 + off, b.width - (tx - b.x), b.height,
			0x1a4a1aa0, PD_LEFT);
		pd_text(cr, font, label, tx, b.y + off, b.width - (tx - b.x), b.height, fg, PD_LEFT);
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
	list_add(items, menu_item_new("Task Manager", "exec $taskmanager"));
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
		if (!surface && !entry) {
			// a command without an icon of its own
			surface = apps_icon(ctx->panel, "application-x-executable", icon * ctx->surface->scale);
		}
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
