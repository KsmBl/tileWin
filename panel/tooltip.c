#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"

struct tooltip {
	char *text;
};

static const char *tooltip_font(struct panel *panel) {
	return tw_theme_str(panel->theme, "tooltip.font", bar_font(panel));
}

static void tooltip_render(struct psurface *s, cairo_t *cr) {
	struct tooltip *tip = s->data;
	struct panel *panel = s->panel;
	enum pstyle style = panel_style(panel);
	uint32_t bg = tw_theme_color(panel->theme, "tooltip.bg", 0xffffe1ff);
	uint32_t fg = tw_theme_color(panel->theme, "tooltip.fg", 0x000000ff);
	uint32_t border = tw_theme_color(panel->theme, "tooltip.border", 0x000000ff);
	double r = style == PS_FLUENT ? 6 : style == PS_AERO ? 3 : 0;
	pd_rounded(cr, 0.5, 0.5, s->width - 1, s->height - 1, r);
	pd_color(cr, bg);
	cairo_fill_preserve(cr);
	pd_color(cr, border);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	pd_text(cr, tooltip_font(panel), tip->text, 6, 0, s->width - 12, s->height, fg, PD_LEFT);
}

static void tooltip_closed(struct psurface *s) {
	struct panel *panel = s->panel;
	if (panel->tooltip == s) {
		panel->tooltip = NULL;
	}
	struct tooltip *tip = s->data;
	free(tip->text);
	free(tip);
	psurface_destroy(s);
}

static const struct psurface_impl tooltip_impl = {
	.render = tooltip_render,
	.closed = tooltip_closed,
};

static void destroy_tooltip_surface(struct panel *panel) {
	if (!panel->tooltip) {
		return;
	}
	struct psurface *s = panel->tooltip;
	panel->tooltip = NULL;
	struct tooltip *tip = s->data;
	free(tip->text);
	free(tip);
	psurface_destroy(s);
}

static void show_tooltip(void *data) {
	struct panel *panel = data;
	panel->tooltip_timer = NULL;
	struct psurface *source = panel->tooltip_source;
	struct hotspot *hs = &panel->tooltip_hotspot;
	if (!source || !hs->widget || !hs->widget->impl->tooltip || panel->popup) {
		return;
	}
	char *text = hs->widget->impl->tooltip(hs->widget, hs);
	if (!text || !*text) {
		free(text);
		return;
	}
	destroy_tooltip_surface(panel);

	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(scratch);
	int tw = 0, th = 0;
	pd_text_size(cr, tooltip_font(panel), text, &tw, &th);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
	int width = tw + 14, height = th + 8;
	if (width > 600) {
		width = 600;
	}

	struct panel_output *output = source->output;
	if (!output) {
		free(text);
		return;
	}
	bool bottom = panel->config->layouts[panel->layout].bottom;
	int x = hs->box.x + hs->box.width / 2 - width / 2;
	if (x + width > output->width - 2) {
		x = output->width - width - 2;
	}
	if (x < 2) {
		x = 2;
	}
	int y = bottom ? output->height - source->height - height - 6 : source->height + 6;

	struct tooltip *tip = calloc(1, sizeof(*tip));
	tip->text = text;
	struct psurface *s = psurface_create(panel, output, &tooltip_impl, tip,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-tooltip");
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(s->layer_surface, y, 0, 0, x);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	psurface_set_size(s, width, height);
	// tooltips never take input
	struct wl_region *region = wl_compositor_create_region(panel->compositor);
	wl_surface_set_input_region(s->surface, region);
	wl_region_destroy(region);
	wl_surface_commit(s->surface);
	panel->tooltip = s;
}

void tooltip_schedule(struct panel *panel, struct psurface *s, struct hotspot *hs) {
	struct hotspot *cur = &panel->tooltip_hotspot;
	if (panel->tooltip_source == s && cur->widget == hs->widget && cur->id == hs->id &&
			cur->kind == hs->kind && cur->box.x == hs->box.x) {
		return;
	}
	bool visible = panel->tooltip != NULL;
	tooltip_cancel(panel);
	panel->tooltip_source = s;
	*cur = *hs;
	cur->str = hs->str ? strdup(hs->str) : NULL;
	if (!hs->widget || !hs->widget->impl->tooltip) {
		return;
	}
	int delay = visible ? 50 : (panel->config ? panel->config->tooltip_delay : 600);
	if (delay >= 0) {
		panel->tooltip_timer = loop_add_timer(panel->loop, delay, show_tooltip, panel);
	}
}

void tooltip_cancel(struct panel *panel) {
	if (panel->tooltip_timer) {
		loop_remove_timer(panel->loop, panel->tooltip_timer);
		panel->tooltip_timer = NULL;
	}
	free(panel->tooltip_hotspot.str);
	memset(&panel->tooltip_hotspot, 0, sizeof(panel->tooltip_hotspot));
	panel->tooltip_source = NULL;
	destroy_tooltip_surface(panel);
}

void bar_hide_tooltip(struct panel *panel) {
	tooltip_cancel(panel);
}
