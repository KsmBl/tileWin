#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "sway/tw_priority.h"

/*
 * The focused window first: the process of the window that has the focus
 * gets a lower nice value (all its threads), so the scheduler lets it draw
 * before the rest; when another window gets the focus, the one before gets
 * back the nice value it had, and the new one is raised. A process that
 * already runs at a lower nice value keeps it.
 *
 * Lowering a nice value takes CAP_SYS_NICE (install.sh gives it to tileWin,
 * as sway's own documentation suggests for its scheduling); without it the
 * setting does nothing, and says so once in the log.
 *
 * A process is known by its pid and the time it started, so a pid used again
 * by a new process after the old one ended is not mistaken for it.
 */

static int sys_get_nice(pid_t tid, int *nice) {
	errno = 0;
	int v = getpriority(PRIO_PROCESS, (id_t)tid);
	if (v == -1 && errno) {
		return -1;
	}
	*nice = v;
	return 0;
}

static int sys_set_nice(pid_t tid, int nice) {
	return setpriority(PRIO_PROCESS, (id_t)tid, nice);
}

static int sys_threads(pid_t pid, pid_t *out, int max) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	DIR *d = opendir(path);
	if (!d) {
		return -1;
	}
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		if (e->d_name[0] >= '0' && e->d_name[0] <= '9') {
			out[n++] = (pid_t)atoi(e->d_name);
		}
	}
	closedir(d);
	return n;
}

static unsigned long long sys_start_time(pid_t pid) {
	char path[64], buf[1024];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	// after the name in parentheses (which may hold anything) come fields 3 on;
	// the start time is field 22
	char *p = strrchr(buf, ')');
	if (!p) {
		return 0;
	}
	unsigned long long start = 0;
	int field = 2;
	for (char *tok = strtok(p + 1, " "); tok; tok = strtok(NULL, " ")) {
		if (++field == 22) {
			start = strtoull(tok, NULL, 10);
			break;
		}
	}
	return start;
}

static const struct tw_priority_ops sys_ops = {
	sys_get_nice, sys_set_nice, sys_threads, sys_start_time,
};

#define THREADS_MAX 4096

static struct {
	const struct tw_priority_ops *ops;
	pid_t pid;                 // raised now, 0 for none
	unsigned long long start;  // when it started
	int before;                // its nice value before
	int raised;                // what it was given
	bool warned;               // the missing capability said once
} state;

static const struct tw_priority_ops *ops(void) {
	return state.ops ? state.ops : &sys_ops;
}

void tw_priority_set_ops(const struct tw_priority_ops *o) {
	state.ops = o;
}

/* Every thread of a process to a nice value; false when it is not allowed. */
static bool set_all(pid_t pid, int nice, bool only_if, int if_now) {
	static pid_t tids[THREADS_MAX];
	int n = ops()->threads(pid, tids, THREADS_MAX);
	if (n < 0) {
		return true; // gone: nothing to do
	}
	bool ok = true;
	for (int i = 0; i < n; i++) {
		int now;
		// on restoring, a thread the app gave a nice value of its own keeps it
		if (only_if && (ops()->get_nice(tids[i], &now) != 0 || now != if_now)) {
			continue;
		}
		if (ops()->set_nice(tids[i], nice) != 0 && errno != ESRCH) {
			ok = false;
		}
	}
	return ok;
}

static void restore(void) {
	if (state.pid <= 0) {
		return;
	}
	if (ops()->start_time(state.pid) == state.start && state.raised != state.before) {
		set_all(state.pid, state.before, true, state.raised);
	}
	state.pid = 0;
}

void tw_priority_focus(pid_t pid, int nice) {
	if (nice >= 0 || pid <= 1 || pid == getpid()) {
		restore();
		return;
	}
	unsigned long long start = ops()->start_time(pid);
	if (pid == state.pid && start == state.start) {
		if (state.raised == (nice < state.before ? nice : state.before)) {
			return; // the same window's process, or another of its windows
		}
	}
	restore();
	int before;
	if (start == 0 || ops()->get_nice(pid, &before) != 0) {
		return; // gone already
	}
	int raised = nice < before ? nice : before;
	state.pid = pid;
	state.start = start;
	state.before = before;
	state.raised = raised;
	if (raised == before) {
		return; // it runs at that or lower already
	}
	if (!set_all(pid, raised, false, 0)) {
		// not allowed: put back what did change, and say why once
		set_all(pid, before, true, raised);
		state.pid = 0;
		if (!state.warned) {
			fprintf(stderr, "tileWin: focus_priority needs CAP_SYS_NICE to lower the nice "
				"value of the focused window (install.sh gives it; or run "
				"'sudo setcap cap_sys_nice=ep <tilewin>')\n");
			state.warned = true;
		}
	}
}

pid_t tw_priority_raised(void) {
	return state.pid;
}

void tw_priority_forget(void) {
	restore();
	state.warned = false;
}
