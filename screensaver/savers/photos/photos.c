#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_photos;

/* Photos. */

#define PHOTOS_MAX 2000
#define PHOTO_FADE 1.5

struct photo {
	cairo_surface_t *image;
	double z0, z1, x0, y0, x1, y1; // zoom and pan from start to end
};

struct photos {
	double t;
	char **files;
	int count, next;
	struct photo current, coming;
	double shown, seconds;
	bool coming_loaded;
	int width, height;
	char folder[512];
};

static bool is_picture(const char *name) {
	const char *dot = strrchr(name, '.');
	static const char *const types[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp",
		".tif", ".tiff", NULL };
	for (int i = 0; dot && types[i]; i++) {
		if (strcasecmp(dot, types[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void photos_scan(struct photos *s, const char *dir, int depth) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && s->count < PHOTOS_MAX) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char *path = g_build_filename(dir, e->d_name, NULL);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && depth < 4) {
			photos_scan(s, path, depth + 1);
		} else if (S_ISREG(st.st_mode) && is_picture(e->d_name)) {
			s->files = realloc(s->files, (s->count + 1) * sizeof(char *));
			s->files[s->count++] = path;
			path = NULL;
		}
		g_free(path);
	}
	if (d) {
		closedir(d);
	}
}

/* A picture as a cairo surface just big enough to cover the area as it zooms. */
static cairo_surface_t *photo_load(const char *path, int width, int height) {
	int iw = 0, ih = 0;
	if (!gdk_pixbuf_get_file_info(path, &iw, &ih) || iw <= 0 || ih <= 0) {
		return NULL;
	}
	double scale = fmax(width * 1.15 / iw, height * 1.15 / ih);
	GdkPixbuf *loaded = gdk_pixbuf_new_from_file_at_scale(path, (int)ceil(iw * scale),
		(int)ceil(ih * scale), FALSE, NULL);
	if (!loaded) {
		return NULL;
	}
	GdkPixbuf *pixbuf = gdk_pixbuf_apply_embedded_orientation(loaded);
	g_object_unref(loaded);
	int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
	int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
	const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *out = cairo_image_surface_get_data(image);
	int out_stride = cairo_image_surface_get_stride(image);
	for (int y = 0; y < h; y++) {
		const guchar *p = pixels + y * stride;
		uint32_t *o = (uint32_t *)(out + y * out_stride);
		for (int x = 0; x < w; x++, p += channels) {
			unsigned a = channels == 4 ? p[3] : 255;
			o[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(image);
	g_object_unref(pixbuf);
	return image;
}

static void photo_next(struct photos *s, struct photo *photo) {
	photo->image = NULL;
	for (int tries = 0; tries < 5 && !photo->image && s->count; tries++) {
		photo->image = photo_load(s->files[s->next], s->width, s->height);
		s->next = (s->next + 1) % s->count;
	}
	// a slow zoom in or out, and a drift towards a corner
	bool in = saver_random() < 0.5;
	photo->z0 = in ? 1.0 : 1.12;
	photo->z1 = in ? 1.12 : 1.0;
	photo->x0 = saver_between(0.35, 0.65);
	photo->y0 = saver_between(0.35, 0.65);
	photo->x1 = saver_between(0.3, 0.7);
	photo->y1 = saver_between(0.3, 0.7);
}

static void *photos_create(int width, int height, const struct saver_options *options) {
	struct photos *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->seconds = options->photo_seconds >= 2 ? options->photo_seconds : 8;
	if (options->photos && *options->photos) {
		snprintf(s->folder, sizeof(s->folder), "%s", options->photos);
	} else {
		const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
		snprintf(s->folder, sizeof(s->folder), "%s", pictures ? pictures : g_get_home_dir());
	}
	photos_scan(s, s->folder, 0);
	// in a new order every time
	for (int i = s->count - 1; i > 0; i--) {
		int j = (int)(saver_random() * (i + 1));
		char *swap = s->files[i];
		s->files[i] = s->files[j];
		s->files[j] = swap;
	}
	photo_next(s, &s->current);
	return s;
}

static void photo_paint(struct photos *s, cairo_t *cr, struct photo *photo, double progress,
		double alpha) {
	if (!photo->image) {
		return;
	}
	double iw = cairo_image_surface_get_width(photo->image);
	double ih = cairo_image_surface_get_height(photo->image);
	double zoom = photo->z0 + (photo->z1 - photo->z0) * progress;
	double scale = fmax(s->width / iw, s->height / ih) * zoom;
	double px = photo->x0 + (photo->x1 - photo->x0) * progress;
	double py = photo->y0 + (photo->y1 - photo->y0) * progress;
	double spare_x = iw * scale - s->width, spare_y = ih * scale - s->height;
	cairo_save(cr);
	cairo_translate(cr, -spare_x * px, -spare_y * py);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, photo->image, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

static void photos_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct photos *s = state;
	s->t += dt;
	if (!s->current.image) {
		char message[640];
		snprintf(message, sizeof(message), "Put pictures into %s to see them here", s->folder);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
			CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, fmax(10, saver_unit(width, height) * 26));
		cairo_text_extents_t ext;
		cairo_text_extents(cr, message, &ext);
		// drifting, so it does not burn into the screen either
		cairo_move_to(cr, (width - ext.width) / 2 + sin(s->t * 0.3) * width * 0.1,
			height / 2.0 + sin(s->t * 0.23) * height * 0.3);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		cairo_show_text(cr, message);
		return;
	}
	s->shown += dt;
	double life = s->seconds + PHOTO_FADE;
	if (!s->coming_loaded && s->shown > 0.5) {
		// loading takes a moment: in the quiet middle of a photo, not in the fade
		photo_next(s, &s->coming);
		s->coming_loaded = true;
	}
	photo_paint(s, cr, &s->current, saver_clamp(s->shown / life, 0, 1), 1);
	if (s->shown > s->seconds && s->coming_loaded) {
		double fade = saver_clamp((s->shown - s->seconds) / PHOTO_FADE, 0, 1);
		photo_paint(s, cr, &s->coming, fade * PHOTO_FADE / life, fade);
		if (fade >= 1) {
			if (s->current.image) {
				cairo_surface_destroy(s->current.image);
			}
			s->current = s->coming;
			s->coming.image = NULL;
			s->coming_loaded = false;
			s->shown = PHOTO_FADE;
		}
	}
}

static void photos_destroy(void *state) {
	struct photos *s = state;
	for (int i = 0; i < s->count; i++) {
		g_free(s->files[i]);
	}
	free(s->files);
	if (s->current.image) {
		cairo_surface_destroy(s->current.image);
	}
	if (s->coming.image) {
		cairo_surface_destroy(s->coming.image);
	}
	free(s);
}

const struct saver saver_photos = {
	.name = "photos",
	.title = "Photos",
	.description = "A slideshow of your pictures, slowly zooming and fading into each other",
	.create = photos_create,
	.draw = photos_draw,
	.destroy = photos_destroy,
};

TILEWIN_SAVER(.saver = &saver_photos, .order = 60, .shot_seconds = 3);
