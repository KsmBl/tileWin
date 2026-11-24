#define _GNU_SOURCE
#include <dirent.h>
#include <json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "stringop.h"
#include "tw_claude_usage.h"
#include "tw_paths.h"
#include "twconf.h"

#define STATUS_PROGRAM "tilewin-claude-usage"

char *tw_claude_normalize_dir(const char *dir) {
	if (!dir || !*dir) {
		return NULL;
	}
	char *expanded = tw_expand_home(dir);
	char *real = expanded ? realpath(expanded, NULL) : NULL;
	// a folder that is not there (yet) is kept as written
	char *out = real ? real : strdup(expanded ? expanded : dir);
	free(expanded);
	size_t len = strlen(out);
	while (len > 1 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
	return out;
}

char *tw_claude_default_dir(void) {
	const char *env = getenv("CLAUDE_CONFIG_DIR");
	if (env && *env) {
		return tw_claude_normalize_dir(env);
	}
	const char *home = getenv("HOME");
	char *dir = format_str("%s/.claude", home ? home : "");
	char *out = tw_claude_normalize_dir(dir);
	free(dir);
	return out;
}

static json_object *read_json(const char *path) {
	struct stat st;
	if (!path || stat(path, &st) != 0 || st.st_size > 16 * 1024 * 1024) {
		return NULL;
	}
	return json_object_from_file(path);
}

char *tw_claude_account(const char *dir) {
	if (!dir) {
		return NULL;
	}
	// the installed one keeps it beside its folder, in ~/.claude.json
	char *path = format_str("%s/.claude.json", dir);
	json_object *root = read_json(path);
	free(path);
	if (!root) {
		const char *home = getenv("HOME");
		char *home_claude = format_str("%s/.claude", home ? home : "");
		char *normal = tw_claude_normalize_dir(home_claude);
		if (normal && strcmp(normal, dir) == 0) {
			path = format_str("%s/.claude.json", home ? home : "");
			root = read_json(path);
			free(path);
		}
		free(normal);
		free(home_claude);
	}
	json_object *account, *email;
	char *out = NULL;
	if (root && json_object_object_get_ex(root, "oauthAccount", &account) &&
			json_object_object_get_ex(account, "emailAddress", &email) &&
			json_object_get_string(email) && *json_object_get_string(email)) {
		out = strdup(json_object_get_string(email));
	}
	json_object_put(root);
	return out;
}

static char *usage_dir(void) {
	char *cache = tw_cache_dir();
	char *dir = format_str("%s/claude-usage", cache ? cache : "/tmp");
	free(cache);
	return dir;
}

char *tw_claude_cache_path(const char *dir) {
	if (!dir) {
		return NULL;
	}
	// the folder as a file name: slashes become dashes, "-home-me-.claude.json"
	char *name = strdup(dir);
	for (char *p = name; *p; p++) {
		if (*p == '/') {
			*p = '-';
		}
	}
	char *base = usage_dir();
	char *path = format_str("%s/%s.json", base, name);
	free(base);
	free(name);
	return path;
}

bool tw_claude_connected(const char *dir) {
	char *path = dir ? format_str("%s/settings.json", dir) : NULL;
	json_object *root = read_json(path);
	free(path);
	json_object *line, *command;
	bool yes = root && json_object_object_get_ex(root, "statusLine", &line) &&
		json_object_object_get_ex(line, "command", &command) &&
		json_object_get_string(command) &&
		strstr(json_object_get_string(command), STATUS_PROGRAM) != NULL;
	json_object_put(root);
	return yes;
}

static void read_window(json_object *root, const char *key, struct tw_claude_window *out) {
	json_object *window, *used, *resets;
	*out = (struct tw_claude_window){ 0 };
	if (!json_object_object_get_ex(root, key, &window)) {
		return;
	}
	if (json_object_object_get_ex(window, "used_percentage", &used)) {
		out->known = true;
		out->used = json_object_get_double(used);
	}
	if (json_object_object_get_ex(window, "resets_at", &resets)) {
		out->resets_at = (time_t)json_object_get_int64(resets);
	}
}

bool tw_claude_usage_read(const char *dir, struct tw_claude_usage *out) {
	*out = (struct tw_claude_usage){ 0 };
	out->dir = tw_claude_normalize_dir(dir);
	if (!out->dir) {
		return false;
	}
	out->connected = tw_claude_connected(out->dir);
	out->history = tw_claude_history_path(out->dir);
	char *path = tw_claude_cache_path(out->dir);
	json_object *root = read_json(path);
	free(path);
	json_object *value;
	if (root && json_object_object_get_ex(root, "account", &value) &&
			json_object_get_string(value) && *json_object_get_string(value)) {
		out->account = strdup(json_object_get_string(value));
	}
	if (!out->account) {
		out->account = tw_claude_account(out->dir);
	}
	if (!root) {
		return false;
	}
	if (json_object_object_get_ex(root, "updated", &value)) {
		out->updated = (time_t)json_object_get_int64(value);
	}
	read_window(root, "five_hour", &out->five_hour);
	read_window(root, "seven_day", &out->seven_day);
	json_object_put(root);
	tw_claude_usage_pace(out);
	return true;
}

static void pace_start(const struct tw_claude_point *points, int n, bool week,
		struct tw_claude_window *w) {
	w->pace_from = 0;
	w->pace_base = 0;
	if (!w->known || w->resets_at <= 0) {
		return;
	}
	bool any = false;
	double before = 0, dropped_from = 0;
	for (int i = 0; i < n; i++) {
		const struct tw_claude_window *p = week ? &points[i].seven_day : &points[i].five_hour;
		if (!p->known || !tw_claude_same_window(p->resets_at, w->resets_at)) {
			continue;
		}
		if (any && p->used < before - TW_CLAUDE_RESET_DROP) {
			w->pace_from = points[i].at;
			w->pace_base = p->used;
			dropped_from = before;
		} else if (w->pace_from && p->used >= dropped_from - TW_CLAUDE_RESET_DROP) {
			// back up where it was before the drop: an old value of a session that
			// stayed behind, not a reset by hand
			w->pace_from = 0;
			w->pace_base = 0;
		}
		before = p->used;
		any = true;
	}
}

void tw_claude_usage_pace(struct tw_claude_usage *u) {
	struct tw_claude_point *points = NULL;
	int n = u->history ? tw_claude_history_read(u->history, &points) : 0;
	pace_start(points, n, false, &u->five_hour);
	pace_start(points, n, true, &u->seven_day);
	free(points);
}

void tw_claude_usage_finish(struct tw_claude_usage *usage) {
	free(usage->dir);
	free(usage->account);
	free(usage->history);
	*usage = (struct tw_claude_usage){ 0 };
}

static bool listed(list_t *list, const char *dir) {
	for (int i = 0; i < list->length; i++) {
		if (strcmp(((struct tw_claude_usage *)list->items[i])->dir, dir) == 0) {
			return true;
		}
	}
	return false;
}

static void add_dir(list_t *list, const char *dir, bool only_if_there) {
	char *normal = tw_claude_normalize_dir(dir);
	struct stat st;
	if (!normal || listed(list, normal) ||
			(only_if_there && (stat(normal, &st) != 0 || !S_ISDIR(st.st_mode)))) {
		free(normal);
		return;
	}
	struct tw_claude_usage *u = calloc(1, sizeof(*u));
	tw_claude_usage_read(normal, u);
	free(normal);
	list_add(list, u);
}

static int newest_first(const void *a, const void *b) {
	const struct tw_claude_usage *ua = *(struct tw_claude_usage *const *)a;
	const struct tw_claude_usage *ub = *(struct tw_claude_usage *const *)b;
	return ua->updated < ub->updated ? 1 : ua->updated > ub->updated ? -1 : 0;
}

list_t *tw_claude_usage_all(void) {
	list_t *list = create_list();
	char *installed = tw_claude_default_dir();
	add_dir(list, installed, true);
	free(installed);
	// every folder something was written down for, portable ones too
	char *base = usage_dir();
	DIR *d = opendir(base);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		size_t len = strlen(de->d_name);
		if (len < 6 || strcmp(de->d_name + len - 5, ".json") != 0) {
			continue;
		}
		char *path = format_str("%s/%s", base, de->d_name);
		json_object *root = read_json(path);
		free(path);
		json_object *dir;
		if (root && json_object_object_get_ex(root, "dir", &dir) && json_object_get_string(dir)) {
			add_dir(list, json_object_get_string(dir), false);
		}
		json_object_put(root);
	}
	if (d) {
		closedir(d);
	}
	free(base);
	qsort(list->items, list->length, sizeof(void *), newest_first);
	return list;
}

void tw_claude_usage_list_free(list_t *list) {
	for (int i = 0; list && i < list->length; i++) {
		tw_claude_usage_finish(list->items[i]);
		free(list->items[i]);
	}
	list_free(list);
}

double tw_claude_window_now(const struct tw_claude_window *window, time_t now) {
	if (!window->known) {
		return -1;
	}
	if (window->resets_at > 0 && now >= window->resets_at) {
		return 0; // the window started over since Claude Code showed it
	}
	return window->used < 0 ? 0 : window->used > 100 ? 100 : window->used;
}

char *tw_usage_select_dir(const struct tw_usage_tool *tool, const char *account) {
	if (account && *account) {
		return tw_claude_normalize_dir(account);
	}
	list_t *all = tool->all();
	char *dir = all->length > 0 ?
		strdup(((struct tw_claude_usage *)all->items[0])->dir) : tool->default_dir();
	tw_claude_usage_list_free(all);
	return dir;
}

const struct tw_usage_tool tw_claude_tool = {
	.type = "claude",
	.name = "Claude Code",
	.usage_site = "claude.ai",
	.usage_url = "https://claude.ai/settings/usage",
	.connects = true,
	.default_dir = tw_claude_default_dir,
	.read = tw_claude_usage_read,
	.all = tw_claude_usage_all,
};

/* ---------- what the widget shows ---------- */

const struct tw_claude_item tw_claude_items[] = {
	{ "usage_5h", "Usage of the 5-hour limit", "5h", false, TW_CLAUDE_USAGE },
	{ "usage_7d", "Usage of the weekly limit", "7d", true, TW_CLAUDE_USAGE },
	{ "reset_5h", "Time until the 5-hour limit resets", "5h", false, TW_CLAUDE_RESET },
	{ "reset_7d", "Time until the weekly limit resets", "7d", true, TW_CLAUDE_RESET },
	{ "left_5h", "Time until the 5-hour limit is used up, at this pace", "5h", false,
		TW_CLAUDE_LEFT },
	{ "left_7d", "Time until the weekly limit is used up, at this pace", "7d", true,
		TW_CLAUDE_LEFT },
};
const size_t tw_claude_item_count = sizeof(tw_claude_items) / sizeof(tw_claude_items[0]);

const struct tw_claude_item *tw_claude_item_find(const char *id) {
	for (size_t i = 0; id && i < tw_claude_item_count; i++) {
		if (strcmp(tw_claude_items[i].id, id) == 0) {
			return &tw_claude_items[i];
		}
	}
	return NULL;
}

const struct tw_claude_preset tw_claude_taskbar_presets[] = {
	{ "Both limits", TW_CLAUDE_SHOW_TASKBAR },
	{ "The 5-hour limit and when it resets", "usage_5h reset_5h" },
	{ "The 5-hour limit, its reset and how long it lasts", "usage_5h reset_5h left_5h" },
	{ "Both limits and when the 5 hours reset", "usage_5h reset_5h usage_7d" },
	{ "Only the 5-hour limit", "usage_5h" },
	{ 0 },
};

const struct tw_claude_preset tw_claude_desktop_presets[] = {
	{ "Everything", TW_CLAUDE_SHOW_DESKTOP },
	{ "Both limits and when they reset", "usage_5h reset_5h usage_7d reset_7d" },
	{ "Both limits and how long they last", "usage_5h left_5h usage_7d left_7d" },
	{ "The 5 hours in full", "usage_5h reset_5h left_5h" },
	{ "Both limits", TW_CLAUDE_SHOW_TASKBAR },
	{ 0 },
};

double tw_claude_window_pace(const struct tw_claude_window *window, long length, time_t now,
		double used) {
	time_t from = window->resets_at - length;
	double base = 0;
	if (window->pace_from > from && window->pace_from <= now) {
		from = window->pace_from;
		base = window->pace_base;
		// an hour at least, so a reset just now does not make a pace of a few readings
		if (now - from < 3600) {
			from = now - 3600;
		}
	}
	if (now - from < 60 || used <= base) {
		return 0;
	}
	return (used - base) / (double)(now - from);
}

long tw_claude_window_left(const struct tw_claude_window *window, long length, time_t now,
		bool *known) {
	double used = tw_claude_window_now(window, now);
	*known = used >= 0;
	if (used < 0 || window->resets_at <= now) {
		return -1; // nothing known, or started over: it lasts
	}
	if (used >= 100) {
		return 0;
	}
	// the pace since the window started, or since it was reset by hand
	double pace = tw_claude_window_pace(window, length, now, used);
	if (pace <= 0) {
		return -1;
	}
	double left = (100 - used) / pace;
	if (now + left >= window->resets_at) {
		return -1; // lasts until it resets
	}
	return (long)left;
}

void tw_claude_format_span(long seconds, bool brief, char *out, size_t size) {
	if (seconds < 60) {
		snprintf(out, size, brief ? "<1m" : "less than a minute");
		return;
	}
	long minutes = seconds / 60, hours = minutes / 60, days = hours / 24;
	if (days > 0) {
		snprintf(out, size, brief ? "%ldd %ldh" : "%ld d %ld h", days, hours % 24);
	} else if (hours > 0) {
		snprintf(out, size, brief ? "%ldh %ldm" : "%ld h %ld min", hours, minutes % 60);
	} else {
		snprintf(out, size, brief ? "%ldm" : "%ld min", minutes);
	}
}

/* ---------- the history ---------- */

// a history file is written again with only its last days once it is this big
// for each day it keeps (8 days: 96 KiB)
#define HISTORY_BYTES_A_DAY (12 * 1024)

int tw_usage_history_days(void) {
	static int days = TW_USAGE_HISTORY_DAYS;
	static time_t read_mtime = -1;
	static char *path;
	if (!path) {
		char *dir = tw_config_dir();
		path = dir ? format_str("%s/taskbar.conf", dir) : NULL;
		free(dir);
		if (!path) {
			return days;
		}
	}
	struct stat st;
	time_t mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
	if (mtime == read_mtime) {
		return days;
	}
	read_mtime = mtime;
	days = TW_USAGE_HISTORY_DAYS;
	struct twconf_node *root = mtime ? twconf_parse_file(path, NULL) : NULL;
	for (int i = 0; root && i < twconf_count(root); i++) {
		struct twconf_node *w = twconf_at(root, i);
		if (strcmp(w->name, "widget") == 0 && w->argc >= 1 && strcmp(w->argv[0], "agents") == 0) {
			const char *value = twconf_value(w, "history_days");
			int n = value ? atoi(value) : 0;
			if (n > 0) {
				days = n < TW_USAGE_HISTORY_DAYS ? TW_USAGE_HISTORY_DAYS : n > 3650 ? 3650 : n;
			}
		}
	}
	twconf_free(root);
	return days;
}

char *tw_usage_history_path(const char *kind, const char *dir) {
	if (!dir) {
		return NULL;
	}
	char *name = strdup(dir);
	for (char *p = name; *p; p++) {
		if (*p == '/') {
			*p = '-';
		}
	}
	char *state = tw_state_dir();
	char *path = format_str("%s/%s/%s.history", state ? state : "/tmp", kind, name);
	free(state);
	free(name);
	return path;
}

char *tw_claude_history_path(const char *dir) {
	return tw_usage_history_path("claude-usage", dir);
}

bool tw_claude_same_window(time_t a, time_t b) {
	return a > 0 && b > 0 && (a > b ? a - b : b - a) <= 300;
}

bool tw_claude_window_older(const struct tw_claude_window *a, const struct tw_claude_window *b) {
	return a->known && b->known && a->resets_at > 0 && b->resets_at > 0 &&
		a->resets_at + 300 < b->resets_at;
}

/* "<time> <5h used> <5h reset> <7d used> <7d reset>", "-" for what is not known. */
static bool parse_point(const char *line, struct tw_claude_point *p) {
	char f[4][32];
	long long at;
	*p = (struct tw_claude_point){ 0 };
	if (sscanf(line, "%lld %31s %31s %31s %31s", &at, f[0], f[1], f[2], f[3]) != 5 || at <= 0) {
		return false;
	}
	p->at = (time_t)at;
	struct tw_claude_window *w[2] = { &p->five_hour, &p->seven_day };
	for (int i = 0; i < 2; i++) {
		if (strcmp(f[2 * i], "-") != 0) {
			w[i]->known = true;
			w[i]->used = strtod(f[2 * i], NULL);
		}
		if (strcmp(f[2 * i + 1], "-") != 0) {
			w[i]->resets_at = (time_t)strtoll(f[2 * i + 1], NULL, 10);
		}
	}
	return true;
}

static void format_window(const struct tw_claude_window *w, char *out, size_t size) {
	char used[24] = "-", reset[24] = "-";
	if (w->known) {
		snprintf(used, sizeof(used), "%.1f", w->used);
	}
	if (w->resets_at > 0) {
		snprintf(reset, sizeof(reset), "%lld", (long long)w->resets_at);
	}
	snprintf(out, size, "%s %s", used, reset);
}

int tw_claude_history_read(const char *path, struct tw_claude_point **out) {
	*out = NULL;
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return 0;
	}
	int count = 0, cap = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		struct tw_claude_point p;
		if (!parse_point(line, &p)) {
			continue;
		}
		if (count == cap) {
			cap = cap ? cap * 2 : 128;
			struct tw_claude_point *grown = realloc(*out, cap * sizeof(**out));
			if (!grown) {
				break;
			}
			*out = grown;
		}
		(*out)[count++] = p;
	}
	fclose(f);
	return count;
}

static bool window_differs(const struct tw_claude_window *a, const struct tw_claude_window *b) {
	return a->known != b->known || (a->known && fabs(a->used - b->used) >= 0.05) ||
		!(a->resets_at == b->resets_at || tw_claude_same_window(a->resets_at, b->resets_at));
}

bool tw_claude_history_write(const char *path, const struct tw_claude_point *points, int n) {
	char *dir = path ? strdup(path) : NULL;
	char *slash = dir ? strrchr(dir, '/') : NULL;
	if (slash) {
		*slash = '\0';
		tw_mkdir_p(dir);
	}
	free(dir);
	FILE *f = path ? fopen(path, "a") : NULL;
	if (!f) {
		return false;
	}
	struct tw_claude_point last = { 0 };
	bool any = false;
	for (int i = 0; i < n; i++) {
		struct tw_claude_point p = points[i];
		if (any) {
			// a window never goes back (a stale session's after a reset by hand)
			if (tw_claude_window_older(&p.five_hour, &last.five_hour)) {
				p.five_hour = last.five_hour;
			}
			if (tw_claude_window_older(&p.seven_day, &last.seven_day)) {
				p.seven_day = last.seven_day;
			}
			// within the same window what was used only grows
			if (p.five_hour.known && last.five_hour.known &&
					tw_claude_same_window(p.five_hour.resets_at, last.five_hour.resets_at) &&
					last.five_hour.used > p.five_hour.used) {
				p.five_hour.used = last.five_hour.used;
			}
			if (p.seven_day.known && last.seven_day.known &&
					tw_claude_same_window(p.seven_day.resets_at, last.seven_day.resets_at) &&
					last.seven_day.used > p.seven_day.used) {
				p.seven_day.used = last.seven_day.used;
			}
			if (!window_differs(&last.five_hour, &p.five_hour) &&
					!window_differs(&last.seven_day, &p.seven_day)) {
				continue; // the same as the line before
			}
		}
		char a[64], b[64];
		format_window(&p.five_hour, a, sizeof(a));
		format_window(&p.seven_day, b, sizeof(b));
		fprintf(f, "%lld %s %s\n", (long long)p.at, a, b);
		last = p;
		any = true;
	}
	fclose(f);
	return true;
}

void tw_claude_history_add(const char *history, const struct tw_claude_usage *usage,
		time_t now) {
	char *path = history ? strdup(history) : NULL;
	if (!path) {
		return;
	}
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		tw_mkdir_p(path);
		*slash = '/';
	}
	struct tw_claude_point *points;
	int n = tw_claude_history_read(path, &points);
	// the values are those tilewin-claude-usage took, which tells a stale session
	// from a fresh answer: lower in the same window is usage reset by hand
	struct tw_claude_usage u = *usage;
	if (n > 0) {
		const struct tw_claude_point *last = &points[n - 1];
		// a window never goes back: an older one is a stale session's
		if (tw_claude_window_older(&u.five_hour, &last->five_hour)) {
			u.five_hour = last->five_hour;
		}
		if (tw_claude_window_older(&u.seven_day, &last->seven_day)) {
			u.seven_day = last->seven_day;
		}
	}
	usage = &u;
	if (n > 0 && !window_differs(&points[n - 1].five_hour, &usage->five_hour) &&
			!window_differs(&points[n - 1].seven_day, &usage->seven_day)) {
		free(points);
		free(path);
		return; // the same as the last line
	}
	char five[64], seven[64];
	format_window(&usage->five_hour, five, sizeof(five));
	format_window(&usage->seven_day, seven, sizeof(seven));
	struct stat st;
	int days = tw_usage_history_days();
	if (stat(path, &st) == 0 && st.st_size > (off_t)days * HISTORY_BYTES_A_DAY) {
		// grown: written again with only the last days
		char *tmp = format_str("%s.new", path);
		FILE *f = fopen(tmp, "w");
		for (int i = 0; f && i < n; i++) {
			if (points[i].at < now - (time_t)days * 24 * 3600) {
				continue;
			}
			char a[64], b[64];
			format_window(&points[i].five_hour, a, sizeof(a));
			format_window(&points[i].seven_day, b, sizeof(b));
			fprintf(f, "%lld %s %s\n", (long long)points[i].at, a, b);
		}
		if (f) {
			fprintf(f, "%lld %s %s\n", (long long)now, five, seven);
			fclose(f);
			rename(tmp, path);
		}
		free(tmp);
	} else {
		FILE *f = fopen(path, "a");
		if (f) {
			fprintf(f, "%lld %s %s\n", (long long)now, five, seven);
			fclose(f);
		}
	}
	free(points);
	free(path);
}
