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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void copy_window(json_object *limits, const char *key, json_object *out) {
	json_object *window, *used, *resets;
	if (!limits || !json_object_object_get_ex(limits, key, &window) ||
			!json_object_object_get_ex(window, "used_percentage", &used)) {
		return; // not shown this time: what was written before stays
	}
	json_object *copy = json_object_new_object();
	json_object_object_add(copy, "used_percentage",
		json_object_new_double(json_object_get_double(used)));
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
	json_object_object_add(out, "updated", json_object_new_int64((int64_t)time(NULL)));
	copy_window(limits, "five_hour", out);
	copy_window(limits, "seven_day", out);
	write_atomic(path, json_object_to_json_string_ext(out, JSON_C_TO_STRING_PRETTY), 0600);
	free(path);
	json_object_put(out);
	json_object_put(root);
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

int main(int argc, char **argv) {
	const char *cmd = argc > 1 ? argv[1] : "";
	if (strcmp(cmd, "statusline") == 0) {
		return cmd_statusline(argc - 2, argv + 2);
	} else if (strcmp(cmd, "connect") == 0) {
		return cmd_connect(argc - 2, argv + 2);
	} else if (strcmp(cmd, "disconnect") == 0) {
		return cmd_disconnect(argc - 2, argv + 2);
	} else if (strcmp(cmd, "list") == 0) {
		return cmd_list();
	}
	fprintf(stderr, "usage: tilewin-claude-usage connect [DIR] | disconnect [DIR] | "
		"statusline [--dir DIR] | list\n"
		"The usage limits of Claude Code for tileWin's Claude widget, as Claude Code shows "
		"them to its status line; DIR is a Claude Code folder, ~/.claude by default.\n");
	return strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ? 0 : 2;
}
