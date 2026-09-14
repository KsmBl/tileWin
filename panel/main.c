#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include "ipc-client.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "tw_paths.h"

static struct panel panel;
static volatile sig_atomic_t signal_flags = 0;

#define SIGNAL_STOP 1
#define SIGNAL_RELOAD 2

static void handle_signal(int sig) {
	if (sig == SIGUSR1) {
		signal_flags |= SIGNAL_RELOAD;
	} else {
		signal_flags |= SIGNAL_STOP;
	}
}

enum pstyle panel_style(struct panel *p) {
	const char *style = tw_theme_str(p->theme, "panel.style", NULL);
	if (!style) {
		style = p->theme && p->theme->style ? p->theme->style : "win10";
	}
	if (strcmp(style, "classic") == 0 || strcmp(style, "win95") == 0) {
		return PS_CLASSIC;
	} else if (strcmp(style, "luna") == 0 || strcmp(style, "winxp") == 0) {
		return PS_LUNA;
	} else if (strcmp(style, "aero") == 0 || strcmp(style, "win7") == 0) {
		return PS_AERO;
	} else if (strcmp(style, "fluent") == 0 || strcmp(style, "win11") == 0) {
		return PS_FLUENT;
	}
	return PS_FLAT;
}

void panel_set_dirty(struct panel *p) {
	struct panel_output *output;
	wl_list_for_each(output, &p->outputs, link) {
		if (output->bar) {
			psurface_set_dirty(output->bar);
		}
	}
}

struct panel_output *panel_focused_output(struct panel *p) {
	struct panel_output *output, *first = NULL;
	wl_list_for_each(output, &p->outputs, link) {
		if (!output->ready) {
			continue;
		}
		if (!first) {
			first = output;
		}
		if (p->state.focused_output && output->name &&
				strcmp(output->name, p->state.focused_output) == 0) {
			return output;
		}
	}
	return first;
}

struct panel_seat *panel_first_seat(struct panel *p) {
	if (wl_list_empty(&p->seats)) {
		return NULL;
	}
	struct panel_seat *seat = wl_container_of(p->seats.next, seat, link);
	return seat;
}

static void load_theme(struct panel *p) {
	char *name = tw_theme_current_name();
	char *error = NULL;
	struct tw_theme *theme = tw_theme_load(name, &error);
	if (!theme) {
		sway_log(SWAY_ERROR, "Failed to load theme %s: %s", name, error ? error : "?");
		free(error);
		error = NULL;
		theme = tw_theme_load(TW_DEFAULT_THEME, &error);
		free(error);
	}
	free(name);
	if (!theme) {
		theme = calloc(1, sizeof(*theme));
		theme->kv = create_list();
		theme->name = strdup(TW_DEFAULT_THEME);
		theme->title = strdup("built-in");
		theme->style = strdup("win10");
	}
	tw_theme_free(p->theme);
	p->theme = theme;
}

static void watch_files(struct panel *p) {
	if (p->inotify_fd < 0) {
		return;
	}
	if (p->config_watch >= 0) {
		inotify_rm_watch(p->inotify_fd, p->config_watch);
		p->config_watch = -1;
	}
	if (p->theme_watch >= 0) {
		inotify_rm_watch(p->inotify_fd, p->theme_watch);
		p->theme_watch = -1;
	}
	if (p->desktop_watch >= 0) {
		inotify_rm_watch(p->inotify_fd, p->desktop_watch);
		p->desktop_watch = -1;
	}
	if (p->config && p->config->desktop_icons) {
		char *desktop = desktop_directory();
		tw_mkdir_p(desktop);
		p->desktop_watch = inotify_add_watch(p->inotify_fd, desktop, IN_CREATE | IN_DELETE |
			IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB);
		free(desktop);
	}
	// watch the directories so editors that replace files are handled
	char *dir = tw_config_dir();
	if (dir) {
		tw_mkdir_p(dir);
		p->config_watch = inotify_add_watch(p->inotify_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
		free(dir);
	}
	if (p->config_path) {
		char *copy = strdup(p->config_path);
		char *slash = strrchr(copy, '/');
		if (slash) {
			*slash = '\0';
			char *cfg = tw_config_dir();
			if (!cfg || strcmp(cfg, copy) != 0) {
				p->theme_watch = inotify_add_watch(p->inotify_fd, copy,
					IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
			}
			free(cfg);
		}
		free(copy);
	}
}

static void do_reload(void *data) {
	struct panel *p = data;
	p->reload_timer = NULL;
	sway_log(SWAY_DEBUG, "Reloading panel config and theme");
	popup_close_all(p);
	tooltip_cancel(p);
	load_theme(p);
	struct panel_config *config = panel_config_load(p, p->config_path);
	if (!config) {
		sway_log(SWAY_ERROR, "Keeping the previous taskbar config");
	} else {
		// bars reference widgets of the old config: destroy them first
		struct panel_output *output;
		wl_list_for_each(output, &p->outputs, link) {
			bar_destroy(output);
		}
		panel_config_free(p->config);
		p->config = config;
	}
	panel_outputs_update_bars(p);
	watch_files(p);
}

void panel_request_reload(struct panel *p) {
	if (!p->reload_timer) {
		p->reload_timer = loop_add_timer(p->loop, 150, do_reload, p);
	}
}

static void inotify_in(int fd, short mask, void *data) {
	struct panel *p = data;
	char buf[4096];
	ssize_t len = read(fd, buf, sizeof(buf));
	bool relevant = false, desktop = false;
	for (char *ptr = buf; len > 0 && ptr < buf + len;) {
		struct inotify_event *ev = (struct inotify_event *)ptr;
		if (p->desktop_watch >= 0 && ev->wd == p->desktop_watch) {
			desktop = true;
		} else if (ev->len > 0 && (strcmp(ev->name, "taskbar.conf") == 0 ||
				strcmp(ev->name, "current-theme") == 0 ||
				(p->config_path && strstr(p->config_path, ev->name)))) {
			relevant = true;
		}
		ptr += sizeof(struct inotify_event) + ev->len;
	}
	if (relevant) {
		panel_request_reload(p);
	}
	if (desktop) {
		desktop_dir_changed(p);
	}
}

static void display_in(int fd, short mask, void *data) {
	struct panel *p = data;
	if (mask & (POLLHUP | POLLERR)) {
		p->running = false;
		return;
	}
	if (wl_display_dispatch(p->display) == -1) {
		sway_log(SWAY_ERROR, "Wayland connection lost");
		p->running = false;
	}
}

static void ipc_in(int fd, short mask, void *data) {
	struct panel *p = data;
	if (mask & (POLLHUP | POLLERR)) {
		sway_log(SWAY_ERROR, "IPC connection lost");
		p->running = false;
		return;
	}
	ipc_panel_readable(p);
}

static const char usage[] =
	"Usage: tilewin-panel [options...]\n"
	"\n"
	"  -h, --help             Show this help and quit.\n"
	"  -v, --version          Show the version and quit.\n"
	"  -c, --config <file>    Taskbar config (default ~/.config/tileWin/taskbar.conf).\n"
	"  -s, --socket <path>    tileWin IPC socket.\n"
	"  -d, --debug            Verbose logging.\n"
	"\n"
	"The panel is normally started and supervised by tileWin (panel_command).\n";

int main(int argc, char **argv) {
	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},
		{"config", required_argument, NULL, 'c'},
		{"socket", required_argument, NULL, 's'},
		{"debug", no_argument, NULL, 'd'},
		{0, 0, 0, 0},
	};
	bool debug = false;
	int c;
	while ((c = getopt_long(argc, argv, "hvc:s:d", long_options, NULL)) != -1) {
		switch (c) {
		case 'c':
			free(panel.config_path);
			panel.config_path = tw_expand_home(optarg);
			break;
		case 's':
			free(panel.socket_path);
			panel.socket_path = strdup(optarg);
			break;
		case 'd':
			debug = true;
			break;
		case 'v':
			printf("tilewin-panel version " SWAY_VERSION "\n");
			return 0;
		default:
			fprintf(stderr, "%s", usage);
			return c == 'h' ? 0 : 1;
		}
	}
	sway_log_init(debug ? SWAY_DEBUG : SWAY_ERROR, NULL);

	struct sigaction sa = { .sa_handler = handle_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	struct sigaction ign = { .sa_handler = SIG_IGN };
	sigaction(SIGCHLD, &ign, NULL);
	sigaction(SIGPIPE, &ign, NULL);

	panel.ipc_cmd_fd = panel.ipc_event_fd = -1;
	panel.inotify_fd = panel.config_watch = panel.theme_watch = panel.desktop_watch = -1;
	panel.loop = loop_create();
	panel.layout = LAYOUT_WINDOW;
	if (!panel.config_path) {
		panel.config_path = panel_default_config_path();
	}

	if (!panel.socket_path) {
		panel.socket_path = get_socketpath();
	}
	if (!panel.socket_path || !ipc_panel_init(&panel)) {
		sway_log(SWAY_ERROR, "Unable to connect to tileWin IPC");
		return 1;
	}

	load_theme(&panel);
	if (!panel_wayland_init(&panel)) {
		return 1;
	}
	panel.config = panel_config_load(&panel, panel.config_path);
	if (!panel.config) {
		sway_log(SWAY_ERROR, "Using the built-in taskbar layout");
		panel.config = panel_config_load(&panel, NULL);
	}
	panel_outputs_update_bars(&panel);

	panel.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	watch_files(&panel);

	loop_add_fd(panel.loop, wl_display_get_fd(panel.display), POLLIN, display_in, &panel);
	loop_add_fd(panel.loop, panel.ipc_event_fd, POLLIN, ipc_in, &panel);
	if (panel.inotify_fd >= 0) {
		loop_add_fd(panel.loop, panel.inotify_fd, POLLIN, inotify_in, &panel);
	}

	panel.running = true;
	while (panel.running) {
		if (signal_flags & SIGNAL_STOP) {
			break;
		}
		if (signal_flags & SIGNAL_RELOAD) {
			signal_flags &= ~SIGNAL_RELOAD;
			panel_request_reload(&panel);
		}
		errno = 0;
		if (wl_display_flush(panel.display) == -1 && errno != EAGAIN) {
			break;
		}
		loop_poll(panel.loop);
	}

	panel_wayland_fini(&panel);
	panel_config_free(panel.config);
	tw_theme_free(panel.theme);
	ipc_panel_fini(&panel);
	if (panel.inotify_fd >= 0) {
		close(panel.inotify_fd);
	}
	free(panel.config_path);
	free(panel.socket_path);
	return 0;
}
