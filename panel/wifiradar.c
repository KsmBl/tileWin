/*
 * The Wi-Fi radar (wifiradar.h). nmcli is asked for the networks in reach,
 * their signal, security and band, and for the saved connections; every
 * radar draws from that. A network keeps its bearing (from its name) and
 * glides to its new distance when its signal changes; one that comes in
 * fades in with a ping, one that goes fades out. The sweep lights a contact
 * up as it passes and names it.
 *
 * Connecting: a saved network (nmcli connection up) and an open one (nmcli
 * device wifi connect) are connected to at once, while the radar shows the
 * link being made; a new network with a password, or a saved one whose
 * password was turned down, opens in the network flyout with the field for
 * it, which does the rest. The contact connected to is tied to the middle by
 * a beam.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "wifiradar.h"

#define SCAN_MS 30000     // NetworkManager scans again when its list is older than that
#define SOON_MS 1500
#define GLIDE 1.6         // seconds a contact takes to its new distance
#define FADE 0.8          // seconds to fade in, and out
#define FAILED_SHOWN 6.0
#define JOINED_SHOWN 2.0
#define PING 1.4          // seconds the ping of a click goes out
#define LISTENERS 16

static struct {
	struct panel *panel;
	int holds;
	struct loop_timer *timer;
	struct proc *proc;
	int looks;
	double started;
	enum radar_status status;
	struct radar_net nets[RADAR_MAX];
	int count;
	// connecting from the radar
	char trying[96];
	struct proc *link_proc;
	struct popup_anchor anchor;
	char failed[96], why[128];
	double failed_at;
	char joined[96];
	double joined_at;
	double ping_at;
	struct {
		void (*fn)(void *data);
		void *data;
	} listeners[LISTENERS];
} rd;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void changed(void) {
	for (int i = 0; i < LISTENERS; i++) {
		if (rd.listeners[i].fn) {
			rd.listeners[i].fn(rd.listeners[i].data);
		}
	}
}

void wifiradar_listen(void (*fn)(void *data), void *data) {
	for (int i = 0; i < LISTENERS; i++) {
		if (!rd.listeners[i].fn) {
			rd.listeners[i].fn = fn;
			rd.listeners[i].data = data;
			return;
		}
	}
}

void wifiradar_unlisten(void (*fn)(void *data), void *data) {
	for (int i = 0; i < LISTENERS; i++) {
		if (rd.listeners[i].fn == fn && rd.listeners[i].data == data) {
			rd.listeners[i].fn = NULL;
			rd.listeners[i].data = NULL;
		}
	}
}

/* ---------- the networks ---------- */

static double angle_of(const char *name) {
	unsigned long h = 5381;
	for (const char *c = name; *c; c++) {
		h = h * 33 + (unsigned char)*c;
	}
	return (h % 3600) / 3600.0 * 2 * M_PI;
}

/* The distance from the middle a signal has: 0 in the middle, 1 at the outer ring. */
static double distance_of(int signal) {
	return 0.14 + 0.82 * (1 - fmin(fmax(signal, 0), 100) / 100.0);
}

/* Splits a line of nmcli -t at the colons that are not escaped, in place. */
static int split_terse(char *line, char **fields, int max) {
	int n = 0;
	fields[n++] = line;
	char *out = line;
	for (char *p = line; *p; p++) {
		if (*p == '\\' && p[1]) {
			*out++ = *++p;
		} else if (*p == ':' && n < max) {
			*out++ = '\0';
			fields[n++] = out;
		} else {
			*out++ = *p;
		}
	}
	*out = '\0';
	return n;
}

static struct radar_net *find(const char *ssid) {
	for (int i = 0; i < rd.count; i++) {
		if (strcmp(rd.nets[i].ssid, ssid) == 0) {
			return &rd.nets[i];
		}
	}
	return NULL;
}

/* Where a contact is now, between where it was and where it goes. */
static double distance_now(const struct radar_net *n, double now) {
	double t = fmin(fmax((now - n->moved) / GLIDE, 0), 1);
	t = t * t * (3 - 2 * t);
	return n->r_from + (n->r_to - n->r_from) * t;
}

static void scan_done(void *data, const char *output) {
	rd.proc = NULL;
	double now = now_s();
	enum { S_RADIO, S_DEV, S_WIFI, S_KNOWN } section = S_RADIO;
	bool radio = false, device = false, missing = strstr(output, "--missing") != NULL;
	bool seen[RADAR_MAX] = { false };
	char known[RADAR_MAX][2][96];
	int known_count = 0;
	char *copy = strdup(output);
	char *save = NULL;
	const char *was_linked = NULL;
	char linked_before[96] = "";
	for (int i = 0; i < rd.count; i++) {
		if (rd.nets[i].active && !rd.nets[i].gone) {
			snprintf(linked_before, sizeof(linked_before), "%s", rd.nets[i].ssid);
		}
		rd.nets[i].active = false;
	}
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strcmp(line, "--dev") == 0) {
			section = S_DEV;
			continue;
		} else if (strcmp(line, "--wifi") == 0) {
			section = S_WIFI;
			continue;
		} else if (strcmp(line, "--known") == 0) {
			section = S_KNOWN;
			continue;
		}
		if (section == S_RADIO) {
			radio = strcmp(line, "enabled") == 0;
		} else if (section == S_DEV) {
			char *v[4];
			if (split_terse(line, v, 4) >= 2 && strcmp(v[1], "wifi") == 0) {
				device = true;
			}
		} else if (section == S_KNOWN) {
			// "<uuid>:<ssid>", written by the query itself, not escaped
			char *colon = strchr(line, ':');
			if (colon && colon[1] && known_count < RADAR_MAX) {
				snprintf(known[known_count][0], 96, "%.*s", (int)(colon - line), line);
				snprintf(known[known_count][1], 96, "%s", colon + 1);
				known_count++;
			}
		} else if (radio) {
			// IN-USE:SIGNAL:FREQ:SECURITY:SSID (none while Wi-Fi is off)
			char *v[5];
			if (split_terse(line, v, 5) < 5 || !v[4][0]) {
				continue;
			}
			struct radar_net *n = find(v[4]);
			int signal = atoi(v[1]);
			if (n && seen[n - rd.nets]) {
				// another access point of a network already seen: the strongest counts
				n->active |= strcmp(v[0], "*") == 0;
				if (signal > n->signal) {
					n->signal = signal;
				}
				continue;
			}
			if (!n) {
				// the place of one long gone, else a new one
				int slot = rd.count < RADAR_MAX ? rd.count++ : -1;
				for (int i = 0; slot < 0 && i < rd.count; i++) {
					if (!seen[i] && rd.nets[i].gone) {
						slot = i;
					}
				}
				if (slot < 0) {
					continue;
				}
				n = &rd.nets[slot];
				memset(n, 0, sizeof(*n));
				snprintf(n->ssid, sizeof(n->ssid), "%s", v[4]);
				n->angle = angle_of(n->ssid);
				n->appeared = rd.looks > 1 ? now : now - FADE; // the first ones are just there
				n->r_from = n->r_to = distance_of(signal);
				n->moved = now - GLIDE;
				n->signal = signal;
			}
			seen[n - rd.nets] = true;
			n->gone = 0;
			n->active = strcmp(v[0], "*") == 0;
			n->freq = atoi(v[2]);
			snprintf(n->security, sizeof(n->security), "%s", v[3]);
			n->secured = v[3][0] && strcmp(v[3], "--") != 0;
			n->signal = signal;
		}
	}
	free(copy);
	for (int i = 0; i < rd.count; i++) {
		struct radar_net *n = &rd.nets[i];
		if (!seen[i]) {
			if (!n->gone) {
				n->gone = now;
			}
			continue;
		}
		double target = distance_of(n->signal);
		if (fabs(target - n->r_to) > 0.01) {
			n->r_from = distance_now(n, now);
			n->r_to = target;
			n->moved = now;
		}
		n->known = false;
		n->uuid[0] = '\0';
		for (int k = 0; k < known_count; k++) {
			if (strcmp(known[k][1], n->ssid) == 0) {
				n->known = true;
				snprintf(n->uuid, sizeof(n->uuid), "%s", known[k][0]);
				break;
			}
		}
		if (n->active) {
			was_linked = n->ssid;
		}
	}
	// those gone for good make room
	int kept = 0;
	for (int i = 0; i < rd.count; i++) {
		if (rd.nets[i].gone && now - rd.nets[i].gone > FADE) {
			continue;
		}
		if (kept != i) {
			rd.nets[kept] = rd.nets[i];
		}
		kept++;
	}
	rd.count = kept;
	rd.status = missing ? RADAR_NO_NM : !device ? RADAR_NO_DEVICE : !radio ? RADAR_OFF :
		RADAR_READY;
	if (was_linked && strcmp(was_linked, linked_before) != 0) {
		// connected now, wherever that was done: the contact flashes
		snprintf(rd.joined, sizeof(rd.joined), "%s", was_linked);
		rd.joined_at = now;
	}
	changed();
}

static void scan(bool rescan) {
	if (rd.proc) {
		return;
	}
	char *cmd = format_str("command -v nmcli >/dev/null || { echo --missing; exit 0; }; "
		"nmcli radio wifi 2>/dev/null; echo --dev; "
		"nmcli -t -f DEVICE,TYPE device 2>/dev/null; echo --wifi; "
		"nmcli -t -f IN-USE,SIGNAL,FREQ,SECURITY,SSID device wifi list --rescan %s 2>/dev/null; "
		// the name of a connection need not be the SSID: each saved one is asked
		"echo --known; nmcli -t -f UUID,TYPE connection show 2>/dev/null | "
		"while IFS=: read -r uuid type; do "
		"[ \"$type\" = 802-11-wireless ] || continue; "
		"ssid=$(nmcli -g 802-11-wireless.ssid connection show uuid \"$uuid\" 2>/dev/null); "
		"[ -n \"$ssid\" ] && printf '%%s:%%s\\n' \"$uuid\" \"$ssid\"; done",
		rescan ? "auto" : "no");
	rd.proc = proc_run(rd.panel, cmd, false, NULL, scan_done, NULL);
	free(cmd);
}

static void scan_tick(void *data) {
	rd.timer = NULL;
	// the first look takes what NetworkManager has, the second soon after asks it to scan
	scan(rd.looks > 0);
	rd.timer = loop_add_timer_lazy(rd.panel->loop, rd.looks++ == 0 ? 3000 : SCAN_MS,
		scan_tick, NULL);
}

void wifiradar_rescan(void) {
	if (!rd.holds) {
		return;
	}
	if (rd.timer) {
		loop_remove_timer(rd.panel->loop, rd.timer);
	}
	rd.timer = loop_add_timer_lazy(rd.panel->loop, SOON_MS, scan_tick, NULL);
}

void wifiradar_hold(struct panel *panel) {
	if (rd.holds++ > 0) {
		return;
	}
	rd.panel = panel;
	if (rd.started == 0) {
		rd.started = now_s();
	}
	rd.looks = 0;
	scan_tick(NULL);
}

void wifiradar_release(void) {
	if (rd.holds == 0 || --rd.holds > 0) {
		return;
	}
	if (rd.timer) {
		loop_remove_timer(rd.panel->loop, rd.timer);
		rd.timer = NULL;
	}
	proc_cancel(rd.proc);
	rd.proc = NULL;
}

enum radar_status wifiradar_status(void) {
	return rd.status;
}

int wifiradar_slots(void) {
	return rd.count;
}

const struct radar_net *wifiradar_slot(int i) {
	return i >= 0 && i < rd.count ? &rd.nets[i] : NULL;
}

int wifiradar_count(void) {
	int n = 0;
	for (int i = 0; i < rd.count; i++) {
		n += !rd.nets[i].gone;
	}
	return n;
}

const char *wifiradar_linked(void) {
	if (rd.status != RADAR_READY) {
		return NULL;
	}
	for (int i = 0; i < rd.count; i++) {
		if (rd.nets[i].active && !rd.nets[i].gone) {
			return rd.nets[i].ssid;
		}
	}
	return NULL;
}

const char *wifiradar_connecting(void) {
	if (rd.trying[0]) {
		return rd.trying;
	}
	return flyout_network_connecting();
}

bool wifiradar_busy(void) {
	double now = now_s();
	if (wifiradar_connecting() || now - rd.ping_at < PING || now - rd.joined_at < JOINED_SHOWN) {
		return true;
	}
	for (int i = 0; i < rd.count; i++) {
		const struct radar_net *n = &rd.nets[i];
		if (now - n->moved < GLIDE || now - n->appeared < FADE || n->gone) {
			return true;
		}
	}
	return false;
}

/* ---------- connecting ---------- */

static bool password_error(const char *error) {
	static const char *const words[] = { "secrets were required", "no secrets", "psk",
		"wep-key", "password", "802-1x", "802.1x supplicant", "supplicant-disconnect" };
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strcasestr(error, words[i])) {
			return true;
		}
	}
	return false;
}

static void link_done(void *data, const char *output) {
	rd.link_proc = NULL;
	char ssid[96];
	snprintf(ssid, sizeof(ssid), "%s", rd.trying);
	rd.trying[0] = '\0';
	const char *error = strstr(output, "Error");
	if (error && password_error(error)) {
		// the saved password was turned down: the flyout asks for a new one
		flyout_network_open_for(rd.panel, rd.anchor, ssid, true);
	} else if (error) {
		snprintf(rd.failed, sizeof(rd.failed), "%s", ssid);
		const char *why = error + 5;
		while (*why == ':' || *why == ' ') {
			why++;
		}
		// what is left after "Connection activation failed:" says why
		const char *after = strstr(why, "failed: ");
		if (after && after[8] && after[8] != '\n') {
			why = after + 8;
		}
		snprintf(rd.why, sizeof(rd.why), "%.*s", (int)strcspn(why, "\n"), why);
		rd.failed_at = now_s();
	} else {
		snprintf(rd.joined, sizeof(rd.joined), "%s", ssid);
		rd.joined_at = now_s();
	}
	wifiradar_rescan();
	changed();
}

static void connect_to(const struct radar_net *n, struct popup_anchor anchor) {
	if (n->active) {
		// connected already: its details, in the flyout
		flyout_network_open_for(rd.panel, anchor, n->ssid, false);
		return;
	}
	if (n->secured && !n->known) {
		// a password is needed: the flyout has the field for it
		flyout_network_open_for(rd.panel, anchor, n->ssid, true);
		return;
	}
	if (rd.link_proc) {
		return; // one at a time
	}
	char *cmd;
	if (n->known && n->uuid[0]) {
		char *uuid = g_shell_quote(n->uuid);
		cmd = format_str("nmcli connection up uuid %s 2>&1", uuid);
		g_free(uuid);
	} else {
		// nmcli keeps the connection it makes even when it fails: one that was
		// not there before goes again, so the network is not taken as saved
		char *ssid = g_shell_quote(n->ssid);
		cmd = format_str("before=$(nmcli -g UUID connection show 2>/dev/null); "
			"if ! nmcli device wifi connect %s 2>&1; then "
			"for u in $(nmcli -g UUID connection show 2>/dev/null); do "
			"case \"$before\" in *\"$u\"*) continue ;; esac; "
			"[ \"$(nmcli -g 802-11-wireless.ssid connection show uuid \"$u\" 2>/dev/null)\" = %s ] && "
			"nmcli connection delete uuid \"$u\" >/dev/null 2>&1; "
			"done; fi", ssid, ssid);
		g_free(ssid);
	}
	snprintf(rd.trying, sizeof(rd.trying), "%s", n->ssid);
	rd.failed[0] = '\0';
	rd.anchor = anchor;
	rd.link_proc = proc_run(rd.panel, cmd, false, NULL, link_done, NULL);
	free(cmd);
	changed();
}

/* ---------- geometry ---------- */

/* Room outside the outer ring: the ticks, and the bearings when there are some. */
static double margin(const struct radar_look *look) {
	return (look->bearings ? 26 : 10) * look->scale;
}

void wifiradar_geom(double x, double y, double w, double h, const struct radar_look *look,
		struct radar_geom *g) {
	double size = fmin(w, h);
	g->cx = x + w / 2;
	g->cy = y + h / 2;
	g->R = fmax(size / 2 - margin(look), 8);
}

bool wifiradar_in_disc(const struct radar_geom *g, double x, double y) {
	double r = g->R + 6;
	return (x - g->cx) * (x - g->cx) + (y - g->cy) * (y - g->cy) <= r * r;
}

/*
 * Where the contacts are drawn: at their bearing and distance, those too close
 * to one drawn before turned a little further round so each can be pointed at.
 */
static void positions(const struct radar_geom *g, const struct radar_look *look, double now,
		double *xs, double *ys) {
	double apart = 11 * look->scale;
	for (int i = 0; i < rd.count; i++) {
		const struct radar_net *n = &rd.nets[i];
		double r = g->R * distance_now(n, now), a = n->angle - M_PI / 2;
		double step = r > 1 ? fmin(apart / r, 0.6) : 0.6;
		for (int tries = 0; tries < 12; tries++) {
			xs[i] = g->cx + cos(a) * r;
			ys[i] = g->cy + sin(a) * r;
			bool crowded = false;
			for (int j = 0; j < i && !crowded; j++) {
				crowded = hypot(xs[i] - xs[j], ys[i] - ys[j]) < apart;
			}
			if (!crowded) {
				break;
			}
			a += step;
		}
	}
}

int wifiradar_hit(const struct radar_geom *g, const struct radar_look *look, double x,
		double y) {
	if (rd.status != RADAR_READY) {
		return -1;
	}
	double xs[RADAR_MAX], ys[RADAR_MAX];
	positions(g, look, now_s(), xs, ys);
	int best = -1;
	double best_d = 12 * look->scale;
	best_d = fmax(best_d, 10);
	for (int i = 0; i < rd.count; i++) {
		if (rd.nets[i].gone) {
			continue;
		}
		double d = hypot(x - xs[i], y - ys[i]);
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

void wifiradar_click(const struct radar_geom *g, const struct radar_look *look, double x,
		double y, struct popup_anchor anchor) {
	int i = wifiradar_hit(g, look, x, y);
	if (i >= 0) {
		connect_to(&rd.nets[i], anchor);
		return;
	}
	if (!wifiradar_in_disc(g, x, y)) {
		return;
	}
	if (rd.status == RADAR_OFF) {
		proc_run(rd.panel, "nmcli radio wifi on 2>&1", false, NULL, NULL, NULL);
		rd.status = RADAR_LOOKING;
		rd.looks = 1; // the next look scans
		wifiradar_rescan();
		changed();
		return;
	}
	if (rd.status == RADAR_NO_DEVICE || rd.status == RADAR_NO_NM) {
		return;
	}
	// a ping goes out, and the networks are looked for again
	rd.ping_at = now_s();
	if (rd.timer) {
		loop_remove_timer(rd.panel->loop, rd.timer);
		rd.timer = NULL;
	}
	scan(true);
	rd.timer = loop_add_timer_lazy(rd.panel->loop, SCAN_MS, scan_tick, NULL);
	changed();
}

/* ---------- drawing ---------- */

static uint32_t alpha(uint32_t color, double a) {
	a = fmin(fmax(a, 0), 1);
	return (color & 0xffffff00) | (uint32_t)lround((color & 0xff) * a);
}

static void font(char *out, size_t size, const struct radar_look *look, bool bold, double px) {
	snprintf(out, size, "%s%s %dpx", look->family, bold ? " Bold" : "",
		(int)lround(fmax(px, 6)));
}

static void label(cairo_t *cr, const struct radar_look *look, const char *f, const char *str,
		double x, double y, double w, double h, uint32_t color, enum pd_align align) {
	char *up = look->upper ? g_utf8_strup(str, -1) : NULL;
	if (look->shadow) {
		pd_text(cr, f, up ? up : str, x + 1, y + 1, w, h, alpha(0x00000090, (color & 0xff) / 255.0),
			align);
	}
	pd_text(cr, f, up ? up : str, x, y, w, h, color, align);
	g_free(up);
}

static double sweep_angle(const struct radar_look *look, double now) {
	double turn = look->turn > 0.5 ? look->turn : 6;
	return fmod((now - rd.started) / turn, 1.0) * 2 * M_PI - M_PI / 2;
}

static const char *band_of(int freq) {
	return freq >= 5925 ? "6 GHz" : freq >= 4900 ? "5 GHz" : freq > 0 ? "2.4 GHz" : NULL;
}

/* The card of the contact under the pointer: its name, how it is, what a click does. */
static void draw_card(cairo_t *cr, const struct radar_look *look, const struct radar_net *n,
		double bx, double by) {
	double sc = look->scale;
	char bold[200], small[200];
	font(bold, sizeof(bold), look, true, 12 * sc);
	font(small, sizeof(small), look, false, 10.5 * sc);
	char facts[160];
	const char *band = band_of(n->freq);
	snprintf(facts, sizeof(facts), "Signal %d%%%s%s%s%s", n->signal,
		n->secured ? " · " : " · Open", n->secured ? n->security : "", band ? " · " : "",
		band ? band : "");
	const char *connecting = wifiradar_connecting();
	char hint[200];
	if (connecting && strcmp(connecting, n->ssid) == 0) {
		snprintf(hint, sizeof(hint), "Connecting...");
	} else if (rd.failed[0] && strcmp(rd.failed, n->ssid) == 0 &&
			now_s() - rd.failed_at < FAILED_SHOWN) {
		snprintf(hint, sizeof(hint), "Could not connect%s%s", rd.why[0] ? ": " : "", rd.why);
	} else if (n->active) {
		snprintf(hint, sizeof(hint), "Connected · click for details");
	} else if (n->known) {
		snprintf(hint, sizeof(hint), "Saved · click to connect");
	} else if (n->secured) {
		snprintf(hint, sizeof(hint), "Click to connect with its password");
	} else {
		snprintf(hint, sizeof(hint), "Click to connect");
	}
	char *name = look->upper ? g_utf8_strup(n->ssid, -1) : g_strdup(n->ssid);
	int w1 = 0, w2 = 0, w3 = 0, h1 = 0, h2 = 0;
	pd_text_size(cr, bold, name, &w1, &h1);
	pd_text_size(cr, small, facts, &w2, &h2);
	pd_text_size(cr, small, hint, &w3, NULL);
	double pad = 8 * sc, line = fmax(h2, 10 * sc) + 2 * sc;
	double w = fmin(fmax(fmax(w1, w2), w3) + 2 * pad, look->cards_w);
	double h = fmax(h1, 12 * sc) + 2 * line + 2 * pad;
	// beside the contact, kept inside where cards may go
	double x = bx + 14 * sc, y = by + 10 * sc;
	if (x + w > look->cards_x + look->cards_w) {
		x = bx - 14 * sc - w;
	}
	if (y + h > look->cards_y + look->cards_h) {
		y = by - 10 * sc - h;
	}
	x = fmax(look->cards_x, fmin(x, look->cards_x + look->cards_w - w));
	y = fmax(look->cards_y, fmin(y, look->cards_y + look->cards_h - h));
	pd_rounded(cr, x, y, w, h, 6 * sc);
	pd_color(cr, look->card_bg);
	cairo_fill_preserve(cr);
	pd_color(cr, look->card_border);
	cairo_set_line_width(cr, fmax(sc, 1));
	cairo_stroke(cr);
	double ty = y + pad;
	pd_text(cr, bold, name, x + pad, ty, w - 2 * pad, fmax(h1, 12 * sc), look->card_fg,
		PD_LEFT);
	g_free(name);
	ty += fmax(h1, 12 * sc);
	pd_text(cr, small, facts, x + pad, ty, w - 2 * pad, line, alpha(look->card_fg, 0.75), PD_LEFT);
	ty += line;
	bool failed = rd.failed[0] && strcmp(rd.failed, n->ssid) == 0 &&
		now_s() - rd.failed_at < FAILED_SHOWN;
	pd_text(cr, small, hint, x + pad, ty, w - 2 * pad, line,
		failed ? look->bad : n->active ? look->linked : look->sweep, PD_LEFT);
}

/* The corners of a target around a contact: hovered, or turned down. */
static void brackets(cairo_t *cr, double x, double y, double s, double len, uint32_t color,
		double width) {
	cairo_new_path(cr);
	for (int k = 0; k < 4; k++) {
		double sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
		cairo_move_to(cr, x + sx * s, y + sy * (s - len));
		cairo_line_to(cr, x + sx * s, y + sy * s);
		cairo_line_to(cr, x + sx * (s - len), y + sy * s);
	}
	pd_color(cr, color);
	cairo_set_line_width(cr, width);
	cairo_stroke(cr);
}

static void draw_frame(cairo_t *cr, const struct radar_geom *g, const struct radar_look *look,
		bool lit) {
	double sc = look->scale, R = g->R, cx = g->cx, cy = g->cy;
	double lw = fmax(sc, 1);
	if (look->disc) {
		// a screen, a little brighter towards the middle
		cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, R + 6 * sc);
		uint32_t d = look->disc;
		cairo_pattern_add_color_stop_rgba(p, 0, (d >> 24 & 0xff) / 255.0,
			(d >> 16 & 0xff) / 255.0, (d >> 8 & 0xff) / 255.0, fmin((d & 0xff) / 255.0 * 0.75, 1));
		cairo_pattern_add_color_stop_rgba(p, 1, (d >> 24 & 0xff) / 255.0,
			(d >> 16 & 0xff) / 255.0, (d >> 8 & 0xff) / 255.0, (d & 0xff) / 255.0);
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, R + 6 * sc, 0, 2 * M_PI);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	double dimmed = lit ? 1 : 0.45;
	cairo_set_line_width(cr, lw);
	for (int i = 1; i <= 4; i++) {
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, R * i / 4, 0, 2 * M_PI);
		pd_color(cr, look->mono ? look->ring : alpha(look->ring, (i == 4 ? 0.7 : 0.28) * dimmed));
		if (look->mono && i < 4) {
			double dots[] = { lw, 3 * lw };
			cairo_set_dash(cr, dots, 2, 0);
		}
		cairo_stroke(cr);
		cairo_set_dash(cr, NULL, 0, 0);
	}
	cairo_new_path(cr);
	cairo_move_to(cr, cx - R, cy);
	cairo_line_to(cr, cx + R, cy);
	cairo_move_to(cr, cx, cy - R);
	cairo_line_to(cr, cx, cy + R);
	pd_color(cr, alpha(look->ring, 0.18 * dimmed));
	cairo_stroke(cr);
	for (int d = 0; d < 360; d += 10) {
		double a = d * M_PI / 180 - M_PI / 2;
		double in = R + (d % 30 == 0 ? 2 : 4) * sc, out = R + 7 * sc;
		cairo_new_path(cr);
		cairo_move_to(cr, cx + cos(a) * in, cy + sin(a) * in);
		cairo_line_to(cr, cx + cos(a) * out, cy + sin(a) * out);
		pd_color(cr, alpha(look->ring, (d % 90 == 0 ? 1 : 0.5) * dimmed));
		cairo_set_line_width(cr, d % 90 == 0 ? 2 * lw : lw);
		cairo_stroke(cr);
	}
	if (look->bearings) {
		static const char *const bearings[] = { "000", "090", "180", "270" };
		char small[200];
		font(small, sizeof(small), look, false, 9 * sc);
		for (int i = 0; i < 4; i++) {
			double a = i * M_PI / 2 - M_PI / 2;
			double bx = cx + cos(a) * (R + 17 * sc), by = cy + sin(a) * (R + 17 * sc);
			pd_text(cr, small, bearings[i], bx - 16 * sc, by - 8 * sc, 32 * sc, 16 * sc,
				alpha(look->ring, dimmed), PD_CENTER);
		}
	}
}

/* The words in the middle while there is nothing to show: Wi-Fi off, none in reach. */
static void draw_message(cairo_t *cr, const struct radar_geom *g, const struct radar_look *look,
		const char *line1, const char *line2) {
	double sc = look->scale;
	char bold[200], small[200];
	font(bold, sizeof(bold), look, true, 13 * sc);
	font(small, sizeof(small), look, false, 10.5 * sc);
	double w = g->R * 1.8, x = g->cx - w / 2;
	double y = g->cy + g->R * 0.18;
	if (look->disc) {
		// a plate, so the words read over the rings
		double ph = (line2 ? 38 : 22) * sc;
		pd_rounded(cr, x + w * 0.08, y - 3 * sc, w * 0.84, ph, 6 * sc);
		pd_color(cr, alpha(look->card_bg, 0.85));
		cairo_fill(cr);
	}
	uint32_t fg = look->disc ? look->card_fg : look->fg;
	label(cr, look, bold, line1, x, y, w, 16 * sc, fg, PD_CENTER);
	if (line2) {
		label(cr, look, small, line2, x, y + 17 * sc, w, 14 * sc, look->disc ?
			alpha(look->card_fg, 0.7) : look->dim, PD_CENTER);
	}
}

void wifiradar_draw(cairo_t *cr, const struct radar_geom *g, const struct radar_look *look,
		int hover) {
	double now = now_s(), sc = look->scale, R = g->R, cx = g->cx, cy = g->cy;
	double lw = fmax(sc, 1);
	bool lit = rd.status == RADAR_READY || rd.status == RADAR_LOOKING;
	cairo_save(cr);
	draw_frame(cr, g, look, lit);
	if (!lit) {
		const char *a = rd.status == RADAR_OFF ? "Wi-Fi is off" :
			rd.status == RADAR_NO_DEVICE ? "No Wi-Fi adapter" : "No NetworkManager";
		const char *b = rd.status == RADAR_OFF ? "Click to turn it on" :
			rd.status == RADAR_NO_DEVICE ? NULL : "nmcli is needed to look for networks";
		draw_message(cr, g, look, a, b);
		cairo_restore(cr);
		return;
	}

	// the sweep, with its trail fading behind it
	double sweep = sweep_angle(look, now);
	const int steps = 32;
	if (look->mono) {
		// the trail as hatching, thinning out behind the sweep
		double a = sweep;
		for (int k = 1; k < 8; k++) {
			a -= 0.05 * k;
			double in = R * (0.35 + 0.08 * k);
			cairo_new_path(cr);
			cairo_move_to(cr, cx + cos(a) * in, cy + sin(a) * in);
			cairo_line_to(cr, cx + cos(a) * R, cy + sin(a) * R);
			pd_color(cr, look->sweep);
			cairo_set_line_width(cr, lw);
			cairo_stroke(cr);
		}
	}
	for (int k = 0; k < steps && !look->mono; k++) {
		double a1 = sweep - k * 0.035, a0 = a1 - 0.036;
		cairo_new_path(cr);
		cairo_move_to(cr, cx, cy);
		cairo_arc(cr, cx, cy, R, a0, a1);
		cairo_close_path(cr);
		double t = 1 - (double)k / steps;
		pd_color(cr, alpha(look->sweep, 0.42 * t * t));
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy);
	cairo_line_to(cr, cx + cos(sweep) * R, cy + sin(sweep) * R);
	pd_color(cr, look->sweep);
	cairo_set_line_width(cr, 2 * lw);
	cairo_stroke(cr);

	// the ping of a click: a ring going out from the middle
	double since_ping = now - rd.ping_at;
	if (since_ping < PING) {
		double t = since_ping / PING;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, R * t, 0, 2 * M_PI);
		pd_color(cr, alpha(look->sweep, 1 - t));
		cairo_set_line_width(cr, 3 * lw * (1 - t) + lw);
		cairo_stroke(cr);
	}

	double xs[RADAR_MAX], ys[RADAR_MAX];
	positions(g, look, now, xs, ys);
	const char *connecting = wifiradar_connecting();
	char small[200], bold_small[200];
	font(small, sizeof(small), look, false, 10.5 * sc);
	font(bold_small, sizeof(bold_small), look, true, 10.5 * sc);

	// the link to the network connected to, or being connected to: a beam to the middle
	for (int i = 0; i < rd.count; i++) {
		const struct radar_net *n = &rd.nets[i];
		bool trying = connecting && strcmp(connecting, n->ssid) == 0;
		if ((!n->active && !trying) || n->gone) {
			continue;
		}
		double dash[] = { 4 * sc, 4 * sc };
		cairo_new_path(cr);
		cairo_move_to(cr, xs[i], ys[i]);
		cairo_line_to(cr, cx, cy);
		cairo_set_dash(cr, dash, 2, fmod(now * (trying ? 30 : 10) * sc, 8 * sc) * -1);
		pd_color(cr, alpha(trying ? look->sweep : look->linked, trying ? 0.9 : 0.6));
		cairo_set_line_width(cr, (trying ? 2 : 1.5) * lw);
		cairo_stroke(cr);
		cairo_set_dash(cr, NULL, 0, 0);
		if (trying) {
			// rings going out from it while the link is made
			for (int k = 0; k < 3; k++) {
				double t = fmod(now / 1.2 + k / 3.0, 1.0);
				cairo_new_path(cr);
				cairo_arc(cr, xs[i], ys[i], (4 + 18 * t) * sc, 0, 2 * M_PI);
				pd_color(cr, alpha(look->sweep, 1 - t));
				cairo_set_line_width(cr, lw);
				cairo_stroke(cr);
			}
		}
	}

	// this computer, in the middle
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 3 * sc, 0, 2 * M_PI);
	pd_color(cr, look->fg);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 6 * sc, 0, 2 * M_PI);
	pd_color(cr, alpha(look->fg, 0.4));
	cairo_set_line_width(cr, lw);
	cairo_stroke(cr);

	// the contacts: bright where the sweep just passed, fading for a round
	for (int i = 0; i < rd.count; i++) {
		const struct radar_net *n = &rd.nets[i];
		double a = atan2(ys[i] - cy, xs[i] - cx);
		double since = fmod(sweep - a + 4 * M_PI, 2 * M_PI) / (2 * M_PI); // 0: just passed
		double fade = n->gone ? 1 - (now - n->gone) / FADE : (now - n->appeared) / FADE;
		fade = fmin(fmax(fade, 0), 1);
		if (fade <= 0) {
			continue;
		}
		bool hovered = hover == i && !n->gone;
		bool trying = connecting && strcmp(connecting, n->ssid) == 0;
		bool failed = rd.failed[0] && strcmp(rd.failed, n->ssid) == 0 &&
			now - rd.failed_at < FAILED_SHOWN;
		double glow = hovered || n->active || trying ? 1 : 0.28 + 0.72 * pow(1 - since, 2);
		uint32_t color = n->active ? look->linked : failed ? look->bad : look->blip;
		double size = (2.6 + 1.6 * n->signal / 100.0) * sc;
		if (hovered || n->active) {
			size += 1.2 * sc;
		}
		// the ping of the sweep passing it
		if (since < 0.1 && !n->gone) {
			double t = since / 0.1;
			cairo_new_path(cr);
			cairo_arc(cr, xs[i], ys[i], size + 10 * sc * t, 0, 2 * M_PI);
			pd_color(cr, alpha(color, 0.6 * (1 - t) * fade));
			cairo_set_line_width(cr, lw);
			cairo_stroke(cr);
		}
		// one that just came: a ring closing in on it
		double young = now - n->appeared;
		if (young < FADE * 1.5 && !n->gone) {
			double t = young / (FADE * 1.5);
			cairo_new_path(cr);
			cairo_arc(cr, xs[i], ys[i], size + 14 * sc * (1 - t), 0, 2 * M_PI);
			pd_color(cr, alpha(color, 1 - t));
			cairo_set_line_width(cr, lw);
			cairo_stroke(cr);
		}
		cairo_new_path(cr);
		cairo_arc(cr, xs[i], ys[i], size, 0, 2 * M_PI);
		pd_color(cr, alpha(color, glow * fade));
		cairo_fill(cr);
		if (n->known || n->active) {
			// a saved network: a ring around it
			cairo_new_path(cr);
			cairo_arc(cr, xs[i], ys[i], size + 3 * sc, 0, 2 * M_PI);
			pd_color(cr, alpha(color, 0.7 * glow * fade));
			cairo_set_line_width(cr, lw);
			cairo_stroke(cr);
		}
		if (n->secured && !n->known) {
			// a lock: a small bow over it
			cairo_new_path(cr);
			cairo_arc(cr, xs[i] + size + 3 * sc, ys[i] - size - 1 * sc, 2 * sc, M_PI, 2 * M_PI);
			pd_color(cr, alpha(color, 0.8 * glow * fade));
			cairo_set_line_width(cr, lw);
			cairo_stroke(cr);
			pd_rect(cr, xs[i] + size + 0.6 * sc, ys[i] - size - 1 * sc, 4.8 * sc, 3.4 * sc,
				alpha(color, 0.8 * glow * fade));
		}
		if (failed) {
			brackets(cr, xs[i], ys[i], size + 6 * sc, 4 * sc, look->bad, 1.5 * lw);
		} else if (hovered) {
			brackets(cr, xs[i], ys[i], size + 6 * sc, 4 * sc, look->fg, 1.5 * lw);
		}
		double joined = now - rd.joined_at;
		if (joined < JOINED_SHOWN && strcmp(rd.joined, n->ssid) == 0) {
			// connected: a ring that goes out bright
			double t = joined / JOINED_SHOWN;
			cairo_new_path(cr);
			cairo_arc(cr, xs[i], ys[i], size + 30 * sc * t, 0, 2 * M_PI);
			pd_color(cr, alpha(look->linked, 1 - t));
			cairo_set_line_width(cr, 2.5 * lw * (1 - t) + lw * 0.5);
			cairo_stroke(cr);
		}
		// the name, while it is lit
		double named = hovered || n->active || trying || failed ? 1 :
			glow > 0.72 ? (glow - 0.72) / 0.28 : 0;
		if (named > 0 && !hovered) {
			const char *f = n->active || trying ? bold_small : small;
			int tw = 0;
			char *up = look->upper ? g_utf8_strup(n->ssid, -1) : NULL;
			pd_text_size(cr, f, up ? up : n->ssid, &tw, NULL);
			g_free(up);
			tw = (int)fmin(tw, R);
			double lx = xs[i] + size + 5 * sc;
			if (lx + tw > look->cards_x + look->cards_w) {
				lx = xs[i] - size - 5 * sc - tw;
			}
			label(cr, look, f, n->ssid, lx, ys[i] - 8 * sc, tw, 16 * sc,
				alpha(n->active ? look->linked : failed ? look->bad : look->fg,
					named * fade * (n->active || trying ? 1 : 0.85)), PD_LEFT);
		}
	}
	if (rd.count == 0) {
		draw_message(cr, g, look, rd.status == RADAR_LOOKING ? "Looking for networks..." :
			"No networks in reach", NULL);
	}
	if (hover >= 0 && hover < rd.count && !rd.nets[hover].gone) {
		draw_card(cr, look, &rd.nets[hover], xs[hover], ys[hover]);
	}
	cairo_restore(cr);
}

/* ---------- the taskbar's small radar ---------- */

void wifiradar_draw_icon(cairo_t *cr, double x, double y, double s, uint32_t fg, uint32_t sweep,
		bool square) {
	double cx = x + s / 2, cy = y + s / 2, R = s * 0.46, lw = fmax(s / 16, 1);
	cairo_save(cr);
	cairo_set_line_width(cr, lw);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
	pd_color(cr, fg);
	cairo_stroke(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, R * 0.5, 0, 2 * M_PI);
	pd_color(cr, alpha(fg, 0.5));
	cairo_stroke(cr);
	// the sweep, standing at half past one
	double a = -M_PI / 4;
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy);
	cairo_arc_negative(cr, cx, cy, R, a, a - 0.9);
	cairo_close_path(cr);
	pd_color(cr, alpha(sweep, 0.45));
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy);
	cairo_line_to(cr, cx + cos(a) * R, cy + sin(a) * R);
	pd_color(cr, sweep);
	cairo_stroke(cr);
	// the strongest few, where they are on the big one
	bool used[RADAR_MAX] = { false };
	for (int shown = 0; shown < 5; shown++) {
		int best = -1;
		for (int i = 0; i < rd.count; i++) {
			if (!used[i] && !rd.nets[i].gone &&
					(best < 0 || rd.nets[i].signal > rd.nets[best].signal)) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		used[best] = true;
		const struct radar_net *n = &rd.nets[best];
		double r = R * n->r_to * 0.92, b = n->angle - M_PI / 2;
		double d = fmax(s / 9, 1.5);
		if (square) {
			pd_rect(cr, round(cx + cos(b) * r - d / 2), round(cy + sin(b) * r - d / 2), d, d,
				n->active ? sweep : fg);
		} else {
			cairo_new_path(cr);
			cairo_arc(cr, cx + cos(b) * r, cy + sin(b) * r, d / 2, 0, 2 * M_PI);
			pd_color(cr, n->active ? sweep : fg);
			cairo_fill(cr);
		}
	}
	cairo_restore(cr);
}
