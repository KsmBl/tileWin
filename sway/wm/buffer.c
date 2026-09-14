#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include "sway/tilewin.h"

struct tw_cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct tw_cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct tw_cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// nothing to do, the data stays mapped
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

struct wlr_buffer *tw_buffer_from_surface(cairo_surface_t *surface) {
	cairo_surface_flush(surface);
	struct tw_cairo_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl,
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface));
	buffer->surface = surface;
	return &buffer->base;
}

void tw_scene_buffer_set_surface(struct wlr_scene_buffer *node,
		cairo_surface_t *surface, int width, int height) {
	if (!surface) {
		wlr_scene_buffer_set_buffer(node, NULL);
		return;
	}
	struct wlr_buffer *buffer = tw_buffer_from_surface(surface);
	if (!buffer) {
		wlr_scene_buffer_set_buffer(node, NULL);
		return;
	}
	wlr_scene_buffer_set_buffer(node, buffer);
	wlr_scene_buffer_set_dest_size(node, width, height);
	wlr_buffer_drop(buffer);
}
