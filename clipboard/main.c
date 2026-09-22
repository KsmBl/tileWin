/*
 * tilewin-clipboard: the clipboard history, in a process of its own.
 *
 * It watches the clipboard with ext-data-control and keeps the last copied
 * texts and pictures. Choosing an entry puts it on the clipboard again and
 * pastes it into the focused window with a virtual keyboard (Ctrl+V, or
 * Ctrl+Shift+V where the taskbar says the window is a terminal). Pinned
 * entries are kept in ~/.local/state/tileWin/clipboard/, everything else is
 * forgotten when this program quits. Copies that password managers mark as
 * secret are never recorded; "clipboard_history no" in taskbar.conf turns
 * recording off and "clipboard_paste no" only copies.
 *
 * It runs on its own so that the memory the history takes belongs to a process
 * that says what it is: a single copied picture can be sixteen megabytes, and
 * inside the taskbar that looked like the taskbar growing for no reason. The
 * taskbar draws the list and asks this program over a socket in
 * $XDG_RUNTIME_DIR; it only ever holds the thumbnails and the first few lines
 * of each text, never the copied data itself.
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
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo.h>
#include "ext-data-control-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "stringop.h"
#include "twconf.h"
#include "tw_paths.h"

#define MAX_ENTRIES 25
#define MAX_TEXT (1024 * 1024)
#define MAX_IMAGE (16 * 1024 * 1024)
#define THUMB_W 300
#define THUMB_H 120
/* The taskbar shows at most three wrapped lines, so it needs no more than this. */
#define PREVIEW_MAX 512
#define REQUEST_MAX 4096

static const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

struct clip {
	uint64_t id;
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
	struct wl_display *display;
	struct wl_seat *seat;
	struct ext_data_control_manager_v1 *manager;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
	struct ext_data_control_device_v1 *device;
	struct loop *loop;
	struct offer *pending;
	list_t *entries; // struct clip *, newest first
	uint64_t next_id;
	int read_fd;
	bool read_image;
	char *buf;
	size_t len, cap;
	struct zwp_virtual_keyboard_v1 *keyboard;
	uint32_t ctrl_mask, shift_mask;
	struct loop_timer *paste_timer;
	bool paste_shift;
	int listen_fd;
	list_t *watchers; // subscribed connections, as int fds cast to pointers
	bool running;
} cb = { .read_fd = -1, .listen_fd = -1, .running = true };

/* ---------- the taskbar config ---------- */

static struct {
	struct twconf_node *root;
	time_t mtime;
	char *path;
} conf;

static char *config_path(void) {
	char *dir = tw_config_dir();
	if (dir) {
		char *path = format_str("%s/taskbar.conf", dir);
		free(dir);
		if (access(path, R_OK) == 0) {
			return path;
		}
		free(path);
	}
	char *path = format_str("%s/config/taskbar.conf", tw_data_dir());
	if (access(path, R_OK) == 0) {
		return path;
	}
	free(path);
	return NULL;
}

/* Re-read the config only when it was written since it was last looked at. */
static bool config_bool(const char *name, bool fallback) {
	if (!conf.path) {
		conf.path = config_path();
	}
	struct stat st;
	if (conf.path && stat(conf.path, &st) == 0 && st.st_mtime != conf.mtime) {
		twconf_free(conf.root);
		char *error = NULL;
		conf.root = twconf_parse_file(conf.path, &error);
		free(error);
		conf.mtime = st.st_mtime;
	}
	return twconf_parse_bool(conf.root ? twconf_value(conf.root, name) : NULL, fallback);
}

static bool recording(void) {
	return config_bool("clipboard_history", true);
}

/* ---------- entries ---------- */

static void notify_watchers(void);

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

static struct clip *entry_by_id(uint64_t id) {
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		if (c->id == id) {
			return c;
		}
	}
	return NULL;
}

static int index_by_id(uint64_t id) {
	for (int i = 0; i < cb.entries->length; i++) {
		if (((struct clip *)cb.entries->items[i])->id == id) {
			return i;
		}
	}
	return -1;
}

static char *clip_dir(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = format_str("%s/clipboard", dir);
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
				char *path = format_str("%s/%s", dir, name);
				write_file(path, c->data, c->len);
				free(path);
			}
			json_object_object_add(obj, "image", json_object_new_string(c->file));
		} else {
			json_object_object_add(obj, "text", json_object_new_string_len(c->data, c->len));
		}
		json_object_array_add(array, obj);
	}
	char *index = format_str("%s/index.json", dir);
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
	char *index = format_str("%s/index.json", dir);
	json_object *array = json_object_from_file(index);
	free(index);
	for (size_t i = 0; array && json_object_is_type(array, json_type_array) &&
			i < json_object_array_length(array); i++) {
		json_object *obj = json_object_array_get_idx(array, i), *value;
		struct clip *c = calloc(1, sizeof(*c));
		c->id = ++cb.next_id;
		c->pinned = true;
		if (json_object_object_get_ex(obj, "text", &value)) {
			c->len = json_object_get_string_len(value);
			c->data = strndup(json_object_get_string(value), c->len);
		} else if (json_object_object_get_ex(obj, "image", &value) &&
				!strchr(json_object_get_string(value), '/')) {
			char *path = format_str("%s/%s", dir, json_object_get_string(value));
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
	notify_watchers();
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
			notify_watchers();
			return;
		}
	}
	struct clip *c = calloc(1, sizeof(*c));
	c->id = ++cb.next_id;
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
	notify_watchers();
}

/* ---------- reading the clipboard ---------- */

static void finish_read(bool ok) {
	loop_remove_fd(cb.loop, cb.read_fd);
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
			loop_add_fd(cb.loop, cb.read_fd, POLLIN, read_in, NULL);
		}
	}
	offer_free(o);
	wl_display_flush(cb.display);
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
	loop_remove_fd(cb.loop, fd);
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
	loop_add_fd(cb.loop, fd, POLLOUT, writer_out, w);
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

static void set_clipboard(bool image, const char *data, size_t len) {
	if (!cb.device || !cb.manager) {
		return;
	}
	struct source *s = calloc(1, sizeof(*s));
	s->data = malloc(len ? len : 1);
	memcpy(s->data, data, len);
	s->len = len;
	s->source = ext_data_control_manager_v1_create_data_source(cb.manager);
	ext_data_control_source_v1_add_listener(s->source, &source_listener, s);
	if (image) {
		ext_data_control_source_v1_offer(s->source, "image/png");
	} else {
		for (size_t i = 0; i < sizeof(text_mimes) / sizeof(text_mimes[0]); i++) {
			ext_data_control_source_v1_offer(s->source, text_mimes[i]);
		}
	}
	ext_data_control_device_v1_set_selection(cb.device, s->source);
	wl_display_flush(cb.display);
}

/* ---------- pasting ---------- */

static bool setup_keyboard(void) {
	if (cb.keyboard) {
		return true;
	}
	if (!cb.seat || !cb.keyboard_manager) {
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
				cb.keyboard_manager, cb.seat);
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
	wl_display_flush(cb.display);
}

/* The taskbar knows which window has the keyboard, so it says whether the
 * paste needs Shift; a terminal takes Ctrl+Shift+V. */
static void use_entry(uint64_t id, bool shift) {
	struct clip *c = entry_by_id(id);
	if (!c) {
		return;
	}
	set_clipboard(c->image, c->data, c->len);
	if (!config_bool("clipboard_paste", true)) {
		return;
	}
	// after the list has closed and the window has the keyboard again
	cb.paste_shift = shift;
	if (cb.paste_timer) {
		loop_remove_timer(cb.loop, cb.paste_timer);
	}
	cb.paste_timer = loop_add_timer(cb.loop, 150, paste_fired, NULL);
}

/* ---------- the socket the taskbar asks over ---------- */

/*
 * One request per connection, a line of text, answered right away:
 *
 *   list                  -> "<count>\n", then per entry
 *                            "<id> <image> <pinned> <preview bytes>\n<preview>"
 *   thumb <id>            -> "<width> <height> <stride> <bytes>\n<pixels>",
 *                            all zeroes when the entry has no picture
 *   use <id> <shift>      -> nothing; copies it and pastes it
 *   pin <id>              -> nothing; pins or unpins
 *   remove <id>           -> nothing
 *   clear                 -> nothing; forgets everything that is not pinned
 *   copy <bytes>\n<text>  -> nothing; puts that text on the clipboard
 *   watch                 -> stays open, a "." arrives whenever the list changes
 *
 * Only this user can reach the socket, and the only program that speaks to it
 * is the taskbar, so the reads are plain blocking ones with a short timeout
 * rather than a state machine.
 */

char *clipboard_socket_path(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return NULL;
	}
	return format_str("%s/tilewin-clipboard-%s.sock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
}

static bool write_all(int fd, const void *data, size_t len) {
	const char *p = data;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n > 0) {
			p += n;
			len -= n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

static bool read_all(int fd, void *data, size_t len) {
	char *p = data;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n > 0) {
			p += n;
			len -= n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

static bool read_line(int fd, char *out, size_t size) {
	size_t i = 0;
	while (i + 1 < size) {
		char ch;
		ssize_t n = read(fd, &ch, 1);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		if (ch == '\n') {
			out[i] = '\0';
			return true;
		}
		out[i++] = ch;
	}
	return false;
}

static void notify_watchers(void) {
	for (int i = 0; cb.watchers && i < cb.watchers->length; i++) {
		int fd = (int)(intptr_t)cb.watchers->items[i];
		if (send(fd, ".", 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
			loop_remove_fd(cb.loop, fd);
			close(fd);
			list_del(cb.watchers, i--);
		}
	}
}

static void watcher_event(int fd, short mask, void *data) {
	char buf[64];
	if (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0 && !(mask & (POLLERR | POLLHUP))) {
		return; // the taskbar says nothing here, but a stray byte is harmless
	}
	for (int i = 0; i < cb.watchers->length; i++) {
		if ((int)(intptr_t)cb.watchers->items[i] == fd) {
			list_del(cb.watchers, i);
			break;
		}
	}
	loop_remove_fd(cb.loop, fd);
	close(fd);
}

/* Never cuts a character in half: pango would draw the rest as a question mark. */
static size_t preview_length(const char *text, size_t len) {
	size_t n = len < PREVIEW_MAX ? len : PREVIEW_MAX;
	while (n > 0 && n < len && ((unsigned char)text[n] & 0xc0) == 0x80) {
		n--;
	}
	return n;
}

static void send_list(int fd) {
	char head[64];
	snprintf(head, sizeof(head), "%d\n", cb.entries->length);
	if (!write_all(fd, head, strlen(head))) {
		return;
	}
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		size_t n = c->image ? 0 : preview_length(c->data, c->len);
		char line[128];
		snprintf(line, sizeof(line), "%llu %d %d %zu\n", (unsigned long long)c->id,
			c->image ? 1 : 0, c->pinned ? 1 : 0, n);
		if (!write_all(fd, line, strlen(line)) || (n && !write_all(fd, c->data, n))) {
			return;
		}
	}
}

static void send_thumb(int fd, uint64_t id) {
	struct clip *c = entry_by_id(id);
	cairo_surface_t *t = c && c->image ? c->thumb : NULL;
	char head[96];
	if (!t) {
		snprintf(head, sizeof(head), "0 0 0 0\n");
		write_all(fd, head, strlen(head));
		return;
	}
	cairo_surface_flush(t);
	int w = cairo_image_surface_get_width(t), h = cairo_image_surface_get_height(t);
	int stride = cairo_image_surface_get_stride(t);
	size_t len = (size_t)stride * h;
	snprintf(head, sizeof(head), "%d %d %d %zu\n", w, h, stride, len);
	if (write_all(fd, head, strlen(head))) {
		write_all(fd, cairo_image_surface_get_data(t), len);
	}
}

static void clear_unpinned(void) {
	bool had_pinned = false;
	for (int i = 0; i < cb.entries->length; i++) {
		struct clip *c = cb.entries->items[i];
		if (c->pinned) {
			had_pinned = true;
		} else {
			list_del(cb.entries, i--);
			clip_free(c);
		}
	}
	(void)had_pinned;
	notify_watchers();
}

static void handle_request(int fd) {
	char line[REQUEST_MAX];
	if (!read_line(fd, line, sizeof(line))) {
		return;
	}
	unsigned long long id = 0;
	int flag = 0;
	size_t len = 0;
	if (strcmp(line, "list") == 0) {
		send_list(fd);
	} else if (sscanf(line, "thumb %llu", &id) == 1) {
		send_thumb(fd, id);
	} else if (sscanf(line, "use %llu %d", &id, &flag) == 2) {
		use_entry(id, flag != 0);
	} else if (sscanf(line, "pin %llu", &id) == 1) {
		struct clip *c = entry_by_id(id);
		if (c) {
			c->pinned = !c->pinned;
			save_pinned();
			notify_watchers();
		}
	} else if (sscanf(line, "remove %llu", &id) == 1) {
		int index = index_by_id(id);
		if (index >= 0) {
			remove_at(index);
		}
	} else if (strcmp(line, "clear") == 0) {
		clear_unpinned();
	} else if (sscanf(line, "copy %zu", &len) == 1 && len > 0 && len <= MAX_TEXT) {
		char *text = malloc(len + 1);
		if (text && read_all(fd, text, len)) {
			text[len] = '\0';
			set_clipboard(false, text, len);
		}
		free(text);
	}
}

static void connection_ready(int fd, short mask, void *data) {
	int conn = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (conn < 0) {
		return;
	}
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	char peek[8] = { 0 };
	ssize_t n = recv(conn, peek, 5, MSG_PEEK);
	if (n == 5 && memcmp(peek, "watch", 5) == 0) {
		char line[REQUEST_MAX];
		read_line(conn, line, sizeof(line));
		list_add(cb.watchers, (void *)(intptr_t)conn);
		loop_add_fd(cb.loop, conn, POLLIN, watcher_event, NULL);
		return;
	}
	handle_request(conn);
	close(conn);
}

static bool listen_on_socket(void) {
	char *path = clipboard_socket_path();
	if (!path) {
		return false;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) {
		free(path);
		return false;
	}
	strcpy(addr.sun_path, path);
	unlink(path); // only reached once the lock said no one else is running
	cb.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	bool ok = cb.listen_fd >= 0 &&
		bind(cb.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
		listen(cb.listen_fd, 8) == 0;
	if (ok) {
		chmod(path, 0600);
		loop_add_fd(cb.loop, cb.listen_fd, POLLIN, connection_ready, NULL);
	} else if (cb.listen_fd >= 0) {
		close(cb.listen_fd);
		cb.listen_fd = -1;
	}
	free(path);
	return ok;
}

/* ---------- Wayland ---------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0 && !cb.seat) {
		cb.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
		cb.manager = wl_registry_bind(registry, name,
			&ext_data_control_manager_v1_interface, 1);
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		cb.keyboard_manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void display_ready(int fd, short mask, void *data) {
	if ((mask & (POLLERR | POLLHUP)) || wl_display_dispatch(cb.display) < 0) {
		cb.running = false;
	}
}

static bool single_instance(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return true;
	}
	char *path = format_str("%s/tilewin-clipboard-%s.lock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	free(path);
	return fd < 0 || flock(fd, LOCK_EX | LOCK_NB) == 0; // the fd stays open
}

int main(int argc, char **argv) {
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		printf("Usage: tilewin-clipboard\n\n"
			"Keeps the clipboard history for the tileWin taskbar. It is started by the\n"
			"taskbar and only one runs per Wayland display.\n");
		return 0;
	}
	if (!single_instance()) {
		return 0;
	}
	cb.display = wl_display_connect(NULL);
	if (!cb.display) {
		fprintf(stderr, "tilewin-clipboard: can't connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(cb.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(cb.display);
	if (!cb.manager || !cb.seat) {
		fprintf(stderr, "tilewin-clipboard: the compositor has no ext-data-control\n");
		return 1;
	}
	cb.entries = create_list();
	cb.watchers = create_list();
	cb.loop = loop_create();
	cb.device = ext_data_control_manager_v1_get_data_device(cb.manager, cb.seat);
	ext_data_control_device_v1_add_listener(cb.device, &device_listener, NULL);
	load_pinned();
	// a new virtual keyboard loses its first keys, so it is made before anything
	// is ever pasted rather than at the first paste
	setup_keyboard();
	if (!listen_on_socket()) {
		fprintf(stderr, "tilewin-clipboard: can't listen in XDG_RUNTIME_DIR\n");
		return 1;
	}
	loop_add_fd(cb.loop, wl_display_get_fd(cb.display), POLLIN, display_ready, NULL);
	while (cb.running) {
		wl_display_dispatch_pending(cb.display);
		if (wl_display_flush(cb.display) < 0 && errno != EAGAIN) {
			break;
		}
		loop_poll(cb.loop);
	}
	char *path = clipboard_socket_path();
	if (path) {
		unlink(path);
		free(path);
	}
	return 0;
}
