/*
 * focus_priority, on processes made up here: the focused window's process gets
 * the lower nice value in all its threads, the one before gets its own back
 * (a thread the app gave a value of its own keeps it), a process at a lower
 * value already keeps that, a pid used again by a new process is not touched,
 * turning it off gives the value back, and without the right to lower a nice
 * value nothing is left half changed.
 */
#include <errno.h>
#include <stdio.h>
#include "sway/tw_priority.h"

static int failures;

static void check(int ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

#define PROCS 4
#define THREADS 3

/* Process k has pid 100 * (k + 1) and threads pid, pid + 1, pid + 2. */
static struct {
	int nice[THREADS];
	unsigned long long start;
	bool alive;
} procs[PROCS];
static bool allowed = true; // may nice values be lowered

static int find(pid_t tid, int *thread) {
	int k = tid / 100 - 1, t = tid % 100;
	if (k < 0 || k >= PROCS || t >= THREADS || !procs[k].alive) {
		return -1;
	}
	*thread = t;
	return k;
}

static int fake_get(pid_t tid, int *nice) {
	int t, k = find(tid, &t);
	if (k < 0) {
		errno = ESRCH;
		return -1;
	}
	*nice = procs[k].nice[t];
	return 0;
}

static int fake_set(pid_t tid, int nice) {
	int t, k = find(tid, &t);
	if (k < 0) {
		errno = ESRCH;
		return -1;
	}
	if (nice < procs[k].nice[t] && !allowed) {
		errno = EACCES;
		return -1;
	}
	procs[k].nice[t] = nice;
	return 0;
}

static int fake_threads(pid_t pid, pid_t *out, int max) {
	int t, k = find(pid, &t);
	if (k < 0) {
		return -1;
	}
	for (int i = 0; i < THREADS && i < max; i++) {
		out[i] = pid + i;
	}
	return THREADS;
}

static unsigned long long fake_start(pid_t pid) {
	int t, k = find(pid, &t);
	return k < 0 ? 0 : procs[k].start;
}

static const struct tw_priority_ops fake = { fake_get, fake_set, fake_threads, fake_start };

static bool all_at(int k, int nice) {
	for (int t = 0; t < THREADS; t++) {
		if (procs[k].nice[t] != nice) {
			return false;
		}
	}
	return true;
}

int main(void) {
	tw_priority_set_ops(&fake);
	for (int k = 0; k < PROCS; k++) {
		procs[k].alive = true;
		procs[k].start = 1000 + k;
	}
	procs[2].nice[0] = procs[2].nice[1] = procs[2].nice[2] = 5;   // runs niced already
	procs[3].nice[0] = procs[3].nice[1] = procs[3].nice[2] = -12; // runs higher than asked

	tw_priority_focus(100, -10);
	check(all_at(0, -10), "the focused window's process is not raised in all its threads");
	check(tw_priority_raised() == 100, "the raised process is not remembered");

	tw_priority_focus(100, -10);
	check(all_at(0, -10), "focusing another window of the same process changes it");

	tw_priority_focus(200, -10);
	check(all_at(0, 0) && all_at(1, -10), "moving the focus does not give the old one its value back");

	tw_priority_focus(300, -10);
	check(all_at(1, 0) && all_at(2, -10), "a niced process is not raised");
	tw_priority_focus(100, -10);
	check(all_at(2, 5), "a niced process does not get its own value back");

	tw_priority_focus(400, -10);
	check(all_at(3, -12), "a process at a lower value already is changed");
	tw_priority_focus(100, -10);
	check(all_at(3, -12), "a process at a lower value already is changed on leaving it");

	// an app that gives a thread of its own a value keeps it
	procs[0].nice[2] = 3;
	tw_priority_focus(200, -10);
	check(procs[0].nice[0] == 0 && procs[0].nice[1] == 0 && procs[0].nice[2] == 3,
		"a thread with a value of its own does not keep it");

	// the process ends, and a new one gets its pid: it is not touched
	procs[1].start = 5555;
	procs[1].nice[0] = procs[1].nice[1] = procs[1].nice[2] = -10;
	tw_priority_focus(100, -10);
	check(all_at(1, -10), "a new process with the pid of the old one is changed");
	procs[1].nice[0] = procs[1].nice[1] = procs[1].nice[2] = 0;

	// a raised process that ended
	procs[0].alive = false;
	tw_priority_focus(200, -10);
	check(all_at(1, -10), "focusing after the raised process ended fails");
	procs[0].alive = true; // (a new one, in its place)
	procs[0].nice[0] = procs[0].nice[1] = procs[0].nice[2] = 0;

	// a desktop with no window, and turning it off
	tw_priority_focus(0, -10);
	check(all_at(1, 0) && tw_priority_raised() == 0, "focusing no window does not give it back");
	tw_priority_focus(200, -5);
	check(all_at(1, -5), "the value set is not used");
	tw_priority_focus(200, -15);
	check(all_at(1, -15), "a new value for the same window is not used");
	tw_priority_focus(200, 0);
	check(all_at(1, 0) && tw_priority_raised() == 0, "turning it off does not give the value back");

	// without the right to lower nice values: nothing half done, and nothing to give back
	allowed = false;
	tw_priority_focus(200, -10);
	check(all_at(1, 0) && tw_priority_raised() == 0, "without the right, something changed");
	allowed = true;

	// the compositor itself and init are never touched
	tw_priority_focus(1, -10);
	check(tw_priority_raised() == 0, "init is raised");

	tw_priority_focus(100, -10);
	tw_priority_forget();
	check(all_at(0, 0), "at exit the raised process does not get its value back");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
