#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "tw_battery.h"
#include "tw_paths.h"

const char *tw_power_supply_dir(void) {
	const char *root = getenv("TILEWIN_SYS_POWER");
	return root && *root ? root : TW_POWER_SUPPLY_DIR;
}

static bool read_value(const char *dir, const char *name, char *buf, size_t size) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	if (ok) {
		buf[strcspn(buf, "\r\n")] = '\0';
	}
	return ok;
}

bool tw_battery_read(const char *root, int *percent, bool *charging) {
	DIR *dir = opendir(root);
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir))) {
		char base[512], buf[64];
		snprintf(base, sizeof(base), "%s/%s", root, de->d_name);
		if (de->d_name[0] == '.' || !read_value(base, "type", buf, sizeof(buf)) ||
				strcmp(buf, "Battery") != 0 || !read_value(base, "capacity", buf, sizeof(buf))) {
			continue;
		}
		int value = atoi(buf);
		*percent = value < 0 ? 0 : value > 100 ? 100 : value;
		*charging = read_value(base, "status", buf, sizeof(buf)) && strcmp(buf, "Charging") == 0;
		found = true;
	}
	closedir(dir);
	return found;
}

void tw_battery_history_free(struct tw_battery_history *h) {
	free(h->samples);
	h->samples = NULL;
	h->count = h->capacity = 0;
}

void tw_battery_history_append(struct tw_battery_history *h, struct tw_battery_sample s) {
	if (h->count > 0 && s.time <= h->samples[h->count - 1].time) {
		return;
	}
	if (h->count == h->capacity) {
		int capacity = h->capacity ? h->capacity * 2 : 64;
		struct tw_battery_sample *grown = realloc(h->samples, capacity * sizeof(*grown));
		if (!grown) {
			return;
		}
		h->samples = grown;
		h->capacity = capacity;
	}
	h->samples[h->count++] = s;
}

void tw_battery_history_parse(struct tw_battery_history *h, const char *text) {
	h->count = 0;
	const char *line = text;
	while (line && *line) {
		const char *end = strchr(line, '\n');
		long long time;
		int percent;
		char flag;
		if (line[0] != '#' && sscanf(line, "%lld %d %c", &time, &percent, &flag) == 3 &&
				time > 0 && percent >= 0 && percent <= 100 && (flag == 'c' || flag == '-')) {
			tw_battery_history_append(h, (struct tw_battery_sample){ time, percent, flag == 'c' });
		}
		line = end ? end + 1 : NULL;
	}
}

char *tw_battery_history_format(const struct tw_battery_history *h) {
	size_t size = 64 + (size_t)h->count * 32, len = 0;
	char *out = malloc(size);
	if (!out) {
		return NULL;
	}
	len += snprintf(out, size, "# unix time, battery percent, c when charging\n");
	for (int i = 0; i < h->count; i++) {
		len += snprintf(out + len, size - len, "%lld %d %c\n", h->samples[i].time,
			h->samples[i].percent, h->samples[i].charging ? 'c' : '-');
	}
	return out;
}

bool tw_battery_history_load(struct tw_battery_history *h, const char *path) {
	h->count = 0;
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char *text = NULL;
	size_t size = 0;
	ssize_t len = getdelim(&text, &size, '\0', f);
	fclose(f);
	if (len > 0) {
		tw_battery_history_parse(h, text);
	}
	free(text);
	return len > 0;
}

bool tw_battery_history_save(const struct tw_battery_history *h, const char *path) {
	char *text = tw_battery_history_format(h);
	bool ok = text && path && tw_write_string(path, text);
	free(text);
	return ok;
}

void tw_battery_history_prune(struct tw_battery_history *h, long long now, long long keep) {
	int first = 0;
	while (first < h->count && h->samples[first].time < now - keep) {
		first++;
	}
	int last = h->count;
	while (last > first && h->samples[last - 1].time > now) {
		last--;
	}
	if (first > 0) {
		memmove(h->samples, h->samples + first, (last - first) * sizeof(*h->samples));
	}
	h->count = last - first;
}

bool tw_battery_history_record(struct tw_battery_history *h, long long now, int percent,
		bool charging, long long interval) {
	// a clock set back: what seems to come after now goes
	while (h->count > 0 && h->samples[h->count - 1].time >= now) {
		const struct tw_battery_sample *last = &h->samples[h->count - 1];
		if (last->time == now && last->percent == percent && last->charging == charging) {
			return false;
		}
		h->count--;
	}
	if (h->count > 0) {
		const struct tw_battery_sample *last = &h->samples[h->count - 1];
		if (now - last->time < interval && last->percent == percent &&
				last->charging == charging) {
			return false;
		}
	}
	tw_battery_history_append(h, (struct tw_battery_sample){ now, percent, charging });
	return true;
}

/* What the line from a to b stands for. Over a gap the battery was not looked
 * at: it charged if it came back fuller. */
static void segment(struct tw_battery_point *p, const struct tw_battery_sample *a,
		const struct tw_battery_sample *b) {
	p->gap = b->time - a->time > TW_BATTERY_GAP;
	p->charging = p->gap ? b->percent > a->percent : a->charging;
}

struct tw_battery_point *tw_battery_history_points(const struct tw_battery_history *h,
		long long now, long long span, const struct tw_battery_sample *current, int *count) {
	*count = 0;
	if (span <= 0) {
		return NULL;
	}
	// the readings up to now, the one of now last
	long long until = current ? current->time : now;
	int n = 0;
	struct tw_battery_sample *all = malloc((h->count + 1) * sizeof(*all));
	if (!all) {
		return NULL;
	}
	for (int i = 0; i < h->count; i++) {
		if (h->samples[i].time < until || (!current && h->samples[i].time == until)) {
			all[n++] = h->samples[i];
		}
	}
	if (current) {
		all[n++] = *current;
	}
	long long start = now - span;
	int first = 0;
	while (first < n && all[first].time < start) {
		first++;
	}
	struct tw_battery_point *points = NULL;
	if (first < n) {
		points = malloc((n - first + 1) * sizeof(*points));
	}
	if (!points) {
		free(all);
		return NULL;
	}
	int k = 0;
	if (first > 0) {
		// the line from before the span, cut at its left edge
		const struct tw_battery_sample *a = &all[first - 1], *b = &all[first];
		double t = (double)(start - a->time) / (double)(b->time - a->time);
		points[k] = (struct tw_battery_point){ .x = 0,
			.y = (a->percent + (b->percent - a->percent) * t) / 100.0 };
		segment(&points[k++], a, b);
	}
	for (int i = first; i < n; i++) {
		double x = (double)(all[i].time - start) / (double)span;
		points[k] = (struct tw_battery_point){ .x = x > 1 ? 1 : x, .y = all[i].percent / 100.0,
			.charging = all[i].charging };
		if (i + 1 < n) {
			segment(&points[k], &all[i], &all[i + 1]);
		}
		k++;
	}
	free(all);
	*count = k;
	return points;
}

char *tw_battery_history_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = format_str("%s/battery-history", dir);
	free(dir);
	return path;
}
