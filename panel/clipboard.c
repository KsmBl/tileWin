/*
 * The clipboard history list (Win+V or "panel clipboard").
 *
 * The history itself lives in tilewin-clipboard, a program of its own, because
 * a single copied picture can be sixteen megabytes and inside the taskbar that
 * looked like the taskbar growing for no reason. This file only draws the list
 * and asks that program over a socket in $XDG_RUNTIME_DIR: it holds a
 * thumbnail and the first few hundred bytes of each text, never the copied
 * data. The taskbar starts the program if nobody else has, and leaves it
 * running when it restarts itself, so the history survives a restart.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "draw.h"
#include "flyout.h"
#include "log.h"
#include "popup.h"
#include "stringop.h"
#include "tw_paths.h"

#define VIEW_W 360
#define VIEW_MAX_H 480
#define HEADER 56
#define EMPTY_H 90
#define CARD_PAD 12
#define CARD_GAP 8
#define VIEW_ROWS_MAX 128
#define REPLY_MAX (8 * 1024 * 1024)

/* What the list needs to draw one entry. The copied data stays where it is. */
struct entry {
	uint64_t id;
	bool image, pinned;
	char *preview; // the first lines of a text, NUL terminated
	cairo_surface_t *thumb;
};

static struct {
	struct panel *panel;
	list_t *entries; // struct entry *, newest first
	int watch_fd;
	struct loop_timer *connect_timer;
	int connect_tries;
	bool connected;
} cb = { .watch_fd = -1 };

static void view_changed(void);

/* ---------- talking to tilewin-clipboard ---------- */

static char *socket_path(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return NULL;
	}
	return format_str("%s/tilewin-clipboard-%s.sock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
}

static int clip_connect(void) {
	char *path = socket_path();
	if (!path) {
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) {
		free(path);
		return -1;
	}
	strcpy(addr.sun_path, path);
	free(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool write_all(int fd, const void *data, size_t len) {
	const char *p = data;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
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

/* Sends one command and closes; nothing comes back. */
static void clip_send(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *line = vformat_str(fmt, args);
	va_end(args);
	int fd = clip_connect();
	if (fd >= 0) {
		write_all(fd, line, strlen(line));
		close(fd);
	}
	free(line);
}

static void entry_free(struct entry *e) {
	if (e->thumb) {
		cairo_surface_destroy(e->thumb);
	}
	free(e->preview);
	free(e);
}

static cairo_surface_t *fetch_thumb(uint64_t id) {
	int fd = clip_connect();
	if (fd < 0) {
		return NULL;
	}
	char *line = format_str("thumb %llu\n", (unsigned long long)id);
	cairo_surface_t *thumb = NULL;
	char head[128];
	int w = 0, h = 0, stride = 0;
	size_t len = 0;
	if (write_all(fd, line, strlen(line)) && read_line(fd, head, sizeof(head)) &&
			sscanf(head, "%d %d %d %zu", &w, &h, &stride, &len) == 4 &&
			w > 0 && h > 0 && len > 0 && len <= REPLY_MAX &&
			stride >= w * 4 && len == (size_t)stride * h) {
		thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		if (cairo_surface_status(thumb) == CAIRO_STATUS_SUCCESS &&
				cairo_image_surface_get_stride(thumb) == stride &&
				read_all(fd, cairo_image_surface_get_data(thumb), len)) {
			cairo_surface_mark_dirty(thumb);
		} else {
			cairo_surface_destroy(thumb);
			thumb = NULL;
		}
	}
	free(line);
	close(fd);
	return thumb;
}

/* Reads the list again, keeping the thumbnails of entries that are still there. */
static void refresh(void) {
	int fd = clip_connect();
	if (fd < 0) {
		cb.connected = false;
		return;
	}
	cb.connected = true;
	list_t *old = cb.entries;
	list_t *fresh = create_list();
	char head[64];
	int count = 0;
	if (write_all(fd, "list\n", 5) && read_line(fd, head, sizeof(head)) &&
			sscanf(head, "%d", &count) == 1 && count >= 0) {
		for (int i = 0; i < count; i++) {
			char line[160];
			unsigned long long id = 0;
			int image = 0, pinned = 0;
			size_t len = 0;
			if (!read_line(fd, line, sizeof(line)) ||
					sscanf(line, "%llu %d %d %zu", &id, &image, &pinned, &len) != 4 ||
					len > REPLY_MAX) {
				break;
			}
			struct entry *e = calloc(1, sizeof(*e));
			e->id = id;
			e->image = image != 0;
			e->pinned = pinned != 0;
			e->preview = malloc(len + 1);
			if (!e->preview || (len && !read_all(fd, e->preview, len))) {
				entry_free(e);
				break;
			}
			e->preview[len] = '\0';
			list_add(fresh, e);
		}
	}
	close(fd);

	cb.entries = fresh;
	for (int i = 0; i < fresh->length; i++) {
		struct entry *e = fresh->items[i];
		if (!e->image) {
			continue;
		}
		struct entry *was = NULL;
		for (int j = 0; old && j < old->length && !was; j++) {
			struct entry *o = old->items[j];
			was = o->id == e->id ? o : NULL;
		}
		if (was && was->thumb) {
			e->thumb = was->thumb;
			was->thumb = NULL;
		} else {
			e->thumb = fetch_thumb(e->id);
		}
	}
	// an entry whose picture never arrived would draw nothing at all
	for (int i = 0; i < cb.entries->length; i++) {
		struct entry *e = cb.entries->items[i];
		if (e->image && !e->thumb) {
			list_del(cb.entries, i--);
			entry_free(e);
		}
	}
	for (int i = 0; old && i < old->length; i++) {
		entry_free(old->items[i]);
	}
	list_free(old);
}

static void watch_event(int fd, short mask, void *data) {
	char buf[64];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n <= 0 && !(n < 0 && errno == EAGAIN)) {
		loop_remove_fd(cb.panel->loop, fd);
		close(fd);
		cb.watch_fd = -1;
		cb.connected = false;
		return;
	}
	refresh();
	view_changed();
}

static void try_connect(void *data);

static void schedule_connect(int ms) {
	if (cb.connect_timer) {
		loop_remove_timer(cb.panel->loop, cb.connect_timer);
	}
	cb.connect_timer = loop_add_timer(cb.panel->loop, ms, try_connect, NULL);
}

static void try_connect(void *data) {
	cb.connect_timer = NULL;
	int fd = clip_connect();
	if (fd < 0) {
		if (cb.connect_tries++ == 0) {
			proc_spawn("tilewin-clipboard");
		}
		if (cb.connect_tries < 12) {
			schedule_connect(250);
		} else {
			sway_log(SWAY_INFO, "Clipboard history: tilewin-clipboard did not come up");
		}
		return;
	}
	if (!write_all(fd, "watch\n", 6)) {
		close(fd);
		schedule_connect(1000);
		return;
	}
	cb.watch_fd = fd;
	cb.connected = true;
	loop_add_fd(cb.panel->loop, fd, POLLIN, watch_event, NULL);
	refresh();
	view_changed();
}

void clipboard_copy_text(struct panel *panel, const char *text) {
	if (!text || !*text) {
		return;
	}
	size_t len = strlen(text);
	int fd = clip_connect();
	if (fd >= 0) {
		char *head = format_str("copy %zu\n", len);
		bool ok = write_all(fd, head, strlen(head)) && write_all(fd, text, len);
		free(head);
		close(fd);
		if (ok) {
			return;
		}
	}
	// no clipboard program: wl-copy does it
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

static bool recording(void) {
	struct panel_config *config = cb.panel->config;
	return !config || twconf_parse_bool(twconf_value(config->root, "clipboard_history"), true);
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
	if (index < 0 || index >= cb.entries->length) {
		return;
	}
	struct entry *e = cb.entries->items[index];
	clip_send("use %llu %d\n", (unsigned long long)e->id, focused_terminal(cb.panel) ? 1 : 0);
	popup_close_later(cb.panel);
}

static void remove_at(int index) {
	if (index < 0 || index >= cb.entries->length) {
		return;
	}
	struct entry *e = cb.entries->items[index];
	clip_send("remove %llu\n", (unsigned long long)e->id);
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
	struct view_row rows[VIEW_ROWS_MAX];
	int row_count;
};

static struct view *view_current = NULL;

static int card_height(struct entry *c, cairo_t *cr, const struct fly_style *st, int w) {
	if (c->image) {
		return cairo_image_surface_get_height(c->thumb) + 2 * CARD_PAD;
	}
	int h = pd_text_wrapped(cr, st->font, c->preview, 0, 0, w - 2 * CARD_PAD - 56, 3, st->fg, false);
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
		unpinned |= !((struct entry *)cb.entries->items[i])->pinned;
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
		pd_text_wrapped(cr, st.font, !cb.connected ?
			"tilewin-clipboard is not running, so nothing is being kept." : recording() ?
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
		struct entry *c = cb.entries->items[i];
		int h = card_height(c, cr, &st, cw);
		if (v->row_count >= VIEW_ROWS_MAX) {
			break;
		}
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
			pd_text_wrapped(cr, st.font, c->preview, x0 + CARD_PAD, y + CARD_PAD,
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
		clip_send("clear\n");
		v->selected = 0;
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
			struct entry *c = cb.entries->items[i];
			clip_send("pin %llu\n", (unsigned long long)c->id);
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
	refresh(); // the list may have changed while nothing was watching
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
}

/* ---------- start and stop ---------- */

void clipboard_init(struct panel *panel) {
	cb.panel = panel;
	cb.entries = create_list();
	try_connect(NULL);
}

void clipboard_fini(struct panel *panel) {
	if (cb.connect_timer) {
		loop_remove_timer(panel->loop, cb.connect_timer);
		cb.connect_timer = NULL;
	}
	if (cb.watch_fd >= 0) {
		loop_remove_fd(panel->loop, cb.watch_fd);
		close(cb.watch_fd);
		cb.watch_fd = -1;
	}
	for (int i = 0; cb.entries && i < cb.entries->length; i++) {
		entry_free(cb.entries->items[i]);
	}
	list_free(cb.entries);
	cb.entries = NULL;
	// tilewin-clipboard keeps running: the history outlives a taskbar restart
}
