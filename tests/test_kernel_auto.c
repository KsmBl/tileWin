/*
 * What the kernel widget and the Kernel page make of the files of the
 * automatic updates (tw_kernel_auto): off when never switched on; how far a
 * build is while a run holds its lock; a kernel waiting for the restart; fixes
 * applied lately; an error first; the log newest first; the release of a name.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tw_kernel_auto.h"

static int failures;
static char root[256];

#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static void put(const char *path, const char *text) {
	char full[512], cmd[600];
	snprintf(full, sizeof(full), "%s%s", root, path);
	snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", full);
	if (system(cmd) != 0) {
		return;
	}
	FILE *f = fopen(full, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

int main(void) {
	snprintf(root, sizeof(root), "/tmp/tw-kernel-auto-XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}
	setenv("TILEWIN_KERNEL_AUTO_ROOT", root, 1);
	struct tw_kernel_auto ka;
	struct tw_kernel_auto_summary s;
	time_t now = time(NULL);

	CHECK(!tw_kernel_auto_read(&ka), "read without a config");
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_OFF, "never switched on is off, is %d", s.kind);

	put("/etc/tilewin/kernel-auto.conf", "enabled=yes\nprofile=device\nseries=new\nnative=on\n");
	char status[512];
	snprintf(status, sizeof(status), "state=building\nrunning=7.3.4-tilewin1\nrunning_auto=yes\n"
		"fixes=7.3.5\nfixes_left=2\nchecked=%ld\n", (long)now);
	put("/var/lib/tilewin/kernel-auto/status", status);
	char build[256];
	snprintf(build, sizeof(build), "kind=kernel\nphase=compile\npercent=69\nversion=7.3.6\n"
		"updated=%ld\n", (long)now);
	put("/var/lib/tilewin/kernel-auto/build/kernel", build);
	put("/run/tilewin-kernel-auto.lock", "");
	char lock[512];
	snprintf(lock, sizeof(lock), "%s/run/tilewin-kernel-auto.lock", root);
	int fd = open(lock, O_RDWR);
	flock(fd, LOCK_EX);
	// a lock of another open file, as tilewin-kernel-root holds it
	CHECK(tw_kernel_auto_read(&ka), "read with a config");
	CHECK(strcmp(ka.profile, "device") == 0 && ka.native && strcmp(ka.series, "new") == 0,
		"the options of the config");
	CHECK(ka.busy, "a run that holds the lock goes on");
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_BUILDING && s.percent == 69, "building at 69%%: %d %d", s.kind, s.percent);
	CHECK(strcmp(s.brief, "Building 7.3.6 · 69%") == 0, "brief: %s", s.brief);
	snprintf(build, sizeof(build), "kind=kernel\nphase=download\npercent=15\nversion=7.3.6\n"
		"updated=%ld\n", (long)now);
	put("/var/lib/tilewin/kernel-auto/build/kernel", build);
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_DOWNLOADING && s.percent == 15, "downloading at 15%%");
	flock(fd, LOCK_UN);
	close(fd);

	snprintf(status, sizeof(status), "state=idle\nrunning=7.3.4-tilewin1\nrunning_auto=yes\n"
		"next=7.3.6-tilewin1\nfixes=7.3.5\n");
	put("/var/lib/tilewin/kernel-auto/status", status);
	tw_kernel_auto_read(&ka);
	CHECK(!ka.busy, "no run without the lock held");
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_RESTART && strcmp(s.brief, "7.3.6 at restart") == 0,
		"a kernel waiting for the restart: %d %s", s.kind, s.brief);

	snprintf(status, sizeof(status), "state=idle\nrunning=7.3.4-tilewin1\nrunning_auto=yes\n"
		"next=7.3.6-tilewin1\nerror=could not download\n");
	put("/var/lib/tilewin/kernel-auto/status", status);
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_ERROR && s.attention, "an error comes first");

	snprintf(status, sizeof(status), "state=idle\nrunning=7.3.4-tilewin1\nrunning_auto=yes\n"
		"fixes=7.3.6\nfixes_applied=%ld\n", (long)now - 60);
	put("/var/lib/tilewin/kernel-auto/status", status);
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_FIXED, "fixes applied lately");
	tw_kernel_auto_summarize(&ka, now + 2 * 86400, &s);
	CHECK(s.kind == TW_KA_OK, "two days later: up to date");

	put("/var/lib/tilewin/kernel-auto/status", "state=idle\nrunning=7.3.4-arch1-1\nrunning_auto=no\n");
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_PENDING, "the distribution's kernel runs: the first kernel comes");

	put("/var/lib/tilewin/kernel-auto/log", "100\tfirst\n200\tsecond\n300\tthird\n");
	tw_kernel_auto_read(&ka);
	CHECK(ka.log_count == 3 && strcmp(ka.log[0].text, "third") == 0 && ka.log[2].when == 100,
		"the log, newest first");

	char rel[32];
	tw_kernel_auto_release("7.3.6-tilewin2", rel, sizeof(rel));
	CHECK(strcmp(rel, "7.3.6") == 0, "release of 7.3.6-tilewin2: %s", rel);
	tw_kernel_auto_release("7.4.0-tilewin1", rel, sizeof(rel));
	CHECK(strcmp(rel, "7.4") == 0, "release of 7.4.0-tilewin1: %s", rel);

	char words[96];
	put("/etc/tilewin/kernel-auto.conf", "enabled=yes\nwhen=wed\ntime=21:30\n");
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_schedule_words(&ka, words, sizeof(words));
	CHECK(!ka.timer_on && strstr(words, "Update now"), "the timer off: only Update now: %s", words);
	put("/etc/systemd/system/timers.target.wants/tilewin-kernel-auto.timer", "");
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_schedule_words(&ka, words, sizeof(words));
	CHECK(ka.timer_on && strcmp(words, "every Wednesday at 21:30") == 0, "the timer on: %s", words);

	put("/etc/tilewin/kernel-auto.conf", "enabled=no\n");
	tw_kernel_auto_read(&ka);
	tw_kernel_auto_summarize(&ka, now, &s);
	CHECK(s.kind == TW_KA_OFF, "switched off");

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0) {
		failures++;
	}
	printf(failures ? "%d failed\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
