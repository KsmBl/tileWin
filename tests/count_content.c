/*
 * Counts the pixels in a part of a screenshot that are not the background. A
 * settings page that does not lay itself out draws nothing but its background,
 * so the count tells an empty page from one that is really there. The
 * background is whichever color covers most of the area, which keeps the
 * answer the same in a light and in a dark theme.
 *
 * usage: count-content <png> <x> <y> <width> <height>
 */
#include <stdio.h>
#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define DIFFERENT_ENOUGH 24

static guint color_at(const guchar *pixels, int stride, int channels, int x, int y) {
	const guchar *p = pixels + y * stride + x * channels;
	return ((guint)p[0] << 16) | ((guint)p[1] << 8) | p[2];
}

static int channel_distance(guint a, guint b) {
	int most = 0;
	for (int shift = 0; shift <= 16; shift += 8) {
		int difference = abs((int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff));
		if (difference > most) {
			most = difference;
		}
	}
	return most;
}

int main(int argc, char **argv) {
	if (argc != 6) {
		fprintf(stderr, "usage: %s <png> <x> <y> <width> <height>\n", argv[0]);
		return 2;
	}
	GError *error = NULL;
	GdkPixbuf *image = gdk_pixbuf_new_from_file(argv[1], &error);
	if (!image) {
		fprintf(stderr, "%s: %s\n", argv[1], error->message);
		g_error_free(error);
		return 2;
	}
	int left = MAX(atoi(argv[2]), 0), top = MAX(atoi(argv[3]), 0);
	int right = MIN(left + atoi(argv[4]), gdk_pixbuf_get_width(image));
	int bottom = MIN(top + atoi(argv[5]), gdk_pixbuf_get_height(image));
	int channels = gdk_pixbuf_get_n_channels(image);
	int stride = gdk_pixbuf_get_rowstride(image);
	const guchar *pixels = gdk_pixbuf_read_pixels(image);

	GHashTable *counts = g_hash_table_new(g_direct_hash, g_direct_equal);
	guint background = 0, most = 0;
	for (int y = top; y < bottom; y++) {
		for (int x = left; x < right; x++) {
			guint color = color_at(pixels, stride, channels, x, y);
			gpointer key = GUINT_TO_POINTER(color);
			guint seen = GPOINTER_TO_UINT(g_hash_table_lookup(counts, key)) + 1;
			g_hash_table_insert(counts, key, GUINT_TO_POINTER(seen));
			if (seen > most) {
				most = seen;
				background = color;
			}
		}
	}
	g_hash_table_destroy(counts);

	guint content = 0;
	for (int y = top; y < bottom; y++) {
		for (int x = left; x < right; x++) {
			guint color = color_at(pixels, stride, channels, x, y);
			if (channel_distance(color, background) > DIFFERENT_ENOUGH) {
				content++;
			}
		}
	}
	printf("%u\n", content);
	g_object_unref(image);
	return 0;
}
