/*
 * tilewin-screensaver: covers every screen with a screen saver until the
 * mouse moves or a key is pressed. tileWin starts it after "idle_timeout
 * screensaver" and ends it on input itself; started by hand (the Preview
 * button of the settings) it ends on the first input after a moment.
 *
 * It draws only while it runs, at most 30 times a second and only when the
 * compositor has shown the last picture, so a screen that is off costs nothing.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "pool-buffer.h"
#include "savers.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define FRAME_MS 33
#define GRACE_MS 1000    // input right after the start is the input that started it
#define MOVE_PIXELS 12   // a pointer that drifts less is not someone coming back

struct output {
	struct wl_output *wl_output;
	uint32_t global_name;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct pool_buffer buffers[2];
	struct saver_run *run;
	int width, height;
	bool configured, frame_pending;
	struct timespec last_frame;
	struct wl_list link;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_list outputs;
	const struct saver *saver;
	struct saver_options options;
	struct timespec started;
	double moved, last_x, last_y;
	bool have_pointer_position;
	volatile sig_atomic_t running;
} ss;

static long ms_between(const struct timespec *a, const struct timespec *b) {
	return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

static long ms_since(const struct timespec *t) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return ms_between(t, &now);
}

/* Someone is back: end, unless it is the input that started the saver. */
static void input(void) {
	if (ms_since(&ss.started) >= GRACE_MS) {
		ss.running = false;
	}
}

/* ---------- drawing ---------- */

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct output *o = data;
	wl_callback_destroy(callback);
	o->frame_pending = false;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void render(struct output *o) {
	if (!o->configured || o->frame_pending || o->width <= 0 || o->height <= 0) {
		return;
	}
	struct pool_buffer *buffer = get_next_buffer(ss.shm, o->buffers, o->width, o->height);
	if (!buffer) {
		return; // both still on screen: next time
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double dt = o->run ? ms_between(&o->last_frame, &now) / 1000.0 : 0;
	o->last_frame = now;
	if (!o->run) {
		o->run = saver_run_new(ss.saver, o->width, o->height, &ss.options);
	}
	saver_run_draw(o->run, buffer->cairo, dt);
	cairo_surface_flush(buffer->surface);
	wl_surface_attach(o->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, o->width, o->height);
	struct wl_callback *callback = wl_surface_frame(o->surface);
	wl_callback_add_listener(callback, &frame_listener, o);
	o->frame_pending = true;
	wl_surface_commit(o->surface);
}

/* ---------- the surfaces ---------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct output *o = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if ((int)width != o->width || (int)height != o->height) {
		o->width = width;
		o->height = height;
		saver_run_free(o->run); // a new size starts the saver over
		o->run = NULL;
	}
	o->configured = true;
}

static void destroy_output(struct output *o);

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	destroy_output(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void create_surface(struct output *o) {
	if (o->surface || !ss.compositor || !ss.layer_shell) {
		return;
	}
	o->surface = wl_compositor_create_surface(ss.compositor);
	o->layer = zwlr_layer_shell_v1_get_layer_surface(ss.layer_shell, o->surface, o->wl_output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-screensaver");
	zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
	zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1); // over the taskbar too
	zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE); // no key reaches a window
	wl_surface_commit(o->surface);
}

static void destroy_output(struct output *o) {
	wl_list_remove(&o->link);
	saver_run_free(o->run);
	destroy_buffer(&o->buffers[0]);
	destroy_buffer(&o->buffers[1]);
	if (o->layer) {
		zwlr_layer_surface_v1_destroy(o->layer);
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
	}
	wl_output_destroy(o->wl_output);
	free(o);
}

/* ---------- input ---------- */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	wl_pointer_set_cursor(pointer, serial, NULL, 0, 0); // no pointer over the saver
	ss.last_x = wl_fixed_to_double(x);
	ss.last_y = wl_fixed_to_double(y);
	ss.have_pointer_position = true;
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface) {
	ss.have_pointer_position = false;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
	if (ss.have_pointer_position) {
		ss.moved += fabs(px - ss.last_x) + fabs(py - ss.last_y);
	}
	ss.last_x = px;
	ss.last_y = py;
	ss.have_pointer_position = true;
	if (ss.moved > MOVE_PIXELS) {
		input();
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	input();
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	input();
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
		int32_t discrete) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface) {
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		input();
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
		int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ss.pointer) {
		ss.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ss.pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ss.keyboard) {
		ss.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(ss.keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------- globals ---------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		ss.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		ss.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		ss.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !ss.seat) {
		ss.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(ss.seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		o->global_name = name;
		o->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		wl_list_insert(ss.outputs.prev, &o->link);
		create_surface(o); // a screen plugged in while the saver runs
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &ss.outputs, link) {
		if (o->global_name == name) {
			destroy_output(o);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void handle_signal(int sig) {
	ss.running = false;
}

static void list_savers(void) {
	for (int i = 0; i < saver_count; i++) {
		printf("%-14s %s: %s\n", savers[i]->name, savers[i]->title, savers[i]->description);
	}
	printf("%-14s one of them, picked each time\n", "random");
}

int main(int argc, char **argv) {
	static const struct option long_options[] = {
		{ "saver", required_argument, NULL, 's' },
		{ "list", no_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	const char *wanted = NULL;
	int c;
	while ((c = getopt_long(argc, argv, "s:lh", long_options, NULL)) != -1) {
		switch (c) {
		case 's':
			wanted = optarg;
			break;
		case 'l':
			list_savers();
			return 0;
		case 'h':
		default:
			printf("Usage: tilewin-screensaver [--saver <name>] [--list]\n"
				"Shows a screen saver on every screen until the mouse moves or a key is\n"
				"pressed. Without --saver, the one of the \"screensaver\" block of\n"
				"taskbar.conf, or Bubbles.\n");
			return c == 'h' ? 0 : 1;
		}
	}
	setlocale(LC_ALL, "");
	char *configured = saver_options_load(&ss.options);
	ss.saver = saver_find(wanted ? wanted : configured ? configured : "bubbles");
	free(configured);
	if (!ss.saver) {
		fprintf(stderr, "tilewin-screensaver: no screen saver called '%s' (--list shows them)\n",
			wanted ? wanted : "?");
		return 1;
	}

	struct sigaction sa = { .sa_handler = handle_signal };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	wl_list_init(&ss.outputs);
	ss.display = wl_display_connect(NULL);
	if (!ss.display) {
		fprintf(stderr, "tilewin-screensaver: cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(ss.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(ss.display);
	if (!ss.compositor || !ss.shm || !ss.layer_shell) {
		fprintf(stderr, "tilewin-screensaver: the compositor has no layer shell\n");
		return 1;
	}
	struct output *o;
	wl_list_for_each(o, &ss.outputs, link) {
		create_surface(o);
	}
	wl_display_roundtrip(ss.display);
	clock_gettime(CLOCK_MONOTONIC, &ss.started);

	ss.running = true;
	struct timespec next_frame = ss.started;
	while (ss.running) {
		if (ms_since(&next_frame) >= 0) {
			wl_list_for_each(o, &ss.outputs, link) {
				render(o);
			}
			clock_gettime(CLOCK_MONOTONIC, &next_frame);
			next_frame.tv_nsec += FRAME_MS * 1000000L;
			if (next_frame.tv_nsec >= 1000000000L) {
				next_frame.tv_sec++;
				next_frame.tv_nsec -= 1000000000L;
			}
		}
		while (wl_display_prepare_read(ss.display) != 0) {
			wl_display_dispatch_pending(ss.display);
		}
		if (wl_display_flush(ss.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(ss.display);
			break;
		}
		long wait = -ms_since(&next_frame);
		struct pollfd fd = { .fd = wl_display_get_fd(ss.display), .events = POLLIN };
		int ret = poll(&fd, 1, wait > 0 ? (int)wait : 0);
		if (ret > 0 && (fd.revents & POLLIN)) {
			if (wl_display_read_events(ss.display) < 0) {
				break;
			}
		} else {
			wl_display_cancel_read(ss.display);
		}
		if (ret > 0 && (fd.revents & (POLLERR | POLLHUP))) {
			break;
		}
		if (wl_display_dispatch_pending(ss.display) < 0) {
			break;
		}
	}

	struct output *tmp;
	wl_list_for_each_safe(o, tmp, &ss.outputs, link) {
		destroy_output(o);
	}
	wl_display_flush(ss.display);
	wl_display_disconnect(ss.display);
	saver_options_finish(&ss.options);
	return 0;
}
