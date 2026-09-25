#include <ctype.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"

/* ================= separator / spacer ================= */

static int separator_measure(struct widget *w, struct render_ctx *ctx) {
	return widget_conf_int(w, "width", 8);
}

static void separator_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	double x = b.x + b.width / 2;
	if (ctx->style == PSV_CLASSIC) {
		pd_rect(ctx->cairo, x - 1, b.y + 5, 1, b.height - 9, 0x808080ff);
		pd_rect(ctx->cairo, x, b.y + 5, 1, b.height - 9, 0xffffffff);
	} else {
		uint32_t fg = bar_fg(ctx->panel);
		pd_rect(ctx->cairo, x, b.y + b.height * 0.25, 1, b.height * 0.5,
			(fg & 0xffffff00) | 0x40);
	}
}

const struct widget_impl widget_separator = {
	.type = "separator",
	.measure = separator_measure,
	.render = separator_render,
};

static int spacer_measure(struct widget *w, struct render_ctx *ctx) {
	const char *width = widget_conf(w, "width", "8");
	if (strcasecmp(width, "expand") == 0) {
		return ctx->centered ? 0 : -1;
	}
	return atoi(width);
}

const struct widget_impl widget_spacer = {
	.type = "spacer",
	.measure = spacer_measure,
};

/* ================= clock ================= */

struct clock_data {
	struct loop_timer *timer;
	char text[128];
	bool seconds;
};

static void clock_update(struct widget *w) {
	struct clock_data *d = w->data;
	const char *format = widget_conf(w, "format",
		tw_theme_str(w->panel->theme, "clock.format", "%H:%M"));
	d->seconds = strstr(format, "%S") || strstr(format, "%T") || strstr(format, "%s");
	if (w->desk) {
		// the second hand of the analog clock, the last LEDs of the binary one
		const char *style = twconf_value(w->desk, "style");
		bool second_style = style && (strcmp(style, "analog") == 0 ||
			strcmp(style, "binary") == 0);
		d->seconds = d->seconds || widget_conf_bool(w, "seconds", second_style);
	}
	char *unescaped = strdup(format);
	// allow a literal "\n" in the config for two-line clocks
	char *p;
	while ((p = strstr(unescaped, "\\n"))) {
		p[0] = '\n';
		memmove(p + 1, p + 2, strlen(p + 2) + 1);
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(d->text, sizeof(d->text), unescaped, &tm);
	free(unescaped);
}

static void clock_tick(void *data) {
	struct widget *w = data;
	struct clock_data *d = w->data;
	d->timer = NULL;
	if (!w->active) {
		return;
	}
	clock_update(w);
	widget_set_dirty(w);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	int ms = d->seconds ? 1000 - ts.tv_nsec / 1000000 :
		(60 - ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000;
	d->timer = loop_add_timer(w->panel->loop, ms + 5, clock_tick, w);
}

static void clock_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct clock_data));
	clock_update(w);
}

static void clock_set_active(struct widget *w, bool active) {
	struct clock_data *d = w->data;
	if (active && !d->timer) {
		clock_tick(w);
	} else if (!active && d->timer) {
		loop_remove_timer(w->panel->loop, d->timer);
		d->timer = NULL;
	}
}

static void clock_destroy(struct widget *w) {
	struct clock_data *d = w->data;
	if (d->timer) {
		loop_remove_timer(w->panel->loop, d->timer);
	}
	free(d);
}

static int clock_measure(struct widget *w, struct render_ctx *ctx) {
	struct clock_data *d = w->data;
	int tw = 0;
	pd_text_size(ctx->cairo, bar_font(ctx->panel), d->text, &tw, NULL);
	return tw + (ctx->style == PSV_CLASSIC ? 12 : 20);
}

static void clock_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct clock_data *d = w->data;
	bool hover = render_hover(ctx, b);
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, hover, render_pressed(ctx, b));
	}
	const char *align = tw_theme_str(ctx->panel->theme, "clock.align", "center");
	pd_text(ctx->cairo, bar_font(ctx->panel), d->text, b.x + 6, b.y, b.width - 12, b.height,
		tw_theme_color(ctx->panel->theme, "clock.fg", bar_fg(ctx->panel)),
		strcmp(align, "right") == 0 ? PD_RIGHT : PD_CENTER);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool clock_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	calendar_toggle(w->panel, anchor, widget_conf(w, "settings", NULL));
	return true;
}

static char *clock_tooltip(struct widget *w, struct hotspot *hs) {
	const char *format = widget_conf(w, "tooltip_format",
		tw_theme_str(w->panel->theme, "clock.tooltip_format", "%A, %d %B %Y"));
	char buf[256];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), format, &tm);
	return strdup(buf);
}

const struct widget_impl widget_clock = {
	.type = "clock",
	.init = clock_init,
	.destroy = clock_destroy,
	.measure = clock_measure,
	.render = clock_render,
	.click = clock_click,
	.tooltip = clock_tooltip,
	.set_active = clock_set_active,
};

/* ================= focused window title ================= */

static const char *focused_title(struct panel *panel) {
	struct pwindow *win = panel_find_window(panel, panel->state.focused_window);
	return win ? win->title : "";
}

static int title_measure(struct widget *w, struct render_ctx *ctx) {
	const char *title = focused_title(ctx->panel);
	if (!title || !*title) {
		return 0; // nothing focused: no room taken, and no empty card on the desktop
	}
	int tw = render_text_width(ctx, bar_font(ctx->panel), title);
	int max = widget_conf_int(w, "max_width", 480);
	return (tw > max ? max : tw) + 16;
}

static void title_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct pwindow *win = panel_find_window(ctx->panel, ctx->panel->state.focused_window);
	if (win && render_hover(ctx, b)) {
		render_item_bg(ctx, b, false, true, render_pressed(ctx, b));
	}
	pd_text(ctx->cairo, bar_font(ctx->panel), focused_title(ctx->panel), b.x + 8, b.y,
		b.width - 16, b.height, bar_fg(ctx->panel), PD_LEFT);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool title_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT || !panel_find_window(w->panel, w->panel->state.focused_window)) {
		return false;
	}
	// the window: where it is, and minimize, maximize and close
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x, 0);
	info_flyout_toggle(w, anchor);
	return true;
}

const struct widget_impl widget_title = {
	.type = "title",
	.measure = title_measure,
	.render = title_render,
	.click = title_click,
};

/* ================= workspaces ================= */

/* Label of a workspace: "workspaces { icon { 1 <icon>; urgent ! } }" or its name. */
static const char *workspace_label(struct render_ctx *ctx, struct pworkspace *ws) {
	const struct tw_theme *t = ctx->panel->theme;
	if (ws->urgent) {
		const char *urgent = tw_theme_str(t, "workspaces.icon.urgent", NULL);
		if (urgent) {
			return urgent;
		}
	}
	char key[256];
	snprintf(key, sizeof(key), "workspaces.icon.%s", ws->name);
	// a desktop given a name is called "2:Work": the number keeps its place in
	// the order, the button shows what it was called
	const char *colon = strchr(ws->name, ':');
	const char *shown = colon && colon[1] ? colon + 1 : ws->name;
	return tw_theme_str(t, key, shown);
}

static int workspace_button_width(struct render_ctx *ctx, struct pworkspace *ws) {
	return render_text_width(ctx, bar_bold_font(ctx->panel), workspace_label(ctx, ws)) +
		2 * tw_theme_int(ctx->panel->theme, "workspaces.padding", 8);
}

static int workspaces_measure(struct widget *w, struct render_ctx *ctx) {
	int total = 0;
	list_t *wss = ctx->panel->state.workspaces;
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		if (ctx->output->name && strcmp(ws->output, ctx->output->name) == 0) {
			total += workspace_button_width(ctx, ws) + 2;
		}
	}
	// "workspaces.margin": space around the buttons, like waybar's module padding
	int margin = tw_theme_int(ctx->panel->theme, "workspaces.margin", 0);
	return total ? total + 2 + 2 * margin : 0;
}

static void workspaces_render(struct widget *w, struct render_ctx *ctx, struct pbox box) {
	cairo_t *cr = ctx->cairo;
	list_t *wss = ctx->panel->state.workspaces;
	int x = box.x + 2 + tw_theme_int(ctx->panel->theme, "workspaces.margin", 0);
	for (int i = 0; wss && i < wss->length; i++) {
		struct pworkspace *ws = wss->items[i];
		if (!ctx->output->name || strcmp(ws->output, ctx->output->name) != 0) {
			continue;
		}
		int bw = workspace_button_width(ctx, ws);
		struct pbox b = { x, box.y, bw, box.height };
		bool hover = render_hover(ctx, b);
		const struct tw_theme *t = ctx->panel->theme;
		uint32_t fg = widget_fg(ctx->panel, "workspaces");
		bool pill = strcmp(tw_theme_str(t, "workspaces.style", "default"), "pill") == 0;
		if (pill) {
			// rounded buttons like waybar's sway/workspaces
			int inset = tw_theme_int(t, "workspaces.inset", 6);
			uint32_t bg = ws->urgent ? tw_theme_color(t, "workspaces.urgent_bg", 0xe8112338) :
				ws->focused ? tw_theme_color(t, "workspaces.active_bg", 0xffffff29) :
				hover ? tw_theme_color(t, "workspaces.hover_bg", 0xffffff14) : 0;
			if (bg) {
				pd_rounded(cr, b.x, b.y + inset, b.width, b.height - 2 * inset,
					tw_theme_int(t, "workspaces.radius", 10));
				pd_color(cr, bg);
				cairo_fill(cr);
			}
			if (ws->urgent) {
				fg = tw_theme_color(t, "workspaces.urgent_fg", fg);
			} else if (ws->focused) {
				fg = tw_theme_color(t, "workspaces.active_fg", fg);
			}
		} else if (ctx->style == PSV_CLASSIC) {
			struct pbox bb = { b.x, b.y + 3, b.width, b.height - 5 };
			pd_rect(cr, bb.x, bb.y, bb.width, bb.height, ws->visible ? 0xe0e0e0ff : 0xc0c0c0ff);
			pd_bevel(cr, bb.x, bb.y, bb.width, bb.height, ws->visible);
		} else {
			render_item_bg(ctx, b, ws->visible, hover, render_pressed(ctx, b));
			if (ws->focused) {
				pd_rect(cr, b.x, ctx->bottom ? b.y + b.height - 2 : b.y, b.width, 2,
					tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x76b9edff));
			}
		}
		if (ws->urgent && !pill) {
			pd_rect(cr, b.x + 2, b.y + 2, b.width - 4, 2, 0xe81123ff);
		}
		pd_text(cr, ws->focused ? bar_bold_font(ctx->panel) : bar_font(ctx->panel),
			workspace_label(ctx, ws), b.x, b.y, b.width, b.height, fg, PD_CENTER);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, i, ws->name);
		x += bw + 2;
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

const struct widget_impl widget_workspaces = {
	.type = "workspaces",
	.measure = workspaces_measure,
	.render = workspaces_render,
	.click = workspaces_click,
	.scroll = workspaces_scroll,
};

/* ================= keyboard layout ================= */

static void layout_short_name(const char *layout, char *out, size_t size) {
	if (!layout || !*layout) {
		snprintf(out, size, "--");
		return;
	}
	// "English (US)" -> "ENG", "German" -> "DEU" style: first 3 letters
	size_t n = 0;
	for (const char *p = layout; *p && n < 3 && n + 1 < size; p++) {
		if (isalpha((unsigned char)*p)) {
			out[n++] = toupper((unsigned char)*p);
		} else if (n > 0) {
			break;
		}
	}
	out[n] = '\0';
}

static int keyboard_measure(struct widget *w, struct render_ctx *ctx) {
	if (!ctx->panel->state.keyboard_layout) {
		return 0;
	}
	char name[8];
	layout_short_name(ctx->panel->state.keyboard_layout, name, sizeof(name));
	return render_text_width(ctx, bar_font(ctx->panel), name) + 16;
}

static void keyboard_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	char name[8];
	layout_short_name(ctx->panel->state.keyboard_layout, name, sizeof(name));
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	pd_text(ctx->cairo, bar_font(ctx->panel), name, b.x, b.y, b.width, b.height,
		bar_fg(ctx->panel), PD_CENTER);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool keyboard_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button == BTN_MIDDLE) {
		// the next layout straight away, as a click did before there was a flyout
		ipc_panel_command(w->panel, "input type:keyboard xkb_switch_layout next");
		return true;
	}
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
	return true;
}

static char *keyboard_tooltip(struct widget *w, struct hotspot *hs) {
	return w->panel->state.keyboard_layout ? strdup(w->panel->state.keyboard_layout) : NULL;
}

const struct widget_impl widget_keyboard = {
	.type = "keyboard",
	.measure = keyboard_measure,
	.render = keyboard_render,
	.click = keyboard_click,
	.tooltip = keyboard_tooltip,
};

/* ================= mode switch ================= */

static int modeswitch_measure(struct widget *w, struct render_ctx *ctx) {
	return ctx->height < 32 ? 28 : 40;
}

static void modeswitch_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	bool tile = ctx->panel->layout == LAYOUT_TILE;
	bool hover = render_hover(ctx, b);
	if (ctx->style == PSV_CLASSIC) {
		if (hover) {
			pd_bevel(ctx->cairo, b.x + 2, b.y + 3, b.width - 4, b.height - 6,
				render_pressed(ctx, b));
		}
	} else {
		render_item_bg(ctx, b, false, hover, render_pressed(ctx, b));
	}
	double size = b.height < 32 ? 16 : 20;
	double gx = b.x + (b.width - size) / 2, gy = b.y + (b.height - size) / 2;
	if (tile) {
		pd_glyph_tile(ctx->cairo, gx, gy, size, bar_fg(ctx->panel));
	} else {
		pd_glyph_overlap(ctx->cairo, gx, gy, size, bar_fg(ctx->panel));
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool modeswitch_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	ipc_panel_command(w->panel, "wm_mode toggle");
	return true;
}

static char *modeswitch_tooltip(struct widget *w, struct hotspot *hs) {
	return strdup(w->panel->layout == LAYOUT_TILE ?
		"Tile mode - click to switch to window mode" :
		"Window mode - click to switch to tile mode");
}

const struct widget_impl widget_modeswitch = {
	.type = "modeswitch",
	.measure = modeswitch_measure,
	.render = modeswitch_render,
	.click = modeswitch_click,
	.tooltip = modeswitch_tooltip,
};

/* ================= show desktop ================= */

static int showdesktop_measure(struct widget *w, struct render_ctx *ctx) {
	int fallback = ctx->style == PSV_AERO ? 15 : ctx->style == PSV_FLUENT ? 10 :
		ctx->style == PSV_FLAT ? 8 : 24;
	return widget_conf_int(w, "width", fallback);
}

static void showdesktop_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	cairo_t *cr = ctx->cairo;
	bool hover = render_hover(ctx, b);
	switch (ctx->style) {
	case PSV_CLASSIC:
	case PSV_LUNA:
		if (hover) {
			if (ctx->style == PSV_CLASSIC) {
				pd_bevel(cr, b.x + 1, b.y + 4, b.width - 2, b.height - 7, render_pressed(ctx, b));
			} else {
				render_item_bg(ctx, b, false, true, render_pressed(ctx, b));
			}
		}
		// small desktop glyph
		pd_rect(cr, b.x + b.width / 2 - 7, b.y + b.height / 2 - 6, 14, 10, 0x3a6ea5ff);
		pd_rect(cr, b.x + b.width / 2 - 6, b.y + b.height / 2 - 5, 12, 8, 0x5fa0e0ff);
		pd_rect(cr, b.x + b.width / 2 - 3, b.y + b.height / 2 + 5, 6, 2, 0x404040ff);
		break;
	case PSV_AERO:
		cairo_rectangle(cr, b.x + 1, b.y, b.width - 1, b.height);
		cairo_set_source_u32(cr, hover ? 0xffffff50 : 0xffffff18);
		cairo_fill(cr);
		pd_rect(cr, b.x, b.y, 1, b.height, 0xffffff40);
		break;
	default:
		pd_rect(cr, b.x, b.y + b.height * 0.25, 1, b.height * 0.5,
			(bar_fg(ctx->panel) & 0xffffff00) | 0x50);
		if (hover) {
			pd_rect(cr, b.x + 1, b.y, b.width - 1, b.height,
				ctx->style == PSV_FLUENT ? 0x0000000d : 0xffffff1f);
		}
		break;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool showdesktop_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	ipc_panel_command(w->panel, "showdesktop");
	return true;
}

static char *showdesktop_tooltip(struct widget *w, struct hotspot *hs) {
	return strdup("Show desktop");
}

const struct widget_impl widget_showdesktop = {
	.type = "showdesktop",
	.measure = showdesktop_measure,
	.render = showdesktop_render,
	.click = showdesktop_click,
	.tooltip = showdesktop_tooltip,
};

/* ================= search box ================= */

static int search_measure(struct widget *w, struct render_ctx *ctx) {
	if (ctx->style == PSV_FLAT) {
		return widget_conf_int(w, "width", tw_theme_int(ctx->panel->theme, "search.width", 300));
	}
	return widget_conf_int(w, "width", ctx->style == PSV_FLUENT ? 44 :
		ctx->style == PSV_CLASSIC || ctx->style == PSV_LUNA ? 0 : 40);
}

static void search_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	if (ctx->style == PSV_FLAT && b.width > 80) {
		bool hover = render_hover(ctx, b);
		pd_rect(cr, b.x, b.y, b.width, b.height, hover ?
			tw_theme_color(t, "search.hover_bg", 0xffffffff) :
			tw_theme_color(t, "search.bg", 0xf2f2f2ff));
		uint32_t fg = tw_theme_color(t, "search.fg", 0x6b6b6bff);
		pd_glyph_search(cr, b.x + 12, b.y + (b.height - 16) / 2.0, 16,
			tw_theme_color(t, "search.glyph", 0x000000ff));
		const char *label = widget_conf(w, "label",
			tw_theme_str(t, "search.label", "Type here to search"));
		pd_text(cr, bar_font(ctx->panel), label, b.x + 40, b.y, b.width - 48, b.height, fg, PD_LEFT);
	} else {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
		pd_glyph_search(cr, b.x + (b.width - 18) / 2.0, b.y + (b.height - 18) / 2.0, 18,
			bar_fg(ctx->panel));
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool search_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	if (popup_is_open(w->panel, POPUP_STARTMENU)) {
		popup_close_all(w->panel);
	} else {
		startmenu_toggle(w->panel, s->output, true);
	}
	return true;
}

static char *search_tooltip(struct widget *w, struct hotspot *hs) {
	return strdup("Search");
}

const struct widget_impl widget_search = {
	.type = "search",
	.measure = search_measure,
	.render = search_render,
	.click = search_click,
	.tooltip = search_tooltip,
};
