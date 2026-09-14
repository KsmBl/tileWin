#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "tw_paths.h"

/* ---------- panel supervision ---------- */

static struct {
	pid_t pid;
	int pidfd;
	struct wl_event_source *source;
	struct wl_event_source *timer;
	struct timespec started;
	int quick_deaths;
	bool stopping;
	char *command;
} panel = { .pid = -1, .pidfd = -1 };

static bool panel_command_enabled(const char *cmd) {
	return cmd && *cmd && strcmp(cmd, "none") != 0;
}

static int handle_panel_exit(int fd, uint32_t mask, void *data);

static void panel_spawn(void) {
	const char *cmd = config ? config->tw_panel_command : NULL;
	if (!panel_command_enabled(cmd) || panel.pid > 0) {
		return;
	}
	char *shell_cmd = format_str("exec %s", cmd);
	pid_t pid = fork();
	if (pid < 0) {
		sway_log_errno(SWAY_ERROR, "fork failed for panel");
		free(shell_cmd);
		return;
	}
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", shell_cmd, (char *)NULL);
		_exit(127);
	}
	free(shell_cmd);

	panel.pid = pid;
	free(panel.command);
	panel.command = strdup(cmd);
	clock_gettime(CLOCK_MONOTONIC, &panel.started);
	panel.pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (panel.pidfd >= 0) {
		fcntl(panel.pidfd, F_SETFD, FD_CLOEXEC);
		panel.source = wl_event_loop_add_fd(server.wl_event_loop, panel.pidfd,
			WL_EVENT_READABLE, handle_panel_exit, NULL);
	} else {
		sway_log_errno(SWAY_INFO, "pidfd_open failed, panel is not supervised");
	}
	sway_log(SWAY_DEBUG, "Spawned panel '%s' (pid %d)", cmd, pid);
}

static int handle_respawn_timer(void *data) {
	wl_event_source_remove(panel.timer);
	panel.timer = NULL;
	panel_spawn();
	return 0;
}

static int handle_panel_exit(int fd, uint32_t mask, void *data) {
	wl_event_source_remove(panel.source);
	panel.source = NULL;
	close(panel.pidfd);
	panel.pidfd = -1;
	panel.pid = -1;

	if (panel.stopping) {
		panel.stopping = false;
		return 0;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long alive_ms = (now.tv_sec - panel.started.tv_sec) * 1000 +
		(now.tv_nsec - panel.started.tv_nsec) / 1000000;
	panel.quick_deaths = alive_ms < 5000 ? panel.quick_deaths + 1 : 0;
	if (panel.quick_deaths >= 5) {
		sway_log(SWAY_ERROR, "Panel keeps exiting, not restarting it again "
			"(use 'restart panel' after fixing it)");
		return 0;
	}
	int delay = panel.quick_deaths > 0 ? 1000 * panel.quick_deaths : 1;
	panel.timer = wl_event_loop_add_timer(server.wl_event_loop, handle_respawn_timer, NULL);
	if (panel.timer) {
		wl_event_source_timer_update(panel.timer, delay);
	}
	return 0;
}

void tw_panel_start(void) {
	if (panel.pid < 0 && !panel.timer) {
		panel_spawn();
	}
}

void tw_panel_restart(void) {
	panel.quick_deaths = 0;
	if (panel.pid > 0) {
		// the exit handler respawns the panel
		kill(panel.pid, SIGTERM);
		if (panel.pidfd < 0) {
			panel.pid = -1;
			panel_spawn();
		}
	} else {
		panel_spawn();
	}
}

void tw_panel_stop(void) {
	if (panel.timer) {
		wl_event_source_remove(panel.timer);
		panel.timer = NULL;
	}
	if (panel.pid > 0) {
		panel.stopping = panel.pidfd >= 0;
		kill(panel.pid, SIGTERM);
		if (panel.pidfd < 0) {
			panel.pid = -1;
		}
	}
}

void tw_panel_config_reloaded(void) {
	const char *cmd = config ? config->tw_panel_command : NULL;
	bool enabled = panel_command_enabled(cmd);
	if (!enabled) {
		tw_panel_stop();
		return;
	}
	if (panel.pid > 0 && panel.command && strcmp(panel.command, cmd) != 0) {
		tw_panel_restart();
	} else if (!config->reloading && panel.pid < 0 && !panel.timer && root &&
			root->outputs->length > 0) {
		// started later from main once the server runs
	}
}

pid_t tw_panel_pid(void) {
	return panel.pid;
}

/* ---------- session: saved windows, restart and login restore ---------- */

/*
 * The open windows are written to ~/.local/state/tileWin/last-session a
 * moment after they change, so the file is current when the system shuts
 * down. A shutdown terminates the apps and tileWin at the same time, so
 * saving stops when tileWin receives SIGTERM; windows closing during the
 * shutdown are never written. Logging out with the exit command saves once
 * more while the apps are still running.
 *
 * At login the saved apps are started again from their command line and
 * working directory, and each new window is moved back to its workspace and
 * position. Apps restore their own content if they support it.
 */

#define SESSION_SAVE_DELAY_MS 2000
// terminals change directories without window events, so look again now and then
#define SESSION_REFRESH_MS 60000
#define RESTORE_GUARD_SEC 30
#define PLACEMENT_TIMEOUT_SEC 90

static struct {
	struct wl_event_source *timer;
	bool frozen;
	struct timespec guard_until; // restored apps are still starting
	char *saved_text;            // content of last-session as written
	int saved_windows, saved_launches;
} session;

struct placement {
	char *app_id;
	char *workspace;
	struct wlr_box box;
	bool maximized, minimized, floating;
	enum tw_snap snap;
	pid_t pid; // process started for this window, 0 if unknown
	struct timespec expires;
};

static list_t *placements = NULL;

static char *state_file(const char *name) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	tw_mkdir_p(dir);
	char *path = format_str("%s/%s", dir, name);
	free(dir);
	return path;
}

static char *read_cmdline(pid_t pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	while (n > 0 && buf[n - 1] == '\0') {
		n--;
	}
	if (n == 0) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		if (buf[i] == '\0') {
			buf[i] = '\x1f';
		} else if (buf[i] == '\n' || buf[i] == '\t') {
			buf[i] = ' ';
		}
	}
	buf[n] = '\0';
	return strdup(buf);
}

static char *read_cwd(pid_t pid) {
	char link[64], buf[PATH_MAX];
	snprintf(link, sizeof(link), "/proc/%d/cwd", (int)pid);
	ssize_t n = readlink(link, buf, sizeof(buf) - 1);
	if (n <= 0) {
		return NULL;
	}
	buf[n] = '\0';
	return strpbrk(buf, "\t\n") ? NULL : strdup(buf);
}

/* Copies a string for the tab separated file format. */
static char *sanitize(const char *str) {
	char *copy = strdup(str ? str : "");
	for (char *p = copy; *p; p++) {
		if (*p == '\t' || *p == '\n') {
			*p = ' ';
		}
	}
	return copy;
}

struct save_ctx {
	FILE *file;
	list_t *pids;    // pid_t * per launch line, 0 for launches of a single window
	list_t *seen;    // pid_t * per saved window, to count windows of a process
	int windows;
};

/*
 * Apps whose content tileWin restores itself: terminals start again in the
 * working directory of their shell, file managers open the folder shown in
 * their window title.
 */
enum app_kind {
	APP_TERMINAL,
	APP_FILE_MANAGER,
};

struct app_rule {
	const char *process;    // basename of the running program
	enum app_kind kind;
	const char *launcher;   // program to start instead of the saved command line
	const char *dir_option; // terminal option for the directory; ending in '=' joins it
};

static const struct app_rule app_rules[] = {
	{ "xfce4-terminal", APP_TERMINAL, NULL, "--working-directory=" },
	{ "gnome-terminal-server", APP_TERMINAL, "gnome-terminal", "--working-directory=" },
	{ "gnome-terminal", APP_TERMINAL, NULL, "--working-directory=" },
	{ "ptyxis", APP_TERMINAL, NULL, "--working-directory=" },
	{ "konsole", APP_TERMINAL, NULL, "--workdir" },
	{ "kitty", APP_TERMINAL, NULL, "--directory" },
	{ "alacritty", APP_TERMINAL, NULL, "--working-directory" },
	{ "foot", APP_TERMINAL, NULL, "--working-directory=" },
	{ "tilix", APP_TERMINAL, NULL, "--working-directory=" },
	{ "terminator", APP_TERMINAL, NULL, "--working-directory=" },
	{ "qterminal", APP_TERMINAL, NULL, "--workdir" },
	{ "lxterminal", APP_TERMINAL, NULL, "--working-directory=" },
	{ "xterm", APP_TERMINAL, NULL, NULL },
	{ "urxvt", APP_TERMINAL, NULL, NULL },
	{ "st", APP_TERMINAL, NULL, NULL },
	{ "thunar", APP_FILE_MANAGER, NULL, NULL },
	{ "Thunar", APP_FILE_MANAGER, "thunar", NULL },
	{ "dolphin", APP_FILE_MANAGER, NULL, NULL },
	{ "nautilus", APP_FILE_MANAGER, NULL, NULL },
	{ "nemo", APP_FILE_MANAGER, NULL, NULL },
	{ "caja", APP_FILE_MANAGER, NULL, NULL },
	{ "pcmanfm", APP_FILE_MANAGER, NULL, NULL },
	{ "pcmanfm-qt", APP_FILE_MANAGER, NULL, NULL },
};

static const struct app_rule *find_app_rule(const char *cmdline) {
	if (!cmdline) {
		return NULL;
	}
	size_t len = strcspn(cmdline, "\x1f");
	char *arg0 = strndup(cmdline, len);
	const char *base = strrchr(arg0, '/') ? strrchr(arg0, '/') + 1 : arg0;
	const struct app_rule *found = NULL;
	for (size_t i = 0; !found && i < sizeof(app_rules) / sizeof(app_rules[0]); i++) {
		if (strcmp(base, app_rules[i].process) == 0) {
			found = &app_rules[i];
		}
	}
	free(arg0);
	return found;
}

static int count_windows(struct save_ctx *ctx, pid_t pid) {
	int count = 0;
	for (int i = 0; i < ctx->seen->length; i++) {
		count += *(pid_t *)ctx->seen->items[i] == pid;
	}
	pid_t *entry = malloc(sizeof(pid_t));
	*entry = pid;
	list_add(ctx->seen, entry);
	return count;
}

struct child_proc {
	pid_t pid;
	unsigned long long start;
	bool tty; // has a controlling terminal: the shell, not a helper process
};

static int child_cmp(const void *a, const void *b) {
	const struct child_proc *ca = a, *cb = b;
	return ca->start < cb->start ? -1 : ca->start > cb->start;
}

/* Working directory of the index-th child (by start time) of a process. */
static char *child_cwd(pid_t parent, int index) {
	DIR *dir = opendir("/proc");
	if (!dir) {
		return NULL;
	}
	struct child_proc children[64];
	int count = 0;
	struct dirent *de;
	while ((de = readdir(dir)) && count < 64) {
		if (!isdigit((unsigned char)de->d_name[0])) {
			continue;
		}
		char path[300], buf[1024];
		snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = '\0';
		char *p = strrchr(buf, ')');
		if (!p) {
			continue;
		}
		// fields after the command name: state ppid ... starttime is the 20th
		int field = 0;
		pid_t ppid = 0;
		long tty = 0;
		unsigned long long start = 0;
		char *save = NULL;
		for (char *tok = strtok_r(p + 1, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
			if (field == 1) {
				ppid = atoi(tok);
			} else if (field == 4) {
				tty = atol(tok);
			} else if (field == 19) {
				start = strtoull(tok, NULL, 10);
				break;
			}
			field++;
		}
		if (ppid == parent) {
			children[count].pid = atoi(de->d_name);
			children[count].start = start;
			children[count].tty = tty != 0;
			count++;
		}
	}
	closedir(dir);
	// shells run on a terminal; helpers such as utempter do not
	int shells = 0;
	for (int i = 0; i < count; i++) {
		if (children[i].tty) {
			children[shells++] = children[i];
		}
	}
	if (shells > 0) {
		count = shells;
	}
	if (count == 0) {
		return NULL;
	}
	qsort(children, count, sizeof(children[0]), child_cmp);
	return read_cwd(children[index < count ? index : count - 1].pid);
}

/* Folder of a file manager window from titles like "/home/me/Music - Thunar". */
static char *title_directory(const char *title) {
	if (!title || !*title) {
		return NULL;
	}
	char *copy = strdup(title);
	static const char *const separators[] = { " - ", " \xe2\x80\x94 ", " \xe2\x80\x93 " };
	char *cut = NULL;
	for (size_t i = 0; i < sizeof(separators) / sizeof(separators[0]); i++) {
		for (char *pos = strstr(copy, separators[i]); pos; pos = strstr(pos + 1, separators[i])) {
			if (!cut || pos > cut) {
				cut = pos;
			}
		}
	}
	if (cut) {
		*cut = '\0';
	}
	char *path = NULL;
	if (copy[0] == '/' || copy[0] == '~') {
		path = tw_expand_home(copy);
	} else if (getenv("HOME") && !strchr(copy, '/')) {
		path = format_str("%s/%s", getenv("HOME"), copy); // e.g. "Downloads"
	}
	struct stat st;
	if (path && (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || strpbrk(path, "\t\n\x1f"))) {
		free(path);
		path = NULL;
	}
	free(copy);
	return path;
}

/*
 * File managers usually show only the folder name; switch them to full paths
 * in the title (once per session) so every folder can be restored.
 */
static void spawn_shell(const char *script);

static void prepare_file_manager(const struct app_rule *rule) {
	static bool thunar_done, dolphin_done, nemo_done;
	if (getenv("TILEWIN_NO_APP_TWEAKS")) {
		return;
	}
	const char *name = rule->launcher ? rule->launcher : rule->process;
	if (strcmp(name, "thunar") == 0 && !thunar_done) {
		thunar_done = true;
		spawn_shell("command -v xfconf-query >/dev/null && "
			"! xfconf-query -c thunar -p /misc-window-title-style >/dev/null 2>&1 && "
			"xfconf-query -c thunar -p /misc-window-title-style -n -t string "
			"-s THUNAR_WINDOW_TITLE_STYLE_FULL_PATH_WITH_THUNAR_SUFFIX");
	} else if (strcmp(name, "dolphin") == 0 && !dolphin_done) {
		dolphin_done = true;
		spawn_shell("r=$(command -v kreadconfig6 || command -v kreadconfig5) && "
			"w=$(command -v kwriteconfig6 || command -v kwriteconfig5) && "
			"[ -z \"$($r --file dolphinrc --group General --key ShowFullPathInTitlebar)\" ] && "
			"$w --file dolphinrc --group General --key ShowFullPathInTitlebar true");
	} else if (strcmp(name, "nemo") == 0 && !nemo_done) {
		nemo_done = true;
		spawn_shell("gsettings set org.nemo.preferences show-full-path-titles true 2>/dev/null");
	}
}

/* Removes an existing directory option from a saved command line. */
static char *strip_dir_option(const char *cmdline, const char *option) {
	if (!option) {
		return strdup(cmdline);
	}
	size_t option_len = strlen(option);
	bool joined = option[option_len - 1] == '=';
	char *copy = strdup(cmdline);
	char *out = calloc(1, strlen(cmdline) + 1);
	bool skip_next = false;
	char *save = NULL;
	for (char *arg = strtok_r(copy, "\x1f", &save); arg; arg = strtok_r(NULL, "\x1f", &save)) {
		if (skip_next) {
			skip_next = false;
			continue;
		}
		if (joined ? strncmp(arg, option, option_len) == 0 : strcmp(arg, option) == 0) {
			skip_next = !joined;
			continue;
		}
		if (*out) {
			strcat(out, "\x1f");
		}
		strcat(out, arg);
	}
	free(copy);
	return out;
}

/* Command line (fields separated by 0x1f) that restores the content of a window. */
static char *content_command(struct save_ctx *ctx, struct sway_container *con,
		const char *cmdline, char **cwd) {
	const struct app_rule *rule = find_app_rule(cmdline);
	if (!rule) {
		return NULL;
	}
	pid_t pid = con->view->pid;
	int nth = count_windows(ctx, pid);
	char *dir;
	if (rule->kind == APP_TERMINAL) {
		dir = child_cwd(pid, nth);
	} else {
		const char *title = view_get_title(con->view);
		dir = title_directory(title);
		if (!title || title[0] != '/') {
			prepare_file_manager(rule);
		}
	}
	if (!dir) {
		return NULL;
	}
	char *command;
	if (rule->kind == APP_FILE_MANAGER) {
		command = format_str("%s\x1f%s", rule->launcher ? rule->launcher : rule->process, dir);
	} else {
		char *base = strip_dir_option(rule->launcher ? rule->launcher : cmdline,
			rule->dir_option);
		size_t len = rule->dir_option ? strlen(rule->dir_option) : 0;
		// the option goes right after the program: options such as -x or -e
		// take the rest of the command line as the command to run
		size_t program_len = strcspn(base, "\x1f");
		const char *rest = base + program_len;
		if (!rule->dir_option) {
			command = strdup(base);
		} else if (rule->dir_option[len - 1] == '=') {
			command = format_str("%.*s\x1f%s%s%s", (int)program_len, base, rule->dir_option,
				dir, rest);
		} else {
			command = format_str("%.*s\x1f%s\x1f%s%s", (int)program_len, base,
				rule->dir_option, dir, rest);
		}
		free(base);
	}
	*cwd = dir;
	return command;
}

static void save_container(struct sway_container *con, void *data) {
	struct save_ctx *ctx = data;
	if (!con->view || con->view->pid <= 1 || con->view->pid == getpid() ||
			con->view->pid == tw_panel_pid()) {
		return;
	}
	const char *app_id = view_get_app_id(con->view);
	if (!app_id) {
		app_id = view_get_class(con->view);
	}
	bool floating = container_is_floating(con);
	struct wlr_box box = { 0 };
	if (floating) {
		box = con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE ?
			con->tw.restore_box : (struct wlr_box){ con->pending.x, con->pending.y,
			con->pending.width, con->pending.height };
	}
	int process = -1;
	char *cmdline = read_cmdline(con->view->pid);
	char *content_cwd = NULL;
	char *content = cmdline ? content_command(ctx, con, cmdline, &content_cwd) : NULL;
	if (content) {
		// a launch per window that brings back its folder or directory
		pid_t *none = calloc(1, sizeof(pid_t));
		list_add(ctx->pids, none);
		process = ctx->pids->length - 1;
		fprintf(ctx->file, "launch\t%s\t%s\n", content_cwd, content);
		free(content);
		free(content_cwd);
	} else {
		// one launch per process: apps with several windows restore them themselves
		for (int i = 0; i < ctx->pids->length; i++) {
			if (*(pid_t *)ctx->pids->items[i] == con->view->pid) {
				process = i;
			}
		}
		if (process < 0 && cmdline) {
			pid_t *pid = malloc(sizeof(pid_t));
			*pid = con->view->pid;
			list_add(ctx->pids, pid);
			process = ctx->pids->length - 1;
			char *cwd = read_cwd(con->view->pid);
			fprintf(ctx->file, "launch\t%s\t%s\n", cwd ? cwd : "", cmdline);
			free(cwd);
		}
	}
	free(cmdline);

	char *workspace = sanitize(con->pending.workspace ? con->pending.workspace->name : "");
	char *id = sanitize(app_id);
	fprintf(ctx->file, "place\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n", workspace,
		box.x, box.y, box.width, box.height, con->pending.tw_maximized ? 1 : 0,
		con->pending.tw_minimized ? 1 : 0, floating ? 1 : 0, floating ? (int)con->tw.snap : 0,
		process, id);
	free(workspace);
	free(id);
	ctx->windows++;
}

static char *session_text(int *windows, int *launches) {
	char *text = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&text, &size);
	if (!f) {
		return NULL;
	}
	fprintf(f, "# tileWin session\n");
	struct save_ctx ctx = { .file = f, .pids = create_list(), .seen = create_list() };
	root_for_each_container(save_container, &ctx);
	*windows = ctx.windows;
	*launches = ctx.pids->length;
	list_free_items_and_destroy(ctx.pids);
	list_free_items_and_destroy(ctx.seen);
	fclose(f);
	return text;
}

/*
 * Writes last-session if its content changed. With keep_complete, nothing is
 * written when windows or apps are missing compared to the saved session,
 * because they are probably being terminated by a shutdown.
 */
static bool session_write(bool keep_complete) {
	int windows = 0, launches = 0;
	char *text = session_text(&windows, &launches);
	if (!text) {
		return false;
	}
	if (keep_complete && (windows < session.saved_windows || launches < session.saved_launches)) {
		sway_log(SWAY_DEBUG, "Not saving an incomplete session (%d of %d windows)", windows,
			session.saved_windows);
		free(text);
		return false;
	}
	if (session.saved_text && strcmp(text, session.saved_text) == 0) {
		free(text);
		return true;
	}
	char *path = state_file("last-session");
	bool ok = path && tw_write_string(path, text);
	if (ok) {
		free(session.saved_text);
		session.saved_text = text;
		session.saved_windows = windows;
		session.saved_launches = launches;
		sway_log(SWAY_DEBUG, "Saved session with %d windows", windows);
	} else {
		sway_log(SWAY_ERROR, "Could not save the session to %s", path ? path : "(no state dir)");
		free(text);
	}
	free(path);
	return ok;
}

static int handle_save_timer(void *data) {
	if (session.frozen) {
		return 0;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec < session.guard_until.tv_sec) {
		// don't replace the saved session while its apps are still starting
		wl_event_source_timer_update(session.timer,
			(session.guard_until.tv_sec - now.tv_sec) * 1000);
		return 0;
	}
	session_write(false);
	wl_event_source_timer_update(session.timer, SESSION_REFRESH_MS);
	return 0;
}

void tw_session_changed(void) {
	if (session.frozen || !server.wl_event_loop) {
		return;
	}
	if (!session.timer) {
		session.timer = wl_event_loop_add_timer(server.wl_event_loop, handle_save_timer, NULL);
	}
	if (session.timer) {
		wl_event_source_timer_update(session.timer, SESSION_SAVE_DELAY_MS);
	}
}

void tw_session_freeze(void) {
	session.frozen = true;
	if (session.timer) {
		wl_event_source_timer_update(session.timer, 0);
	}
}

void tw_session_save_now(void) {
	if (session.frozen) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec >= session.guard_until.tv_sec) {
		session_write(false);
	}
	// the apps are about to exit together with tileWin
	tw_session_freeze();
}

void tw_session_shutdown(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!session.frozen && now.tv_sec >= session.guard_until.tv_sec) {
		// catch directory changes since the last save, unless the shutdown has
		// already taken windows away
		session_write(true);
	}
	tw_session_freeze();
}

bool tw_restart(bool relaunch_apps, char **error) {
	char *marker = state_file("restart");
	if (!marker || !tw_write_string(marker, relaunch_apps ? "relaunch\n" : "plain\n")) {
		*error = format_str("Cannot write %s", marker ? marker : "the state directory");
		free(marker);
		return false;
	}
	free(marker);
	tw_session_save_now();
	sway_log(SWAY_INFO, "Restarting tileWin%s", relaunch_apps ? " (relaunching apps)" : "");
	tw_panel_stop();
	sway_terminate(TW_RESTART_EXIT_CODE);
	return true;
}

static void spawn_shell(const char *script) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", script, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

void tw_session_export_environment(void) {
	if (getenv("TILEWIN_NO_APP_TWEAKS")) {
		return; // nested or test instances must not take over the user session
	}
	// D-Bus and systemd start portals and apps such as Thunar as services;
	// without the display in their environment they cannot open windows and
	// the settings portal cannot tell apps about the color scheme
	spawn_shell("command -v dbus-update-activation-environment >/dev/null || exit 0; "
		"dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY "
		"XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE TILEWINSOCK SWAYSOCK I3SOCK; "
		"command -v systemctl >/dev/null || exit 0; "
		"systemctl --user reset-failed xdg-desktop-portal-gtk.service xdg-desktop-portal.service "
		"thunar.service 2>/dev/null; "
		"systemctl --user try-restart xdg-desktop-portal.service thunar.service 2>/dev/null");
}

/* Starts an app detached from tileWin and returns its pid (0 on failure). */
static pid_t spawn_app(const char *cwd, char *cmdline) {
	int argc = 0;
	char **argv = calloc(1, sizeof(char *));
	char *save = NULL;
	for (char *arg = strtok_r(cmdline, "\x1f", &save); arg;
			arg = strtok_r(NULL, "\x1f", &save)) {
		argv = realloc(argv, (argc + 2) * sizeof(char *));
		argv[argc++] = arg;
	}
	argv[argc] = NULL;
	int fds[2];
	if (argc == 0 || pipe2(fds, O_CLOEXEC) != 0) {
		free(argv);
		return 0;
	}
	pid_t app = 0;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		// double fork so the app is reparented and never becomes a zombie
		pid_t child = fork();
		if (child == 0) {
			if (!cwd || !*cwd || chdir(cwd) != 0) {
				const char *home = getenv("HOME");
				if (home && chdir(home) != 0) {
					// start in the current directory
				}
			}
			execvp(argv[0], argv);
			_exit(127);
		}
		ssize_t written = write(fds[1], &child, sizeof(child));
		_exit(written == sizeof(child) ? 0 : 1);
	} else if (pid > 0) {
		close(fds[1]);
		if (read(fds[0], &app, sizeof(app)) != sizeof(app) || app < 0) {
			app = 0;
		}
		waitpid(pid, NULL, 0);
	} else {
		close(fds[1]);
	}
	close(fds[0]);
	free(argv);
	return app;
}

/* Splits at tabs, keeping empty fields. Returns the number of fields. */
static int split_tabs(char *line, char **fields, int max) {
	int count = 0;
	char *field;
	while (count < max && (field = strsep(&line, "\t"))) {
		fields[count++] = field;
	}
	return count;
}

void tw_session_restore(void) {
	char *marker_path = state_file("restart");
	char *marker = marker_path ? tw_read_first_line(marker_path) : NULL;
	if (marker_path) {
		unlink(marker_path);
	}
	free(marker_path);
	bool restore = marker ? strcmp(marker, "relaunch") == 0 : config->tw_session_restore;
	free(marker);
	if (!restore) {
		return;
	}

	char *path = state_file("last-session");
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	if (!f) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	placements = create_list();
	list_t *pids = create_list(); // pid_t * per launch line
	int launched = 0;
	char *line = NULL;
	size_t size = 0;
	ssize_t n;
	while ((n = getline(&line, &size, f)) > 0) {
		if (line[n - 1] == '\n') {
			line[n - 1] = '\0';
		}
		char *fields[12] = { 0 };
		int count = split_tabs(line, fields, 12);
		if (count == 12 && strcmp(fields[0], "place") == 0) {
			struct placement *p = calloc(1, sizeof(*p));
			p->workspace = strdup(fields[1]);
			p->box = (struct wlr_box){ atoi(fields[2]), atoi(fields[3]),
				atoi(fields[4]), atoi(fields[5]) };
			p->maximized = atoi(fields[6]) != 0;
			p->minimized = atoi(fields[7]) != 0;
			p->floating = atoi(fields[8]) != 0;
			p->snap = (enum tw_snap)atoi(fields[9]);
			int process = atoi(fields[10]);
			if (process >= 0 && process < pids->length) {
				p->pid = *(pid_t *)pids->items[process];
			}
			p->app_id = strdup(fields[11]);
			p->expires = now;
			p->expires.tv_sec += PLACEMENT_TIMEOUT_SEC;
			list_add(placements, p);
		} else if (count >= 3 && strcmp(fields[0], "launch") == 0) {
			pid_t *pid = malloc(sizeof(pid_t));
			*pid = spawn_app(fields[1], fields[2]);
			list_add(pids, pid);
			launched++;
		}
	}
	free(line);
	fclose(f);
	list_free_items_and_destroy(pids);
	if (launched > 0) {
		sway_log(SWAY_INFO, "Restoring %d apps of the last session", launched);
		session.guard_until = now;
		session.guard_until.tv_sec += RESTORE_GUARD_SEC;
	}
}

static void placement_free(struct placement *p) {
	free(p->app_id);
	free(p->workspace);
	free(p);
}

void tw_session_apply_placement(struct sway_container *con) {
	if (!placements || !con->view) {
		return;
	}
	const char *app_id = view_get_app_id(con->view);
	if (!app_id) {
		app_id = view_get_class(con->view);
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct placement *found = NULL, *by_app = NULL;
	for (int i = placements->length - 1; i >= 0; i--) {
		struct placement *p = placements->items[i];
		if (now.tv_sec > p->expires.tv_sec) {
			placement_free(p);
			list_del(placements, i);
		} else if (p->pid > 0 && p->pid == con->view->pid) {
			found = p; // the earliest entry of this process wins
		} else if (app_id && strcmp(p->app_id, app_id) == 0 &&
				(p->pid <= 0 || kill(p->pid, 0) != 0)) {
			// only when the started process is gone and cannot map it itself
			by_app = p;
		}
	}
	if (!found) {
		// apps that hand over to another process (wrappers, single-instance apps)
		found = by_app;
	}
	if (found) {
		list_del(placements, list_find(placements, found));
		if (found->workspace[0] && !strchr(found->workspace, '"') &&
				(!con->pending.workspace ||
				strcmp(con->pending.workspace->name, found->workspace) != 0)) {
			char *cmd = format_str("move container to workspace \"%s\"", found->workspace);
			list_t *results = execute_command(cmd, NULL, con);
			while (results && results->length > 0) {
				free_cmd_results(results->items[0]);
				list_del(results, 0);
			}
			list_free(results);
			free(cmd);
		}
		if (container_is_floating(con)) {
			if (found->floating && found->box.width > 0 && found->box.height > 0) {
				tw_set_box(con, &found->box);
			}
			if (found->maximized) {
				tw_maximize(con, true);
			} else if (found->snap != TW_SNAP_NONE) {
				tw_snap_to(con, found->snap);
			}
			if (found->minimized) {
				tw_minimize(con, true);
			}
		}
		placement_free(found);
	}
	if (placements->length == 0) {
		list_free(placements);
		placements = NULL;
	}
}
