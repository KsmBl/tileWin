/*
 * Pointer effects. Tapping Ctrl draws gray rings that shrink onto the pointer,
 * like "Show location of pointer when I press the CTRL key" on Windows. Themes
 * turn it on with "pointer { locate yes; locate_color <color> }".
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
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

void tw_pointer_fini(void) {
	stop();
	if (state.timer) {
		wl_event_source_remove(state.timer);
		state.timer = NULL;
	}
}
