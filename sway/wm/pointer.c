/*
 * Pointer effects. Tapping Ctrl draws gray rings that shrink onto the pointer,
 * like "Show location of pointer when I press the CTRL key" on Windows. Themes
 * turn it on with "pointer { locate yes; locate_color <color> }".
 *
 * "pointer_trail <milliseconds>" leaves copies of the pointer behind it while
 * it moves, like the mouse trails of Windows. Each copy has its own countdown
 * and fades away when it runs out, so a copy lives just as long whether the
 * pointer raced past or crawled.
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

#define TRAIL_MAX 64     // copies that can be alive at once
#define TRAIL_GAP 6.0    // logical pixels the pointer moves between two copies
#define TRAIL_MIN_MS 12  // and the shortest time between two of them

struct trail_copy {
	double x, y;
	struct timespec born;
	struct wlr_scene_buffer *node;
	bool alive;
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

	// the copies of the pointer that follow it, oldest first in the ring
	struct trail_copy trail[TRAIL_MAX];
	int trail_head, trail_alive; // next slot to write, copies still alive
	struct wlr_scene_tree *trail_tree;
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
	if (!server.wl_event_loop || !root) {
		return false;
	}
	// "pointer_locate" wins over the theme when it was set
	if (config && config->tw_pointer_locate >= 0) {
		return config->tw_pointer_locate == 1;
	}
	return tw_theme && tw_theme_bool(tw_theme, "pointer.locate", false);
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

/* How long one copy of the pointer lives, in milliseconds; 0 turns it off. */
static int trail_lifetime(void) {
	int ms = config ? config->tw_pointer_trail : 0;
	return ms < 0 ? 0 : ms > 2000 ? 2000 : ms;
}

static void trail_clear(void) {
	state.trail_head = state.trail_alive = 0;
	if (state.trail_tree) {
		wlr_scene_node_destroy(&state.trail_tree->node);
		state.trail_tree = NULL;
	}
	for (int i = 0; i < TRAIL_MAX; i++) {
		state.trail[i].node = NULL;
		state.trail[i].alive = false;
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
static bool trail_load_image(struct sway_cursor *cursor, double scale, bool *changed) {
	const char *name = cursor->image ? cursor->image : "default";
	*changed = false;
	if (state.trail_image && state.trail_scale == scale &&
			strcmp(state.trail_name, name) == 0) {
		return true;
	}
	*changed = true;
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

/*
 * Ages every copy: what is past its lifetime goes away, the rest fades as its
 * countdown runs out. Returns how many are left.
 */
static int trail_age(void) {
	int lifetime = trail_lifetime();
	double scale = state.trail_scale > 0 ? state.trail_scale : 1;
	double w = state.image_width / scale, h = state.image_height / scale;
	double hx = state.image_hotspot_x / scale, hy = state.image_hotspot_y / scale;
	int alive = 0;
	for (int i = 0; i < TRAIL_MAX; i++) {
		struct trail_copy *copy = &state.trail[i];
		if (!copy->alive || !copy->node) {
			continue;
		}
		double left = lifetime > 0 ? 1 - elapsed_ms(&copy->born) / lifetime : 0;
		if (left <= 0) {
			copy->alive = false;
			wlr_scene_node_set_enabled(&copy->node->node, false);
			continue;
		}
		alive++;
		wlr_scene_node_set_enabled(&copy->node->node, true);
		wlr_scene_buffer_set_opacity(copy->node, (float)(0.85 * left));
		wlr_scene_buffer_set_dest_size(copy->node, (int)round(w), (int)round(h));
		wlr_scene_node_set_position(&copy->node->node, (int)round(copy->x - hx),
			(int)round(copy->y - hy));
	}
	state.trail_alive = alive;
	return alive;
}

static int trail_tick(void *data) {
	if (trail_age() > 0) {
		wl_event_source_timer_update(state.trail_timer, FRAME_MS);
	}
	schedule_frames();
	return 0;
}

/* Called for every movement of the pointer. */
void tw_pointer_moved(struct sway_cursor *cursor) {
	if (trail_lifetime() == 0 || !cursor || !cursor->cursor || cursor->hidden || !root ||
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
	bool image_changed = false;
	if (!trail_load_image(cursor, scale, &image_changed)) {
		return;
	}
	if (!state.trail_tree) {
		state.trail_tree = wlr_scene_tree_create(root->layers.seat);
		if (!state.trail_tree) {
			return;
		}
	}
	if (image_changed) {
		for (int i = 0; i < TRAIL_MAX; i++) {
			if (state.trail[i].node) {
				// setting the same buffer again would redraw every copy for nothing
				wlr_scene_buffer_set_buffer(state.trail[i].node, state.trail_image);
			}
		}
	}

	// a new copy once the pointer has come far enough, and not too soon after
	// the last one, so a fast pointer does not fill the ring in one sweep
	int newest = (state.trail_head + TRAIL_MAX - 1) % TRAIL_MAX;
	bool first = !state.trail[newest].alive;
	double dx = first ? TRAIL_GAP : x - state.trail[newest].x;
	double dy = first ? TRAIL_GAP : y - state.trail[newest].y;
	bool far_enough = dx * dx + dy * dy >= TRAIL_GAP * TRAIL_GAP;
	bool long_enough = first || elapsed_ms(&state.trail[newest].born) >= TRAIL_MIN_MS;
	if (far_enough && long_enough) {
		struct trail_copy *copy = &state.trail[state.trail_head];
		if (!copy->node) {
			copy->node = wlr_scene_buffer_create(state.trail_tree, state.trail_image);
			if (!copy->node) {
				trail_clear();
				return;
			}
			wlr_scene_buffer_set_filter_mode(copy->node, WLR_SCALE_FILTER_BILINEAR);
		}
		copy->x = x;
		copy->y = y;
		copy->alive = true;
		clock_gettime(CLOCK_MONOTONIC, &copy->born);
		wlr_scene_node_raise_to_top(&copy->node->node); // the newest one in front
		state.trail_head = (state.trail_head + 1) % TRAIL_MAX;
	}

	trail_age();
	schedule_frames();
	if (!state.trail_timer && server.wl_event_loop) {
		state.trail_timer = wl_event_loop_add_timer(server.wl_event_loop, trail_tick, NULL);
	}
	if (state.trail_timer) {
		wl_event_source_timer_update(state.trail_timer, FRAME_MS);
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
