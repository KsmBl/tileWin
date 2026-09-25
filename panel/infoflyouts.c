#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <ifaddrs.h>
#include <json.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "flyout.h"
#include "ipc.h"
#include "popup.h"
#include "stringop.h"
#include "tw_disks.h"
#include "tw_paths.h"

/*
 * The flyouts of the widgets that had none: a click on the disk lamp, the GPU,
 * network usage, disk space, power draw, keyboard layout, window title, git
 * and script widgets opens one. They share one shape, like the CPU and memory
 * flyouts: a headline with the reading, the last minute as a chart where that
 * means something, a list of rows (some of which can be clicked) and links at
 * the bottom. Each widget type only says where its figures come from.
 *
 * Nothing runs while no flyout is open; an open one reads its figures again
 * every second or so, and stops the moment it closes.
 */

#define WIDTH 360
#define PAD 16
#define HEADER 66
#define GRAPH 112
#define ROWS_TITLE 30
#define ROW 28
#define BAR_ROW 36
#define FOOTER 44
#define HISTORY 60
#define MAX_ROWS 24
#define MAX_ACTIONS 3

enum info_hotspot {
	HS_ROW = 1,
	HS_ACTION,
};

struct info_flyout;

struct info_row {
	char label[128], value[96];
	double bar;    // 0 to 1, below 0 for none
	bool full_is_bad; // the bar turns red when nearly full (disk space, video memory)
	char *command; // run when the row is clicked, NULL when it cannot be
	bool marked;   // the one in use, e.g. the active keyboard layout
	bool heading;  // names the rows below it
};

struct info_action {
	char label[48];
	char *command;
	void (*run)(struct info_flyout *f);
};

struct info_source {
	const char *type;
	int interval_ms; // how often the figures are read again, 0 for never
	void (*sample)(struct info_flyout *f);
	void (*free_state)(void *state);
};

struct info_flyout {
	struct panel *panel;
	struct popup *popup;
	struct popup_anchor anchor;
	struct widget *widget;
	const struct info_source *source;
	double px, py;
	bool inside;

	char big[32], title[160], subtitle[200];
	bool graph, two_series;
	double history[2][HISTORY];
	int history_len;
	double graph_max; // 0 scales to the highest reading
	char graph_note[64], series_names[2][24];

	char rows_title[64];
	struct info_row rows[MAX_ROWS];
	int row_count;
	struct info_action actions[MAX_ACTIONS];
	int action_count;

	struct loop_timer *tick;
	void *state;
};

static struct info_flyout *current;

static void refill(struct info_flyout *f);

/* ---------- filling one in ---------- */

static void clear_content(struct info_flyout *f) {
	for (int i = 0; i < f->row_count; i++) {
		free(f->rows[i].command);
	}
	for (int i = 0; i < f->action_count; i++) {
		free(f->actions[i].command);
	}
	f->row_count = f->action_count = 0;
	f->big[0] = f->title[0] = f->subtitle[0] = f->rows_title[0] = '\0';
}

static struct info_row *add_row(struct info_flyout *f, const char *label, const char *value) {
	if (f->row_count == MAX_ROWS) {
		return NULL;
	}
	struct info_row *row = &f->rows[f->row_count++];
	memset(row, 0, sizeof(*row));
	snprintf(row->label, sizeof(row->label), "%s", label ? label : "");
	snprintf(row->value, sizeof(row->value), "%s", value ? value : "");
	row->bar = -1;
	return row;
}

static void add_heading(struct info_flyout *f, const char *text) {
	struct info_row *row = add_row(f, text, NULL);
	if (row) {
		row->heading = true;
	}
}

static void add_action(struct info_flyout *f, const char *label, const char *command,
		void (*run)(struct info_flyout *f)) {
	if (f->action_count == MAX_ACTIONS) {
		return;
	}
	struct info_action *a = &f->actions[f->action_count++];
	snprintf(a->label, sizeof(a->label), "%s", label);
	a->command = command ? strdup(command) : NULL;
	a->run = run;
}

static void push_history(struct info_flyout *f, double first, double second) {
	if (f->history_len == HISTORY) {
		memmove(f->history[0], f->history[0] + 1, (HISTORY - 1) * sizeof(double));
		memmove(f->history[1], f->history[1] + 1, (HISTORY - 1) * sizeof(double));
		f->history_len--;
	}
	f->history[0][f->history_len] = first;
	f->history[1][f->history_len] = second;
	f->history_len++;
}

static void format_bytes(char *out, size_t size, double bytes) {
	static const char *const units[] = { "B", "kB", "MB", "GB", "TB" };
	int unit = 0;
	while (bytes >= 1000 && unit < 4) {
		bytes /= 1000;
		unit++;
	}
	snprintf(out, size, unit == 0 || bytes >= 100 ? "%.0f %s" : "%.1f %s", bytes, units[unit]);
}

static void format_rate(char *out, size_t size, double bytes_per_second) {
	char amount[32];
	format_bytes(amount, sizeof(amount), bytes_per_second);
	snprintf(out, size, "%s/s", amount);
}

static bool read_line(const char *path, char *out, size_t size) {
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	bool ok = fgets(out, size, file) != NULL;
	fclose(file);
	if (ok) {
		out[strcspn(out, "\n")] = '\0';
	}
	return ok;
}

static long long read_number(const char *path, long long fallback) {
	char line[64];
	return read_line(path, line, sizeof(line)) ? atoll(line) : fallback;
}

static double seconds_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---------- disk activity ---------- */

#define DISK_MAX 16

struct disk_state {
	int count;
	struct {
		char name[32], label[128];
		unsigned long long read, written; // sectors at the last reading
		double read_rate, write_rate;     // bytes per second
	} disks[DISK_MAX];
	double sampled;
};

static void disk_sample(struct info_flyout *f) {
	struct disk_state *s = f->state;
	if (!s) {
		s = f->state = calloc(1, sizeof(*s));
		struct tw_disk *disks;
		size_t count = tw_disks_list(TW_BLOCK_DIR, &disks);
		for (size_t i = 0; i < count && s->count < DISK_MAX; i++) {
			snprintf(s->disks[s->count].name, sizeof(s->disks[0].name), "%s", disks[i].name);
			char *label = tw_disk_label(&disks[i]);
			snprintf(s->disks[s->count].label, sizeof(s->disks[0].label), "%s", label);
			free(label);
			s->count++;
		}
		tw_disks_free(disks, count);
	}
	double now = seconds_now(), elapsed = s->sampled > 0 ? now - s->sampled : 0;
	FILE *file = fopen("/proc/diskstats", "r");
	char line[512];
	unsigned long long total_read = 0, total_written = 0;
	while (file && fgets(line, sizeof(line), file)) {
		char name[32];
		unsigned long long v[7];
		if (sscanf(line, "%*u %*u %31s %llu %llu %llu %llu %llu %llu %llu", name, &v[0], &v[1],
				&v[2], &v[3], &v[4], &v[5], &v[6]) != 8) {
			continue;
		}
		for (int i = 0; i < s->count; i++) {
			if (strcmp(s->disks[i].name, name) != 0) {
				continue;
			}
			unsigned long long read = v[2], written = v[6];
			if (elapsed > 0) {
				s->disks[i].read_rate = (read - s->disks[i].read) * 512.0 / elapsed;
				s->disks[i].write_rate = (written - s->disks[i].written) * 512.0 / elapsed;
			}
			s->disks[i].read = read;
			s->disks[i].written = written;
			total_read += read;
			total_written += written;
		}
	}
	if (file) {
		fclose(file);
	}
	s->sampled = now;

	double reading = 0, writing = 0;
	for (int i = 0; i < s->count; i++) {
		reading += s->disks[i].read_rate;
		writing += s->disks[i].write_rate;
	}
	push_history(f, reading, writing);
	char read_text[32], write_text[32], text[96];
	format_rate(read_text, sizeof(read_text), reading);
	format_rate(write_text, sizeof(write_text), writing);
	format_rate(f->big, sizeof(f->big), reading + writing);
	snprintf(f->title, sizeof(f->title), "Disk activity");
	snprintf(f->subtitle, sizeof(f->subtitle), "Reading %s, writing %s", read_text, write_text);
	f->graph = f->two_series = true;
	snprintf(f->graph_note, sizeof(f->graph_note), "60 seconds");
	snprintf(f->series_names[0], sizeof(f->series_names[0]), "Read");
	snprintf(f->series_names[1], sizeof(f->series_names[1]), "Written");
	snprintf(f->rows_title, sizeof(f->rows_title), "Disks");
	for (int i = 0; i < s->count; i++) {
		format_rate(read_text, sizeof(read_text), s->disks[i].read_rate);
		format_rate(write_text, sizeof(write_text), s->disks[i].write_rate);
		snprintf(text, sizeof(text), "↓ %s  ↑ %s", read_text, write_text);
		add_row(f, s->disks[i].label, text);
	}
	if (s->count == 0) {
		add_row(f, "No disk found", NULL);
	}
	format_bytes(read_text, sizeof(read_text), total_read * 512.0);
	format_bytes(write_text, sizeof(write_text), total_written * 512.0);
	snprintf(text, sizeof(text), "%s read, %s written", read_text, write_text);
	add_row(f, "Since the computer started", text);
	add_action(f, "Open Task Manager", "exec $taskmanager", NULL);
}

/* ---------- graphics card ---------- */

struct gpu_state {
	char card[256]; // /sys/class/drm/cardN/device, empty when none reports
	char nvidia[256]; // the last answer of nvidia-smi
	struct proc *proc;
};

static void gpu_find(struct info_flyout *f, struct gpu_state *s) {
	const char *wanted = widget_conf(f->widget, "device", NULL);
	DIR *dir = opendir("/sys/class/drm");
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') ||
				(wanted && strcmp(entry->d_name, wanted) != 0)) {
			continue;
		}
		char path[256];
		snprintf(path, sizeof(path), "/sys/class/drm/%.200s/device", entry->d_name);
		char probe[320];
		snprintf(probe, sizeof(probe), "%s/gpu_busy_percent", path);
		bool reports = access(probe, R_OK) == 0;
		snprintf(probe, sizeof(probe), "/sys/class/drm/%s/gt_cur_freq_mhz", entry->d_name);
		reports = reports || access(probe, R_OK) == 0;
		if (reports || (wanted && !s->card[0])) {
			snprintf(s->card, sizeof(s->card), "%s", path);
			if (reports) {
				break;
			}
		}
	}
	if (dir) {
		closedir(dir);
	}
}

static void gpu_nvidia_done(void *data, const char *output) {
	struct info_flyout *f = data;
	if (current != f || !f->state) {
		return;
	}
	struct gpu_state *s = f->state;
	s->proc = NULL;
	snprintf(s->nvidia, sizeof(s->nvidia), "%s", output ? output : "");
}

/* The first file of the card's hwmon folder with that name. */
static long long gpu_hwmon(struct gpu_state *s, const char *file) {
	char path[320];
	snprintf(path, sizeof(path), "%s/hwmon", s->card);
	DIR *dir = opendir(path);
	struct dirent *entry;
	long long value = -1;
	while (dir && value < 0 && (entry = readdir(dir))) {
		if (entry->d_name[0] != '.') {
			char full[600];
			snprintf(full, sizeof(full), "%s/%s/%s", path, entry->d_name, file);
			value = read_number(full, -1);
		}
	}
	if (dir) {
		closedir(dir);
	}
	return value;
}

static void gpu_sample(struct info_flyout *f) {
	struct gpu_state *s = f->state;
	if (!s) {
		s = f->state = calloc(1, sizeof(*s));
		gpu_find(f, s);
	}
	char path[400], text[96], line[256];
	int usage = -1;
	snprintf(f->title, sizeof(f->title), "Graphics card");
	f->graph = true;
	f->graph_max = 100;
	snprintf(f->graph_note, sizeof(f->graph_note), "60 seconds");
	if (s->card[0]) {
		snprintf(path, sizeof(path), "%s/gpu_busy_percent", s->card);
		usage = (int)read_number(path, -1);
		char driver[256] = "";
		snprintf(path, sizeof(path), "%s/driver", s->card);
		ssize_t n = readlink(path, driver, sizeof(driver) - 1);
		driver[n > 0 ? n : 0] = '\0';
		const char *slash = strrchr(driver, '/');
		char vendor[16] = "", device[16] = "";
		snprintf(path, sizeof(path), "%s/vendor", s->card);
		read_line(path, vendor, sizeof(vendor));
		snprintf(path, sizeof(path), "%s/device", s->card);
		read_line(path, device, sizeof(device));
		snprintf(f->subtitle, sizeof(f->subtitle), "%s · %s:%s", slash ? slash + 1 : "card",
			vendor[0] ? vendor + 2 : "?", device[0] ? device + 2 : "?");
		snprintf(path, sizeof(path), "%s/mem_info_vram_used", s->card);
		long long used = read_number(path, -1);
		snprintf(path, sizeof(path), "%s/mem_info_vram_total", s->card);
		long long total = read_number(path, -1);
		if (used >= 0 && total > 0) {
			char a[32], b[32];
			format_bytes(a, sizeof(a), used);
			format_bytes(b, sizeof(b), total);
			snprintf(text, sizeof(text), "%s of %s", a, b);
			struct info_row *row = add_row(f, "Video memory", text);
			if (row) {
				row->bar = (double)used / total;
				row->full_is_bad = true;
			}
		}
		long long temp = gpu_hwmon(s, "temp1_input");
		if (temp > 0) {
			snprintf(text, sizeof(text), "%.0f °C", temp / 1000.0);
			add_row(f, "Temperature", text);
		}
		long long power = gpu_hwmon(s, "power1_average");
		if (power < 0) {
			power = gpu_hwmon(s, "power1_input");
		}
		if (power > 0) {
			snprintf(text, sizeof(text), "%.1f W", power / 1e6);
			add_row(f, "Power", text);
		}
		// Intel reports its clock instead of a load
		const char *card_dir = s->card;
		char gt[400];
		snprintf(gt, sizeof(gt), "%s/../gt_cur_freq_mhz", card_dir);
		long long freq = read_number(gt, -1);
		if (freq < 0) {
			char base[256];
			snprintf(base, sizeof(base), "%s", card_dir);
			char *dev = strrchr(base, '/');
			if (dev) {
				*dev = '\0';
			}
			snprintf(gt, sizeof(gt), "%s/gt_cur_freq_mhz", base);
			freq = read_number(gt, -1);
			snprintf(gt, sizeof(gt), "%s/gt_max_freq_mhz", base);
		} else {
			snprintf(gt, sizeof(gt), "%s/../gt_max_freq_mhz", card_dir);
		}
		long long max = read_number(gt, -1);
		if (freq > 0) {
			snprintf(text, sizeof(text), max > 0 ? "%lld of %lld MHz" : "%lld MHz", freq, max);
			add_row(f, "Clock", text);
			if (usage < 0 && max > 0) {
				usage = (int)(freq * 100 / max);
			}
		}
	}
	if (usage < 0 && tw_in_path("nvidia-smi")) {
		// cards that tell /sys nothing: ask their tool, in the background
		if (!s->proc) {
			s->proc = proc_run(f->panel, "nvidia-smi --query-gpu=name,utilization.gpu,"
				"memory.used,memory.total,temperature.gpu,power.draw --format=csv,noheader,nounits",
				false, NULL, gpu_nvidia_done, f);
		}
		char name[128];
		double load, mem_used, mem_total, temp, watts;
		snprintf(line, sizeof(line), "%s", s->nvidia);
		if (sscanf(line, "%127[^,], %lf, %lf, %lf, %lf, %lf", name, &load, &mem_used, &mem_total,
				&temp, &watts) >= 4) {
			usage = (int)load;
			snprintf(f->subtitle, sizeof(f->subtitle), "%s", name);
			snprintf(text, sizeof(text), "%.0f of %.0f MiB", mem_used, mem_total);
			struct info_row *row = add_row(f, "Video memory", text);
			if (row && mem_total > 0) {
				row->bar = mem_used / mem_total;
				row->full_is_bad = true;
			}
			snprintf(text, sizeof(text), "%.0f °C", temp);
			add_row(f, "Temperature", text);
			snprintf(text, sizeof(text), "%.1f W", watts);
			add_row(f, "Power", text);
		}
	}
	if (usage >= 0) {
		snprintf(f->big, sizeof(f->big), "%d%%", usage);
		push_history(f, usage, 0);
	} else {
		snprintf(f->big, sizeof(f->big), "–");
		if (!f->subtitle[0]) {
			snprintf(f->subtitle, sizeof(f->subtitle), "No card reports its load");
		}
		f->graph = false;
	}
	add_action(f, "Open Task Manager", "exec $taskmanager", NULL);
}

static void gpu_free(void *data) {
	struct gpu_state *s = data;
	if (s->proc) {
		proc_cancel(s->proc);
	}
	free(s);
}

/* ---------- network usage ---------- */

#define NET_MAX 12

struct net_state {
	int count;
	struct {
		char name[IF_NAMESIZE + 1];
		unsigned long long rx, tx;
		double down, up;
	} ifaces[NET_MAX];
	double sampled;
};

static void net_address(const char *iface, char *out, size_t size) {
	out[0] = '\0';
	struct ifaddrs *list;
	if (getifaddrs(&list) != 0) {
		return;
	}
	for (struct ifaddrs *a = list; a; a = a->ifa_next) {
		if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, iface) == 0) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, out, size);
			break;
		}
	}
	freeifaddrs(list);
}

static void net_sample(struct info_flyout *f) {
	struct net_state *s = f->state;
	if (!s) {
		s = f->state = calloc(1, sizeof(*s));
	}
	double now = seconds_now(), elapsed = s->sampled > 0 ? now - s->sampled : 0;
	DIR *dir = opendir("/sys/class/net");
	struct dirent *entry;
	unsigned long long all_rx = 0, all_tx = 0;
	double down = 0, up = 0;
	while (dir && (entry = readdir(dir))) {
		if (entry->d_name[0] == '.' || strcmp(entry->d_name, "lo") == 0) {
			continue;
		}
		char path[512], state[32] = "";
		snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", entry->d_name);
		read_line(path, state, sizeof(state));
		if (strcmp(state, "up") != 0 && strcmp(state, "unknown") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", entry->d_name);
		unsigned long long rx = read_number(path, 0);
		snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", entry->d_name);
		unsigned long long tx = read_number(path, 0);
		int index = -1;
		for (int i = 0; i < s->count; i++) {
			if (strcmp(s->ifaces[i].name, entry->d_name) == 0) {
				index = i;
			}
		}
		if (index < 0 && s->count < NET_MAX) {
			index = s->count++;
			snprintf(s->ifaces[index].name, sizeof(s->ifaces[index].name), "%.*s", IF_NAMESIZE,
				entry->d_name);
			s->ifaces[index].rx = rx;
			s->ifaces[index].tx = tx;
		}
		if (index < 0) {
			continue;
		}
		if (elapsed > 0) {
			s->ifaces[index].down = rx >= s->ifaces[index].rx ?
				(rx - s->ifaces[index].rx) / elapsed : 0;
			s->ifaces[index].up = tx >= s->ifaces[index].tx ?
				(tx - s->ifaces[index].tx) / elapsed : 0;
		}
		s->ifaces[index].rx = rx;
		s->ifaces[index].tx = tx;
		down += s->ifaces[index].down;
		up += s->ifaces[index].up;
		all_rx += rx;
		all_tx += tx;
	}
	if (dir) {
		closedir(dir);
	}
	s->sampled = now;
	push_history(f, down, up);

	char down_text[32], up_text[32], text[128], address[INET_ADDRSTRLEN];
	format_rate(down_text, sizeof(down_text), down);
	format_rate(up_text, sizeof(up_text), up);
	snprintf(f->big, sizeof(f->big), "↓ %.24s", down_text);
	snprintf(f->title, sizeof(f->title), "Network usage");
	snprintf(f->subtitle, sizeof(f->subtitle), "↑ %s going out", up_text);
	f->graph = f->two_series = true;
	snprintf(f->graph_note, sizeof(f->graph_note), "60 seconds");
	snprintf(f->series_names[0], sizeof(f->series_names[0]), "Down");
	snprintf(f->series_names[1], sizeof(f->series_names[1]), "Up");
	snprintf(f->rows_title, sizeof(f->rows_title), "Connections");
	for (int i = 0; i < s->count; i++) {
		net_address(s->ifaces[i].name, address, sizeof(address));
		char label[64];
		snprintf(label, sizeof(label), address[0] ? "%s · %s" : "%s", s->ifaces[i].name,
			address);
		format_rate(down_text, sizeof(down_text), s->ifaces[i].down);
		format_rate(up_text, sizeof(up_text), s->ifaces[i].up);
		snprintf(text, sizeof(text), "↓ %s  ↑ %s", down_text, up_text);
		add_row(f, label, text);
	}
	if (s->count == 0) {
		add_row(f, "Not connected", NULL);
	}
	format_bytes(down_text, sizeof(down_text), all_rx);
	format_bytes(up_text, sizeof(up_text), all_tx);
	snprintf(text, sizeof(text), "%s in, %s out", down_text, up_text);
	add_row(f, "Since the connections came up", text);
	add_action(f, "Network settings", TW_NETWORK_SETTINGS, NULL);
}

/* ---------- disk space ---------- */

static bool storage_type(const char *type) {
	static const char *const real[] = { "ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "vfat",
		"exfat", "ntfs", "ntfs3", "fuseblk", "zfs", "bcachefs", "jfs", "reiserfs" };
	for (size_t i = 0; i < sizeof(real) / sizeof(real[0]); i++) {
		if (strcmp(type, real[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* /proc/mounts writes spaces in paths as \040. */
static void unescape_mount(char *path) {
	char *out = path;
	for (char *in = path; *in; in++) {
		if (in[0] == '\\' && isdigit((unsigned char)in[1]) && isdigit((unsigned char)in[2]) &&
				isdigit((unsigned char)in[3])) {
			*out++ = (char)((in[1] - '0') * 64 + (in[2] - '0') * 8 + (in[3] - '0'));
			in += 3;
		} else {
			*out++ = *in;
		}
	}
	*out = '\0';
}

static void storage_sample(struct info_flyout *f) {
	const char *watched = widget_conf(f->widget, "path", "/");
	FILE *mounts = fopen("/proc/mounts", "r");
	char line[1024], devices[MAX_ROWS][256];
	int device_count = 0;
	snprintf(f->title, sizeof(f->title), "Disk space");
	snprintf(f->rows_title, sizeof(f->rows_title), "Drives");
	while (mounts && fgets(line, sizeof(line), mounts) && f->row_count < MAX_ROWS - 1) {
		char device[256], mount[512], type[64];
		if (sscanf(line, "%255s %511s %63s", device, mount, type) != 3 || !storage_type(type)) {
			continue;
		}
		bool seen = false;
		for (int i = 0; i < device_count && !seen; i++) {
			seen = strcmp(devices[i], device) == 0; // btrfs subvolumes of one drive
		}
		if (seen) {
			continue;
		}
		snprintf(devices[device_count++], sizeof(devices[0]), "%s", device);
		unescape_mount(mount);
		struct statvfs st;
		if (statvfs(mount, &st) != 0 || st.f_blocks == 0) {
			continue;
		}
		double total = (double)st.f_blocks * st.f_frsize;
		double free_space = (double)st.f_bavail * st.f_frsize;
		char free_text[32], total_text[32], value[96];
		format_bytes(free_text, sizeof(free_text), free_space);
		format_bytes(total_text, sizeof(total_text), total);
		snprintf(value, sizeof(value), "%s free of %s", free_text, total_text);
		const char *base = strrchr(device, '/');
		char label[160];
		snprintf(label, sizeof(label), "%.100s · %.50s", mount, base ? base + 1 : device);
		struct info_row *row = add_row(f, label, value);
		row->bar = 1 - free_space / total;
		row->full_is_bad = true;
		char *quoted = g_shell_quote(mount);
		row->command = format_str("exec xdg-open %s", quoted);
		g_free(quoted);
		if (strcmp(mount, watched) == 0) {
			row->marked = true;
			snprintf(f->big, sizeof(f->big), "%.0f%%", row->bar * 100);
			snprintf(f->subtitle, sizeof(f->subtitle), "%.100s: %.90s", mount, value);
		}
	}
	if (mounts) {
		fclose(mounts);
	}
	if (!f->big[0]) {
		struct statvfs st;
		if (statvfs(watched, &st) == 0 && st.f_blocks > 0) {
			snprintf(f->big, sizeof(f->big), "%.0f%%",
				100 - 100.0 * st.f_bavail / st.f_blocks);
			snprintf(f->subtitle, sizeof(f->subtitle), "In use on %s", watched);
		}
	}
	add_action(f, "Open the home folder", "exec xdg-open ~", NULL);
}

/* ---------- power draw ---------- */

struct power_state {
	char battery[256]; // /sys/class/power_supply/BATn, empty when there is none
	long long energy;  // µJ of RAPL at the last reading, for machines without a battery
	double sampled;
};

/*
 * energy_<what> of a battery in µWh; batteries that count charge instead
 * (charge_<what> in µAh) are converted with their design voltage.
 */
static long long battery_energy(const char *battery, const char *what) {
	char path[400];
	snprintf(path, sizeof(path), "%s/energy_%s", battery, what);
	long long energy = read_number(path, -1);
	if (energy >= 0) {
		return energy;
	}
	snprintf(path, sizeof(path), "%s/charge_%s", battery, what);
	long long charge = read_number(path, -1);
	snprintf(path, sizeof(path), "%s/voltage_min_design", battery);
	long long voltage = read_number(path, -1);
	return charge >= 0 && voltage > 0 ? charge * voltage / 1000000 : -1;
}

static void power_sample(struct info_flyout *f) {
	struct power_state *s = f->state;
	if (!s) {
		s = f->state = calloc(1, sizeof(*s));
		const char *wanted = widget_conf(f->widget, "device", NULL);
		DIR *dir = opendir("/sys/class/power_supply");
		struct dirent *entry;
		while (dir && (entry = readdir(dir))) {
			char type[32] = "", path[512];
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name);
			read_line(path, type, sizeof(type));
			if (strcmp(type, "Battery") == 0 && (!wanted || strcmp(wanted, entry->d_name) == 0)) {
				snprintf(s->battery, sizeof(s->battery), "/sys/class/power_supply/%.200s",
					entry->d_name);
				break;
			}
		}
		if (dir) {
			closedir(dir);
		}
	}
	char path[400], text[96];
	double watts = -1;
	snprintf(f->title, sizeof(f->title), "Power draw");
	if (s->battery[0]) {
		snprintf(path, sizeof(path), "%s/power_now", s->battery);
		long long power = read_number(path, -1);
		if (power < 0) {
			snprintf(path, sizeof(path), "%s/current_now", s->battery);
			long long current = read_number(path, -1);
			snprintf(path, sizeof(path), "%s/voltage_now", s->battery);
			long long voltage = read_number(path, -1);
			if (current >= 0 && voltage > 0) {
				power = current * voltage / 1000000;
			}
		}
		if (power >= 0) {
			watts = power / 1e6;
		}
		char status[32] = "";
		snprintf(path, sizeof(path), "%s/status", s->battery);
		read_line(path, status, sizeof(status));
		snprintf(path, sizeof(path), "%s/capacity", s->battery);
		long long capacity = read_number(path, -1);
		if (capacity >= 0) {
			snprintf(text, sizeof(text), "%lld%%", capacity);
			struct info_row *row = add_row(f, "Charge", text);
			row->bar = capacity / 100.0;
		}
		add_row(f, "State", status[0] ? status : "Unknown");
		long long now = battery_energy(s->battery, "now");
		long long full = battery_energy(s->battery, "full");
		long long design = battery_energy(s->battery, "full_design");
		if (now >= 0 && full > 0) {
			snprintf(text, sizeof(text), "%.1f of %.1f Wh", now / 1e6, full / 1e6);
			add_row(f, "Energy", text);
			if (watts > 0.5) {
				double hours = strcmp(status, "Charging") == 0 ? (full - now) / 1e6 / watts :
					now / 1e6 / watts;
				snprintf(text, sizeof(text), "%d h %02d min", (int)hours,
					(int)((hours - (int)hours) * 60));
				add_row(f, strcmp(status, "Charging") == 0 ? "Until full" : "Left", text);
			}
		}
		if (full > 0 && design > 0) {
			snprintf(text, sizeof(text), "%.0f%% of what it held new", 100.0 * full / design);
			add_row(f, "Health", text);
		}
		snprintf(path, sizeof(path), "%s/cycle_count", s->battery);
		long long cycles = read_number(path, -1);
		if (cycles > 0) {
			snprintf(text, sizeof(text), "%lld", cycles);
			add_row(f, "Charge cycles", text);
		}
	}
	if (s->battery[0]) {
		snprintf(f->subtitle, sizeof(f->subtitle), "Drawn from the battery or the charger");
	}
	if (watts <= 0) {
		// no battery to ask, or one that rests on the charger: what the processor
		// package used, from RAPL, where it may be read
		long long energy = read_number("/sys/class/powercap/intel-rapl:0/energy_uj", -1);
		double now = seconds_now();
		if (energy >= 0 && s->energy > 0 && energy >= s->energy && now > s->sampled) {
			watts = (energy - s->energy) / 1e6 / (now - s->sampled);
			snprintf(f->subtitle, sizeof(f->subtitle), "The processor package, measured by RAPL");
		}
		s->energy = energy;
		s->sampled = now;
	}
	if (watts >= 0) {
		snprintf(f->big, sizeof(f->big), "%.1f W", watts);
		push_history(f, watts, 0);
		f->graph = true;
		snprintf(f->graph_note, sizeof(f->graph_note), "60 seconds");
	} else {
		snprintf(f->big, sizeof(f->big), "–");
		snprintf(f->subtitle, sizeof(f->subtitle), "This computer does not report it");
	}
	add_action(f, "Power and screen settings", "exec tilewin-settings --page screen", NULL);
}

/* ---------- keyboard layout ---------- */

static void keyboard_sample(struct info_flyout *f) {
	json_object *inputs = ipc_panel_request(f->panel, IPC_GET_INPUTS);
	size_t count = inputs && json_object_is_type(inputs, json_type_array) ?
		json_object_array_length(inputs) : 0;
	snprintf(f->title, sizeof(f->title), "Keyboard layout");
	snprintf(f->rows_title, sizeof(f->rows_title), "Pick a layout");
	for (size_t i = 0; i < count; i++) {
		json_object *input = json_object_array_get_idx(inputs, i), *type, *names, *active;
		if (!json_object_object_get_ex(input, "type", &type) ||
				strcmp(json_object_get_string(type), "keyboard") != 0 ||
				!json_object_object_get_ex(input, "xkb_layout_names", &names)) {
			continue;
		}
		int active_index = json_object_object_get_ex(input, "xkb_active_layout_index", &active) ?
			json_object_get_int(active) : 0;
		for (size_t n = 0; n < json_object_array_length(names); n++) {
			const char *name = json_object_get_string(json_object_array_get_idx(names, n));
			struct info_row *row = add_row(f, name, (int)n == active_index ? "In use" : NULL);
			if (!row) {
				break;
			}
			row->marked = (int)n == active_index;
			row->command = format_str("input type:keyboard xkb_switch_layout %zu", n);
			if (row->marked) {
				snprintf(f->subtitle, sizeof(f->subtitle), "%s", name);
			}
		}
		break;
	}
	json_object_put(inputs);
	if (f->row_count == 0) {
		add_row(f, "No keyboard reports its layouts", NULL);
	}
	add_action(f, "Keyboard settings", "exec tilewin-settings --page keyboard", NULL);
}

/* ---------- the focused window ---------- */

static void title_sample(struct info_flyout *f) {
	struct pwindow *win = panel_find_window(f->panel, f->panel->state.focused_window);
	if (!win) {
		snprintf(f->title, sizeof(f->title), "No window has the focus");
		return;
	}
	snprintf(f->title, sizeof(f->title), "%s", win->title && *win->title ? win->title : "Untitled");
	const char *app = win->app_id && *win->app_id ? apps_display_name(win->app_id) : NULL;
	snprintf(f->subtitle, sizeof(f->subtitle), "%s", app ? app : win->app_id ? win->app_id : "");
	char text[64];
	add_row(f, "Desktop", win->workspace);
	add_row(f, "Screen", win->output);
	if (win->pid > 0) {
		snprintf(text, sizeof(text), "%d", win->pid);
		add_row(f, "Process", text);
	}
	add_row(f, "State", win->maximized ? "Maximized" : win->floating ? "A window" : "Tiled");
	char *command = format_str("[con_id=%lld] minimize enable", (long long)win->id);
	add_action(f, "Minimize", command, NULL);
	free(command);
	command = format_str("[con_id=%lld] maximize toggle", (long long)win->id);
	add_action(f, win->maximized ? "Restore" : "Maximize", command, NULL);
	free(command);
	command = format_str("[con_id=%lld] kill", (long long)win->id);
	add_action(f, "Close", command, NULL);
	free(command);
}

/* ---------- script widgets ---------- */

static void custom_again(struct info_flyout *f) {
	custom_run_now(f->widget);
}

static void add_text_rows(struct info_flyout *f, const char *text) {
	char *copy = strdup(text ? text : "");
	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		add_row(f, line, NULL);
	}
	free(copy);
}

static void custom_sample(struct info_flyout *f) {
	const char *colon = strchr(f->widget->name, ':');
	snprintf(f->title, sizeof(f->title), "%s", colon ? colon + 1 : f->widget->name);
	const char *command = widget_conf(f->widget, "exec", NULL);
	if (!command) {
		command = widget_conf(f->widget, "exec_listen", "");
	}
	snprintf(f->subtitle, sizeof(f->subtitle), "%s", command);
	snprintf(f->rows_title, sizeof(f->rows_title), "What it said last");
	add_text_rows(f, custom_output(f->widget));
	struct hotspot none = { 0 };
	char *tooltip = f->widget->impl->tooltip ? f->widget->impl->tooltip(f->widget, &none) : NULL;
	if (tooltip && (!custom_output(f->widget) || strcmp(tooltip, custom_output(f->widget)) != 0)) {
		add_text_rows(f, tooltip);
	}
	free(tooltip);
	if (f->row_count == 0) {
		add_row(f, "Nothing yet", NULL);
	}
	add_action(f, "Run it again", NULL, custom_again);
	add_action(f, "Taskbar settings", "exec tilewin-settings --page taskbar", NULL);
}

/* ---------- git ---------- */

struct git_state {
	char *log;  // the latest commits, once git has told
	struct proc *proc;
	char *repo; // the repository the log is of
};

static void git_log_done(void *data, const char *output) {
	struct info_flyout *f = data;
	if (current != f || !f->state) {
		return;
	}
	struct git_state *s = f->state;
	s->proc = NULL;
	free(s->log);
	s->log = strdup(output ? output : "");
	refill(f); // everything once more, now with the log
}

static void git_sample(struct info_flyout *f) {
	struct git_state *s = f->state;
	if (!s) {
		s = f->state = calloc(1, sizeof(*s));
	}
	const char *repo = git_widget_repo(f->widget);
	if (!repo) {
		snprintf(f->title, sizeof(f->title), "No repository");
		return;
	}
	struct hotspot none = { 0 };
	char *tooltip = f->widget->impl->tooltip(f->widget, &none);
	// the tooltip says the repository, the branch and then how the work stands
	char *copy = strdup(tooltip ? tooltip : "");
	char *save = NULL;
	int n = 0;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save), n++) {
		if (n == 0) {
			snprintf(f->subtitle, sizeof(f->subtitle), "%s", line);
		} else if (n == 1) {
			snprintf(f->title, sizeof(f->title), "%s", line);
		} else {
			add_row(f, line, NULL);
		}
	}
	free(copy);
	free(tooltip);
	char *quoted = g_shell_quote(repo);
	if ((!s->repo || strcmp(s->repo, repo) != 0) && !s->proc) {
		free(s->repo);
		s->repo = strdup(repo);
		char *command = format_str("git -C %s log -5 --format='%%h %%s' 2>/dev/null", quoted);
		s->proc = proc_run(f->panel, command, false, NULL, git_log_done, f);
		free(command);
	}
	if (s->log && *s->log) {
		add_heading(f, "Latest commits");
		add_text_rows(f, s->log);
	}
	char *command = format_str("exec cd %s && exec $term", quoted);
	add_action(f, "Open a terminal here", command, NULL);
	free(command);
	command = format_str("exec xdg-open %s", quoted);
	add_action(f, "Open the folder", command, NULL);
	free(command);
	g_free(quoted);
}

static void git_free(void *data) {
	struct git_state *s = data;
	if (s->proc) {
		proc_cancel(s->proc);
	}
	free(s->log);
	free(s->repo);
	free(s);
}

static const struct info_source sources[] = {
	{ "disk", 1000, disk_sample, free },
	{ "gpu", 1000, gpu_sample, gpu_free },
	{ "net", 1000, net_sample, free },
	{ "storage", 5000, storage_sample, free },
	{ "power", 1000, power_sample, free },
	{ "keyboard", 0, keyboard_sample, free },
	{ "title", 1000, title_sample, free },
	{ "custom", 1000, custom_sample, free },
	{ "git", 2000, git_sample, git_free },
};

/* ---------- drawing ---------- */

static bool hovered(struct info_flyout *f, struct pbox b) {
	return f->inside && pbox_contains(&b, f->px, f->py);
}

static int row_height(const struct info_row *row) {
	return row->heading ? ROWS_TITLE : row->bar >= 0 ? BAR_ROW : ROW;
}

static int content_height(struct info_flyout *f) {
	int h = HEADER;
	if (f->graph) {
		h += GRAPH;
	}
	if (f->row_count > 0) {
		h += 6 + (f->rows_title[0] ? ROWS_TITLE : 0);
		for (int i = 0; i < f->row_count; i++) {
			h += row_height(&f->rows[i]);
		}
		h += 6;
	}
	if (f->action_count > 0) {
		h += FOOTER;
	}
	return h;
}

static void draw_graph(struct info_flyout *f, cairo_t *cr, const struct fly_style *st,
		struct pbox g) {
	cairo_new_path(cr);
	pd_rounded(cr, g.x, g.y, g.width, g.height, st->style == PS_CLASSIC ? 0 : 6);
	pd_color(cr, st->button_bg);
	cairo_fill(cr);
	for (int i = 1; i < 4; i++) {
		pd_rect(cr, g.x, g.y + g.height * i / 4, g.width, 1, st->line);
	}
	double max = f->graph_max;
	for (int s = 0; max <= 0 && s < (f->two_series ? 2 : 1); s++) {
		for (int i = 0; i < f->history_len; i++) {
			max = fmax(max, f->history[s][i]);
		}
	}
	for (int s = 0; !f->graph_max && s < (f->two_series ? 2 : 1); s++) {
		for (int i = 0; i < f->history_len; i++) {
			max = fmax(max, f->history[s][i] * 1.1);
		}
	}
	if (max <= 0) {
		max = 1;
	}
	double step = (double)g.width / (HISTORY - 1);
	double first_x = g.x + g.width - (f->history_len - 1) * step;
	uint32_t colors[2] = { st->accent, st->dark ? 0xf0a060ff : 0xc0602aff };
	for (int s = (f->two_series ? 1 : 0); s >= 0 && f->history_len > 1; s--) {
		cairo_new_path(cr);
		cairo_move_to(cr, first_x, g.y + g.height);
		for (int i = 0; i < f->history_len; i++) {
			cairo_line_to(cr, first_x + i * step,
				g.y + g.height - g.height * fmin(f->history[s][i] / max, 1));
		}
		cairo_line_to(cr, g.x + g.width, g.y + g.height);
		cairo_close_path(cr);
		pd_color(cr, (colors[s] & 0xffffff00) | 0x40);
		cairo_fill(cr);
		cairo_new_path(cr);
		for (int i = 0; i < f->history_len; i++) {
			double x = first_x + i * step;
			double y = g.y + g.height - g.height * fmin(f->history[s][i] / max, 1);
			if (i == 0) {
				cairo_move_to(cr, x, y);
			} else {
				cairo_line_to(cr, x, y);
			}
		}
		pd_color(cr, colors[s]);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
	}
	int y = g.y + g.height + 2;
	pd_text(cr, st->font, f->graph_note, g.x, y, g.width, 22, st->dim, PD_LEFT);
	if (f->two_series) {
		// a key for the two lines
		int x = g.x + g.width;
		for (int s = 1; s >= 0; s--) {
			int tw = 0;
			pd_text_size(cr, st->font, f->series_names[s], &tw, NULL);
			x -= tw;
			pd_text(cr, st->font, f->series_names[s], x, y, tw, 22, st->dim, PD_LEFT);
			x -= 14;
			pd_rect(cr, x, y + 10, 10, 3, colors[s]);
			x -= 12;
		}
	}
}

static void info_render(struct popup *p, cairo_t *cr) {
	struct info_flyout *f = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(p->panel, cr, W, H, M, "menu");
	int x0 = M + PAD, cw = W - 2 * M - 2 * PAD;
	int y = M;

	// the reading, what it is and the details
	int text_x = x0;
	if (f->big[0]) {
		int bw = 0;
		pd_text_size(cr, st.big, f->big, &bw, NULL);
		bw = bw < 86 ? 86 : bw + 12;
		pd_text(cr, st.big, f->big, x0, y + 12, bw, 36, st.fg, PD_LEFT);
		text_x = x0 + bw + 4;
	}
	pd_text(cr, st.bold, f->title, text_x, y + 12, x0 + cw - text_x, 20, st.fg, PD_LEFT);
	pd_text(cr, st.font, f->subtitle, text_x, y + 34, x0 + cw - text_x, 20, st.dim, PD_LEFT);
	y += HEADER;

	if (f->graph) {
		draw_graph(f, cr, &st, (struct pbox){ x0, y, cw, GRAPH - 26 });
		y += GRAPH;
	}

	if (f->row_count > 0) {
		draw_line(cr, &st, p, y);
		y += 6;
		if (f->rows_title[0]) {
			pd_text(cr, st.font, f->rows_title, x0, y, cw, ROWS_TITLE - 6, st.dim, PD_LEFT);
			y += ROWS_TITLE;
		}
		for (int i = 0; i < f->row_count; i++) {
			struct info_row *row = &f->rows[i];
			int h = row_height(row);
			if (row->heading) {
				if (i > 0) {
					draw_line(cr, &st, p, y + 2);
				}
				pd_text(cr, st.font, row->label, x0, y + 6, cw, h - 6, st.dim, PD_LEFT);
				y += h;
				continue;
			}
			struct pbox box = { x0 - 6, y, cw + 12, h };
			if (row->command) {
				if (hovered(f, box)) {
					fill_hover(cr, &st, box);
				}
				psurface_add_hotspot(p->surface, box.x, box.y, box.width, box.height, NULL,
					HS_ROW, i, NULL);
			}
			if (row->marked) {
				pd_rect(cr, x0 - 6, y + 6, 3, h - 12, st.accent);
			}
			int vw = 0, lw = 0;
			if (row->value[0]) {
				// the label keeps what it needs up to half the width, the value gets the rest
				pd_text_size(cr, st.font, row->value, &vw, NULL);
				pd_text_size(cr, row->marked ? st.bold : st.font, row->label, &lw, NULL);
				int room = cw - (lw < cw / 2 ? lw : cw / 2) - 12;
				vw = vw > room ? room : vw;
			}
			int text_h = row->bar >= 0 ? 22 : h;
			pd_text(cr, row->marked ? st.bold : st.font, row->label, x0, y,
				cw - vw - (vw ? 12 : 0), text_h, st.fg, PD_LEFT);
			if (row->value[0]) {
				pd_text(cr, st.font, row->value, x0 + cw - vw, y, vw, text_h, st.dim, PD_RIGHT);
			}
			if (row->bar >= 0) {
				double share = fmin(fmax(row->bar, 0), 1);
				cairo_new_path(cr);
				pd_rounded(cr, x0, y + 24, cw, 6, st.style == PS_CLASSIC ? 0 : 3);
				pd_color(cr, st.track);
				cairo_fill(cr);
				if (share > 0) {
					cairo_new_path(cr);
					pd_rounded(cr, x0, y + 24, cw * share, 6, st.style == PS_CLASSIC ? 0 : 3);
					pd_color(cr, row->full_is_bad && share > 0.9 ? st.error : st.accent);
					cairo_fill(cr);
				}
			}
			y += h;
		}
		y += 6;
	}

	if (f->action_count > 0) {
		draw_line(cr, &st, p, y);
		uint32_t color = st.style == PS_CLASSIC ? 0x0000ffff : st.accent;
		int x = x0;
		for (int i = 0; i < f->action_count; i++) {
			int tw = 0;
			pd_text_size(cr, st.font, f->actions[i].label, &tw, NULL);
			struct pbox link = { x, y + 1, tw, FOOTER - 1 };
			pd_text(cr, st.font, f->actions[i].label, link.x, link.y, link.width, link.height,
				color, PD_LEFT);
			if (hovered(f, link)) {
				pd_rect(cr, link.x, link.y + link.height / 2.0 + 9, tw, 1, color);
			}
			psurface_add_hotspot(p->surface, link.x, link.y, link.width, link.height, NULL,
				HS_ACTION, i, NULL);
			x += tw + 24;
		}
	}
}

/* ---------- life of a flyout ---------- */

static void geometry(struct info_flyout *f, int *x, int *y, int *width, int *height) {
	int M = popup_shadow_margin(f->panel);
	*width = WIDTH + 2 * M;
	*height = content_height(f) + 2 * M;
	*x = f->anchor.right_align ? f->anchor.x - *width + M : f->anchor.x - M;
	*y = f->anchor.above ? f->anchor.y - *height : f->anchor.y;
}

static void refill(struct info_flyout *f) {
	clear_content(f);
	f->source->sample(f);
	if (!f->popup) {
		return;
	}
	int x, y, width, height;
	geometry(f, &x, &y, &width, &height);
	if (height != f->popup->height && height <= f->anchor.output->height) {
		popup_move_resize(f->popup, x, y, width, height);
	}
	popup_set_dirty(f->popup);
}

static void tick(void *data) {
	struct info_flyout *f = data;
	f->tick = NULL;
	refill(f);
	f->tick = loop_add_timer(f->panel->loop, f->source->interval_ms, tick, f);
}

static void info_motion(struct popup *p, double x, double y) {
	struct info_flyout *f = p->data;
	f->px = x;
	f->py = y;
	f->inside = true;
	popup_set_dirty(p);
}

static void info_leave(struct popup *p) {
	struct info_flyout *f = p->data;
	f->inside = false;
	popup_set_dirty(p);
}

static void info_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct info_flyout *f = p->data;
	if (button != BTN_LEFT || pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	if (hs->kind == HS_ROW && hs->id < f->row_count && f->rows[hs->id].command) {
		bar_run_command(f->panel, f->rows[hs->id].command, NULL);
		popup_close_later(f->panel);
	} else if (hs->kind == HS_ACTION && hs->id < f->action_count) {
		struct info_action *a = &f->actions[hs->id];
		if (a->run) {
			a->run(f);
			refill(f);
		} else {
			bar_run_command(f->panel, a->command, NULL);
			popup_close_later(f->panel);
		}
	}
}

static void info_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void info_destroy(struct popup *p) {
	struct info_flyout *f = p->data;
	if (current == f) {
		current = NULL;
	}
	if (f->tick) {
		loop_remove_timer(p->panel->loop, f->tick);
	}
	clear_content(f);
	if (f->state) {
		f->source->free_state(f->state);
	}
	free(f);
}

static const struct popup_vtable info_vtable = {
	.render = info_render,
	.motion = info_motion,
	.leave = info_leave,
	.button = info_button,
	.key = info_key,
	.destroy = info_destroy,
};

void info_flyout_toggle(struct widget *w, struct popup_anchor anchor) {
	struct panel *panel = w->panel;
	if (current && current->widget == w && popup_is_open(panel, POPUP_INFO)) {
		popup_close_all(panel);
		return;
	}
	const struct info_source *source = NULL;
	for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		if (strcmp(sources[i].type, w->impl->type) == 0) {
			source = &sources[i];
		}
	}
	if (!source || !anchor.output) {
		return;
	}
	popup_close_all(panel);
	struct info_flyout *f = calloc(1, sizeof(*f));
	f->panel = panel;
	f->widget = w;
	f->anchor = anchor;
	f->source = source;
	current = f;
	refill(f);
	int x, y, width, height;
	geometry(f, &x, &y, &width, &height);
	f->popup = popup_create(panel, POPUP_INFO, NULL, anchor.output, x, y, width, height,
		&info_vtable, f);
	if (!f->popup) {
		current = NULL;
		clear_content(f);
		if (f->state) {
			source->free_state(f->state);
		}
		free(f);
		return;
	}
	if (source->interval_ms > 0) {
		f->tick = loop_add_timer(panel->loop, source->interval_ms, tick, f);
	}
}
