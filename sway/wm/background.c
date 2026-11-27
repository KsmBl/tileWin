/*
 * Programs started with "exec --background": their windows open minimized
 * and do not take the focus, as an app started into the tray would, so that
 * starting it when the computer is idle (the taskbar's "Start in the
 * background when idle") gets in nobody's way.
 *
 * A window is one of theirs when its process is the program started or one it
 * started, or when its app id is the one given with --app (for programs that
 * hand their window to another process), while the start is fresh: two
 * minutes, and twenty seconds more after its first window (a splash screen
 * and then the main window).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wayland-util.h>
#include "sway/tilewin.h"
#include "sway/tree/view.h"
#include "log.h"

#define FRESH_S 120
#define AFTER_FIRST_S 20

struct background_start {
	pid_t pid;
	char *app; // NULL: by the process alone
	time_t until;
	bool matched;
	struct wl_list link;
};

static struct wl_list starts;
static bool starts_ready;

static time_t now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void start_free(struct background_start *s) {
	wl_list_remove(&s->link);
	free(s->app);
	free(s);
}

static void forget_old(void) {
	if (!starts_ready) {
		wl_list_init(&starts);
		starts_ready = true;
	}
	time_t now = now_s();
	struct background_start *s, *tmp;
	wl_list_for_each_safe(s, tmp, &starts, link) {
		if (now >= s->until) {
			start_free(s);
		}
	}
}

void tw_background_start(pid_t pid, const char *app) {
	forget_old();
	struct background_start *s = calloc(1, sizeof(*s));
	if (!s) {
		return;
	}
	s->pid = pid;
	s->app = app && *app ? strdup(app) : NULL;
	s->until = now_s() + FRESH_S;
	wl_list_insert(&starts, &s->link);
	sway_log(SWAY_DEBUG, "Started in the background: pid %d, app %s", pid, app ? app : "-");
}

static pid_t parent_of(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	pid_t parent = -1;
	if (fgets(line, sizeof(line), f)) {
		// "pid (name) state ppid": the name may hold spaces and brackets
		char *close = strrchr(line, ')');
		if (close && sscanf(close + 1, " %*c %d", &parent) != 1) {
			parent = -1;
		}
	}
	fclose(f);
	return parent;
}

/* "org.mozilla.firefox" and "firefox" and "Firefox" are the same app here. */
static bool same_app(const char *want, const char *have) {
	if (!want || !have) {
		return false;
	}
	if (strcasecmp(want, have) == 0) {
		return true;
	}
	const char *dot = strrchr(want, '.');
	const char *hdot = strrchr(have, '.');
	return strcasecmp(dot ? dot + 1 : want, hdot ? hdot + 1 : have) == 0;
}

bool tw_background_view(struct sway_view *view) {
	forget_old();
	if (wl_list_empty(&starts)) {
		return false;
	}
	const char *app_id = view_get_app_id(view);
	const char *class = view_get_class(view);
	struct background_start *found = NULL, *s;
	wl_list_for_each(s, &starts, link) {
		if (same_app(s->app, app_id) || same_app(s->app, class)) {
			found = s;
			break;
		}
	}
	for (pid_t pid = view->pid; !found && pid > 1; pid = parent_of(pid)) {
		wl_list_for_each(s, &starts, link) {
			if (s->pid == pid) {
				found = s;
				break;
			}
		}
	}
	if (!found) {
		return false;
	}
	if (!found->matched) {
		found->matched = true;
		time_t soon = now_s() + AFTER_FIRST_S;
		if (soon < found->until) {
			found->until = soon;
		}
	}
	return true;
}
