/*
 * tilewin-claude-usage: the usage limits of Claude Code for tileWin's Claude
 * widget, as Claude Code itself shows them, without tileWin ever touching a
 * login of Claude's.
 *
 *   tilewin-claude-usage connect [DIR]      puts itself before the status line of
 *                                           the Claude Code folder DIR (~/.claude,
 *                                           or $CLAUDE_CONFIG_DIR, by default)
 *   tilewin-claude-usage disconnect [DIR]   gives the folder its status line back
 *   tilewin-claude-usage statusline [--dir DIR]
 *                                           what Claude Code runs: writes down the
 *                                           5-hour and 7-day limits of the JSON on
 *                                           standard input, then runs the status
 *                                           line the folder had (same input, its
 *                                           output shown), or shows the limits
 *   tilewin-claude-usage list               the folders known and their limits
 *
 * Claude Code passes rate_limits.five_hour and rate_limits.seven_day (the share
 * used in percent and when the window resets) to its status line command, for
 * Claude.ai subscriptions, after each answer. They are written to
 * ~/.cache/tileWin/claude-usage/<folder>.json; the status line the folder had
 * is kept in ~/.local/state/tileWin/claude-usage/<folder>.json until
 * disconnect. A portable Claude Code is connected by its own folder.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#ifdef __FreeBSD__
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "stringop.h"
#include "tw_claude_usage.h"
#include "tw_paths.h"

#define INPUT_LIMIT (4 * 1024 * 1024)

static char *read_all(int fd, size_t *len) {
	size_t size = 0, cap = 8192;
	char *buf = malloc(cap + 1);
	ssize_t n;
	while (buf && (n = read(fd, buf + size, cap - size)) != 0) {
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		size += (size_t)n;
		if (size == cap) {
			if (cap >= INPUT_LIMIT) {
				break;
			}
			cap *= 2;
			char *bigger = realloc(buf, cap + 1);
			if (!bigger) {
				break;
			}
			buf = bigger;
		}
	}
	if (buf) {
		buf[size] = '\0';
	}
	*len = size;
	return buf;
}

/* Writes the text to a file of its own next to path, then puts it in place. */
static bool write_atomic(const char *path, const char *text, mode_t mode) {
	char *dir = strdup(path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		tw_mkdir_p(dir);
	}
	free(dir);
	char *tmp = format_str("%s.tw-%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	bool ok = fd >= 0;
	size_t len = strlen(text), done = 0;
	while (ok && done < len) {
		ssize_t n = write(fd, text + done, len - done);
		if (n <= 0) {
			ok = false;
		} else {
			done += (size_t)n;
		}
	}
	if (fd >= 0 && close(fd) != 0) {
		ok = false;
	}
	ok = ok && rename(tmp, path) == 0;
	if (!ok) {
		unlink(tmp);
	}
	free(tmp);
	return ok;
}

static char *state_path(const char *dir) {
	char *name = strdup(dir);
	for (char *p = name; *p; p++) {
		if (*p == '/') {
			*p = '-';
		}
	}
	char *state = tw_state_dir();
	char *path = format_str("%s/claude-usage/%s.json", state ? state : "/tmp", name);
	free(state);
	free(name);
	return path;
}

/* ---------- statusline ---------- */

static void copy_window(json_object *limits, const char *key, json_object *out,
		json_object *session) {
	json_object *window, *used, *resets;
	if (!limits || !json_object_object_get_ex(limits, key, &window) ||
			!json_object_object_get_ex(window, "used_percentage", &used)) {
		return; // not shown this time: what was written before stays
	}
	double value = json_object_get_double(used);
	int64_t now_resets = json_object_object_get_ex(window, "resets_at", &resets) ?
		json_object_get_int64(resets) : 0;
	json_object *before, *before_used, *before_resets;
	// a window never goes back: after a reset by hand a session still running
	// shows the window before it, which resets earlier than the one written down
	bool stale_window = json_object_object_get_ex(out, key, &before) &&
		json_object_object_get_ex(before, "resets_at", &before_resets) &&
		now_resets > 0 && now_resets + 300 < json_object_get_int64(before_resets);
	// what is written down of this window, when it is the same window
	bool have_before = !stale_window && json_object_object_get_ex(out, key, &before) &&
		json_object_object_get_ex(before, "used_percentage", &before_used) &&
		json_object_object_get_ex(before, "resets_at", &before_resets) &&
		tw_claude_same_window((time_t)json_object_get_int64(before_resets), (time_t)now_resets);
	double written = have_before ? json_object_get_double(before_used) : -1;
	/*
	 * Several sessions of Claude Code each show the limits as of their own last
	 * answer, so an older, lower value can come after a newer one. Within the same
	 * window what was used only grows, except after a reset by hand: a session
	 * showing a lower value than written down counts only once it shows that
	 * changing again, still below (6, then 7, after a reset to 6); a session that
	 * stayed behind shows the same old value on and on, or its first value of
	 * this window when its status line is first drawn in it.
	 */
	bool session_prev_same = false, session_moved = false;
	double session_prev = -1;
	if (session) {
		json_object *seen, *seen_used, *seen_resets;
		if (json_object_object_get_ex(session, key, &seen) &&
				json_object_object_get_ex(seen, "used_percentage", &seen_used) &&
				json_object_object_get_ex(seen, "resets_at", &seen_resets) &&
				tw_claude_same_window((time_t)json_object_get_int64(seen_resets),
					(time_t)now_resets)) {
			session_prev_same = true;
			session_prev = json_object_get_double(seen_used);
			session_moved = session_prev != value;
		}
		json_object *mark = json_object_new_object();
		json_object_object_add(mark, "used_percentage", json_object_new_double(value));
		json_object_object_add(mark, "resets_at", json_object_new_int64(now_resets));
		json_object_object_add(session, key, mark);
	}
	if (stale_window) {
		return; // stale: what was written stays
	}
	if (session_prev_same && !session_moved) {
		return; // its status line redrawn with what it showed already: nothing new
	}
	if (have_before && value < written) {
		bool reset_by_hand = session && session_prev_same && session_moved &&
			session_prev < written;
		if (!reset_by_hand) {
			return; // an older value: what was written stays
		}
	}
	json_object *copy = json_object_new_object();
	json_object_object_add(copy, "used_percentage", json_object_new_double(value));
	if (json_object_object_get_ex(window, "resets_at", &resets)) {
		json_object_object_add(copy, "resets_at",
			json_object_new_int64(json_object_get_int64(resets)));
	}
	json_object_object_add(out, key, copy);
}

static void record(const char *dir, const char *input, size_t len) {
	json_tokener *tok = json_tokener_new();
	json_object *root = json_tokener_parse_ex(tok, input, (int)len);
	json_tokener_free(tok);
	json_object *limits = NULL;
	if (!root || !json_object_object_get_ex(root, "rate_limits", &limits) ||
			!json_object_is_type(limits, json_type_object)) {
		json_object_put(root);
		return; // an API key, or no answer yet: nothing to write down
	}
	char *path = tw_claude_cache_path(dir);
	json_object *out = json_object_from_file(path);
	if (!out || !json_object_is_type(out, json_type_object)) {
		json_object_put(out);
		out = json_object_new_object();
	}
	json_object_object_add(out, "dir", json_object_new_string(dir));
	char *account = tw_claude_account(dir);
	if (account) {
		json_object_object_add(out, "account", json_object_new_string(account));
		free(account);
	}
	time_t now = time(NULL);
	json_object_object_add(out, "updated", json_object_new_int64((int64_t)now));
	// what each session showed last, to tell a fresh answer from a redrawn one;
	// sessions not heard of for a week are forgotten
	json_object *session = NULL, *id;
	if (json_object_object_get_ex(root, "session_id", &id) &&
			json_object_is_type(id, json_type_string) && *json_object_get_string(id)) {
		json_object *sessions;
		if (!json_object_object_get_ex(out, "sessions", &sessions) ||
				!json_object_is_type(sessions, json_type_object)) {
			sessions = json_object_new_object();
			json_object_object_add(out, "sessions", sessions);
		}
		const char *gone[64];
		int ngone = 0;
		json_object_object_foreach(sessions, sid, s) {
			json_object *at;
			if (ngone < 64 && (!json_object_object_get_ex(s, "at", &at) ||
					json_object_get_int64(at) < (int64_t)now - 7 * 86400)) {
				gone[ngone++] = sid;
			}
		}
		for (int i = 0; i < ngone; i++) {
			json_object_object_del(sessions, gone[i]);
		}
		if (!json_object_object_get_ex(sessions, json_object_get_string(id), &session)) {
			session = json_object_new_object();
			json_object_object_add(sessions, json_object_get_string(id), session);
		}
		json_object_object_add(session, "at", json_object_new_int64((int64_t)now));
	}
	copy_window(limits, "five_hour", out, session);
	copy_window(limits, "seven_day", out, session);
	write_atomic(path, json_object_to_json_string_ext(out, JSON_C_TO_STRING_PRETTY), 0600);
	free(path);
	json_object_put(out);
	json_object_put(root);
	// and into the history, for the charts of the widget's flyout
	struct tw_claude_usage usage;
	if (tw_claude_usage_read(dir, &usage)) {
		tw_claude_history_add(usage.history, &usage, usage.updated ? usage.updated : time(NULL));
	}
	tw_claude_usage_finish(&usage);
}

/* The status line command the folder had before, or NULL. */
static char *previous_command(const char *dir) {
	char *path = state_path(dir);
	json_object *prev = json_object_from_file(path);
	free(path);
	json_object *command;
	char *out = NULL;
	if (prev && json_object_object_get_ex(prev, "command", &command) &&
			json_object_get_string(command) && *json_object_get_string(command)) {
		out = strdup(json_object_get_string(command));
	}
	json_object_put(prev);
	return out;
}

/* Runs the old status line with the same input; its output is the status line. */
static int run_previous(const char *command, const char *input, size_t len) {
	int fds[2];
	if (pipe(fds) != 0) {
		return 1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return 1;
	}
	if (pid == 0) {
		close(fds[1]);
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	signal(SIGPIPE, SIG_IGN);
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fds[1], input + done, len - done);
		if (n <= 0) {
			break;
		}
		done += (size_t)n;
	}
	close(fds[1]);
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void print_window(const char *name, const struct tw_claude_window *w, time_t now) {
	double used = tw_claude_window_now(w, now);
	if (used < 0) {
		return;
	}
	printf("%s %.0f%%", name, used);
	if (w->resets_at > now) {
		struct tm tm;
		char when[32];
		localtime_r(&w->resets_at, &tm);
		strftime(when, sizeof(when), w->resets_at - now < 24 * 3600 ? "%H:%M" : "%a %H:%M", &tm);
		printf(" (resets %s)", when);
	}
}

static int cmd_statusline(int argc, char **argv) {
	char *dir = NULL;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
			dir = tw_claude_normalize_dir(argv[++i]);
		}
	}
	if (!dir) {
		dir = tw_claude_default_dir();
	}
	size_t len = 0;
	char *input = read_all(STDIN_FILENO, &len);
	if (input && dir) {
		record(dir, input, len);
	}
	char *previous = dir ? previous_command(dir) : NULL;
	int ret = 0;
	if (previous) {
		ret = run_previous(previous, input ? input : "", len);
	} else if (dir) {
		// no status line of its own before: the limits are it
		struct tw_claude_usage u;
		if (tw_claude_usage_read(dir, &u)) {
			time_t now = time(NULL);
			print_window("5h", &u.five_hour, now);
			if (u.five_hour.known && u.seven_day.known) {
				printf(" · ");
			}
			print_window("7d", &u.seven_day, now);
			printf("\n");
		}
		tw_claude_usage_finish(&u);
	}
	free(previous);
	free(input);
	free(dir);
	return ret;
}

/* ---------- connect, disconnect ---------- */

static char *shell_quote(const char *s) {
	size_t extra = 2;
	for (const char *p = s; *p; p++) {
		extra += *p == '\'' ? 3 : 0;
	}
	char *out = malloc(strlen(s) + extra + 1), *o = out;
	*o++ = '\'';
	for (const char *p = s; *p; p++) {
		if (*p == '\'') {
			memcpy(o, "'\\''", 4);
			o += 4;
		} else {
			*o++ = *p;
		}
	}
	*o++ = '\'';
	*o = '\0';
	return out;
}

static char *self_path(void) {
	char buf[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) {
		return strdup("tilewin-claude-usage");
	}
	buf[n] = '\0';
	return strdup(buf);
}

static json_object *read_settings(const char *path, bool *broken) {
	*broken = false;
	if (access(path, F_OK) != 0) {
		return json_object_new_object();
	}
	json_object *root = json_object_from_file(path);
	if (!root || !json_object_is_type(root, json_type_object)) {
		json_object_put(root);
		*broken = true; // never written over: the user's file stays as it is
		return NULL;
	}
	return root;
}

static bool write_settings(const char *path, json_object *root) {
	struct stat st;
	mode_t mode = stat(path, &st) == 0 ? st.st_mode & 0777 : 0644;
	char *text = format_str("%s\n", json_object_to_json_string_ext(root,
		JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE));
	bool ok = write_atomic(path, text, mode);
	free(text);
	return ok;
}

static char *dir_argument(int argc, char **argv) {
	return argc > 0 ? tw_claude_normalize_dir(argv[0]) : tw_claude_default_dir();
}

static int cmd_connect(int argc, char **argv) {
	char *dir = dir_argument(argc, argv);
	struct stat st;
	if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "tilewin-claude-usage: %s is not a Claude Code folder\n",
			dir ? dir : "?");
		free(dir);
		return 1;
	}
	if (tw_claude_connected(dir)) {
		printf("%s is connected already\n", dir);
		free(dir);
		return 0;
	}
	char *path = format_str("%s/settings.json", dir);
	bool broken;
	json_object *root = read_settings(path, &broken);
	if (!root) {
		fprintf(stderr, "tilewin-claude-usage: %s is not JSON tileWin can read; "
			"nothing was changed\n", path);
		free(path);
		free(dir);
		return 1;
	}
	// the status line it had is kept, to run after us and to give back
	json_object *old = NULL;
	json_object_object_get_ex(root, "statusLine", &old);
	char *state = state_path(dir);
	const char *old_text = old ? json_object_to_json_string(old) : "{}";
	if (!write_atomic(state, old_text, 0600)) {
		fprintf(stderr, "tilewin-claude-usage: cannot write %s\n", state);
		free(state);
		json_object_put(root);
		free(path);
		free(dir);
		return 1;
	}
	free(state);
	json_object *line = json_object_new_object();
	if (old && json_object_is_type(old, json_type_object)) {
		json_object_object_foreach(old, key, value) {
			if (strcmp(key, "type") != 0 && strcmp(key, "command") != 0) {
				json_object_object_add(line, key, json_object_get(value)); // padding and such
			}
		}
	}
	char *self = self_path(), *quoted_self = shell_quote(self), *quoted_dir = shell_quote(dir);
	char *command = format_str("%s statusline --dir %s", quoted_self, quoted_dir);
	json_object_object_add(line, "type", json_object_new_string("command"));
	json_object_object_add(line, "command", json_object_new_string(command));
	json_object_object_add(root, "statusLine", line);
	bool ok = write_settings(path, root);
	if (ok) {
		printf("%s: Claude Code shows its usage limits to tileWin from its next answer on\n",
			dir);
	} else {
		fprintf(stderr, "tilewin-claude-usage: cannot write %s\n", path);
	}
	free(command);
	free(quoted_dir);
	free(quoted_self);
	free(self);
	json_object_put(root);
	free(path);
	free(dir);
	return ok ? 0 : 1;
}

static int cmd_disconnect(int argc, char **argv) {
	char *dir = dir_argument(argc, argv);
	if (!dir) {
		return 1;
	}
	char *path = format_str("%s/settings.json", dir);
	char *state = state_path(dir);
	bool broken;
	json_object *root = tw_claude_connected(dir) ? read_settings(path, &broken) : NULL;
	int ret = 0;
	if (root) {
		json_object *old = json_object_from_file(state);
		if (old && json_object_is_type(old, json_type_object) &&
				json_object_object_length(old) > 0) {
			json_object_object_add(root, "statusLine", json_object_get(old));
		} else {
			json_object_object_del(root, "statusLine");
		}
		json_object_put(old);
		if (!write_settings(path, root)) {
			fprintf(stderr, "tilewin-claude-usage: cannot write %s\n", path);
			ret = 1;
		}
		json_object_put(root);
	}
	if (ret == 0) {
		unlink(state);
		char *cache = tw_claude_cache_path(dir);
		unlink(cache);
		free(cache);
		printf("%s: its status line is its own again\n", dir);
	}
	free(state);
	free(path);
	free(dir);
	return ret;
}

static int cmd_list(void) {
	list_t *all = tw_claude_usage_all();
	time_t now = time(NULL);
	for (int i = 0; i < all->length; i++) {
		struct tw_claude_usage *u = all->items[i];
		printf("%s  %s%s\n", u->dir, u->account ? u->account : "(no account)",
			u->connected ? "" : "  (not connected)");
		if (u->updated) {
			printf("    ");
			print_window("5h", &u->five_hour, now);
			printf("   ");
			print_window("7d", &u->seven_day, now);
			printf("   as of %ld minutes ago\n", (long)((now - u->updated) / 60));
		}
	}
	tw_claude_usage_list_free(all);
	return 0;
}

/* The Antigravity status line has its own schema and quota buckets. */
static int agy_pid(void) {
	int pid = getppid();
	for (int n = 0; pid > 1 && n < 12; n++) {
		char path[64], line[1024], name[32], state;
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE *proc = fopen(path, "r");
		bool read_ok = proc && fgets(line, sizeof(line), proc);
		if (proc) {
			fclose(proc);
		}
		if (!read_ok) {
			break;
		}
		if (sscanf(line, "%*d (%31[^)]) %c", name, &state) == 2 && strcmp(name, "agy") == 0) {
			return pid;
		}
		char *end = strrchr(line, ')');
		if (!end || sscanf(end + 1, " %c %d", &state, &pid) != 2) {
			break;
		}
	}
	return 0;
}

static void copy_fields(json_object *out, json_object *source, const char *const *keys) {
	for (; *keys; keys++) {
		json_object *value;
		if (json_object_object_get_ex(source, *keys, &value)) {
			json_object_object_add(out, *keys, json_object_get(value));
		}
	}
}

static json_object *status_field(json_object *o, const char *name) {
	json_object *value = NULL;
	json_object_object_get_ex(o, name, &value);
	return value;
}

static const char *status_text(json_object *o, const char *name) {
	json_object *v = status_field(o, name);
	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}

static bool status_number(json_object *o, const char *name, double *out) {
	json_object *v = status_field(o, name);
	if (!json_object_is_type(v, json_type_int) && !json_object_is_type(v, json_type_double)) {
		return false;
	}
	*out = json_object_get_double(v);
	return isfinite(*out);
}

struct terminal_status {
	int column, width;
	bool color;
};

static struct terminal_status terminal_status(json_object *root) {
	double width;
	return (struct terminal_status){
		.width = status_number(root, "terminal_width", &width) && width >= 30 && width <= 1000 ?
			(int)width : 100,
		.color = !getenv("NO_COLOR"),
	};
}

static void status_segment_begin(struct terminal_status *line, int width) {
	if (line->column && line->column + width + 3 > line->width) {
		putchar('\n');
		line->column = 0;
	}
	if (line->column) {
		fputs(" | ", stdout);
		line->column += 3;
	}
	line->column += width;
}

static void status_write_text(const struct terminal_status *line, const char *text, const char *color) {
	if (line->color && color) {
		fputs(color, stdout);
	}
	for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
		if (*c >= 32 && *c != 127) {
			putchar(*c);
		}
	}
	if (line->color && color) {
		fputs("\033[0m", stdout);
	}
}

static void status_segment(struct terminal_status *line, const char *text, const char *color) {
	status_segment_begin(line, strlen(text));
	status_write_text(line, text, color);
}

static void status_bar(struct terminal_status *line, const char *label, double percent,
		const char *suffix) {
	if (!(percent >= 0 && percent <= 100)) {
		return;
	}
	int filled = (int)((percent + 10) / 20);
	char bar[6], value[128];
	for (int i = 0; i < 5; i++) {
		bar[i] = i < filled ? '=' : '-';
	}
	bar[5] = '\0';
	snprintf(value, sizeof(value), "%s [%s] %.1f%%%s", label, bar, percent, suffix ? suffix : "");
	status_segment(line, value, percent >= 95 ? "\033[91m" : percent >= 80 ? "\033[93m" : "\033[96m");
}

static void status_model(struct terminal_status *line, json_object *root) {
	json_object *model = status_field(root, "model");
	const char *name = status_text(model, "display_name");
	if (!name) {
		name = status_text(model, "id");
	}
	const char *effort = status_text(model, "effort");
	if (!effort) {
		effort = status_text(status_field(root, "effort"), "level");
	}
	if (name && *name) {
		char text[128];
		snprintf(text, sizeof(text), "%s%s%s", name,
			effort && !strcasestr(name, effort) ? " " : "",
			effort && !strcasestr(name, effort) ? effort : "");
		status_segment(line, text, "\033[1m");
	}
}

static int status_label(char *out, size_t size, const char *text, size_t len) {
	// Match Claude's 20-character middle truncation and keep UTF-8 sequences intact.
	size_t count = 0, right = len;
	const char *left = text;
	for (size_t i = 0; i < len; i++) {
		if (((unsigned char)text[i] & 0xc0) == 0x80) {
			continue;
		}
		if (count == 10) {
			left = text + i;
		}
		count++;
	}
	if (count <= 20) {
		snprintf(out, size, "%.*s", (int)len, text);
		return (int)count;
	}
	for (int i = 0; i < 9 && right > 0; i++) {
		do {
			right--;
		} while (right && ((unsigned char)text[right] & 0xc0) == 0x80);
	}
	snprintf(out, size, "%.*s…%.*s", (int)(left - text), text, (int)(len - right), text + right);
	return 20;
}

static bool status_git_read(const char *cwd, const char *command, const char *option,
		char *out, size_t size) {
	int ends[2];
	if (pipe(ends) < 0) {
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(ends[0]);
		if (dup2(ends[1], STDOUT_FILENO) < 0) {
			_exit(126);
		}
		close(ends[1]);
		int quiet = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (quiet < 0 || dup2(quiet, STDIN_FILENO) < 0 || dup2(quiet, STDERR_FILENO) < 0) {
			_exit(126);
		}
		execlp("git", "git", "-C", cwd, command, option, "HEAD", (char *)NULL);
		_exit(127);
	}
	close(ends[1]);
	ssize_t count = -1;
	if (pid > 0) {
		struct pollfd ready = { .fd = ends[0], .events = POLLIN };
		if (poll(&ready, 1, 500) > 0 && (ready.revents & POLLIN)) {
			count = read(ends[0], out, size - 1);
		} else {
			kill(pid, SIGKILL);
		}
	}
	close(ends[0]);
	int status = 0;
	pid_t waited = -1;
	if (pid > 0) {
		do {
			waited = waitpid(pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
	}
	if (count <= 0 || waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return false;
	}
	while (count > 0 && (out[count - 1] == '\n' || out[count - 1] == '\r')) {
		count--;
	}
	out[count] = '\0';
	return count > 0;
}

static void agy_status_workspace(struct terminal_status *line, json_object *root) {
	const char *cwd = status_text(status_field(root, "workspace"), "current_dir");
	if (!cwd || !*cwd) {
		cwd = status_text(root, "cwd");
	}
	if (!cwd || !*cwd) {
		return;
	}
	const char *end = cwd + strlen(cwd);
	while (end > cwd + 1 && end[-1] == '/') {
		end--;
	}
	const char *base = end;
	while (base > cwd && base[-1] != '/') {
		base--;
	}
	if (base == end) {
		base = cwd; // a root directory
	}
	char directory[96], branch_label[96], local_branch[256] = "";
	int width = status_label(directory, sizeof(directory), base, end - base);
	const char *branch = status_text(status_field(root, "worktree"), "branch");
	if (!branch || !*branch) {
		branch = status_text(status_field(root, "vcs"), "branch");
	}
	if ((!branch || !*branch) && cwd[0] == '/') {
		if (status_git_read(cwd, "symbolic-ref", "--short", local_branch, sizeof(local_branch))) {
			branch = local_branch;
		} else if (status_git_read(cwd, "rev-parse", "--short", local_branch + 9, sizeof(local_branch) - 9)) {
			memcpy(local_branch, "detached:", 9);
			branch = local_branch;
		}
	}
	if (branch && *branch) {
		int branch_width = status_label(branch_label, sizeof(branch_label), branch, strlen(branch));
		status_segment_begin(line, width);
		status_write_text(line, directory, "\033[36m");
		if (line->column + branch_width + 3 > line->width) {
			putchar('\n');
			line->column = 0;
		} else {
			putchar(' ');
			line->column++;
		}
		status_write_text(line, "⎇ ", "\033[38;5;220m");
		status_write_text(line, branch_label, "\033[38;5;220m");
		line->column += branch_width + 2;
	} else {
		status_segment_begin(line, width);
		status_write_text(line, directory, "\033[36m");
	}
}

static const char *status_usage_color(double percent) {
	const char *const colors[] = {
		"\033[38;5;46m", "\033[38;5;118m", "\033[38;5;154m", "\033[38;5;226m",
		"\033[38;5;214m", "\033[38;5;202m", "\033[38;5;208m", "\033[38;5;196m",
	};
	return colors[(int)fmin(7, floor(percent / 12.5))];
}

static void status_vertical_bar(const struct terminal_status *line, double percent) {
	const char *const blocks[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
	if (line->color) {
		fputs(status_usage_color(percent), stdout);
	}
	fputs(blocks[(int)fmax(0, ceil(percent / 12.5) - 1)], stdout);
	if (line->color) {
		fputs("\033[0m", stdout);
	}
}

static void status_compact_tokens(char *out, size_t size, double count) {
	if (count >= 1e9) {
		snprintf(out, size, "%.0fG", round(count / 1e9));
	} else if (count >= 1e6) {
		snprintf(out, size, "%.0fM", round(count / 1e6));
	} else if (count >= 1e3) {
		snprintf(out, size, "%.0fk", round(count / 1e3));
	} else {
		snprintf(out, size, "%.0f", round(count));
	}
}

static void agy_status_context(struct terminal_status *line, json_object *root) {
	json_object *context = status_field(root, "context_window");
	double used, limit, percent;
	bool have_used = status_number(context, "total_input_tokens", &used) && used >= 0;
	bool have_limit = status_number(context, "context_window_size", &limit) && limit > 0;
	if (!status_number(context, "used_percentage", &percent)) {
		if (!have_used || !have_limit) {
			return;
		}
		percent = used / limit * 100;
	}
	percent = fmax(0, fmin(100, percent));
	char text[128];
	int width = snprintf(text, sizeof(text), " %.0f%%", round(percent));
	if (have_limit) {
		char input[48], capacity[48];
		status_compact_tokens(input, sizeof(input), have_used ? used : percent / 100 * limit);
		status_compact_tokens(capacity, sizeof(capacity), limit);
		width += snprintf(text + width, sizeof(text) - width, " (%s/%s)", input, capacity);
	}
	status_segment_begin(line, 5 + width); // "ctx " and the one-cell glyph
	fputs("ctx ", stdout);
	status_vertical_bar(line, percent);
	fputs(text, stdout);
}

/* Each model family gets one row. Empty periods occupy their column so
 * weekly-only buckets stay aligned with the other weekly bars. */
static const struct agy_status_group {
	const char *name, *short_name, *five, *week, *color;
} agy_status_groups[] = {
	{ "Gemini", "Gemini", "gemini-5h", "gemini-weekly", "\033[94m" },
	{ "Claude/GPT", "C/GPT", "3p-5h", "3p-weekly", "\033[96m" },
	{ "Argon", "Argon", NULL, "gemini-argon-wtu-weekly", "\033[95m" },
};

struct agy_status_quota {
	double used, balance;
	long reset_in;
	bool is_percent, is_balance;
};

static struct agy_status_quota agy_status_quota(json_object *value) {
	struct agy_status_quota quota = { 0 };
	double number;
	if (!value || json_object_get_boolean(status_field(value, "disabled"))) {
		return quota;
	}
	if (status_number(value, "remaining_fraction", &number) && number >= 0 && number <= 1) {
		quota.is_percent = true;
		quota.used = 100 * (1 - number);
		if (quota.used > 0 && status_number(value, "reset_in_seconds", &number) &&
				number > 0 && number <= TW_CLAUDE_SEVEN_DAYS) {
			quota.reset_in = (long)number;
		}
	} else if (status_number(value, "remaining_amount", &number) && number >= 0) {
		quota.is_balance = true;
		quota.balance = number;
	}
	return quota;
}

static bool agy_status_quota_known(struct agy_status_quota quota) {
	return quota.is_percent || quota.is_balance;
}

static int agy_status_cell(const struct terminal_status *line, const char *period,
		struct agy_status_quota quota, bool full) {
	if (!quota.is_percent) {
		char text[32];
		int width = quota.is_balance ?
			snprintf(text, sizeof(text), "%s %.4g left", period, quota.balance) :
			snprintf(text, sizeof(text), "%s —", period);
		fputs(text, stdout);
		return width - (quota.is_balance ? 0 : 2);
	}
	char value[40];
	int value_width = snprintf(value, sizeof(value), full ? "%.1f%%" : "%.0f%%", quota.used);
	printf("%s ", period);
	status_vertical_bar(line, quota.used);
	printf(" %s", value);
	int width = (int)strlen(period) + value_width + 3;
	if (full && quota.reset_in) {
		char span[24];
		tw_claude_format_span(quota.reset_in, true, span, sizeof(span));
		printf(" ↻ %s", span);
		width += 3 + (int)strlen(span);
	}
	return width;
}

static bool agy_status_known_key(const char *key) {
	for (size_t i = 0; i < sizeof(agy_status_groups) / sizeof(*agy_status_groups); i++) {
		const struct agy_status_group *group = &agy_status_groups[i];
		if ((group->five && strcmp(key, group->five) == 0) || strcmp(key, group->week) == 0) {
			return true;
		}
	}
	return false;
}

static void agy_status_quotas(struct terminal_status *line, json_object *quota) {
	const bool full = line->width >= 64;
	const bool narrow = line->width < 34;
	const int name_width = narrow ? 6 : 10;
	const int cell_width = full ? 23 : 9;
	for (size_t i = 0; i < sizeof(agy_status_groups) / sizeof(*agy_status_groups); i++) {
		const struct agy_status_group *group = &agy_status_groups[i];
		struct agy_status_quota five = agy_status_quota(group->five ?
			status_field(quota, group->five) : NULL);
		struct agy_status_quota week = agy_status_quota(status_field(quota, group->week));
		if (!agy_status_quota_known(five) && !agy_status_quota_known(week)) {
			continue;
		}
		if (line->column) {
			putchar('\n');
			line->column = 0;
		}
		if (line->color) {
			fputs(group->color, stdout);
		}
		printf("%-*s", name_width, narrow ? group->short_name : group->name);
		if (line->color) {
			fputs("\033[0m", stdout);
		}
		fputs("  ", stdout);
		int width = agy_status_cell(line, "5h", five, full);
		printf("%*s | ", width < cell_width ? cell_width - width : 0, "");
		agy_status_cell(line, "7d", week, full);
		putchar('\n');
	}

	// Preserve provider-specific buckets that do not belong to a known family.
	json_object_object_foreach(quota, key, value) {
		if (agy_status_known_key(key)) {
			continue;
		}
		struct agy_status_quota other = agy_status_quota(value);
		if (other.is_percent) {
			status_segment(line, key, NULL);
			line->column += agy_status_cell(line, "", other, full);
		} else if (other.is_balance) {
			char text[120];
			snprintf(text, sizeof(text), "%s %.4g left", key, other.balance);
			status_segment(line, text, NULL);
		}
	}
}

static int cmd_copilot_statusline(void) {
	size_t len;
	char *input = read_all(STDIN_FILENO, &len);
	json_object *root = input ? json_tokener_parse(input) : NULL;
	free(input);
	if (!json_object_is_type(root, json_type_object)) {
		json_object_put(root);
		return 1;
	}
	struct terminal_status line = terminal_status(root);
	status_model(&line, root);
	double context;
	if (status_number(status_field(root, "context_window"), "used_percentage", &context)) {
		status_bar(&line, "Ctx", context, NULL);
	}
	if (line.column) {
		putchar('\n');
	}
	json_object_put(root);
	return 0;
}

#define AGY_QUOTA_MARKER "tileWin quota sampled"

static int cmd_agy_statusline(bool probe) {
	size_t len;
	char *input = read_all(STDIN_FILENO, &len);
	json_object *root = input ? json_tokener_parse(input) : NULL;
	free(input);
	if (!json_object_is_type(root, json_type_object)) {
		json_object_put(root);
		return 1;
	}
	json_object *model = NULL, *context = NULL, *session = NULL;
	json_object_object_get_ex(root, "model", &model);
	json_object_object_get_ex(root, "context_window", &context);
	json_object_object_get_ex(root, "conversation_id", &session);
	const char *id = json_object_is_type(session, json_type_string) ?
		json_object_get_string(session) : "";
	json_object *out = json_object_new_object();
	json_object *quota = NULL, *buckets = json_object_new_object();
	if (json_object_object_get_ex(root, "quota", &quota) &&
			json_object_is_type(quota, json_type_object)) {
		json_object_object_foreach(quota, key, value) {
			json_object *entry = json_object_new_object();
			copy_fields(entry, value, (const char *const[]){
				"remaining_fraction", "reset_time", "disabled", NULL
			});
			json_object_object_add(buckets, key, entry);
		}
	}
	json_object_object_add(out, "quota", buckets);
	json_object *details = json_object_new_object();
	copy_fields(details, model, (const char *const[]){ "id", "effort", NULL });
	json_object_object_add(out, "model", details);
	json_object *totals = json_object_new_object();
	copy_fields(totals, context, (const char *const[]){ "total_input_tokens", "total_output_tokens", NULL });
	json_object_object_add(out, "totals", totals);
	json_object_object_add(out, "pid", json_object_new_int(probe ? 0 : agy_pid()));
	if (!probe) {
		struct terminal_status line = terminal_status(root);
		agy_status_workspace(&line, root);
		status_model(&line, root);
		agy_status_context(&line, root);
		if (json_object_is_type(quota, json_type_object)) {
			agy_status_quotas(&line, quota);
		}
		if (line.column) {
			putchar('\n');
		}
	}
	char *state_dir = tw_state_dir();
	bool account_only = (probe || !*id) && json_object_object_length(buckets) > 0;
	bool recorded = false;
	if (account_only) {
		id = "account";
	}
	if (state_dir && (account_only || (!probe && *id && strlen(id) <= 64 &&
			strspn(id, "0123456789abcdefABCDEF-") == strlen(id)))) {
		char *last_path = format_str("%s/agent-usage/agy/%s.json", state_dir, id);
		char *log_path = format_str("%s/agent-usage/agy/%s.jsonl", state_dir, id);
		char *dir = format_str("%s/agent-usage/agy", state_dir);
		tw_mkdir_p(dir);
		int fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0 && flock(fd, LOCK_EX) == 0) {
			json_object *last = json_object_from_file(last_path);
			if (!json_object_equal(last, out)) {
				json_object *sample = json_object_new_object();
				json_object_object_add(sample, "at", json_object_new_int64(time(NULL)));
				json_object_object_add(sample, "data", json_object_get(out));
				if (dprintf(fd, "%s\n", json_object_to_json_string_ext(sample,
						JSON_C_TO_STRING_PLAIN)) >= 0) {
					recorded = true;
					write_atomic(last_path, json_object_to_json_string_ext(out,
						JSON_C_TO_STRING_PLAIN), 0600);
				}
				json_object_put(sample);
			} else {
				recorded = true;
			}
			json_object_put(last);
		}
		if (fd >= 0) {
			close(fd);
		}
		free(dir);
		free(last_path);
		free(log_path);
	}
	free(state_dir);
	json_object_put(out);
	json_object_put(root);
	if (probe && account_only && recorded) {
		puts(AGY_QUOTA_MARKER);
	}
	return 0;
}

/* Antigravity renders the startup status line only on a terminal. The probe
 * supplies a private terminal and never writes keystrokes or prompts to it. */
static int cmd_agy_quota(const char *program, const char *home) {
	char workspace[] = "/tmp/tilewin-agy-quota.XXXXXX";
	if (!mkdtemp(workspace)) {
		return 1;
	}
	struct winsize size = { .ws_row = 28, .ws_col = 110 };
	int terminal;
	pid_t pid = forkpty(&terminal, NULL, NULL, &size);
	if (pid < 0) {
		rmdir(workspace);
		return 1;
	}
	if (pid == 0) {
		if (chdir(workspace) != 0) {
			_exit(1);
		}
		setenv("TERM", "xterm-256color", 1);
		if (home && *home) {
			setenv("HOME", home, 1); // a portable one: its login is in its own home
		}
		execlp(program && *program ? program : "agy", "agy", "--settings",
			"{\"statusLine\":{\"type\":\"command\","
			"\"command\":\"tilewin-claude-usage agy-statusline --probe\"}}", (char *)NULL);
		_exit(127);
	}
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct pollfd watch = { .fd = terminal, .events = POLLIN };
	char output[4096];
	size_t offset = 0;
	bool sampled = false;
	while (!sampled) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= 8) {
			break;
		}
		int ready = poll(&watch, 1, 250);
		if (ready <= 0) {
			if (ready < 0 && errno != EINTR) {
				break;
			}
			continue;
		}
		ssize_t n = read(terminal, output + offset, sizeof(output) - 1 - offset);
		if (n <= 0) {
			break;
		}
		offset += n;
		output[offset] = '\0';
		sampled = strstr(output, AGY_QUOTA_MARKER) != NULL;
		size_t keep = strlen(AGY_QUOTA_MARKER) - 1;
		if (offset > keep) {
			memmove(output, output + offset - keep, keep);
			offset = keep;
		}
	}
	kill(-pid, SIGTERM);
	kill(pid, SIGTERM);
	close(terminal);
	struct timespec grace = { .tv_nsec = 100000000 };
	nanosleep(&grace, NULL);
	kill(-pid, SIGKILL);
	if (waitpid(pid, NULL, WNOHANG) == 0) {
		kill(pid, SIGKILL);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
		}
	}
	rmdir(workspace);
	return sampled ? 0 : 1;
}

int main(int argc, char **argv) {
	const char *cmd = argc > 1 ? argv[1] : "";
	// what the status lines write is the user's, also of an agent with a $HOME of its own
	tw_paths_use_login_home();
	if (strcmp(cmd, "agy-statusline") == 0) {
		return cmd_agy_statusline(argc == 3 && strcmp(argv[2], "--probe") == 0);
	} else if (strcmp(cmd, "copilot-statusline") == 0) {
		return cmd_copilot_statusline();
	} else if (strcmp(cmd, "agy-quota") == 0) {
		return cmd_agy_quota(argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL);
	} else if (strcmp(cmd, "statusline") == 0) {
		return cmd_statusline(argc - 2, argv + 2);
	} else if (strcmp(cmd, "connect") == 0) {
		return cmd_connect(argc - 2, argv + 2);
	} else if (strcmp(cmd, "disconnect") == 0) {
		return cmd_disconnect(argc - 2, argv + 2);
	} else if (strcmp(cmd, "list") == 0) {
		return cmd_list();
	}
	fprintf(stderr, "usage: tilewin-claude-usage connect [DIR] | disconnect [DIR] | "
		"statusline [--dir DIR] | agy-statusline | agy-quota [PROGRAM [HOME]] | copilot-statusline | list\n"
		"The usage limits of Claude Code for tileWin's Claude widget, as Claude Code shows "
		"them to its status line; DIR is a Claude Code folder, ~/.claude by default.\n");
	return strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ? 0 : 2;
}
