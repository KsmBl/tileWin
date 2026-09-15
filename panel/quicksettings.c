/*
 * Quick settings, like on Windows 11 (Win+A or "panel quicksettings"): Wi-Fi,
 * Bluetooth, airplane mode, do not disturb, night light and tile mode
 * buttons, volume and brightness sliders and the battery. With the theme key
 * panel.quick_settings the network, volume and battery icons open it too.
 *
 * The state is read when it opens (nmcli and pactl in one shell, sysfs) and
 * after changes; nothing runs while it is closed.
 */
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "flyout.h"
#include "popup.h"
#include "tw_paths.h"

#define QS_WIDTH 360
#define PAD 16
#define TILE_H 48
#define LABEL_H 24
#define TILE_GAP 8
#define MORE_W 30
#define SLIDER_ROW 44
#define FOOTER_H 48

enum qs_tile {
	QT_WIFI,
	QT_BLUETOOTH,
	QT_AIRPLANE,
	QT_DND,
	QT_NIGHT,
	QT_TILE,
	QT_COUNT,
};

enum qs_drag {
	QD_NONE,
	QD_VOLUME,
	QD_BRIGHTNESS,
};

struct qs {
	struct panel *panel;
	struct popup *popup;
	struct popup_anchor anchor;
	double px, py;
	bool inside;

	bool have_nmcli, wifi_on;
	char ssid[128];
	bool audio, muted;
	int volume;
	bool backlight;
	int brightness;
	bool battery, charging;
	int capacity;

	struct pbox tiles[QT_COUNT], more[QT_COUNT];
	struct pbox volume_icon, volume_slider, volume_more, brightness_slider;
	struct pbox battery_box, settings_box;

	enum qs_drag drag;
	int drag_value;
	bool drag_dirty;
	struct loop_timer *drag_timer, *requery_timer;
	struct proc *proc;
};

static struct qs *qs_current = NULL;

/* ---------- state ---------- */

static bool read_file(const char *path, char *buf, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	size_t n = fread(buf, 1, size - 1, f);
	fclose(f);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
		n--;
	}
	buf[n] = '\0';
	return true;
}

static void read_sysfs(struct qs *q) {
	q->battery = false;
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *de;
	while (dir && (de = readdir(dir))) {
		char path[512], buf[64];
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
		if (de->d_name[0] == '.' || !read_file(path, buf, sizeof(buf)) ||
				strcmp(buf, "Battery") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
		if (!read_file(path, buf, sizeof(buf))) {
			continue;
		}
		q->battery = true;
		q->capacity = atoi(buf);
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
		q->charging = read_file(path, buf, sizeof(buf)) && strcmp(buf, "Charging") == 0;
		break;
	}
	if (dir) {
		closedir(dir);
	}
	if (q->drag == QD_BRIGHTNESS) {
		return;
	}
	q->backlight = false;
	dir = opendir("/sys/class/backlight");
	while (dir && (de = readdir(dir))) {
		char path[512], cur[32], max[32];
		snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", de->d_name);
		bool ok = de->d_name[0] != '.' && read_file(path, cur, sizeof(cur));
		snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", de->d_name);
		if (ok && read_file(path, max, sizeof(max)) && atol(max) > 0) {
			q->backlight = true;
			q->brightness = (int)((atol(cur) * 100 + atol(max) / 2) / atol(max));
			break;
		}
	}
	if (dir) {
		closedir(dir);
	}
}

/* Airplane mode: every radio is soft blocked. */
static bool airplane_state(bool *available) {
	DIR *dir = opendir("/sys/class/rfkill");
	struct dirent *de;
	int radios = 0, blocked = 0;
	while (dir && (de = readdir(dir))) {
		char path[512], buf[16];
		snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", de->d_name);
		if (de->d_name[0] != '.' && read_file(path, buf, sizeof(buf))) {
			radios++;
			blocked += atoi(buf) == 1;
		}
	}
	if (dir) {
		closedir(dir);
	}
	*available = radios > 0;
	return radios > 0 && blocked == radios;
}

static char *nightlight_state_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = malloc(strlen(dir) + 16);
	sprintf(path, "%s/nightlight", dir);
	free(dir);
	return path;
}

static bool nightlight_on(void) {
	char *path = nightlight_state_path();
	char buf[8] = "";
	bool on = path && read_file(path, buf, sizeof(buf)) && strcmp(buf, "on") == 0;
	free(path);
	return on;
}

static bool tile_mode(struct panel *panel) {
	return panel->state.mode && strcmp(panel->state.mode, "tile") == 0;
}

static const char query_script[] =
	"if command -v nmcli >/dev/null 2>&1; then echo nmcli=1; "
	"echo wifi=$(nmcli -t -f WIFI radio 2>/dev/null); "
	"nmcli -t -f ACTIVE,SSID dev wifi list --rescan no 2>/dev/null | "
	"sed -n 's/^yes:/ssid=/p' | head -n 1; fi; "
	"v=$(pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null) && "
	"echo \"volume=$(printf '%s' \"$v\" | grep -o '[0-9]*%' | head -n 1)\" && "
	"pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null | sed -n 's/.*: /mute=/p'";

static void unescape_nmcli(char *s) {
	char *o = s;
	for (char *p = s; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
		}
		*o++ = *p;
	}
	*o = '\0';
}

static int qs_height(struct qs *q);

static void qs_resize(struct qs *q) {
	int M = popup_shadow_margin(q->panel);
	int width = QS_WIDTH + 2 * M, height = qs_height(q) + 2 * M;
	if (height != q->popup->height) {
		int x = q->anchor.right_align ? q->anchor.x - width + M : q->anchor.x - M;
		int y = q->anchor.above ? q->anchor.y - height : q->anchor.y;
		popup_move_resize(q->popup, x, y, width, height);
	}
	popup_set_dirty(q->popup);
}

static void query_done(void *data, const char *output) {
	struct qs *q = qs_current;
	if (!q) {
		return;
	}
	q->proc = NULL;
	q->have_nmcli = false;
	q->ssid[0] = '\0';
	bool audio = false;
	char *copy = strdup(output ? output : "");
	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "nmcli=", 6) == 0) {
			q->have_nmcli = true;
		} else if (strncmp(line, "wifi=", 5) == 0) {
			q->wifi_on = strcmp(line + 5, "enabled") == 0;
		} else if (strncmp(line, "ssid=", 5) == 0) {
			unescape_nmcli(line + 5);
			snprintf(q->ssid, sizeof(q->ssid), "%s", line + 5);
		} else if (strncmp(line, "volume=", 7) == 0 && q->drag != QD_VOLUME) {
			audio = line[7] != '\0';
			q->volume = atoi(line + 7);
		} else if (strncmp(line, "mute=", 5) == 0) {
			q->muted = strcmp(line + 5, "yes") == 0;
		}
	}
	free(copy);
	if (q->drag != QD_VOLUME) {
		q->audio = audio;
	}
	read_sysfs(q);
	qs_resize(q);
}

static void query(struct qs *q) {
	if (!q->proc) {
		q->proc = proc_run(q->panel, query_script, false, NULL, query_done, NULL);
	}
}

static void requery_fired(void *data) {
	struct qs *q = qs_current;
	if (q) {
		q->requery_timer = NULL;
		query(q);
	}
}

static void query_later(struct qs *q, int ms) {
	if (q->requery_timer) {
		loop_remove_timer(q->panel->loop, q->requery_timer);
	}
	q->requery_timer = loop_add_timer(q->panel->loop, ms, requery_fired, NULL);
}

void quicksettings_redraw(struct panel *panel) {
	if (qs_current && qs_current->panel == panel) {
		popup_set_dirty(qs_current->popup);
	}
}

void quicksettings_changed(struct panel *panel) {
	struct qs *q = qs_current;
	if (q && q->panel == panel) {
		if (q->drag == QD_NONE) {
			query_later(q, 150);
		}
		popup_set_dirty(q->popup);
	}
}

/* ---------- drawing ---------- */

static bool qs_hovered(struct qs *q, struct pbox b) {
	return q->inside && pbox_contains(&b, q->px, q->py);
}

static void draw_airplane(cairo_t *cr, double x, double y, double s, uint32_t color) {
	static const double half[][2] = {
		{ 0, 0.04 }, { 0.07, 0.1 }, { 0.08, 0.36 }, { 0.47, 0.6 }, { 0.47, 0.7 },
		{ 0.08, 0.57 }, { 0.07, 0.8 }, { 0.2, 0.9 }, { 0.2, 0.97 }, { 0, 0.92 },
	};
	size_t n = sizeof(half) / sizeof(half[0]);
	double cx = x + s / 2;
	cairo_new_path(cr);
	for (size_t i = 0; i < n; i++) {
		cairo_line_to(cr, cx + half[i][0] * s, y + half[i][1] * s);
	}
	for (size_t i = n; i-- > 0;) {
		cairo_line_to(cr, cx - half[i][0] * s, y + half[i][1] * s);
	}
	cairo_close_path(cr);
	pd_color(cr, color);
	cairo_fill(cr);
}

static void draw_moon(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_push_group(cr);
	cairo_new_path(cr);
	cairo_arc(cr, x + s * 0.5, y + s * 0.5, s * 0.4, 0, 2 * M_PI);
	pd_color(cr, color);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_arc(cr, x + s * 0.72, y + s * 0.32, s * 0.34, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
}

static void draw_gear(cairo_t *cr, double cx, double cy, double r, uint32_t color) {
	cairo_new_path(cr);
	pd_color(cr, color);
	cairo_set_line_width(cr, r * 0.26);
	cairo_arc(cr, cx, cy, r * 0.5, 0, 2 * M_PI);
	cairo_stroke(cr);
	for (int i = 0; i < 8; i++) {
		double a = i * M_PI / 4;
		cairo_move_to(cr, cx + cos(a) * r * 0.62, cy + sin(a) * r * 0.62);
		cairo_line_to(cr, cx + cos(a) * r, cy + sin(a) * r);
	}
	cairo_set_line_width(cr, r * 0.3);
	cairo_stroke(cr);
}

static void draw_chevron(cairo_t *cr, double cx, double cy, double s, uint32_t color) {
	cairo_new_path(cr);
	cairo_move_to(cr, cx - s * 0.2, cy - s * 0.4);
	cairo_line_to(cr, cx + s * 0.2, cy);
	cairo_line_to(cr, cx - s * 0.2, cy + s * 0.4);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.4);
	cairo_stroke(cr);
}

static void draw_tile_icon(struct qs *q, cairo_t *cr, enum qs_tile t, double x, double y,
		double s, uint32_t color) {
	switch (t) {
	case QT_WIFI:
		ti_network(q->panel, cr, x, y, s, 4, true, true, color);
		break;
	case QT_BLUETOOTH:
		draw_bluetooth_glyph(cr, x, y, s, color);
		break;
	case QT_AIRPLANE:
		draw_airplane(cr, x, y, s, color);
		break;
	case QT_DND:
		notify_draw_bell(cr, x, y, s, color, true);
		break;
	case QT_NIGHT:
		draw_moon(cr, x, y, s, color);
		break;
	case QT_TILE:
		pd_glyph_tile(cr, x, y, s, color);
		break;
	case QT_COUNT:
		break;
	}
}

struct tile_state {
	bool on, enabled, has_more;
	const char *label;
};

static void tile_state(struct qs *q, enum qs_tile t, struct tile_state *ts) {
	bool available;
	ts->has_more = false;
	ts->enabled = true;
	switch (t) {
	case QT_WIFI:
		ts->on = q->have_nmcli && q->wifi_on;
		ts->enabled = q->have_nmcli;
		ts->has_more = q->have_nmcli;
		ts->label = q->ssid[0] && ts->on ? q->ssid : "Wi-Fi";
		break;
	case QT_BLUETOOTH:
		ts->on = bt_powered();
		ts->enabled = bt_available();
		ts->has_more = bt_available();
		ts->label = "Bluetooth";
		for (int i = 0; ts->on && bt_devices() && i < bt_devices()->length; i++) {
			struct bt_device *d = bt_devices()->items[i];
			if (d->connected && d->name) {
				ts->label = d->name;
				break;
			}
		}
		break;
	case QT_AIRPLANE:
		ts->on = airplane_state(&available);
		ts->enabled = available;
		ts->label = "Airplane mode";
		break;
	case QT_DND:
		ts->on = notify_dnd();
		ts->label = "Do not disturb";
		break;
	case QT_NIGHT:
		ts->on = nightlight_on();
		ts->label = "Night light";
		break;
	case QT_TILE:
		ts->on = tile_mode(q->panel);
		ts->label = "Tile mode";
		break;
	case QT_COUNT:
		break;
	}
}

static void draw_tile(struct qs *q, cairo_t *cr, const struct fly_style *st, enum qs_tile t,
		struct pbox b) {
	struct tile_state ts;
	tile_state(q, t, &ts);
	q->tiles[t] = b;
	q->more[t] = (struct pbox){ 0 };
	if (ts.has_more) {
		q->more[t] = (struct pbox){ b.x + b.width - MORE_W, b.y, MORE_W, b.height };
	}
	bool hover = ts.enabled && qs_hovered(q, b);
	uint32_t icon_color = ts.on && st->style != PS_CLASSIC ? 0xffffffff : st->fg;
	if (!ts.enabled) {
		icon_color = st->dim;
	}
	if (st->style == PS_CLASSIC) {
		pd_rect(cr, b.x, b.y, b.width, b.height, ts.on ? 0xdfdfdfff : 0xc0c0c0ff);
		pd_bevel(cr, b.x, b.y, b.width, b.height, ts.on);
		if (ts.has_more) {
			pd_rect(cr, q->more[t].x, b.y + 6, 1, b.height - 12, 0x808080ff);
			pd_rect(cr, q->more[t].x + 1, b.y + 6, 1, b.height - 12, 0xffffffff);
		}
	} else {
		double r = st->style == PS_FLUENT ? 6 : st->style == PS_FLAT ? 0 : 4;
		cairo_new_path(cr);
		pd_rounded(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1, r);
		pd_color(cr, ts.on ? st->accent : st->button_bg);
		cairo_fill_preserve(cr);
		pd_color(cr, ts.on ? st->accent : st->button_border);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		if (hover) {
			struct pbox hb = ts.has_more && qs_hovered(q, q->more[t]) ? q->more[t] :
				ts.has_more ? (struct pbox){ b.x, b.y, b.width - MORE_W, b.height } : b;
			cairo_new_path(cr);
			pd_rounded(cr, hb.x + 1, hb.y + 1, hb.width - 2, hb.height - 2, r);
			pd_color(cr, ts.on ? 0xffffff30 : st->button_hover);
			cairo_fill(cr);
		}
		if (ts.has_more) {
			pd_rect(cr, q->more[t].x, b.y + 8, 1, b.height - 16,
				ts.on ? 0xffffff60 : st->button_border);
		}
	}
	double icon = 18;
	double area = b.width - (ts.has_more ? MORE_W : 0);
	draw_tile_icon(q, cr, t, b.x + (area - icon) / 2, b.y + (b.height - icon) / 2, icon,
		icon_color);
	if (ts.has_more) {
		draw_chevron(cr, q->more[t].x + MORE_W / 2.0, b.y + b.height / 2.0, 12, icon_color);
	}
	pd_text(cr, st->font, ts.label, b.x - 2, b.y + b.height + 2, b.width + 4, LABEL_H - 4,
		ts.enabled ? st->fg : st->dim, PD_CENTER);
}

static int qs_height(struct qs *q) {
	return PAD + 2 * (TILE_H + LABEL_H) + TILE_GAP + 8 + SLIDER_ROW +
		(q->backlight ? SLIDER_ROW : 0) + 8 + FOOTER_H;
}

static void qs_render(struct popup *p, cairo_t *cr) {
	struct qs *q = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int tw = (cw - 2 * TILE_GAP) / 3;
	int y = M + PAD;
	for (int t = 0; t < QT_COUNT; t++) {
		int row = t / 3, col = t % 3;
		struct pbox b = { x0 + col * (tw + TILE_GAP), y + row * (TILE_H + LABEL_H + TILE_GAP),
			tw, TILE_H };
		draw_tile(q, cr, &st, t, b);
	}
	y += 2 * (TILE_H + LABEL_H) + TILE_GAP + 8;

	// volume
	q->volume_icon = (struct pbox){ x0 - 6, y + (SLIDER_ROW - 32) / 2, 32, 32 };
	q->volume_more = (struct pbox){ x0 + cw - 28, y + (SLIDER_ROW - 32) / 2, 28, 32 };
	q->volume_slider = (struct pbox){ x0 + 32, y, cw - 32 - 34, SLIDER_ROW };
	if (q->audio) {
		if (qs_hovered(q, q->volume_icon)) {
			fill_hover(cr, &st, q->volume_icon);
		}
		ti_speaker(p->panel, cr, x0, y + (SLIDER_ROW - 20) / 2.0, 20,
			q->drag == QD_VOLUME ? q->drag_value : q->volume, q->muted, st.fg);
		draw_slider(cr, &st, q->volume_slider,
			q->drag == QD_VOLUME ? q->drag_value : q->volume);
		if (qs_hovered(q, q->volume_more)) {
			fill_hover(cr, &st, q->volume_more);
		}
		draw_chevron(cr, q->volume_more.x + 14, y + SLIDER_ROW / 2.0, 12, st.fg);
	} else {
		ti_speaker(p->panel, cr, x0, y + (SLIDER_ROW - 20) / 2.0, 20, 0, true, st.dim);
		pd_text(cr, st.font, "No sound device", x0 + 40, y, cw - 40, SLIDER_ROW, st.dim,
			PD_LEFT);
	}
	y += SLIDER_ROW;

	// brightness
	q->brightness_slider = (struct pbox){ 0 };
	if (q->backlight) {
		ti_brightness(p->panel, cr, x0, y + (SLIDER_ROW - 20) / 2.0, 20, st.fg);
		q->brightness_slider = (struct pbox){ x0 + 32, y, cw - 32 - 34, SLIDER_ROW };
		draw_slider(cr, &st, q->brightness_slider,
			q->drag == QD_BRIGHTNESS ? q->drag_value : q->brightness);
		y += SLIDER_ROW;
	}

	// footer: battery and settings
	int fy = H - M - FOOTER_H;
	pd_rect(cr, M, fy, W - 2 * M, FOOTER_H, st.dark ? 0x00000030 : 0x00000008);
	draw_line(cr, &st, p, fy);
	q->battery_box = (struct pbox){ 0 };
	if (q->battery) {
		char text[16];
		snprintf(text, sizeof(text), "%d%%", q->capacity);
		int tw2 = 0;
		pd_text_size(cr, st.font, text, &tw2, NULL);
		q->battery_box = (struct pbox){ x0 - 6, fy + 8, 20 + 8 + tw2 + 12, FOOTER_H - 16 };
		if (qs_hovered(q, q->battery_box)) {
			fill_hover(cr, &st, q->battery_box);
		}
		ti_battery(p->panel, cr, x0, fy + (FOOTER_H - 20) / 2.0, 20, q->capacity, q->charging,
			st.fg);
		pd_text(cr, st.font, text, x0 + 28, fy, tw2 + 4, FOOTER_H, st.fg, PD_LEFT);
	}
	q->settings_box = (struct pbox){ x0 + cw - 32, fy + 8, 36, FOOTER_H - 16 };
	if (qs_hovered(q, q->settings_box)) {
		fill_hover(cr, &st, q->settings_box);
	}
	draw_gear(cr, q->settings_box.x + 18, fy + FOOTER_H / 2.0, 8, st.fg);
}

/* ---------- input ---------- */

static void apply_drag(struct qs *q, bool now);

static void drag_timer_fired(void *data) {
	struct qs *q = qs_current;
	if (!q) {
		return;
	}
	q->drag_timer = NULL;
	if (q->drag_dirty) {
		apply_drag(q, false);
	}
}

/* At most every 60 ms while dragging. */
static void apply_drag(struct qs *q, bool now) {
	if (!now && q->drag_timer) {
		q->drag_dirty = true;
		return;
	}
	q->drag_dirty = false;
	char cmd[96];
	if (q->drag == QD_VOLUME) {
		snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%%", q->drag_value);
		q->volume = q->drag_value;
	} else if (q->drag == QD_BRIGHTNESS) {
		int value = q->drag_value < 1 ? 1 : q->drag_value; // never turn the screen off
		snprintf(cmd, sizeof(cmd), "brightnessctl -q set %d%%", value);
		q->brightness = value;
	} else {
		return;
	}
	proc_spawn(cmd);
	if (!now) {
		q->drag_timer = loop_add_timer(q->panel->loop, 60, drag_timer_fired, NULL);
	}
}

enum next_flyout {
	NEXT_NETWORK,
	NEXT_VOLUME,
	NEXT_POWER,
	NEXT_BLUETOOTH,
};

static struct {
	enum next_flyout kind;
	struct panel *panel;
	struct popup_anchor anchor;
	struct loop_timer *timer;
} next;

static void next_fired(void *data) {
	next.timer = NULL;
	switch (next.kind) {
	case NEXT_NETWORK:
		flyout_network_toggle(next.panel, next.anchor, NULL);
		break;
	case NEXT_VOLUME:
		flyout_volume_toggle(next.panel, next.anchor, "exec pavucontrol");
		break;
	case NEXT_POWER:
		flyout_power_toggle(next.panel, next.anchor, NULL);
		break;
	case NEXT_BLUETOOTH:
		flyout_bluetooth_toggle(next.panel, next.anchor);
		break;
	}
}

/* Another flyout replaces this one after the input handler returns. */
static void open_next(struct qs *q, enum next_flyout kind) {
	next.kind = kind;
	next.panel = q->panel;
	next.anchor = q->anchor;
	if (!next.timer) {
		next.timer = loop_add_timer(q->panel->loop, 0, next_fired, NULL);
	}
}

static void toggle_tile(struct qs *q, enum qs_tile t) {
	struct tile_state ts;
	tile_state(q, t, &ts);
	if (!ts.enabled) {
		return;
	}
	switch (t) {
	case QT_WIFI:
		proc_spawn(ts.on ? "nmcli radio wifi off" : "nmcli radio wifi on");
		q->wifi_on = !ts.on;
		query_later(q, 1500);
		break;
	case QT_BLUETOOTH:
		bt_set_powered(!ts.on);
		break;
	case QT_AIRPLANE:
		proc_spawn(ts.on ? "rfkill unblock all" : "rfkill block all");
		query_later(q, 800);
		break;
	case QT_DND:
		notify_set_dnd(q->panel, !ts.on);
		break;
	case QT_NIGHT: {
		char *path = nightlight_state_path();
		if (path) {
			tw_write_string(path, ts.on ? "off\n" : "on\n");
			free(path);
		}
		break;
	}
	case QT_TILE:
		ipc_panel_command(q->panel, "wm_mode toggle");
		popup_close_later(q->panel);
		break;
	case QT_COUNT:
		break;
	}
	popup_set_dirty(q->popup);
}

static void qs_motion(struct popup *p, double x, double y) {
	struct qs *q = p->data;
	q->px = x;
	q->py = y;
	q->inside = true;
	if (q->drag != QD_NONE) {
		struct pbox b = q->drag == QD_VOLUME ? q->volume_slider : q->brightness_slider;
		q->drag_value = slider_value(b, x);
		apply_drag(q, false);
	}
	popup_set_dirty(p);
}

static void qs_leave(struct popup *p) {
	struct qs *q = p->data;
	q->inside = false;
	popup_set_dirty(p);
}

static void qs_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct qs *q = p->data;
	if (button != BTN_LEFT) {
		return;
	}
	if (!pressed) {
		if (q->drag != QD_NONE) {
			apply_drag(q, true);
			q->drag = QD_NONE;
			query_later(q, 300);
			popup_set_dirty(p);
		}
		return;
	}
	for (int t = 0; t < QT_COUNT; t++) {
		if (q->more[t].width && pbox_contains(&q->more[t], x, y)) {
			open_next(q, t == QT_WIFI ? NEXT_NETWORK : NEXT_BLUETOOTH);
			return;
		}
		if (pbox_contains(&q->tiles[t], x, y)) {
			toggle_tile(q, t);
			return;
		}
	}
	if (q->audio && pbox_contains(&q->volume_icon, x, y)) {
		proc_spawn("pactl set-sink-mute @DEFAULT_SINK@ toggle");
		q->muted = !q->muted;
		query_later(q, 300);
		popup_set_dirty(p);
	} else if (q->audio && pbox_contains(&q->volume_more, x, y)) {
		open_next(q, NEXT_VOLUME);
	} else if (q->audio && pbox_contains(&q->volume_slider, x, y)) {
		q->drag = QD_VOLUME;
		q->drag_value = slider_value(q->volume_slider, x);
		apply_drag(q, true);
		popup_set_dirty(p);
	} else if (q->backlight && pbox_contains(&q->brightness_slider, x, y)) {
		q->drag = QD_BRIGHTNESS;
		q->drag_value = slider_value(q->brightness_slider, x);
		apply_drag(q, true);
		popup_set_dirty(p);
	} else if (q->battery_box.width && pbox_contains(&q->battery_box, x, y)) {
		open_next(q, NEXT_POWER);
	} else if (pbox_contains(&q->settings_box, x, y)) {
		bar_run_command(q->panel, "exec tilewin-settings", NULL);
		popup_close_later(q->panel);
	}
}

static void qs_axis(struct popup *p, double x, double y, int direction) {
	struct qs *q = p->data;
	enum qs_drag target = q->backlight && pbox_contains(&q->brightness_slider, x, y) ?
		QD_BRIGHTNESS : q->audio ? QD_VOLUME : QD_NONE;
	if (target == QD_NONE || q->drag != QD_NONE) {
		return;
	}
	int current = target == QD_VOLUME ? q->volume : q->brightness;
	q->drag = target;
	q->drag_value = current + (direction < 0 ? 5 : -5);
	q->drag_value = q->drag_value < 0 ? 0 : q->drag_value > 100 ? 100 : q->drag_value;
	apply_drag(q, true);
	q->drag = QD_NONE;
	query_later(q, 300);
	popup_set_dirty(p);
}

static void qs_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void qs_destroy(struct popup *p) {
	struct qs *q = p->data;
	if (q->proc) {
		proc_cancel(q->proc);
	}
	if (q->drag_timer) {
		loop_remove_timer(q->panel->loop, q->drag_timer);
	}
	if (q->requery_timer) {
		loop_remove_timer(q->panel->loop, q->requery_timer);
	}
	if (qs_current == q) {
		qs_current = NULL;
	}
	free(q);
}

static const struct popup_vtable qs_vtable = {
	.render = qs_render,
	.motion = qs_motion,
	.leave = qs_leave,
	.button = qs_button,
	.axis = qs_axis,
	.key = qs_key,
	.destroy = qs_destroy,
};

void quicksettings_toggle(struct panel *panel, struct popup_anchor anchor) {
	if (popup_is_open(panel, POPUP_QUICKSETTINGS)) {
		popup_close_all(panel);
		return;
	}
	if (!anchor.output) {
		return;
	}
	struct qs *q = calloc(1, sizeof(*q));
	q->panel = panel;
	q->anchor = anchor;
	q->volume = 50;
	read_sysfs(q);
	int M = popup_shadow_margin(panel);
	int width = QS_WIDTH + 2 * M, height = qs_height(q) + 2 * M;
	int x = anchor.right_align ? anchor.x - width + M : anchor.x - M;
	int y = anchor.above ? anchor.y - height : anchor.y;
	q->popup = popup_create(panel, POPUP_QUICKSETTINGS, NULL, anchor.output, x, y, width,
		height, &qs_vtable, q);
	if (!q->popup) {
		free(q);
		return;
	}
	qs_current = q;
	query(q);
}
