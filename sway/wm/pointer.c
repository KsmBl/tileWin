/*
 * Pointer effects. Tapping Ctrl draws gray rings that shrink onto the pointer,
 * like "Show location of pointer when I press the CTRL key" on Windows. Themes
 * turn it on with "pointer { locate yes; locate_color <color> }".
 *
 * "pointer_trail <count>" leaves that many copies of the pointer behind it
 * while it moves, like the mouse trails of Windows.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#include "sway/config.h"
#include "sway/input/cursor.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/root.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAME_MS 8
#define TAP_MS 400      // longest press that still counts as a tap
#define RINGS 3
#define RING_MS 420     // how long one ring takes to reach the pointer
#define STAGGER_MS 110  // between two rings
#define MAX_RADIUS 92   // logical pixels the outermost ring starts at
#define SIZE (2 * (MAX_RADIUS + 6))
#define LOCATE_MS (RING_MS + (RINGS - 1) * STAGGER_MS)

#define TRAIL_MAX 20
#define TRAIL_GAP 7.0     // logical pixels between two copies
#define TRAIL_HOLD_MS 130 // the trail is gone that long after the pointer stops

struct trail_point {
	double x, y;
};

static struct {
	// the Ctrl key that is held, 0 when none is or the tap was cancelled
	xkb_keysym_t held;
	struct timespec pressed_at;

	bool running;
	struct timespec start;
	double x, y; // where the pointer was when the rings started
	double scale;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *buffer;
	struct wl_event_source *timer;

	// the copies of the pointer that follow it
	struct trail_point trail[TRAIL_MAX];
	int trail_length;
	struct wlr_scene_tree *trail_tree;
	struct wlr_scene_buffer *trail_nodes[TRAIL_MAX];
	struct wlr_buffer *trail_image; // the pointer image the copies show
	char trail_name[64];            // the cursor it was made from
	double trail_scale;
	int image_width, image_height, image_hotspot_x, image_hotspot_y;
	struct wl_event_source *trail_timer;
} state;

static double elapsed_ms(struct timespec *since) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since->tv_sec) * 1000.0 +
		(now.tv_nsec - since->tv_nsec) / 1000000.0;
}

static void schedule_frames(void) {
	for (int i = 0; root && i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (output->wlr_output && output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void stop(void) {
	state.running = false;
	if (state.tree) {
		wlr_scene_node_destroy(&state.tree->node);
		state.tree = NULL;
		state.buffer = NULL;
	}
}

static void set_source(cairo_t *cr, uint32_t color, double alpha) {
	cairo_set_source_rgba(cr, (color >> 24 & 0xff) / 255.0, (color >> 16 & 0xff) / 255.0,
		(color >> 8 & 0xff) / 255.0, (color & 0xff) / 255.0 * alpha);
}

/* One frame of the rings, ms into the effect. */
static void draw(double ms) {
	if (!state.buffer) {
		return;
	}
	uint32_t color = tw_theme_color(tw_theme, "pointer.locate_color", 0xf0f0f0ff);
	int pixels = (int)ceil(SIZE * state.scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, state.scale, state.scale);
	double c = SIZE / 2.0;
	for (int i = 0; i < RINGS; i++) {
		double t = (ms - i * STAGGER_MS) / RING_MS;
		if (t < 0 || t > 1) {
			continue;
		}
		double radius = MAX_RADIUS * (1 - t) + 4;
		// fade in quickly, then out while the ring closes in
		double alpha = t < 0.15 ? t / 0.15 : 1 - (t - 0.15) / 0.85;
		cairo_new_path(cr);
		cairo_arc(cr, c, c, radius, 0, 2 * M_PI);
		// a dark line under the bright one keeps the ring visible on any wallpaper
		cairo_set_line_width(cr, 5);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * alpha);
		cairo_stroke_preserve(cr);
		cairo_set_line_width(cr, 2.5);
		set_source(cr, color, alpha);
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	tw_scene_buffer_set_surface(state.buffer, surface, SIZE, SIZE);
	wlr_scene_node_set_position(&state.tree->node, (int)round(state.x - c),
		(int)round(state.y - c));
}

static int tick(void *data) {
	if (!state.running) {
		return 0;
	}
	double ms = elapsed_ms(&state.start);
	if (ms >= LOCATE_MS) {
		stop();
		schedule_frames();
		return 0;
	}
	draw(ms);
	schedule_frames();
	wl_event_source_timer_update(state.timer, FRAME_MS);
	return 0;
}

static bool locate_enabled(void) {
	return tw_theme && tw_theme_bool(tw_theme, "pointer.locate", false) &&
		server.wl_event_loop && root;
}

/* Starts the rings at the pointer. */
static void locate(void) {
	struct sway_seat *seat = input_manager_current_seat();
	if (!seat || !seat->cursor || !seat->cursor->cursor) {
		return;
	}
	state.x = seat->cursor->cursor->x;
	state.y = seat->cursor->cursor->y;
	struct wlr_output *output = root->output_layout ?
		wlr_output_layout_output_at(root->output_layout, state.x, state.y) : NULL;
	state.scale = output && output->scale > 0 ? output->scale : 1;

	if (!state.timer) {
		state.timer = wl_event_loop_add_timer(server.wl_event_loop, tick, NULL);
		if (!state.timer) {
			return;
		}
	}
	if (!state.tree) {
		state.tree = wlr_scene_tree_create(root->layers.seat);
		if (!state.tree) {
			return;
		}
		state.buffer = wlr_scene_buffer_create(state.tree, NULL);
		if (!state.buffer) {
			stop();
			return;
		}
	}
	state.running = true;
	clock_gettime(CLOCK_MONOTONIC, &state.start);
	draw(0);
	wl_event_source_timer_update(state.timer, FRAME_MS);
}

/*
 * Ctrl on its own shows the pointer; held together with anything else it is a
 * shortcut, so the tap is dropped.
 */
void tw_pointer_key(xkb_keysym_t sym, bool pressed) {
	bool ctrl = sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R;
	if (!ctrl) {
		if (pressed) {
			state.held = 0;
		}
		return;
	}
	if (pressed) {
		state.held = sym;
		clock_gettime(CLOCK_MONOTONIC, &state.pressed_at);
		return;
	}
	if (state.held == sym && elapsed_ms(&state.pressed_at) < TAP_MS && locate_enabled()) {
		locate();
	}
	state.held = 0;
}

void tw_pointer_cancel_tap(void) {
	state.held = 0;
}

/* ---------- the trail of pointers ---------- */

static int trail_count(void) {
	int count = config ? config->tw_pointer_trail : 0;
	return count < 0 ? 0 : count > TRAIL_MAX ? TRAIL_MAX : count;
}

static void trail_clear(void) {
	state.trail_length = 0;
	if (state.trail_tree) {
		wlr_scene_node_destroy(&state.trail_tree->node);
		state.trail_tree = NULL;
	}
	for (int i = 0; i < TRAIL_MAX; i++) {
		state.trail_nodes[i] = NULL;
	}
}

static void trail_free_image(void) {
	if (state.trail_image) {
		wlr_buffer_drop(state.trail_image);
		state.trail_image = NULL;
	}
	state.trail_name[0] = '\0';
}

/*
 * The image of the cursor the copies show, from the cursor theme. It is
 * remade when the cursor or the scale of the screen under it changes.
 */
static bool trail_load_image(struct sway_cursor *cursor, double scale) {
	const char *name = cursor->image ? cursor->image : "default";
	if (state.trail_image && state.trail_scale == scale &&
			strcmp(state.trail_name, name) == 0) {
		return true;
	}
	if (!cursor->xcursor_manager) {
		return false;
	}
	wlr_xcursor_manager_load(cursor->xcursor_manager, scale);
	struct wlr_xcursor *xcursor =
		wlr_xcursor_manager_get_xcursor(cursor->xcursor_manager, name, scale);
	if (!xcursor && strcmp(name, "default") != 0) {
		xcursor = wlr_xcursor_manager_get_xcursor(cursor->xcursor_manager, "default", scale);
	}
	if (!xcursor || xcursor->image_count == 0) {
		return false;
	}
	struct wlr_xcursor_image *image = xcursor->images[0];
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		image->width, image->height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return false;
	}
	// both are premultiplied ARGB8888, but the strides need not match
	unsigned char *dst = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	for (uint32_t row = 0; row < image->height; row++) {
		memcpy(dst + (size_t)row * stride, image->buffer + (size_t)row * image->width * 4,
			(size_t)image->width * 4);
	}
	cairo_surface_mark_dirty(surface);

	trail_free_image();
	state.trail_image = tw_buffer_from_surface(surface); // takes the surface
	if (!state.trail_image) {
		return false;
	}
	snprintf(state.trail_name, sizeof(state.trail_name), "%s", name);
	state.trail_scale = scale;
	state.image_width = image->width;
	state.image_height = image->height;
	state.image_hotspot_x = image->hotspot_x;
	state.image_hotspot_y = image->hotspot_y;
	return true;
}

static void trail_place(void) {
	int count = trail_count();
	double scale = state.trail_scale > 0 ? state.trail_scale : 1;
	double w = state.image_width / scale, h = state.image_height / scale;
	double hx = state.image_hotspot_x / scale, hy = state.image_hotspot_y / scale;
	for (int i = 0; i < count; i++) {
		struct wlr_scene_buffer *node = state.trail_nodes[i];
		if (!node) {
			continue;
		}
		bool shown = i < state.trail_length;
		wlr_scene_node_set_enabled(&node->node, shown);
		if (!shown) {
			continue;
		}
		// the copies further behind are fainter, so the trail shows the way
		float alpha = (float)(0.8 - 0.55 * i / (double)count);
		wlr_scene_buffer_set_opacity(node, alpha < 0.1f ? 0.1f : alpha);
		wlr_scene_buffer_set_dest_size(node, (int)round(w), (int)round(h));
		wlr_scene_node_set_position(&node->node, (int)round(state.trail[i].x - hx),
			(int)round(state.trail[i].y - hy));
	}
}

static int trail_expired(void *data) {
	state.trail_timer = NULL;
	state.trail_length = 0;
	trail_place();
	schedule_frames();
	return 0;
}

/* Called for every movement of the pointer. */
void tw_pointer_moved(struct sway_cursor *cursor) {
	int count = trail_count();
	if (count == 0 || !cursor || !cursor->cursor || cursor->hidden || !root ||
			server.session_lock.lock) {
		if (state.trail_tree) {
			trail_clear();
		}
		return;
	}
	double x = cursor->cursor->x, y = cursor->cursor->y;
	struct wlr_output *output = root->output_layout ?
		wlr_output_layout_output_at(root->output_layout, x, y) : NULL;
	double scale = output && output->scale > 0 ? output->scale : 1;
	if (!trail_load_image(cursor, scale)) {
		return;
	}
	if (!state.trail_tree) {
		state.trail_tree = wlr_scene_tree_create(root->layers.seat);
		if (!state.trail_tree) {
			return;
		}
	}
	// the newest copy is the one closest to the pointer, so build from the back
	for (int i = count - 1; i >= 0; i--) {
		if (state.trail_nodes[i]) {
			continue;
		}
		state.trail_nodes[i] = wlr_scene_buffer_create(state.trail_tree, state.trail_image);
		if (!state.trail_nodes[i]) {
			trail_clear();
			return;
		}
		wlr_scene_buffer_set_filter_mode(state.trail_nodes[i], WLR_SCALE_FILTER_BILINEAR);
	}
	for (int i = 0; i < TRAIL_MAX; i++) {
		if (i >= count && state.trail_nodes[i]) {
			wlr_scene_node_destroy(&state.trail_nodes[i]->node);
			state.trail_nodes[i] = NULL;
		} else if (i < count) {
			wlr_scene_buffer_set_buffer(state.trail_nodes[i], state.trail_image);
		}
	}

	double dx = state.trail_length ? x - state.trail[0].x : TRAIL_GAP;
	double dy = state.trail_length ? y - state.trail[0].y : TRAIL_GAP;
	if (dx * dx + dy * dy >= TRAIL_GAP * TRAIL_GAP) {
		for (int i = TRAIL_MAX - 1; i > 0; i--) {
			state.trail[i] = state.trail[i - 1];
		}
		state.trail[0] = (struct trail_point){ x, y };
		if (state.trail_length < count) {
			state.trail_length++;
		}
	}
	trail_place();
	schedule_frames();

	if (!state.trail_timer && server.wl_event_loop) {
		state.trail_timer = wl_event_loop_add_timer(server.wl_event_loop, trail_expired, NULL);
	}
	if (state.trail_timer) {
		wl_event_source_timer_update(state.trail_timer, TRAIL_HOLD_MS);
	}
}

void tw_pointer_trail_changed(void) {
	trail_clear();
	trail_free_image();
}

void tw_pointer_fini(void) {
	stop();
	trail_clear();
	trail_free_image();
	if (state.timer) {
		wl_event_source_remove(state.timer);
		state.timer = NULL;
	}
	if (state.trail_timer) {
		wl_event_source_remove(state.trail_timer);
		state.trail_timer = NULL;
	}
}
