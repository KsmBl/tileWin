/*
 * A moving wallpaper: a theme that has one ("live_wallpaper { scene console }")
 * gets, over its own still wallpaper, a few panels of the desktop that move and
 * tell how the computer is. The Starship Console's:
 *
 *   - a sensor: the Wi-Fi radar (wifiradar.c), rings with a sweep going
 *     round and the networks in reach as contacts; the pointer on a contact
 *     shows its card, a click connects to it;
 *   - a panel of readouts: the time, the date, the processor (and its last
 *     minute), memory, disk, network, battery, temperature, uptime and the
 *     windows open; a row lights up under the pointer, and a click opens what
 *     it is about (the CPU, memory, network or power flyout, the calendar).
 *
 * Each is a small surface of its own under the desktop's icons, so it costs
 * little: the sweep is drawn some twelve times a second into a few hundred
 * pixels, the readouts once a second, and when windows cover them the
 * compositor stops asking for frames and they stop too. The battery saver
 * slows the sweep down. They show only where the theme's own wallpaper is (not
 * over a picture of one's own), and "live_wallpaper no" in taskbar.conf (the
 * Wallpaper page) keeps them away. The pointer reaches them through the
 * desktop's surface (desktop.c), which lies over them.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "ipc-client.h"
#include "log.h"
#include "panel.h"
#include "tw_battery.h"
#include "wifiradar.h"

#define SWEEP_MS 80          // a frame of the sweep, some twelve a second
#define SWEEP_SAVER_MS 500   // with the battery saver on
#define SWEEP_TURN 6.0       // seconds for a round
#define SAMPLE_MS 1000
#define HISTORY 60
#define READOUT_W 380
#define ROW_H 24

enum part { PART_SENSOR, PART_READOUT, PART_COUNT };

struct row {
	const char *label;
	char value[96];
	double bar;          // 0 to 1, below 0 for none
	const char *command; // what a click opens
	bool shown;
	bool full_is_good;   // a battery: low is what warns, not high
};

enum { ROW_TIME, ROW_DATE, ROW_CPU, ROW_MEM, ROW_DISK, ROW_NET, ROW_BAT, ROW_TEMP, ROW_UPTIME,
	ROW_WINDOWS, ROW_COUNT };

/* The surfaces of one screen. */
struct live_output {
	struct panel_output *output;
	struct psurface *parts[PART_COUNT];
	struct pbox boxes[PART_COUNT]; // where they are, in the screen's own pixels
	int hover_row, hover_contact;
	struct wl_list link;
};

static struct {
	struct panel *panel;
	struct wl_list outputs; // struct live_output
	bool inited;
	char *scene;
	struct loop_timer *sweep_timer, *sample_timer;
	bool radar_held;
	// readings
	unsigned long long cpu_total, cpu_idle;
	double cpu, cpu_history[HISTORY];
	int history_len;
	unsigned long long net_rx, net_tx;
	double rx_rate, tx_rate;
	bool rx_lamp, tx_lamp;
	double disk_used;
	int samples;
	struct row rows[ROW_COUNT];
} lw;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- readings ---------- */

static void read_cpu(void) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return;
	}
	unsigned long long v[8] = { 0 };
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
			&v[4], &v[5], &v[6], &v[7]) >= 4) {
		unsigned long long total = 0;
		for (int i = 0; i < 8; i++) {
			total += v[i];
		}
		unsigned long long idle = v[3] + v[4];
		if (lw.cpu_total && total > lw.cpu_total) {
			lw.cpu = 1.0 - (double)(idle - lw.cpu_idle) / (double)(total - lw.cpu_total);
			lw.cpu = lw.cpu < 0 ? 0 : lw.cpu > 1 ? 1 : lw.cpu;
			if (lw.history_len == HISTORY) {
				memmove(lw.cpu_history, lw.cpu_history + 1, (HISTORY - 1) * sizeof(double));
				lw.history_len--;
			}
			lw.cpu_history[lw.history_len++] = lw.cpu;
		}
		lw.cpu_total = total;
		lw.cpu_idle = idle;
	}
	fclose(f);
}

static void read_net(double seconds) {
	FILE *f = fopen("/proc/net/dev", "r");
	if (!f) {
		return;
	}
	char line[512];
	unsigned long long rx = 0, tx = 0;
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':');
		if (!colon) {
			continue;
		}
		*colon = '\0';
		char *name = line;
		while (*name == ' ') {
			name++;
		}
		if (strcmp(name, "lo") == 0) {
			continue;
		}
		unsigned long long r, t, skip;
		if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &r, &skip, &skip,
				&skip, &skip, &skip, &skip, &skip, &t) == 9) {
			rx += r;
			tx += t;
		}
	}
	fclose(f);
	if (lw.net_rx && seconds > 0) {
		lw.rx_rate = rx >= lw.net_rx ? (rx - lw.net_rx) / seconds : 0;
		lw.tx_rate = tx >= lw.net_tx ? (tx - lw.net_tx) / seconds : 0;
	}
	lw.rx_lamp = lw.rx_rate > 2048;
	lw.tx_lamp = lw.tx_rate > 2048;
	lw.net_rx = rx;
	lw.net_tx = tx;
}

static void rate_text(double bytes, char *out, size_t size) {
	if (bytes >= 1024.0 * 1024) {
		snprintf(out, size, "%.1f MB/s", bytes / 1024 / 1024);
	} else if (bytes >= 1024) {
		snprintf(out, size, "%.0f kB/s", bytes / 1024);
	} else {
		snprintf(out, size, "%.0f B/s", bytes);
	}
}

/* The warmest thermal zone, in degrees; below 0 for none. */
static double read_temp(void) {
	double best = -1;
	for (int i = 0; i < 16; i++) {
		char path[96];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		long milli;
		if (fscanf(f, "%ld", &milli) == 1 && milli > 0 && milli < 150000) {
			best = fmax(best, milli / 1000.0);
		}
		fclose(f);
	}
	return best;
}

static void set_row(int i, const char *label, const char *command) {
	lw.rows[i].label = label;
	lw.rows[i].command = command;
	lw.rows[i].bar = -1;
	lw.rows[i].shown = true;
	lw.rows[i].full_is_good = i == ROW_BAT;
}

static void sample(void) {
	static double last;
	double now = now_s();
	double seconds = last > 0 ? now - last : 1;
	last = now;
	lw.samples++;
	read_cpu();
	read_net(seconds);

	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char buf[96];
	set_row(ROW_TIME, "TIME", "panel calendar");
	strftime(lw.rows[ROW_TIME].value, sizeof(lw.rows[ROW_TIME].value), "%H:%M:%S", &tm);
	set_row(ROW_DATE, "DATE", "panel calendar");
	strftime(buf, sizeof(buf), "%a %d %b %Y", &tm);
	char *upper = g_utf8_strup(buf, -1);
	snprintf(lw.rows[ROW_DATE].value, sizeof(lw.rows[ROW_DATE].value), "%s · DAY %d", upper,
		tm.tm_yday + 1);
	g_free(upper);

	set_row(ROW_CPU, "PROCESSOR", "panel cpu");
	snprintf(lw.rows[ROW_CPU].value, sizeof(lw.rows[ROW_CPU].value), "%.0f%%", lw.cpu * 100);
	lw.rows[ROW_CPU].bar = lw.cpu;

	set_row(ROW_MEM, "MEMORY", "panel memory");
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) {
		unsigned long long total = 0, avail = 0, v;
		char key[64];
		while (fscanf(f, "%63s %llu kB\n", key, &v) == 2) {
			if (strcmp(key, "MemTotal:") == 0) {
				total = v;
			} else if (strcmp(key, "MemAvailable:") == 0) {
				avail = v;
			}
		}
		fclose(f);
		if (total) {
			double used = (double)(total - avail) / total;
			snprintf(lw.rows[ROW_MEM].value, sizeof(lw.rows[ROW_MEM].value), "%.1f OF %.0f GiB",
				(total - avail) / 1048576.0, total / 1048576.0);
			lw.rows[ROW_MEM].bar = used;
		}
	}

	set_row(ROW_DISK, "DISK", "exec tilewin-settings --page about");
	if (lw.samples % 10 == 1) {
		struct statvfs vfs;
		if (statvfs("/", &vfs) == 0 && vfs.f_blocks) {
			lw.disk_used = 1.0 - (double)vfs.f_bavail / vfs.f_blocks;
		}
	}
	snprintf(lw.rows[ROW_DISK].value, sizeof(lw.rows[ROW_DISK].value), "%.0f%% USED",
		lw.disk_used * 100);
	lw.rows[ROW_DISK].bar = lw.disk_used;

	set_row(ROW_NET, "NETWORK", "panel network");
	char down[32], up[32];
	rate_text(lw.rx_rate, down, sizeof(down));
	rate_text(lw.tx_rate, up, sizeof(up));
	snprintf(lw.rows[ROW_NET].value, sizeof(lw.rows[ROW_NET].value), "↓ %s  ↑ %s", down, up);

	set_row(ROW_BAT, "POWER", "panel power");
	int percent;
	bool charging;
	if (tw_battery_read(tw_power_supply_dir(), &percent, &charging)) {
		snprintf(lw.rows[ROW_BAT].value, sizeof(lw.rows[ROW_BAT].value), "%d%%%s", percent,
			charging ? " CHARGING" : "");
		lw.rows[ROW_BAT].bar = percent / 100.0;
	} else {
		snprintf(lw.rows[ROW_BAT].value, sizeof(lw.rows[ROW_BAT].value), "MAINS");
	}

	double temp = read_temp();
	set_row(ROW_TEMP, "TEMPERATURE", "panel cpu");
	lw.rows[ROW_TEMP].shown = temp >= 0;
	snprintf(lw.rows[ROW_TEMP].value, sizeof(lw.rows[ROW_TEMP].value), "%.0f °C", temp);

	set_row(ROW_UPTIME, "UPTIME", NULL);
	f = fopen("/proc/uptime", "r");
	double up_s = 0;
	if (f) {
		if (fscanf(f, "%lf", &up_s) != 1) {
			up_s = 0;
		}
		fclose(f);
	}
	long mins = (long)(up_s / 60);
	if (mins >= 1440) {
		snprintf(lw.rows[ROW_UPTIME].value, sizeof(lw.rows[ROW_UPTIME].value), "%ld D %ld H",
			mins / 1440, mins % 1440 / 60);
	} else {
		snprintf(lw.rows[ROW_UPTIME].value, sizeof(lw.rows[ROW_UPTIME].value), "%ld H %ld MIN",
			mins / 60, mins % 60);
	}

	set_row(ROW_WINDOWS, "WINDOWS", NULL);
	int windows = lw.panel->state.windows ? lw.panel->state.windows->length : 0;
	snprintf(lw.rows[ROW_WINDOWS].value, sizeof(lw.rows[ROW_WINDOWS].value), "%d OPEN",
		windows);
}

/* ---------- drawing ---------- */

static uint32_t pal(int i) {
	return panel_console_color(lw.panel, i);
}

static const char *font_of(bool bold, int size, char *buf, size_t n) {
	const char *base = tw_theme_str(lw.panel->theme, bold ? "panel.bold_font" : "panel.font",
		"Sans 11");
	// the family of the theme's font, in another size
	const char *space = strrchr(base, ' ');
	snprintf(buf, n, "%.*s %d", space ? (int)(space - base) : (int)strlen(base), base, size);
	return buf;
}

/* The radar in the colors of the console, the names in capitals. */
static void sensor_look(struct radar_look *look, double S) {
	memset(look, 0, sizeof(*look));
	const char *base = tw_theme_str(lw.panel->theme, "panel.font", "Sans 11");
	const char *space = strrchr(base, ' ');
	snprintf(look->family, sizeof(look->family), "%.*s",
		space ? (int)(space - base) : (int)strlen(base), base);
	look->disc = 0x000000a8;
	look->ring = look->sweep = pal(0);
	look->fg = tw_theme_color(lw.panel->theme, "panel.fg", 0xd3d6ffff);
	look->dim = pal(2);
	look->blip = pal(4);
	look->linked = pal(1);
	look->bad = 0xe0606aff;
	look->card_bg = 0x000000e8;
	look->card_fg = look->fg;
	look->card_border = pal(0);
	look->upper = true;
	look->bearings = true;
	look->scale = 1;
	look->turn = 6;
	look->cards_x = 0;
	look->cards_y = 20;
	look->cards_w = S;
	look->cards_h = S - 40;
}

static void sensor_geom(double S, struct radar_look *look, struct radar_geom *g) {
	sensor_look(look, S);
	wifiradar_geom(0, 0, S, S, look, g);
}

static void sensor_render(struct psurface *s, cairo_t *cr) {
	struct live_output *lo = s->data;
	double S = s->width;
	struct radar_look look;
	struct radar_geom g;
	sensor_geom(S, &look, &g);
	wifiradar_draw(cr, &g, &look, lo->hover_contact);
	char font[160], small[160];
	font_of(true, 11, font, sizeof(font));
	font_of(false, 9, small, sizeof(small));
	int n = wifiradar_count();
	char count[48];
	snprintf(count, sizeof(count), n ? "%d CONTACTS" : "NO CONTACTS", n);
	pd_text(cr, font, "SENSORS", 0, 0, S, 18, pal(0), PD_LEFT);
	pd_text(cr, small, count, 0, S - 16, S, 16, pal(2), PD_RIGHT);
}

static int readout_height(void) {
	int rows = 0;
	for (int i = 0; i < ROW_COUNT; i++) {
		rows += lw.rows[i].shown || lw.samples == 0;
	}
	return 34 + rows * ROW_H + 22 + 46 + 14;
}

static void readout_render(struct psurface *s, cairo_t *cr) {
	struct live_output *lo = s->data;
	double W = s->width, H = s->height;
	char font[160], bold[160], big[160], small[160];
	font_of(false, 11, font, sizeof(font));
	font_of(true, 11, bold, sizeof(bold));
	font_of(true, 15, big, sizeof(big));
	font_of(false, 9, small, sizeof(small));
	uint32_t fg = tw_theme_color(lw.panel->theme, "panel.fg", 0xd3d6ffff);
	uint32_t frame = pal(0);

	pd_rounded(cr, 0, 0, W, H, 18);
	pd_color(cr, 0x000000b0);
	cairo_fill(cr);
	// the elbow: the bar along the top to the name, the bar down the left
	double bar = 22, rail = 14, outer = 22, inner = 10, elbow = 64;
	cairo_new_path(cr);
	cairo_move_to(cr, 0, outer);
	cairo_arc(cr, outer, outer, outer, M_PI, 3 * M_PI / 2);
	cairo_line_to(cr, elbow, 0);
	cairo_line_to(cr, elbow, bar);
	cairo_line_to(cr, rail + inner, bar);
	cairo_arc_negative(cr, rail + inner, bar + inner, inner, -M_PI / 2, M_PI);
	cairo_line_to(cr, rail, H - rail / 2);
	cairo_arc(cr, rail / 2, H - rail / 2, rail / 2, 0, M_PI);
	cairo_close_path(cr);
	pd_color(cr, frame);
	cairo_fill(cr);
	int tw = 0;
	pd_text_size(cr, bold, "STATUS", &tw, NULL);
	pd_text(cr, bold, "STATUS", W - 12 - tw, 0, tw + 2, bar, frame, PD_LEFT);
	// lamps of what goes on: network in, network out, then the bar to the name
	double lx = elbow + 5;
	const bool lamps[] = { lw.rx_lamp, lw.tx_lamp };
	for (int i = 0; i < 2; i++) {
		pd_console_block(cr, lx, 0, 22, bar, false, false, lamps[i] ? pal(1) : 0x2a2f55ff);
		lx += 26;
	}
	if (W - 20 - tw - lx > bar) {
		pd_console_block(cr, lx, 0, W - 20 - tw - lx, bar, false, true, pal(2));
	}

	double y = 34, x = rail + 12, label_w = 104;
	for (int i = 0; i < ROW_COUNT; i++) {
		const struct row *r = &lw.rows[i];
		if (!r->shown || !r->label) {
			continue;
		}
		bool hover = lo->hover_row == i && r->command;
		uint32_t color = hover ? pal(1) : pal(2 + i % 5);
		pd_console_block(cr, x, y + 3, label_w, ROW_H - 6, true, false, color);
		pd_text(cr, small, r->label, x, y + 3, label_w - 8, ROW_H - 6, 0x000000ff, PD_RIGHT);
		double vx = x + label_w + 10, vw = W - vx - 12;
		if (r->bar >= 0) {
			// the share in blocks, as the consoles show a level
			double bw = 96, blocks = 12, step = bw / blocks;
			for (int k = 0; k < blocks; k++) {
				bool on = (k + 0.5) / blocks <= r->bar;
				double level = r->full_is_good ? 1 - r->bar : r->bar;
				uint32_t c = !on ? 0x2a2f55ff : level > 0.9 ? 0xe0606aff :
					level > 0.75 ? 0xffcc99ff : frame;
				pd_rect(cr, W - 12 - bw + k * step, y + 8, step - 2, ROW_H - 16, c);
			}
			vw -= bw + 8;
		}
		pd_text(cr, i == ROW_TIME ? big : font, r->value, vx, y, vw, ROW_H,
			hover ? pal(1) : i == ROW_TIME ? pal(1) : fg, PD_LEFT);
		y += ROW_H;
	}
	// the processor's last minute
	y += 22;
	double cw = W - x - 12, ch = 40;
	pd_text(cr, small, "PROCESSOR · LAST MINUTE", x, y - 14, cw, 14, pal(2), PD_LEFT);
	double step = cw / HISTORY;
	for (int k = 0; k < HISTORY; k++) {
		int at = k - (HISTORY - lw.history_len);
		double v = at >= 0 ? lw.cpu_history[at] : 0;
		double bh = fmax(ch * v, 1);
		pd_rect(cr, x + k * step, y + ch - bh, fmax(step - 1, 1), bh,
			at >= 0 ? (v > 0.9 ? 0xe0606aff : pal(3)) : 0x2a2f55ff);
	}
}

/* ---------- surfaces ---------- */

static void part_configured(struct psurface *s) {
	psurface_set_dirty(s);
}

static void part_closed(struct psurface *s) {
	struct live_output *lo = s->data;
	for (int i = 0; i < PART_COUNT; i++) {
		if (lo->parts[i] == s) {
			lo->parts[i] = NULL;
		}
	}
	psurface_destroy(s);
}

static const struct psurface_impl sensor_impl = {
	.render = sensor_render,
	.configured = part_configured,
	.closed = part_closed,
};

static const struct psurface_impl readout_impl = {
	.render = readout_render,
	.configured = part_configured,
	.closed = part_closed,
};

/* Where the parts go on a screen: along the bottom, right of the desktop icons. */
static void layout(struct live_output *lo) {
	struct panel_output *o = lo->output;
	int S = (int)fmin(fmax(o->height * 0.42, 240), 420);
	int RH = readout_height();
	int bottom = 84, left = 150;
	lo->boxes[PART_SENSOR] = (struct pbox){ left, o->height - bottom - S, S, S };
	int rx = left + S + 30;
	if (rx + READOUT_W > o->width - 20) {
		// a narrow screen: the readouts over the sensor, at the top
		rx = left;
		lo->boxes[PART_READOUT] = (struct pbox){ rx, 40, READOUT_W, RH };
	} else {
		lo->boxes[PART_READOUT] = (struct pbox){ rx, o->height - bottom - RH, READOUT_W, RH };
	}
}

static void place(struct psurface *s, struct pbox b) {
	zwlr_layer_surface_v1_set_margin(s->layer_surface, b.y, 0, 0, b.x);
	psurface_set_size(s, b.width, b.height);
	wl_surface_commit(s->surface);
}

static struct psurface *make_part(struct live_output *lo, const struct psurface_impl *impl,
		struct pbox b) {
	struct panel *panel = lw.panel;
	struct psurface *s = psurface_create(panel, lo->output, impl, lo,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "tilewin-live-wallpaper");
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	// the pointer goes through to the desktop, which hands it on (livewall_pointer)
	struct wl_region *none = wl_compositor_create_region(panel->compositor);
	wl_surface_set_input_region(s->surface, none);
	wl_region_destroy(none);
	place(s, b);
	return s;
}

static void output_free(struct live_output *lo) {
	for (int i = 0; i < PART_COUNT; i++) {
		if (lo->parts[i]) {
			psurface_destroy(lo->parts[i]);
		}
	}
	wl_list_remove(&lo->link);
	free(lo);
}

static struct live_output *live_of(struct panel_output *output) {
	if (!lw.inited) {
		return NULL;
	}
	struct live_output *lo;
	wl_list_for_each(lo, &lw.outputs, link) {
		if (lo->output == output) {
			return lo;
		}
	}
	return NULL;
}

/* ---------- time ---------- */

static void redraw(int part) {
	struct live_output *lo;
	wl_list_for_each(lo, &lw.outputs, link) {
		if (lo->parts[part]) {
			psurface_set_dirty(lo->parts[part]);
		}
	}
}

static void sweep_tick(void *data) {
	lw.sweep_timer = NULL;
	redraw(PART_SENSOR);
	lw.sweep_timer = loop_add_timer_lazy(lw.panel->loop, lw.panel->state.battery_saver ?
		SWEEP_SAVER_MS : SWEEP_MS, sweep_tick, NULL);
}

static void sample_tick(void *data) {
	lw.sample_timer = NULL;
	int before = readout_height();
	sample();
	if (readout_height() != before) {
		// a row came or went (the temperature): the panel grows or shrinks
		struct live_output *lo;
		wl_list_for_each(lo, &lw.outputs, link) {
			layout(lo);
			if (lo->parts[PART_READOUT]) {
				place(lo->parts[PART_READOUT], lo->boxes[PART_READOUT]);
			}
		}
	}
	redraw(PART_READOUT);
	lw.sample_timer = loop_add_timer_lazy(lw.panel->loop, SAMPLE_MS, sample_tick, NULL);
}

static void stop_timers(void) {
	struct loop_timer **timers[] = { &lw.sweep_timer, &lw.sample_timer };
	for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
		if (*timers[i]) {
			loop_remove_timer(lw.panel->loop, *timers[i]);
			*timers[i] = NULL;
		}
	}
	if (lw.radar_held) {
		lw.radar_held = false;
		wifiradar_release();
	}
}

/* ---------- which screens ---------- */

/* Whether a screen shows the theme's own wallpaper, as the compositor says. */
static bool theme_wallpaper_on(json_object *outputs, const char *name) {
	const char *dir = lw.panel->theme ? lw.panel->theme->dir : NULL;
	size_t n = outputs && json_object_is_type(outputs, json_type_array) ?
		json_object_array_length(outputs) : 0;
	for (size_t i = 0; dir && i < n; i++) {
		json_object *o = json_object_array_get_idx(outputs, i), *v, *wp, *img;
		if (!json_object_object_get_ex(o, "name", &v) ||
				strcmp(json_object_get_string(v), name) != 0) {
			continue;
		}
		if (!json_object_object_get_ex(o, "tw_wallpaper", &wp) ||
				!json_object_object_get_ex(wp, "image", &img)) {
			return false;
		}
		const char *image = json_object_get_string(img);
		return image && strncmp(image, dir, strlen(dir)) == 0;
	}
	return false;
}

void livewall_update(struct panel *panel) {
	if (!lw.inited) {
		lw.panel = panel;
		wl_list_init(&lw.outputs);
		lw.inited = true;
		for (int i = 0; i < ROW_COUNT; i++) {
			lw.rows[i].bar = -1;
		}
	}
	const char *scene = panel->theme ? tw_theme_str(panel->theme, "live_wallpaper.scene", NULL) :
		NULL;
	bool wanted = scene && strcmp(scene, "console") == 0 && panel->config &&
		twconf_parse_bool(twconf_value(panel->config->root, "live_wallpaper"), true);
	json_object *outputs = wanted ? ipc_request(panel, IPC_GET_OUTPUTS, NULL) : NULL;
	// the screens that show it now, and only those
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		struct live_output *lo = live_of(output);
		bool on = wanted && output->ready && output->name &&
			theme_wallpaper_on(outputs, output->name);
		if (!on) {
			if (lo) {
				output_free(lo);
			}
			continue;
		}
		if (!lo) {
			lo = calloc(1, sizeof(*lo));
			lo->output = output;
			lo->hover_row = lo->hover_contact = -1;
			wl_list_insert(&lw.outputs, &lo->link);
		}
		if (lw.samples == 0) {
			sample();
		}
		layout(lo);
		if (!lo->parts[PART_SENSOR]) {
			lo->parts[PART_SENSOR] = make_part(lo, &sensor_impl, lo->boxes[PART_SENSOR]);
		} else {
			place(lo->parts[PART_SENSOR], lo->boxes[PART_SENSOR]);
		}
		if (!lo->parts[PART_READOUT]) {
			lo->parts[PART_READOUT] = make_part(lo, &readout_impl, lo->boxes[PART_READOUT]);
		} else {
			place(lo->parts[PART_READOUT], lo->boxes[PART_READOUT]);
		}
	}
	json_object_put(outputs);
	if (wl_list_empty(&lw.outputs)) {
		stop_timers();
		return;
	}
	if (!lw.sweep_timer) {
		sweep_tick(NULL);
	}
	if (!lw.sample_timer) {
		sample_tick(NULL);
	}
	if (!lw.radar_held) {
		lw.radar_held = true;
		wifiradar_hold(panel);
	}
}

void livewall_output_gone(struct panel_output *output) {
	struct live_output *lo = live_of(output);
	if (lo) {
		output_free(lo);
	}
	if (lw.inited && wl_list_empty(&lw.outputs)) {
		stop_timers();
	}
}

void livewall_fini(void) {
	if (!lw.inited) {
		return;
	}
	struct live_output *lo, *tmp;
	wl_list_for_each_safe(lo, tmp, &lw.outputs, link) {
		output_free(lo);
	}
	stop_timers();
}

/* ---------- the pointer, from the desktop ---------- */

static int row_at(struct live_output *lo, double x, double y) {
	struct pbox b = lo->boxes[PART_READOUT];
	if (!lo->parts[PART_READOUT] || !pbox_contains(&b, x, y)) {
		return -1;
	}
	double ry = b.y + 34;
	for (int i = 0; i < ROW_COUNT; i++) {
		if (!lw.rows[i].shown || !lw.rows[i].label) {
			continue;
		}
		if (y >= ry && y < ry + ROW_H) {
			return i;
		}
		ry += ROW_H;
	}
	return -1;
}

static int contact_at(struct live_output *lo, double x, double y) {
	struct pbox b = lo->boxes[PART_SENSOR];
	if (!lo->parts[PART_SENSOR] || !pbox_contains(&b, x, y)) {
		return -1;
	}
	struct radar_look look;
	struct radar_geom g;
	sensor_geom(b.width, &look, &g);
	return wifiradar_hit(&g, &look, x - b.x, y - b.y);
}

bool livewall_pointer(struct panel_output *output, double x, double y) {
	struct live_output *lo = live_of(output);
	if (!lo) {
		return false;
	}
	int row = row_at(lo, x, y), contact = contact_at(lo, x, y);
	if (row != lo->hover_row) {
		lo->hover_row = row;
		if (lo->parts[PART_READOUT]) {
			psurface_set_dirty(lo->parts[PART_READOUT]);
		}
	}
	if (contact != lo->hover_contact) {
		lo->hover_contact = contact;
		if (lo->parts[PART_SENSOR]) {
			psurface_set_dirty(lo->parts[PART_SENSOR]);
		}
	}
	return (row >= 0 && lw.rows[row].command) ||
		(lo->parts[PART_SENSOR] && pbox_contains(&lo->boxes[PART_SENSOR], x, y));
}

bool livewall_click(struct panel_output *output, double x, double y) {
	struct live_output *lo = live_of(output);
	if (!lo) {
		return false;
	}
	int row = row_at(lo, x, y);
	if (row >= 0 && lw.rows[row].command) {
		bar_run_command(lw.panel, lw.rows[row].command, NULL);
		return true;
	}
	if (lo->parts[PART_SENSOR] && pbox_contains(&lo->boxes[PART_SENSOR], x, y)) {
		// within the rings, not the empty corners
		struct pbox b = lo->boxes[PART_SENSOR];
		struct radar_look look;
		struct radar_geom g;
		sensor_geom(b.width, &look, &g);
		double lx = x - b.x, ly = y - b.y;
		if (wifiradar_in_disc(&g, lx, ly) || wifiradar_hit(&g, &look, lx, ly) >= 0) {
			bool above = y > output->height / 2;
			struct popup_anchor anchor = {
				.output = output,
				.x = (int)x,
				.y = above ? (int)y - 16 : (int)y + 16,
				.above = above,
			};
			wifiradar_click(&g, &look, lx, ly, anchor);
			psurface_set_dirty(lo->parts[PART_SENSOR]);
			return true;
		}
	}
	return false;
}
