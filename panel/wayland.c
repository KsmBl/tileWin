#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-cursor.h>
#include "log.h"
#include "panel.h"

/* ---------- pointer ---------- */

static struct psurface *surface_from_wl(struct wl_surface *surface) {
	return surface ? wl_surface_get_user_data(surface) : NULL;
}

static void set_default_cursor(struct panel_seat *seat, struct wl_pointer *pointer,
		uint32_t serial) {
	struct panel *panel = seat->panel;
	if (panel->cursor_shape_manager) {
		struct wp_cursor_shape_device_v1 *device =
			wp_cursor_shape_manager_v1_get_pointer(panel->cursor_shape_manager, pointer);
		wp_cursor_shape_device_v1_set_shape(device, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
		return;
	}
	if (!seat->cursor_theme) {
		const char *size_env = getenv("XCURSOR_SIZE");
		int size = size_env ? atoi(size_env) : 24;
		seat->cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
			size > 0 ? size : 24, panel->shm);
	}
	if (!seat->cursor_theme) {
		return;
	}
	struct wl_cursor *cursor = wl_cursor_theme_get_cursor(seat->cursor_theme, "default");
	if (!cursor) {
		cursor = wl_cursor_theme_get_cursor(seat->cursor_theme, "left_ptr");
	}
	if (!cursor) {
		return;
	}
	if (!seat->cursor_surface) {
		seat->cursor_surface = wl_compositor_create_surface(panel->compositor);
	}
	struct wl_cursor_image *image = cursor->images[0];
	wl_surface_attach(seat->cursor_surface, wl_cursor_image_get_buffer(image), 0, 0);
	wl_surface_damage_buffer(seat->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(seat->cursor_surface);
	wl_pointer_set_cursor(pointer, serial, seat->cursor_surface,
		image->hotspot_x, image->hotspot_y);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
	struct panel_seat *seat = data;
	seat->pointer_focus = surface_from_wl(surface);
	seat->enter_serial = serial;
	seat->px = wl_fixed_to_double(sx);
	seat->py = wl_fixed_to_double(sy);
	set_default_cursor(seat, pointer, serial);
	struct psurface *s = seat->pointer_focus;
	if (s && s->impl && s->impl->pointer_motion) {
		s->impl->pointer_motion(s, seat->px, seat->py);
	}
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface) {
	struct panel_seat *seat = data;
	struct psurface *s = seat->pointer_focus;
	seat->pointer_focus = NULL;
	if (s && s->impl && s->impl->pointer_leave) {
		s->impl->pointer_leave(s);
	}
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy) {
	struct panel_seat *seat = data;
	seat->px = wl_fixed_to_double(sx);
	seat->py = wl_fixed_to_double(sy);
	struct psurface *s = seat->pointer_focus;
	if (s && s->impl && s->impl->pointer_motion) {
		s->impl->pointer_motion(s, seat->px, seat->py);
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	struct panel_seat *seat = data;
	struct psurface *s = seat->pointer_focus;
	if (s && s->impl && s->impl->pointer_button) {
		s->impl->pointer_button(s, seat->px, seat->py, button,
			state == WL_POINTER_BUTTON_STATE_PRESSED);
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	struct panel_seat *seat = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	seat->axis_accum += wl_fixed_to_double(value);
	struct psurface *s = seat->pointer_focus;
	while (seat->axis_accum >= 10 || seat->axis_accum <= -10) {
		int dir = seat->axis_accum > 0 ? 1 : -1;
		seat->axis_accum -= dir * 10;
		if (s && s->impl && s->impl->pointer_axis) {
			s->impl->pointer_axis(s, seat->px, seat->py, dir);
		}
	}
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis) {
	struct panel_seat *seat = data;
	seat->axis_accum = 0;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t discrete) {
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

/* ---------- keyboard ---------- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct panel_seat *seat = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	if (!seat->xkb_context) {
		seat->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(seat->xkb_context, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		return;
	}
	if (seat->xkb_state) {
		xkb_state_unref(seat->xkb_state);
	}
	if (seat->xkb_keymap) {
		xkb_keymap_unref(seat->xkb_keymap);
	}
	seat->xkb_keymap = keymap;
	seat->xkb_state = xkb_state_new(keymap);
}

static void stop_repeat(struct panel_seat *seat) {
	if (seat->repeat_timer) {
		loop_remove_timer(seat->panel->loop, seat->repeat_timer);
		seat->repeat_timer = NULL;
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
	struct panel_seat *seat = data;
	seat->keyboard_focus = surface_from_wl(surface);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface) {
	struct panel_seat *seat = data;
	stop_repeat(seat);
	struct psurface *s = seat->keyboard_focus;
	seat->keyboard_focus = NULL;
	if (s && s->impl && s->impl->keyboard_leave) {
		s->impl->keyboard_leave(s);
	}
}

static void dispatch_key(struct panel_seat *seat, uint32_t key) {
	struct psurface *s = seat->keyboard_focus;
	if (!s || !s->impl || !s->impl->key || !seat->xkb_state) {
		return;
	}
	xkb_keycode_t code = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->xkb_state, code);
	char utf8[16] = { 0 };
	xkb_state_key_get_utf8(seat->xkb_state, code, utf8, sizeof(utf8));
	uint32_t mods = 0;
	if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_EFFECTIVE) > 0) {
		mods |= 1;
	}
	if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_EFFECTIVE) > 0) {
		mods |= 2;
	}
	s->impl->key(s, sym, utf8, mods);
}

static void repeat_fire(void *data) {
	struct panel_seat *seat = data;
	seat->repeat_timer = NULL;
	if (!seat->keyboard_focus || seat->repeat_rate <= 0) {
		return;
	}
	dispatch_key(seat, seat->repeat_key);
	seat->repeat_timer = loop_add_timer(seat->panel->loop, 1000 / seat->repeat_rate,
		repeat_fire, seat);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	struct panel_seat *seat = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (key == seat->repeat_key) {
			stop_repeat(seat);
		}
		return;
	}
	stop_repeat(seat);
	dispatch_key(seat, key);
	if (seat->xkb_keymap && xkb_keymap_key_repeats(seat->xkb_keymap, key + 8) &&
			seat->repeat_rate > 0 && seat->keyboard_focus) {
		seat->repeat_key = key;
		seat->repeat_timer = loop_add_timer(seat->panel->loop,
			seat->repeat_delay > 0 ? seat->repeat_delay : 400, repeat_fire, seat);
	}
}

/*
 * Modifiers for clicks, e.g. Ctrl+click on a desktop icon. They are only known
 * while one of our surfaces has the keyboard, which it gets when clicked.
 */
uint32_t panel_modifiers(struct panel *panel) {
	struct panel_seat *seat;
	wl_list_for_each(seat, &panel->seats, link) {
		if (!seat->keyboard_focus || !seat->xkb_state) {
			continue;
		}
		uint32_t mods = 0;
		if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_CTRL,
				XKB_STATE_MODS_EFFECTIVE) > 0) {
			mods |= 1;
		}
		if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_SHIFT,
				XKB_STATE_MODS_EFFECTIVE) > 0) {
			mods |= 2;
		}
		return mods;
	}
	return 0;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group) {
	struct panel_seat *seat = data;
	if (seat->xkb_state) {
		xkb_state_update_mask(seat->xkb_state, depressed, latched, locked, 0, 0, group);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
		int32_t rate, int32_t delay) {
	struct panel_seat *seat = data;
	seat->repeat_rate = rate;
	seat->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* ---------- seat ---------- */

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t caps) {
	struct panel_seat *seat = data;
	bool pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	bool keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (pointer && !seat->pointer) {
		seat->pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);
	} else if (!pointer && seat->pointer) {
		wl_pointer_release(seat->pointer);
		seat->pointer = NULL;
		seat->pointer_focus = NULL;
	}
	if (keyboard && !seat->keyboard) {
		seat->keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
	} else if (!keyboard && seat->keyboard) {
		stop_repeat(seat);
		wl_keyboard_release(seat->keyboard);
		seat->keyboard = NULL;
		seat->keyboard_focus = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
}

const struct wl_seat_listener panel_seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

void panel_seat_destroy(struct panel_seat *seat) {
	stop_repeat(seat);
	if (seat->pointer) {
		wl_pointer_release(seat->pointer);
	}
	if (seat->keyboard) {
		wl_keyboard_release(seat->keyboard);
	}
	if (seat->cursor_surface) {
		wl_surface_destroy(seat->cursor_surface);
	}
	if (seat->cursor_theme) {
		wl_cursor_theme_destroy(seat->cursor_theme);
	}
	if (seat->xkb_state) {
		xkb_state_unref(seat->xkb_state);
	}
	if (seat->xkb_keymap) {
		xkb_keymap_unref(seat->xkb_keymap);
	}
	if (seat->xkb_context) {
		xkb_context_unref(seat->xkb_context);
	}
	wl_seat_destroy(seat->wl_seat);
	wl_list_remove(&seat->link);
	free(seat);
}

/* ---------- outputs ---------- */

static void output_ready_check(struct panel_output *output) {
	if (!output->ready && output->name && output->width > 0) {
		output->ready = true;
		panel_outputs_update_bars(output->panel);
	}
}

static void wl_output_geometry(void *data, struct wl_output *wl_output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	struct panel_output *output = data;
	output->subpixel = subpixel;
}

static void wl_output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
}

static void wl_output_done(void *data, struct wl_output *wl_output) {
}

static void wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	struct panel_output *output = data;
	if (output->scale != factor) {
		output->scale = factor;
		struct psurface *s;
		wl_list_for_each(s, &output->panel->surfaces, link) {
			if (s->output == output) {
				s->dirty = true;
				psurface_render(s);
			}
		}
	}
}

static void wl_output_name(void *data, struct wl_output *wl_output, const char *name) {
	struct panel_output *output = data;
	free(output->name);
	output->name = strdup(name);
}

static void wl_output_description(void *data, struct wl_output *wl_output,
		const char *description) {
}

static const struct wl_output_listener output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
	.name = wl_output_name,
	.description = wl_output_description,
};

static void xdg_output_position(void *data, struct zxdg_output_v1 *xdg_output,
		int32_t x, int32_t y) {
	struct panel_output *output = data;
	output->x = x;
	output->y = y;
}

static void xdg_output_size(void *data, struct zxdg_output_v1 *xdg_output,
		int32_t width, int32_t height) {
	struct panel_output *output = data;
	output->width = width;
	output->height = height;
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *xdg_output) {
	output_ready_check(data);
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output,
		const char *name) {
	struct panel_output *output = data;
	if (!output->name) {
		output->name = strdup(name);
	}
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *xdg_output,
		const char *description) {
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_position,
	.logical_size = xdg_output_size,
	.done = xdg_output_done,
	.name = xdg_output_name,
	.description = xdg_output_description,
};

static void add_xdg_output(struct panel *panel, struct panel_output *output) {
	if (output->xdg_output || !panel->xdg_output_manager) {
		return;
	}
	output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
		panel->xdg_output_manager, output->wl_output);
	zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
}

static void output_destroy(struct panel_output *output) {
	deskwidgets_output_gone(output);
	desktop_destroy(output);
	bar_destroy(output);
	struct psurface *s, *tmp;
	wl_list_for_each_safe(s, tmp, &output->panel->surfaces, link) {
		if (s->output == output) {
			s->output = NULL;
		}
	}
	if (output->xdg_output) {
		zxdg_output_v1_destroy(output->xdg_output);
	}
	wl_output_release(output->wl_output);
	wl_list_remove(&output->link);
	free(output->name);
	free(output);
}

/* ---------- registry ---------- */

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct panel *panel = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		panel->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		panel->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		panel->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		panel->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 2);
		struct panel_output *output;
		wl_list_for_each(output, &panel->outputs, link) {
			add_xdg_output(panel, output);
		}
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		panel->cursor_shape_manager = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		panel->screencopy_version = version < 3 ? version : 3;
		panel->screencopy = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, panel->screencopy_version);
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		panel->toplevel_list = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
		thumbnails_list_bound(panel);
	} else if (strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		panel->toplevel_capture = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		panel->copy_capture = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		panel->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct panel_seat *seat = calloc(1, sizeof(*seat));
		seat->panel = panel;
		seat->wl_name = name;
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(seat->wl_seat, &panel_seat_listener, seat);
		wl_list_insert(&panel->seats, &seat->link);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct panel_output *output = calloc(1, sizeof(*output));
		output->panel = panel;
		output->wl_name = name;
		output->scale = 1;
		output->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(panel->outputs.prev, &output->link);
		add_xdg_output(panel, output);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct panel *panel = data;
	struct panel_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &panel->outputs, link) {
		if (output->wl_name == name) {
			popup_close_all(panel);
			output_destroy(output);
			return;
		}
	}
	struct panel_seat *seat, *stmp;
	wl_list_for_each_safe(seat, stmp, &panel->seats, link) {
		if (seat->wl_name == name) {
			panel_seat_destroy(seat);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

bool panel_wayland_init(struct panel *panel) {
	wl_list_init(&panel->outputs);
	wl_list_init(&panel->seats);
	wl_list_init(&panel->surfaces);
	panel->display = wl_display_connect(NULL);
	if (!panel->display) {
		sway_log(SWAY_ERROR, "Unable to connect to the Wayland compositor");
		return false;
	}
	struct wl_registry *registry = wl_display_get_registry(panel->display);
	wl_registry_add_listener(registry, &registry_listener, panel);
	wl_display_roundtrip(panel->display);
	if (!panel->compositor || !panel->shm || !panel->layer_shell) {
		sway_log(SWAY_ERROR, "Compositor lacks wl_compositor, wl_shm or layer-shell");
		return false;
	}
	wl_display_roundtrip(panel->display);
	return true;
}

void panel_wayland_fini(struct panel *panel) {
	popup_close_all(panel);
	struct panel_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &panel->outputs, link) {
		output_destroy(output);
	}
	struct psurface *s, *stmp;
	wl_list_for_each_safe(s, stmp, &panel->surfaces, link) {
		psurface_destroy(s);
	}
	struct panel_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &panel->seats, link) {
		panel_seat_destroy(seat);
	}
	if (panel->display) {
		wl_display_flush(panel->display);
		wl_display_disconnect(panel->display);
	}
}

void panel_outputs_update_bars(struct panel *panel) {
	if (!panel->config) {
		return;
	}
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		if (!output->ready) {
			continue;
		}
		bool wanted = !panel->config->outputs;
		if (panel->config->outputs) {
			for (int i = 0; i < panel->config->outputs->length; i++) {
				const char *name = panel->config->outputs->items[i];
				if (strcmp(name, "*") == 0 || strcmp(name, output->name) == 0) {
					wanted = true;
					break;
				}
			}
		}
		if (wanted) {
			bar_destroy(output);
			bar_create(output);
		} else {
			bar_destroy(output);
		}
		if (panel->config->desktop_icons) {
			desktop_create(output);
		} else {
			desktop_destroy(output);
		}
	}
	// after the desktop icons, so the widgets lie over the surface that takes clicks
	deskwidgets_update(panel);
}
