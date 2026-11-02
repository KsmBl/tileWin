#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "sway/config.h"
#include "sway/tilewin.h"
#include "tw_search.h"

/*
 * Dark mode by the clock (dark_schedule 21:00 07:00), looked at once a minute by
 * the same timer as the battery. It only acts when the time passes a start or
 * an end, as the night light does, so a change by hand lasts until the next of
 * them. TILEWIN_SCHEDULE_CLOCK names a file holding the time, for the tests.
 */
static struct {
	int from, to;
	int wanted; // what it last asked for: 0 light, 1 dark, -1 nothing yet
} ds = { -1, -1, -1 };

static int minutes_now(void) {
	const char *file = getenv("TILEWIN_SCHEDULE_CLOCK");
	if (file && *file) {
		FILE *f = fopen(file, "r");
		char line[16] = "";
		int minutes = -1;
		if (f) {
			if (fgets(line, sizeof(line), f)) {
				line[strcspn(line, "\n")] = '\0';
				tw_time_parse(line, &minutes);
			}
			fclose(f);
		}
		if (minutes >= 0) {
			return minutes;
		}
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return tm.tm_hour * 60 + tm.tm_min;
}

void tw_dark_schedule_check(void) {
	if (!config) {
		return;
	}
	int from = config->tw_dark_from, to = config->tw_dark_to;
	if (from != ds.from || to != ds.to) {
		ds.from = from;
		ds.to = to;
		ds.wanted = -1; // a new schedule applies at once
	}
	if (from < 0) {
		return;
	}
	int wanted = tw_time_between(minutes_now(), from, to);
	if (wanted == ds.wanted) {
		return;
	}
	ds.wanted = wanted;
	char *error = NULL;
	if (!tw_set_color_scheme(wanted, &error)) {
		sway_log(SWAY_ERROR, "The schedule of dark mode: %s", error ? error : "failed");
	}
	free(error);
}
