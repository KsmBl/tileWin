#include <dirent.h>
#include <limits.h>
#include <ifaddrs.h>
#include <linux/input-event-codes.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
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

/* ---------- meters: text, a history chart or a bar ---------- */

#define METER_HISTORY 32

enum meter_style {
	METER_TEXT,
	METER_GRAPH,
	METER_BAR,
};

/* Percentages of the last measurements, oldest at pos. */
struct meter_history {
	int values[METER_HISTORY];
	int pos;
};

static void meter_push(struct meter_history *h, int percent) {
	h->values[h->pos] = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	h->pos = (h->pos + 1) % METER_HISTORY;
}

static enum meter_style meter_style_of(struct widget *w) {
	const char *style = widget_conf(w, "style", "text");
	if (strcasecmp(style, "graph") == 0) {
		return METER_GRAPH;
	}
	if (strcasecmp(style, "bar") == 0) {
		return METER_BAR;
	}
	return METER_TEXT;
}

/*
 * The color of a meter at that percentage. "warning" and "critical" are the
 * levels it changes at and "warning_fg"/"critical_fg" the colors, from the
 * widget itself or, where it says nothing, from the theme.
 */
static uint32_t meter_fg(struct widget *w, struct render_ctx *ctx, int value, bool low_is_bad) {
	const struct tw_theme *t = ctx->panel->theme;
	const char *type = w->impl->type;
	char key[64];
	snprintf(key, sizeof(key), "%s.critical", type);
	int critical = widget_conf_int(w, "critical", tw_theme_int(t, key, -1));
	snprintf(key, sizeof(key), "%s.warning", type);
	int warning = widget_conf_int(w, "warning", tw_theme_int(t, key, -1));
	if (critical >= 0 && (low_is_bad ? value <= critical : value >= critical)) {
		snprintf(key, sizeof(key), "%s.critical_fg", type);
		return widget_conf_color(w, "critical_fg", tw_theme_color(t, key, 0xf87171ff));
	}
	if (warning >= 0 && (low_is_bad ? value <= warning : value >= warning)) {
		snprintf(key, sizeof(key), "%s.warning_fg", type);
		return widget_conf_color(w, "warning_fg", tw_theme_color(t, key, 0xfbbf24ff));
	}
	snprintf(key, sizeof(key), "%s.fg", type);
	return widget_conf_color(w, "fg", widget_fg(ctx->panel, type));
}

static int meter_measure(struct widget *w, struct render_ctx *ctx, const char *text) {
	enum meter_style style = meter_style_of(w);
	if (style == METER_GRAPH) {
		return widget_conf_int(w, "width", 44);
	}
	if (style == METER_BAR) {
		return widget_conf_int(w, "width", 14) + 2 * item_padding(ctx);
	}
	return text_item_measure(ctx, text, false);
}

/* Draws the meter; text is used by the text style and may be NULL. */
static void meter_render(struct widget *w, struct render_ctx *ctx, struct pbox b,
		const char *text, int percent, const struct meter_history *history,
		bool low_is_bad) {
	double x = render_item_start(ctx, b);
	uint32_t fg = meter_fg(w, ctx, percent, low_is_bad);
	cairo_t *cr = ctx->cairo;
	switch (meter_style_of(w)) {
	case METER_GRAPH: {
		double gh = b.height * 0.6, gy = b.y + (b.height - gh) / 2;
		double gw = b.width - 8, step = gw / (METER_HISTORY - 1);
		cairo_new_path(cr);
		cairo_move_to(cr, x - 2, gy + gh);
		for (int i = 0; i < METER_HISTORY; i++) {
			int v = history ? history->values[(history->pos + i) % METER_HISTORY] : 0;
			cairo_line_to(cr, x - 2 + i * step, gy + gh - gh * v / 100.0);
		}
		cairo_line_to(cr, x - 2 + gw, gy + gh);
		cairo_close_path(cr);
		pd_color(cr, fg);
		cairo_fill(cr);
		break;
	}
	case METER_BAR: {
		double bw = widget_conf_int(w, "width", 14);
		double bh = b.height * 0.62, by = b.y + (b.height - bh) / 2;
		double filled = bh * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0;
		pd_rect(cr, x, by, bw, bh, (fg & 0xffffff00) | 0x30);
		pd_rect(cr, x, by + bh - filled, bw, filled, fg);
		break;
	}
	case METER_TEXT:
		if (text && *text) {
			pd_text(cr, bar_font(ctx->panel), text, x, b.y, b.width - 12, b.height, fg, PD_LEFT);
		}
		break;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ================= cpu ================= */

/* ---------- what the processor is made of ---------- */

#define CPU_MAX_CPUS 512

struct cpu_topology {
	bool loaded;
	int threads, cores;
	int perf_cores, perf_threads;
	int eff_cores, eff_threads;
	bool hybrid, smt;
};

/* "0-5,8,10-11" */
static bool cpulist_contains(const char *list, int cpu) {
	for (const char *p = list; p && *p;) {
		char *end = NULL;
		long low = strtol(p, &end, 10);
		if (end == p) {
			break;
		}
		long high = low;
		if (*end == '-') {
			p = end + 1;
			high = strtol(p, &end, 10);
		}
		if (cpu >= low && cpu <= high) {
			return true;
		}
		p = *end == ',' ? end + 1 : "";
	}
	return false;
}

static int cpulist_count(const char *list) {
	int n = 0;
	for (const char *p = list; p && *p;) {
		char *end = NULL;
		long low = strtol(p, &end, 10);
		if (end == p) {
			break;
		}
		long high = low;
		if (*end == '-') {
			p = end + 1;
			high = strtol(p, &end, 10);
		}
		n += (int)(high - low + 1);
		p = *end == ',' ? end + 1 : "";
	}
	return n;
}

/*
 * Reads which logical processors share a core (SMT) and which of them are the
 * slower efficiency ones, either from the classes the kernel names or, where
 * it does not, from the capacity it gives each of them.
 */
static void topology_load(struct cpu_topology *t) {
	t->loaded = true;
	char efficiency[1024] = "";
	DIR *types = opendir("/sys/devices/system/cpu/types");
	struct dirent *de;
	while (types && (de = readdir(types))) {
		if (de->d_name[0] == '.' || !strstr(de->d_name, "atom")) {
			continue;
		}
		char path[NAME_MAX + 64], buf[1024];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/types/%s/cpulist", de->d_name);
		if (read_file(path, buf, sizeof(buf))) {
			size_t len = strlen(efficiency);
			snprintf(efficiency + len, sizeof(efficiency) - len, "%s%s", len ? "," : "", buf);
		}
	}
	if (types) {
		closedir(types);
	}

	// without named classes, the capacity tells them apart
	long capacities[CPU_MAX_CPUS];
	long best = 0;
	bool have_capacity = false;
	for (int cpu = 0; cpu < CPU_MAX_CPUS; cpu++) {
		char path[256], buf[64];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
		capacities[cpu] = read_file(path, buf, sizeof(buf)) ? atol(buf) : 0;
		if (capacities[cpu] > 0) {
			have_capacity = true;
			best = capacities[cpu] > best ? capacities[cpu] : best;
		}
	}

	for (int cpu = 0; cpu < CPU_MAX_CPUS; cpu++) {
		char path[256], siblings[256];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
			cpu);
		if (!read_file(path, siblings, sizeof(siblings))) {
			continue;
		}
		t->threads++;
		int count = cpulist_count(siblings);
		if (count > 1) {
			t->smt = true;
		}
		// the lowest of a group of siblings stands for the core
		bool primary = strtol(siblings, NULL, 10) == cpu;
		bool slow = efficiency[0] ? cpulist_contains(efficiency, cpu) :
			(have_capacity && best > 0 && capacities[cpu] > 0 && capacities[cpu] < best);
		if (slow) {
			t->eff_threads++;
			t->eff_cores += primary;
		} else {
			t->perf_threads++;
			t->perf_cores += primary;
		}
		t->cores += primary;
	}
	t->hybrid = t->eff_cores > 0 && t->perf_cores > 0;
}


struct cpu_state {
	unsigned long long last_total, last_idle;
	int usage;
	struct meter_history history;
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
	meter_push(&s->history, s->usage);
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

static int cpu_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = cpu_text(w);
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void cpu_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct cpu_state *s = ((struct poll_data *)w->data)->state;
	char *text = cpu_text(w);
	meter_render(w, ctx, b, text, s->usage, &s->history, false);
	free(text);
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
	static struct cpu_topology topology;
	if (!topology.loaded) {
		topology_load(&topology);
	}
	if (topology.hybrid) {
		return format_str("CPU usage: %d%%\n%d performance core%s (%d thread%s)\n"
			"%d efficiency core%s (%d thread%s)", s->usage,
			topology.perf_cores, topology.perf_cores == 1 ? "" : "s",
			topology.perf_threads, topology.perf_threads == 1 ? "" : "s",
			topology.eff_cores, topology.eff_cores == 1 ? "" : "s",
			topology.eff_threads, topology.eff_threads == 1 ? "" : "s");
	}
	if (topology.cores > 0) {
		return format_str("CPU usage: %d%%\n%d core%s, %d thread%s%s", s->usage,
			topology.cores, topology.cores == 1 ? "" : "s",
			topology.threads, topology.threads == 1 ? "" : "s",
			topology.smt ? " (two per core)" : "");
	}
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
	struct meter_history history;
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
	long long used = s->total_kb - s->available_kb;
	meter_push(&s->history, s->total_kb ? (int)(used * 100 / s->total_kb) : 0);
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
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void memory_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct mem_state *s = ((struct poll_data *)w->data)->state;
	char *text = memory_text(w);
	int percent = s->total_kb ? (int)((s->total_kb - s->available_kb) * 100 / s->total_kb) : 0;
	meter_render(w, ctx, b, text, percent, &s->history, false);
	free(text);
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

/* ================= disk ================= */

/*
 * Disk activity, like the drive lamp of a PC: the lamp lights up while the
 * watched disks move more than "threshold" KiB per second. "devices" picks
 * which disks are watched; without it every whole disk is (no partitions, no
 * loop or ram devices).
 */

#define DISK_MAX 16
#define DISK_DEFAULT_THRESHOLD 50 // KiB per second

struct disk_dev {
	char name[32];
	unsigned long long sectors; // read and written together, at the last sample
};

struct disk_state {
	struct disk_dev devices[DISK_MAX];
	int count;
	double kbps; // read and written together
	struct timespec sampled;
	bool sampled_once;
	bool busy;
};

/* Whether a name of /proc/diskstats is a whole disk worth watching. */
static bool disk_is_whole(const char *name) {
	static const char *const skip[] = { "loop", "ram", "zram", "dm-", "md", "sr", "fd" };
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strncmp(name, skip[i], strlen(skip[i])) == 0) {
			return false;
		}
	}
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
	return access(path, F_OK) != 0; // a partition has this file, a disk has not
}

static bool disk_wanted(struct widget *w, const char *name) {
	const char *list = widget_conf(w, "devices", NULL);
	if (!list || !*list) {
		return disk_is_whole(name);
	}
	size_t len = strlen(name);
	for (const char *p = list; *p;) {
		size_t n = strcspn(p, " ,\t");
		if (n == len && strncmp(p, name, n) == 0) {
			return true;
		}
		p += n;
		p += strspn(p, " ,\t");
	}
	return false;
}

static void disk_update(struct widget *w) {
	struct disk_state *s = ((struct poll_data *)w->data)->state;
	FILE *f = fopen("/proc/diskstats", "r");
	if (!f) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double seconds = s->sampled_once ? (now.tv_sec - s->sampled.tv_sec) +
		(now.tv_nsec - s->sampled.tv_nsec) / 1e9 : 0;
	unsigned long long moved = 0; // sectors since the last sample
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char name[32];
		unsigned long long reads, rmerged, sread, rms, writes, wmerged, swritten;
		if (sscanf(line, "%*u %*u %31s %llu %llu %llu %llu %llu %llu %llu", name, &reads,
				&rmerged, &sread, &rms, &writes, &wmerged, &swritten) != 8) {
			continue;
		}
		if (!disk_wanted(w, name)) {
			continue;
		}
		struct disk_dev *dev = NULL;
		for (int i = 0; i < s->count && !dev; i++) {
			if (strcmp(s->devices[i].name, name) == 0) {
				dev = &s->devices[i];
			}
		}
		if (!dev) {
			if (s->count == DISK_MAX) {
				continue;
			}
			dev = &s->devices[s->count++];
			snprintf(dev->name, sizeof(dev->name), "%s", name);
			dev->sectors = sread + swritten;
			continue; // the first sample only sets the starting point
		}
		unsigned long long sectors = sread + swritten;
		if (seconds > 0 && sectors >= dev->sectors) {
			moved += sectors - dev->sectors;
		}
		dev->sectors = sectors;
	}
	fclose(f);
	if (seconds > 0) {
		// a sector is 512 bytes, so two of them make a KiB
		s->kbps = moved / 2.0 / seconds;
	}
	s->sampled = now;
	s->sampled_once = true;
	int threshold = widget_conf_int(w, "threshold", DISK_DEFAULT_THRESHOLD);
	s->busy = s->kbps >= threshold;
}

static void disk_init(struct widget *w) {
	poll_init(w, 1, disk_update, sizeof(struct disk_state));
}

static void disk_rate_text(double kbps, char *buffer, size_t size) {
	if (kbps >= 1024 * 1024) {
		snprintf(buffer, size, "%.1f GB/s", kbps / (1024 * 1024));
	} else if (kbps >= 1024) {
		snprintf(buffer, size, "%.1f MB/s", kbps / 1024);
	} else {
		snprintf(buffer, size, "%.0f kB/s", kbps);
	}
}

static char *disk_text(struct widget *w) {
	struct disk_state *s = ((struct poll_data *)w->data)->state;
	char rate[32], kb[32];
	disk_rate_text(s->kbps, rate, sizeof(rate));
	snprintf(kb, sizeof(kb), "%.0f", s->kbps);
	const char *values[] = { "rate", rate, "kbps", kb, NULL };
	return format_text(widget_conf(w, "format", ""), values);
}

static int disk_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = disk_text(w);
	int width = text_item_measure(ctx, text, !text_icons(w, text));
	free(text);
	return width;
}

static void disk_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct disk_state *s = ((struct poll_data *)w->data)->state;
	double x = render_item_start(ctx, b);
	char *text = disk_text(w);
	uint32_t fg = widget_fg(ctx->panel, "disk");
	if (!text_icons(w, text)) {
		double size = glyph_size(ctx);
		// the lamp uses the accent color while the disks are busy
		uint32_t on = tw_theme_color(ctx->panel->theme, "disk.active_fg",
			tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x0078d4ff));
		ti_disk(ctx->panel, ctx->cairo, x, b.y + (b.height - size) / 2, size, s->busy,
			s->busy ? on : fg);
		x += size + 4;
	}
	if (text && *text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y,
			b.width - (x - b.x) - item_padding(ctx), b.height, fg, PD_LEFT);
	}
	free(text);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static char *disk_tooltip(struct widget *w, struct hotspot *hs) {
	struct disk_state *s = ((struct poll_data *)w->data)->state;
	char rate[32];
	disk_rate_text(s->kbps, rate, sizeof(rate));
	if (s->count == 0) {
		return format_str("No disk is being watched");
	}
	char names[256] = "";
	for (int i = 0; i < s->count; i++) {
		size_t len = strlen(names);
		snprintf(names + len, sizeof(names) - len, "%s%s", len ? ", " : "", s->devices[i].name);
	}
	return format_str("Disk activity: %s (%s)", rate, names);
}

const struct widget_impl widget_disk = {
	.type = "disk",
	.init = disk_init,
	.destroy = poll_destroy,
	.measure = disk_measure,
	.render = disk_render,
	.tooltip = disk_tooltip,
	.set_active = poll_set_active,
};

/* ================= gpu ================= */

/*
 * How busy a graphics card is. AMD and some others report it in sysfs as
 * gpu_busy_percent; for the rest the widget runs the command in "command"
 * (e.g. nvidia-smi) and reads a number from its output. "device card1" picks
 * the card, otherwise the first one that reports anything is taken.
 */

struct gpu_state {
	int percent;
	char card[32];
	struct meter_history history;
	struct proc *running;
	struct widget *widget;
};

static bool gpu_read_sysfs(const char *card, int *percent) {
	char path[256], buf[64];
	snprintf(path, sizeof(path), "/sys/class/drm/%s/device/gpu_busy_percent", card);
	if (!read_file(path, buf, sizeof(buf))) {
		return false;
	}
	*percent = atoi(buf);
	return true;
}

static void gpu_command_done(void *data, const char *output) {
	struct widget *w = data;
	struct gpu_state *s = ((struct poll_data *)w->data)->state;
	s->running = NULL;
	const char *p = output;
	while (*p && (*p < '0' || *p > '9')) {
		p++;
	}
	if (*p) {
		s->percent = atoi(p);
		s->percent = s->percent < 0 ? 0 : s->percent > 100 ? 100 : s->percent;
	}
	meter_push(&s->history, s->percent);
	panel_set_dirty(w->panel);
}

static void gpu_update(struct widget *w) {
	struct gpu_state *s = ((struct poll_data *)w->data)->state;
	s->widget = w;
	const char *command = widget_conf(w, "command", NULL);
	if (command) {
		if (!s->running) {
			s->running = proc_run(w->panel, command, false, NULL, gpu_command_done, w);
		}
		return;
	}
	const char *device = widget_conf(w, "device", NULL);
	if (device) {
		snprintf(s->card, sizeof(s->card), "%s", device);
	} else if (!s->card[0]) {
		for (int i = 0; i < 8 && !s->card[0]; i++) {
			char card[32];
			int percent;
			snprintf(card, sizeof(card), "card%d", i);
			if (gpu_read_sysfs(card, &percent)) {
				snprintf(s->card, sizeof(s->card), "%s", card);
			}
		}
	}
	if (s->card[0]) {
		gpu_read_sysfs(s->card, &s->percent);
	}
	meter_push(&s->history, s->percent);
}

static void gpu_init(struct widget *w) {
	poll_init(w, 2, gpu_update, sizeof(struct gpu_state));
}

static char *gpu_text(struct widget *w) {
	struct gpu_state *s = ((struct poll_data *)w->data)->state;
	char usage[16];
	snprintf(usage, sizeof(usage), "%d", s->percent);
	const char *values[] = { "usage", usage, NULL };
	return format_text(widget_conf(w, "format", "GPU {usage}%"), values);
}

static int gpu_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = gpu_text(w);
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void gpu_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct gpu_state *s = ((struct poll_data *)w->data)->state;
	char *text = gpu_text(w);
	meter_render(w, ctx, b, text, s->percent, &s->history, false);
	free(text);
}

static char *gpu_tooltip(struct widget *w, struct hotspot *hs) {
	struct gpu_state *s = ((struct poll_data *)w->data)->state;
	return format_str("Graphics: %d%%%s%s", s->percent, s->card[0] ? " on " : "", s->card);
}

const struct widget_impl widget_gpu = {
	.type = "gpu",
	.init = gpu_init,
	.destroy = poll_destroy,
	.measure = gpu_measure,
	.render = gpu_render,
	.tooltip = gpu_tooltip,
	.set_active = poll_set_active,
};

/* ================= net ================= */

/*
 * What goes through a network interface. "device wlan0" picks it, otherwise
 * the busiest one is followed. The graph and the bar are drawn against
 * "max_rate" (KiB per second) so they have a scale.
 */

struct nm_state {
	char device[32];
	unsigned long long rx, tx;
	double rx_rate, tx_rate; // KiB per second
	struct timespec sampled;
	bool sampled_once;
	struct meter_history history;
};

static bool nm_counters(const char *device, unsigned long long *rx, unsigned long long *tx) {
	char path[256], buf[64];
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", device);
	if (!read_file(path, buf, sizeof(buf))) {
		return false;
	}
	*rx = strtoull(buf, NULL, 10);
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", device);
	*tx = read_file(path, buf, sizeof(buf)) ? strtoull(buf, NULL, 10) : 0;
	return true;
}

/* The interface carrying the most, ignoring loopback and down ones. */
static void nm_pick_device(struct nm_state *s) {
	DIR *dir = opendir("/sys/class/net");
	struct dirent *de;
	unsigned long long best = 0;
	while (dir && (de = readdir(dir))) {
		if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0) {
			continue;
		}
		char path[NAME_MAX + 64], buf[64];
		snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", de->d_name);
		if (read_file(path, buf, sizeof(buf)) && strcmp(buf, "up") != 0) {
			continue;
		}
		unsigned long long rx = 0, tx = 0;
		if (nm_counters(de->d_name, &rx, &tx) && rx + tx >= best) {
			best = rx + tx;
			snprintf(s->device, sizeof(s->device), "%.*s", (int)sizeof(s->device) - 1,
				de->d_name);
		}
	}
	if (dir) {
		closedir(dir);
	}
}

static void nm_update(struct widget *w) {
	struct nm_state *s = ((struct poll_data *)w->data)->state;
	const char *device = widget_conf(w, "device", NULL);
	if (device) {
		snprintf(s->device, sizeof(s->device), "%s", device);
	} else {
		nm_pick_device(s);
	}
	unsigned long long rx = 0, tx = 0;
	if (!s->device[0] || !nm_counters(s->device, &rx, &tx)) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double seconds = s->sampled_once ? (now.tv_sec - s->sampled.tv_sec) +
		(now.tv_nsec - s->sampled.tv_nsec) / 1e9 : 0;
	if (seconds > 0 && rx >= s->rx && tx >= s->tx) {
		s->rx_rate = (rx - s->rx) / 1024.0 / seconds;
		s->tx_rate = (tx - s->tx) / 1024.0 / seconds;
	}
	s->rx = rx;
	s->tx = tx;
	s->sampled = now;
	s->sampled_once = true;
	int max = widget_conf_int(w, "max_rate", 12500);
	double total = s->rx_rate + s->tx_rate;
	meter_push(&s->history, max > 0 ? (int)(total * 100 / max) : 0);
}

static void nm_init(struct widget *w) {
	poll_init(w, 2, nm_update, sizeof(struct nm_state));
}

static void nm_rate_text(double kbps, char *buffer, size_t size) {
	if (kbps >= 1024) {
		snprintf(buffer, size, "%.1f MB/s", kbps / 1024);
	} else {
		snprintf(buffer, size, "%.0f kB/s", kbps);
	}
}

static char *nm_text(struct widget *w) {
	struct nm_state *s = ((struct poll_data *)w->data)->state;
	char down[32], up[32], total[32];
	nm_rate_text(s->rx_rate, down, sizeof(down));
	nm_rate_text(s->tx_rate, up, sizeof(up));
	nm_rate_text(s->rx_rate + s->tx_rate, total, sizeof(total));
	const char *values[] = { "down", down, "up", up, "total", total,
		"device", s->device, NULL };
	return format_text(widget_conf(w, "format", "↓ {down} ↑ {up}"), values);
}

static int nm_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = nm_text(w);
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void nm_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct nm_state *s = ((struct poll_data *)w->data)->state;
	char *text = nm_text(w);
	int max = widget_conf_int(w, "max_rate", 12500);
	double total = s->rx_rate + s->tx_rate;
	meter_render(w, ctx, b, text, max > 0 ? (int)(total * 100 / max) : 0, &s->history, false);
	free(text);
}

static char *nm_tooltip(struct widget *w, struct hotspot *hs) {
	struct nm_state *s = ((struct poll_data *)w->data)->state;
	char down[32], up[32];
	nm_rate_text(s->rx_rate, down, sizeof(down));
	nm_rate_text(s->tx_rate, up, sizeof(up));
	return format_str("%s\nDown %s\nUp %s", s->device[0] ? s->device : "No interface", down, up);
}

const struct widget_impl widget_net = {
	.type = "net",
	.init = nm_init,
	.destroy = poll_destroy,
	.measure = nm_measure,
	.render = nm_render,
	.tooltip = nm_tooltip,
	.set_active = poll_set_active,
};

/* ================= storage ================= */

/* How full a file system is. "path /home" picks it, default is "/". */

struct storage_state {
	char path[256];
	int percent;
	double used_gb, total_gb;
	struct meter_history history;
};

static void storage_update(struct widget *w) {
	struct storage_state *s = ((struct poll_data *)w->data)->state;
	snprintf(s->path, sizeof(s->path), "%s", widget_conf(w, "path", "/"));
	struct statvfs st;
	if (statvfs(s->path, &st) != 0 || st.f_blocks == 0) {
		return;
	}
	double total = (double)st.f_blocks * st.f_frsize;
	double free_bytes = (double)st.f_bavail * st.f_frsize;
	s->total_gb = total / (1024.0 * 1024 * 1024);
	s->used_gb = (total - free_bytes) / (1024.0 * 1024 * 1024);
	s->percent = total > 0 ? (int)((total - free_bytes) * 100 / total) : 0;
	meter_push(&s->history, s->percent);
}

static void storage_init(struct widget *w) {
	poll_init(w, 30, storage_update, sizeof(struct storage_state));
}

static char *storage_text(struct widget *w) {
	struct storage_state *s = ((struct poll_data *)w->data)->state;
	char percent[16], used[32], total[32], freed[32];
	snprintf(percent, sizeof(percent), "%d", s->percent);
	snprintf(used, sizeof(used), "%.1f", s->used_gb);
	snprintf(total, sizeof(total), "%.1f", s->total_gb);
	snprintf(freed, sizeof(freed), "%.1f", s->total_gb - s->used_gb);
	const char *values[] = { "used_percent", percent, "used", used, "total", total,
		"free", freed, "path", s->path, NULL };
	return format_text(widget_conf(w, "format", "{path} {used_percent}%"), values);
}

static int storage_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = storage_text(w);
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void storage_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct storage_state *s = ((struct poll_data *)w->data)->state;
	char *text = storage_text(w);
	meter_render(w, ctx, b, text, s->percent, &s->history, false);
	free(text);
}

static char *storage_tooltip(struct widget *w, struct hotspot *hs) {
	struct storage_state *s = ((struct poll_data *)w->data)->state;
	return format_str("%s\n%.1f GiB of %.1f GiB used (%d%%)", s->path, s->used_gb,
		s->total_gb, s->percent);
}

const struct widget_impl widget_storage = {
	.type = "storage",
	.init = storage_init,
	.destroy = poll_destroy,
	.measure = storage_measure,
	.render = storage_render,
	.tooltip = storage_tooltip,
	.set_active = poll_set_active,
};

/* ================= power ================= */

/*
 * What the computer is drawing, from the battery. Some report the power
 * straight away, others the current and the voltage it is drawn at.
 */

struct power_state {
	char device[32];
	double watts;
	bool charging;
	struct meter_history history;
};

static void power_update(struct widget *w) {
	struct power_state *s = ((struct poll_data *)w->data)->state;
	const char *device = widget_conf(w, "device", NULL);
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *de;
	s->watts = 0;
	while (dir && (de = readdir(dir))) {
		if (de->d_name[0] == '.' || (device && strcmp(de->d_name, device) != 0)) {
			continue;
		}
		char path[512], buf[64];
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
		if (!read_file(path, buf, sizeof(buf)) || strcmp(buf, "Battery") != 0) {
			continue;
		}
		double micro_watts = 0;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/power_now", de->d_name);
		if (read_file(path, buf, sizeof(buf))) {
			micro_watts = atof(buf);
		} else {
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/current_now", de->d_name);
			double current = read_file(path, buf, sizeof(buf)) ? atof(buf) : 0;
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/voltage_now", de->d_name);
			double voltage = read_file(path, buf, sizeof(buf)) ? atof(buf) : 0;
			micro_watts = current * voltage / 1e6;
		}
		if (micro_watts <= 0) {
			continue;
		}
		s->watts = micro_watts / 1e6;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
		s->charging = read_file(path, buf, sizeof(buf)) && strcmp(buf, "Charging") == 0;
		snprintf(s->device, sizeof(s->device), "%.*s", (int)sizeof(s->device) - 1,
				de->d_name);
		break;
	}
	if (dir) {
		closedir(dir);
	}
	int max = widget_conf_int(w, "max_watts", 60);
	meter_push(&s->history, max > 0 ? (int)(s->watts * 100 / max) : 0);
}

static void power_init(struct widget *w) {
	poll_init(w, 5, power_update, sizeof(struct power_state));
}

static char *power_text(struct widget *w) {
	struct power_state *s = ((struct poll_data *)w->data)->state;
	char watts[32];
	snprintf(watts, sizeof(watts), "%.1f", s->watts);
	const char *values[] = { "watts", watts, NULL };
	return format_text(widget_conf(w, "format", "{watts} W"), values);
}

static int power_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = power_text(w);
	int width = meter_measure(w, ctx, text);
	free(text);
	return width;
}

static void power_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct power_state *s = ((struct poll_data *)w->data)->state;
	char *text = power_text(w);
	int max = widget_conf_int(w, "max_watts", 60);
	meter_render(w, ctx, b, text, max > 0 ? (int)(s->watts * 100 / max) : 0, &s->history, false);
	free(text);
}

static char *power_tooltip(struct widget *w, struct hotspot *hs) {
	struct power_state *s = ((struct poll_data *)w->data)->state;
	if (s->watts <= 0) {
		return format_str("No power reading");
	}
	return format_str("%s %.1f W%s", s->charging ? "Charging at" : "Drawing", s->watts,
		s->device[0] ? "" : "");
}

const struct widget_impl widget_power = {
	.type = "power",
	.init = power_init,
	.destroy = poll_destroy,
	.measure = power_measure,
	.render = power_render,
	.tooltip = power_tooltip,
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
		if (!read_file(path, buf, sizeof(buf))) {
			// batteries of wireless mice and keyboards report no percentage;
			// keep looking, the battery of the computer comes later
			continue;
		}
		s->capacity = atoi(buf);
		s->present = true;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
		if (!read_file(path, s->status, sizeof(s->status))) {
			s->status[0] = '\0';
		}
		snprintf(s->device, sizeof(s->device), "%.*s", (int)sizeof(s->device) - 1,
				de->d_name);
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
	uint32_t fg = meter_fg(w, ctx, s->capacity, true);
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
