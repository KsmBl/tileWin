/*
 * The usage limits of Codex from its session logs (tw_codex_usage.h). The
 * names of the fields are those of the rate limit snapshot of Codex (codex-rs,
 * protocol, RateLimitSnapshot and RateLimitWindow, Apache-2.0), checked
 * against the logs of codex-cli 0.167.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "stringop.h"
#include "tw_codex_usage.h"
#include "tw_paths.h"

#define HISTORY_DAYS 8
#define FIRST_TAIL (256 * 1024)
#define MAX_TAIL (64 * 1024 * 1024)

char *tw_codex_default_dir(void) {
	const char *env = getenv("CODEX_HOME");
	if (env && *env) {
		return tw_claude_normalize_dir(env);
	}
	const char *home = getenv("HOME");
	char *dir = format_str("%s/.codex", home ? home : "");
	char *out = tw_claude_normalize_dir(dir);
	free(dir);
	return out;
}

char *tw_codex_history_path(const char *dir) {
	return tw_usage_history_path("codex-usage", dir);
}

/* ---------- one line of a session log ---------- */

struct codex_point {
	struct tw_claude_point p;
	char plan[32];
};

/* "2026-11-09T18:06:27.536Z" as a time; 0 when it is not one. */
static time_t parse_time(const char *text) {
	struct tm tm = { 0 };
	if (!text || sscanf(text, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
			&tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
		return 0;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	return timegm(&tm);
}

/* A window of the snapshot; its length in minutes (0 when not named). */
static bool parse_window(json_object *w, time_t at, struct tw_claude_window *out, long *minutes) {
	json_object *v;
	*out = (struct tw_claude_window){ 0 };
	*minutes = 0;
	if (!w || !json_object_is_type(w, json_type_object) ||
			!json_object_object_get_ex(w, "used_percent", &v) ||
			json_object_is_type(v, json_type_null)) {
		return false;
	}
	out->known = true;
	out->used = json_object_get_double(v);
	if (json_object_object_get_ex(w, "window_minutes", &v) && !json_object_is_type(v, json_type_null)) {
		*minutes = (long)json_object_get_int64(v);
	}
	if (json_object_object_get_ex(w, "resets_at", &v) && !json_object_is_type(v, json_type_null)) {
		out->resets_at = (time_t)json_object_get_int64(v);
	} else if (json_object_object_get_ex(w, "resets_in_seconds", &v) &&
			!json_object_is_type(v, json_type_null)) {
		out->resets_at = at + (time_t)json_object_get_int64(v); // older Codex
	}
	return true;
}

/* A line with rate limits into a point; false for any other line. */
static bool parse_line(const char *line, size_t len, struct codex_point *out) {
	if (!memmem(line, len, "\"rate_limits\"", 13)) {
		return false; // most lines: not worth parsing
	}
	json_tokener *tok = json_tokener_new();
	json_object *root = json_tokener_parse_ex(tok, line, (int)len);
	json_tokener_free(tok);
	json_object *payload, *limits, *v;
	bool ok = false;
	*out = (struct codex_point){ 0 };
	if (root && json_object_object_get_ex(root, "payload", &payload) &&
			json_object_object_get_ex(payload, "rate_limits", &limits) &&
			json_object_is_type(limits, json_type_object) &&
			json_object_object_get_ex(root, "timestamp", &v)) {
		out->p.at = parse_time(json_object_get_string(v));
		struct tw_claude_window w[2];
		long minutes[2];
		const char *keys[2] = { "primary", "secondary" };
		for (int i = 0; i < 2; i++) {
			json_object *win = NULL;
			json_object_object_get_ex(limits, keys[i], &win);
			if (!parse_window(win, out->p.at, &w[i], &minutes[i])) {
				continue;
			}
			// by its length: up to a day is the short window, longer the week;
			// not named, primary is the 5 hours and secondary the week
			bool week = minutes[i] > 0 ? minutes[i] > 24 * 60 : i == 1;
			struct tw_claude_window *slot = week ? &out->p.seven_day : &out->p.five_hour;
			if (slot->known) {
				slot = week ? &out->p.five_hour : &out->p.seven_day;
			}
			*slot = w[i];
		}
		if (json_object_object_get_ex(limits, "plan_type", &v) &&
				json_object_is_type(v, json_type_string)) {
			snprintf(out->plan, sizeof(out->plan), "%s", json_object_get_string(v));
		}
		ok = out->p.at > 0 && (out->p.five_hour.known || out->p.seven_day.known);
	}
	json_object_put(root);
	return ok;
}

/* ---------- the session logs ---------- */

struct log_file {
	char *path;
	time_t mtime;
};

struct log_list {
	struct log_file *items;
	int count, cap;
};

/*
 * The logs below sessions/, of the days since `cutoff` (the folders are named
 * by the day a session started: "2026/11/09"); a folder older than that is not
 * gone into.
 */
static void collect(const char *base, const char *rel, int depth, const char *cutoff,
		struct log_list *out) {
	char *path = *rel ? format_str("%s/%s", base, rel) : strdup(base);
	DIR *d = opendir(path);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char *sub = *rel ? format_str("%s/%s", rel, de->d_name) : strdup(de->d_name);
		if (depth < 3) {
			// "2026", "2026/11", "2026/11/09" against as much of the cutoff
			size_t n = strlen(sub);
			if (n <= strlen(cutoff) && strncmp(sub, cutoff, n) >= 0) {
				collect(base, sub, depth + 1, cutoff, out);
			}
		} else {
			size_t n = strlen(de->d_name);
			struct stat st;
			char *file = format_str("%s/%s", path, de->d_name);
			if (strncmp(de->d_name, "rollout-", 8) == 0 && n > 6 &&
					strcmp(de->d_name + n - 6, ".jsonl") == 0 && stat(file, &st) == 0 &&
					S_ISREG(st.st_mode)) {
				if (out->count == out->cap) {
					int cap = out->cap ? out->cap * 2 : 32;
					struct log_file *grown = realloc(out->items, cap * sizeof(*out->items));
					if (grown) {
						out->items = grown;
						out->cap = cap;
					}
				}
				if (out->count < out->cap) {
					out->items[out->count++] = (struct log_file){ file, st.st_mtime };
					file = NULL;
				}
			}
			free(file);
		}
		free(sub);
	}
	if (d) {
		closedir(d);
	}
	free(path);
}

static int newest_file_first(const void *a, const void *b) {
	const struct log_file *fa = a, *fb = b;
	return fa->mtime < fb->mtime ? 1 : fa->mtime > fb->mtime ? -1 : 0;
}

static struct log_list session_logs(const char *dir, time_t now) {
	struct log_list list = { 0 };
	time_t from = now - HISTORY_DAYS * 24 * 3600;
	struct tm tm;
	localtime_r(&from, &tm);
	char cutoff[40];
	snprintf(cutoff, sizeof(cutoff), "%04d/%02d/%02d", tm.tm_year + 1900, tm.tm_mon + 1,
		tm.tm_mday);
	char *base = format_str("%s/sessions", dir);
	collect(base, "", 0, cutoff, &list);
	free(base);
	if (list.count > 1) {
		qsort(list.items, list.count, sizeof(*list.items), newest_file_first);
	}
	return list;
}

static void logs_free(struct log_list *list) {
	for (int i = 0; i < list->count; i++) {
		free(list->items[i].path);
	}
	free(list->items);
}

/*
 * The last line with limits of a log, from its end: a bigger piece each time
 * until one is found or the whole log was read.
 */
static bool last_point(const char *path, struct codex_point *out) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	struct stat st;
	bool found = false;
	if (fstat(fileno(f), &st) != 0) {
		fclose(f);
		return false;
	}
	for (off_t piece = FIRST_TAIL; !found; piece *= 4) {
		off_t from = st.st_size > piece ? st.st_size - piece : 0;
		size_t len = (size_t)(st.st_size - from);
		char *buf = malloc(len + 1);
		if (!buf || fseeko(f, from, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
			free(buf);
			break;
		}
		buf[len] = '\0';
		// the lines from the last, the first one cut off unless at the start
		char *end = buf + len;
		while (end > buf && !found) {
			char *start = end;
			while (start > buf && start[-1] != '\n') {
				start--;
			}
			if (start == buf && from > 0) {
				break;
			}
			found = end > start && parse_line(start, (size_t)(end - start), out);
			end = start > buf ? start - 1 : buf;
		}
		free(buf);
		if (from == 0 || piece >= MAX_TAIL) {
			break;
		}
	}
	fclose(f);
	return found;
}

/* "plus" as "ChatGPT Plus". */
static char *plan_name(const char *plan) {
	if (!plan || !*plan) {
		return NULL;
	}
	char *out = format_str("ChatGPT %s", plan);
	char *first = out + strlen("ChatGPT ");
	*first = (char)toupper((unsigned char)*first);
	for (char *p = first; *p; p++) {
		if (*p == '_') {
			*p = ' ';
		}
	}
	return out;
}

/* Within the same window what was used only grows: what the history has stays. */
static void keep_highest(struct tw_claude_window *w, const struct tw_claude_window *seen) {
	if (tw_claude_window_older(w, seen)) {
		*w = *seen; // a window never goes back
		return;
	}
	if (w->known && seen->known && tw_claude_same_window(w->resets_at, seen->resets_at) &&
			seen->used > w->used) {
		w->used = seen->used;
	}
}

bool tw_codex_usage_read(const char *dir, struct tw_claude_usage *out) {
	*out = (struct tw_claude_usage){ 0 };
	out->dir = tw_claude_normalize_dir(dir);
	if (!out->dir) {
		return false;
	}
	out->history = tw_codex_history_path(out->dir);
	char *sessions = format_str("%s/sessions", out->dir);
	struct stat st;
	out->connected = stat(sessions, &st) == 0 && S_ISDIR(st.st_mode);
	free(sessions);
	if (!out->connected) {
		return false;
	}
	// several sessions may run: the newest line of all, and a log is never older
	// than its newest line, so the logs written before the best one so far are
	// passed over
	struct log_list logs = session_logs(out->dir, time(NULL));
	struct codex_point best = { 0 }, p;
	for (int i = 0; i < logs.count; i++) {
		if (best.p.at > 0 && logs.items[i].mtime < best.p.at) {
			break;
		}
		if (last_point(logs.items[i].path, &p) && p.p.at > best.p.at) {
			best = p;
		}
	}
	logs_free(&logs);
	if (best.p.at <= 0) {
		return false;
	}
	struct tw_claude_point *points;
	int n = tw_claude_history_read(out->history, &points);
	if (n > 0) {
		keep_highest(&best.p.five_hour, &points[n - 1].five_hour);
		keep_highest(&best.p.seven_day, &points[n - 1].seven_day);
	}
	free(points);
	out->updated = best.p.at;
	out->five_hour = best.p.five_hour;
	out->seven_day = best.p.seven_day;
	out->account = plan_name(best.plan);
	return true;
}

list_t *tw_codex_usage_all(void) {
	list_t *list = create_list();
	char *dir = tw_codex_default_dir();
	struct stat st;
	if (dir && stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
		struct tw_claude_usage *u = calloc(1, sizeof(*u));
		tw_codex_usage_read(dir, u);
		list_add(list, u);
	}
	free(dir);
	return list;
}

/* ---------- the history from the logs ---------- */

static int oldest_point_first(const void *a, const void *b) {
	const struct codex_point *pa = a, *pb = b;
	return pa->p.at < pb->p.at ? -1 : pa->p.at > pb->p.at ? 1 : 0;
}

int tw_codex_history_points(const char *dir, time_t now, struct tw_claude_point **out) {
	*out = NULL;
	if (!dir) {
		return 0;
	}
	time_t from = now - HISTORY_DAYS * 24 * 3600;
	struct log_list logs = session_logs(dir, now);
	struct codex_point *points = NULL;
	int count = 0, cap = 0;
	for (int i = 0; i < logs.count; i++) {
		if (logs.items[i].mtime < from) {
			continue;
		}
		FILE *f = fopen(logs.items[i].path, "r");
		char *line = NULL;
		size_t size = 0;
		ssize_t len;
		while (f && (len = getline(&line, &size, f)) > 0) {
			struct codex_point p;
			if (!parse_line(line, (size_t)len, &p) || p.p.at < from || p.p.at > now) {
				continue;
			}
			if (count == cap) {
				int grown_cap = cap ? cap * 2 : 256;
				struct codex_point *grown = realloc(points, grown_cap * sizeof(*points));
				if (!grown) {
					break;
				}
				points = grown;
				cap = grown_cap;
			}
			points[count++] = p;
		}
		free(line);
		if (f) {
			fclose(f);
		}
	}
	logs_free(&logs);
	if (count > 1) {
		qsort(points, count, sizeof(*points), oldest_point_first);
	}
	struct tw_claude_point *plain = count > 0 ? malloc(count * sizeof(*plain)) : NULL;
	for (int i = 0; plain && i < count; i++) {
		plain[i] = points[i].p;
	}
	free(points);
	*out = plain;
	return plain ? count : 0;
}

int tw_codex_history_backfill(const char *dir, time_t now) {
	char *normal = tw_claude_normalize_dir(dir);
	char *path = tw_codex_history_path(normal);
	if (!path || access(path, F_OK) == 0) {
		free(path);
		free(normal);
		return 0;
	}
	struct tw_claude_point *points = NULL;
	int count = tw_codex_history_points(normal, now, &points);
	tw_claude_history_write(path, points, count);
	free(points);
	free(path);
	free(normal);
	return count;
}

const struct tw_usage_tool tw_codex_tool = {
	.type = "codex",
	.name = "Codex",
	.usage_site = "chatgpt.com",
	.usage_url = "https://chatgpt.com/codex/settings/usage",
	.connects = false,
	.default_dir = tw_codex_default_dir,
	.read = tw_codex_usage_read,
	.all = tw_codex_usage_all,
	.backfill = tw_codex_history_backfill,
};
