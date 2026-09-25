/*
 * Types into a compositor with a keyboard of its own, so that a test can fill
 * a field in the way a person would.
 *
 *   keyboard-tool <step>...
 *
 * where a step is one of
 *   text <word>            types the word (lowercase letters and digits)
 *   key <mods> <name>      presses one key with those modifiers held, where
 *                          mods is ctrl, shift, ctrl+shift or none, and name
 *                          is a letter, a digit, "return", "tab", "escape" or
 *                          an arrow: "up", "down", "left", "right"
 *
 * The modifiers have to be announced as well as pressed: a virtual keyboard
 * that only sends the key events leaves the compositor thinking nothing is
 * held, and Ctrl+A arrives as a plain "a".
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define MOD_SHIFT (1 << 0)
#define MOD_CTRL (1 << 2)

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;
static struct wl_display *display;
static struct zwp_virtual_keyboard_v1 *keyboard;

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
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

static const struct {
	const char *name;
	uint32_t code;
} keys[] = {
	{ "a", KEY_A }, { "b", KEY_B }, { "c", KEY_C }, { "d", KEY_D }, { "e", KEY_E },
	{ "f", KEY_F }, { "g", KEY_G }, { "h", KEY_H }, { "i", KEY_I }, { "j", KEY_J },
	{ "k", KEY_K }, { "l", KEY_L }, { "m", KEY_M }, { "n", KEY_N }, { "o", KEY_O },
	{ "p", KEY_P }, { "q", KEY_Q }, { "r", KEY_R }, { "s", KEY_S }, { "t", KEY_T },
	{ "u", KEY_U }, { "v", KEY_V }, { "w", KEY_W }, { "x", KEY_X }, { "y", KEY_Y },
	{ "z", KEY_Z }, { "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 },
	{ "4", KEY_4 }, { "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 },
	{ "9", KEY_9 }, { "return", KEY_ENTER }, { "tab", KEY_TAB }, { "escape", KEY_ESC },
	{ "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
	{ "minus", KEY_MINUS }, { "slash", KEY_SLASH }, { "space", KEY_SPACE },
};

static bool code_of(const char *name, uint32_t *code) {
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strcmp(keys[i].name, name) == 0) {
			*code = keys[i].code;
			return true;
		}
	}
	return false;
}

static void press(uint32_t code, uint32_t mods) {
	zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, 0);
	zwp_virtual_keyboard_v1_key(keyboard, now_ms(), code, WL_KEYBOARD_KEY_STATE_PRESSED);
	wl_display_flush(display);
	rest(30);
	zwp_virtual_keyboard_v1_key(keyboard, now_ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
	wl_display_flush(display);
	rest(30);
}

static bool type_word(const char *word) {
	for (const char *c = word; *c; c++) {
		char name[2] = { *c, '\0' };
		uint32_t code;
		if (!code_of(name, &code)) {
			fprintf(stderr, "cannot type '%c'\n", *c);
			return false;
		}
		press(code, 0);
	}
	return true;
}

static bool send_keymap(void) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *keymap = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
	char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	bool ok = false;
	if (text) {
		size_t size = strlen(text) + 1;
		char path[] = "/tmp/tilewin-test-keymap-XXXXXX";
		int fd = mkstemp(path);
		unlink(path);
		if (fd >= 0 && write(fd, text, size) == (ssize_t)size) {
			zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				size);
			ok = true;
		}
		if (fd >= 0) {
			close(fd);
		}
		free(text);
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	return ok;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <step>...\n", argv[0]);
		return 2;
	}
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "there is no display to type into\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager || !seat) {
		fprintf(stderr, "the compositor offers no virtual keyboard\n");
		return 1;
	}
	keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
	if (!send_keymap()) {
		fprintf(stderr, "cannot hand the compositor a keymap\n");
		return 1;
	}
	wl_display_roundtrip(display);
	rest(300);

	for (int i = 1; i < argc; ) {
		if (strcmp(argv[i], "text") == 0 && i + 1 < argc) {
			if (!type_word(argv[i + 1])) {
				return 2;
			}
			i += 2;
		} else if (strcmp(argv[i], "key") == 0 && i + 2 < argc) {
			uint32_t mods = 0;
			if (strstr(argv[i + 1], "ctrl")) {
				mods |= MOD_CTRL;
			}
			if (strstr(argv[i + 1], "shift")) {
				mods |= MOD_SHIFT;
			}
			uint32_t code;
			if (!code_of(argv[i + 2], &code)) {
				fprintf(stderr, "cannot read the key \"%s\"\n", argv[i + 2]);
				return 2;
			}
			press(code, mods);
			i += 3;
		} else {
			fprintf(stderr, "cannot read the step at \"%s\"\n", argv[i]);
			return 2;
		}
	}
	wl_display_roundtrip(display);
	rest(500);
	zwp_virtual_keyboard_v1_destroy(keyboard);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
