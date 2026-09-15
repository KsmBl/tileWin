/*
 * Clipboard history, like on Windows (Win+V or "panel clipboard"): the last
 * copied texts and pictures.
 *
 * The panel watches the clipboard with ext-data-control. Choosing an entry
 * puts it on the clipboard again and pastes it into the focused window with a
 * virtual keyboard (Ctrl+V, Ctrl+Shift+V in terminals). Pinned entries are
 * kept in ~/.local/state/tileWin/clipboard/, everything else is forgotten when
 * the panel quits. Copies that password managers mark as secret are never
 * recorded; "clipboard_history no" in taskbar.conf turns recording off and
 * "clipboard_paste no" only copies.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "flyout.h"
#include "log.h"
#include "popup.h"
#include "tw_paths.h"

#define MAX_ENTRIES 25
#define MAX_TEXT (1024 * 1024)
#define MAX_IMAGE (16 * 1024 * 1024)
#define THUMB_W 300
#define THUMB_H 120
#define VIEW_W 360
#define VIEW_MAX_H 480
#define HEADER 56
#define EMPTY_H 90
#define CARD_PAD 12
#define CARD_GAP 8

static const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

struct clip {
	bool image;
	char *data; // text (NUL terminated) or PNG bytes
	size_t len;
	cairo_surface_t *thumb;
	bool pinned;
	char *file; // PNG file of a pinned picture
};

struct offer {
	struct ext_data_control_offer_v1 *offer;
	int text_rank; // index into text_mimes, -1 if no text
	bool png, secret;
};

struct source {
	struct ext_data_control_source_v1 *source;
	char *data;
	size_t len;
};

struct writer {
	int fd;
	char *data;
	size_t len, done;
};

static struct {
	struct panel *panel;
	struct ext_data_control_device_v1 *device;
	struct offer *pending;
	list_t *entries; // struct clip *, newest first
	int read_fd;
	bool read_image;
	char *buf;
	size_t len, cap;
	struct zwp_virtual_keyboard_v1 *keyboard;
	uint32_t ctrl_mask, shift_mask;
	struct loop_timer *paste_timer;
	bool paste_shift;
} cb = { .read_fd = -1 };

static void view_changed(void);

/* ---------- entries ---------- */

static void clip_free(struct clip *c) {
	if (c->thumb) {
		cairo_surface_destroy(c->thumb);
	}
	free(c->data);
	free(c->file);
	free(c);
}

struct png_reader {
	const unsigned char *data;
	size_t len, pos;
};

static cairo_status_t png_read(void *closure, unsigned char *out, unsigned int length) {
	struct png_reader *r = closure;
	if (r->pos + length > r->len) {
		return CAIRO_STATUS_READ_ERROR;
	}
	memcpy(out, r->data + r->pos, length);
	r->pos += length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *make_thumb(const char *data, size_t len) {
	if (len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) {
		return NULL;
	}
	struct png_reader reader = { (const unsigned char *)data, len, 0 };
	cairo_surface_t *full = cairo_image_surface_create_from_png_stream(png_read, &reader);
	if (cairo_surface_status(full) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(full);
		return NULL;
	}
	int w = cairo_image_surface_get_width(full), h = cairo_image_surface_get_height(full);
	double scale = 1;
	if (w > THUMB_W) {
		scale = (double)THUMB_W / w;
	}
	if (h * scale > THUMB_H) {
		scale = (double)THUMB_H / h;
	}
	int tw = w * scale < 1 ? 1 : (int)(w * scale), th = h * scale < 1 ? 1 : (int)(h * scale);
	cairo_surface_t *thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
	cairo_t *cr = cairo_create(thumb);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, full, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(full);
	return thumb;
}

static char *clip_dir(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = malloc(strlen(dir) + 16);
	sprintf(path, "%s/clipboard", dir);
	free(dir);
	return path;
}

static bool write_file(const char *path, const char *data, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

static char *read_file(const char *path, size_t *len, size_t max) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	char *data = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long size = ftell(f);
		if (size > 0 && (size_t)size <= max && fseek(f, 0, SEEK_SET) == 0) {
			data = malloc(size + 1);
			if (fread(data, 1, size, f) == (size_t)size) {
				data[size] = '\0';
				*len = size;
			} else {
				free(data);
				data = NULL;
			}
		}
	}
	fclose(f);
	return data;
}

/* Pinned entries survive restarts: texts in index.json, pictures next to it. */
static void save_pinned(void) {
	char *dir = clip_dir();
	if (!dir || !tw_mkdir_p(dir)) {
		free(dir);
		return;
	}
	json_object *array = json_object_new_array();
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		if (!c->pinned) {
			continue;
		}
		json_object *obj = json_object_new_object();
		if (c->image) {
			if (!c->file) {
				char name[64];
				snprintf(name, sizeof(name), "%lld-%d.png", (long long)time(NULL), i);
				c->file = strdup(name);
				char *path = malloc(strlen(dir) + strlen(name) + 2);
				sprintf(path, "%s/%s", dir, name);
				write_file(path, c->data, c->len);
				free(path);
			}
			json_object_object_add(obj, "image", json_object_new_string(c->file));
		} else {
			json_object_object_add(obj, "text", json_object_new_string_len(c->data, c->len));
		}
		json_object_array_add(array, obj);
	}
	char *index = malloc(strlen(dir) + 16);
	sprintf(index, "%s/index.json", dir);
	tw_write_string(index, json_object_to_json_string_ext(array, JSON_C_TO_STRING_PLAIN));
	free(index);
	json_object_put(array);

	// pictures that are no longer pinned
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 5 || strcmp(de->d_name + n - 4, ".png") != 0) {
			continue;
		}
		bool used = false;
		for (int i = 0; i < cb.entries->length && !used; i++) {
			struct clip *c = cb.entries->items[i];
			used = c->pinned && c->file && strcmp(c->file, de->d_name) == 0;
		}
		if (!used) {
			unlinkat(dirfd(d), de->d_name, 0);
		}
	}
	if (d) {
		closedir(d);
	}
	free(dir);
}

static void load_pinned(void) {
	char *dir = clip_dir();
	if (!dir) {
		return;
	}
	char *index = malloc(strlen(dir) + 16);
	sprintf(index, "%s/index.json", dir);
	json_object *array = json_object_from_file(index);
	free(index);
	for (size_t i = 0; array && json_object_is_type(array, json_type_array) &&
			i < json_object_array_length(array); i++) {
		json_object *obj = json_object_array_get_idx(array, i), *value;
		struct clip *c = calloc(1, sizeof(*c));
		c->pinned = true;
		if (json_object_object_get_ex(obj, "text", &value)) {
			c->len = json_object_get_string_len(value);
			c->data = strndup(json_object_get_string(value), c->len);
		} else if (json_object_object_get_ex(obj, "image", &value) &&
				!strchr(json_object_get_string(value), '/')) {
			char *path = malloc(strlen(dir) + json_object_get_string_len(value) + 2);
			sprintf(path, "%s/%s", dir, json_object_get_string(value));
			c->image = true;
			c->file = strdup(json_object_get_string(value));
			c->data = read_file(path, &c->len, MAX_IMAGE);
			c->thumb = c->data ? make_thumb(c->data, c->len) : NULL;
			free(path);
		}
		if (c->data && (!c->image || c->thumb)) {
			list_add(cb.entries, c);
		} else {
			clip_free(c);
		}
	}
	json_object_put(array);
	free(dir);
}

static void remove_at(int index) {
	struct clip *c = cb.entries->items[index];
	bool pinned = c->pinned;
	list_del(cb.entries, index);
	clip_free(c);
	if (pinned) {
		save_pinned();
	}
	view_changed();
}

static bool only_space(const char *s, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
			return false;
		}
	}
	return true;
}

/* Takes the data; a copy of an existing entry moves it to the top. */
static void add_entry(bool image, char *data, size_t len) {
	cairo_surface_t *thumb = NULL;
	if (image) {
		thumb = make_thumb(data, len);
		if (!thumb) {
			free(data);
			return;
		}
	} else if (only_space(data, len)) {
		free(data);
		return;
	}
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		if (c->image == image && c->len == len && memcmp(c->data, data, len) == 0) {
			list_del(cb.entries, i);
			list_insert(cb.entries, 0, c);
			free(data);
			if (thumb) {
				cairo_surface_destroy(thumb);
			}
			view_changed();
			return;
		}
	}
	struct clip *c = calloc(1, sizeof(*c));
	c->image = image;
	c->data = data;
	c->len = len;
	c->thumb = thumb;
	list_insert(cb.entries, 0, c);
	int unpinned = 0;
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *e = cb.entries->items[i];
		if (!e->pinned && ++unpinned > MAX_ENTRIES) {
			list_del(cb.entries, i--);
			clip_free(e);
		}
	}
	view_changed();
}

/* ---------- reading the clipboard ---------- */

static void finish_read(bool ok) {
	loop_remove_fd(cb.panel->loop, cb.read_fd);
	close(cb.read_fd);
	cb.read_fd = -1;
	char *data = cb.buf;
	size_t len = cb.len;
	cb.buf = NULL;
	cb.len = cb.cap = 0;
	if (!ok || len == 0) {
		free(data);
		return;
	}
	if (!cb.read_image) {
		data[len] = '\0'; // the buffer always has room for it
	}
	add_entry(cb.read_image, data, len);
}

static void read_in(int fd, short mask, void *data) {
	size_t max = cb.read_image ? MAX_IMAGE : MAX_TEXT;
	while (true) {
		if (cb.cap - cb.len < 4097) {
			size_t cap = cb.cap ? cb.cap * 2 : 16384;
			char *buf = realloc(cb.buf, cap);
			if (!buf) {
				finish_read(false);
				return;
			}
			cb.buf = buf;
			cb.cap = cap;
		}
		ssize_t n = read(fd, cb.buf + cb.len, cb.cap - cb.len - 1);
		if (n > 0) {
			cb.len += n;
			if (cb.len > max) {
				finish_read(false); // too big to keep
				return;
			}
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			return;
		} else {
			finish_read(n == 0);
			return;
		}
	}
}

static bool recording(void) {
	struct panel_config *config = cb.panel->config;
	return !config || twconf_parse_bool(twconf_value(config->root, "clipboard_history"), true);
}

static void offer_free(struct offer *o) {
	if (o) {
		ext_data_control_offer_v1_destroy(o->offer);
		free(o);
	}
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *offer, const char *mime) {
	struct offer *o = data;
	if (strcmp(mime, "x-kde-passwordManagerHint") == 0) {
		o->secret = true;
	} else if (strcmp(mime, "image/png") == 0) {
		o->png = true;
	}
	for (int i = 0; i < (int)(sizeof(text_mimes) / sizeof(text_mimes[0])); i++) {
		if (strcmp(mime, text_mimes[i]) == 0 && (o->text_rank < 0 || i < o->text_rank)) {
			o->text_rank = i;
		}
	}
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
	.offer = offer_mime,
};

static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
		struct ext_data_control_offer_v1 *offer) {
	offer_free(cb.pending);
	cb.pending = calloc(1, sizeof(*cb.pending));
	cb.pending->offer = offer;
	cb.pending->text_rank = -1;
	ext_data_control_offer_v1_add_listener(offer, &offer_listener, cb.pending);
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
		struct ext_data_control_offer_v1 *offer) {
	struct offer *o = cb.pending && cb.pending->offer == offer ? cb.pending : NULL;
	if (!o) {
		return;
	}
	cb.pending = NULL;
	if (recording() && !o->secret && (o->text_rank >= 0 || o->png)) {
		int fds[2];
		if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
			if (cb.read_fd >= 0) {
				finish_read(false); // a newer copy replaces an unfinished one
			}
			cb.read_image = o->text_rank < 0;
			ext_data_control_offer_v1_receive(o->offer,
				cb.read_image ? "image/png" : text_mimes[o->text_rank], fds[1]);
			close(fds[1]);
			cb.read_fd = fds[0];
			loop_add_fd(cb.panel->loop, cb.read_fd, POLLIN, read_in, NULL);
		}
	}
	offer_free(o);
	wl_display_flush(cb.panel->display);
}

static void device_primary_selection(void *data, struct ext_data_control_device_v1 *device,
		struct ext_data_control_offer_v1 *offer) {
	if (cb.pending && cb.pending->offer == offer) {
		offer_free(cb.pending);
		cb.pending = NULL;
	}
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device) {
	ext_data_control_device_v1_destroy(device);
	if (cb.device == device) {
		cb.device = NULL;
	}
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_primary_selection,
};

/* ---------- setting the clipboard ---------- */

static void writer_out(int fd, short mask, void *data) {
	struct writer *w = data;
	while (w->done < w->len) {
		ssize_t n = write(fd, w->data + w->done, w->len - w->done);
		if (n > 0) {
			w->done += n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN && !(mask & (POLLERR | POLLHUP))) {
			return;
		} else {
			break;
		}
	}
	loop_remove_fd(cb.panel->loop, fd);
	close(fd);
	free(w->data);
	free(w);
}

static void source_send(void *data, struct ext_data_control_source_v1 *source,
		const char *mime, int32_t fd) {
	struct source *s = data;
	struct writer *w = calloc(1, sizeof(*w));
	w->data = malloc(s->len ? s->len : 1);
	if (!w->data) {
		free(w);
		close(fd);
		return;
	}
	memcpy(w->data, s->data, s->len);
	w->len = s->len;
	w->fd = fd;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	loop_add_fd(cb.panel->loop, fd, POLLOUT, writer_out, w);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *source) {
	struct source *s = data;
	ext_data_control_source_v1_destroy(source);
	free(s->data);
	free(s);
}

static const struct ext_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled,
};

static void set_clipboard(struct clip *c) {
	struct panel *panel = cb.panel;
	if (!cb.device || !panel->data_control) {
		return;
	}
	struct source *s = calloc(1, sizeof(*s));
	s->data = malloc(c->len ? c->len : 1);
	memcpy(s->data, c->data, c->len);
	s->len = c->len;
	s->source = ext_data_control_manager_v1_create_data_source(panel->data_control);
	ext_data_control_source_v1_add_listener(s->source, &source_listener, s);
	if (c->image) {
		ext_data_control_source_v1_offer(s->source, "image/png");
	} else {
		for (size_t i = 0; i < sizeof(text_mimes) / sizeof(text_mimes[0]); i++) {
			ext_data_control_source_v1_offer(s->source, text_mimes[i]);
		}
	}
	ext_data_control_device_v1_set_selection(cb.device, s->source);
	wl_display_flush(panel->display);
}

void clipboard_copy_text(struct panel *panel, const char *text) {
	if (!text || !*text) {
		return;
	}
	if (cb.device && panel->data_control) {
		struct clip c = { .image = false, .data = (char *)text, .len = strlen(text) };
		set_clipboard(&c);
		return;
	}
	// no data control: wl-copy does it
	GString *cmd = g_string_new("wl-copy -- '");
	for (const char *s = text; *s; s++) {
		if (*s == '\'') {
			g_string_append(cmd, "'\\''");
		} else {
			g_string_append_c(cmd, *s);
		}
	}
	g_string_append_c(cmd, '\'');
	proc_spawn(cmd->str);
	g_string_free(cmd, TRUE);
}

/* ---------- pasting ---------- */

static bool setup_keyboard(void) {
	struct panel *panel = cb.panel;
	struct panel_seat *seat = panel_first_seat(panel);
	if (cb.keyboard) {
		return true;
	}
	if (!seat || !panel->virtual_keyboard) {
		return false;
	}
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *keymap = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
	char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	bool ok = false;
	if (text) {
		size_t size = strlen(text) + 1;
		int fd = memfd_create("tilewin-keymap", MFD_CLOEXEC);
		if (fd >= 0 && write(fd, text, size) == (ssize_t)size) {
			cb.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
				panel->virtual_keyboard, seat->wl_seat);
			zwp_virtual_keyboard_v1_keymap(cb.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				size);
			cb.ctrl_mask = 1 << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
			cb.shift_mask = 1 << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
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

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_key(uint32_t key, bool down, uint32_t mods) {
	zwp_virtual_keyboard_v1_key(cb.keyboard, now_ms(), key,
		down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(cb.keyboard, mods, 0, 0, 0);
}

static void paste_fired(void *data) {
	cb.paste_timer = NULL;
	if (!setup_keyboard()) {
		return;
	}
	uint32_t mods = cb.ctrl_mask;
	send_key(KEY_LEFTCTRL, true, mods);
	if (cb.paste_shift) {
		mods |= cb.shift_mask;
		send_key(KEY_LEFTSHIFT, true, mods);
	}
	send_key(KEY_V, true, mods);
	send_key(KEY_V, false, mods);
	if (cb.paste_shift) {
		mods &= ~cb.shift_mask;
		send_key(KEY_LEFTSHIFT, false, mods);
	}
	send_key(KEY_LEFTCTRL, false, 0);
	wl_display_flush(cb.panel->display);
}

static bool focused_terminal(struct panel *panel) {
	static const char *const names[] = { "term", "kitty", "alacritty", "foot", "konsole",
		"wezterm", "tilix", "terminator", "ghostty", "urxvt", "st-256color" };
	struct pwindow *win = panel_find_window(panel, panel->state.focused_window);
	for (size_t i = 0; win && win->app_id && i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasestr(win->app_id, names[i])) {
			return true;
		}
	}
	return false;
}

static void choose(int index) {
	struct panel *panel = cb.panel;
	if (index < 0 || index >= cb.entries->length) {
		return;
	}
	set_clipboard(cb.entries->items[index]);
	popup_close_later(panel);
	bool paste = !panel->config ||
		twconf_parse_bool(twconf_value(panel->config->root, "clipboard_paste"), true);
	if (paste) {
		// after the list closed and the window has the keyboard again
		cb.paste_shift = focused_terminal(panel);
		if (cb.paste_timer) {
			loop_remove_timer(panel->loop, cb.paste_timer);
		}
		cb.paste_timer = loop_add_timer(panel->loop, 150, paste_fired, NULL);
	}
}

/* ---------- the list ---------- */

struct view_row {
	struct pbox card, pin, remove;
};

struct view {
	struct panel *panel;
	struct popup *popup;
	struct panel_output *output;
	double px, py;
	bool inside;
	int selected, scroll, content;
	struct pbox list, clear_all;
	struct view_row rows[MAX_ENTRIES * 2];
	int row_count;
};

static struct view *view_current = NULL;

static int card_height(struct clip *c, cairo_t *cr, const struct fly_style *st, int w) {
	if (c->image) {
		return cairo_image_surface_get_height(c->thumb) + 2 * CARD_PAD;
	}
	int h = pd_text_wrapped(cr, st->font, c->data, 0, 0, w - 2 * CARD_PAD - 56, 3, st->fg, false);
	return (h < 20 ? 20 : h) + 2 * CARD_PAD;
}

static int list_height(cairo_t *cr, const struct fly_style *st, int w) {
	int h = 0;
	for (int i = 0; i < cb.entries->length; i++) {
		h += card_height(cb.entries->items[i], cr, st, w) + CARD_GAP;
	}
	return h;
}

static void draw_pin(cairo_t *cr, double cx, double cy, uint32_t color, bool filled) {
	cairo_new_path(cr);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.3);
	cairo_move_to(cr, cx - 3.5, cy - 7);
	cairo_line_to(cr, cx + 3.5, cy - 7);
	cairo_line_to(cr, cx + 2.5, cy - 1);
	cairo_line_to(cr, cx + 5, cy + 1.5);
	cairo_line_to(cr, cx - 5, cy + 1.5);
	cairo_line_to(cr, cx - 2.5, cy - 1);
	cairo_close_path(cr);
	if (filled) {
		cairo_fill_preserve(cr);
	}
	cairo_stroke(cr);
	cairo_move_to(cr, cx, cy + 1.5);
	cairo_line_to(cr, cx, cy + 7);
	cairo_stroke(cr);
}

static void draw_small_x(cairo_t *cr, double cx, double cy, uint32_t color) {
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 4, cy - 4);
	cairo_line_to(cr, cx + 4, cy + 4);
	cairo_move_to(cr, cx + 4, cy - 4);
	cairo_line_to(cr, cx - 4, cy + 4);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.3);
	cairo_stroke(cr);
}

static bool view_hovered(struct view *v, struct pbox b) {
	return v->inside && pbox_contains(&v->list, v->px, v->py) && pbox_contains(&b, v->px, v->py);
}

static void view_render(struct popup *p, cairo_t *cr) {
	struct view *v = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + CARD_GAP + 4, cw = W - 2 * M - 2 * (CARD_GAP + 4);
	pd_text(cr, st.big, "Clipboard", x0 + 4, M + 10, cw - 100, 36, st.fg, PD_LEFT);
	v->clear_all = (struct pbox){ 0 };
	bool unpinned = false;
	for (int i = 0; i < cb.entries->length; i++) {
		unpinned |= !((struct clip *)cb.entries->items[i])->pinned;
	}
	if (unpinned) {
		int tw = 0;
		pd_text_size(cr, st.font, "Clear all", &tw, NULL);
		v->clear_all = (struct pbox){ x0 + cw - tw - 12, M + 14, tw + 12, 28 };
		if (v->inside && pbox_contains(&v->clear_all, v->px, v->py)) {
			fill_hover(cr, &st, v->clear_all);
		}
		pd_text(cr, st.font, "Clear all", v->clear_all.x, v->clear_all.y, v->clear_all.width,
			28, st.style == PS_CLASSIC ? 0x0000ffff : st.accent, PD_CENTER);
	}

	v->list = (struct pbox){ M, M + HEADER, W - 2 * M, H - 2 * M - HEADER };
	v->row_count = 0;
	if (cb.entries->length == 0) {
		pd_text_wrapped(cr, st.font, recording() ?
			"Nothing here yet. Copy some text or a picture and it shows up here." :
			"Clipboard history is off (clipboard_history in taskbar.conf).",
			x0 + 4, v->list.y + 8, cw - 8, 3, st.dim, true);
		return;
	}
	v->content = list_height(cr, &st, cw);
	int max_scroll = v->content - v->list.height;
	v->scroll = v->scroll > max_scroll ? max_scroll : v->scroll;
	v->scroll = v->scroll < 0 ? 0 : v->scroll;
	cairo_save(cr);
	cairo_rectangle(cr, v->list.x, v->list.y, v->list.width, v->list.height);
	cairo_clip(cr);
	int y = v->list.y - v->scroll;
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		int h = card_height(c, cr, &st, cw);
		struct view_row *r = &v->rows[v->row_count++];
		r->card = (struct pbox){ x0, y, cw, h };
		r->pin = (struct pbox){ x0 + cw - 56, y + 6, 26, 26 };
		r->remove = (struct pbox){ x0 + cw - 30, y + 6, 26, 26 };
		bool hover = view_hovered(v, r->card);
		bool selected = i == v->selected;
		if (st.style == PS_CLASSIC) {
			pd_rect(cr, r->card.x, r->card.y, r->card.width, h, selected ? 0xffffffff : 0xdfdfdfff);
			pd_bevel(cr, r->card.x, r->card.y, r->card.width, h, true);
		} else {
			cairo_new_path(cr);
			pd_rounded(cr, r->card.x + 0.5, r->card.y + 0.5, cw - 1, h - 1, 6);
			pd_color(cr, hover || selected ? st.button_hover : st.button_bg);
			cairo_fill_preserve(cr);
			pd_color(cr, selected ? st.accent : st.button_border);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
		if (c->image) {
			pd_icon(cr, NULL, 0, 0, 0); // keeps the source clean
			cairo_save(cr);
			cairo_set_source_surface(cr, c->thumb, x0 + CARD_PAD, y + CARD_PAD);
			cairo_paint(cr);
			cairo_restore(cr);
		} else {
			pd_text_wrapped(cr, st.font, c->data, x0 + CARD_PAD, y + CARD_PAD,
				cw - 2 * CARD_PAD - 56, 3, st.fg, true);
		}
		if (hover || c->pinned) {
			if (hover && view_hovered(v, r->pin)) {
				fill_hover(cr, &st, r->pin);
			}
			draw_pin(cr, r->pin.x + 13, r->pin.y + 13, c->pinned ? st.accent : st.fg, c->pinned);
		}
		if (hover) {
			if (view_hovered(v, r->remove)) {
				fill_hover(cr, &st, r->remove);
			}
			draw_small_x(cr, r->remove.x + 13, r->remove.y + 13, st.fg);
		}
		y += h + CARD_GAP;
	}
	cairo_restore(cr);
}

static void view_geometry(struct view *v, int *x, int *y, int *width, int *height) {
	struct panel *panel = v->panel;
	struct panel_output *o = v->output;
	int M = popup_shadow_margin(panel);
	struct fly_style st;
	fly_style_init(&st, panel);
	int cw = VIEW_W - 2 * (CARD_GAP + 4);
	int content = cb.entries->length ? list_height(popup_scratch_cairo(), &st, cw) : EMPTY_H;
	*width = VIEW_W + 2 * M;
	*height = HEADER + (content < VIEW_MAX_H - HEADER ? content : VIEW_MAX_H - HEADER) + 2 * M;
	int bar = o->bar ? o->bar->height : 0;
	bool bottom = panel->config ? panel->config->layouts[panel->layout].bottom : true;
	*x = (o->width - *width) / 2;
	*y = bottom ? o->height - bar - *height - 12 : bar + 12;
}

static void view_changed(void) {
	struct view *v = view_current;
	if (!v) {
		return;
	}
	if (v->selected >= cb.entries->length) {
		v->selected = cb.entries->length - 1;
	}
	int x, y, width, height;
	view_geometry(v, &x, &y, &width, &height);
	if (height != v->popup->height || y != v->popup->y) {
		popup_move_resize(v->popup, x, y, width, height);
	}
	popup_set_dirty(v->popup);
}

static void ensure_visible(struct view *v) {
	struct fly_style st;
	fly_style_init(&st, v->panel);
	int cw = VIEW_W - 2 * (CARD_GAP + 4), top = 0;
	for (int i = 0; i < v->selected && i < cb.entries->length; i++) {
		top += card_height(cb.entries->items[i], popup_scratch_cairo(), &st, cw) + CARD_GAP;
	}
	int h = v->selected >= 0 && v->selected < cb.entries->length ?
		card_height(cb.entries->items[v->selected], popup_scratch_cairo(), &st, cw) : 0;
	if (top < v->scroll) {
		v->scroll = top;
	} else if (top + h > v->scroll + v->list.height) {
		v->scroll = top + h - v->list.height;
	}
}

static void view_motion(struct popup *p, double x, double y) {
	struct view *v = p->data;
	v->px = x;
	v->py = y;
	v->inside = true;
	popup_set_dirty(p);
}

static void view_leave(struct popup *p) {
	struct view *v = p->data;
	v->inside = false;
	popup_set_dirty(p);
}

static void view_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct view *v = p->data;
	if (!pressed || button != BTN_LEFT) {
		return;
	}
	if (v->clear_all.width && pbox_contains(&v->clear_all, x, y)) {
		for (int i = cb.entries->length - 1; i >= 0; i--) {
			struct clip *c = cb.entries->items[i];
			if (!c->pinned) {
				list_del(cb.entries, i);
				clip_free(c);
			}
		}
		v->selected = 0;
		view_changed();
		return;
	}
	if (!pbox_contains(&v->list, x, y)) {
		return;
	}
	for (int i = 0; i < v->row_count && i < cb.entries->length; i++) {
		struct view_row r = v->rows[i];
		if (!pbox_contains(&r.card, x, y)) {
			continue;
		}
		if (pbox_contains(&r.remove, x, y)) {
			remove_at(i);
		} else if (pbox_contains(&r.pin, x, y)) {
			struct clip *c = cb.entries->items[i];
			c->pinned = !c->pinned;
			save_pinned();
			popup_set_dirty(p);
		} else {
			choose(i);
		}
		return;
	}
}

static void view_axis(struct popup *p, double x, double y, int direction) {
	struct view *v = p->data;
	v->scroll += direction < 0 ? -48 : 48;
	popup_set_dirty(p);
}

static void view_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct view *v = p->data;
	int count = cb.entries->length;
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		break;
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		if (count) {
			v->selected = (v->selected + (sym == XKB_KEY_Up ? count - 1 : 1)) % count;
			ensure_visible(v);
			popup_set_dirty(p);
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		choose(v->selected);
		break;
	case XKB_KEY_Delete:
		if (v->selected >= 0 && v->selected < count) {
			remove_at(v->selected);
		}
		break;
	default:
		break;
	}
}

static void view_destroy(struct popup *p) {
	struct view *v = p->data;
	if (view_current == v) {
		view_current = NULL;
	}
	free(v);
}

static const struct popup_vtable view_vtable = {
	.render = view_render,
	.motion = view_motion,
	.leave = view_leave,
	.button = view_button,
	.axis = view_axis,
	.key = view_key,
	.destroy = view_destroy,
};

void clipboard_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_CLIPBOARD)) {
		popup_close_all(panel);
		return;
	}
	if (!output || !cb.entries) {
		return;
	}
	struct view *v = calloc(1, sizeof(*v));
	v->panel = panel;
	v->output = output;
	int x, y, width, height;
	view_geometry(v, &x, &y, &width, &height);
	v->popup = popup_create(panel, POPUP_CLIPBOARD, NULL, output, x, y, width, height,
		&view_vtable, v);
	if (!v->popup) {
		free(v);
		return;
	}
	view_current = v;
	// a new virtual keyboard loses its first keys: create it before anything is pasted
	if (twconf_parse_bool(panel->config ? twconf_value(panel->config->root, "clipboard_paste") :
			NULL, true)) {
		setup_keyboard();
		wl_display_flush(panel->display);
	}
}

/* ---------- start and stop ---------- */

void clipboard_init(struct panel *panel) {
	cb.panel = panel;
	cb.entries = create_list();
	load_pinned();
	struct panel_seat *seat = panel_first_seat(panel);
	if (!panel->data_control || !seat) {
		sway_log(SWAY_INFO, "Clipboard history: the compositor has no data control");
		return;
	}
	cb.device = ext_data_control_manager_v1_get_data_device(panel->data_control, seat->wl_seat);
	ext_data_control_device_v1_add_listener(cb.device, &device_listener, NULL);
}

void clipboard_fini(struct panel *panel) {
	if (!cb.entries) {
		return;
	}
	if (cb.paste_timer) {
		loop_remove_timer(panel->loop, cb.paste_timer);
		cb.paste_timer = NULL;
	}
	if (cb.read_fd >= 0) {
		finish_read(false);
	}
	offer_free(cb.pending);
	cb.pending = NULL;
	if (cb.device) {
		ext_data_control_device_v1_destroy(cb.device);
		cb.device = NULL;
	}
	if (cb.keyboard) {
		zwp_virtual_keyboard_v1_destroy(cb.keyboard);
		cb.keyboard = NULL;
	}
	for (int i = 0; i < cb.entries->length; i++) {
		clip_free(cb.entries->items[i]);
	}
	list_free(cb.entries);
	cb.entries = NULL;
}
