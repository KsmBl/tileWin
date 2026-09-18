#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include "sway/tilewin.h"
#include "sway/tree/view.h"

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

/* ---------- copies of the buffers of a window ---------- */

struct snapshot {
	struct wlr_scene_tree *parent;
	double scale_x, scale_y;
	double x, y; // offset of the copied tree
};

static void snapshot_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct snapshot *s = data;
	if (!buffer->buffer) {
		return;
	}
	int w = buffer->dst_width > 0 ? buffer->dst_width : buffer->buffer->width;
	int h = buffer->dst_height > 0 ? buffer->dst_height : buffer->buffer->height;
	struct wlr_scene_buffer *copy = wlr_scene_buffer_create(s->parent, NULL);
	if (!copy) {
		return;
	}
	wlr_scene_buffer_set_dest_size(copy, fmax(1, round(w * s->scale_x)),
		fmax(1, round(h * s->scale_y)));
	wlr_scene_buffer_set_opacity(copy, buffer->opacity);
	wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
	wlr_scene_buffer_set_transfer_function(copy, buffer->transfer_function);
	wlr_scene_buffer_set_primaries(copy, buffer->primaries);
	wlr_scene_buffer_set_source_box(copy, &buffer->src_box);
	wlr_scene_buffer_set_transform(copy, buffer->transform);
	wlr_scene_node_set_position(&copy->node, round(s->x + sx * s->scale_x),
		round(s->y + sy * s->scale_y));
	wlr_scene_buffer_set_buffer(copy, buffer->buffer);
}

void tw_snapshot_view(struct wlr_scene_tree *parent, struct sway_view *view,
		double scale_x, double scale_y, double x, double y) {
	struct wlr_scene_tree *source = view->saved_surface_tree ?
		view->saved_surface_tree : view->content_tree;
	if (!source) {
		return;
	}
	struct snapshot s = { parent, scale_x, scale_y, x, y };
	// the source tree's own offset is included by the iterator
	wlr_scene_node_for_each_buffer(&source->node, snapshot_buffer, &s);
}

void tw_snapshot_tree(struct wlr_scene_tree *parent, struct wlr_scene_node *source,
		double scale, double x, double y) {
	struct snapshot s = { parent, scale, scale, x, y };
	wlr_scene_node_for_each_buffer(source, snapshot_buffer, &s);
}
