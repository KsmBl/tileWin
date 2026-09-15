#include <ctype.h>
#include <dirent.h>
#include <json.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "draw.h"
#include "flyout.h"
#include "popup.h"
#include "textfield.h"
#include "stringop.h"

/*
 * Flyouts of the network, volume and battery widgets. They only exist while
 * open: data is fetched with nmcli, pactl and powerprofilesctl when a flyout
 * opens and after each change, and nothing runs in the background otherwise.
 */

#define FLYOUT_WIDTH 360
#define PAD 16
#define FOOTER 44

/* ================= shared helpers ================= */

void fly_style_init(struct fly_style *st, struct panel *panel) {
	const struct tw_theme *t = panel->theme;
	st->style = panel_style(panel);
	st->fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	st->dim = tw_theme_color(t, "menu.disabled_fg", 0x6d6d6dff);
	st->accent = st->style == PS_CLASSIC ? 0x000080ff :
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff);
	uint32_t bg = tw_theme_color(t, "menu.bg", 0xf2f2f2ff);
	int luma = (int)((bg >> 24 & 0xff) * 299 + (bg >> 16 & 0xff) * 587 + (bg >> 8 & 0xff) * 114) / 1000;
	st->dark = luma < 128;
	uint32_t overlay = st->dark ? 0xffffff00 : 0x00000000;
	st->hover = st->style == PS_CLASSIC ? 0x00008024 : overlay | 0x14;
	st->track = st->style == PS_CLASSIC ? 0x808080ff : overlay | (st->dark ? 0x50 : 0x3d);
	st->line = st->style == PS_CLASSIC ? 0x808080ff : overlay | (st->dark ? 0x2a : 0x1f);
	st->button_bg = overlay | (st->dark ? 0x18 : 0x10);
	st->button_hover = overlay | (st->dark ? 0x30 : 0x24);
	st->button_border = overlay | (st->dark ? 0x30 : 0x26);
	st->field_bg = tw_theme_color(t, "menu.field_bg", st->dark ? 0x1f1f1fff : 0xffffffff);
	st->field_fg = tw_theme_color(t, "menu.field_fg", st->dark ? 0xffffffff : 0x000000ff);
	st->error = 0xc42b1cff;
	st->font = tw_theme_str(t, "menu.font", bar_font(panel));
	st->bold = bar_bold_font(panel);
	const char *space = strrchr(st->bold, ' ');
	if (space && atoi(space + 1) > 0) {
		snprintf(st->big, sizeof(st->big), "%.*s 18", (int)(space - st->bold), st->bold);
	} else {
		snprintf(st->big, sizeof(st->big), "%s 18", st->bold);
	}
}

struct flyout {
	struct panel *panel;
	struct popup *popup;
	struct popup_anchor anchor;
	char *settings; // command of the footer link, NULL hides it
	double px, py;
	bool inside;
};

static void flyout_geometry(struct flyout *f, int content_height, int *x, int *y,
		int *width, int *height) {
	int M = popup_shadow_margin(f->panel);
	*width = FLYOUT_WIDTH + 2 * M;
	*height = content_height + 2 * M;
	*x = f->anchor.right_align ? f->anchor.x - *width + M : f->anchor.x - M;
	*y = f->anchor.above ? f->anchor.y - *height : f->anchor.y;
}

static bool flyout_open(struct flyout *f, enum popup_kind kind, int content_height,
		const struct popup_vtable *vtable, void *data) {
	int x, y, width, height;
	flyout_geometry(f, content_height, &x, &y, &width, &height);
	f->popup = popup_create(f->panel, kind, NULL, f->anchor.output, x, y, width, height,
		vtable, data);
	return f->popup != NULL;
}

static void flyout_resize(struct flyout *f, int content_height) {
	int x, y, width, height;
	flyout_geometry(f, content_height, &x, &y, &width, &height);
	if (height != f->popup->height && height <= f->anchor.output->height) {
		popup_move_resize(f->popup, x, y, width, height);
	}
	popup_set_dirty(f->popup);
}

static void flyout_motion(struct popup *p, double x, double y) {
	struct flyout *f = p->data;
	f->px = x;
	f->py = y;
	f->inside = true;
	popup_set_dirty(p);
}

static void flyout_leave(struct popup *p) {
	struct flyout *f = p->data;
	f->inside = false;
	popup_set_dirty(p);
}

static bool hovered(struct flyout *f, struct pbox b) {
	return f->inside && pbox_contains(&b, f->px, f->py);
}

static void run_settings(struct flyout *f) {
	if (f->settings) {
		bar_run_command(f->panel, f->settings, NULL);
	}
	popup_close_later(f->panel);
}

static char *shell_quote(const char *s) {
	size_t len = 3;
	for (const char *p = s; *p; p++) {
		len += *p == '\'' ? 4 : 1;
	}
	char *out = malloc(len);
	char *o = out;
	*o++ = '\'';
	for (const char *p = s; *p; p++) {
		if (*p == '\'') {
			memcpy(o, "'\\''", 4);
			o += 4;
		} else {
			*o++ = *p;
		}
	}
	*o++ = '\'';
	*o = '\0';
	return out;
}

void fill_hover(cairo_t *cr, const struct fly_style *st, struct pbox b) {
	cairo_new_path(cr);
	pd_rounded(cr, b.x, b.y, b.width, b.height, st->style == PS_CLASSIC ? 0 : 4);
	pd_color(cr, st->hover);
	cairo_fill(cr);
}

void draw_line(cairo_t *cr, const struct fly_style *st, struct popup *p, int y) {
	int M = popup_shadow_margin(p->panel);
	pd_rect(cr, M, y, p->surface->width - 2 * M, 1, st->line);
}

int slider_value(struct pbox b, double x) {
	double v = (x - b.x - 10) * 100.0 / (b.width - 20);
	return v < 0 ? 0 : v > 100 ? 100 : (int)(v + 0.5);
}

void draw_slider(cairo_t *cr, const struct fly_style *st, struct pbox b, int value) {
	double x = b.x + 10, w = b.width - 20, cy = b.y + b.height / 2.0;
	value = value < 0 ? 0 : value > 100 ? 100 : value;
	double kx = x + w * value / 100.0;
	if (st->style == PS_CLASSIC) {
		pd_bevel(cr, x, cy - 2, w, 4, true);
		pd_rect(cr, kx - 5, cy - 10, 11, 20, 0xc0c0c0ff);
		pd_bevel(cr, kx - 5, cy - 10, 11, 20, false);
		return;
	}
	cairo_new_path(cr);
	pd_rounded(cr, x, cy - 2, w, 4, 2);
	pd_color(cr, st->track);
	cairo_fill(cr);
	if (kx > x + 2) {
		cairo_new_path(cr);
		pd_rounded(cr, x, cy - 2, kx - x, 4, 2);
		pd_color(cr, st->accent);
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	if (st->style == PS_LUNA || st->style == PS_AERO) {
		pd_rounded(cr, kx - 5.5, cy - 9.5, 11, 19, 3);
		pd_color(cr, 0xf4f4f4ff);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000066);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		cairo_arc(cr, kx, cy, 9, 0, 2 * M_PI);
		pd_color(cr, 0xffffffff);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000033);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_arc(cr, kx, cy, 5, 0, 2 * M_PI);
		pd_color(cr, st->accent);
		cairo_fill(cr);
	}
}

void draw_switch(cairo_t *cr, const struct fly_style *st, int x, int y, bool on) {
	if (st->style == PS_CLASSIC) {
		pd_rect(cr, x, y + 3, 14, 14, 0xffffffff);
		pd_bevel(cr, x, y + 3, 14, 14, true);
		if (on) {
			cairo_new_path(cr);
			cairo_move_to(cr, x + 3, y + 10);
			cairo_line_to(cr, x + 6, y + 13);
			cairo_line_to(cr, x + 11, y + 6);
			pd_color(cr, 0x000000ff);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
		return;
	}
	cairo_new_path(cr);
	pd_rounded(cr, x + 0.5, y + 0.5, 39, 19, 9.5);
	if (on) {
		pd_color(cr, st->accent);
		cairo_fill(cr);
	} else {
		pd_color(cr, st->fg);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	cairo_new_path(cr);
	cairo_arc(cr, on ? x + 30 : x + 10, y + 10, 5, 0, 2 * M_PI);
	pd_color(cr, on ? 0xffffffff : st->fg);
	cairo_fill(cr);
}

void draw_button(cairo_t *cr, const struct fly_style *st, struct pbox b,
		const char *label, bool primary, bool hover) {
	if (st->style == PS_CLASSIC) {
		pd_rect(cr, b.x, b.y, b.width, b.height, 0xc0c0c0ff);
		pd_bevel(cr, b.x, b.y, b.width, b.height, primary);
		pd_text(cr, primary ? st->bold : st->font, label, b.x, b.y, b.width, b.height,
			0x000000ff, PD_CENTER);
		return;
	}
	cairo_new_path(cr);
	pd_rounded(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1, 4);
	pd_color(cr, primary ? st->accent : hover ? st->button_hover : st->button_bg);
	cairo_fill_preserve(cr);
	pd_color(cr, st->button_border);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (primary && hover) {
		cairo_new_path(cr);
		pd_rounded(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1, 4);
		pd_color(cr, 0xffffff30);
		cairo_fill(cr);
	}
	pd_text(cr, st->font, label, b.x, b.y, b.width, b.height,
		primary ? 0xffffffff : st->fg, PD_CENTER);
}

static void draw_field(cairo_t *cr, const struct fly_style *st, struct pbox b,
		const char *text, const struct text_cursor *tc, bool mask, const char *placeholder) {
	if (st->style == PS_CLASSIC) {
		pd_rect(cr, b.x, b.y, b.width, b.height, st->field_bg);
		pd_bevel(cr, b.x, b.y, b.width, b.height, true);
	} else {
		cairo_new_path(cr);
		pd_rounded(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1, 4);
		pd_color(cr, st->field_bg);
		cairo_fill_preserve(cr);
		pd_color(cr, st->button_border | 0x20);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_rect(cr, b.x + 1, b.y + b.height - 2, b.width - 2, 2, st->accent);
	}
	if (!*text) {
		pd_text(cr, st->font, placeholder, b.x + 8, b.y, b.width - 16, b.height, st->dim,
			PD_LEFT);
	}
	struct text_style ts = { .font = st->font, .fg = st->field_fg, .selection_bg = st->accent,
		.selection_fg = 0xffffffff, .caret = true, .mask = mask };
	text_draw(cr, &ts, text, tc, b.x + 8, b.y, b.width - 16, b.height);
}

static void draw_link(cairo_t *cr, const struct fly_style *st, struct flyout *f, struct pbox b,
		const char *label, enum pd_align align) {
	int tw = 0;
	pd_text_size(cr, st->font, label, &tw, NULL);
	uint32_t color = st->style == PS_CLASSIC ? 0x0000ffff : st->accent;
	pd_text(cr, st->font, label, b.x, b.y, b.width, b.height, color, align);
	if (hovered(f, b)) {
		double lx = align == PD_RIGHT ? b.x + b.width - tw : b.x;
		pd_rect(cr, lx, b.y + b.height / 2.0 + 9, tw, 1, color);
	}
}

static bool read_sys(const char *path, char *buf, size_t size) {
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	size_t n = fread(buf, 1, size - 1, file);
	fclose(file);
	buf[n] = '\0';
	while (n > 0 && isspace((unsigned char)buf[n - 1])) {
		buf[--n] = '\0';
	}
	return true;
}

struct popup_anchor flyout_anchor(struct panel *panel, struct panel_output *output) {
	bool bottom = panel->config ? panel->config->layouts[panel->layout].bottom : true;
	int height = bar_height(panel);
	struct popup_anchor anchor = {
		.output = output,
		.x = output->width - 4,
		.y = bottom ? output->height - height : height,
		.above = bottom,
		.right_align = true,
	};
	return anchor;
}

/* ================= network ================= */

#define NET_HEADER 64
#define NET_ROW 48
#define NET_ROWS 6
#define NET_EXPAND 42
#define NET_MESSAGE 56

enum {
	NET_HS_TOGGLE = 1,
	NET_HS_ROW,
	NET_HS_CONNECT,
	NET_HS_DISCONNECT,
	NET_HS_CANCEL,
	NET_HS_SETTINGS,
	NET_HS_REFRESH,
};

struct wifi_net {
	char *ssid;
	char *security;
	int signal;
	bool active, known;
};

struct net_flyout {
	struct flyout base; // first member
	list_t *nets; // struct wifi_net *
	char *wifi_device;
	char *wired_connection;
	bool wired;
	bool have_nmcli, wifi_enabled, loaded, rescanned;
	char *expanded; // SSID of the expanded row
	bool asking_password;
	char password[128];
	struct text_cursor password_cursor;
	char status[256];
	bool status_error;
	int scroll;
	struct loop_timer *timer;
};

static struct net_flyout *net_current = NULL;

static void wifi_net_free(struct wifi_net *n) {
	free(n->ssid);
	free(n->security);
	free(n);
}

static void net_clear(struct net_flyout *f) {
	for (int i = 0; i < f->nets->length; i++) {
		wifi_net_free(f->nets->items[i]);
	}
	f->nets->length = 0;
	free(f->wifi_device);
	free(f->wired_connection);
	f->wifi_device = NULL;
	f->wired_connection = NULL;
	f->wired = false;
}

static int signal_bars(int signal) {
	return signal > 75 ? 4 : signal > 50 ? 3 : signal > 25 ? 2 : 1;
}

static bool net_list_shown(struct net_flyout *f) {
	return f->have_nmcli && f->wifi_device && f->wifi_enabled && f->nets->length > 0;
}

static int net_visible_rows(struct net_flyout *f) {
	return f->nets->length < NET_ROWS ? f->nets->length : NET_ROWS;
}

static int net_height(struct net_flyout *f) {
	int h = NET_HEADER + 1;
	if (net_list_shown(f)) {
		h += net_visible_rows(f) * NET_ROW + 8 + (f->expanded ? NET_EXPAND : 0);
	} else {
		h += NET_MESSAGE;
	}
	if (f->status[0]) {
		h += 28;
	}
	return h + FOOTER;
}

static void net_update(struct net_flyout *f) {
	flyout_resize(&f->base, net_height(f));
}

static struct wifi_net *net_active(struct net_flyout *f) {
	for (int i = 0; i < f->nets->length; i++) {
		struct wifi_net *n = f->nets->items[i];
		if (n->active) {
			return n;
		}
	}
	return NULL;
}

/* Splits an nmcli terse line at unescaped ':' and removes the escapes. */
static list_t *terse_split(const char *line) {
	list_t *fields = create_list();
	char *buf = malloc(strlen(line) + 1);
	size_t n = 0;
	for (const char *p = line;; p++) {
		if (*p == '\\' && p[1]) {
			buf[n++] = *++p;
		} else if (*p == ':' || *p == '\0') {
			buf[n] = '\0';
			list_add(fields, strdup(buf));
			n = 0;
			if (!*p) {
				break;
			}
		} else {
			buf[n++] = *p;
		}
	}
	free(buf);
	return fields;
}

static int wifi_net_cmp(const void *a, const void *b) {
	const struct wifi_net *na = *(struct wifi_net **)a;
	const struct wifi_net *nb = *(struct wifi_net **)b;
	if (na->active != nb->active) {
		return na->active ? -1 : 1;
	}
	return nb->signal - na->signal;
}

static void net_query(struct net_flyout *f, bool rescan);

static void net_query_done(void *data, const char *output) {
	struct net_flyout *f = net_current;
	if (!f) {
		return;
	}
	net_clear(f);
	f->have_nmcli = !strstr(output, "--missing");
	enum { S_RADIO, S_DEVICES, S_WIFI, S_KNOWN } section = S_RADIO;
	list_t *known = create_list();
	char *copy = strdup(output);
	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strcmp(line, "--dev") == 0) {
			section = S_DEVICES;
			continue;
		} else if (strcmp(line, "--wifi") == 0) {
			section = S_WIFI;
			continue;
		} else if (strcmp(line, "--known") == 0) {
			section = S_KNOWN;
			continue;
		}
		if (section == S_RADIO) {
			f->wifi_enabled = strcmp(line, "enabled") == 0;
			continue;
		}
		list_t *fields = terse_split(line);
		char **v = (char **)fields->items;
		if (section == S_DEVICES && fields->length >= 4) {
			if (strcmp(v[1], "wifi") == 0 && !f->wifi_device) {
				f->wifi_device = strdup(v[0]);
			} else if (strcmp(v[1], "ethernet") == 0 && !f->wired &&
					strncmp(v[2], "connected", 9) == 0) {
				f->wired = true;
				f->wired_connection = strdup(v[3]);
			}
		} else if (section == S_WIFI && fields->length >= 4 && v[3][0]) {
			struct wifi_net *found = NULL;
			for (int i = 0; i < f->nets->length && !found; i++) {
				struct wifi_net *n = f->nets->items[i];
				if (strcmp(n->ssid, v[3]) == 0) {
					found = n;
				}
			}
			if (!found) {
				found = calloc(1, sizeof(*found));
				found->ssid = strdup(v[3]);
				found->security = strdup(v[2]);
				list_add(f->nets, found);
			}
			if (atoi(v[1]) > found->signal) {
				found->signal = atoi(v[1]);
			}
			found->active |= strcmp(v[0], "*") == 0;
		} else if (section == S_KNOWN && fields->length >= 2 &&
				strcmp(v[1], "802-11-wireless") == 0) {
			list_add(known, strdup(v[0]));
		}
		list_free_items_and_destroy(fields);
	}
	free(copy);
	for (int i = 0; i < f->nets->length; i++) {
		struct wifi_net *n = f->nets->items[i];
		for (int j = 0; j < known->length; j++) {
			n->known |= strcmp(known->items[j], n->ssid) == 0;
		}
	}
	list_free_items_and_destroy(known);
	list_qsort(f->nets, wifi_net_cmp);
	if (f->scroll > f->nets->length - net_visible_rows(f)) {
		f->scroll = 0;
	}
	f->loaded = true;
	if (!f->rescanned && f->have_nmcli && f->wifi_device && f->wifi_enabled) {
		f->rescanned = true;
		net_query(f, true);
	}
	net_update(f);
}

static void net_query(struct net_flyout *f, bool rescan) {
	char *cmd = format_str("command -v nmcli >/dev/null || { echo --missing; exit 0; }; "
		"nmcli radio wifi 2>/dev/null; echo --dev; "
		"nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device 2>/dev/null; echo --wifi; "
		"nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID device wifi list --rescan %s 2>/dev/null; "
		"echo --known; nmcli -t -f NAME,TYPE connection show 2>/dev/null",
		rescan ? "auto" : "no");
	proc_run(f->base.panel, cmd, false, NULL, net_query_done, NULL);
	free(cmd);
}

static void net_requery(void *data) {
	struct net_flyout *f = net_current;
	if (f) {
		f->timer = NULL;
		net_query(f, true);
	}
}

static void net_action_done(void *data, const char *output) {
	struct net_flyout *f = net_current;
	if (!f) {
		return;
	}
	const char *error = strstr(output, "Error");
	if (error) {
		snprintf(f->status, sizeof(f->status), "%.*s", (int)strcspn(error, "\n"), error);
		f->status_error = true;
	} else {
		f->status[0] = '\0';
		f->status_error = false;
		f->asking_password = false;
		free(f->expanded);
		f->expanded = NULL;
	}
	net_query(f, false);
	net_update(f);
}

static void net_run_action(struct net_flyout *f, const char *command, const char *status) {
	snprintf(f->status, sizeof(f->status), "%s", status);
	f->status_error = false;
	proc_run(f->base.panel, command, false, NULL, net_action_done, NULL);
	net_update(f);
}

static void net_connect(struct net_flyout *f, int index) {
	if (index < 0 || index >= f->nets->length) {
		return;
	}
	struct wifi_net *n = f->nets->items[index];
	bool secured = n->security[0] && strcmp(n->security, "--") != 0;
	if (secured && !n->known && !f->asking_password) {
		f->asking_password = true;
		f->password[0] = '\0';
		net_update(f);
		return;
	}
	if (secured && !n->known && !f->password[0]) {
		return;
	}
	char *ssid = shell_quote(n->ssid);
	char *status = format_str("Connecting to %s...", n->ssid);
	char *cmd;
	if (n->known) {
		cmd = format_str("nmcli connection up id %s 2>&1", ssid);
	} else if (secured) {
		// the password reaches nmcli through the environment of the child,
		// so it never appears in the shell's command line
		setenv("TILEWIN_WIFI_PSK", f->password, 1);
		cmd = format_str("nmcli device wifi connect %s password \"$TILEWIN_WIFI_PSK\" 2>&1",
			ssid);
	} else {
		cmd = format_str("nmcli device wifi connect %s 2>&1", ssid);
	}
	net_run_action(f, cmd, status);
	unsetenv("TILEWIN_WIFI_PSK");
	memset(f->password, 0, sizeof(f->password));
	free(cmd);
	free(status);
	free(ssid);
}

static int net_expanded_index(struct net_flyout *f) {
	for (int i = 0; f->expanded && i < f->nets->length; i++) {
		struct wifi_net *n = f->nets->items[i];
		if (strcmp(n->ssid, f->expanded) == 0) {
			return i;
		}
	}
	return -1;
}

static void net_render(struct popup *p, cairo_t *cr) {
	struct net_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	struct wifi_net *active = net_active(f);
	const char *title, *subtitle;
	if (active) {
		title = active->ssid;
		subtitle = active->security[0] ? "Connected, secured" : "Connected";
	} else if (f->wired) {
		title = f->wired_connection && *f->wired_connection ? f->wired_connection : "Ethernet";
		subtitle = "Connected";
	} else {
		title = "Not connected";
		subtitle = !f->loaded ? "Checking..." : f->have_nmcli ? "No network connection" :
			"NetworkManager (nmcli) was not found";
	}
	ti_network(p->panel, cr, x0, y + 18, 28, active ? signal_bars(active->signal) : 4,
		active || !f->wired, active || f->wired, st.fg);
	int text_w = cw - 44 - (f->wifi_device ? 100 : 0);
	pd_text(cr, st.bold, title, x0 + 44, y + 12, text_w, 22, st.fg, PD_LEFT);
	pd_text(cr, st.font, subtitle, x0 + 44, y + 33, text_w, 20, st.dim, PD_LEFT);
	if (f->have_nmcli && f->wifi_device) {
		int sx = x0 + cw - 40, sy = y + 22;
		pd_text(cr, st.font, "Wi-Fi", sx - 54, sy, 46, 20, st.fg, PD_RIGHT);
		draw_switch(cr, &st, sx, sy, f->wifi_enabled);
		psurface_add_hotspot(p->surface, sx - 56, sy - 6, 96, 32, NULL, NET_HS_TOGGLE, 0, NULL);
	}
	y += NET_HEADER;
	draw_line(cr, &st, p, y);
	y += 1;

	if (net_list_shown(f)) {
		y += 4;
		int rows = net_visible_rows(f);
		for (int i = f->scroll; i < f->nets->length && i < f->scroll + rows; i++) {
			struct wifi_net *n = f->nets->items[i];
			bool expanded = f->expanded && strcmp(f->expanded, n->ssid) == 0;
			struct pbox row = { M + 6, y, W - 2 * M - 12, NET_ROW + (expanded ? NET_EXPAND : 0) };
			if (expanded || hovered(&f->base, row)) {
				fill_hover(cr, &st, row);
			}
			ti_network(p->panel, cr, x0, y + 14, 20, signal_bars(n->signal), true, true, st.fg);
			pd_text(cr, n->active ? st.bold : st.font, n->ssid, x0 + 34, y + 6, cw - 34, 20,
				st.fg, PD_LEFT);
			bool secured = n->security[0] && strcmp(n->security, "--") != 0;
			char sub[96];
			snprintf(sub, sizeof(sub), "%s%s%s", n->active ? "Connected, " : "",
				secured ? "secured" : "open", n->known && !n->active ? ", saved" : "");
			sub[0] = toupper((unsigned char)sub[0]);
			pd_text(cr, st.font, sub, x0 + 34, y + 25, cw - 34, 18, st.dim, PD_LEFT);
			psurface_add_hotspot(p->surface, row.x, row.y, row.width, NET_ROW, NULL,
				NET_HS_ROW, i, NULL);
			if (expanded) {
				int by = y + NET_ROW + 2, bh = 30;
				if (f->asking_password) {
					struct pbox field = { x0 + 34, by, cw - 34 - 170, bh };
					draw_field(cr, &st, field, f->password, &f->password_cursor, true,
						"Password");
					struct pbox ok = { x0 + cw - 164, by, 80, bh };
					struct pbox cancel = { x0 + cw - 80, by, 80, bh };
					draw_button(cr, &st, ok, "Connect", true, hovered(&f->base, ok));
					draw_button(cr, &st, cancel, "Cancel", false, hovered(&f->base, cancel));
					psurface_add_hotspot(p->surface, ok.x, ok.y, ok.width, ok.height, NULL,
						NET_HS_CONNECT, i, NULL);
					psurface_add_hotspot(p->surface, cancel.x, cancel.y, cancel.width,
						cancel.height, NULL, NET_HS_CANCEL, i, NULL);
				} else {
					struct pbox button = { x0 + cw - 110, by, 110, bh };
					draw_button(cr, &st, button, n->active ? "Disconnect" : "Connect",
						!n->active, hovered(&f->base, button));
					psurface_add_hotspot(p->surface, button.x, button.y, button.width,
						button.height, NULL, n->active ? NET_HS_DISCONNECT : NET_HS_CONNECT, i,
						NULL);
				}
			}
			y += row.height;
		}
		y += 4;
	} else {
		const char *message = !f->loaded ? "Searching for networks..." :
			!f->have_nmcli ? "Install NetworkManager to manage Wi-Fi here." :
			!f->wifi_device ? "No Wi-Fi adapter found." :
			!f->wifi_enabled ? "Wi-Fi is turned off." : "Searching for networks...";
		pd_text(cr, st.font, message, x0, y, cw, NET_MESSAGE, st.dim, PD_CENTER);
		y += NET_MESSAGE;
	}
	if (f->status[0]) {
		pd_text(cr, st.font, f->status, x0, y, cw, 28, f->status_error ? st.error : st.dim,
			PD_LEFT);
		y += 28;
	}
	draw_line(cr, &st, p, y);
	struct pbox settings = { x0, y + 1, cw / 2, FOOTER - 1 };
	struct pbox refresh = { x0 + cw / 2, y + 1, cw / 2, FOOTER - 1 };
	if (f->base.settings) {
		draw_link(cr, &st, &f->base, settings, "Network settings", PD_LEFT);
		psurface_add_hotspot(p->surface, settings.x, settings.y, settings.width,
			settings.height, NULL, NET_HS_SETTINGS, 0, NULL);
	}
	if (f->have_nmcli && f->wifi_device && f->wifi_enabled) {
		draw_link(cr, &st, &f->base, refresh, "Refresh", PD_RIGHT);
		psurface_add_hotspot(p->surface, refresh.x, refresh.y, refresh.width, refresh.height,
			NULL, NET_HS_REFRESH, 0, NULL);
	}
}

static void net_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct net_flyout *f = p->data;
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	struct wifi_net *n = hs->id >= 0 && hs->id < f->nets->length ? f->nets->items[hs->id] : NULL;
	switch (hs->kind) {
	case NET_HS_TOGGLE:
		f->wifi_enabled = !f->wifi_enabled;
		net_run_action(f, f->wifi_enabled ? "nmcli radio wifi on 2>&1" :
			"nmcli radio wifi off 2>&1", f->wifi_enabled ? "Turning Wi-Fi on..." :
			"Turning Wi-Fi off...");
		if (f->wifi_enabled && !f->timer) {
			f->timer = loop_add_timer(p->panel->loop, 4000, net_requery, NULL);
		}
		break;
	case NET_HS_ROW:
		if (n && f->expanded && strcmp(f->expanded, n->ssid) == 0) {
			if (!f->asking_password) {
				free(f->expanded);
				f->expanded = NULL;
			}
		} else if (n) {
			free(f->expanded);
			f->expanded = strdup(n->ssid);
			f->asking_password = false;
			memset(f->password, 0, sizeof(f->password));
			// keep the expanded row visible
			if (hs->id >= f->scroll + NET_ROWS - 1 && f->nets->length > NET_ROWS) {
				f->scroll = hs->id - NET_ROWS + 2;
			}
		}
		net_update(f);
		break;
	case NET_HS_CONNECT:
		net_connect(f, (int)hs->id);
		break;
	case NET_HS_DISCONNECT:
		if (f->wifi_device) {
			char *device = shell_quote(f->wifi_device);
			char *cmd = format_str("nmcli device disconnect %s 2>&1", device);
			net_run_action(f, cmd, "Disconnecting...");
			free(cmd);
			free(device);
		}
		break;
	case NET_HS_CANCEL:
		f->asking_password = false;
		memset(f->password, 0, sizeof(f->password));
		net_update(f);
		break;
	case NET_HS_SETTINGS:
		run_settings(&f->base);
		break;
	case NET_HS_REFRESH:
		snprintf(f->status, sizeof(f->status), "Searching for networks...");
		f->status_error = false;
		net_query(f, true);
		net_update(f);
		break;
	}
}

static void net_axis(struct popup *p, double x, double y, int direction) {
	struct net_flyout *f = p->data;
	int max = f->nets->length - net_visible_rows(f);
	f->scroll += direction;
	f->scroll = f->scroll < 0 ? 0 : f->scroll > max ? max : f->scroll;
	popup_set_dirty(p);
}

static void net_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct net_flyout *f = p->data;
	if (!f->asking_password) {
		if (sym == XKB_KEY_Escape) {
			popup_close_later(p->panel);
		}
		return;
	}
	if (sym == XKB_KEY_Escape) {
		f->asking_password = false;
		memset(f->password, 0, sizeof(f->password));
	} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		net_connect(f, net_expanded_index(f));
		return;
	} else if (text_key(f->password, sizeof(f->password), &f->password_cursor, sym, utf8,
			mods) == TEXT_KEY_IGNORED) {
		return;
	}
	net_update(f);
}

static void net_destroy(struct popup *p) {
	struct net_flyout *f = p->data;
	if (net_current == f) {
		net_current = NULL;
	}
	if (f->timer) {
		loop_remove_timer(p->panel->loop, f->timer);
	}
	memset(f->password, 0, sizeof(f->password));
	net_clear(f);
	list_free(f->nets);
	free(f->expanded);
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable net_vtable = {
	.render = net_render,
	.motion = flyout_motion,
	.leave = flyout_leave,
	.button = net_button,
	.axis = net_axis,
	.key = net_key,
	.destroy = net_destroy,
};

void flyout_network_toggle(struct panel *panel, struct popup_anchor anchor,
		const char *settings) {
	if (popup_is_open(panel, POPUP_NETWORK)) {
		popup_close_all(panel);
		return;
	}
	struct net_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = strdup(settings ? settings : TW_NETWORK_SETTINGS);
	f->nets = create_list();
	f->have_nmcli = true;
	if (!flyout_open(&f->base, POPUP_NETWORK, net_height(f), &net_vtable, f)) {
		list_free(f->nets);
		free(f->base.settings);
		free(f);
		return;
	}
	net_current = f;
	net_query(f, false);
}

/* ================= volume ================= */

#define VOL_MASTER 64
#define VOL_DEVICE 40
#define VOL_DEVICE_ROW 36
#define VOL_APPS_TITLE 30
#define VOL_APP 56
#define VOL_MAX_APPS 5

enum {
	VOL_HS_MUTE = 1,
	VOL_HS_SLIDER,
	VOL_HS_DEVICES,
	VOL_HS_SINK,
	VOL_HS_MIXER,
};

struct sink {
	char *name, *description;
	int volume;
	bool muted;
};

struct stream {
	int64_t index;
	char *name, *icon;
	int volume;
	bool muted;
};

struct volume_flyout {
	struct flyout base; // first member
	list_t *sinks;   // struct sink *
	list_t *streams; // struct stream *
	char *default_sink;
	bool loaded, available, devices_open, query_pending;
	bool dragging, drag_dirty;
	int64_t drag_id; // -1 for the output, otherwise a stream index
	struct pbox drag_box;
	int drag_value;
	struct loop_timer *drag_timer, *refresh_timer;
};

static struct volume_flyout *vol_current = NULL;

static void vol_clear(struct volume_flyout *f) {
	for (int i = 0; i < f->sinks->length; i++) {
		struct sink *s = f->sinks->items[i];
		free(s->name);
		free(s->description);
		free(s);
	}
	f->sinks->length = 0;
	for (int i = 0; i < f->streams->length; i++) {
		struct stream *s = f->streams->items[i];
		free(s->name);
		free(s->icon);
		free(s);
	}
	f->streams->length = 0;
}

static int vol_shown_streams(struct volume_flyout *f) {
	return f->streams->length < VOL_MAX_APPS ? f->streams->length : VOL_MAX_APPS;
}

static int vol_height(struct volume_flyout *f) {
	if (!f->available) {
		return 72 + FOOTER;
	}
	int h = VOL_MASTER + VOL_DEVICE;
	if (f->devices_open) {
		h += f->sinks->length * VOL_DEVICE_ROW + 4;
	}
	if (f->streams->length) {
		h += 1 + VOL_APPS_TITLE + vol_shown_streams(f) * VOL_APP;
	}
	return h + FOOTER;
}

static void vol_update(struct volume_flyout *f) {
	flyout_resize(&f->base, vol_height(f));
}

static struct sink *vol_default_sink(struct volume_flyout *f) {
	for (int i = 0; i < f->sinks->length; i++) {
		struct sink *s = f->sinks->items[i];
		if (f->default_sink && strcmp(s->name, f->default_sink) == 0) {
			return s;
		}
	}
	return f->sinks->length ? f->sinks->items[0] : NULL;
}

static int json_volume(json_object *obj) {
	json_object *volume;
	int sum = 0, count = 0;
	if (json_object_object_get_ex(obj, "volume", &volume) &&
			json_object_is_type(volume, json_type_object)) {
		json_object_object_foreach(volume, channel_name, channel) {
			json_object *percent;
			if (channel_name && json_object_object_get_ex(channel, "value_percent", &percent)) {
				sum += atoi(json_object_get_string(percent));
				count++;
			}
		}
	}
	return count ? sum / count : 0;
}

static const char *json_str(json_object *obj, const char *key) {
	json_object *value;
	return obj && json_object_object_get_ex(obj, key, &value) ?
		json_object_get_string(value) : NULL;
}

static bool json_flag(json_object *obj, const char *key) {
	json_object *value;
	return json_object_object_get_ex(obj, key, &value) && json_object_get_boolean(value);
}

static void vol_query_done(void *data, const char *output) {
	struct volume_flyout *f = vol_current;
	if (!f) {
		return;
	}
	f->query_pending = false;
	if (f->dragging) {
		return; // the slider is in the user's hand
	}
	vol_clear(f);
	const char *sinks = strstr(output, "--sinks\n");
	const char *inputs = strstr(output, "--inputs\n");
	f->loaded = true;
	if (!sinks || !inputs || inputs < sinks) {
		f->available = false;
		vol_update(f);
		return;
	}
	free(f->default_sink);
	f->default_sink = strndup(output, strcspn(output, "\n"));

	char *sinks_json = strndup(sinks + 8, inputs - (sinks + 8));
	json_object *array = json_tokener_parse(sinks_json);
	free(sinks_json);
	int n = array && json_object_is_type(array, json_type_array) ?
		(int)json_object_array_length(array) : 0;
	for (int i = 0; i < n; i++) {
		json_object *obj = json_object_array_get_idx(array, i);
		struct sink *s = calloc(1, sizeof(*s));
		s->name = strdup(json_str(obj, "name") ? json_str(obj, "name") : "");
		const char *description = json_str(obj, "description");
		s->description = strdup(description ? description : s->name);
		s->volume = json_volume(obj);
		s->muted = json_flag(obj, "mute");
		list_add(f->sinks, s);
	}
	json_object_put(array);

	array = json_tokener_parse(inputs + 9);
	n = array && json_object_is_type(array, json_type_array) ?
		(int)json_object_array_length(array) : 0;
	for (int i = 0; i < n; i++) {
		json_object *obj = json_object_array_get_idx(array, i);
		json_object *properties = NULL, *index;
		json_object_object_get_ex(obj, "properties", &properties);
		struct stream *s = calloc(1, sizeof(*s));
		s->index = json_object_object_get_ex(obj, "index", &index) ?
			json_object_get_int64(index) : -1;
		const char *name = json_str(properties, "application.name");
		if (!name) {
			name = json_str(properties, "media.name");
		}
		s->name = strdup(name ? name : "Unknown app");
		const char *icon = json_str(properties, "application.icon_name");
		if (!icon) {
			icon = json_str(properties, "application.process.binary");
		}
		s->icon = icon ? strdup(icon) : NULL;
		s->volume = json_volume(obj);
		s->muted = json_flag(obj, "mute");
		list_add(f->streams, s);
	}
	json_object_put(array);
	f->available = f->sinks->length > 0;
	vol_update(f);
}

static void vol_query(struct volume_flyout *f) {
	if (f->query_pending) {
		return;
	}
	f->query_pending = true;
	proc_run(f->base.panel, "pactl get-default-sink 2>/dev/null; echo --sinks; "
		"pactl -f json list sinks 2>/dev/null; echo; echo --inputs; "
		"pactl -f json list sink-inputs 2>/dev/null; echo", false, NULL, vol_query_done, NULL);
}

static void vol_refresh_timer(void *data) {
	struct volume_flyout *f = vol_current;
	if (f) {
		f->refresh_timer = NULL;
		vol_query(f);
	}
}

static void vol_query_later(struct volume_flyout *f) {
	if (!f->refresh_timer) {
		f->refresh_timer = loop_add_timer(f->base.panel->loop, 150, vol_refresh_timer, NULL);
	}
}

void flyout_volume_changed(struct panel *panel) {
	quicksettings_changed(panel);
	if (vol_current && !vol_current->dragging) {
		vol_query_later(vol_current);
	}
}

static void vol_apply_drag(struct volume_flyout *f, bool now);

static void vol_drag_timer(void *data) {
	struct volume_flyout *f = vol_current;
	if (!f) {
		return;
	}
	f->drag_timer = NULL;
	if (f->drag_dirty) {
		vol_apply_drag(f, false);
	}
}

/* Sets the dragged volume, at most every 60 ms while the pointer moves. */
static void vol_apply_drag(struct volume_flyout *f, bool now) {
	if (!now && f->drag_timer) {
		f->drag_dirty = true;
		return;
	}
	f->drag_dirty = false;
	char cmd[128];
	if (f->drag_id < 0) {
		snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%%", f->drag_value);
		struct sink *s = vol_default_sink(f);
		if (s) {
			s->volume = f->drag_value;
		}
	} else {
		snprintf(cmd, sizeof(cmd), "pactl set-sink-input-volume %lld %d%%",
			(long long)f->drag_id, f->drag_value);
		for (int i = 0; i < f->streams->length; i++) {
			struct stream *s = f->streams->items[i];
			if (s->index == f->drag_id) {
				s->volume = f->drag_value;
			}
		}
	}
	proc_spawn(cmd);
	if (!now) {
		f->drag_timer = loop_add_timer(f->base.panel->loop, 60, vol_drag_timer, NULL);
	}
}

static void draw_percent(cairo_t *cr, const struct fly_style *st, int x, int y, int h,
		int value) {
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	pd_text(cr, st->bold, text, x, y, 40, h, st->fg, PD_RIGHT);
}

static void vol_render(struct popup *p, cairo_t *cr) {
	struct volume_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	if (!f->available) {
		pd_text(cr, st.font, f->loaded ? "No audio output found (needs pactl)" : "Loading...",
			x0, y, cw, 72, st.dim, PD_CENTER);
		y += 72;
	} else {
		struct sink *sink = vol_default_sink(f);
		int volume = f->dragging && f->drag_id < 0 ? f->drag_value : sink->volume;
		struct pbox mute = { x0 - 6, y + 14, 36, 36 };
		if (hovered(&f->base, mute)) {
			fill_hover(cr, &st, mute);
		}
		ti_speaker(p->panel, cr, x0, y + 20, 24, volume, sink->muted, st.fg);
		psurface_add_hotspot(p->surface, mute.x, mute.y, mute.width, mute.height, NULL,
			VOL_HS_MUTE, -1, NULL);
		struct pbox slider = { x0 + 38, y + 14, cw - 38 - 46, 36 };
		draw_slider(cr, &st, slider, volume);
		psurface_add_hotspot(p->surface, slider.x, slider.y, slider.width, slider.height, NULL,
			VOL_HS_SLIDER, -1, NULL);
		draw_percent(cr, &st, x0 + cw - 40, y + 14, 36, volume);
		y += VOL_MASTER;

		struct pbox devices = { M + 6, y, W - 2 * M - 12, VOL_DEVICE - 4 };
		if (hovered(&f->base, devices) || f->devices_open) {
			fill_hover(cr, &st, devices);
		}
		char *label = format_str("%s  %s", sink->description, f->devices_open ?
			"\xe2\x96\xb4" : "\xe2\x96\xbe"); // small up / down triangle
		pd_text(cr, st.font, label, x0, y, cw, VOL_DEVICE - 4, st.fg, PD_LEFT);
		free(label);
		psurface_add_hotspot(p->surface, devices.x, devices.y, devices.width, devices.height,
			NULL, VOL_HS_DEVICES, 0, NULL);
		y += VOL_DEVICE;
		if (f->devices_open) {
			for (int i = 0; i < f->sinks->length; i++) {
				struct sink *s = f->sinks->items[i];
				struct pbox row = { M + 6, y, W - 2 * M - 12, VOL_DEVICE_ROW };
				if (hovered(&f->base, row)) {
					fill_hover(cr, &st, row);
				}
				if (s == sink) {
					pd_rect(cr, M + 8, y + 8, 3, VOL_DEVICE_ROW - 16, st.accent);
				}
				pd_text(cr, s == sink ? st.bold : st.font, s->description, x0 + 12, y,
					cw - 12, VOL_DEVICE_ROW, st.fg, PD_LEFT);
				psurface_add_hotspot(p->surface, row.x, row.y, row.width, row.height, NULL,
					VOL_HS_SINK, i, NULL);
				y += VOL_DEVICE_ROW;
			}
			y += 4;
		}

		if (f->streams->length) {
			draw_line(cr, &st, p, y);
			y += 1;
			pd_text(cr, st.font, "Apps", x0, y, cw, VOL_APPS_TITLE, st.dim, PD_LEFT);
			y += VOL_APPS_TITLE;
			for (int i = 0; i < vol_shown_streams(f); i++) {
				struct stream *s = f->streams->items[i];
				int value = f->dragging && f->drag_id == s->index ? f->drag_value : s->volume;
				pd_text(cr, st.font, s->name, x0 + 38, y, cw - 38, 18, st.fg, PD_LEFT);
				struct pbox app_mute = { x0 - 6, y + 14, 36, 36 };
				if (hovered(&f->base, app_mute)) {
					fill_hover(cr, &st, app_mute);
				}
				cairo_surface_t *icon = s->icon ?
					apps_icon(p->panel, s->icon, 24 * p->surface->scale) : NULL;
				if (icon && !s->muted) {
					pd_icon(cr, icon, x0, y + 20, 24);
				} else {
					ti_speaker(p->panel, cr, x0 + 2, y + 22, 20, value, s->muted, st.fg);
				}
				psurface_add_hotspot(p->surface, app_mute.x, app_mute.y, app_mute.width,
					app_mute.height, NULL, VOL_HS_MUTE, s->index, NULL);
				struct pbox app_slider = { x0 + 38, y + 16, cw - 38 - 46, 32 };
				draw_slider(cr, &st, app_slider, value);
				psurface_add_hotspot(p->surface, app_slider.x, app_slider.y, app_slider.width,
					app_slider.height, NULL, VOL_HS_SLIDER, s->index, NULL);
				draw_percent(cr, &st, x0 + cw - 40, y + 16, 32, value);
				y += VOL_APP;
			}
		}
	}
	draw_line(cr, &st, p, y);
	struct pbox mixer = { x0, y + 1, cw, FOOTER - 1 };
	draw_link(cr, &st, &f->base, mixer, "Volume mixer", PD_LEFT);
	psurface_add_hotspot(p->surface, mixer.x, mixer.y, cw / 2, mixer.height, NULL,
		VOL_HS_MIXER, 0, NULL);
}

static void vol_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct volume_flyout *f = p->data;
	if (button != BTN_LEFT) {
		return;
	}
	if (!pressed) {
		if (f->dragging) {
			vol_apply_drag(f, true);
			f->dragging = false;
			vol_query_later(f);
			popup_set_dirty(p);
		}
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	char cmd[512];
	switch (hs->kind) {
	case VOL_HS_SLIDER:
		f->dragging = true;
		f->drag_id = hs->id;
		f->drag_box = hs->box;
		f->drag_value = slider_value(hs->box, x);
		vol_apply_drag(f, false);
		popup_set_dirty(p);
		break;
	case VOL_HS_MUTE:
		if (hs->id < 0) {
			proc_spawn("pactl set-sink-mute @DEFAULT_SINK@ toggle");
			struct sink *s = vol_default_sink(f);
			if (s) {
				s->muted = !s->muted;
			}
		} else {
			snprintf(cmd, sizeof(cmd), "pactl set-sink-input-mute %lld toggle",
				(long long)hs->id);
			proc_spawn(cmd);
			for (int i = 0; i < f->streams->length; i++) {
				struct stream *s = f->streams->items[i];
				if (s->index == hs->id) {
					s->muted = !s->muted;
				}
			}
		}
		vol_query_later(f);
		popup_set_dirty(p);
		break;
	case VOL_HS_DEVICES:
		f->devices_open = !f->devices_open;
		vol_update(f);
		break;
	case VOL_HS_SINK:
		if (hs->id >= 0 && hs->id < f->sinks->length) {
			struct sink *s = f->sinks->items[hs->id];
			char *name = shell_quote(s->name);
			snprintf(cmd, sizeof(cmd), "pactl set-default-sink %s", name);
			free(name);
			proc_spawn(cmd);
			free(f->default_sink);
			f->default_sink = strdup(s->name);
			f->devices_open = false;
			vol_query_later(f);
			vol_update(f);
		}
		break;
	case VOL_HS_MIXER:
		run_settings(&f->base);
		break;
	}
}

static void vol_motion(struct popup *p, double x, double y) {
	struct volume_flyout *f = p->data;
	flyout_motion(p, x, y);
	if (f->dragging) {
		f->drag_value = slider_value(f->drag_box, x);
		vol_apply_drag(f, false);
	}
}

static void vol_axis(struct popup *p, double x, double y, int direction) {
	struct volume_flyout *f = p->data;
	struct sink *s = vol_default_sink(f);
	if (!s || f->dragging) {
		return;
	}
	f->drag_id = -1;
	f->drag_value = s->volume + (direction < 0 ? 5 : -5);
	f->drag_value = f->drag_value < 0 ? 0 : f->drag_value > 100 ? 100 : f->drag_value;
	vol_apply_drag(f, false);
	vol_query_later(f);
	popup_set_dirty(p);
}

static void vol_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	} else if (sym == XKB_KEY_Up || sym == XKB_KEY_Right) {
		vol_axis(p, 0, 0, -1);
	} else if (sym == XKB_KEY_Down || sym == XKB_KEY_Left) {
		vol_axis(p, 0, 0, 1);
	}
}

static void vol_destroy(struct popup *p) {
	struct volume_flyout *f = p->data;
	if (vol_current == f) {
		vol_current = NULL;
	}
	if (f->drag_timer) {
		loop_remove_timer(p->panel->loop, f->drag_timer);
	}
	if (f->refresh_timer) {
		loop_remove_timer(p->panel->loop, f->refresh_timer);
	}
	vol_clear(f);
	list_free(f->sinks);
	list_free(f->streams);
	free(f->default_sink);
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable vol_vtable = {
	.render = vol_render,
	.motion = vol_motion,
	.leave = flyout_leave,
	.button = vol_button,
	.axis = vol_axis,
	.key = vol_key,
	.destroy = vol_destroy,
};

void flyout_volume_toggle(struct panel *panel, struct popup_anchor anchor, const char *mixer) {
	if (popup_is_open(panel, POPUP_VOLUME)) {
		popup_close_all(panel);
		return;
	}
	struct volume_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = strdup(mixer ? mixer : "exec pavucontrol");
	f->sinks = create_list();
	f->streams = create_list();
	if (!flyout_open(&f->base, POPUP_VOLUME, vol_height(f), &vol_vtable, f)) {
		list_free(f->sinks);
		list_free(f->streams);
		free(f->base.settings);
		free(f);
		return;
	}
	vol_current = f;
	vol_query(f);
}

/* ================= battery and power ================= */

#define POWER_HEADER 84
#define POWER_BRIGHTNESS 56
#define POWER_MODES 80

enum {
	POWER_HS_BRIGHTNESS = 1,
	POWER_HS_PROFILE,
	POWER_HS_SETTINGS,
};

static const char *const profile_names[] = { "power-saver", "balanced", "performance" };
static const char *const profile_titles[] = { "Battery saver", "Balanced", "Performance" };

struct power_flyout {
	struct flyout base; // first member
	bool battery, charging;
	int capacity;
	char detail[96];
	bool backlight;
	int brightness;
	int profiles; // bit mask of available profiles
	int profile;  // index of the active profile
	bool dragging, drag_dirty;
	struct pbox drag_box;
	int drag_value;
	struct loop_timer *drag_timer, *tick;
};

static struct power_flyout *power_current = NULL;

static long read_long(const char *dir, const char *name) {
	char path[512], buf[64];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return read_sys(path, buf, sizeof(buf)) ? atol(buf) : -1;
}

static void format_minutes(char *out, size_t size, long minutes, const char *suffix) {
	if (minutes >= 60) {
		snprintf(out, size, "%ld h %ld min %s", minutes / 60, minutes % 60, suffix);
	} else {
		snprintf(out, size, "%ld min %s", minutes, suffix);
	}
}

static void power_read(struct power_flyout *f) {
	f->battery = false;
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *de;
	while (dir && (de = readdir(dir))) {
		char base[300], path[512], buf[64];
		snprintf(base, sizeof(base), "/sys/class/power_supply/%s", de->d_name);
		snprintf(path, sizeof(path), "%s/type", base);
		if (de->d_name[0] == '.' || !read_sys(path, buf, sizeof(buf)) ||
				strcmp(buf, "Battery") != 0) {
			continue;
		}
		long capacity = read_long(base, "capacity");
		if (capacity < 0) {
			continue;
		}
		f->battery = true;
		f->capacity = (int)capacity;
		char status[32] = "";
		snprintf(path, sizeof(path), "%s/status", base);
		read_sys(path, status, sizeof(status));
		f->charging = strcmp(status, "Charging") == 0;
		long now = read_long(base, "energy_now"), rate = read_long(base, "power_now");
		long full = read_long(base, "energy_full");
		if (now < 0) {
			now = read_long(base, "charge_now");
			rate = read_long(base, "current_now");
			full = read_long(base, "charge_full");
		}
		if (f->charging && rate > 0 && full > now) {
			format_minutes(f->detail, sizeof(f->detail), (full - now) * 60 / rate,
				"until fully charged");
		} else if (strcmp(status, "Discharging") == 0 && rate > 0 && now > 0) {
			format_minutes(f->detail, sizeof(f->detail), now * 60 / rate, "remaining");
		} else if (strcmp(status, "Full") == 0) {
			snprintf(f->detail, sizeof(f->detail), "Fully charged");
		} else if (strcmp(status, "Not charging") == 0) {
			snprintf(f->detail, sizeof(f->detail), "Plugged in, not charging");
		} else {
			snprintf(f->detail, sizeof(f->detail), "%s", f->charging ? "Charging" : status);
		}
		break;
	}
	if (dir) {
		closedir(dir);
	}
	if (!f->dragging) {
		f->backlight = false;
		dir = opendir("/sys/class/backlight");
		while (dir && (de = readdir(dir))) {
			char base[300];
			snprintf(base, sizeof(base), "/sys/class/backlight/%s", de->d_name);
			long current = read_long(base, "brightness"), max = read_long(base, "max_brightness");
			if (de->d_name[0] != '.' && current >= 0 && max > 0) {
				f->backlight = true;
				f->brightness = (int)((current * 100 + max / 2) / max);
				break;
			}
		}
		if (dir) {
			closedir(dir);
		}
	}
}

static int power_height(struct power_flyout *f) {
	int h = POWER_HEADER;
	if (f->backlight) {
		h += POWER_BRIGHTNESS;
	}
	if (f->profiles) {
		h += POWER_MODES;
	}
	return h + (f->base.settings ? FOOTER : 8);
}

static void power_update(struct power_flyout *f) {
	flyout_resize(&f->base, power_height(f));
}

static void power_profiles_done(void *data, const char *output) {
	struct power_flyout *f = power_current;
	if (!f) {
		return;
	}
	f->profiles = 0;
	char *copy = strdup(output);
	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		bool active = false;
		while (*line == ' ' || *line == '*') {
			active |= *line == '*';
			line++;
		}
		size_t len = strlen(line);
		if (len < 2 || line[len - 1] != ':') {
			continue;
		}
		line[len - 1] = '\0';
		for (int i = 0; i < 3; i++) {
			if (strcmp(line, profile_names[i]) == 0) {
				f->profiles |= 1 << i;
				if (active) {
					f->profile = i;
				}
			}
		}
	}
	free(copy);
	power_update(f);
}

static void power_query_profiles(struct power_flyout *f) {
	proc_run(f->base.panel, "powerprofilesctl list 2>/dev/null", false, NULL,
		power_profiles_done, NULL);
}

static void power_tick(void *data) {
	struct power_flyout *f = power_current;
	if (!f) {
		return;
	}
	power_read(f);
	power_update(f);
	f->tick = loop_add_timer(f->base.panel->loop, 10000, power_tick, NULL);
}

static void power_apply_drag(struct power_flyout *f, bool now);

static void power_drag_timer(void *data) {
	struct power_flyout *f = power_current;
	if (!f) {
		return;
	}
	f->drag_timer = NULL;
	if (f->drag_dirty) {
		power_apply_drag(f, false);
	}
}

static void power_apply_drag(struct power_flyout *f, bool now) {
	if (!now && f->drag_timer) {
		f->drag_dirty = true;
		return;
	}
	f->drag_dirty = false;
	int value = f->drag_value < 1 ? 1 : f->drag_value; // never switch the screen off
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "brightnessctl -q set %d%%", value);
	proc_spawn(cmd);
	f->brightness = value;
	if (!now) {
		f->drag_timer = loop_add_timer(f->base.panel->loop, 60, power_drag_timer, NULL);
	}
}

static void power_render(struct popup *p, cairo_t *cr) {
	struct power_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	if (f->battery) {
		ti_battery(p->panel, cr, x0, y + 20, 44, f->capacity, f->charging, st.fg);
		char percent[16];
		snprintf(percent, sizeof(percent), "%d%%", f->capacity);
		pd_text(cr, st.big, percent, x0 + 60, y + 14, cw - 60, 32, st.fg, PD_LEFT);
		pd_text(cr, st.font, f->detail, x0 + 60, y + 46, cw - 60, 22, st.dim, PD_LEFT);
	} else {
		pd_glyph_power(cr, x0 + 6, y + 26, 32, st.fg);
		pd_text(cr, st.bold, "No battery", x0 + 60, y + 18, cw - 60, 24, st.fg, PD_LEFT);
		pd_text(cr, st.font, "Running on external power", x0 + 60, y + 42, cw - 60, 22,
			st.dim, PD_LEFT);
	}
	y += POWER_HEADER;

	if (f->backlight) {
		draw_line(cr, &st, p, y);
		int value = f->dragging ? f->drag_value : f->brightness;
		ti_brightness(p->panel, cr, x0, y + 18, 22, st.fg);
		struct pbox slider = { x0 + 38, y + 10, cw - 38 - 46, 36 };
		draw_slider(cr, &st, slider, value);
		psurface_add_hotspot(p->surface, slider.x, slider.y, slider.width, slider.height, NULL,
			POWER_HS_BRIGHTNESS, 0, NULL);
		draw_percent(cr, &st, x0 + cw - 40, y + 10, 36, value);
		y += POWER_BRIGHTNESS;
	}

	if (f->profiles) {
		draw_line(cr, &st, p, y);
		pd_text(cr, st.font, "Power mode", x0, y + 6, cw, 22, st.dim, PD_LEFT);
		int count = 0;
		for (int i = 0; i < 3; i++) {
			count += (f->profiles >> i) & 1;
		}
		int bw = (cw - (count - 1) * 6) / count, bx = x0;
		for (int i = 0; i < 3; i++) {
			if (!(f->profiles & (1 << i))) {
				continue;
			}
			struct pbox button = { bx, y + 34, bw, 32 };
			draw_button(cr, &st, button, profile_titles[i], f->profile == i,
				hovered(&f->base, button));
			psurface_add_hotspot(p->surface, button.x, button.y, button.width, button.height,
				NULL, POWER_HS_PROFILE, i, NULL);
			bx += bw + 6;
		}
		y += POWER_MODES;
	}

	if (f->base.settings) {
		draw_line(cr, &st, p, y);
		struct pbox link = { x0, y + 1, cw, FOOTER - 1 };
		draw_link(cr, &st, &f->base, link, "Power settings", PD_LEFT);
		psurface_add_hotspot(p->surface, link.x, link.y, cw / 2, link.height, NULL,
			POWER_HS_SETTINGS, 0, NULL);
	}
}

static void power_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct power_flyout *f = p->data;
	if (button != BTN_LEFT) {
		return;
	}
	if (!pressed) {
		if (f->dragging) {
			power_apply_drag(f, true);
			f->dragging = false;
			popup_set_dirty(p);
		}
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	if (hs->kind == POWER_HS_BRIGHTNESS) {
		f->dragging = true;
		f->drag_box = hs->box;
		f->drag_value = slider_value(hs->box, x);
		power_apply_drag(f, false);
		popup_set_dirty(p);
	} else if (hs->kind == POWER_HS_PROFILE && hs->id >= 0 && hs->id < 3) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "powerprofilesctl set %s", profile_names[hs->id]);
		proc_spawn(cmd);
		f->profile = (int)hs->id;
		popup_set_dirty(p);
	} else if (hs->kind == POWER_HS_SETTINGS) {
		run_settings(&f->base);
	}
}

static void power_motion(struct popup *p, double x, double y) {
	struct power_flyout *f = p->data;
	flyout_motion(p, x, y);
	if (f->dragging) {
		f->drag_value = slider_value(f->drag_box, x);
		power_apply_drag(f, false);
	}
}

static void power_axis(struct popup *p, double x, double y, int direction) {
	struct power_flyout *f = p->data;
	if (!f->backlight || f->dragging) {
		return;
	}
	int value = f->brightness + (direction < 0 ? 5 : -5);
	f->drag_value = value < 1 ? 1 : value > 100 ? 100 : value;
	power_apply_drag(f, false);
	popup_set_dirty(p);
}

static void power_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void power_destroy(struct popup *p) {
	struct power_flyout *f = p->data;
	if (power_current == f) {
		power_current = NULL;
	}
	if (f->drag_timer) {
		loop_remove_timer(p->panel->loop, f->drag_timer);
	}
	if (f->tick) {
		loop_remove_timer(p->panel->loop, f->tick);
	}
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable power_vtable = {
	.render = power_render,
	.motion = power_motion,
	.leave = flyout_leave,
	.button = power_button,
	.axis = power_axis,
	.key = power_key,
	.destroy = power_destroy,
};

void flyout_power_toggle(struct panel *panel, struct popup_anchor anchor, const char *settings) {
	if (popup_is_open(panel, POPUP_POWER)) {
		popup_close_all(panel);
		return;
	}
	struct power_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = settings ? strdup(settings) : NULL;
	power_read(f);
	if (!flyout_open(&f->base, POPUP_POWER, power_height(f), &power_vtable, f)) {
		free(f->base.settings);
		free(f);
		return;
	}
	power_current = f;
	f->tick = loop_add_timer(panel->loop, 10000, power_tick, NULL);
	power_query_profiles(f);
}

/* ================= cpu ================= */

/*
 * CPU flyout: usage of the last minute, the cores, load, uptime and the
 * processes using the most CPU. Everything is read from /proc and /sys once a
 * second while the flyout is open.
 */

#define CPU_HISTORY_LEN 60
#define CPU_MAX_CORES 256
#define CPU_TOP 5
#define CPU_HEADER 66
#define CPU_GRAPH 112
#define CPU_CORE_ROW 20
#define CPU_INFO 60
#define CPU_PROC_ROW 24

enum cpu_hotspot {
	CPU_HS_TASK_MANAGER = 1,
};

struct cpu_ticks {
	unsigned long long total, idle;
};

struct proc_sample {
	int pid;
	unsigned long long ticks;
};

struct proc_usage {
	char name[64];
	double percent;
	int count;
};

struct cpu_flyout {
	struct flyout base;
	char model[128];
	int cores;
	struct cpu_ticks total;
	struct cpu_ticks core_ticks[CPU_MAX_CORES];
	int usage; // percent of all cores together
	int core_usage[CPU_MAX_CORES];
	int history[CPU_HISTORY_LEN];
	int history_len;
	double mhz;
	int temp; // degrees Celsius, -1 if unknown
	double load[3];
	int processes, threads;
	long uptime;
	struct proc_sample *samples; // sorted by pid
	int sample_count;
	struct proc_usage top[CPU_TOP];
	int top_count;
	struct loop_timer *tick;
};

static struct cpu_flyout *cpu_current = NULL;

static void cpu_read_stat(struct cpu_flyout *f, unsigned long long *total_delta) {
	*total_delta = 0;
	FILE *file = fopen("/proc/stat", "r");
	if (!file) {
		return;
	}
	char line[512];
	int core = 0;
	while (fgets(line, sizeof(line), file) && strncmp(line, "cpu", 3) == 0) {
		char name[16];
		unsigned long long v[8] = { 0 };
		if (sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", name, &v[0], &v[1],
				&v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 5) {
			continue;
		}
		unsigned long long idle = v[3] + v[4], total = 0;
		for (int i = 0; i < 8; i++) {
			total += v[i];
		}
		bool all = strcmp(name, "cpu") == 0;
		if (!all && core >= CPU_MAX_CORES) {
			continue;
		}
		struct cpu_ticks *prev = all ? &f->total : &f->core_ticks[core];
		int *usage = all ? &f->usage : &f->core_usage[core];
		if (!all) {
			core++;
		}
		if (prev->total && total > prev->total) {
			unsigned long long dt = total - prev->total;
			unsigned long long di = idle >= prev->idle ? idle - prev->idle : 0;
			*usage = (int)((dt - (di > dt ? dt : di)) * 100 / dt);
			if (all) {
				*total_delta = dt;
			}
		}
		prev->total = total;
		prev->idle = idle;
	}
	fclose(file);
	f->cores = core;
}

static void cpu_tidy_model(char *model);

static void cpu_read_info(struct cpu_flyout *f) {
	FILE *file = fopen("/proc/cpuinfo", "r");
	double sum = 0;
	int count = 0;
	if (file) {
		char line[512];
		while (fgets(line, sizeof(line), file)) {
			char *colon = strchr(line, ':');
			if (!colon) {
				continue;
			}
			char *value = colon + 1;
			while (*value == ' ' || *value == '\t') {
				value++;
			}
			value[strcspn(value, "\n")] = '\0';
			if (!f->model[0] && (strncmp(line, "model name", 10) == 0 ||
					strncmp(line, "Hardware", 8) == 0)) {
				snprintf(f->model, sizeof(f->model), "%s", value);
				cpu_tidy_model(f->model);
			} else if (strncmp(line, "cpu MHz", 7) == 0) {
				sum += atof(value);
				count++;
			}
		}
		fclose(file);
	}
	if (count > 0) {
		f->mhz = sum / count;
	} else {
		char buf[64];
		f->mhz = read_sys("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", buf,
			sizeof(buf)) ? atol(buf) / 1000.0 : 0;
	}
}

/* "Intel(R) Core(TM) i7-8665U CPU @ 1.90GHz" -> "Intel Core i7-8665U" */
static void cpu_tidy_model(char *model) {
	static const char *const noise[] = { "(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor",
		" processor" };
	char *at = strstr(model, " @ ");
	if (at) {
		*at = '\0';
	}
	for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); i++) {
		char *hit;
		size_t len = strlen(noise[i]);
		while ((hit = strstr(model, noise[i]))) {
			memmove(hit, hit + len, strlen(hit + len) + 1);
		}
	}
	// collapse double spaces left behind
	char *out = model;
	for (char *in = model; *in; in++) {
		if (*in == ' ' && (out == model || out[-1] == ' ')) {
			continue;
		}
		*out++ = *in;
	}
	while (out > model && out[-1] == ' ') {
		out--;
	}
	*out = '\0';
}

static int cpu_read_temp(void) {
	static const char *const sensors[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal",
		"soc_thermal", "acpitz" };
	int best = -1;
	size_t best_rank = sizeof(sensors) / sizeof(sensors[0]);
	DIR *dir = opendir("/sys/class/hwmon");
	struct dirent *de;
	while (dir && (de = readdir(dir))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[512], buf[64];
		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", de->d_name);
		if (!read_sys(path, buf, sizeof(buf))) {
			continue;
		}
		for (size_t i = 0; i < best_rank; i++) {
			if (strcmp(buf, sensors[i]) == 0) {
				snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", de->d_name);
				if (read_sys(path, buf, sizeof(buf))) {
					best = atoi(buf);
					best_rank = i;
				}
				break;
			}
		}
	}
	if (dir) {
		closedir(dir);
	}
	if (best < 0) {
		char buf[64];
		if (read_sys("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf))) {
			best = atoi(buf);
		}
	}
	return best > 0 ? best / 1000 : -1;
}

static int sample_cmp(const void *a, const void *b) {
	const struct proc_sample *sa = a, *sb = b;
	return (sa->pid > sb->pid) - (sa->pid < sb->pid);
}

static int usage_cmp(const void *a, const void *b) {
	const struct proc_usage *ua = a, *ub = b;
	return (ua->percent < ub->percent) - (ua->percent > ub->percent);
}

static void cpu_read_processes(struct cpu_flyout *f, unsigned long long total_delta) {
	DIR *dir = opendir("/proc");
	if (!dir) {
		return;
	}
	struct proc_sample *samples = NULL;
	int count = 0, cap = 0;
	struct proc_usage *groups = NULL;
	int group_count = 0, group_cap = 0;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (!isdigit((unsigned char)de->d_name[0])) {
			continue;
		}
		char path[300], buf[1024];
		snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
		FILE *file = fopen(path, "r");
		if (!file) {
			continue;
		}
		size_t n = fread(buf, 1, sizeof(buf) - 1, file);
		fclose(file);
		buf[n] = '\0';
		char *open = strchr(buf, '('), *close = strrchr(buf, ')');
		if (!open || !close || close < open || !close[1]) {
			continue;
		}
		// after the name: state is field 3, utime 14 and stime 15
		unsigned long long ticks = 0;
		int field = 3;
		char *save = NULL;
		for (char *tok = strtok_r(close + 2, " ", &save); tok && field <= 15;
				tok = strtok_r(NULL, " ", &save), field++) {
			if (field == 14 || field == 15) {
				ticks += strtoull(tok, NULL, 10);
			}
		}
		if (count == cap) {
			cap = cap ? cap * 2 : 256;
			samples = realloc(samples, cap * sizeof(*samples));
		}
		struct proc_sample sample = { atoi(de->d_name), ticks };
		samples[count++] = sample;
		if (!f->samples || total_delta == 0) {
			continue;
		}
		struct proc_sample *prev = bsearch(&sample, f->samples, f->sample_count,
			sizeof(sample), sample_cmp);
		if (!prev || ticks <= prev->ticks) {
			continue;
		}
		char name[64];
		size_t len = (size_t)(close - open - 1);
		if (len >= sizeof(name)) {
			len = sizeof(name) - 1;
		}
		memcpy(name, open + 1, len);
		name[len] = '\0';
		struct proc_usage *group = NULL;
		for (int i = 0; i < group_count; i++) {
			if (strcmp(groups[i].name, name) == 0) {
				group = &groups[i];
				break;
			}
		}
		if (!group) {
			if (group_count == group_cap) {
				group_cap = group_cap ? group_cap * 2 : 32;
				groups = realloc(groups, group_cap * sizeof(*groups));
			}
			group = &groups[group_count++];
			memset(group, 0, sizeof(*group));
			snprintf(group->name, sizeof(group->name), "%s", name);
		}
		group->percent += (double)(ticks - prev->ticks) * 100.0 / total_delta;
		group->count++;
	}
	closedir(dir);
	f->processes = count;
	if (samples) {
		qsort(samples, count, sizeof(*samples), sample_cmp);
	}
	free(f->samples);
	f->samples = samples;
	f->sample_count = count;
	if (total_delta > 0) {
		if (groups) {
			qsort(groups, group_count, sizeof(*groups), usage_cmp);
		}
		f->top_count = group_count < CPU_TOP ? group_count : CPU_TOP;
		for (int i = 0; i < f->top_count; i++) {
			f->top[i] = groups[i];
		}
	}
	free(groups);
}

static void cpu_sample(struct cpu_flyout *f) {
	unsigned long long delta;
	cpu_read_stat(f, &delta);
	cpu_read_info(f);
	f->temp = cpu_read_temp();
	FILE *file = fopen("/proc/loadavg", "r");
	if (file) {
		int running = 0;
		if (fscanf(file, "%lf %lf %lf %d/%d", &f->load[0], &f->load[1], &f->load[2],
				&running, &f->threads) < 5) {
			f->threads = 0;
		}
		fclose(file);
	}
	file = fopen("/proc/uptime", "r");
	if (file) {
		double up = 0;
		if (fscanf(file, "%lf", &up) == 1) {
			f->uptime = (long)up;
		}
		fclose(file);
	}
	cpu_read_processes(f, delta);
	if (delta > 0) {
		if (f->history_len == CPU_HISTORY_LEN) {
			memmove(f->history, f->history + 1, (CPU_HISTORY_LEN - 1) * sizeof(int));
			f->history_len--;
		}
		f->history[f->history_len++] = f->usage;
	}
}

static int cpu_core_rows(struct cpu_flyout *f, int *columns) {
	int n = f->cores > 0 ? f->cores : 1;
	*columns = n <= 4 ? n : n <= 16 ? 4 : 8;
	return (n + *columns - 1) / *columns;
}

static int cpu_height(struct cpu_flyout *f) {
	int columns;
	int rows = cpu_core_rows(f, &columns);
	return CPU_HEADER + CPU_GRAPH + (f->cores > 1 ? 30 + rows * CPU_CORE_ROW + 6 : 0) +
		CPU_INFO + 30 + CPU_TOP * CPU_PROC_ROW + 8 + FOOTER;
}

static void cpu_tick(void *data) {
	struct cpu_flyout *f = cpu_current;
	if (!f) {
		return;
	}
	cpu_sample(f);
	flyout_resize(&f->base, cpu_height(f));
	f->tick = loop_add_timer(f->base.panel->loop, 1000, cpu_tick, NULL);
}

static void cpu_bar(cairo_t *cr, const struct fly_style *st, double x, double y, double w,
		double h, int percent) {
	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	cairo_new_path(cr);
	pd_rounded(cr, x, y, w, h, st->style == PS_CLASSIC ? 0 : h / 2);
	pd_color(cr, st->track);
	cairo_fill(cr);
	if (percent > 0) {
		cairo_new_path(cr);
		pd_rounded(cr, x, y, w * percent / 100.0, h, st->style == PS_CLASSIC ? 0 : h / 2);
		pd_color(cr, st->accent);
		cairo_fill(cr);
	}
}

static void cpu_render(struct popup *p, cairo_t *cr) {
	struct cpu_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;
	char text[256];

	// header: total usage, model and details
	snprintf(text, sizeof(text), "%d%%", f->usage);
	pd_text(cr, st.big, text, x0, y + 12, 86, 36, st.fg, PD_LEFT);
	pd_text(cr, st.bold, f->model[0] ? f->model : "Processor", x0 + 90, y + 12, cw - 90, 20,
		st.fg, PD_LEFT);
	int len = snprintf(text, sizeof(text), "%d %s", f->cores, f->cores == 1 ? "core" : "cores");
	if (f->mhz > 0 && len < (int)sizeof(text)) {
		len += snprintf(text + len, sizeof(text) - len, " · %.1f GHz", f->mhz / 1000.0);
	}
	if (f->temp >= 0 && len < (int)sizeof(text)) {
		snprintf(text + len, sizeof(text) - len, " · %d°C", f->temp);
	}
	pd_text(cr, st.font, text, x0 + 90, y + 34, cw - 90, 20, st.dim, PD_LEFT);
	y += CPU_HEADER;

	// usage graph of the last minute
	struct pbox g = { x0, y, cw, CPU_GRAPH - 26 };
	cairo_new_path(cr);
	pd_rounded(cr, g.x, g.y, g.width, g.height, st.style == PS_CLASSIC ? 0 : 6);
	pd_color(cr, st.button_bg);
	cairo_fill(cr);
	for (int i = 1; i < 4; i++) {
		pd_rect(cr, g.x, g.y + g.height * i / 4, g.width, 1, st.line);
	}
	if (f->history_len > 1) {
		double step = (double)g.width / (CPU_HISTORY_LEN - 1);
		double first_x = g.x + g.width - (f->history_len - 1) * step;
		cairo_new_path(cr);
		cairo_move_to(cr, first_x, g.y + g.height);
		for (int i = 0; i < f->history_len; i++) {
			cairo_line_to(cr, first_x + i * step, g.y + g.height - g.height * f->history[i] / 100.0);
		}
		cairo_line_to(cr, g.x + g.width, g.y + g.height);
		cairo_close_path(cr);
		pd_color(cr, (st.accent & 0xffffff00) | 0x40);
		cairo_fill(cr);
		cairo_new_path(cr);
		for (int i = 0; i < f->history_len; i++) {
			double px = first_x + i * step;
			double py = g.y + g.height - g.height * f->history[i] / 100.0;
			if (i == 0) {
				cairo_move_to(cr, px, py);
			} else {
				cairo_line_to(cr, px, py);
			}
		}
		pd_color(cr, st.accent);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
	}
	pd_text(cr, st.font, "60 seconds", x0, g.y + g.height + 2, cw, 22, st.dim, PD_LEFT);
	pd_text(cr, st.font, "100%", x0, g.y + g.height + 2, cw, 22, st.dim, PD_RIGHT);
	y += CPU_GRAPH;

	// cores
	if (f->cores > 1) {
		draw_line(cr, &st, p, y);
		pd_text(cr, st.font, "Cores", x0, y + 6, cw, 20, st.dim, PD_LEFT);
		y += 30;
		int columns;
		int rows = cpu_core_rows(f, &columns);
		int cell = (cw - (columns - 1) * 10) / columns;
		for (int i = 0; i < f->cores && i < CPU_MAX_CORES; i++) {
			int cx = x0 + (i % columns) * (cell + 10), cy = y + (i / columns) * CPU_CORE_ROW;
			snprintf(text, sizeof(text), "%d", i);
			pd_text(cr, st.font, text, cx, cy, 20, CPU_CORE_ROW - 4, st.dim, PD_LEFT);
			cpu_bar(cr, &st, cx + 22, cy + (CPU_CORE_ROW - 4) / 2.0 - 3, cell - 22, 6,
				f->core_usage[i]);
		}
		y += rows * CPU_CORE_ROW + 6;
	}

	// load, uptime, processes
	draw_line(cr, &st, p, y);
	// column widths fit "0.61 0.81 0.72", "3d 10h 14m" and "221"
	int cols_x[3] = { 0, cw * 43 / 100, cw * 72 / 100 };
	int cols_w[3] = { cols_x[1] - 6, cols_x[2] - cols_x[1] - 6, cw - cols_x[2] };
	const char *labels[3] = { "Load", "Up time", "Processes" };
	char values[3][64];
	snprintf(values[0], sizeof(values[0]), "%.2f %.2f %.2f", f->load[0], f->load[1], f->load[2]);
	long days = f->uptime / 86400, hours = f->uptime / 3600 % 24, minutes = f->uptime / 60 % 60;
	if (days > 0) {
		snprintf(values[1], sizeof(values[1]), "%ldd %ldh %ldm", days, hours, minutes);
	} else {
		snprintf(values[1], sizeof(values[1]), "%ldh %ldm", hours, minutes);
	}
	snprintf(values[2], sizeof(values[2]), "%d", f->processes);
	for (int i = 0; i < 3; i++) {
		pd_text(cr, st.font, labels[i], x0 + cols_x[i], y + 8, cols_w[i], 20, st.dim, PD_LEFT);
		pd_text(cr, st.bold, values[i], x0 + cols_x[i], y + 30, cols_w[i], 22, st.fg, PD_LEFT);
	}
	y += CPU_INFO;

	// processes using the most CPU
	draw_line(cr, &st, p, y);
	pd_text(cr, st.font, "Most CPU", x0, y + 6, cw, 20, st.dim, PD_LEFT);
	pd_text(cr, st.font, "CPU", x0, y + 6, cw, 20, st.dim, PD_RIGHT);
	y += 30;
	if (f->top_count == 0) {
		pd_text(cr, st.font, f->history_len ? "Nothing is using the CPU" : "Measuring...", x0, y,
			cw, CPU_PROC_ROW, st.dim, PD_LEFT);
	}
	for (int i = 0; i < f->top_count; i++) {
		struct proc_usage *u = &f->top[i];
		double share = u->percent > 100 ? 1 : u->percent / 100.0;
		if (share > 0) {
			cairo_new_path(cr);
			pd_rounded(cr, x0 - 4, y + 2, (cw + 8) * share, CPU_PROC_ROW - 4,
				st.style == PS_CLASSIC ? 0 : 4);
			pd_color(cr, (st.accent & 0xffffff00) | 0x30);
			cairo_fill(cr);
		}
		if (u->count > 1) {
			snprintf(text, sizeof(text), "%s (%d)", u->name, u->count);
		} else {
			snprintf(text, sizeof(text), "%s", u->name);
		}
		pd_text(cr, st.font, text, x0, y, cw - 70, CPU_PROC_ROW, st.fg, PD_LEFT);
		snprintf(text, sizeof(text), "%.1f%%", u->percent);
		pd_text(cr, st.font, text, x0, y, cw, CPU_PROC_ROW, st.fg, PD_RIGHT);
		y += CPU_PROC_ROW;
	}
	y = M + cpu_height(f) - FOOTER;

	draw_line(cr, &st, p, y);
	struct pbox link = { x0, y + 1, cw, FOOTER - 1 };
	draw_link(cr, &st, &f->base, link, "Open Task Manager", PD_LEFT);
	psurface_add_hotspot(p->surface, link.x, link.y, cw / 2, link.height, NULL,
		CPU_HS_TASK_MANAGER, 0, NULL);
}

static void cpu_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct cpu_flyout *f = p->data;
	if (button != BTN_LEFT || pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == CPU_HS_TASK_MANAGER) {
		run_settings(&f->base);
	}
}

static void cpu_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void cpu_destroy(struct popup *p) {
	struct cpu_flyout *f = p->data;
	if (cpu_current == f) {
		cpu_current = NULL;
	}
	if (f->tick) {
		loop_remove_timer(p->panel->loop, f->tick);
	}
	free(f->samples);
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable cpu_vtable = {
	.render = cpu_render,
	.motion = flyout_motion,
	.leave = flyout_leave,
	.button = cpu_button,
	.key = cpu_key,
	.destroy = cpu_destroy,
};

/* btop, htop or top in the taskbar's terminal. */
static char *default_task_manager(struct panel *panel) {
	const char *terminal = panel->config && panel->config->terminal ?
		panel->config->terminal : "xfce4-terminal -x";
	static const char *const tools[] = { "btop", "htop" };
	const char *path = getenv("PATH");
	for (size_t i = 0; path && i < sizeof(tools) / sizeof(tools[0]); i++) {
		char *dirs = strdup(path);
		char *save = NULL;
		for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
			char *candidate = format_str("%s/%s", dir, tools[i]);
			bool found = access(candidate, X_OK) == 0;
			free(candidate);
			if (found) {
				free(dirs);
				return format_str("exec %s %s", terminal, tools[i]);
			}
		}
		free(dirs);
	}
	return format_str("exec %s top", terminal);
}

void flyout_cpu_toggle(struct panel *panel, struct popup_anchor anchor, const char *task_manager) {
	if (popup_is_open(panel, POPUP_CPU)) {
		popup_close_all(panel);
		return;
	}
	struct cpu_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = task_manager ? strdup(task_manager) : default_task_manager(panel);
	cpu_sample(f);
	if (!flyout_open(&f->base, POPUP_CPU, cpu_height(f), &cpu_vtable, f)) {
		free(f->samples);
		free(f->base.settings);
		free(f);
		return;
	}
	cpu_current = f;
	// the first measurement needs a second sample
	f->tick = loop_add_timer(panel->loop, 400, cpu_tick, NULL);
}

/* ================= bluetooth ================= */

#define BT_HEADER 60
#define BT_ROW 52
#define BT_ROWS 6
#define BT_MESSAGE 56
#define BT_STATUS 30

struct bt_row {
	char *path;
	struct pbox row, action, forget;
};

struct bt_flyout {
	struct flyout base; // first member
	int scroll;
	bool discovery_started;
	struct pbox power_switch, link;
	struct bt_row rows[BT_ROWS];
	int row_count;
};

static struct bt_flyout *bt_current = NULL;

void draw_bluetooth_glyph(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_move_to(cr, x + s * 0.24, y + s * 0.3);
	cairo_line_to(cr, x + s * 0.72, y + s * 0.7);
	cairo_line_to(cr, x + s * 0.5, y + s * 0.92);
	cairo_line_to(cr, x + s * 0.5, y + s * 0.08);
	cairo_line_to(cr, x + s * 0.72, y + s * 0.3);
	cairo_line_to(cr, x + s * 0.24, y + s * 0.7);
	pd_color(cr, color);
	cairo_set_line_width(cr, s * 0.09);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* Unnamed nearby devices are only called by their address, e.g. "4C-87-5D-12-AB-01". */
static bool bt_named(const struct bt_device *d) {
	if (!d->name || !*d->name) {
		return false;
	}
	if (strlen(d->name) != 17) {
		return true;
	}
	for (int i = 0; i < 17; i++) {
		char c = d->name[i];
		if (i % 3 == 2 ? (c != '-' && c != ':') : !isxdigit((unsigned char)c)) {
			return true;
		}
	}
	return false;
}

static int bt_device_cmp(const void *a, const void *b) {
	const struct bt_device *x = *(struct bt_device *const *)a;
	const struct bt_device *y = *(struct bt_device *const *)b;
	if (x->connected != y->connected) {
		return x->connected ? -1 : 1;
	}
	if (x->paired != y->paired) {
		return x->paired ? -1 : 1;
	}
	return strcasecmp(x->name ? x->name : "", y->name ? y->name : "");
}

/* Paired and named nearby devices, connected ones first. */
static int bt_visible(struct bt_device **out, int max) {
	list_t *devices = bt_devices();
	int count = 0;
	for (int i = 0; devices && i < devices->length && count < max; i++) {
		struct bt_device *d = devices->items[i];
		if (d->paired || d->connected || bt_named(d)) {
			out[count++] = d;
		}
	}
	qsort(out, count, sizeof(*out), bt_device_cmp);
	return count;
}

static int bt_height(struct bt_flyout *f) {
	struct bt_device *devices[128];
	int count = bt_powered() ? bt_visible(devices, 128) : 0;
	int h = BT_HEADER + 1;
	h += count ? (count < BT_ROWS ? count : BT_ROWS) * BT_ROW + 8 : BT_MESSAGE;
	if (bt_status()) {
		h += BT_STATUS;
	}
	return h + FOOTER;
}

static void bt_rows_clear(struct bt_flyout *f) {
	for (int i = 0; i < f->row_count; i++) {
		free(f->rows[i].path);
	}
	f->row_count = 0;
}

static void bt_render(struct popup *p, cairo_t *cr) {
	struct bt_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	draw_bluetooth_glyph(cr, x0, y + 20, 22, st.fg);
	pd_text(cr, st.big, "Bluetooth", x0 + 32, y + 12, cw - 90, 36, st.fg, PD_LEFT);
	f->power_switch = (struct pbox){ 0 };
	if (bt_available()) {
		f->power_switch = (struct pbox){ x0 + cw - 48, y + 16, 48, 28 };
		draw_switch(cr, &st, x0 + cw - 40, y + 20, bt_powered());
	}
	y += BT_HEADER;
	draw_line(cr, &st, p, y);
	y += 1;

	bt_rows_clear(f);
	struct bt_device *devices[128];
	int count = bt_powered() ? bt_visible(devices, 128) : 0;
	if (f->scroll > count - BT_ROWS) {
		f->scroll = count - BT_ROWS;
	}
	if (f->scroll < 0) {
		f->scroll = 0;
	}
	if (count == 0) {
		const char *message = !bt_available() ? "Bluetooth isn't available" :
			!bt_powered() ? "Turn on Bluetooth to connect devices" :
			bt_discovering() ? "Searching for devices…" : "No devices found";
		pd_text(cr, st.font, message, x0, y, cw, BT_MESSAGE, st.dim, PD_LEFT);
		y += BT_MESSAGE;
	} else {
		y += 4;
		for (int i = f->scroll; i < count && f->row_count < BT_ROWS; i++) {
			struct bt_device *d = devices[i];
			struct bt_row *r = &f->rows[f->row_count++];
			r->path = strdup(d->path);
			r->row = (struct pbox){ M + 4, y, W - 2 * M - 8, BT_ROW };
			r->action = (struct pbox){ x0 + cw - 96, y + 11, 96, 30 };
			r->forget = (struct pbox){ 0 };
			bool hover = hovered(&f->base, r->row);
			if (hover) {
				fill_hover(cr, &st, r->row);
			}
			cairo_surface_t *icon = d->icon ? apps_icon(p->panel, d->icon, 24) : NULL;
			if (icon) {
				pd_icon(cr, icon, x0, y + 14, 24);
			} else {
				draw_bluetooth_glyph(cr, x0, y + 14, 24, st.fg);
			}
			int text_w = cw - 36 - 104 - (d->paired ? 28 : 0);
			pd_text(cr, st.bold, d->name ? d->name : d->address, x0 + 36, y + 7, text_w, 20,
				st.fg, PD_LEFT);
			pd_text(cr, st.font, d->connected ? "Connected" : d->paired ? "Paired" :
				"Not paired", x0 + 36, y + 26, text_w, 18, st.dim, PD_LEFT);
			if (d->paired && hover && !d->busy) {
				r->forget = (struct pbox){ r->action.x - 30, y + 14, 24, 24 };
				if (hovered(&f->base, r->forget)) {
					fill_hover(cr, &st, r->forget);
				}
				cairo_new_path(cr);
				double cx = r->forget.x + 12, cy = r->forget.y + 12;
				cairo_move_to(cr, cx - 4, cy - 4);
				cairo_line_to(cr, cx + 4, cy + 4);
				cairo_move_to(cr, cx + 4, cy - 4);
				cairo_line_to(cr, cx - 4, cy + 4);
				pd_color(cr, st.fg);
				cairo_set_line_width(cr, 1.3);
				cairo_stroke(cr);
			}
			const char *label = d->busy ? "…" : d->connected ? "Disconnect" :
				d->paired ? "Connect" : "Pair";
			draw_button(cr, &st, r->action, label, !d->paired && !d->busy,
				hovered(&f->base, r->action));
			y += BT_ROW;
		}
		y += 4;
	}
	if (bt_status()) {
		pd_text(cr, st.font, bt_status(), x0, y, cw, BT_STATUS, st.fg, PD_LEFT);
		y += BT_STATUS;
	}
	int fy = H - M - FOOTER;
	draw_line(cr, &st, p, fy);
	f->link = (struct pbox){ x0, fy + 1, cw, FOOTER - 1 };
	draw_link(cr, &st, &f->base, f->link, "Bluetooth settings", PD_LEFT);
}

static void bt_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct bt_flyout *f = p->data;
	if (!pressed || button != BTN_LEFT) {
		return;
	}
	if (f->power_switch.width && pbox_contains(&f->power_switch, x, y)) {
		bt_set_powered(!bt_powered());
		return;
	}
	if (pbox_contains(&f->link, x, y)) {
		run_settings(&f->base);
		return;
	}
	for (int i = 0; i < f->row_count; i++) {
		struct bt_row *r = &f->rows[i];
		if (!pbox_contains(&r->row, x, y)) {
			continue;
		}
		// the rows are rebuilt when BlueZ answers: keep a copy of the device path
		char *path = strdup(r->path);
		bool forget = r->forget.width && pbox_contains(&r->forget, x, y);
		bool action = pbox_contains(&r->action, x, y);
		struct bt_device *d = NULL;
		for (int k = 0; bt_devices() && k < bt_devices()->length; k++) {
			struct bt_device *candidate = bt_devices()->items[k];
			if (strcmp(candidate->path, path) == 0) {
				d = candidate;
			}
		}
		if (d && forget) {
			bt_remove(path);
		} else if (d && action) {
			if (d->connected) {
				bt_connect(path, false);
			} else if (d->paired) {
				bt_connect(path, true);
			} else {
				bt_pair(path);
			}
		}
		free(path);
		return;
	}
}

static void bt_axis(struct popup *p, double x, double y, int direction) {
	struct bt_flyout *f = p->data;
	f->scroll += direction < 0 ? -1 : 1;
	popup_set_dirty(p);
}

static void bt_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void bt_destroy(struct popup *p) {
	struct bt_flyout *f = p->data;
	if (f->discovery_started && bt_discovering()) {
		bt_set_discovery(false);
	}
	bt_rows_clear(f);
	if (bt_current == f) {
		bt_current = NULL;
	}
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable bt_vtable = {
	.render = bt_render,
	.motion = flyout_motion,
	.leave = flyout_leave,
	.button = bt_button,
	.axis = bt_axis,
	.key = bt_key,
	.destroy = bt_destroy,
};

void flyout_bluetooth_changed(struct panel *panel) {
	struct bt_flyout *f = bt_current;
	if (!f || f->base.panel != panel) {
		return;
	}
	// look for nearby devices while it is open
	if (bt_powered() && !f->discovery_started) {
		f->discovery_started = true;
		bt_set_discovery(true);
	} else if (!bt_powered()) {
		f->discovery_started = false;
	}
	flyout_resize(&f->base, bt_height(f));
}

void flyout_bluetooth_toggle(struct panel *panel, struct popup_anchor anchor) {
	if (popup_is_open(panel, POPUP_BLUETOOTH)) {
		popup_close_all(panel);
		return;
	}
	struct bt_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = strdup("exec tilewin-settings --page bluetooth");
	if (!flyout_open(&f->base, POPUP_BLUETOOTH, bt_height(f), &bt_vtable, f)) {
		free(f->base.settings);
		free(f);
		return;
	}
	bt_current = f;
	flyout_bluetooth_changed(panel);
}
