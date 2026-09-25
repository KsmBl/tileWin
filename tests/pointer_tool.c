/*
 * Drives a compositor with a pointer of its own, so that a test can work the
 * settings app the way a person would.
 *
 *   pointer-tool <width> <height> <step>...
 *
 * where a step is one of
 *   move <x> <y>                  put the pointer there
 *   click <x> <y>                 put it there and press the left button
 *   scroll <x> <y> <notches>      put it there and turn the wheel
 *   shake <swings> <steps> <px> <ms>   swing it back and forth
 *   throw <x1> <y1> <x2> <y2>     press at the first point, sweep to the second
 *                                 and let go while still moving
 *   drag <x1> <y1> <x2> <y2>      press at the first point, carry it slowly to
 *                                 the second, hold still there and let go
 *
 * The width and height are those of the screen the coordinates are in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;
static struct wl_display *display;
static struct zwlr_virtual_pointer_v1 *pointer;
static uint32_t width, height;

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	handle_global, handle_global_remove,
};

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void rest(long ms) {
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&t, NULL);
}

static void move(int x, int y) {
	zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), x, y, width, height);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(300);
}

static void click(void) {
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(80);
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(900);
}

static void scroll(int notches) {
	int steps = notches < 0 ? -notches : notches;
	for (int i = 0; i < steps; i++) {
		zwlr_virtual_pointer_v1_axis_discrete(pointer, now_ms(),
			WL_POINTER_AXIS_VERTICAL_SCROLL,
			wl_fixed_from_int(notches > 0 ? 15 : -15), notches > 0 ? 1 : -1);
		zwlr_virtual_pointer_v1_frame(pointer);
		wl_display_flush(display);
		rest(50);
	}
	rest(600);
}

/* Press, sweep, and release without stopping: what a thrown window needs. */
static void throw_window(int x1, int y1, int x2, int y2) {
	move(x1, y1); // move() rests long enough for the first report to be taken
	move(x1, y1);
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(100);
	for (int i = 1; i <= 8; i++) {
		zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
			x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8, width, height);
		zwlr_virtual_pointer_v1_frame(pointer);
		wl_display_flush(display);
		rest(25);
	}
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(200);
}

/* Press, carry and release after a pause: a drag and drop, which the app only
 * takes once the pointer has crossed its threshold and rested over the target. */
static void drag(int x1, int y1, int x2, int y2) {
	move(x1, y1);
	move(x1, y1);
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(150);
	for (int i = 1; i <= 30; i++) {
		zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
			x1 + (x2 - x1) * i / 30, y1 + (y2 - y1) * i / 30, width, height);
		zwlr_virtual_pointer_v1_frame(pointer);
		wl_display_flush(display);
		rest(30);
	}
	rest(500);
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(display);
	rest(900);
}

static void shake(int swings, int steps, int pixels, int ms) {
	for (int swing = 0; swing < swings; swing++) {
		int direction = swing % 2 ? -1 : 1;
		for (int step = 0; step < steps; step++) {
			zwlr_virtual_pointer_v1_motion(pointer, now_ms(),
				wl_fixed_from_int(direction * pixels), wl_fixed_from_int(0));
			zwlr_virtual_pointer_v1_frame(pointer);
			wl_display_flush(display);
			rest(ms);
		}
	}
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <width> <height> <step>...\n", argv[0]);
		return 2;
	}
	width = atoi(argv[1]);
	height = atoi(argv[2]);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "there is no display to drive\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "the compositor offers no virtual pointer\n");
		return 1;
	}
	pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
	wl_display_roundtrip(display);

	for (int i = 3; i < argc; ) {
		if (strcmp(argv[i], "move") == 0 && i + 2 < argc) {
			move(atoi(argv[i + 1]), atoi(argv[i + 2]));
			i += 3;
		} else if (strcmp(argv[i], "click") == 0 && i + 2 < argc) {
			move(atoi(argv[i + 1]), atoi(argv[i + 2]));
			click();
			i += 3;
		} else if (strcmp(argv[i], "scroll") == 0 && i + 3 < argc) {
			move(atoi(argv[i + 1]), atoi(argv[i + 2]));
			scroll(atoi(argv[i + 3]));
			i += 4;
		} else if (strcmp(argv[i], "throw") == 0 && i + 4 < argc) {
			throw_window(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3]),
				atoi(argv[i + 4]));
			i += 5;
		} else if (strcmp(argv[i], "drag") == 0 && i + 4 < argc) {
			drag(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3]), atoi(argv[i + 4]));
			i += 5;
		} else if (strcmp(argv[i], "shake") == 0 && i + 4 < argc) {
			shake(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3]),
				atoi(argv[i + 4]));
			i += 5;
		} else {
			fprintf(stderr, "cannot read the step at \"%s\"\n", argv[i]);
			return 2;
		}
	}
	wl_display_roundtrip(display);
	rest(600);
	zwlr_virtual_pointer_v1_destroy(pointer);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
