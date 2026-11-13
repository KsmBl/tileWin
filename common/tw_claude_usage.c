#define _GNU_SOURCE
#include <dirent.h>
#include <json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "stringop.h"
#include "tw_claude_usage.h"
#include "tw_paths.h"

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
	return true;
}

void tw_claude_usage_finish(struct tw_claude_usage *usage) {
	free(usage->dir);
	free(usage->account);
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
	// the pace since the window started: the share used over the time gone
	time_t start = window->resets_at - length;
	double gone = (double)(now - start);
	if (used <= 0 || gone < 60) {
		return -1;
	}
	double left = (100 - used) * gone / used;
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
