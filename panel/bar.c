#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"

struct bar {
	struct panel_output *output;
	double px, py;
	bool inside;
	bool pressed;
};

static int default_height(enum pstyle style) {
	switch (style) {
	case PS_CLASSIC:
		return 28;
	case PS_LUNA:
		return 30;
	case PS_AERO:
	case PS_FLAT:
		return 40;
	case PS_FLUENT:
		return 48;
	}
	return 40;
}

int bar_height(struct panel *panel) {
	struct layout_config *layout = &panel->config->layouts[panel->layout];
	if (layout->height > 0) {
		return layout->height;
	}
	return tw_theme_int(panel->theme, "panel.height", default_height(panel_style(panel)));
}

const char *bar_font(struct panel *panel) {
	if (panel->config && panel->config->font) {
		return panel->config->font;
	}
	return tw_theme_str(panel->theme, "panel.font", "Noto Sans 9");
}

const char *bar_bold_font(struct panel *panel) {
	return tw_theme_str(panel->theme, "panel.bold_font", bar_font(panel));
}

uint32_t bar_fg(struct panel *panel) {
	enum pstyle style = panel_style(panel);
	return tw_theme_color(panel->theme, "panel.fg",
		style == PS_CLASSIC || style == PS_FLUENT ? 0x000000ff : 0xffffffff);
}

bool render_hover(struct render_ctx *ctx, struct pbox box) {
	return ctx->pointer_inside && pbox_contains(&box, ctx->px, ctx->py);
}

bool render_pressed(struct render_ctx *ctx, struct pbox box) {
	return ctx->pressed && render_hover(ctx, box);
}

int render_text_width(struct render_ctx *ctx, const char *font, const char *text) {
	int w = 0;
	pd_text_size(ctx->cairo, font, text, &w, NULL);
	return w;
}

void render_item_bg(struct render_ctx *ctx, struct pbox b, bool active, bool hover,
		bool pressed) {
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	switch (ctx->style) {
	case PSV_CLASSIC:
		if (active || pressed) {
			pd_bevel(cr, b.x, b.y, b.width, b.height, true);
		}
		break;
	case PSV_LUNA:
		if (hover || active) {
			pd_rounded(cr, b.x + 1, b.y + 2, b.width - 2, b.height - 4, 3);
			cairo_set_source_u32(cr, active ? 0x00000030 : 0xffffff28);
			cairo_fill(cr);
		}
		break;
	case PSV_AERO:
		if (hover || active || pressed) {
			pd_rounded(cr, b.x + 1.5, b.y + 1.5, b.width - 3, b.height - 3, 3);
			pd_source(cr, t, pressed ? "taskbar.active" : "taskbar.hover", b.y, b.height,
				0xffffff30);
			cairo_fill_preserve(cr);
			cairo_set_source_u32(cr, 0xffffff50);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
		break;
	case PSV_FLAT:
		if (hover || active || pressed) {
			pd_rect(cr, b.x, b.y, b.width, b.height, tw_theme_color(t,
				pressed || active ? "taskbar.active_bg" : "taskbar.hover_bg",
				pressed || active ? 0xffffff29 : 0xffffff1f));
		}
		break;
	case PSV_FLUENT:
		if (hover || active || pressed) {
			pd_rounded(cr, b.x + 2, b.y + 4, b.width - 4, b.height - 8, 4);
			cairo_set_source_u32(cr, tw_theme_color(t,
				pressed || active ? "taskbar.active_bg" : "taskbar.hover_bg",
				pressed || active ? 0x00000014 : 0x0000000d));
			cairo_fill(cr);
		}
		break;
	}
}

static bool is_system_widget(struct widget *w) {
	static const char *types[] = { "tray", "clock", "volume", "network", "battery",
		"keyboard", "cpu", "memory", "brightness", "custom" };
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (strcmp(w->impl->type, types[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void draw_background(struct render_ctx *ctx, int width, int height) {
	cairo_t *cr = ctx->cairo;
	const struct tw_theme *t = ctx->panel->theme;
	cairo_rectangle(cr, 0, 0, width, height);
	uint32_t fallback = ctx->style == PSV_CLASSIC ? 0xc0c0c0ff :
		ctx->style == PSV_FLUENT ? 0xeeeeeef2 : 0x101010f0;
	pd_fill(cr, t, "panel.bg", 0, height, fallback);
	int edge_y = ctx->bottom ? 0 : height - 1;
	int dir = ctx->bottom ? 1 : -1;
	uint32_t line1 = tw_theme_color(t, "panel.border_top",
		ctx->style == PSV_CLASSIC ? 0xdfdfdfff : 0);
	uint32_t line2 = tw_theme_color(t, "panel.border_top2",
		ctx->style == PSV_CLASSIC ? 0xffffffff : 0);
	pd_rect(cr, 0, edge_y, width, 1, line1);
	pd_rect(cr, 0, edge_y + dir, width, 1, line2);
}

struct placed {
	struct widget *widget;
	int width;
};

static void layout_section(struct render_ctx *ctx, list_t *widgets, list_t *out,
		int *fixed, int *expanders) {
	for (int i = 0; widgets && i < widgets->length; i++) {
		struct widget *w = widgets->items[i];
		if (!w->active) {
			w->active = true;
			if (w->impl->set_active) {
				w->impl->set_active(w, true);
			}
		}
		int width = w->impl->measure ? w->impl->measure(w, ctx) : 0;
		if (width == 0) {
			continue;
		}
		struct placed *p = calloc(1, sizeof(*p));
		p->widget = w;
		p->width = width;
		if (width < 0) {
			(*expanders)++;
		} else {
			*fixed += width;
		}
		list_add(out, p);
	}
}

static void render_group_background(struct render_ctx *ctx, list_t *placed, int start_x) {
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		return;
	}
	int x = start_x, group_start = -1, group_end = -1;
	for (int i = 0; i < placed->length; i++) {
		struct placed *p = placed->items[i];
		if (is_system_widget(p->widget)) {
			if (group_start < 0) {
				group_start = x;
			}
			group_end = x + p->width;
		}
		x += p->width;
	}
	if (group_start < 0) {
		return;
	}
	if (ctx->style == PSV_LUNA && group_end + 8 >= x - 48) {
		// the XP notification area runs to the end of the taskbar, including
		// buttons such as show desktop that follow it
		group_end = ctx->surface->width - 8;
	}
	cairo_t *cr = ctx->cairo;
	int h = ctx->height;
	if (ctx->style == PSV_CLASSIC) {
		pd_border_sunken_thin(cr, group_start - 2, 4, group_end - group_start + 4, h - 7);
	} else {
		cairo_rectangle(cr, group_start - 8, 0, group_end - group_start + 16, h);
		pd_fill(cr, ctx->panel->theme, "tray.bg", 0, h, 0x0f8de6ff);
		pd_rect(cr, group_start - 8, 0, 1, h,
			tw_theme_color(ctx->panel->theme, "tray.border", 0x1047a8ff));
		pd_rect(cr, group_start - 7, 0, 1, h, 0xffffff30);
	}
}

static void bar_render(struct psurface *s, cairo_t *cr) {
	struct bar *bar = s->data;
	struct panel *panel = s->panel;
	struct layout_config *layout = &panel->config->layouts[panel->layout];
	struct render_ctx ctx = {
		.panel = panel,
		.surface = s,
		.output = bar->output,
		.cairo = cr,
		.height = s->height,
		.style = (enum pstyle_value)panel_style(panel),
		.bottom = layout->bottom,
		.pointer_inside = bar->inside,
		.pressed = bar->pressed,
		.px = bar->px,
		.py = bar->py,
	};
	draw_background(&ctx, s->width, s->height);
	// background hotspot for the empty area (right-click menu)
	psurface_add_hotspot(s, 0, 0, s->width, s->height, NULL, 0, 0, NULL);

	bool center_left = !layout->center->length && strcmp(tw_theme_str(panel->theme,
		"panel.alignment", "left"), "center") == 0;
	list_t *left = create_list(), *center = create_list(), *right = create_list();
	int left_fixed = 0, center_fixed = 0, right_fixed = 0;
	int left_exp = 0, center_exp = 0, right_exp = 0;
	ctx.centered = center_left;
	layout_section(&ctx, layout->left, left, &left_fixed, &left_exp);
	ctx.centered = false;
	layout_section(&ctx, layout->center, center, &center_fixed, &center_exp);
	layout_section(&ctx, layout->right, right, &right_fixed, &right_exp);

	int total_exp = left_exp + center_exp + right_exp;
	int remaining = s->width - left_fixed - center_fixed - right_fixed;
	int exp_width = total_exp > 0 && remaining > 0 ? remaining / total_exp : 0;

	int left_x = 0;
	if (center_left) {
		int group = left_fixed + left_exp * exp_width;
		left_x = (s->width - group) / 2;
		if (left_x + group > s->width - right_fixed) {
			left_x = s->width - right_fixed - group;
		}
		if (left_x < 0) {
			left_x = 0;
		}
	}

	struct {
		list_t *list;
		int x;
	} sections[3] = {
		{ left, left_x },
		{ center, 0 },
		{ right, 0 },
	};
	int center_width = center_fixed + center_exp * exp_width;
	sections[1].x = (s->width - center_width) / 2;
	int right_width = right_fixed + right_exp * exp_width;
	sections[2].x = s->width - right_width;

	render_group_background(&ctx, right, sections[2].x);
	for (int sec = 0; sec < 3; sec++) {
		int x = sections[sec].x;
		for (int i = 0; i < sections[sec].list->length; i++) {
			struct placed *p = sections[sec].list->items[i];
			int w = p->width < 0 ? exp_width : p->width;
			struct pbox box = { x, 0, w, s->height };
			cairo_save(cr);
			cairo_rectangle(cr, box.x, box.y, box.width, box.height);
			cairo_clip(cr);
			if (p->widget->impl->render) {
				p->widget->impl->render(p->widget, &ctx, box);
			}
			cairo_restore(cr);
			x += w;
			free(p);
		}
		list_free(sections[sec].list);
	}
}

static void bar_update_hover(struct psurface *s) {
	struct bar *bar = s->data;
	struct hotspot *hs = bar->inside ? psurface_hotspot_at(s, bar->px, bar->py) : NULL;
	if (hs && hs->widget) {
		tooltip_schedule(s->panel, s, hs);
	} else {
		tooltip_cancel(s->panel);
	}
}

static void bar_pointer_motion(struct psurface *s, double x, double y) {
	struct bar *bar = s->data;
	struct hotspot *before = bar->inside ? psurface_hotspot_at(s, bar->px, bar->py) : NULL;
	bar->px = x;
	bar->py = y;
	bar->inside = true;
	struct hotspot *after = psurface_hotspot_at(s, x, y);
	bool changed = !before || !after || before->widget != after->widget ||
		before->id != after->id || before->kind != after->kind ||
		before->box.x != after->box.x;
	if (changed) {
		psurface_set_dirty(s);
		bar_update_hover(s);
	}
}

static void bar_pointer_leave(struct psurface *s) {
	struct bar *bar = s->data;
	bar->inside = false;
	bar->pressed = false;
	tooltip_cancel(s->panel);
	psurface_set_dirty(s);
}

void bar_run_command(struct panel *panel, const char *command, const char *context_id) {
	if (!command || !*command) {
		return;
	}
	char *cmd = strdup(command);
	if (context_id) {
		char *pos;
		while ((pos = strstr(cmd, "{id}"))) {
			char *next = format_str("%.*s%s%s", (int)(pos - cmd), cmd, context_id, pos + 4);
			free(cmd);
			cmd = next;
		}
	}
	if (strncmp(cmd, "panel ", 6) == 0) {
		bar_handle_panel_command(panel, cmd + 6);
	} else {
		ipc_panel_command(panel, cmd);
	}
	free(cmd);
}

static list_t *default_taskbar_menu(struct panel *panel) {
	list_t *items = panel_named_menu(panel, "taskbar");
	if (items) {
		return items;
	}
	items = create_list();
	bool window_mode = panel->layout == LAYOUT_WINDOW;
	list_add(items, menu_item_new("Cascade windows", "arrange cascade"));
	list_add(items, menu_item_new("Show windows stacked", "arrange vertical"));
	list_add(items, menu_item_new("Show windows side by side", "arrange horizontal"));
	list_add(items, menu_item_new("Arrange windows optimally", "arrange optimal"));
	list_add(items, menu_item_separator());
	if (window_mode) {
		list_add(items, menu_item_new("Show the desktop", "showdesktop"));
	}
	list_add(items, menu_item_new(window_mode ? "Switch to tile mode" :
		"Switch to window mode", "wm_mode toggle"));
	list_add(items, menu_item_separator());
	list_add(items, menu_item_new("Taskbar settings",
		"exec tilewin-settings --page taskbar"));
	list_add(items, menu_item_new("Restart taskbar", "restart panel"));
	return items;
}

static void bar_pointer_button(struct psurface *s, double x, double y,
		uint32_t button, bool pressed) {
	struct bar *bar = s->data;
	struct panel *panel = s->panel;
	bar->px = x;
	bar->py = y;
	bar->pressed = pressed;
	psurface_set_dirty(s);
	if (pressed) {
		tooltip_cancel(panel);
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(s, x, y);
	if (!hs) {
		return;
	}
	struct widget *w = hs->widget;
	if (!w) {
		if (button == BTN_RIGHT) {
			struct popup_anchor anchor = popup_anchor_for_bar(s, (int)x, 0);
			menu_open(panel, default_taskbar_menu(panel), true, anchor, NULL);
		}
		return;
	}
	const char *cmd = button == BTN_LEFT ? w->on_click :
		button == BTN_MIDDLE ? w->on_middle_click :
		button == BTN_RIGHT ? w->on_right_click : NULL;
	if (cmd) {
		bar_run_command(panel, cmd, NULL);
		return;
	}
	if (w->impl->click && w->impl->click(w, s, hs, button, x, y)) {
		return;
	}
	if (button == BTN_RIGHT) {
		struct popup_anchor anchor = popup_anchor_for_bar(s, (int)x, 0);
		if (w->menu) {
			menu_open(panel, w->menu, false, anchor, NULL);
		} else {
			menu_open(panel, default_taskbar_menu(panel), true, anchor, NULL);
		}
	}
}

static void bar_pointer_axis(struct psurface *s, double x, double y, int direction) {
	struct hotspot *hs = psurface_hotspot_at(s, x, y);
	if (!hs || !hs->widget) {
		return;
	}
	struct widget *w = hs->widget;
	const char *cmd = direction < 0 ? w->on_scroll_up : w->on_scroll_down;
	if (cmd) {
		bar_run_command(s->panel, cmd, NULL);
	} else if (w->impl->scroll) {
		w->impl->scroll(w, s, hs, direction);
	}
}

static void bar_closed(struct psurface *s) {
	struct bar *bar = s->data;
	bar->output->bar = NULL;
	free(bar);
	psurface_destroy(s);
}

static const struct psurface_impl bar_impl = {
	.render = bar_render,
	.pointer_motion = bar_pointer_motion,
	.pointer_leave = bar_pointer_leave,
	.pointer_button = bar_pointer_button,
	.pointer_axis = bar_pointer_axis,
	.closed = bar_closed,
};

struct popup_anchor popup_anchor_for_bar(struct psurface *bar_surface, int x, int width) {
	struct panel *panel = bar_surface->panel;
	struct bar *bar = bar_surface->data;
	bool bottom = panel->config->layouts[panel->layout].bottom;
	struct popup_anchor anchor = {
		.output = bar->output,
		.x = x,
		.y = bottom ? bar->output->height - bar_surface->height : bar_surface->height,
		.above = bottom,
	};
	(void)width;
	return anchor;
}

void bar_create(struct panel_output *output) {
	struct panel *panel = output->panel;
	if (output->bar) {
		return;
	}
	struct layout_config *layout = &panel->config->layouts[panel->layout];
	if (!layout->left->length && !layout->center->length && !layout->right->length) {
		return;
	}
	struct bar *bar = calloc(1, sizeof(*bar));
	bar->output = output;
	struct psurface *s = psurface_create(panel, output, &bar_impl, bar,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "tilewin-panel");
	int height = bar_height(panel);
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
		(layout->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
	psurface_set_size(s, 0, height);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, height);
	wl_surface_commit(s->surface);
	output->bar = s;

	// deactivate widgets that are not part of the active layout
	for (int i = 0; i < panel->config->widgets->length; i++) {
		struct widget *w = panel->config->widgets->items[i];
		bool used = list_find(layout->left, w) >= 0 || list_find(layout->center, w) >= 0 ||
			list_find(layout->right, w) >= 0;
		if (!used && w->active) {
			w->active = false;
			if (w->impl->set_active) {
				w->impl->set_active(w, false);
			}
		}
	}
}

void bar_destroy(struct panel_output *output) {
	if (!output->bar) {
		return;
	}
	struct psurface *s = output->bar;
	struct bar *bar = s->data;
	output->bar = NULL;
	free(bar);
	psurface_destroy(s);
}

void bar_handle_panel_command(struct panel *panel, const char *args) {
	int argc = 0;
	char **argv = split_args(args, &argc);
	if (argc == 0) {
		free_argv(argc, argv);
		return;
	}
	const char *cmd = argv[0];
	struct panel_output *output = panel_focused_output(panel);
	if (strcmp(cmd, "startmenu") == 0) {
		const char *action = argc > 1 ? argv[1] : "toggle";
		bool open = popup_is_open(panel, POPUP_STARTMENU);
		if (strcmp(action, "close") == 0 || (strcmp(action, "toggle") == 0 && open)) {
			popup_close_all(panel);
		} else if (output && (!open || strcmp(action, "search") == 0)) {
			startmenu_toggle(panel, output, strcmp(action, "search") == 0);
		}
	} else if (strcmp(cmd, "launcher") == 0) {
		if (output) {
			launcher_toggle(panel, output);
		}
	} else if (strcmp(cmd, "desktop") == 0) {
		desktop_handle_command(panel, argc - 1, argv + 1);
	} else if (strcmp(cmd, "network") == 0 && output) {
		flyout_network_toggle(panel, flyout_anchor(panel, output), NULL);
	} else if (strcmp(cmd, "volume") == 0 && output) {
		flyout_volume_toggle(panel, flyout_anchor(panel, output), NULL);
	} else if (strcmp(cmd, "power") == 0 && output) {
		flyout_power_toggle(panel, flyout_anchor(panel, output), NULL);
	} else if (strcmp(cmd, "run") == 0) {
		if (output) {
			rundialog_open(panel, output);
		}
	} else if (strcmp(cmd, "activate") == 0 && argc > 1) {
		taskbar_activate_index(panel, atoi(argv[1]));
	} else if (strcmp(cmd, "window_menu") == 0 && output) {
		int64_t id = argc > 1 ? atoll(argv[1]) : panel->state.focused_window;
		struct pwindow *win = panel_find_window(panel, id);
		if (win) {
			struct popup_anchor anchor = {
				.output = output,
				.x = argc > 3 ? atoi(argv[2]) : output->width / 2,
				.y = argc > 3 ? atoi(argv[3]) : output->height / 3,
			};
			menu_open(panel, taskbar_window_menu(panel, win), true, anchor, NULL);
		}
	} else if (strcmp(cmd, "menu") == 0 && argc > 1 && output) {
		list_t *items = panel_named_menu(panel, argv[1]);
		if (items) {
			struct popup_anchor anchor = {
				.output = output,
				.x = output->width / 2,
				.y = output->height / 2,
			};
			menu_open(panel, items, true, anchor, NULL);
		}
	} else if (strcmp(cmd, "calendar") == 0 && output && output->bar) {
		struct popup_anchor anchor = popup_anchor_for_bar(output->bar, output->width, 0);
		anchor.right_align = true;
		calendar_toggle(panel, anchor);
	} else if (strcmp(cmd, "reload") == 0) {
		panel_request_reload(panel);
	} else if (strcmp(cmd, "close") == 0) {
		popup_close_all(panel);
	} else {
		sway_log(SWAY_ERROR, "Unknown panel command '%s'", args);
	}
	free_argv(argc, argv);
}
