#include <math.h>
#include <stdio.h>
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "tw_desktop.h"

/*
 * Notification area icons of the active theme (icons/tray-*.svg). Themes with
 * "tray { icons symbolic }" draw white shapes that are tinted with the text
 * color. Without theme icons the drawn glyphs are used.
 */
static bool draw_theme_icon(struct panel *panel, cairo_t *cr, const char *name,
		double x, double y, double size, uint32_t color) {
	if (!tw_icon_theme_has(name)) {
		return false;
	}
	double px = size, py = 0;
	cairo_user_to_device_distance(cr, &px, &py);
	int pixels = (int)ceil(hypot(px, py));
	cairo_surface_t *icon = apps_icon(panel, name, pixels > 0 ? pixels : (int)size);
	if (!icon) {
		return false;
	}
	if (strcmp(tw_theme_str(panel->theme, "tray.icons", "color"), "symbolic") != 0) {
		pd_icon(cr, icon, x, y, size);
		return true;
	}
	int iw = cairo_image_surface_get_width(icon);
	int ih = cairo_image_surface_get_height(icon);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / iw, size / ih);
	pd_color(cr, color);
	cairo_mask_surface(cr, icon, 0, 0);
	cairo_restore(cr);
	return true;
}

void ti_speaker(struct panel *panel, cairo_t *cr, double x, double y, double size,
		int volume, bool muted, uint32_t color) {
	const char *name = muted ? "tray-volume-muted" : volume <= 0 ? "tray-volume-off" :
		volume < 34 ? "tray-volume-low" : volume < 67 ? "tray-volume-medium" : "tray-volume-high";
	if (!draw_theme_icon(panel, cr, name, x, y, size, color)) {
		pd_glyph_speaker(cr, x, y, size, volume, muted, color);
	}
}

void ti_network(struct panel *panel, cairo_t *cr, double x, double y, double size,
		int bars, bool wireless, bool connected, uint32_t color) {
	char name[64];
	if (wireless && connected) {
		snprintf(name, sizeof(name), "tray-network-wireless-%d", bars < 0 ? 0 : bars > 4 ? 4 : bars);
	} else {
		snprintf(name, sizeof(name), "tray-network-%s%s", wireless ? "wireless" : "wired",
			connected ? "" : "-offline");
	}
	if (!draw_theme_icon(panel, cr, name, x, y, size, color)) {
		pd_glyph_network(cr, x, y, size, bars, wireless, connected, color);
	}
}

void ti_battery(struct panel *panel, cairo_t *cr, double x, double y, double size,
		int percent, bool charging, uint32_t color) {
	int level = (percent + 10) / 20 * 20;
	level = level < 0 ? 0 : level > 100 ? 100 : level;
	char name[64];
	snprintf(name, sizeof(name), "tray-battery-%d%s", level, charging ? "-charging" : "");
	if (!draw_theme_icon(panel, cr, name, x, y, size, color)) {
		pd_glyph_battery(cr, x, y, size, percent, charging, color);
	}
}

void ti_brightness(struct panel *panel, cairo_t *cr, double x, double y, double size,
		uint32_t color) {
	if (!draw_theme_icon(panel, cr, "tray-brightness", x, y, size, color)) {
		pd_glyph_brightness(cr, x, y, size, color);
	}
}

void ti_disk(struct panel *panel, cairo_t *cr, double x, double y, double size,
		bool active, uint32_t color) {
	const char *name = active ? "tray-disk-active" : "tray-disk";
	if (!draw_theme_icon(panel, cr, name, x, y, size, color) &&
			!draw_theme_icon(panel, cr, "tray-disk", x, y, size, color)) {
		pd_glyph_disk(cr, x, y, size, active, color);
	}
}
