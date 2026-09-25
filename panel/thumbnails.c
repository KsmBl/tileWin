/*
 * Live previews of windows above the taskbar buttons, like on Windows: hovering
 * a button shows its window (or all windows of a group) instead of a text
 * tooltip. Clicking a preview switches to the window, × or a middle click
 * closes it.
 *
 * The pictures come from ext-image-copy-capture of the windows' foreign
 * toplevel handles and are refreshed every second while the previews are
 * shown; nothing is captured otherwise. "thumbnails no" on the taskbar widget
 * or the theme key taskbar.thumbnails keeps the text tooltips.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "draw.h"
#include "flyout.h"
#include "popup.h"

#define THUMB_W 220
#define THUMB_H 130
#define TITLE_H 30
#define PAD 8
#define GAP 8
#define MAX_THUMBS 6
#define REFRESH_MS 1000
#define HIDE_MS 250

struct handle {
	struct ext_foreign_toplevel_handle_v1 *handle;
	char *identifier;
};

struct thumb {
	int64_t con_id;
	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	uint32_t width, height;
	bool argb, xrgb;
	uint32_t format;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	uint32_t buffer_width, buffer_height;
	cairo_surface_t *image;
	struct loop_timer *timer;
	struct pbox box, close;
};

enum op {
	OP_NONE,
	OP_ACTIVATE,
	OP_CLOSE,
};

static struct {
	struct panel *panel;
	list_t *handles; // struct handle *
	struct psurface *surface;
	struct thumb thumbs[MAX_THUMBS];
	int count;
	int64_t shown_id;
	int shown_kind;
	bool hover;
	double px, py;
	struct loop_timer *hide_timer, *op_timer;
	enum op op;
	int64_t op_id;
} tn;

/* ---------- foreign toplevel handles ---------- */

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	struct handle *h = data;
	for (int i = 0; i < tn.handles->length; i++) {
		if (tn.handles->items[i] == h) {
			list_del(tn.handles, i);
			break;
		}
	}
	ext_foreign_toplevel_handle_v1_destroy(h->handle);
	free(h->identifier);
	free(h);
}

static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
}

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *title) {
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *app_id) {
}

static void handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *identifier) {
	struct handle *h = data;
	free(h->identifier);
	h->identifier = strdup(identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.closed = handle_closed,
	.done = handle_done,
	.title = handle_title,
	.app_id = handle_app_id,
	.identifier = handle_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	struct handle *h = calloc(1, sizeof(*h));
	h->handle = handle;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, h);
	list_add(tn.handles, h);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

void thumbnails_list_bound(struct panel *panel) {
	tn.panel = panel;
	if (!tn.handles) {
		tn.handles = create_list();
	}
	ext_foreign_toplevel_list_v1_add_listener(panel->toplevel_list, &list_listener, NULL);
}

static struct ext_foreign_toplevel_handle_v1 *find_handle(const char *identifier) {
	for (int i = 0; identifier && tn.handles && i < tn.handles->length; i++) {
		struct handle *h = tn.handles->items[i];
		if (h->identifier && strcmp(h->identifier, identifier) == 0) {
			return h->handle;
		}
	}
	return NULL;
}

/* ---------- capturing ---------- */

static void capture(struct thumb *t);

static void thumb_timer(void *data) {
	struct thumb *t = data;
	t->timer = NULL;
	capture(t);
}

static void schedule_capture(struct thumb *t, int ms) {
	if (!t->timer) {
		t->timer = loop_add_timer(tn.panel->loop, ms, thumb_timer, t);
	}
}

static void free_buffer(struct thumb *t) {
	if (t->buffer) {
		wl_buffer_destroy(t->buffer);
		t->buffer = NULL;
	}
	if (t->data) {
		munmap(t->data, t->size);
		t->data = NULL;
	}
}

static bool ensure_buffer(struct thumb *t) {
	if (t->buffer && t->buffer_width == t->width && t->buffer_height == t->height) {
		return true;
	}
	free_buffer(t);
	struct panel *panel = tn.panel;
	size_t size = (size_t)t->width * t->height * 4;
	int fd = memfd_create("tilewin-thumbnail", MFD_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return false;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(panel->shm, fd, size);
	t->buffer = wl_shm_pool_create_buffer(pool, 0, t->width, t->height, t->width * 4,
		t->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	t->data = data;
	t->size = size;
	t->buffer_width = t->width;
	t->buffer_height = t->height;
	return true;
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t transform) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		int32_t x, int32_t y, int32_t width, int32_t height) {
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	struct thumb *t = data;
	ext_image_copy_capture_frame_v1_destroy(t->frame);
	t->frame = NULL;
	cairo_surface_t *src = cairo_image_surface_create_for_data(t->data,
		t->format == WL_SHM_FORMAT_ARGB8888 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		t->width, t->height, t->width * 4);
	double scale = (double)THUMB_W / t->width;
	if (t->height * scale > THUMB_H - 8) {
		scale = (double)(THUMB_H - 8) / t->height;
	}
	int w = t->width * scale < 1 ? 1 : (int)(t->width * scale);
	int h = t->height * scale < 1 ? 1 : (int)(t->height * scale);
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(image);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(src);
	if (t->image) {
		cairo_surface_destroy(t->image);
	}
	t->image = image;
	if (tn.surface) {
		psurface_set_dirty(tn.surface);
	}
	schedule_capture(t, REFRESH_MS);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t reason) {
	struct thumb *t = data;
	ext_image_copy_capture_frame_v1_destroy(t->frame);
	t->frame = NULL;
	if (reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
		schedule_capture(t, REFRESH_MS); // e.g. a minimized window: try again later
	} // otherwise the session sends new constraints and "done"
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void capture(struct thumb *t) {
	if (!t->session || t->frame || t->width == 0 || t->height == 0 || !t->format ||
			!ensure_buffer(t)) {
		return;
	}
	t->frame = ext_image_copy_capture_session_v1_create_frame(t->session);
	ext_image_copy_capture_frame_v1_add_listener(t->frame, &frame_listener, t);
	ext_image_copy_capture_frame_v1_attach_buffer(t->frame, t->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(t->frame, 0, 0, t->width, t->height);
	ext_image_copy_capture_frame_v1_capture(t->frame);
}

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
		uint32_t width, uint32_t height) {
	struct thumb *t = data;
	t->width = width;
	t->height = height;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
		uint32_t format) {
	struct thumb *t = data;
	t->argb |= format == WL_SHM_FORMAT_ARGB8888;
	t->xrgb |= format == WL_SHM_FORMAT_XRGB8888;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
		struct wl_array *device) {
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
		uint32_t format, struct wl_array *modifiers) {
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
	struct thumb *t = data;
	t->format = t->argb ? WL_SHM_FORMAT_ARGB8888 : t->xrgb ? WL_SHM_FORMAT_XRGB8888 : 0;
	capture(t);
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
	struct thumb *t = data;
	if (t->frame) {
		ext_image_copy_capture_frame_v1_destroy(t->frame);
		t->frame = NULL;
	}
	ext_image_copy_capture_session_v1_destroy(t->session);
	t->session = NULL;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

static void thumb_start(struct thumb *t, struct pwindow *win) {
	struct panel *panel = tn.panel;
	memset(t, 0, sizeof(*t));
	t->con_id = win->id;
	struct ext_foreign_toplevel_handle_v1 *handle = find_handle(win->identifier);
	if (!handle || !panel->toplevel_capture || !panel->copy_capture || !panel->shm) {
		return; // the preview shows the app icon
	}
	t->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
		panel->toplevel_capture, handle);
	t->session = ext_image_copy_capture_manager_v1_create_session(panel->copy_capture,
		t->source, 0);
	ext_image_copy_capture_session_v1_add_listener(t->session, &session_listener, t);
}

static void thumb_free(struct thumb *t) {
	if (t->timer) {
		loop_remove_timer(tn.panel->loop, t->timer);
	}
	if (t->frame) {
		ext_image_copy_capture_frame_v1_destroy(t->frame);
	}
	if (t->session) {
		ext_image_copy_capture_session_v1_destroy(t->session);
	}
	if (t->source) {
		ext_image_capture_source_v1_destroy(t->source);
	}
	free_buffer(t);
	if (t->image) {
		cairo_surface_destroy(t->image);
	}
	memset(t, 0, sizeof(*t));
}

/* ---------- the previews ---------- */

static void hide_now(void) {
	if (tn.hide_timer) {
		loop_remove_timer(tn.panel->loop, tn.hide_timer);
		tn.hide_timer = NULL;
	}
	for (int i = 0; i < tn.count; i++) {
		thumb_free(&tn.thumbs[i]);
	}
	tn.count = 0;
	tn.hover = false;
	tn.shown_id = 0;
	if (tn.surface) {
		struct psurface *s = tn.surface;
		tn.surface = NULL;
		psurface_destroy(s);
	}
}

static void hide_fired(void *data) {
	tn.hide_timer = NULL;
	if (!tn.hover) {
		hide_now();
	}
}

void thumbnails_hide_later(struct panel *panel) {
	if (tn.surface && !tn.hide_timer) {
		tn.hide_timer = loop_add_timer(panel->loop, HIDE_MS, hide_fired, NULL);
	}
}

bool thumbnails_visible(void) {
	return tn.surface != NULL;
}

static void render(struct psurface *s, cairo_t *cr) {
	struct panel *panel = s->panel;
	struct fly_style st;
	fly_style_init(&st, panel);
	popup_draw_frame(panel, cr, s->width, s->height, 0, "menu");
	for (int i = 0; i < tn.count; i++) {
		struct thumb *t = &tn.thumbs[i];
		struct pwindow *win = panel_find_window(panel, t->con_id);
		int x = PAD + i * (THUMB_W + GAP);
		t->box = (struct pbox){ x, PAD, THUMB_W, TITLE_H + THUMB_H };
		t->close = (struct pbox){ x + THUMB_W - 28, PAD + 3, 24, 24 };
		if (!win) {
			continue;
		}
		bool hover = tn.hover && pbox_contains(&t->box, tn.px, tn.py);
		if (hover) {
			fill_hover(cr, &st, t->box);
		}
		pd_icon(cr, apps_icon_for_window(panel, win, 16), x + 6, PAD + 7, 16);
		pd_text(cr, st.font, win->title, x + 28, PAD, THUMB_W - 60, TITLE_H, st.fg, PD_LEFT);
		if (hover) {
			if (pbox_contains(&t->close, tn.px, tn.py)) {
				cairo_new_path(cr);
				pd_rounded(cr, t->close.x, t->close.y, 24, 24, 4);
				pd_color(cr, 0xc42b1cff);
				cairo_fill(cr);
			}
			double cx = t->close.x + 12, cy = t->close.y + 12;
			cairo_new_path(cr);
			cairo_move_to(cr, cx - 4.5, cy - 4.5);
			cairo_line_to(cr, cx + 4.5, cy + 4.5);
			cairo_move_to(cr, cx + 4.5, cy - 4.5);
			cairo_line_to(cr, cx - 4.5, cy + 4.5);
			pd_color(cr, pbox_contains(&t->close, tn.px, tn.py) ? 0xffffffff : st.fg);
			cairo_set_line_width(cr, 1.3);
			cairo_stroke(cr);
		}
		double area_y = PAD + TITLE_H, area_h = THUMB_H - 4;
		if (t->image) {
			int w = cairo_image_surface_get_width(t->image);
			int h = cairo_image_surface_get_height(t->image);
			cairo_save(cr);
			cairo_set_source_surface(cr, t->image, x + (THUMB_W - w) / 2,
				area_y + (area_h - h) / 2);
			cairo_paint(cr);
			cairo_restore(cr);
		} else {
			pd_icon(cr, apps_icon_for_window(panel, win, 48), x + (THUMB_W - 48) / 2,
				area_y + (area_h - 48) / 2, 48);
		}
	}
}

static void run_op(void *data) {
	tn.op_timer = NULL;
	struct panel *panel = tn.panel;
	struct pwindow *win = panel_find_window(panel, tn.op_id);
	if (win && tn.op == OP_CLOSE) {
		ipc_panel_commandf(panel, "[con_id=%lld] kill", (long long)win->id);
	} else if (win && tn.op == OP_ACTIVATE) {
		ipc_panel_commandf(panel, win->minimized ? "[con_id=%lld] minimize disable, focus" :
			"[con_id=%lld] focus", (long long)win->id);
	}
	tn.op = OP_NONE;
	hide_now();
}

static void pointer_motion(struct psurface *s, double x, double y) {
	tn.hover = true;
	tn.px = x;
	tn.py = y;
	if (tn.hide_timer) {
		loop_remove_timer(s->panel->loop, tn.hide_timer);
		tn.hide_timer = NULL;
	}
	psurface_set_dirty(s);
}

static void pointer_leave(struct psurface *s) {
	tn.hover = false;
	psurface_set_dirty(s);
	thumbnails_hide_later(s->panel);
}

static void pointer_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	if (!pressed || tn.op_timer) {
		return;
	}
	for (int i = 0; i < tn.count; i++) {
		struct thumb *t = &tn.thumbs[i];
		if (!pbox_contains(&t->box, x, y)) {
			continue;
		}
		tn.op_id = t->con_id;
		if (button == BTN_MIDDLE || (button == BTN_LEFT && pbox_contains(&t->close, x, y))) {
			tn.op = OP_CLOSE;
		} else if (button == BTN_LEFT) {
			tn.op = OP_ACTIVATE;
		} else {
			return;
		}
		// the surface can't be destroyed inside its own input handler
		tn.op_timer = loop_add_timer(s->panel->loop, 0, run_op, NULL);
		return;
	}
}

static void closed(struct psurface *s) {
	if (tn.surface == s) {
		tn.surface = NULL;
		hide_now();
	}
	psurface_destroy(s);
}

static const struct psurface_impl impl = {
	.render = render,
	.pointer_motion = pointer_motion,
	.pointer_leave = pointer_leave,
	.pointer_button = pointer_button,
	.closed = closed,
};

bool thumbnails_show(struct panel *panel, struct psurface *bar, struct hotspot *hs) {
	// taskbar buttons: kind 1 is a window, kind 2 a group of windows of one app
	if (!hs->widget || hs->widget->impl != &widget_taskbar || (hs->kind != 1 && hs->kind != 2)) {
		return false;
	}
	struct pbox placed;
	if (deskwidget_place(bar, hs->box, &placed)) {
		return false; // the previews line up along a taskbar; on the desktop the title shows
	}
	bool enabled = widget_conf_bool(hs->widget, "thumbnails",
		tw_theme_bool(panel->theme, "taskbar.thumbnails", true));
	struct pwindow *win = panel_find_window(panel, hs->id);
	if (!enabled || !win || !bar->output || !panel->toplevel_capture || !panel->copy_capture) {
		return false;
	}
	tn.panel = panel;
	if (tn.surface && tn.shown_id == hs->id && tn.shown_kind == hs->kind) {
		if (tn.hide_timer) {
			loop_remove_timer(panel->loop, tn.hide_timer);
			tn.hide_timer = NULL;
		}
		return true;
	}
	hide_now();

	struct pwindow *windows[MAX_THUMBS];
	int count = 0;
	if (hs->kind == 2) {
		for (int i = 0; i < panel->state.windows->length && count < MAX_THUMBS; i++) {
			struct pwindow *o = panel->state.windows->items[i];
			if (strcmp(o->app_id, win->app_id) == 0 && strcmp(o->output, win->output) == 0) {
				windows[count++] = o;
			}
		}
	}
	if (count == 0) {
		windows[count++] = win;
	}

	struct panel_output *output = bar->output;
	int width = 2 * PAD + count * THUMB_W + (count - 1) * GAP;
	int height = 2 * PAD + TITLE_H + THUMB_H;
	int x = hs->box.x + hs->box.width / 2 - width / 2;
	x = x + width > output->width - 4 ? output->width - width - 4 : x;
	x = x < 4 ? 4 : x;
	bool bottom = panel->config->layouts[panel->layout].bottom;
	int y = bottom ? output->height - bar->height - height - 4 : bar->height + 4;

	struct psurface *s = psurface_create(panel, output, &impl, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-thumbnails");
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(s->layer_surface, y, 0, 0, x);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	psurface_set_size(s, width, height);
	wl_surface_commit(s->surface);
	tn.surface = s;
	tn.count = count;
	tn.shown_id = hs->id;
	tn.shown_kind = hs->kind;
	for (int i = 0; i < count; i++) {
		thumb_start(&tn.thumbs[i], windows[i]);
	}
	return true;
}

void thumbnails_fini(struct panel *panel) {
	if (tn.op_timer) {
		loop_remove_timer(panel->loop, tn.op_timer);
		tn.op_timer = NULL;
	}
	if (tn.panel) {
		hide_now();
	}
	for (int i = 0; tn.handles && i < tn.handles->length; i++) {
		struct handle *h = tn.handles->items[i];
		ext_foreign_toplevel_handle_v1_destroy(h->handle);
		free(h->identifier);
		free(h);
	}
	list_free(tn.handles);
	tn.handles = NULL;
}
