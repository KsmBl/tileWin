#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
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

/* ---------- restart ---------- */

static char *session_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	tw_mkdir_p(dir);
	char *path = format_str("%s/session", dir);
	free(dir);
	return path;
}

struct save_ctx {
	FILE *file;
	list_t *pids;
};

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
	if (n == 0) {
		return NULL;
	}
	while (n > 0 && buf[n - 1] == '\0') {
		n--;
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

static void save_container(struct sway_container *con, void *data) {
	struct save_ctx *ctx = data;
	if (!con->view || con->view->pid <= 1 || con->view->pid == getpid() ||
			con->view->pid == tw_panel_pid()) {
		return;
	}
	for (int i = 0; i < ctx->pids->length; i++) {
		if (*(pid_t *)ctx->pids->items[i] == con->view->pid) {
			return;
		}
	}
	char *cmdline = read_cmdline(con->view->pid);
	if (!cmdline) {
		return;
	}
	pid_t *pid = malloc(sizeof(pid_t));
	*pid = con->view->pid;
	list_add(ctx->pids, pid);

	const char *app_id = view_get_app_id(con->view);
	if (!app_id) {
		app_id = view_get_class(con->view);
	}
	struct wlr_box box = con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE ?
		con->tw.restore_box : (struct wlr_box){ con->pending.x, con->pending.y,
		con->pending.width, con->pending.height };
	fprintf(ctx->file, "app\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
		con->pending.workspace ? con->pending.workspace->name : "",
		box.x, box.y, box.width, box.height, con->pending.tw_maximized ? 1 : 0,
		app_id ? app_id : "", cmdline);
	free(cmdline);
}

bool tw_restart(bool relaunch_apps, char **error) {
	char *path = session_path();
	if (relaunch_apps && path) {
		FILE *f = fopen(path, "w");
		if (!f) {
			*error = format_str("Cannot write %s", path);
			free(path);
			return false;
		}
		struct save_ctx ctx = { .file = f, .pids = create_list() };
		root_for_each_container(save_container, &ctx);
		list_free_items_and_destroy(ctx.pids);
		fclose(f);
	} else if (path) {
		unlink(path);
	}
	free(path);
	sway_log(SWAY_INFO, "Restarting tileWin%s", relaunch_apps ? " (relaunching apps)" : "");
	tw_panel_stop();
	sway_terminate(TW_RESTART_EXIT_CODE);
	return true;
}

struct placement {
	char *app_id;
	struct wlr_box box;
	bool maximized;
	struct timespec expires;
};

static list_t *placements = NULL;

static void spawn_argv(char *cmdline) {
	int argc = 0;
	char **argv = calloc(1, sizeof(char *));
	char *save = NULL;
	for (char *arg = strtok_r(cmdline, "\x1f", &save); arg;
			arg = strtok_r(NULL, "\x1f", &save)) {
		argv = realloc(argv, (argc + 2) * sizeof(char *));
		argv[argc++] = arg;
	}
	argv[argc] = NULL;
	if (argc == 0) {
		free(argv);
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		execvp(argv[0], argv);
		_exit(127);
	}
	free(argv);
}

void tw_session_restore(void) {
	char *path = session_path();
	if (!path) {
		return;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		free(path);
		return;
	}
	unlink(path);
	free(path);

	placements = create_list();
	char *line = NULL;
	size_t size = 0;
	ssize_t n;
	while ((n = getline(&line, &size, f)) > 0) {
		if (line[n - 1] == '\n') {
			line[n - 1] = '\0';
		}
		char *fields[9] = { 0 };
		int count = 0;
		char *save = NULL;
		for (char *tok = strtok_r(line, "\t", &save); tok && count < 9;
				tok = strtok_r(NULL, "\t", &save)) {
			fields[count++] = tok;
		}
		if (count < 9 || strcmp(fields[0], "app") != 0) {
			continue;
		}
		struct placement *p = calloc(1, sizeof(*p));
		p->box = (struct wlr_box){ atoi(fields[2]), atoi(fields[3]),
			atoi(fields[4]), atoi(fields[5]) };
		p->maximized = atoi(fields[6]) != 0;
		p->app_id = strdup(fields[7]);
		clock_gettime(CLOCK_MONOTONIC, &p->expires);
		p->expires.tv_sec += 60;
		list_add(placements, p);
		spawn_argv(fields[8]);
	}
	free(line);
	fclose(f);
}

void tw_session_apply_placement(struct sway_container *con) {
	if (!placements || !con->view || !container_is_floating(con)) {
		return;
	}
	const char *app_id = view_get_app_id(con->view);
	if (!app_id) {
		app_id = view_get_class(con->view);
	}
	if (!app_id) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	for (int i = 0; i < placements->length; i++) {
		struct placement *p = placements->items[i];
		if (now.tv_sec > p->expires.tv_sec || strcmp(p->app_id, app_id) != 0) {
			continue;
		}
		if (p->box.width > 0 && p->box.height > 0) {
			tw_set_box(con, &p->box);
		}
		if (p->maximized) {
			tw_maximize(con, true);
		}
		free(p->app_id);
		free(p);
		list_del(placements, i);
		break;
	}
}
