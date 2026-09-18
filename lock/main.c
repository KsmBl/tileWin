/*
 * tilewin-lock: the lock screen of tileWin (Win+L).
 *
 * Locks the session with ext-session-lock-v1 and shows the wallpaper
 * darkened with the time and date, like Windows. Any key or click shows the
 * sign-in view with the user's picture, name and a password field. The
 * password is checked with PAM (service "tilewin-lock", or "login" when that
 * file is not installed) in a child process, so the screen keeps drawing.
 * If this program dies while the session is locked, the compositor keeps it
 * locked.
 *
 *   tilewin-lock [-f|--daemonize]   -f returns once the session is locked
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "ext-session-lock-v1-client-protocol.h"
#include "pool-buffer.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "tw_theme.h"

#define MESSAGE_MS 3000
#define BACK_TO_CLOCK_MS 30000

enum view {
	VIEW_CLOCK,
	VIEW_SIGN_IN,
};

struct lock_output {
	struct wl_output *wl_output;
	uint32_t global_name;
	int scale;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	int width, height;
	bool configured;
	struct pool_buffer buffers[2];
	cairo_surface_t *background; // wallpaper at the buffer size
	int bg_width, bg_height;
	struct wl_list link;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct ext_session_lock_manager_v1 *manager;
	struct ext_session_lock_v1 *lock;
	struct wl_list outputs; // lock_output::link
	bool locked, running, dirty;
	int ready_fd; // -f: tells the waiting parent that the session is locked

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;

	enum view view;
	char password[256];
	size_t password_len;
	bool checking;
	pid_t auth_pid;
	int auth_fd;
	char message[128];
	struct timespec message_until, last_input;
	int last_minute;

	char *user, *full_name;
	cairo_surface_t *avatar;
	struct tw_theme *theme;
	uint32_t accent;
} lock = { .ready_fd = -1, .auth_fd = -1 };

static long ms_since(const struct timespec *t) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void clear_password(void) {
	explicit_bzero(lock.password, sizeof(lock.password));
	lock.password_len = 0;
}

static void set_message(const char *text) {
	snprintf(lock.message, sizeof(lock.message), "%s", text ? text : "");
	clock_gettime(CLOCK_MONOTONIC, &lock.message_until);
	lock.dirty = true;
}

/* ---------- wallpaper ---------- */

/* The "wallpaper" line of common.conf, NULL for the theme's wallpaper. */
static char *wallpaper_setting(void) {
	char *dir = tw_config_dir();
	char *path = dir ? format_str("%s/common.conf", dir) : NULL;
	free(dir);
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	char *value = NULL, line[1024];
	while (f && fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (strncmp(p, "wallpaper", 9) == 0 && (p[9] == ' ' || p[9] == '\t')) {
			p[strcspn(p, "\r\n")] = '\0';
			free(value);
			value = strdup(p + 10);
		}
	}
	if (f) {
		fclose(f);
	}
	if (value && strncasecmp(value, "theme", 5) == 0) {
		free(value);
		value = NULL;
	}
	return value;
}

static void paint_colors(cairo_t *cr, int w, int h, uint32_t c1, uint32_t c2, bool vertical) {
	cairo_pattern_t *p = vertical ? cairo_pattern_create_linear(0, 0, 0, h) :
		cairo_pattern_create_linear(0, 0, w, 0);
	uint32_t cs[2] = { c1, c2 };
	for (int i = 0; i < 2; i++) {
		cairo_pattern_add_color_stop_rgb(p, i, (cs[i] >> 24 & 0xff) / 255.0,
			(cs[i] >> 16 & 0xff) / 255.0, (cs[i] >> 8 & 0xff) / 255.0);
	}
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
}

static cairo_surface_t *render_background(int w, int h) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(out);
	const struct tw_theme *t = lock.theme;
	uint32_t c1 = tw_theme_color(t, "wallpaper.color", 0x1c3a6eff);
	uint32_t c2 = tw_theme_color(t, "wallpaper.color2", c1);
	bool vertical = strcasecmp(tw_theme_str(t, "wallpaper.direction", "vertical"),
		"horizontal") != 0;
	char *image = NULL;
	char *setting = wallpaper_setting();
	if (setting) {
		int argc = 0;
		char **argv = split_args(setting, &argc);
		if (argc > 1 && strcasecmp(argv[0], "image") == 0) {
			image = tw_expand_home(argv[1]);
		} else if (argc > 1 && (strcasecmp(argv[0], "solid") == 0 ||
				strcasecmp(argv[0], "gradient") == 0)) {
			tw_parse_color(argv[1], &c1);
			c2 = c1;
			if (argc > 2) {
				tw_parse_color(argv[2], &c2);
			}
			vertical = !(argc > 3 && strcasecmp(argv[3], "horizontal") == 0);
		}
		free_argv(argc, argv);
		free(setting);
	} else if (t) {
		// ~/.config/tileWin/wallpapers/<theme>.<ext> replaces the theme's wallpaper
		static const char *exts[] = { "jpg", "jpeg", "png", "webp", "svg" };
		char *dir = tw_config_dir();
		for (size_t i = 0; dir && !image && i < sizeof(exts) / sizeof(exts[0]); i++) {
			char *path = format_str("%s/wallpapers/%s.%s", dir, t->name, exts[i]);
			if (access(path, R_OK) == 0) {
				image = path;
			} else {
				free(path);
			}
		}
		free(dir);
		const char *name = tw_theme_str(t, "wallpaper.image", NULL);
		if (!image && name && strcasecmp(tw_theme_str(t, "wallpaper.type", ""), "image") == 0) {
			image = name[0] == '/' || name[0] == '~' ? tw_expand_home(name) :
				tw_theme_file(t, name);
		}
	}
	paint_colors(cr, w, h, c1, c2, vertical);
	cairo_surface_t *picture = image ? tw_image_render_cover(image, w, h) : NULL;
	if (picture) {
		cairo_set_source_surface(cr, picture, 0, 0);
		cairo_paint(cr);
		cairo_surface_destroy(picture);
	}
	free(image);
	cairo_destroy(cr);
	return out;
}

/* ---------- drawing ---------- */

static void text(cairo_t *cr, const char *font, const char *str, double x, double y,
		double alpha, int align, int *out_w, int *out_h) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, str, -1);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	double tx = align == 1 ? x - w / 2.0 : align == 2 ? x - w : x;
	// soft shadow keeps white text readable on bright wallpapers
	cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * alpha);
	cairo_move_to(cr, tx + 1, y + 2);
	pango_cairo_show_layout(cr, layout);
	cairo_set_source_rgba(cr, 1, 1, 1, alpha);
	cairo_move_to(cr, tx, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	if (out_w) {
		*out_w = w;
	}
	if (out_h) {
		*out_h = h;
	}
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path(cr);
}

static void draw_avatar(cairo_t *cr, double cx, double cy, double r) {
	cairo_save(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_clip(cr);
	if (lock.avatar) {
		double iw = cairo_image_surface_get_width(lock.avatar);
		double ih = cairo_image_surface_get_height(lock.avatar);
		double s = 2 * r / (iw < ih ? iw : ih);
		cairo_translate(cr, cx - iw * s / 2, cy - ih * s / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, lock.avatar, 0, 0);
		cairo_paint(cr);
	} else {
		cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.95);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 0.55, 0.57, 0.6);
		cairo_arc(cr, cx, cy - r * 0.18, r * 0.36, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy + r * 0.95, r * 0.72, M_PI, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

static void render_output(struct lock_output *o) {
	if (!o->configured || o->width <= 0 || o->height <= 0) {
		return;
	}
	int pw = o->width * o->scale, ph = o->height * o->scale;
	struct pool_buffer *buffer = get_next_buffer(lock.shm, o->buffers, pw, ph);
	if (!buffer) {
		lock.dirty = true; // both buffers busy, try again later
		return;
	}
	if (!o->background || o->bg_width != pw || o->bg_height != ph) {
		if (o->background) {
			cairo_surface_destroy(o->background);
		}
		o->background = render_background(pw, ph);
		o->bg_width = pw;
		o->bg_height = ph;
	}
	cairo_t *cr = buffer->cairo;
	cairo_save(cr);
	cairo_set_source_surface(cr, o->background, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, o->scale, o->scale);
	double W = o->width, H = o->height;
	bool sign_in = lock.view == VIEW_SIGN_IN;
	cairo_set_source_rgba(cr, 0, 0, 0, sign_in ? 0.55 : 0.25);
	cairo_paint(cr);

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char clock_text[32], date_text[96];
	strftime(clock_text, sizeof(clock_text), "%H:%M", &tm);
	strftime(date_text, sizeof(date_text), "%A, %e %B", &tm);

	if (!sign_in) {
		double size = H / 9.0 < 40 ? 40 : H / 9.0;
		char font[96];
		snprintf(font, sizeof(font), "Segoe UI Light, Noto Sans Light, Sans %d", (int)size);
		int tw = 0, th = 0;
		text(cr, font, clock_text, W * 0.045, H - size * 3.1, 1, 0, &tw, &th);
		snprintf(font, sizeof(font), "Segoe UI, Noto Sans, Sans %d", (int)(size / 3.2));
		text(cr, font, date_text, W * 0.045 + size * 0.06, H - size * 3.1 + th, 1, 0, NULL, NULL);
	} else {
		double cx = W / 2, cy = H / 2 - 90;
		double r = H / 10 < 60 ? 60 : H / 10 > 100 ? 100 : H / 10;
		draw_avatar(cr, cx, cy - r * 0.2, r);
		int th = 0;
		const char *name = lock.full_name && *lock.full_name ? lock.full_name : lock.user;
		text(cr, "Segoe UI Semilight, Noto Sans, Sans 26", name, cx, cy + r * 0.95, 1, 1, NULL, &th);
		double fy = cy + r * 0.95 + th + 22, fw = 300, fh = 38;
		double fx = cx - fw / 2;
		// password field with a submit button, like the Windows sign-in
		rounded(cr, fx, fy, fw, fh, 4);
		cairo_set_source_rgba(cr, 1, 1, 1, lock.checking ? 0.55 : 0.92);
		cairo_fill(cr);
		double bw = fh;
		rounded(cr, fx + fw - bw, fy, bw, fh, 4);
		cairo_set_source_rgba(cr, (lock.accent >> 24 & 0xff) / 255.0,
			(lock.accent >> 16 & 0xff) / 255.0, (lock.accent >> 8 & 0xff) / 255.0, 1);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 2.2);
		cairo_move_to(cr, fx + fw - bw + 12, fy + fh / 2);
		cairo_line_to(cr, fx + fw - 12, fy + fh / 2);
		cairo_move_to(cr, fx + fw - 19, fy + fh / 2 - 7);
		cairo_line_to(cr, fx + fw - 12, fy + fh / 2);
		cairo_line_to(cr, fx + fw - 19, fy + fh / 2 + 7);
		cairo_stroke(cr);
		if (lock.password_len == 0 && !lock.checking) {
			PangoLayout *layout = pango_cairo_create_layout(cr);
			PangoFontDescription *desc = pango_font_description_from_string("Noto Sans, Sans 11");
			pango_layout_set_font_description(layout, desc);
			pango_font_description_free(desc);
			pango_layout_set_text(layout, "Password", -1);
			int lw, lh;
			pango_layout_get_pixel_size(layout, &lw, &lh);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
			cairo_move_to(cr, fx + 12, fy + (fh - lh) / 2.0);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		}
		// one dot per character (bytes of UTF-8 sequences are counted once)
		size_t chars = 0;
		for (size_t i = 0; i < lock.password_len; i++) {
			chars += ((unsigned char)lock.password[i] & 0xc0) != 0x80;
		}
		double dot_x = fx + 16;
		cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
		for (size_t i = 0; i < chars && dot_x < fx + fw - bw - 10; i++, dot_x += 13) {
			cairo_arc(cr, dot_x, fy + fh / 2, 3.6, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		double my = fy + fh + 18;
		const char *msg = lock.checking ? "Checking the password..." : lock.message;
		if (msg && *msg) {
			text(cr, "Noto Sans, Sans 12", msg, cx, my, 1, 1, NULL, NULL);
			my += 26;
		}
		if (lock.xkb_state && xkb_state_mod_name_is_active(lock.xkb_state, XKB_MOD_NAME_CAPS,
				XKB_STATE_MODS_LOCKED) > 0) {
			text(cr, "Noto Sans, Sans 11", "Caps Lock is on", cx, my, 0.85, 1, NULL, NULL);
		}
		// the time stays visible in the corner
		text(cr, "Noto Sans, Sans 14", clock_text, W - 28, H - 48, 0.9, 2, NULL, NULL);
	}
	cairo_restore(cr);

	wl_surface_set_buffer_scale(o->surface, o->scale);
	wl_surface_attach(o->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(o->surface);
}

static void render_all(void) {
	lock.dirty = false;
	struct lock_output *o;
	wl_list_for_each(o, &lock.outputs, link) {
		render_output(o);
	}
}

/* ---------- authentication ---------- */

static int pam_conversation(int num, const struct pam_message **msg, struct pam_response **resp,
		void *data) {
	struct pam_response *replies = calloc(num, sizeof(*replies));
	if (!replies) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < num; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			replies[i].resp = strdup(lock.password);
			if (!replies[i].resp) {
				free(replies);
				return PAM_BUF_ERR;
			}
		}
	}
	*resp = replies;
	return PAM_SUCCESS;
}

static void start_check(void) {
	if (lock.checking || lock.password_len == 0) {
		return;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		set_message("Could not check the password");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		set_message("Could not check the password");
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		const char *service = access("/etc/pam.d/tilewin-lock", R_OK) == 0 ?
			"tilewin-lock" : "login";
		struct pam_conv conv = { pam_conversation, NULL };
		pam_handle_t *pamh = NULL;
		int ret = pam_start(service, lock.user, &conv, &pamh);
		if (ret == PAM_SUCCESS) {
			ret = pam_authenticate(pamh, 0);
		}
		if (ret == PAM_SUCCESS) {
			ret = pam_acct_mgmt(pamh, 0);
		}
		if (pamh) {
			pam_end(pamh, ret);
		}
		clear_password();
		char result = ret == PAM_SUCCESS ? 'y' : 'n';
		if (write(fds[1], &result, 1) != 1) {
			_exit(1);
		}
		_exit(0);
	}
	close(fds[1]);
	clear_password();
	lock.auth_pid = pid;
	lock.auth_fd = fds[0];
	lock.checking = true;
	lock.message[0] = '\0';
	lock.dirty = true;
}

static void unlock(void) {
	if (lock.lock) {
		ext_session_lock_v1_unlock_and_destroy(lock.lock);
		lock.lock = NULL;
		wl_display_roundtrip(lock.display);
	}
	lock.running = false;
}

static void finish_check(void) {
	char result = 'n';
	if (read(lock.auth_fd, &result, 1) != 1) {
		result = 'n';
	}
	close(lock.auth_fd);
	lock.auth_fd = -1;
	waitpid(lock.auth_pid, NULL, 0);
	lock.checking = false;
	if (result == 'y') {
		unlock();
		return;
	}
	set_message("The password is incorrect. Try again.");
}

/* ---------- input ---------- */

static void wake_up(void) {
	clock_gettime(CLOCK_MONOTONIC, &lock.last_input);
	if (lock.view != VIEW_SIGN_IN) {
		lock.view = VIEW_SIGN_IN;
		lock.dirty = true;
	}
}

static void handle_key(xkb_keysym_t sym, const char *utf8) {
	bool was_clock = lock.view == VIEW_CLOCK;
	wake_up();
	if (lock.checking) {
		return;
	}
	if (was_clock && (sym == XKB_KEY_space || sym == XKB_KEY_Return ||
			sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Escape)) {
		return; // only wakes the screen up, like on Windows
	}
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		start_check();
		return;
	case XKB_KEY_Escape:
		clear_password();
		lock.view = VIEW_CLOCK;
		lock.dirty = true;
		return;
	case XKB_KEY_BackSpace:
		while (lock.password_len > 0) {
			unsigned char c = lock.password[--lock.password_len];
			lock.password[lock.password_len] = '\0';
			if ((c & 0xc0) != 0x80) {
				break;
			}
		}
		lock.dirty = true;
		return;
	}
	if (xkb_state_mod_name_is_active(lock.xkb_state, XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_EFFECTIVE) > 0) {
		if (sym == XKB_KEY_u || sym == XKB_KEY_U) {
			clear_password();
			lock.dirty = true;
		}
		return;
	}
	size_t add = strlen(utf8);
	if (add > 0 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
			lock.password_len + add < sizeof(lock.password) - 1) {
		// a key that only woke the screen up still counts as typing
		memcpy(lock.password + lock.password_len, utf8, add);
		lock.password_len += add;
		lock.password[lock.password_len] = '\0';
		lock.message[0] = '\0';
		lock.dirty = true;
	} else if (was_clock) {
		lock.dirty = true;
	}
}

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd,
		uint32_t size) {
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(lock.xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		return;
	}
	if (lock.xkb_state) {
		xkb_state_unref(lock.xkb_state);
	}
	if (lock.keymap) {
		xkb_keymap_unref(lock.keymap);
	}
	lock.keymap = keymap;
	lock.xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface) {
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !lock.xkb_state) {
		return;
	}
	xkb_keycode_t code = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(lock.xkb_state, code);
	char utf8[16] = { 0 };
	xkb_state_key_get_utf8(lock.xkb_state, code, utf8, sizeof(utf8));
	handle_key(sym, utf8);
	explicit_bzero(utf8, sizeof(utf8));
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	if (lock.xkb_state) {
		xkb_state_update_mask(lock.xkb_state, depressed, latched, locked, 0, 0, group);
		lock.dirty |= lock.view == VIEW_SIGN_IN; // Caps Lock hint
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
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

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	wl_pointer_set_cursor(p, serial, NULL, 0, 0);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface) {
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		wake_up();
	}
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
}

/* The seat is bound at version 5, which also sends these. libwayland aborts the
   whole process when an event arrives for a NULL listener entry, so the ones we
   do not care about still need a handler. */
static void pointer_frame(void *data, struct wl_pointer *p) {
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
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

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;

	/* Every input device goes away while another VT is in front, and the seat
	   loses the capability with them. Let the dead objects go instead of
	   keeping them, or the devices are never bound again on the way back and
	   the lock screen stops seeing input. */
	if (has_keyboard && !lock.keyboard) {
		lock.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(lock.keyboard, &keyboard_listener, NULL);
	} else if (!has_keyboard && lock.keyboard) {
		wl_keyboard_release(lock.keyboard);
		lock.keyboard = NULL;
	}

	if (has_pointer && !lock.pointer) {
		lock.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(lock.pointer, &pointer_listener, NULL);
	} else if (!has_pointer && lock.pointer) {
		wl_pointer_release(lock.pointer);
		lock.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------- outputs and lock surfaces ---------- */

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct lock_output *o = data;
	o->width = width;
	o->height = height;
	o->configured = true;
	ext_session_lock_surface_v1_ack_configure(surface, serial);
	render_output(o);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

static void create_lock_surface(struct lock_output *o) {
	if (!lock.lock || o->lock_surface) {
		return;
	}
	o->surface = wl_compositor_create_surface(lock.compositor);
	o->lock_surface = ext_session_lock_v1_get_lock_surface(lock.lock, o->surface, o->wl_output);
	ext_session_lock_surface_v1_add_listener(o->lock_surface, &lock_surface_listener, o);
}

static void destroy_output(struct lock_output *o) {
	wl_list_remove(&o->link);
	if (o->lock_surface) {
		ext_session_lock_surface_v1_destroy(o->lock_surface);
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
	}
	destroy_buffer(&o->buffers[0]);
	destroy_buffer(&o->buffers[1]);
	if (o->background) {
		cairo_surface_destroy(o->background);
	}
	wl_output_destroy(o->wl_output);
	free(o);
}

static void output_geometry(void *data, struct wl_output *out, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
}

static void output_mode(void *data, struct wl_output *out, uint32_t flags, int32_t w,
		int32_t h, int32_t refresh) {
}

static void output_done(void *data, struct wl_output *out) {
	lock.dirty = true;
}

static void output_scale(void *data, struct wl_output *out, int32_t factor) {
	struct lock_output *o = data;
	o->scale = factor > 0 ? factor : 1;
	lock.dirty = true;
}

static void output_name(void *data, struct wl_output *out, const char *name) {
}

static void output_description(void *data, struct wl_output *out, const char *description) {
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *l) {
	lock.locked = true;
	if (lock.ready_fd >= 0) {
		char ok = 'y';
		if (write(lock.ready_fd, &ok, 1) != 1) {
			perror("tilewin-lock: telling the parent");
		}
		close(lock.ready_fd);
		lock.ready_fd = -1;
	}
}

static void lock_finished(void *data, struct ext_session_lock_v1 *l) {
	if (!lock.locked) {
		fprintf(stderr, "tilewin-lock: the session could not be locked "
			"(is another lock screen running?)\n");
		exit(2);
	}
	// the compositor ended the lock (e.g. unlocked elsewhere)
	lock.running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		lock.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		lock.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !lock.seat) {
		lock.seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(lock.seat, &seat_listener, NULL);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		lock.manager = wl_registry_bind(registry, name,
			&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct lock_output *o = calloc(1, sizeof(*o));
		o->global_name = name;
		o->scale = 1;
		o->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &output_listener, o);
		wl_list_insert(lock.outputs.prev, &o->link);
		create_lock_surface(o); // an output plugged in while locked
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct lock_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &lock.outputs, link) {
		if (o->global_name == name) {
			destroy_output(o);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------- main ---------- */

static void load_user(void) {
	struct passwd *pw = getpwuid(getuid());
	lock.user = strdup(pw && pw->pw_name ? pw->pw_name : getenv("USER") ? getenv("USER") : "");
	if (pw && pw->pw_gecos && *pw->pw_gecos) {
		lock.full_name = strndup(pw->pw_gecos, strcspn(pw->pw_gecos, ","));
	}
	const char *home = pw && pw->pw_dir ? pw->pw_dir : getenv("HOME");
	char *paths[2] = {
		home ? format_str("%s/.face", home) : NULL,
		format_str("/var/lib/AccountsService/icons/%s", lock.user),
	};
	for (int i = 0; i < 2; i++) {
		if (!lock.avatar && paths[i] && access(paths[i], R_OK) == 0) {
			lock.avatar = tw_image_load(paths[i]);
		}
		free(paths[i]);
	}
}

int main(int argc, char **argv) {
	static const struct option long_options[] = {
		{ "daemonize", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	bool daemonize = false;
	int c;
	while ((c = getopt_long(argc, argv, "fhc:", long_options, NULL)) != -1) {
		switch (c) {
		case 'f':
			daemonize = true;
			break;
		case 'c':
			break; // swaylock's color option, accepted for existing configs
		case 'h':
		default:
			printf("Usage: tilewin-lock [-f|--daemonize]\n"
				"Locks the tileWin session. -f returns once the session is locked.\n");
			return c == 'h' ? 0 : 1;
		}
	}
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);

	if (daemonize) {
		int fds[2];
		if (pipe(fds) != 0) {
			perror("tilewin-lock: pipe");
			return 1;
		}
		pid_t pid = fork();
		if (pid < 0) {
			perror("tilewin-lock: fork");
			return 1;
		}
		if (pid > 0) {
			close(fds[1]);
			char ok = 'n';
			ssize_t n = read(fds[0], &ok, 1);
			return n == 1 && ok == 'y' ? 0 : 1;
		}
		close(fds[0]);
		setsid();
		lock.ready_fd = fds[1];
	}

	mlock(lock.password, sizeof(lock.password));
	wl_list_init(&lock.outputs);
	load_user();
	char *theme_name = tw_theme_current_name();
	char *error = NULL;
	lock.theme = tw_theme_load(theme_name, &error);
	free(error);
	free(theme_name);
	lock.accent = tw_theme_color(lock.theme, "taskbar.indicator", 0x0078d4ff);
	lock.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	lock.display = wl_display_connect(NULL);
	if (!lock.display) {
		fprintf(stderr, "tilewin-lock: cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(lock.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(lock.display);
	if (!lock.compositor || !lock.shm || !lock.manager) {
		fprintf(stderr, "tilewin-lock: the compositor does not support locking the session\n");
		return 1;
	}
	lock.lock = ext_session_lock_manager_v1_lock(lock.manager);
	ext_session_lock_v1_add_listener(lock.lock, &lock_listener, NULL);
	struct lock_output *o;
	wl_list_for_each(o, &lock.outputs, link) {
		create_lock_surface(o);
	}
	wl_display_roundtrip(lock.display);

	lock.running = true;
	clock_gettime(CLOCK_MONOTONIC, &lock.last_input);
	lock.last_minute = -1;
	while (lock.running) {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		if (tm.tm_min != lock.last_minute) {
			lock.last_minute = tm.tm_min;
			lock.dirty = true;
		}
		if (lock.message[0] && ms_since(&lock.message_until) > MESSAGE_MS) {
			lock.message[0] = '\0';
			lock.dirty = true;
		}
		if (lock.view == VIEW_SIGN_IN && !lock.checking && lock.password_len == 0 &&
				ms_since(&lock.last_input) > BACK_TO_CLOCK_MS) {
			lock.view = VIEW_CLOCK;
			lock.dirty = true;
		}
		if (lock.dirty) {
			render_all();
		}

		while (wl_display_prepare_read(lock.display) != 0) {
			wl_display_dispatch_pending(lock.display);
		}
		if (wl_display_flush(lock.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(lock.display);
			break;
		}
		struct pollfd fds[2] = {
			{ .fd = wl_display_get_fd(lock.display), .events = POLLIN },
			{ .fd = lock.auth_fd, .events = POLLIN },
		};
		int ret = poll(fds, lock.auth_fd >= 0 ? 2 : 1, 1000);
		if (ret > 0 && (fds[0].revents & POLLIN)) {
			if (wl_display_read_events(lock.display) < 0) {
				break;
			}
		} else {
			wl_display_cancel_read(lock.display);
		}
		if (ret > 0 && (fds[0].revents & (POLLERR | POLLHUP))) {
			break;
		}
		if (wl_display_dispatch_pending(lock.display) < 0) {
			break;
		}
		if (lock.auth_fd >= 0 && ret > 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
			finish_check();
		}
	}
	clear_password();
	return lock.lock ? 1 : 0;
}
