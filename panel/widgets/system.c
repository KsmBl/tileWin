#include <dirent.h>
#include <ifaddrs.h>
#include <linux/input-event-codes.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"

/*
 * System widgets read /proc and /sys directly at their configured interval
 * and only while they are part of the active layout.
 */

struct poll_data {
	struct loop_timer *timer;
	int interval_ms;
	void (*update)(struct widget *w);
	void *state;
};

static void poll_tick(void *data) {
	struct widget *w = data;
	struct poll_data *p = w->data;
	p->timer = NULL;
	if (!w->active) {
		return;
	}
	p->update(w);
	panel_set_dirty(w->panel);
	p->timer = loop_add_timer(w->panel->loop, p->interval_ms, poll_tick, w);
}

static struct poll_data *poll_init(struct widget *w, int default_seconds,
		void (*update)(struct widget *w), size_t state_size) {
	struct poll_data *p = calloc(1, sizeof(*p));
	p->interval_ms = widget_conf_int(w, "interval", default_seconds) * 1000;
	if (p->interval_ms < 250) {
		p->interval_ms = 250;
	}
	p->update = update;
	p->state = calloc(1, state_size);
	w->data = p;
	update(w);
	return p;
}

static void poll_set_active(struct widget *w, bool active) {
	struct poll_data *p = w->data;
	if (active && !p->timer) {
		poll_tick(w);
	} else if (!active && p->timer) {
		loop_remove_timer(w->panel->loop, p->timer);
		p->timer = NULL;
	}
}

static void poll_destroy(struct widget *w) {
	struct poll_data *p = w->data;
	if (p->timer) {
		loop_remove_timer(w->panel->loop, p->timer);
	}
	free(p->state);
	free(p);
}

static bool read_file(const char *path, char *buf, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	size_t n = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
		buf[--n] = '\0';
	}
	return true;
}

/* Replaces {key} placeholders; values is a NULL-terminated key/value list. */
static char *format_text(const char *format, const char **values) {
	char *out = strdup(format);
	for (int i = 0; values[i]; i += 2) {
		char key[64];
		snprintf(key, sizeof(key), "{%s}", values[i]);
		char *pos;
		while ((pos = strstr(out, key))) {
			char *next = format_str("%.*s%s%s", (int)(pos - out), out, values[i + 1],
				pos + strlen(key));
			free(out);
			out = next;
		}
	}
	return out;
}

/* Windows 11: the network, volume and battery icons open quick settings. */
static bool opens_quick_settings(struct widget *w) {
	return widget_conf_bool(w, "quick_settings",
		tw_theme_bool(w->panel->theme, "panel.quick_settings", false));
}

static int glyph_size(struct render_ctx *ctx) {
	return ctx->height < 32 ? 16 : 18;
}

static int item_padding(struct render_ctx *ctx) {
	return tw_theme_int(ctx->panel->theme, "panel.item_padding", 6);
}

static bool has_icon(const char *format) {
	return format && strstr(format, "{icon}");
}

/* Picks the "{icon}" for a percentage from the widget's "icons" list (lowest first). */
static void level_icon(struct widget *w, int percent, char *buf, size_t size) {
	buf[0] = '\0';
	const char *list = widget_conf(w, "icons", NULL);
	if (!list) {
		return;
	}
	char *copy = strdup(list);
	int count = 0;
	char *save = NULL;
	for (char *tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		count++;
	}
	free(copy);
	if (count == 0) {
		return;
	}
	int index = percent < 0 ? 0 : percent * count / 101;
	if (index >= count) {
		index = count - 1;
	}
	copy = strdup(list);
	save = NULL;
	char *tok = strtok_r(copy, " \t", &save);
	for (int i = 0; tok && i < index; i++) {
		tok = strtok_r(NULL, " \t", &save);
	}
	snprintf(buf, size, "%s", tok ? tok : "");
	free(copy);
}

/* Widgets with text icons ("{icon}" or an "icons" list) draw no glyph. */
static bool text_icons(struct widget *w, const char *format) {
	return has_icon(format) || widget_conf(w, "icons", NULL) != NULL;
}

/* Text color for "<type>.warning" and "<type>.critical" levels. */
static uint32_t level_fg(struct widget *w, struct render_ctx *ctx, int value, bool low_is_bad) {
	const struct tw_theme *t = ctx->panel->theme;
	const char *type = w->impl->type;
	char key[64];
	snprintf(key, sizeof(key), "%s.critical", type);
	int critical = tw_theme_int(t, key, -1);
	snprintf(key, sizeof(key), "%s.warning", type);
	int warning = tw_theme_int(t, key, -1);
	if (critical >= 0 && (low_is_bad ? value <= critical : value >= critical)) {
		snprintf(key, sizeof(key), "%s.critical_fg", type);
		return tw_theme_color(t, key, 0xf87171ff);
	}
	if (warning >= 0 && (low_is_bad ? value <= warning : value >= warning)) {
		snprintf(key, sizeof(key), "%s.warning_fg", type);
		return tw_theme_color(t, key, 0xfbbf24ff);
	}
	return widget_fg(ctx->panel, type);
}

static int text_item_measure(struct render_ctx *ctx, const char *text, bool glyph) {
	int width = glyph ? glyph_size(ctx) + 2 * item_padding(ctx) : 2 * item_padding(ctx);
	if (text && *text) {
		width += render_text_width(ctx, bar_font(ctx->panel), text) + (glyph ? 4 : 0);
	}
	return width;
}

static double render_item_start(struct render_ctx *ctx, struct pbox b) {
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	return b.x + item_padding(ctx);
}

/* ================= cpu ================= */

#define CPU_HISTORY 32

struct cpu_state {
	unsigned long long last_total, last_idle;
	int usage;
	int history[CPU_HISTORY];
	int history_pos;
};

static void cpu_update(struct widget *w) {
	struct cpu_state *s = ((struct poll_data *)w->data)->state;
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return;
	}
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
	fclose(f);
	if (n < 8) {
		return;
	}
	unsigned long long idle_all = idle + iowait;
	unsigned long long total = user + nice + system + idle_all + irq + softirq + steal;
	if (s->last_total && total > s->last_total) {
		unsigned long long dt = total - s->last_total;
		unsigned long long di = idle_all - s->last_idle;
		s->usage = (int)(100 * (dt - di) / dt);
	}
	s->last_total = total;
	s->last_idle = idle_all;
	s->history[s->history_pos] = s->usage;
	s->history_pos = (s->history_pos + 1) % CPU_HISTORY;
}

static void cpu_init(struct widget *w) {
	poll_init(w, 2, cpu_update, sizeof(struct cpu_state));
}

static char *cpu_text(struct widget *w) {
	struct cpu_state *s = ((struct poll_data *)w->data)->state;
	char usage[16];
	snprintf(usage, sizeof(usage), "%d", s->usage);
	const char *values[] = { "usage", usage, NULL };
	return format_text(widget_conf(w, "format", "CPU {usage}%"), values);
}

static bool graph_style(struct widget *w) {
	return strcasecmp(widget_conf(w, "style", "text"), "graph") == 0;
}

static int cpu_measure(struct widget *w, struct render_ctx *ctx) {
	if (graph_style(w)) {
		return widget_conf_int(w, "width", 44);
	}
	char *text = cpu_text(w);
	int width = text_item_measure(ctx, text, false);
	free(text);
	return width;
}

static void cpu_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct cpu_state *s = ((struct poll_data *)w->data)->state;
	double x = render_item_start(ctx, b);
	uint32_t fg = level_fg(w, ctx, s->usage, false);
	if (graph_style(w)) {
		double gh = b.height * 0.6, gy = b.y + (b.height - gh) / 2;
		double gw = b.width - 8, step = gw / (CPU_HISTORY - 1);
		cairo_t *cr = ctx->cairo;
		cairo_move_to(cr, x - 2, gy + gh);
		for (int i = 0; i < CPU_HISTORY; i++) {
			int v = s->history[(s->history_pos + i) % CPU_HISTORY];
			cairo_line_to(cr, x - 2 + i * step, gy + gh - gh * v / 100.0);
		}
		cairo_line_to(cr, x - 2 + gw, gy + gh);
		cairo_close_path(cr);
		pd_color(cr, tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x3aa0ffff));
		cairo_fill(cr);
	} else {
		char *text = cpu_text(w);
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.width - 12, b.height, fg, PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool cpu_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	flyout_cpu_toggle(w->panel, anchor, widget_conf(w, "task_manager", NULL));
	return true;
}

static char *cpu_tooltip(struct widget *w, struct hotspot *hs) {
	struct cpu_state *s = ((struct poll_data *)w->data)->state;
	return format_str("CPU usage: %d%%", s->usage);
}

const struct widget_impl widget_cpu = {
	.type = "cpu",
	.init = cpu_init,
	.destroy = poll_destroy,
	.measure = cpu_measure,
	.render = cpu_render,
	.click = cpu_click,
	.tooltip = cpu_tooltip,
	.set_active = poll_set_active,
};

/* ================= memory ================= */

struct mem_state {
	long long total_kb, available_kb;
};

static void memory_update(struct widget *w) {
	struct mem_state *s = ((struct poll_data *)w->data)->state;
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "MemTotal: %lld kB", &s->total_kb);
		sscanf(line, "MemAvailable: %lld kB", &s->available_kb);
	}
	fclose(f);
}

static void memory_init(struct widget *w) {
	poll_init(w, 5, memory_update, sizeof(struct mem_state));
}

static char *memory_text(struct widget *w) {
	struct mem_state *s = ((struct poll_data *)w->data)->state;
	long long used = s->total_kb - s->available_kb;
	char percent[16], used_gb[32], total_gb[32];
	snprintf(percent, sizeof(percent), "%lld", s->total_kb ? used * 100 / s->total_kb : 0);
	snprintf(used_gb, sizeof(used_gb), "%.1f", used / 1048576.0);
	snprintf(total_gb, sizeof(total_gb), "%.1f", s->total_kb / 1048576.0);
	const char *values[] = { "used_percent", percent, "used", used_gb, "total", total_gb, NULL };
	return format_text(widget_conf(w, "format", "RAM {used_percent}%"), values);
}

static int memory_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = memory_text(w);
	int width = text_item_measure(ctx, text, false);
	free(text);
	return width;
}

static void memory_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct mem_state *s = ((struct poll_data *)w->data)->state;
	double x = render_item_start(ctx, b);
	char *text = memory_text(w);
	int percent = s->total_kb ? (int)((s->total_kb - s->available_kb) * 100 / s->total_kb) : 0;
	pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.width - 2 * item_padding(ctx),
		b.height, level_fg(w, ctx, percent, false), PD_LEFT);
	free(text);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool memory_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	flyout_memory_toggle(w->panel, anchor, widget_conf(w, "task_manager", NULL));
	return true;
}

static char *memory_tooltip(struct widget *w, struct hotspot *hs) {
	struct mem_state *s = ((struct poll_data *)w->data)->state;
	return format_str("Memory: %.1f GiB of %.1f GiB used",
		(s->total_kb - s->available_kb) / 1048576.0, s->total_kb / 1048576.0);
}

const struct widget_impl widget_memory = {
	.type = "memory",
	.init = memory_init,
	.destroy = poll_destroy,
	.measure = memory_measure,
	.render = memory_render,
	.click = memory_click,
	.tooltip = memory_tooltip,
	.set_active = poll_set_active,
};

/* ================= battery ================= */

struct battery_state {
	bool present;
	int capacity;
	char status[32];
	char device[256];
};

static void battery_update(struct widget *w) {
	struct battery_state *s = ((struct poll_data *)w->data)->state;
	const char *device = widget_conf(w, "device", NULL);
	s->present = false;
	DIR *dir = opendir("/sys/class/power_supply");
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.' || (device && strcmp(de->d_name, device) != 0)) {
			continue;
		}
		char path[512], buf[64];
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
		if (!read_file(path, buf, sizeof(buf)) || strcmp(buf, "Battery") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
		if (read_file(path, buf, sizeof(buf))) {
			s->capacity = atoi(buf);
			s->present = true;
		}
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
		if (!read_file(path, s->status, sizeof(s->status))) {
			s->status[0] = '\0';
		}
		snprintf(s->device, sizeof(s->device), "%s", de->d_name);
		break;
	}
	closedir(dir);
}

static void battery_init(struct widget *w) {
	poll_init(w, 30, battery_update, sizeof(struct battery_state));
}

static char *battery_text(struct widget *w, bool *icons_in_text) {
	struct battery_state *s = ((struct poll_data *)w->data)->state;
	const char *format = NULL;
	if (strcmp(s->status, "Charging") == 0) {
		format = widget_conf(w, "format_charging", NULL);
	} else if (strcmp(s->status, "Full") == 0) {
		format = widget_conf(w, "format_full", NULL);
	} else if (strcmp(s->status, "Not charging") == 0) {
		format = widget_conf(w, "format_plugged", NULL);
	}
	if (!format) {
		format = widget_conf(w, "format", NULL);
	}
	*icons_in_text = text_icons(w, format);
	if (!format) {
		return NULL;
	}
	char capacity[16], icon[64];
	snprintf(capacity, sizeof(capacity), "%d", s->capacity);
	level_icon(w, s->capacity, icon, sizeof(icon));
	const char *values[] = { "capacity", capacity, "status", s->status, "icon", icon, NULL };
	return format_text(format, values);
}

static int battery_measure(struct widget *w, struct render_ctx *ctx) {
	struct battery_state *s = ((struct poll_data *)w->data)->state;
	if (!s->present) {
		return 0;
	}
	bool icons_in_text;
	char *text = battery_text(w, &icons_in_text);
	int width = text_item_measure(ctx, text, !icons_in_text);
	free(text);
	return width;
}

static void battery_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct battery_state *s = ((struct poll_data *)w->data)->state;
	double x = render_item_start(ctx, b);
	uint32_t fg = level_fg(w, ctx, s->capacity, true);
	bool icons_in_text;
	char *text = battery_text(w, &icons_in_text);
	if (!icons_in_text) {
		int g = glyph_size(ctx);
		ti_battery(ctx->panel, ctx->cairo, x, b.y + (b.height - g) / 2.0, g, s->capacity,
			strcmp(s->status, "Charging") == 0, fg);
		x += g + 4;
	}
	if (text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.x + b.width - x,
			b.height, fg, PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}


/* Battery and brightness open the power flyout. */
static bool power_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	if (opens_quick_settings(w)) {
		quicksettings_toggle(w->panel, anchor);
		return true;
	}
	flyout_power_toggle(w->panel, anchor, widget_conf(w, "settings", NULL));
	return true;
}

static char *battery_tooltip(struct widget *w, struct hotspot *hs) {
	struct battery_state *s = ((struct poll_data *)w->data)->state;
	return format_str("Battery: %d%% (%s)", s->capacity, s->status);
}

const struct widget_impl widget_battery = {
	.type = "battery",
	.init = battery_init,
	.destroy = poll_destroy,
	.measure = battery_measure,
	.render = battery_render,
	.click = power_click,
	.tooltip = battery_tooltip,
	.set_active = poll_set_active,
};

/* ================= network ================= */

struct net_state {
	bool connected;
	bool wireless;
	int quality; // 0..100
	char iface[256];
	char address[64];
};

static void network_update(struct widget *w) {
	struct net_state *s = ((struct poll_data *)w->data)->state;
	const char *wanted = widget_conf(w, "interface", NULL);
	memset(s, 0, sizeof(*s));
	DIR *dir = opendir("/sys/class/net");
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0) {
			continue;
		}
		if (wanted && strcmp(de->d_name, wanted) != 0) {
			continue;
		}
		char path[512], buf[64];
		snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", de->d_name);
		if (!read_file(path, buf, sizeof(buf))) {
			continue;
		}
		bool up = strcmp(buf, "up") == 0;
		snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", de->d_name);
		bool wireless = access(path, F_OK) == 0;
		snprintf(path, sizeof(path), "/sys/class/net/%s/device", de->d_name);
		bool physical = access(path, F_OK) == 0;
		if (!physical && !wanted) {
			continue; // skip bridges, docker, veth...
		}
		if (!s->iface[0] || (up && !s->connected)) {
			snprintf(s->iface, sizeof(s->iface), "%s", de->d_name);
			s->connected = up;
			s->wireless = wireless;
		}
	}
	closedir(dir);
	if (s->wireless) {
		FILE *f = fopen("/proc/net/wireless", "r");
		char line[256];
		while (f && fgets(line, sizeof(line), f)) {
			char name[64];
			double quality;
			if (sscanf(line, " %63[^:]: %*d %lf", name, &quality) == 2 &&
					strcmp(name, s->iface) == 0) {
				s->quality = (int)(quality * 100 / 70);
			}
		}
		if (f) {
			fclose(f);
		}
	}
	struct ifaddrs *addrs;
	if (s->connected && getifaddrs(&addrs) == 0) {
		for (struct ifaddrs *a = addrs; a; a = a->ifa_next) {
			if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET &&
					strcmp(a->ifa_name, s->iface) == 0) {
				inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr,
					s->address, sizeof(s->address));
				break;
			}
		}
		freeifaddrs(addrs);
	}
}

static void network_init(struct widget *w) {
	poll_init(w, 5, network_update, sizeof(struct net_state));
}

static char *network_text(struct widget *w, bool *icons_in_text) {
	struct net_state *s = ((struct poll_data *)w->data)->state;
	const char *format = NULL;
	if (!s->connected) {
		format = widget_conf(w, "format_disconnected", NULL);
	} else if (!s->wireless) {
		format = widget_conf(w, "format_ethernet", NULL);
	}
	if (!format) {
		format = widget_conf(w, "format", NULL);
	}
	*icons_in_text = text_icons(w, format);
	if (!format) {
		return NULL;
	}
	char quality[16], icon[64];
	snprintf(quality, sizeof(quality), "%d", s->quality);
	level_icon(w, s->connected ? s->quality : 0, icon, sizeof(icon));
	const char *values[] = { "quality", quality, "iface", s->iface, "address", s->address,
		"icon", icon, NULL };
	return format_text(format, values);
}

static int network_measure(struct widget *w, struct render_ctx *ctx) {
	struct net_state *s = ((struct poll_data *)w->data)->state;
	if (!s->iface[0]) {
		return 0;
	}
	bool icons_in_text;
	char *text = network_text(w, &icons_in_text);
	int width = text_item_measure(ctx, text, !icons_in_text);
	free(text);
	return width;
}

static void network_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct net_state *s = ((struct poll_data *)w->data)->state;
	double x = render_item_start(ctx, b);
	uint32_t fg = widget_fg(ctx->panel, "network");
	bool icons_in_text;
	char *text = network_text(w, &icons_in_text);
	if (!icons_in_text) {
		int g = glyph_size(ctx);
		int bars = s->quality > 75 ? 4 : s->quality > 50 ? 3 : s->quality > 25 ? 2 : 1;
		ti_network(ctx->panel, ctx->cairo, x, b.y + (b.height - g) / 2.0, g, bars, s->wireless,
			s->connected, fg);
		x += g + 4;
	}
	if (text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.x + b.width - x, b.height,
			fg, PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static char *network_tooltip(struct widget *w, struct hotspot *hs) {
	struct net_state *s = ((struct poll_data *)w->data)->state;
	if (!s->connected) {
		return format_str("%s: not connected", s->iface);
	}
	if (s->wireless) {
		return format_str("%s: %s (signal %d%%)", s->iface, s->address, s->quality);
	}
	return format_str("%s: %s", s->iface, s->address);
}

static bool network_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	if (opens_quick_settings(w)) {
		quicksettings_toggle(w->panel, anchor);
		return true;
	}
	flyout_network_toggle(w->panel, anchor, widget_conf(w, "settings", TW_NETWORK_SETTINGS));
	return true;
}

const struct widget_impl widget_network = {
	.type = "network",
	.init = network_init,
	.destroy = poll_destroy,
	.measure = network_measure,
	.render = network_render,
	.click = network_click,
	.tooltip = network_tooltip,
	.set_active = poll_set_active,
};

/* ================= brightness ================= */

struct brightness_state {
	bool present;
	int percent;
};

static void brightness_update(struct widget *w) {
	struct brightness_state *s = ((struct poll_data *)w->data)->state;
	s->present = false;
	DIR *dir = opendir("/sys/class/backlight");
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[512], cur[32], max[32];
		snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", de->d_name);
		bool ok = read_file(path, cur, sizeof(cur));
		snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", de->d_name);
		ok = ok && read_file(path, max, sizeof(max));
		if (ok && atoi(max) > 0) {
			s->percent = atoi(cur) * 100 / atoi(max);
			s->present = true;
			break;
		}
	}
	closedir(dir);
}

static void brightness_init(struct widget *w) {
	poll_init(w, 10, brightness_update, sizeof(struct brightness_state));
}

static char *brightness_text(struct widget *w, bool *icons_in_text) {
	struct brightness_state *s = ((struct poll_data *)w->data)->state;
	const char *format = widget_conf(w, "format", NULL);
	*icons_in_text = text_icons(w, format);
	if (!format) {
		return NULL;
	}
	char percent[16], icon[64];
	snprintf(percent, sizeof(percent), "%d", s->percent);
	level_icon(w, s->percent, icon, sizeof(icon));
	const char *values[] = { "percent", percent, "icon", icon, NULL };
	return format_text(format, values);
}

static int brightness_measure(struct widget *w, struct render_ctx *ctx) {
	struct brightness_state *s = ((struct poll_data *)w->data)->state;
	if (!s->present) {
		return 0;
	}
	bool icons_in_text;
	char *text = brightness_text(w, &icons_in_text);
	int width = text_item_measure(ctx, text, !icons_in_text);
	free(text);
	return width;
}

static void brightness_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	double x = render_item_start(ctx, b);
	uint32_t fg = widget_fg(ctx->panel, "brightness");
	bool icons_in_text;
	char *text = brightness_text(w, &icons_in_text);
	if (!icons_in_text) {
		int g = glyph_size(ctx);
		ti_brightness(ctx->panel, ctx->cairo, x, b.y + (b.height - g) / 2.0, g, fg);
		x += g + 4;
	}
	if (text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.x + b.width - x, b.height,
			fg, PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool brightness_scroll(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction) {
	proc_spawn(direction < 0 ? "brightnessctl -q set 5%+" : "brightnessctl -q set 5%-");
	brightness_update(w);
	panel_set_dirty(w->panel);
	return true;
}

static char *brightness_tooltip(struct widget *w, struct hotspot *hs) {
	struct brightness_state *s = ((struct poll_data *)w->data)->state;
	return format_str("Brightness: %d%%", s->percent);
}

const struct widget_impl widget_brightness = {
	.type = "brightness",
	.init = brightness_init,
	.destroy = poll_destroy,
	.measure = brightness_measure,
	.render = brightness_render,
	.click = power_click,
	.scroll = brightness_scroll,
	.tooltip = brightness_tooltip,
	.set_active = poll_set_active,
};

/* ================= volume (PulseAudio / PipeWire via pactl) ================= */

struct volume_data {
	struct proc *subscribe;
	struct proc *query;
	bool available;
	bool muted;
	int volume;
	struct loop_timer *debounce;
};

static void volume_query_done(void *data, const char *output) {
	struct widget *w = data;
	struct volume_data *d = w->data;
	d->query = NULL;
	const char *percent = strchr(output, '%');
	if (percent) {
		const char *start = percent;
		while (start > output && start[-1] >= '0' && start[-1] <= '9') {
			start--;
		}
		d->volume = atoi(start);
		d->available = true;
	}
	d->muted = strstr(output, "Mute: yes") != NULL;
	panel_set_dirty(w->panel);
}

static void volume_query(struct widget *w) {
	struct volume_data *d = w->data;
	if (d->query) {
		return;
	}
	d->query = proc_run(w->panel, "pactl get-sink-volume @DEFAULT_SINK@; "
		"pactl get-sink-mute @DEFAULT_SINK@", false, NULL, volume_query_done, w);
}

static void volume_debounced(void *data) {
	struct widget *w = data;
	struct volume_data *d = w->data;
	d->debounce = NULL;
	volume_query(w);
	flyout_volume_changed(w->panel);
}

static void volume_event_line(void *data, const char *line) {
	struct widget *w = data;
	struct volume_data *d = w->data;
	if ((strstr(line, "sink") || strstr(line, "server")) && !d->debounce) {
		d->debounce = loop_add_timer(w->panel->loop, 50, volume_debounced, w);
	}
}

static void volume_subscribe_done(void *data, const char *output) {
	struct widget *w = data;
	struct volume_data *d = w->data;
	d->subscribe = NULL;
}

static void volume_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct volume_data));
}

static void volume_set_active(struct widget *w, bool active) {
	struct volume_data *d = w->data;
	if (active && !d->subscribe) {
		d->subscribe = proc_run(w->panel, "exec pactl subscribe", true,
			volume_event_line, volume_subscribe_done, w);
		volume_query(w);
	} else if (!active) {
		proc_cancel(d->subscribe);
		d->subscribe = NULL;
		proc_cancel(d->query);
		d->query = NULL;
		if (d->debounce) {
			loop_remove_timer(w->panel->loop, d->debounce);
			d->debounce = NULL;
		}
	}
}

static void volume_destroy(struct widget *w) {
	volume_set_active(w, false);
	free(w->data);
}

static char *volume_text(struct widget *w, bool *icons_in_text) {
	struct volume_data *d = w->data;
	const char *format = d->muted ? widget_conf(w, "format_muted", NULL) : NULL;
	if (!format) {
		format = widget_conf(w, "format", NULL);
	}
	*icons_in_text = text_icons(w, format);
	if (!format) {
		return NULL;
	}
	char vol[16], icon[64];
	snprintf(vol, sizeof(vol), "%d", d->volume);
	level_icon(w, d->volume, icon, sizeof(icon));
	const char *values[] = { "volume", vol, "icon", icon, NULL };
	return format_text(format, values);
}

static int volume_measure(struct widget *w, struct render_ctx *ctx) {
	struct volume_data *d = w->data;
	if (!d->available) {
		return 0;
	}
	bool icons_in_text;
	char *text = volume_text(w, &icons_in_text);
	int width = text_item_measure(ctx, text, !icons_in_text);
	free(text);
	return width;
}

static void volume_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct volume_data *d = w->data;
	double x = render_item_start(ctx, b);
	uint32_t fg = widget_fg(ctx->panel, "volume");
	bool icons_in_text;
	char *text = volume_text(w, &icons_in_text);
	if (!icons_in_text) {
		int g = glyph_size(ctx);
		ti_speaker(ctx->panel, ctx->cairo, x, b.y + (b.height - g) / 2.0, g, d->volume, d->muted,
			fg);
		x += g + 4;
	}
	if (text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.x + b.width - x, b.height,
			fg, PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static bool volume_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct volume_data *d = w->data;
	if (button == BTN_LEFT) {
		struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
		anchor.right_align = true;
		if (opens_quick_settings(w)) {
			quicksettings_toggle(w->panel, anchor);
			return true;
		}
		flyout_volume_toggle(w->panel, anchor, widget_conf(w, "mixer", "exec pavucontrol"));
		return true;
	}
	if (button == BTN_MIDDLE) {
		proc_spawn("pactl set-sink-mute @DEFAULT_SINK@ toggle");
		return true;
	}
	if (button == BTN_RIGHT && !w->menu) {
		list_t *items = create_list();
		list_add(items, menu_item_new(d->muted ? "Unmute" : "Mute",
			"exec pactl set-sink-mute @DEFAULT_SINK@ toggle"));
		list_add(items, menu_item_new("Open volume mixer",
			widget_conf(w, "mixer", "exec pavucontrol")));
		struct popup_anchor anchor = popup_anchor_for_bar(s, (int)x, 0);
		menu_open(w->panel, items, true, anchor, NULL);
		return true;
	}
	return false;
}

static bool volume_scroll(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction) {
	int step = widget_conf_int(w, "step", 5);
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %c%d%%",
		direction < 0 ? '+' : '-', step);
	proc_spawn(cmd);
	return true;
}

static char *volume_tooltip(struct widget *w, struct hotspot *hs) {
	struct volume_data *d = w->data;
	return format_str("Volume: %d%%%s", d->volume, d->muted ? " (muted)" : "");
}

const struct widget_impl widget_volume = {
	.type = "volume",
	.init = volume_init,
	.destroy = volume_destroy,
	.measure = volume_measure,
	.render = volume_render,
	.click = volume_click,
	.scroll = volume_scroll,
	.tooltip = volume_tooltip,
	.set_active = volume_set_active,
};
