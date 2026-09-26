#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <json.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <time.h>
#include "draw.h"
#include "flyout.h"
#include "popup.h"
#include "textfield.h"
#include "stringop.h"
#include "wifi_caps.h"

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
#define NET_SECRET 26 // the line showing the password of a saved network
#define NET_MESSAGE 56
#define NET_DETAILS 46

enum {
	NET_HS_TOGGLE = 1,
	NET_HS_ROW,
	NET_HS_CONNECT,
	NET_HS_DISCONNECT,
	NET_HS_CANCEL,
	NET_HS_SETTINGS,
	NET_HS_REFRESH,
	NET_HS_COPY,
	NET_HS_SECRET,      // show the password of a saved network
	NET_HS_COPY_SECRET,
};

struct wifi_net {
	char *ssid;
	char *security;
	char *uuid; // connection of a saved network, whose name may differ from the SSID
	int signal;
	bool active, known;
	int bands;         // bit per enum wifi_band it is seen on
	int band;          // the band shown: the one connected on, else the fastest
	double rate;       // Mbit/s this PC and that access point reach together at most
	struct wifi_link_caps pair; // how: the standard, streams and width both have
	bool pair_known;   // from the capabilities of both, not only the access point's
	bool active_bss;   // band and rate are those of the access point connected to
};

/* A saved connection of NetworkManager and the SSID it is for. */
struct saved_net {
	char *uuid;
	char *ssid;
};

struct net_flyout {
	struct flyout base; // first member
	list_t *nets; // struct wifi_net *
	char *wifi_device;
	char *wired_connection;
	bool wired;
	bool have_nmcli, wifi_enabled, loaded, rescanned;
	char *expanded; // SSID of the expanded row
	char *secret_ssid; // saved network whose password was asked for
	char secret[128];  // its password, empty while NetworkManager is asked
	bool asking_password;
	char password[128];
	struct text_cursor password_cursor;
	char status[256];
	bool status_error;
	int scroll;
	struct loop_timer *timer;
	// the interface of the default route: addresses and current usage
	char iface[IFNAMSIZ];
	char ipv4[INET_ADDRSTRLEN], ipv6[INET6_ADDRSTRLEN];
	uint64_t rx_bytes, tx_bytes;
	struct timespec sampled;
	double rx_rate, tx_rate; // bytes per second
	bool have_rate;
	struct loop_timer *stats_timer;
	char copied[INET6_ADDRSTRLEN]; // the address just copied, shown for a moment
	struct timespec copied_at;
};

static struct net_flyout *net_current = NULL;

static void wifi_net_free(struct wifi_net *n) {
	free(n->ssid);
	free(n->security);
	free(n->uuid);
	free(n);
}

static void saved_net_free(struct saved_net *s) {
	free(s->uuid);
	free(s->ssid);
	free(s);
}

/* Whether the password of a saved network is shown right now. */
static bool net_secret_shown(struct net_flyout *f) {
	return f->secret_ssid && f->expanded && strcmp(f->secret_ssid, f->expanded) == 0;
}

/* Forgets a password that was shown, so it is not kept around. */
static void net_forget_secret(struct net_flyout *f) {
	free(f->secret_ssid);
	f->secret_ssid = NULL;
	memset(f->secret, 0, sizeof(f->secret));
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
	int h = NET_HEADER + 1 + (f->iface[0] ? NET_DETAILS + 1 : 0);
	if (net_list_shown(f)) {
		h += net_visible_rows(f) * NET_ROW + 8 + (f->expanded ? NET_EXPAND : 0) +
			(net_secret_shown(f) ? NET_SECRET : 0);
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

/*
 * One access point of a network: the band it is on, and the most this PC and
 * it reach together, from the capabilities of both (the older standard, the
 * fewer streams, the narrower of its channel now and what both support). Not
 * in the kernel's scan list, the access point's own rate from nmcli is taken,
 * as far as this PC's adapter goes. The network shows the one connected to,
 * else the fastest.
 */
static void net_add_bss(struct wifi_net *n, bool active, const char *bssid, int freq,
		int ap_rate, int width, const struct wifi_adapter *adapter, const struct wifi_ap *aps,
		int ap_count) {
	if (freq <= 0) {
		return;
	}
	enum wifi_band band = wifi_band_of(freq);
	n->bands |= 1 << band;
	uint8_t mac[6];
	const struct wifi_ap *ap = NULL;
	if (wifi_parse_bssid(bssid, mac)) {
		for (int i = 0; i < ap_count && !ap; i++) {
			ap = memcmp(aps[i].bssid, mac, 6) == 0 ? &aps[i] : NULL;
		}
	}
	struct wifi_link_caps pair = { 0 };
	double rate = 0;
	bool known = false;
	if (adapter && ap) {
		rate = wifi_pair_rate(adapter, ap, width, &pair);
		known = rate > 0;
	}
	if (!known && ap_rate > 0) {
		rate = ap_rate;
		const struct wifi_link_caps *mine = adapter ? &adapter->band[band] : NULL;
		if (mine && mine->gen) {
			int w = width > 0 && width < mine->width ? width : mine->width;
			double best = wifi_phy_rate(mine->gen, mine->streams, w);
			rate = rate < best ? rate : best;
		}
	}
	if (rate <= 0 || (n->active_bss && !active)) {
		n->bands |= 1 << band;
		return;
	}
	if (active || rate > n->rate) {
		n->band = band;
		n->rate = rate;
		n->pair = pair;
		n->pair_known = known;
		n->active_bss = active;
	}
}

/* "5 GHz", or the bands a network is seen on: "2.4 · 5 GHz". */
static void net_band_text(const struct wifi_net *n, char *out, size_t size) {
	static const char *const names[WIFI_BANDS] = { "2.4", "5", "6" };
	if (n->active_bss || !(n->bands & (n->bands - 1))) {
		int band = n->active_bss ? n->band : __builtin_ctz(n->bands ? n->bands : 1);
		snprintf(out, size, "%s GHz", names[band]);
		return;
	}
	out[0] = '\0';
	for (int b = 0; b < WIFI_BANDS; b++) {
		if (n->bands & (1 << b)) {
			size_t len = strlen(out);
			snprintf(out + len, size - len, "%s%s", len ? " \u00b7 " : "", names[b]);
		}
	}
	size_t len = strlen(out);
	snprintf(out + len, size - len, " GHz");
}

static void net_rate_text(double mbit, char *out, size_t size) {
	if (mbit >= 1000) {
		snprintf(out, size, "up to %.1f Gbit/s", mbit / 1000);
	} else {
		snprintf(out, size, "up to %.0f Mbit/s", mbit);
	}
}

static void net_query_done(void *data, const char *output) {
	struct net_flyout *f = net_current;
	if (!f) {
		return;
	}
	net_clear(f);
	f->have_nmcli = !strstr(output, "--missing");
	struct wifi_adapter adapter;
	struct wifi_ap *aps = NULL;
	int ap_count = 0;
	bool asked_caps = false, have_adapter = false;
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
		if (section == S_KNOWN) {
			// "<uuid>:<ssid>", written by the query itself, so it is not escaped
			char *colon = strchr(line, ':');
			if (colon && colon[1]) {
				struct saved_net *s = calloc(1, sizeof(*s));
				s->uuid = strndup(line, colon - line);
				s->ssid = strdup(colon + 1);
				list_add(known, s);
			}
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
			if (!asked_caps && f->wifi_device) {
				// what this PC's adapter and the access points around can do, once a query
				asked_caps = true;
				have_adapter = wifi_adapter_caps(f->wifi_device, &adapter);
				ap_count = have_adapter ? wifi_scan_aps(f->wifi_device, &aps) : 0;
			}
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
			if (fields->length >= 8) {
				net_add_bss(found, strcmp(v[0], "*") == 0, v[4], atoi(v[5]), atoi(v[6]), atoi(v[7]),
					have_adapter ? &adapter : NULL, aps, ap_count);
			}
		}
		list_free_items_and_destroy(fields);
	}
	free(copy);
	free(aps);
	for (int i = 0; i < f->nets->length; i++) {
		struct wifi_net *n = f->nets->items[i];
		for (int j = 0; j < known->length && !n->known; j++) {
			struct saved_net *s = known->items[j];
			if (strcmp(s->ssid, n->ssid) == 0) {
				n->known = true;
				n->uuid = strdup(s->uuid);
			}
		}
	}
	for (int i = 0; i < known->length; i++) {
		saved_net_free(known->items[i]);
	}
	list_free(known);
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
		"nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID,BSSID,FREQ,RATE,BANDWIDTH device wifi list "
		"--rescan %s 2>/dev/null; "
		// the name of a connection need not be the SSID, so ask every Wi-Fi
		// connection for the network it is for and remember its uuid
		"echo --known; nmcli -t -f UUID,TYPE connection show 2>/dev/null | "
		"while IFS=: read -r uuid type; do "
		"[ \"$type\" = 802-11-wireless ] || continue; "
		"ssid=$(nmcli -g 802-11-wireless.ssid connection show uuid \"$uuid\" 2>/dev/null); "
		"[ -n \"$ssid\" ] && printf '%%s:%%s\\n' \"$uuid\" \"$ssid\"; done",
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

static void net_secret_done(void *data, const char *output) {
	struct net_flyout *f = net_current;
	if (!f || !f->secret_ssid) {
		return;
	}
	char *line = strdup(output);
	line[strcspn(line, "\n")] = '\0';
	if (strncmp(line, "Error", 5) == 0 || !*line) {
		snprintf(f->status, sizeof(f->status), "%s", *line ?
			"NetworkManager did not give the password" :
			"No saved password, or it may not be read");
		f->status_error = true;
		net_forget_secret(f);
	} else {
		snprintf(f->secret, sizeof(f->secret), "%s", line);
		f->status[0] = '\0';
		f->status_error = false;
	}
	memset(line, 0, strlen(line));
	free(line);
	net_update(f);
}

/*
 * The password of a saved network, from NetworkManager. The connection is
 * addressed by its uuid, because its name need not be the SSID. WPA keeps the
 * password in the psk, older networks in a WEP key and 802.1X ones in the
 * password of the user. Reading a secret needs authorization, so polkit may ask
 * for the password of the computer first.
 */
static void net_show_secret(struct net_flyout *f, int index) {
	if (index < 0 || index >= f->nets->length) {
		return;
	}
	struct wifi_net *n = f->nets->items[index];
	if (!n->uuid) {
		return;
	}
	if (f->secret_ssid && strcmp(f->secret_ssid, n->ssid) == 0) {
		net_forget_secret(f); // clicking again hides it
		net_update(f);
		return;
	}
	net_forget_secret(f);
	f->secret_ssid = strdup(n->ssid);
	f->status[0] = '\0';
	char *uuid = shell_quote(n->uuid);
	char *cmd = format_str(
		"for p in 802-11-wireless-security.psk 802-11-wireless-security.wep-key0 "
		"802-1x.password; do "
		"v=$(nmcli -s -g \"$p\" connection show uuid %s 2>/dev/null); "
		"if [ -n \"$v\" ]; then printf '%%s\\n' \"$v\"; exit 0; fi; done; "
		// nothing came back: run it once more to show why
		"nmcli -s -g 802-11-wireless-security.psk connection show uuid %s 2>&1 | head -n 1",
		uuid, uuid);
	proc_run(f->base.panel, cmd, false, NULL, net_secret_done, NULL);
	free(cmd);
	free(uuid);
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

/* The interface of the default route with the lowest metric. */
static bool default_route_iface(char *out, size_t size) {
	FILE *file = fopen("/proc/net/route", "r");
	if (!file) {
		return false;
	}
	char line[256];
	int best_metric = -1;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return false; // just the header, or nothing
	}
	while (fgets(line, sizeof(line), file)) {
		char name[IFNAMSIZ + 1], dest[16];
		unsigned flags;
		int metric;
		if (sscanf(line, "%16s %15s %*s %x %*d %*d %d", name, dest, &flags, &metric) != 4) {
			continue;
		}
		if (strcmp(dest, "00000000") == 0 && (flags & 1) && strcmp(name, "lo") != 0 &&
				(best_metric < 0 || metric < best_metric)) {
			best_metric = metric;
			snprintf(out, size, "%s", name);
		}
	}
	fclose(file);
	return best_metric >= 0;
}

/* The interface of the IPv6 default route (some networks only route IPv6 by default). */
static bool default_route6_iface(char *out, size_t size) {
	FILE *file = fopen("/proc/net/ipv6_route", "r");
	if (!file) {
		return false;
	}
	char line[256];
	bool found = false;
	while (!found && fgets(line, sizeof(line), file)) {
		char dest[40], name[IFNAMSIZ + 1];
		unsigned prefix;
		if (sscanf(line, "%39s %x %*s %*s %*s %*s %*s %*s %*s %16s", dest, &prefix, name) == 3 &&
				prefix == 0 && strspn(dest, "0") == strlen(dest) && strcmp(name, "lo") != 0) {
			snprintf(out, size, "%s", name);
			found = true;
		}
	}
	fclose(file);
	return found;
}

/* Any interface that is up, isn't the loopback and has an address. */
static bool first_up_iface(char *out, size_t size) {
	struct ifaddrs *addrs = NULL;
	if (getifaddrs(&addrs) != 0) {
		return false;
	}
	bool found = false;
	for (struct ifaddrs *a = addrs; a && !found; a = a->ifa_next) {
		if (a->ifa_addr && a->ifa_name && (a->ifa_flags & IFF_UP) &&
				!(a->ifa_flags & IFF_LOOPBACK) && (a->ifa_addr->sa_family == AF_INET ||
				a->ifa_addr->sa_family == AF_INET6)) {
			snprintf(out, size, "%s", a->ifa_name);
			found = true;
		}
	}
	freeifaddrs(addrs);
	return found;
}

static uint64_t read_counter(const char *iface, const char *name) {
	char path[256], buf[32];
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", iface, name);
	return read_sys(path, buf, sizeof(buf)) ? strtoull(buf, NULL, 10) : 0;
}

static void net_stats_sample(struct net_flyout *f) {
	char iface[IFNAMSIZ] = "";
	if (!default_route_iface(iface, sizeof(iface)) &&
			!default_route6_iface(iface, sizeof(iface))) {
		if (f->wifi_device) {
			snprintf(iface, sizeof(iface), "%s", f->wifi_device);
		} else {
			first_up_iface(iface, sizeof(iface));
		}
	}
	if (strcmp(iface, f->iface) != 0) {
		snprintf(f->iface, sizeof(f->iface), "%s", iface);
		f->have_rate = false;
		f->rx_bytes = f->tx_bytes = 0;
	}
	f->ipv4[0] = f->ipv6[0] = '\0';
	if (!f->iface[0]) {
		return;
	}
	struct ifaddrs *addrs = NULL;
	if (getifaddrs(&addrs) == 0) {
		bool link_local = false;
		for (struct ifaddrs *a = addrs; a; a = a->ifa_next) {
			if (!a->ifa_addr || !a->ifa_name || strcmp(a->ifa_name, f->iface) != 0) {
				continue;
			}
			if (a->ifa_addr->sa_family == AF_INET && !f->ipv4[0]) {
				struct sockaddr_in *in = (struct sockaddr_in *)a->ifa_addr;
				inet_ntop(AF_INET, &in->sin_addr, f->ipv4, sizeof(f->ipv4));
			} else if (a->ifa_addr->sa_family == AF_INET6) {
				struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)a->ifa_addr;
				bool local = IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr);
				// a global address wins over the link-local one
				if (!f->ipv6[0] || (link_local && !local)) {
					inet_ntop(AF_INET6, &in6->sin6_addr, f->ipv6, sizeof(f->ipv6));
					link_local = local;
				}
			}
		}
		freeifaddrs(addrs);
	}
	uint64_t rx = read_counter(f->iface, "rx_bytes");
	uint64_t tx = read_counter(f->iface, "tx_bytes");
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double dt = (now.tv_sec - f->sampled.tv_sec) + (now.tv_nsec - f->sampled.tv_nsec) / 1e9;
	if ((f->rx_bytes || f->tx_bytes) && dt > 0.2 && rx >= f->rx_bytes && tx >= f->tx_bytes) {
		f->rx_rate = (rx - f->rx_bytes) / dt;
		f->tx_rate = (tx - f->tx_bytes) / dt;
		f->have_rate = true;
	}
	f->rx_bytes = rx;
	f->tx_bytes = tx;
	f->sampled = now;
}

static void net_stats_tick(void *data) {
	struct net_flyout *f = net_current;
	if (!f) {
		return;
	}
	f->stats_timer = NULL;
	net_stats_sample(f);
	net_update(f);
	f->stats_timer = loop_add_timer(f->base.panel->loop, 1000, net_stats_tick, NULL);
}

static void format_rate(double bytes, char *out, size_t size) {
	static const char *const units[] = { "B/s", "kB/s", "MB/s", "GB/s" };
	int unit = 0;
	while (bytes >= 1000 && unit < 3) {
		bytes /= 1000;
		unit++;
	}
	snprintf(out, size, bytes < 10 && unit > 0 ? "%.1f %s" : "%.0f %s", bytes, units[unit]);
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

	if (f->iface[0]) {
		if (!f->ipv4[0] && !f->ipv6[0]) {
			pd_text(cr, st.font, "No IP address", x0, y + 4, cw, 20, st.fg, PD_LEFT);
		} else {
			// each address is a button that copies it
			const char *label = "IP address ";
			int tx = x0, lw = 0, sep_w = 0;
			pd_text_size(cr, st.font, label, &lw, NULL);
			pd_text_size(cr, st.font, "  ·  ", &sep_w, NULL);
			pd_text(cr, st.font, label, tx, y + 4, cw, 20, st.fg, PD_LEFT);
			tx += lw;
			const char *addresses[2] = { f->ipv4, f->ipv6 };
			bool first = true;
			for (int i = 0; i < 2; i++) {
				if (!addresses[i][0]) {
					continue;
				}
				if (!first) {
					pd_text(cr, st.font, "  ·  ", tx, y + 4, sep_w + 2, 20, st.dim, PD_LEFT);
					tx += sep_w;
				}
				first = false;
				int w = 0;
				pd_text_size(cr, st.font, addresses[i], &w, NULL);
				if (w > x0 + cw - tx) {
					w = x0 + cw - tx;
				}
				if (w < 12) {
					break;
				}
				struct pbox b = { tx - 3, y + 3, w + 6, 22 };
				bool hover = hovered(&f->base, b);
				if (hover) {
					fill_hover(cr, &st, b);
				}
				pd_text(cr, st.font, addresses[i], tx, y + 4, w, 20, hover ? st.accent : st.fg,
					PD_LEFT);
				psurface_add_hotspot(p->surface, b.x, b.y, b.width, b.height, NULL, NET_HS_COPY,
					i, NULL);
				tx += w;
			}
		}
		char down[24] = "…", up[24] = "…", usage[128];
		if (f->have_rate) {
			format_rate(f->rx_rate, down, sizeof(down));
			format_rate(f->tx_rate, up, sizeof(up));
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (f->copied[0] && now.tv_sec - f->copied_at.tv_sec < 2) {
			snprintf(usage, sizeof(usage), "Copied %s to the clipboard", f->copied);
		} else {
			snprintf(usage, sizeof(usage), "↓ %s    ↑ %s    %s", down, up, f->iface);
		}
		pd_text(cr, st.font, usage, x0, y + 23, cw, 20, st.dim, PD_LEFT);
		y += NET_DETAILS;
		draw_line(cr, &st, p, y);
		y += 1;
	}

	if (net_list_shown(f)) {
		y += 4;
		int rows = net_visible_rows(f);
		for (int i = f->scroll; i < f->nets->length && i < f->scroll + rows; i++) {
			struct wifi_net *n = f->nets->items[i];
			bool expanded = f->expanded && strcmp(f->expanded, n->ssid) == 0;
			bool shown = expanded && net_secret_shown(f);
			struct pbox row = { M + 6, y, W - 2 * M - 12, NET_ROW + (expanded ? NET_EXPAND : 0) +
				(shown ? NET_SECRET : 0) };
			if (expanded || hovered(&f->base, row)) {
				fill_hover(cr, &st, row);
			}
			ti_network(p->panel, cr, x0, y + 14, 20, signal_bars(n->signal), true, true, st.fg);
			// the band on the right, and the most it goes with this PC under it
			int tag = 0;
			if (n->bands) {
				char band[48], rate[48];
				net_band_text(n, band, sizeof(band));
				tag = 118;
				pd_text(cr, st.font, band, x0 + cw - tag, y + 6, tag, 20, st.fg, PD_RIGHT);
				if (n->rate > 0) {
					net_rate_text(n->rate, rate, sizeof(rate));
					pd_text(cr, st.font, rate, x0 + cw - tag, y + 25, tag, 18, st.dim, PD_RIGHT);
				}
			}
			pd_text(cr, n->active ? st.bold : st.font, n->ssid, x0 + 34, y + 6, cw - 34 - tag, 20,
				st.fg, PD_LEFT);
			bool secured = n->security[0] && strcmp(n->security, "--") != 0;
			char sub[96];
			snprintf(sub, sizeof(sub), "%s%s%s", n->active ? "Connected, " : "",
				secured ? "secured" : "open", n->known && !n->active ? ", saved" : "");
			sub[0] = toupper((unsigned char)sub[0]);
			pd_text(cr, st.font, sub, x0 + 34, y + 25, cw - 34 - tag, 18, st.dim, PD_LEFT);
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
					bool asked = f->secret_ssid && strcmp(f->secret_ssid, n->ssid) == 0;
					int left = cw - 110 - (n->known && secured ? 134 : 0) - 34 - 8;
					if (n->pair_known && left > 60) {
						// how the speed comes about: what both of them can do
						char how[96];
						char gen[16];
						snprintf(gen, sizeof(gen), n->pair.gen < 4 ? "802.11a/g" : "Wi-Fi %d",
							n->pair.gen);
						snprintf(how, sizeof(how), "%s \u00b7 %d\u00d7%d \u00b7 %d MHz", gen,
							n->pair.streams, n->pair.streams, n->pair.width);
						pd_text(cr, st.font, how, x0 + 34, by, left, bh, st.dim, PD_LEFT);
					}
					if (n->known && secured) {
						struct pbox show = { x0 + cw - 110 - 134, by, 130, bh };
						draw_button(cr, &st, show, asked ? "Hide password" : "Show password",
							false, hovered(&f->base, show));
						psurface_add_hotspot(p->surface, show.x, show.y, show.width,
							show.height, NULL, NET_HS_SECRET, i, NULL);
					}
					if (shown) {
						// the password on its own line, click it to copy it
						struct pbox line = { x0 + 34, by + bh, cw - 34, NET_SECRET };
						if (f->secret[0] && hovered(&f->base, line)) {
							fill_hover(cr, &st, line);
						}
						char text[160];
						snprintf(text, sizeof(text), f->secret[0] ? "Password: %s" : "%s",
							f->secret[0] ? f->secret : "Asking NetworkManager...");
						pd_text(cr, st.font, text, line.x, line.y, line.width - 60, line.height,
							st.fg, PD_LEFT);
						if (f->secret[0]) {
							pd_text(cr, st.font, "Copy", line.x, line.y, line.width,
								line.height, st.accent, PD_RIGHT);
							psurface_add_hotspot(p->surface, line.x, line.y, line.width,
								line.height, NULL, NET_HS_COPY_SECRET, i, NULL);
						}
					}
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
	case NET_HS_COPY: {
		const char *address = hs->id == 0 ? f->ipv4 : f->ipv6;
		if (*address) {
			clipboard_copy_text(p->panel, address);
			snprintf(f->copied, sizeof(f->copied), "%s", address);
			clock_gettime(CLOCK_MONOTONIC, &f->copied_at);
			popup_set_dirty(p);
		}
		break;
	}
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
			net_forget_secret(f);
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
	case NET_HS_SECRET:
		net_show_secret(f, (int)hs->id);
		break;
	case NET_HS_COPY_SECRET:
		if (f->secret[0]) {
			clipboard_copy_text(p->panel, f->secret);
			snprintf(f->status, sizeof(f->status), "Password copied to the clipboard");
			f->status_error = false;
			net_update(f);
		}
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
	if (f->stats_timer) {
		loop_remove_timer(p->panel->loop, f->stats_timer);
	}
	memset(f->password, 0, sizeof(f->password));
	net_forget_secret(f);
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
	net_stats_tick(NULL);
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

/* The task manager of the Default apps page ($taskmanager in common.conf). */
static char *default_task_manager(struct panel *panel) {
	return strdup("exec $taskmanager");
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

/* ================= memory ================= */

/*
 * Memory flyout: how much RAM is in use over the last minute, what it is made
 * up of, the swap and the processes holding the most of it. Read from /proc
 * once a second while the flyout is open.
 */

#define MEM_HISTORY_LEN 60
#define MEM_TOP 5
#define MEM_HEADER 66
#define MEM_GRAPH 112
#define MEM_BARS 96
#define MEM_INFO 60
#define MEM_PROC_ROW 24

enum mem_hotspot {
	MEM_HS_TASK_MANAGER = 1,
};

struct mem_usage {
	char name[64];
	double gib;
	int count;
};

struct mem_flyout {
	struct flyout base; // first member
	// everything in KiB, as /proc/meminfo has it
	long long total, available, free, buffers, cached, reclaimable;
	long long swap_total, swap_free;
	int history[MEM_HISTORY_LEN];
	int history_len;
	struct mem_usage top[MEM_TOP];
	int top_count;
	struct loop_timer *tick;
};

static struct mem_flyout *mem_current = NULL;

static long long mem_used(struct mem_flyout *f) {
	long long used = f->total - f->available;
	return used > 0 ? used : 0;
}

static int mem_percent(struct mem_flyout *f) {
	return f->total > 0 ? (int)(mem_used(f) * 100 / f->total) : 0;
}

static void mem_read_meminfo(struct mem_flyout *f) {
	FILE *file = fopen("/proc/meminfo", "r");
	if (!file) {
		return;
	}
	static const struct {
		const char *key;
		size_t offset;
	} fields[] = {
		{ "MemTotal:", offsetof(struct mem_flyout, total) },
		{ "MemFree:", offsetof(struct mem_flyout, free) },
		{ "MemAvailable:", offsetof(struct mem_flyout, available) },
		{ "Buffers:", offsetof(struct mem_flyout, buffers) },
		{ "Cached:", offsetof(struct mem_flyout, cached) },
		{ "SReclaimable:", offsetof(struct mem_flyout, reclaimable) },
		{ "SwapTotal:", offsetof(struct mem_flyout, swap_total) },
		{ "SwapFree:", offsetof(struct mem_flyout, swap_free) },
	};
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			size_t len = strlen(fields[i].key);
			if (strncmp(line, fields[i].key, len) != 0) {
				continue;
			}
			*(long long *)((char *)f + fields[i].offset) = strtoll(line + len, NULL, 10);
			break;
		}
	}
	fclose(file);
	// without MemAvailable (very old kernels) fall back to what is reusable
	if (f->available == 0 && f->total > 0) {
		f->available = f->free + f->buffers + f->cached + f->reclaimable;
	}
}

static int mem_usage_cmp(const void *a, const void *b) {
	const struct mem_usage *ua = a, *ub = b;
	return ub->gib > ua->gib ? 1 : ub->gib < ua->gib ? -1 : 0;
}

/* The processes holding the most memory, several of a name counted together. */
static void mem_read_processes(struct mem_flyout *f) {
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	struct mem_usage groups[64];
	int group_count = 0;
	DIR *dir = opendir("/proc");
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		char path[NAME_MAX + 16]; // d_name is a directory name, not just a pid
		snprintf(path, sizeof(path), "/proc/%s/statm", entry->d_name);
		FILE *file = fopen(path, "r");
		if (!file) {
			continue;
		}
		long long size = 0, resident = 0, shared = 0;
		bool ok = fscanf(file, "%lld %lld %lld", &size, &resident, &shared) == 3;
		fclose(file);
		// what the process holds on its own, without what it shares
		long long private_kb = ok ? (resident - shared) * page_kb : 0;
		if (private_kb <= 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		file = fopen(path, "r");
		char name[64] = "";
		if (file) {
			if (fgets(name, sizeof(name), file)) {
				name[strcspn(name, "\n")] = '\0';
			}
			fclose(file);
		}
		if (!name[0]) {
			continue;
		}
		double gib = private_kb / 1048576.0;
		int found = -1;
		for (int i = 0; i < group_count && found < 0; i++) {
			if (strcmp(groups[i].name, name) == 0) {
				found = i;
			}
		}
		if (found >= 0) {
			groups[found].gib += gib;
			groups[found].count++;
		} else if (group_count < (int)(sizeof(groups) / sizeof(groups[0]))) {
			struct mem_usage *u = &groups[group_count++];
			snprintf(u->name, sizeof(u->name), "%s", name);
			u->gib = gib;
			u->count = 1;
		}
	}
	if (dir) {
		closedir(dir);
	}
	qsort(groups, group_count, sizeof(groups[0]), mem_usage_cmp);
	f->top_count = group_count < MEM_TOP ? group_count : MEM_TOP;
	for (int i = 0; i < f->top_count; i++) {
		f->top[i] = groups[i];
	}
}

static void mem_sample(struct mem_flyout *f) {
	mem_read_meminfo(f);
	mem_read_processes(f);
	if (f->history_len == MEM_HISTORY_LEN) {
		memmove(f->history, f->history + 1, (MEM_HISTORY_LEN - 1) * sizeof(int));
		f->history_len--;
	}
	f->history[f->history_len++] = mem_percent(f);
}

static int mem_height(struct mem_flyout *f) {
	return MEM_HEADER + MEM_GRAPH + MEM_BARS + MEM_INFO + 30 + MEM_TOP * MEM_PROC_ROW + 8 +
		FOOTER;
}

static void mem_tick(void *data) {
	struct mem_flyout *f = mem_current;
	if (!f) {
		return;
	}
	mem_sample(f);
	flyout_resize(&f->base, mem_height(f));
	f->tick = loop_add_timer(f->base.panel->loop, 1000, mem_tick, NULL);
}

/* A bar of the breakdown: a share of the whole in its own color. */
static void mem_bar(cairo_t *cr, const struct fly_style *st, double x, double y, double w,
		double h, double share, uint32_t color) {
	cairo_new_path(cr);
	pd_rounded(cr, x, y, w, h, st->style == PS_CLASSIC ? 0 : 4);
	pd_color(cr, st->button_bg);
	cairo_fill(cr);
	double filled = share < 0 ? 0 : share > 1 ? 1 : share;
	if (filled > 0) {
		cairo_new_path(cr);
		pd_rounded(cr, x, y, w * filled, h, st->style == PS_CLASSIC ? 0 : 4);
		pd_color(cr, color);
		cairo_fill(cr);
	}
}

static void mem_format_size(char *buffer, size_t size, long long kib) {
	if (kib >= 1048576) {
		snprintf(buffer, size, "%.1f GiB", kib / 1048576.0);
	} else {
		snprintf(buffer, size, "%.0f MiB", kib / 1024.0);
	}
}

static void mem_render(struct popup *p, cairo_t *cr) {
	struct mem_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;
	char text[256], value[64], second[64];

	// header: share in use, and how much of how much
	snprintf(text, sizeof(text), "%d%%", mem_percent(f));
	pd_text(cr, st.big, text, x0, y + 12, 86, 36, st.fg, PD_LEFT);
	pd_text(cr, st.bold, "Memory", x0 + 90, y + 12, cw - 90, 20, st.fg, PD_LEFT);
	mem_format_size(value, sizeof(value), mem_used(f));
	mem_format_size(second, sizeof(second), f->total);
	snprintf(text, sizeof(text), "%s of %s in use", value, second);
	pd_text(cr, st.font, text, x0 + 90, y + 34, cw - 90, 20, st.dim, PD_LEFT);
	y += MEM_HEADER;

	// how much was in use over the last minute
	struct pbox g = { x0, y, cw, MEM_GRAPH - 26 };
	cairo_new_path(cr);
	pd_rounded(cr, g.x, g.y, g.width, g.height, st.style == PS_CLASSIC ? 0 : 6);
	pd_color(cr, st.button_bg);
	cairo_fill(cr);
	for (int i = 1; i < 4; i++) {
		pd_rect(cr, g.x, g.y + g.height * i / 4, g.width, 1, st.line);
	}
	if (f->history_len > 1) {
		double step = (double)g.width / (MEM_HISTORY_LEN - 1);
		double first_x = g.x + g.width - (f->history_len - 1) * step;
		cairo_new_path(cr);
		cairo_move_to(cr, first_x, g.y + g.height);
		for (int i = 0; i < f->history_len; i++) {
			cairo_line_to(cr, first_x + i * step,
				g.y + g.height - g.height * f->history[i] / 100.0);
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
	y += MEM_GRAPH;

	// what the memory is made up of, and the swap
	draw_line(cr, &st, p, y);
	long long cached = f->cached + f->buffers + f->reclaimable;
	long long swap_used = f->swap_total - f->swap_free;
	const struct {
		const char *label;
		long long kib, of;
		uint32_t color;
	} bars[] = {
		{ "In use", mem_used(f), f->total, st.accent },
		{ "Cached", cached, f->total, (st.accent & 0xffffff00) | 0x70 },
		{ "Swap", swap_used, f->swap_total, st.dim },
	};
	y += 6;
	for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
		pd_text(cr, st.font, bars[i].label, x0, y, 70, 18, st.dim, PD_LEFT);
		if (bars[i].of > 0) {
			mem_format_size(value, sizeof(value), bars[i].kib);
		} else {
			snprintf(value, sizeof(value), "none");
		}
		pd_text(cr, st.font, value, x0, y, cw, 18, st.fg, PD_RIGHT);
		mem_bar(cr, &st, x0 + 74, y + 4, cw - 74 - 90, 10,
			bars[i].of > 0 ? (double)bars[i].kib / bars[i].of : 0, bars[i].color);
		y += 30;
	}
	y += MEM_BARS - 6 - 3 * 30;

	// available, free and swap left
	draw_line(cr, &st, p, y);
	int cols_x[3] = { 0, cw * 36 / 100, cw * 70 / 100 };
	int cols_w[3] = { cols_x[1] - 6, cols_x[2] - cols_x[1] - 6, cw - cols_x[2] };
	const char *labels[3] = { "Available", "Free", "Swap free" };
	char values[3][64];
	mem_format_size(values[0], sizeof(values[0]), f->available);
	mem_format_size(values[1], sizeof(values[1]), f->free);
	if (f->swap_total > 0) {
		mem_format_size(values[2], sizeof(values[2]), f->swap_free);
	} else {
		snprintf(values[2], sizeof(values[2]), "No swap");
	}
	for (int i = 0; i < 3; i++) {
		pd_text(cr, st.font, labels[i], x0 + cols_x[i], y + 8, cols_w[i], 20, st.dim, PD_LEFT);
		pd_text(cr, st.bold, values[i], x0 + cols_x[i], y + 30, cols_w[i], 22, st.fg, PD_LEFT);
	}
	y += MEM_INFO;

	// processes holding the most memory
	draw_line(cr, &st, p, y);
	pd_text(cr, st.font, "Most memory", x0, y + 6, cw, 20, st.dim, PD_LEFT);
	pd_text(cr, st.font, "Private", x0, y + 6, cw, 20, st.dim, PD_RIGHT);
	y += 30;
	if (f->top_count == 0) {
		pd_text(cr, st.font, "Measuring...", x0, y, cw, MEM_PROC_ROW, st.dim, PD_LEFT);
	}
	double most = f->top_count > 0 ? f->top[0].gib : 0;
	for (int i = 0; i < f->top_count; i++) {
		struct mem_usage *u = &f->top[i];
		double share = most > 0 ? u->gib / most : 0;
		if (share > 0) {
			cairo_new_path(cr);
			pd_rounded(cr, x0 - 4, y + 2, (cw + 8) * share, MEM_PROC_ROW - 4,
				st.style == PS_CLASSIC ? 0 : 4);
			pd_color(cr, (st.accent & 0xffffff00) | 0x30);
			cairo_fill(cr);
		}
		if (u->count > 1) {
			snprintf(text, sizeof(text), "%s (%d)", u->name, u->count);
		} else {
			snprintf(text, sizeof(text), "%s", u->name);
		}
		pd_text(cr, st.font, text, x0, y, cw - 80, MEM_PROC_ROW, st.fg, PD_LEFT);
		mem_format_size(value, sizeof(value), (long long)(u->gib * 1048576));
		pd_text(cr, st.font, value, x0, y, cw, MEM_PROC_ROW, st.fg, PD_RIGHT);
		y += MEM_PROC_ROW;
	}
	y = M + mem_height(f) - FOOTER;

	draw_line(cr, &st, p, y);
	struct pbox link = { x0, y + 1, cw, FOOTER - 1 };
	draw_link(cr, &st, &f->base, link, "Open Task Manager", PD_LEFT);
	psurface_add_hotspot(p->surface, link.x, link.y, cw / 2, link.height, NULL,
		MEM_HS_TASK_MANAGER, 0, NULL);
}

static void mem_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct mem_flyout *f = p->data;
	if (button != BTN_LEFT || pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == MEM_HS_TASK_MANAGER) {
		run_settings(&f->base);
	}
}

static void mem_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void mem_destroy(struct popup *p) {
	struct mem_flyout *f = p->data;
	if (mem_current == f) {
		mem_current = NULL;
	}
	if (f->tick) {
		loop_remove_timer(p->panel->loop, f->tick);
	}
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable mem_vtable = {
	.render = mem_render,
	.motion = flyout_motion,
	.leave = flyout_leave,
	.button = mem_button,
	.key = mem_key,
	.destroy = mem_destroy,
};

void flyout_memory_toggle(struct panel *panel, struct popup_anchor anchor,
		const char *task_manager) {
	if (popup_is_open(panel, POPUP_MEMORY)) {
		popup_close_all(panel);
		return;
	}
	struct mem_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = task_manager ? strdup(task_manager) : default_task_manager(panel);
	mem_sample(f);
	if (!flyout_open(&f->base, POPUP_MEMORY, mem_height(f), &mem_vtable, f)) {
		free(f->base.settings);
		free(f);
		return;
	}
	mem_current = f;
	f->tick = loop_add_timer(panel->loop, 1000, mem_tick, NULL);
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

/* ================= clock ================= */

/*
 * The flyout of the clock widget: an analog and a digital clock, the calendar
 * of a month (the mouse wheel and the arrows walk through the months) and a
 * link to the date and time settings.
 */

#define CLOCK_HEADER 108
#define CLOCK_MONTH 34
#define CLOCK_WEEKDAYS 22
#define CLOCK_ROW 34
#define CLOCK_ROWS 6

struct clock_flyout {
	struct flyout base;
	struct loop_timer *tick;
	int year, month; // the month shown, month 0..11
	int sel_day;     // day clicked in that month, 0 for none
};

static struct clock_flyout *clock_current = NULL;

enum {
	CLOCK_HS_PREV = 1,
	CLOCK_HS_NEXT,
	CLOCK_HS_TODAY,
	CLOCK_HS_SETTINGS,
	CLOCK_HS_DAY,
};

static int clock_height(void) {
	return CLOCK_HEADER + CLOCK_MONTH + CLOCK_WEEKDAYS + CLOCK_ROWS * CLOCK_ROW + FOOTER;
}

static int days_in_month(int year, int month) {
	static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
		return 29;
	}
	return days[month];
}

static void clock_show_today(struct clock_flyout *f) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	f->year = tm.tm_year + 1900;
	f->month = tm.tm_mon;
	f->sel_day = 0;
}

static void clock_shift(struct clock_flyout *f, int months) {
	f->month += months;
	while (f->month < 0) {
		f->month += 12;
		f->year--;
	}
	while (f->month > 11) {
		f->month -= 12;
		f->year++;
	}
	f->sel_day = 0;
	popup_set_dirty(f->base.popup);
}

/* The clock face, with the hands of the given time. */
static void draw_clock_face(cairo_t *cr, const struct fly_style *st, double cx, double cy,
		double r, const struct tm *tm) {
	uint32_t face = st->dark ? 0xffffff14 : 0x00000008;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	pd_color(cr, face);
	cairo_fill_preserve(cr);
	pd_color(cr, st->style == PS_CLASSIC ? st->fg : st->line | 0x60);
	cairo_set_line_width(cr, st->style == PS_CLASSIC ? 2 : 1.5);
	cairo_stroke(cr);

	for (int i = 0; i < 12; i++) {
		double a = i * M_PI / 6;
		double len = i % 3 == 0 ? 6 : 3;
		cairo_new_path(cr);
		cairo_move_to(cr, cx + sin(a) * (r - 4), cy - cos(a) * (r - 4));
		cairo_line_to(cr, cx + sin(a) * (r - 4 - len), cy - cos(a) * (r - 4 - len));
		pd_color(cr, i % 3 == 0 ? st->fg : st->dim);
		cairo_set_line_width(cr, i % 3 == 0 ? 2 : 1);
		cairo_stroke(cr);
	}

	double hours = (tm->tm_hour % 12) + tm->tm_min / 60.0;
	double minutes = tm->tm_min + tm->tm_sec / 60.0;
	const struct { double angle, length, width; uint32_t color; } hands[] = {
		{ hours * M_PI / 6, r * 0.5, 3.5, st->fg },
		{ minutes * M_PI / 30, r * 0.74, 2.5, st->fg },
		{ tm->tm_sec * M_PI / 30, r * 0.8, 1, st->style == PS_CLASSIC ? 0xc00000ff : st->accent },
	};
	for (size_t i = 0; i < sizeof(hands) / sizeof(hands[0]); i++) {
		cairo_new_path(cr);
		cairo_move_to(cr, cx - sin(hands[i].angle) * 4, cy + cos(hands[i].angle) * 4);
		cairo_line_to(cr, cx + sin(hands[i].angle) * hands[i].length,
			cy - cos(hands[i].angle) * hands[i].length);
		pd_color(cr, hands[i].color);
		cairo_set_line_width(cr, hands[i].width);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
	}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 3, 0, 2 * M_PI);
	pd_color(cr, st->fg);
	cairo_fill(cr);
}

/* The taskbar clock decides whether the flyout shows a 12- or a 24-hour time. */
static bool clock_uses_12h(struct panel *panel) {
	const char *format = NULL;
	for (int i = 0; panel->config && i < panel->config->widgets->length; i++) {
		struct widget *w = panel->config->widgets->items[i];
		if (w->impl == &widget_clock && widget_conf(w, "format", NULL)) {
			format = widget_conf(w, "format", NULL);
			break;
		}
	}
	if (!format) {
		format = tw_theme_str(panel->theme, "clock.format", "%H:%M");
	}
	return strstr(format, "%I") || strstr(format, "%p") || strstr(format, "%r");
}

static void clock_render(struct popup *p, cairo_t *cr) {
	struct clock_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);

	// header: the clock face, the time and today's date
	double r = (CLOCK_HEADER - 32) / 2.0;
	draw_clock_face(cr, &st, x0 + r + 2, y + 16 + r, r, &today);
	const char *time_format = tw_theme_str(p->panel->theme, "clock.flyout_format",
		clock_uses_12h(p->panel) ? "%-I:%M:%S %p" : "%H:%M:%S");
	const char *date_format = tw_theme_str(p->panel->theme, "clock.flyout_date", "%A, %d %B %Y");
	char big[64], date[128];
	strftime(big, sizeof(big), time_format, &today);
	strftime(date, sizeof(date), date_format, &today);
	int tx = x0 + 2 * (int)r + 20, tw = cw - (tx - x0);
	pd_text(cr, st.big, big, tx, y + 24, tw, 34, st.fg, PD_LEFT);
	pd_text(cr, st.font, date, tx, y + 58, tw, 22, st.dim, PD_LEFT);
	y += CLOCK_HEADER;

	// the month, with an arrow on each side
	draw_line(cr, &st, p, y);
	char title[64];
	struct tm first = { .tm_year = f->year - 1900, .tm_mon = f->month, .tm_mday = 1 };
	mktime(&first);
	strftime(title, sizeof(title), "%B %Y", &first);
	pd_text(cr, st.bold, title, x0 + 28, y, cw - 56, CLOCK_MONTH, st.fg, PD_CENTER);
	struct pbox prev = { x0, y, 28, CLOCK_MONTH }, next = { x0 + cw - 28, y, 28, CLOCK_MONTH };
	if (hovered(&f->base, prev)) {
		fill_hover(cr, &st, prev);
	}
	if (hovered(&f->base, next)) {
		fill_hover(cr, &st, next);
	}
	pd_glyph_arrow(cr, prev.x + 7, y + (CLOCK_MONTH - 14) / 2, 14, 2, st.fg);
	pd_glyph_arrow(cr, next.x + 7, y + (CLOCK_MONTH - 14) / 2, 14, 0, st.fg);
	psurface_add_hotspot(p->surface, prev.x, prev.y, prev.width, prev.height, NULL,
		CLOCK_HS_PREV, 0, NULL);
	psurface_add_hotspot(p->surface, next.x, next.y, next.width, next.height, NULL,
		CLOCK_HS_NEXT, 0, NULL);
	y += CLOCK_MONTH;

	static const char *weekdays[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
	double cell = cw / 7.0;
	for (int i = 0; i < 7; i++) {
		pd_text(cr, st.font, weekdays[i], x0 + i * cell, y, cell, CLOCK_WEEKDAYS, st.dim,
			PD_CENTER);
	}
	y += CLOCK_WEEKDAYS;

	int offset = (first.tm_wday + 6) % 7;
	int days = days_in_month(f->year, f->month);
	int prev_days = days_in_month(f->month == 0 ? f->year - 1 : f->year,
		f->month == 0 ? 11 : f->month - 1);
	for (int i = 0; i < 7 * CLOCK_ROWS; i++) {
		int day = i - offset + 1;
		bool other = day < 1 || day > days;
		int shown = day < 1 ? prev_days + day : day > days ? day - days : day;
		struct pbox box = { x0 + (i % 7) * cell, y + (i / 7) * CLOCK_ROW, cell, CLOCK_ROW };
		bool is_today = !other && day == today.tm_mday && f->month == today.tm_mon &&
			f->year == today.tm_year + 1900;
		bool selected = !other && day == f->sel_day;
		uint32_t color = other ? st.dim : st.fg;
		double cx = box.x + box.width / 2, cy = box.y + box.height / 2;
		double rr = CLOCK_ROW / 2.0 - 2;
		if (is_today || selected) {
			cairo_new_path(cr);
			if (st.style == PS_CLASSIC || st.style == PS_LUNA) {
				if (is_today) {
					pd_rect(cr, cx - rr, cy - rr, 2 * rr, 2 * rr, st.accent);
				} else {
					cairo_rectangle(cr, cx - rr + 0.5, cy - rr + 0.5, 2 * rr - 1, 2 * rr - 1);
					pd_color(cr, st.accent);
					cairo_set_line_width(cr, 1);
					cairo_stroke(cr);
				}
			} else if (is_today) {
				cairo_arc(cr, cx, cy, rr, 0, 2 * M_PI);
				pd_color(cr, st.accent);
				cairo_fill(cr);
			} else {
				cairo_arc(cr, cx, cy, rr - 0.5, 0, 2 * M_PI);
				pd_color(cr, st.accent);
				cairo_set_line_width(cr, 1.5);
				cairo_stroke(cr);
			}
			color = is_today ? 0xffffffff : st.fg;
		} else if (!other && hovered(&f->base, box)) {
			fill_hover(cr, &st, box);
		}
		char num[8];
		snprintf(num, sizeof(num), "%d", shown);
		pd_text(cr, is_today || selected ? st.bold : st.font, num, box.x, box.y, box.width,
			box.height, color, PD_CENTER);
		if (!other) {
			psurface_add_hotspot(p->surface, box.x, box.y, box.width, box.height, NULL,
				CLOCK_HS_DAY, day, NULL);
		}
	}
	y += CLOCK_ROWS * CLOCK_ROW;

	// footer: the settings of the app, and back to this month
	draw_line(cr, &st, p, y);
	struct pbox link = { x0, y + 1, cw, FOOTER - 1 };
	if (f->base.settings) {
		draw_link(cr, &st, &f->base, link, "Change date and time", PD_LEFT);
		psurface_add_hotspot(p->surface, link.x, link.y, cw / 2, link.height, NULL,
			CLOCK_HS_SETTINGS, 0, NULL);
	}
	bool is_this_month = f->year == today.tm_year + 1900 && f->month == today.tm_mon;
	if (!is_this_month || f->sel_day) {
		draw_link(cr, &st, &f->base, link, "Today", PD_RIGHT);
		int tw2 = 0;
		pd_text_size(cr, st.font, "Today", &tw2, NULL);
		psurface_add_hotspot(p->surface, link.x + link.width - tw2 - 8, link.y, tw2 + 8,
			link.height, NULL, CLOCK_HS_TODAY, 0, NULL);
	}
}

static void clock_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct clock_flyout *f = p->data;
	if (button != BTN_LEFT || pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	if (hs->kind == CLOCK_HS_PREV) {
		clock_shift(f, -1);
	} else if (hs->kind == CLOCK_HS_NEXT) {
		clock_shift(f, 1);
	} else if (hs->kind == CLOCK_HS_TODAY) {
		clock_show_today(f);
		popup_set_dirty(p);
	} else if (hs->kind == CLOCK_HS_DAY) {
		f->sel_day = f->sel_day == (int)hs->id ? 0 : (int)hs->id;
		popup_set_dirty(p);
	} else if (hs->kind == CLOCK_HS_SETTINGS) {
		run_settings(&f->base);
	}
}

static void clock_axis(struct popup *p, double x, double y, int direction) {
	clock_shift(p->data, direction);
}

static void clock_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct clock_flyout *f = p->data;
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_Page_Up) {
		clock_shift(f, -1);
	} else if (sym == XKB_KEY_Right || sym == XKB_KEY_Page_Down) {
		clock_shift(f, 1);
	} else if (sym == XKB_KEY_Home) {
		clock_show_today(f);
		popup_set_dirty(p);
	}
}

static void clock_flyout_tick(void *data) {
	struct clock_flyout *f = clock_current;
	if (!f) {
		return;
	}
	popup_set_dirty(f->base.popup);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	f->tick = loop_add_timer(f->base.panel->loop, 1000 - ts.tv_nsec / 1000000,
		clock_flyout_tick, NULL);
}

static void clock_flyout_destroy(struct popup *p) {
	struct clock_flyout *f = p->data;
	if (clock_current == f) {
		clock_current = NULL;
	}
	if (f->tick) {
		loop_remove_timer(p->panel->loop, f->tick);
	}
	free(f->base.settings);
	free(f);
}

static const struct popup_vtable clock_vtable = {
	.render = clock_render,
	.motion = flyout_motion,
	.leave = flyout_leave,
	.button = clock_button,
	.axis = clock_axis,
	.key = clock_key,
	.destroy = clock_flyout_destroy,
};

void calendar_toggle(struct panel *panel, struct popup_anchor anchor, const char *settings) {
	if (popup_is_open(panel, POPUP_CALENDAR)) {
		popup_close_all(panel);
		return;
	}
	struct clock_flyout *f = calloc(1, sizeof(*f));
	f->base.panel = panel;
	f->base.anchor = anchor;
	f->base.settings = strdup(settings && *settings ? settings : TW_DATETIME_SETTINGS);
	clock_show_today(f);
	if (!flyout_open(&f->base, POPUP_CALENDAR, clock_height(), &clock_vtable, f)) {
		free(f->base.settings);
		free(f);
		return;
	}
	clock_current = f;
	clock_flyout_tick(NULL);
}
