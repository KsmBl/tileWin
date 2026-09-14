#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "log.h"
#include "panel.h"

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct psurface *s = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	bool changed = !s->configured || s->width != (int)width || s->height != (int)height;
	s->width = width;
	s->height = height;
	s->configured = true;
	if (s->impl && s->impl->configured) {
		s->impl->configured(s);
	}
	if (changed || s->catcher) {
		s->dirty = true;
		psurface_render(s);
	}
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	struct psurface *s = data;
	if (s->impl && s->impl->closed) {
		s->impl->closed(s);
	} else {
		psurface_destroy(s);
	}
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static struct psurface *create(struct panel *panel, struct panel_output *output,
		const struct psurface_impl *impl, void *data,
		enum zwlr_layer_shell_v1_layer layer, const char *name_space) {
	struct psurface *s = calloc(1, sizeof(*s));
	s->panel = panel;
	s->output = output;
	s->impl = impl;
	s->data = data;
	s->scale = output ? output->scale : 1;
	s->hotspots = create_list();
	s->surface = wl_compositor_create_surface(panel->compositor);
	wl_surface_set_user_data(s->surface, s);
	s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(panel->layer_shell,
		s->surface, output ? output->wl_output : NULL, layer, name_space);
	zwlr_layer_surface_v1_add_listener(s->layer_surface, &layer_surface_listener, s);
	wl_list_insert(panel->surfaces.prev, &s->link);
	return s;
}

struct psurface *psurface_create(struct panel *panel, struct panel_output *output,
		const struct psurface_impl *impl, void *data,
		enum zwlr_layer_shell_v1_layer layer, const char *name_space) {
	return create(panel, output, impl, data, layer, name_space);
}

struct psurface *psurface_create_catcher(struct panel *panel,
		struct panel_output *output, const struct psurface_impl *impl, void *data) {
	struct psurface *s = create(panel, output, impl, data,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-popup-catcher");
	s->catcher = true;
	if (panel->viewporter) {
		s->viewport = wp_viewporter_get_viewport(panel->viewporter, s->surface);
	}
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	wl_surface_commit(s->surface);
	return s;
}

void psurface_clear_hotspots(struct psurface *s) {
	for (int i = 0; i < s->hotspots->length; i++) {
		struct hotspot *hs = s->hotspots->items[i];
		free(hs->str);
		free(hs);
	}
	s->hotspots->length = 0;
	s->hover = NULL;
}

void psurface_destroy(struct psurface *s) {
	if (!s) {
		return;
	}
	struct panel *panel = s->panel;
	if (panel->tooltip_source == s) {
		panel->tooltip_source = NULL;
		tooltip_cancel(panel);
	}
	struct panel_seat *seat;
	wl_list_for_each(seat, &panel->seats, link) {
		if (seat->pointer_focus == s) {
			seat->pointer_focus = NULL;
		}
		if (seat->keyboard_focus == s) {
			seat->keyboard_focus = NULL;
		}
	}
	psurface_clear_hotspots(s);
	list_free(s->hotspots);
	if (s->viewport) {
		wp_viewport_destroy(s->viewport);
	}
	if (s->layer_surface) {
		zwlr_layer_surface_v1_destroy(s->layer_surface);
	}
	wl_surface_destroy(s->surface);
	destroy_buffer(&s->buffers[0]);
	destroy_buffer(&s->buffers[1]);
	wl_list_remove(&s->link);
	free(s);
}

void psurface_set_size(struct psurface *s, int width, int height) {
	s->req_width = width;
	s->req_height = height;
	zwlr_layer_surface_v1_set_size(s->layer_surface, width, height);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	wl_callback_destroy(callback);
	struct psurface *s = data;
	s->frame_pending = false;
	if (s->dirty) {
		psurface_render(s);
	}
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

void psurface_set_dirty(struct psurface *s) {
	s->dirty = true;
	if (!s->frame_pending) {
		psurface_render(s);
	}
}

static void render_catcher(struct psurface *s) {
	int w = s->viewport ? 1 : s->width * s->scale;
	int h = s->viewport ? 1 : s->height * s->scale;
	struct pool_buffer *buffer = get_next_buffer(s->panel->shm, s->buffers, w, h);
	if (!buffer) {
		return;
	}
	cairo_save(buffer->cairo);
	cairo_set_operator(buffer->cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(buffer->cairo);
	cairo_restore(buffer->cairo);
	if (s->viewport) {
		wp_viewport_set_destination(s->viewport, s->width, s->height);
	} else {
		wl_surface_set_buffer_scale(s->surface, s->scale);
	}
	wl_surface_attach(s->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(s->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(s->surface);
	s->dirty = false;
}

void psurface_render(struct psurface *s) {
	if (!s->configured || s->width <= 0 || s->height <= 0) {
		return;
	}
	if (s->catcher) {
		render_catcher(s);
		return;
	}
	if (s->frame_pending) {
		s->dirty = true;
		return;
	}
	s->scale = s->output ? s->output->scale : 1;
	struct pool_buffer *buffer = get_next_buffer(s->panel->shm, s->buffers,
		s->width * s->scale, s->height * s->scale);
	if (!buffer) {
		return;
	}
	cairo_t *cr = buffer->cairo;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);

	cairo_save(cr);
	cairo_scale(cr, s->scale, s->scale);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
	cairo_set_font_options(cr, fo);
	cairo_font_options_destroy(fo);

	psurface_clear_hotspots(s);
	if (s->impl && s->impl->render) {
		s->impl->render(s, cr);
	}
	cairo_restore(cr);
	cairo_surface_flush(buffer->surface);

	wl_surface_set_buffer_scale(s->surface, s->scale);
	wl_surface_attach(s->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(s->surface, 0, 0, INT32_MAX, INT32_MAX);
	struct wl_callback *cb = wl_surface_frame(s->surface);
	wl_callback_add_listener(cb, &frame_listener, s);
	s->frame_pending = true;
	s->dirty = false;
	wl_surface_commit(s->surface);
}

void psurface_add_hotspot(struct psurface *s, int x, int y, int width, int height,
		struct widget *widget, int kind, int64_t id, const char *str) {
	struct hotspot *hs = calloc(1, sizeof(*hs));
	hs->box = (struct pbox){ x, y, width, height };
	hs->widget = widget;
	hs->kind = kind;
	hs->id = id;
	hs->str = str ? strdup(str) : NULL;
	list_add(s->hotspots, hs);
}

struct hotspot *psurface_hotspot_at(struct psurface *s, double x, double y) {
	for (int i = s->hotspots->length - 1; i >= 0; i--) {
		struct hotspot *hs = s->hotspots->items[i];
		if (pbox_contains(&hs->box, x, y)) {
			return hs;
		}
	}
	return NULL;
}
