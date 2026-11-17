/*
 * The ranking of the Command Center (tw_command): a name that starts with what
 * was typed before one where a word does, before initials, before the letters
 * anywhere, keywords last; and the history: what was used often and lately
 * comes first, kept in a file, the oldest beyond the limit forgotten.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tw_command.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

int main(void) {
	CHECK(tw_cmd_match("Firefox", NULL, "fir") == 100, "the start of the name");
	CHECK(tw_cmd_match("Firefox", NULL, "FIR") == 100, "in any case");
	CHECK(tw_cmd_match("Take a screenshot", NULL, "scr") == 85, "the start of a word");
	CHECK(tw_cmd_match("Task View", NULL, "tv") == 75, "the initials");
	CHECK(tw_cmd_match("TaskView", NULL, "tv") == 75, "the initials of joined words");
	CHECK(tw_cmd_match("Notepad", NULL, "tep") == 55, "within the name");
	CHECK(tw_cmd_match("Lock the screen", "away leave", "leav") == 35, "a keyword");
	CHECK(tw_cmd_match("Thunderbird", NULL, "thbd") == 15, "letters in order, four or more");
	CHECK(tw_cmd_match("Shut down or restart", NULL, "not") == 0, "three letters in order are not enough");
	CHECK(tw_cmd_match("Firefox", NULL, "xyz") == 0, "no match");
	CHECK(tw_cmd_match("Firefox", NULL, "") == 1, "nothing typed matches everything a little");
	CHECK(tw_cmd_match(NULL, NULL, "a") == 0, "no name");

	char path[] = "/tmp/tw-command-XXXXXX";
	int fd = mkstemp(path);
	close(fd);
	struct tw_cmd_history h = { 0 };
	long long now = 1800000000LL;
	tw_cmd_history_note(&h, "app:firefox", now - 30 * 86400);
	for (int i = 0; i < 5; i++) {
		tw_cmd_history_note(&h, "app:firefox", now - 30 * 86400);
	}
	tw_cmd_history_note(&h, "act:lock", now - 60);
	tw_cmd_history_note(&h, "act:lock", now - 30);
	double fox = tw_cmd_frecency(&h, "app:firefox", now);
	double lock = tw_cmd_frecency(&h, "act:lock", now);
	CHECK(fox > 0 && lock > 0, "used things count");
	CHECK(tw_cmd_frecency(&h, "act:sleep", now) == 0, "an unused thing counts nothing");
	CHECK(lock > fox * 0.9, "lately counts: twice today about as much as six times a month ago "
		"(%.1f, %.1f)", lock, fox);
	CHECK(tw_cmd_history_save(&h, path), "the history is written");
	struct tw_cmd_history again = { 0 };
	CHECK(tw_cmd_history_load(&again, path), "the history is read");
	CHECK(again.count == 2 && tw_cmd_frecency(&again, "act:lock", now) == lock,
		"written and read again, the same");
	tw_cmd_history_free(&again);

	// beyond the limit the ones used longest ago go
	for (int i = 0; i < TW_CMD_HISTORY_MAX + 20; i++) {
		char key[32];
		snprintf(key, sizeof(key), "k%d", i);
		tw_cmd_history_note(&h, key, now + i);
	}
	tw_cmd_history_save(&h, path);
	tw_cmd_history_load(&again, path);
	CHECK(again.count == TW_CMD_HISTORY_MAX, "at most %d kept, not %d", TW_CMD_HISTORY_MAX,
		again.count);
	CHECK(tw_cmd_frecency(&again, "app:firefox", now) == 0, "the oldest forgotten");
	char last[32];
	snprintf(last, sizeof(last), "k%d", TW_CMD_HISTORY_MAX + 19);
	CHECK(tw_cmd_frecency(&again, last, now) > 0, "the newest kept");
	tw_cmd_history_free(&again);
	tw_cmd_history_free(&h);
	CHECK(!tw_cmd_history_load(&again, "/nonexistent/command-center") && again.count == 0,
		"no file: no history");
	remove(path);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("command: all checks passed\n");
	return 0;
}
