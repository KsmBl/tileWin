/*
 * The usage limits of Codex for the codex widget, read from the session logs
 * Codex writes itself (a folder of the test's own as $CODEX_HOME; no real
 * Codex is run or touched):
 *  - the limits are those of the newest line with rate_limits, of whichever
 *    session answered last, even when another session's log was written later;
 *  - a window is taken by its length (300 minutes or a week), not by whether
 *    Codex calls it primary or secondary; resets_in_seconds of older Codex
 *    counts from the time of its line;
 *  - lines without limits (an API key: rate_limits null) are passed over;
 *  - the plan is the account: "plus" becomes ChatGPT Plus;
 *  - a folder without session logs has nothing to show;
 *  - the history of the charts is filled from the logs of the last eight days
 *    the first time, in the order of time, and only once;
 *  - the Codex tool reads as it is, nothing to connect.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "tw_claude_usage.h"
#include "tw_codex_usage.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}

static bool near(double a, double b) {
	return fabs(a - b) < 1e-6;
}

static char root[] = "/tmp/tw-codex-XXXXXX";
static char home[600];

static void mkdirs(const char *path) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(buf, 0755);
			*p = '/';
		}
	}
	mkdir(buf, 0755);
}

static void iso(time_t t, char *out, size_t size) {
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, size, "%Y-%m-%dT%H:%M:%S.123Z", &tm);
}

/* A session log started at `start`, its last change at `mtime`; returns its path. */
static char *session(time_t start, const char *name, const char *lines, time_t mtime) {
	struct tm tm;
	localtime_r(&start, &tm);
	char dir[800];
	snprintf(dir, sizeof(dir), "%s/sessions/%04d/%02d/%02d", home, tm.tm_year + 1900,
		tm.tm_mon + 1, tm.tm_mday);
	mkdirs(dir);
	char *path = NULL;
	if (asprintf(&path, "%s/rollout-%s.jsonl", dir, name) < 0) {
		return NULL;
	}
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(lines, f);
		fclose(f);
	}
	struct timeval tv[2] = { { mtime, 0 }, { mtime, 0 } };
	utimes(path, tv);
	return path;
}

/* A token_count line as Codex writes it, the 5-hour window first unless swapped. */
static char *limits_line(time_t at, double five, time_t five_reset, double week,
		time_t week_reset, bool swapped) {
	char when[40], a[200], b[200], *out = NULL;
	iso(at, when, sizeof(when));
	snprintf(a, sizeof(a), "{\"used_percent\":%.1f,\"window_minutes\":300,\"resets_at\":%lld}",
		five, (long long)five_reset);
	snprintf(b, sizeof(b), "{\"used_percent\":%.1f,\"window_minutes\":10080,\"resets_at\":%lld}",
		week, (long long)week_reset);
	if (asprintf(&out, "{\"timestamp\":\"%s\",\"type\":\"event_msg\",\"payload\":{\"type\":"
			"\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":1}},\"rate_limits\":"
			"{\"limit_id\":\"codex\",\"limit_name\":null,\"primary\":%s,\"secondary\":%s,"
			"\"credits\":null,\"plan_type\":\"plus\"}}}\n", when, swapped ? b : a,
			swapped ? a : b) < 0) {
		return NULL;
	}
	return out;
}

int main(void) {
	if (!mkdtemp(root)) {
		return 1;
	}
	snprintf(home, sizeof(home), "%s/codex", root);
	char state[700];
	snprintf(state, sizeof(state), "%s/state", root);
	mkdirs(home);
	mkdirs(state);
	setenv("CODEX_HOME", home, 1);
	setenv("XDG_STATE_HOME", state, 1);

	time_t now = time(NULL);
	time_t five_reset = now + 3600, week_reset = now + 3 * 86400;

	// ---- the folder and an empty one
	char *dir = tw_codex_default_dir();
	check(dir && strcmp(dir, home) == 0, "the folder is $CODEX_HOME");
	struct tw_claude_usage u;
	check(!tw_codex_usage_read(dir, &u), "a folder without session logs has nothing to show");
	check(!u.connected, "a folder without session logs is not taken for a Codex in use");
	tw_claude_usage_finish(&u);

	// ---- an old session, its limits ten days ago: past the history's days
	char *l_old = limits_line(now - 10 * 86400, 70, now - 10 * 86400 + 3600, 90,
		now - 8 * 86400, false);
	free(session(now - 10 * 86400, "old", l_old, now - 10 * 86400));

	// ---- yesterday's session, still running: its limits rose and were written last
	char meta[] = "{\"timestamp\":\"2026-01-01T00:00:00.000Z\",\"type\":\"session_meta\","
		"\"payload\":{\"id\":\"x\"}}\n";
	char *l1 = limits_line(now - 600, 10, five_reset, 40, week_reset, false);
	char *l2 = limits_line(now - 300, 20, five_reset, 41, week_reset, false);
	// another session's older, lower value at a later time: a window only grows
	char *l3 = limits_line(now - 200, 15, five_reset, 41, week_reset, false);
	char *yesterday = NULL;
	if (asprintf(&yesterday, "%s%s%s%s", meta, l1, l2, l3) < 0) {
		return 1;
	}
	free(session(now - 86400, "yesterday", yesterday, now - 200));

	// ---- today's session, its log written later but its limits older; an API-key
	// line at the end, and the windows swapped
	char when[40], apikey[300];
	iso(now - 100, when, sizeof(when));
	snprintf(apikey, sizeof(apikey), "{\"timestamp\":\"%s\",\"type\":\"event_msg\",\"payload\":"
		"{\"type\":\"token_count\",\"info\":null,\"rate_limits\":null}}\n", when);
	char *l4 = limits_line(now - 400, 5, five_reset, 39, week_reset, true);
	char *today = NULL;
	if (asprintf(&today, "%s%s%s", meta, l4, apikey) < 0) {
		return 1;
	}
	free(session(now, "today", today, now - 100));

	check(tw_codex_usage_read(dir, &u), "the limits are read from the session logs");
	check(u.connected, "a folder with session logs is a Codex in use");
	check(u.updated == now - 200, "the newest line with limits counts, of whichever session");
	check(u.five_hour.known && near(u.five_hour.used, 15), "the 5-hour usage of that line");
	check(u.five_hour.resets_at == five_reset, "when the 5-hour window resets");
	check(u.seven_day.known && near(u.seven_day.used, 41), "the weekly usage of that line");
	check(u.seven_day.resets_at == week_reset, "when the week resets");
	check(u.account && strcmp(u.account, "ChatGPT Plus") == 0, "the plan is the account");
	check(u.history && strstr(u.history, "/codex-usage/"), "the history is kept apart from Claude's");
	tw_claude_usage_finish(&u);

	// ---- windows by their length, and resets_in_seconds of older Codex
	char *l5 = limits_line(now - 50, 33, five_reset, 44, week_reset, true);
	char *newest = session(now, "swapped", l5, now - 50);
	check(tw_codex_usage_read(dir, &u) && near(u.five_hour.used, 33) &&
		near(u.seven_day.used, 44), "the windows are taken by their length, not their order");
	tw_claude_usage_finish(&u);
	char relative[600];
	iso(now - 40, when, sizeof(when));
	snprintf(relative, sizeof(relative), "{\"timestamp\":\"%s\",\"type\":\"event_msg\","
		"\"payload\":{\"type\":\"token_count\",\"info\":null,\"rate_limits\":{\"primary\":"
		"{\"used_percent\":50.0,\"window_minutes\":300,\"resets_in_seconds\":1200},"
		"\"secondary\":{\"used_percent\":60.0,\"window_minutes\":10080,"
		"\"resets_in_seconds\":7200}}}}\n", when);
	free(session(now, "relative", relative, now - 40));
	check(tw_codex_usage_read(dir, &u) && u.five_hour.resets_at == now - 40 + 1200 &&
		u.seven_day.resets_at == now - 40 + 7200, "resets_in_seconds counts from its line");
	check(u.account == NULL, "no plan named: no account");
	tw_claude_usage_finish(&u);

	// ---- the history, filled from the logs of the last eight days, once
	char *history = tw_codex_history_path(dir);
	check(access(history, F_OK) != 0, "no history before it is filled");
	int added = tw_codex_history_backfill(dir, now);
	check(added > 0, "the history is filled from the session logs");
	struct tw_claude_point *points;
	int n = tw_claude_history_read(history, &points);
	bool ordered = n > 0;
	for (int i = 1; i < n; i++) {
		ordered = ordered && points[i - 1].at <= points[i].at;
	}
	check(ordered, "the history is in the order of time");
	check(n > 0 && points[0].at == now - 600, "the history starts with the oldest line of the "
		"last eight days");
	bool lowered = false;
	for (int i = 1; i < n; i++) {
		lowered = lowered || (points[i].five_hour.used < points[i - 1].five_hour.used &&
			tw_claude_same_window(points[i].five_hour.resets_at,
				points[i - 1].five_hour.resets_at));
	}
	check(!lowered, "an older, lower value does not lower a window in the history");
	free(points);
	// a lower value of the same window after what the history has: it stays as high
	char *l6 = NULL;
	iso(now - 30, when, sizeof(when));
	if (asprintf(&l6, "{\"timestamp\":\"%s\",\"type\":\"event_msg\",\"payload\":{\"type\":"
			"\"token_count\",\"info\":null,\"rate_limits\":{\"primary\":{\"used_percent\":45.0,"
			"\"window_minutes\":300,\"resets_at\":%lld},\"secondary\":{\"used_percent\":60.0,"
			"\"window_minutes\":10080,\"resets_at\":%lld}}}}\n", when,
			(long long)(now - 40 + 1200), (long long)(now - 40 + 7200)) < 0) {
		return 1;
	}
	free(session(now, "lower", l6, now - 30));
	free(l6);
	check(tw_codex_usage_read(dir, &u) && u.updated == now - 30 && near(u.five_hour.used, 50),
		"a window's usage only grows: an older, lower value does not lower it");
	tw_claude_usage_finish(&u);
	check(tw_codex_history_backfill(dir, now) == 0, "the history is filled only once");
	int again = tw_claude_history_read(history, &points);
	check(again == n, "filling again adds nothing");
	free(points);

	// ---- the list and the tool
	list_t *all = tw_codex_usage_all();
	check(all->length == 1 && strcmp(((struct tw_claude_usage *)all->items[0])->dir, home) == 0,
		"the folder is listed");
	tw_claude_usage_list_free(all);
	check(strcmp(tw_codex_tool.type, "codex") == 0 && strcmp(tw_codex_tool.name, "Codex") == 0,
		"the tool is Codex");
	check(!tw_codex_tool.connects && tw_claude_tool.connects, "Codex has nothing to connect; "
		"Claude Code does");

	free(history);
	free(newest);
	free(dir);
	free(l_old);
	free(l1);
	free(l2);
	free(l3);
	free(l4);
	free(l5);
	free(yesterday);
	free(today);
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0) {
		fprintf(stderr, "could not remove %s\n", root);
	}
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
