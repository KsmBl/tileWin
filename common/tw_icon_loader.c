#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <librsvg/rsvg.h>
#include "config.h"
#include "list.h"
#include "log.h"
#include "swaybar/tray/icon.h"
#include "tw_desktop.h"
#if HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

static list_t *themes = NULL;
static list_t *basedirs = NULL;

static bool has_suffix(const char *s, const char *suffix) {
	size_t ls = strlen(s), lx = strlen(suffix);
	return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

#if HAVE_GDK_PIXBUF
static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pixbuf) {
	int w = gdk_pixbuf_get_width(pixbuf);
	int h = gdk_pixbuf_get_height(pixbuf);
	int channels = gdk_pixbuf_get_n_channels(pixbuf);
	int stride_in = gdk_pixbuf_get_rowstride(pixbuf);
	const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	cairo_surface_flush(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (int y = 0; y < h; y++) {
		const guchar *src = pixels + y * stride_in;
		uint32_t *dst = (uint32_t *)(data + y * stride);
		for (int x = 0; x < w; x++) {
			uint32_t r = src[0], g = src[1], b = src[2];
			uint32_t a = channels == 4 ? src[3] : 0xff;
			r = r * a / 255;
			g = g * a / 255;
			b = b * a / 255;
			dst[x] = a << 24 | r << 16 | g << 8 | b;
			src += channels;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}
#endif

cairo_surface_t *tw_image_load(const char *path) {
	if (!path) {
		return NULL;
	}
	if (has_suffix(path, ".png")) {
		cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
			return surface;
		}
		cairo_surface_destroy(surface);
	}
#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		sway_log(SWAY_DEBUG, "Failed to load image %s: %s", path,
			err ? err->message : "unknown error");
		if (err) {
			g_error_free(err);
		}
		return NULL;
	}
	cairo_surface_t *surface = surface_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
	return surface;
#else
	return NULL;
#endif
}

cairo_surface_t *tw_icon_load_file(const char *path, int size) {
	if (!path || size <= 0) {
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(out);
		return NULL;
	}
	cairo_t *cr = cairo_create(out);
	bool ok = false;

	if (has_suffix(path, ".svg") || has_suffix(path, ".svgz")) {
		GError *err = NULL;
		RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
		if (handle) {
			RsvgRectangle viewport = { 0, 0, size, size };
			ok = rsvg_handle_render_document(handle, cr, &viewport, &err);
			g_object_unref(handle);
		}
		if (err) {
			sway_log(SWAY_DEBUG, "Failed to render %s: %s", path, err->message);
			g_error_free(err);
		}
	} else {
		cairo_surface_t *image = tw_image_load(path);
		if (image) {
			int iw = cairo_image_surface_get_width(image);
			int ih = cairo_image_surface_get_height(image);
			double scale = (double)size / (iw > ih ? iw : ih);
			cairo_translate(cr, (size - iw * scale) / 2, (size - ih * scale) / 2);
			cairo_scale(cr, scale, scale);
			cairo_set_source_surface(cr, image, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_surface_destroy(image);
			ok = true;
		}
	}
	cairo_destroy(cr);
	if (!ok) {
		cairo_surface_destroy(out);
		return NULL;
	}
	cairo_surface_flush(out);
	return out;
}

cairo_surface_t *tw_icon_load(const char *name, int size, const char *theme) {
	if (!name || !*name) {
		return NULL;
	}
	if (name[0] == '/') {
		return tw_icon_load_file(name, size);
	}
	if (!themes) {
		init_themes(&themes, &basedirs);
	}
	const char *theme_names[] = { theme ? theme : "hicolor", "Adwaita", "hicolor" };
	for (size_t i = 0; i < sizeof(theme_names) / sizeof(theme_names[0]); i++) {
		int min_size = 0, max_size = 0;
		char *path = find_icon(themes, basedirs, (char *)name, size,
			(char *)theme_names[i], &min_size, &max_size);
		if (path) {
			cairo_surface_t *surface = tw_icon_load_file(path, size);
			free(path);
			if (surface) {
				return surface;
			}
		}
	}
	return NULL;
}

cairo_surface_t *tw_icon_load_for_app(const char *app_id, int size, const char *theme) {
	if (!app_id || !*app_id) {
		return NULL;
	}
	cairo_surface_t *surface = NULL;
	struct tw_desktop_entry *entry = tw_desktop_find_for_app_id(app_id);
	if (entry && entry->icon) {
		surface = tw_icon_load(entry->icon, size, theme);
	}
	tw_desktop_entry_free(entry);
	if (surface) {
		return surface;
	}

	surface = tw_icon_load(app_id, size, theme);
	if (surface) {
		return surface;
	}
	char *lower = strdup(app_id);
	for (char *p = lower; *p; p++) {
		*p = tolower((unsigned char)*p);
	}
	surface = tw_icon_load(lower, size, theme);
	if (!surface) {
		const char *dot = strrchr(lower, '.');
		if (dot && dot[1]) {
			surface = tw_icon_load(dot + 1, size, theme);
		}
	}
	free(lower);
	return surface;
}
