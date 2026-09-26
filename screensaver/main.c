/*
 * tilewin-screensaver: covers every screen with a screen saver until the
 * mouse moves or a key is pressed. tileWin starts it after "idle_timeout
 * screensaver" and ends it on input itself; started by hand (the Preview
 * button of the settings) it ends on the first input after a moment.
 *
 * It draws only while it runs, at most 30 times a second and only when the
 * compositor has shown the last picture, so a screen that is off costs nothing.
 */
#define _GNU_SOURCE // memfd_create
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
#include <sys/mman.h>
#include "pool-buffer.h"
#include "savers.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define FRAME_MS 33
#define GRACE_MS 1000    // input right after the start is the input that started it
#define MOVE_PIXELS 12   // a pointer that drifts less is not someone coming back
#define DESKTOP_WAIT_MS 500 // how long the picture of the screen may take before it starts
#define DESKTOP_SETTLE_MS 150 // for the windows to draw themselves without the focus

struct output {
	struct wl_output *wl_output;
	uint32_t global_name;
	char *name;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct pool_buffer buffers[2];
	struct saver_run *run;
	int width, height;
	bool configured, frame_pending;
	struct timespec last_frame;
	cairo_surface_t *desktop; // the screen before the saver covered it
	struct zwlr_screencopy_frame_v1 *copy;
	struct wl_buffer *copy_buffer;
	void *copy_data;
	size_t copy_size;
	uint32_t copy_format, copy_width, copy_height, copy_stride, copy_flags;
	struct wl_list link;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_screencopy_manager_v1 *screencopy;
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
		struct saver_options options = ss.options;
		options.output = o->name; // Diggers asks tileWin for the windows on it
		options.desktop = o->desktop; // and brings back pieces of them
		o->run = saver_run_new(ss.saver, o->width, o->height, &options);
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

/* ---------- the picture of the screen ---------- */

/*
 * Taken once, before the saver covers the screen, for a saver that shows the
 * windows (Diggers flies pieces of them in, Hellfire burns them). Compositors
 * hand it over in the byte order of their renderer: XRGB, or XBGR with a GPU,
 * or ten bits a colour on a deep screen; all of them become cairo's RGB24.
 */
static bool copy_format_known(uint32_t format) {
	switch (format) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_XRGB2101010:
	case WL_SHM_FORMAT_ARGB2101010:
	case WL_SHM_FORMAT_XBGR2101010:
	case WL_SHM_FORMAT_ABGR2101010:
		return true;
	default:
		return false;
	}
}

/* One pixel of the copy as cairo's 0xffRRGGBB. */
static uint32_t copy_pixel(uint32_t format, uint32_t p) {
	uint32_t r, g, b;
	switch (format) {
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_ABGR8888:
		r = p & 0xff;
		g = (p >> 8) & 0xff;
		b = (p >> 16) & 0xff;
		break;
	case WL_SHM_FORMAT_XRGB2101010:
	case WL_SHM_FORMAT_ARGB2101010:
		r = (p >> 22) & 0xff;
		g = (p >> 12) & 0xff;
		b = (p >> 2) & 0xff;
		break;
	case WL_SHM_FORMAT_XBGR2101010:
	case WL_SHM_FORMAT_ABGR2101010:
		r = (p >> 2) & 0xff;
		g = (p >> 12) & 0xff;
		b = (p >> 22) & 0xff;
		break;
	default: // XRGB8888, ARGB8888: already cairo's
		return p | 0xff000000u;
	}
	return 0xff000000u | r << 16 | g << 8 | b;
}
static void copy_done(struct output *o) {
	if (o->copy) {
		zwlr_screencopy_frame_v1_destroy(o->copy);
		o->copy = NULL;
	}
	if (o->copy_buffer) {
		wl_buffer_destroy(o->copy_buffer);
		o->copy_buffer = NULL;
	}
	if (o->copy_data) {
		munmap(o->copy_data, o->copy_size);
		o->copy_data = NULL;
	}
}

static void copy_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height, uint32_t stride) {
	struct output *o = data;
	if (o->copy_buffer || !copy_format_known(format)) {
		return;
	}
	size_t size = (size_t)stride * height;
	int fd = memfd_create("tilewin-screensaver", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		close(fd);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(ss.shm, fd, size);
	o->copy_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	o->copy_data = mem;
	o->copy_size = size;
	o->copy_format = format;
	o->copy_width = width;
	o->copy_height = height;
	o->copy_stride = stride;
}

static void copy_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct output *o = data;
	o->copy_flags = flags;
}

static void copy_ready(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t sec_hi,
		uint32_t sec_lo, uint32_t nsec) {
	struct output *o = data;
	o->desktop = cairo_image_surface_create(CAIRO_FORMAT_RGB24, o->copy_width, o->copy_height);
	uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(o->desktop);
	int dst_stride = cairo_image_surface_get_stride(o->desktop) / 4;
	bool invert = o->copy_flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
	for (uint32_t y = 0; y < o->copy_height; y++) {
		const uint32_t *src = (const uint32_t *)((const char *)o->copy_data +
			(size_t)(invert ? o->copy_height - 1 - y : y) * o->copy_stride);
		uint32_t *row = dst + (size_t)y * dst_stride;
		for (uint32_t x = 0; x < o->copy_width; x++) {
			row[x] = copy_pixel(o->copy_format, src[x]);
		}
	}
	cairo_surface_mark_dirty(o->desktop);
	copy_done(o);
}

static void copy_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	copy_done(data);
}

static void copy_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	struct output *o = data;
	if (o->copy_buffer) {
		zwlr_screencopy_frame_v1_copy(frame, o->copy_buffer);
	} else {
		copy_done(o); // nothing cairo can read
	}
}

static void copy_damage(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t x,
		uint32_t y, uint32_t width, uint32_t height) {
}

static void copy_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
}

static const struct zwlr_screencopy_frame_v1_listener copy_listener = {
	.buffer = copy_buffer,
	.flags = copy_flags,
	.ready = copy_ready,
	.failed = copy_failed,
	.damage = copy_damage,
	.linux_dmabuf = copy_linux_dmabuf,
	.buffer_done = copy_buffer_done,
};

/* Handles what comes in for up to ms, or until done() says so. */
static void dispatch_for(long ms, bool (*done)(void)) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		long left = ms - ms_since(&start);
		if ((done && done()) || left <= 0) {
			return;
		}
		wl_display_flush(ss.display);
		struct pollfd fd = { wl_display_get_fd(ss.display), POLLIN, 0 };
		if (poll(&fd, 1, left) > 0 && wl_display_dispatch(ss.display) < 0) {
			return;
		}
	}
}

static bool copies_done(void) {
	struct output *o;
	wl_list_for_each(o, &ss.outputs, link) {
		if (o->copy) {
			return false;
		}
	}
	return true;
}

/*
 * Pictures of all screens, waited for a moment; a screen without one gets none.
 * The saver's surfaces are up first, but clear: the keyboard has gone to them
 * and the windows had a moment to draw themselves unfocused, so the picture
 * shows them as they look under the saver.
 */
static void capture_desktops(void) {
	if (!ss.screencopy || !ss.saver->wants_desktop) {
		return;
	}
	struct output *o;
	wl_list_for_each(o, &ss.outputs, link) {
		struct pool_buffer *buffer = o->configured ?
			get_next_buffer(ss.shm, o->buffers, o->width, o->height) : NULL;
		if (buffer) {
			cairo_save(buffer->cairo);
			cairo_set_operator(buffer->cairo, CAIRO_OPERATOR_CLEAR);
			cairo_paint(buffer->cairo);
			cairo_restore(buffer->cairo);
			cairo_surface_flush(buffer->surface);
			wl_surface_attach(o->surface, buffer->buffer, 0, 0);
			wl_surface_damage_buffer(o->surface, 0, 0, o->width, o->height);
			wl_surface_commit(o->surface);
		}
	}
	dispatch_for(DESKTOP_SETTLE_MS, NULL);
	wl_list_for_each(o, &ss.outputs, link) {
		o->copy = zwlr_screencopy_manager_v1_capture_output(ss.screencopy, 0, o->wl_output);
		zwlr_screencopy_frame_v1_add_listener(o->copy, &copy_listener, o);
	}
	dispatch_for(DESKTOP_WAIT_MS, copies_done);
	wl_list_for_each(o, &ss.outputs, link) {
		copy_done(o); // too slow: without a picture
	}
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
	copy_done(o);
	if (o->desktop) {
		cairo_surface_destroy(o->desktop);
	}
	destroy_buffer(&o->buffers[0]);
	destroy_buffer(&o->buffers[1]);
	if (o->layer) {
		zwlr_layer_surface_v1_destroy(o->layer);
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
	}
	wl_output_destroy(o->wl_output);
	free(o->name);
	free(o);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
}

static void output_done(void *data, struct wl_output *wl_output) {
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
	struct output *o = data;
	free(o->name);
	o->name = strdup(name);
}

static void output_description(void *data, struct wl_output *wl_output,
		const char *description) {
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

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
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		ss.screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface,
			version < 3 ? version : 3);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !ss.seat) {
		ss.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(ss.seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		o->global_name = name;
		o->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &output_listener, o);
		wl_list_insert(ss.outputs.prev, &o->link);
		if (ss.running) {
			create_surface(o); // a screen plugged in while the saver runs
		}
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
	capture_desktops();
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
