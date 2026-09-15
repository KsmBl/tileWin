/*
 * On-screen display shown when the volume, microphone, brightness or the
 * playing media changes, like on Windows. tilewin-media (run by the media
 * keys) asks for it with "panel osd volume 45 0" and similar commands.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "log.h"
#include "panel.h"

#define HIDE_MS 1800

enum osd_kind {
	OSD_VOLUME,
	OSD_MIC,
	OSD_BRIGHTNESS,
	OSD_MEDIA,
};

static struct {
	struct panel *panel;
	struct psurface *surface;
	enum osd_kind kind;
	int value; // percent
	bool muted;
	char status[32]; // media: Playing, Paused, Stopped or empty
	char artist[256];
	char title[256];
	struct loop_timer *hide_timer;
	struct proc *proc;
} osd;

static bool vertical(struct panel *panel) {
	return panel_style(panel) == PS_FLAT && osd.kind != OSD_MEDIA;
}

static void osd_size(struct panel *panel, int *width, int *height) {
	if (osd.kind == OSD_MEDIA) {
		*width = 380;
		*height = 76;
	} else if (vertical(panel)) {
		*width = 64;
		*height = 190;
	} else {
		*width = 280;
		*height = 52;
	}
}

static void osd_colors(struct panel *panel, uint32_t *bg, uint32_t *fg, uint32_t *accent,
		uint32_t *border) {
	const struct tw_theme *t = panel->theme;
	switch (panel_style(panel)) {
	case PS_FLAT:
		*bg = 0x1f1f1ff4;
		*fg = 0xffffffff;
		*accent = 0x0078d7ff;
		*border = 0x3c3c3cff;
		break;
	case PS_FLUENT:
		*bg = t->dark ? 0x2c2c2cf6 : 0xf3f3f3f6;
		*fg = t->dark ? 0xffffffff : 0x1b1b1bff;
		*accent = t->dark ? 0x60cdffff : 0x005fb8ff;
		*border = t->dark ? 0xffffff1c : 0x0000001c;
		break;
	default:
		*bg = tw_theme_color(t, "menu.bg", 0xffffffff);
		*fg = tw_theme_color(t, "menu.fg", 0x000000ff);
		*accent = tw_theme_color(t, "menu.hl_bg", 0x316ac5ff);
		*border = tw_theme_color(t, "menu.border", 0x808080ff);
		break;
	}
	*bg = tw_theme_color(t, "osd.bg", *bg);
	*fg = tw_theme_color(t, "osd.fg", *fg);
	*accent = tw_theme_color(t, "osd.accent", *accent);
	*border = tw_theme_color(t, "osd.border", *border);
}

static void draw_mic(cairo_t *cr, double x, double y, double s, bool muted, uint32_t color) {
	cairo_new_path(cr);
	pd_color(cr, color);
	pd_rounded(cr, x + s * 0.36, y + s * 0.08, s * 0.28, s * 0.5, s * 0.14);
	cairo_fill(cr);
	cairo_set_line_width(cr, s * 0.08);
	cairo_arc(cr, x + s * 0.5, y + s * 0.42, s * 0.26, 0, M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, x + s * 0.5, y + s * 0.68);
	cairo_line_to(cr, x + s * 0.5, y + s * 0.88);
	cairo_move_to(cr, x + s * 0.32, y + s * 0.9);
	cairo_line_to(cr, x + s * 0.68, y + s * 0.9);
	cairo_stroke(cr);
	if (muted) {
		cairo_set_line_width(cr, s * 0.1);
		cairo_move_to(cr, x + s * 0.12, y + s * 0.1);
		cairo_line_to(cr, x + s * 0.88, y + s * 0.9);
		cairo_stroke(cr);
	}
}

static void draw_media_glyph(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	pd_color(cr, color);
	if (strcmp(osd.status, "Playing") == 0) {
		cairo_rectangle(cr, x + s * 0.22, y + s * 0.15, s * 0.2, s * 0.7);
		cairo_rectangle(cr, x + s * 0.58, y + s * 0.15, s * 0.2, s * 0.7);
	} else if (strcmp(osd.status, "Paused") == 0) {
		cairo_move_to(cr, x + s * 0.25, y + s * 0.12);
		cairo_line_to(cr, x + s * 0.85, y + s * 0.5);
		cairo_line_to(cr, x + s * 0.25, y + s * 0.88);
		cairo_close_path(cr);
	} else {
		cairo_rectangle(cr, x + s * 0.2, y + s * 0.2, s * 0.6, s * 0.6);
	}
	cairo_fill(cr);
}

static void draw_icon(struct panel *panel, cairo_t *cr, double x, double y, double s,
		uint32_t fg) {
	switch (osd.kind) {
	case OSD_VOLUME:
		ti_speaker(panel, cr, x, y, s, osd.value, osd.muted, fg);
		break;
	case OSD_BRIGHTNESS:
		ti_brightness(panel, cr, x, y, s, fg);
		break;
	case OSD_MIC:
		draw_mic(cr, x, y, s, osd.muted, fg);
		break;
	case OSD_MEDIA:
		draw_media_glyph(cr, x, y, s, fg);
		break;
	}
}

static void osd_render(struct psurface *s, cairo_t *cr) {
	struct panel *panel = s->panel;
	enum pstyle style = panel_style(panel);
	uint32_t bg, fg, accent, border;
	osd_colors(panel, &bg, &fg, &accent, &border);
	double W = s->width, H = s->height;
	uint32_t dim = (fg & 0xffffff00) | 0x50;
	uint32_t fill = osd.muted ? ((fg & 0xffffff00) | 0x90) : accent;

	double r = 0;
	if (!vertical(panel)) {
		r = style == PS_FLUENT ? (osd.kind == OSD_MEDIA ? 12 : H / 2) :
			style == PS_LUNA ? 8 : style == PS_AERO ? 6 : 0;
	}
	pd_rounded(cr, 0.5, 0.5, W - 1, H - 1, r);
	pd_color(cr, bg);
	cairo_fill_preserve(cr);
	pd_color(cr, style == PS_LUNA ? accent : border);
	cairo_set_line_width(cr, style == PS_LUNA ? 2 : 1);
	cairo_stroke(cr);
	if (style == PS_CLASSIC) {
		pd_bevel(cr, 0, 0, W, H, false);
	}

	char number[16] = "";
	if (osd.kind == OSD_VOLUME || osd.kind == OSD_BRIGHTNESS) {
		snprintf(number, sizeof(number), "%d", osd.value);
	}
	const char *font = tw_theme_str(panel->theme, "osd.font", bar_font(panel));
	const char *bold = bar_bold_font(panel);

	if (osd.kind == OSD_MEDIA) {
		draw_icon(panel, cr, 18, (H - 32) / 2, 32, fg);
		const char *title = osd.title[0] ? osd.title :
			osd.status[0] ? osd.status : "Nothing is playing";
		pd_text(cr, bold, title, 66, 10, W - 80, H / 2 - 8, fg, PD_LEFT);
		const char *sub = osd.artist[0] ? osd.artist : osd.status;
		pd_text(cr, font, sub, 66, H / 2, W - 80, H / 2 - 10, (fg & 0xffffff00) | 0xb0, PD_LEFT);
		return;
	}

	if (vertical(panel)) {
		// Windows 10: number, vertical slider, icon
		pd_text(cr, font, number, 0, 10, W, 24, fg, PD_CENTER);
		double top = 44, bottom = H - 50, height = bottom - top;
		double level = osd.kind == OSD_MIC ? (osd.muted ? 0 : 1) : osd.value / 100.0;
		pd_rect(cr, W / 2 - 2, top, 4, height, dim);
		pd_rect(cr, W / 2 - 2, bottom - height * level, 4, height * level, fill);
		pd_rect(cr, W / 2 - 8, bottom - height * level - 4, 16, 8, fg);
		draw_icon(panel, cr, (W - 24) / 2, H - 38, 24, fg);
		return;
	}

	double icon = 24, x = 16;
	draw_icon(panel, cr, x, (H - icon) / 2, icon, fg);
	double bx = x + icon + 16, bw = W - bx - 56;
	if (osd.kind == OSD_MIC) {
		pd_text(cr, font, osd.muted ? "Microphone is muted" : "Microphone is on",
			bx, 0, W - bx - 12, H, fg, PD_LEFT);
		return;
	}
	double level = osd.value / 100.0;
	pd_rounded(cr, bx, H / 2 - 2, bw, 4, 2);
	pd_color(cr, dim);
	cairo_fill(cr);
	if (level > 0) {
		pd_rounded(cr, bx, H / 2 - 2, bw * level, 4, 2);
		pd_color(cr, fill);
		cairo_fill(cr);
	}
	double kx = bx + bw * level;
	if (style == PS_FLUENT) {
		cairo_arc(cr, kx, H / 2, 8, 0, 2 * M_PI);
		pd_color(cr, bg);
		cairo_fill_preserve(cr);
		pd_color(cr, border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_arc(cr, kx, H / 2, 4.5, 0, 2 * M_PI);
		pd_color(cr, fill);
		cairo_fill(cr);
	} else {
		pd_rounded(cr, kx - 4, H / 2 - 9, 8, 18, style == PS_CLASSIC ? 0 : 2);
		pd_color(cr, style == PS_CLASSIC ? 0xc0c0c0ff : fill);
		cairo_fill(cr);
		if (style == PS_CLASSIC) {
			pd_bevel(cr, kx - 4, H / 2 - 9, 8, 18, false);
		}
	}
	pd_text(cr, font, number, W - 48, 0, 38, H, fg, PD_CENTER);
}

static void destroy_surface(void) {
	if (osd.surface) {
		struct psurface *s = osd.surface;
		osd.surface = NULL;
		psurface_destroy(s);
	}
}

static void osd_closed(struct psurface *s) {
	if (osd.surface == s) {
		osd.surface = NULL;
	}
	psurface_destroy(s);
}

static const struct psurface_impl osd_impl = {
	.render = osd_render,
	.closed = osd_closed,
};

static void hide_fired(void *data) {
	osd.hide_timer = NULL;
	destroy_surface();
}

static void osd_show(struct panel *panel) {
	struct panel_output *output = panel_focused_output(panel);
	if (!output || !panel->compositor) {
		return;
	}
	osd.panel = panel;
	int width, height;
	osd_size(panel, &width, &height);
	if (osd.surface && (osd.surface->output != output || osd.surface->req_width != width ||
			osd.surface->req_height != height)) {
		destroy_surface();
	}
	if (!osd.surface) {
		struct psurface *s = psurface_create(panel, output, &osd_impl, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-osd");
		if (panel_style(panel) == PS_FLAT) {
			// Windows 10 shows it in the top left corner
			zwlr_layer_surface_v1_set_anchor(s->layer_surface,
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
			zwlr_layer_surface_v1_set_margin(s->layer_surface, 56, 0, 0, 56);
		} else {
			bool bottom = panel->config && panel->config->layouts[panel->layout].bottom;
			int bar = output->bar ? output->bar->height : 0;
			zwlr_layer_surface_v1_set_anchor(s->layer_surface,
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
			zwlr_layer_surface_v1_set_margin(s->layer_surface, 0, 0,
				(bottom ? bar : 0) + 48, 0);
		}
		zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
		psurface_set_size(s, width, height);
		struct wl_region *region = wl_compositor_create_region(panel->compositor);
		wl_surface_set_input_region(s->surface, region);
		wl_region_destroy(region);
		wl_surface_commit(s->surface);
		osd.surface = s;
	} else {
		psurface_set_dirty(osd.surface);
	}
	if (osd.hide_timer) {
		loop_remove_timer(panel->loop, osd.hide_timer);
	}
	osd.hide_timer = loop_add_timer(panel->loop, HIDE_MS, hide_fired, panel);
}

static void copy_field(char *dest, size_t size, const char *start, const char *end) {
	size_t len = end ? (size_t)(end - start) : strlen(start);
	if (len >= size) {
		len = size - 1;
	}
	memcpy(dest, start, len);
	dest[len] = '\0';
}

static void media_done(void *data, const char *output) {
	struct panel *panel = data;
	osd.proc = NULL;
	osd.status[0] = osd.artist[0] = osd.title[0] = '\0';
	// "status<TAB>artist<TAB>title" of the first player
	const char *line_end = strchr(output, '\n');
	char line[600];
	copy_field(line, sizeof(line), output, line_end);
	char *tab1 = strchr(line, '\t');
	char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
	if (tab1 && tab2) {
		copy_field(osd.status, sizeof(osd.status), line, tab1);
		copy_field(osd.artist, sizeof(osd.artist), tab1 + 1, tab2);
		copy_field(osd.title, sizeof(osd.title), tab2 + 1, NULL);
	}
	osd.kind = OSD_MEDIA;
	osd_show(panel);
}

static bool is_true(const char *s) {
	return s && (strcmp(s, "1") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "true") == 0 ||
		strcmp(s, "muted") == 0 || strcmp(s, "on") == 0);
}

static int clamp_percent(const char *s) {
	int value = s ? atoi(s) : 0;
	return value < 0 ? 0 : value > 100 ? 100 : value;
}

void osd_handle_command(struct panel *panel, int argc, char **argv) {
	if (argc < 1) {
		return;
	}
	const char *kind = argv[0];
	if (strcmp(kind, "hide") == 0) {
		destroy_surface();
		return;
	}
	if (strcmp(kind, "volume") == 0) {
		osd.kind = OSD_VOLUME;
		osd.value = clamp_percent(argc > 1 ? argv[1] : NULL);
		osd.muted = argc > 2 && is_true(argv[2]);
	} else if (strcmp(kind, "brightness") == 0) {
		osd.kind = OSD_BRIGHTNESS;
		osd.value = clamp_percent(argc > 1 ? argv[1] : NULL);
		osd.muted = false;
	} else if (strcmp(kind, "mic") == 0) {
		osd.kind = OSD_MIC;
		osd.muted = argc > 1 && is_true(argv[1]);
	} else if (strcmp(kind, "media") == 0) {
		if (osd.proc) {
			proc_cancel(osd.proc);
		}
		// give the player a moment to switch tracks before asking it
		osd.proc = proc_run(panel, "sleep 0.3; playerctl metadata --format "
			"'{{status}}\t{{artist}}\t{{title}}' 2>/dev/null", false, NULL, media_done, panel);
		return;
	} else {
		sway_log(SWAY_ERROR, "Unknown osd kind '%s'", kind);
		return;
	}
	osd_show(panel);
}
