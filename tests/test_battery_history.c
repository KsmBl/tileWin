/*
 * The history of the battery behind the chart of the battery flyout: readings
 * are taken down every few minutes and on every change, kept for a day and a
 * bit, survive being written and read back, and become the points of the
 * chart, where a gap (the machine off or hibernating) is a straight line from
 * the last reading before it to the first after it, never a drop to nothing.
 * The battery is read from a small copy of /sys/class/power_supply.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tw_battery.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}

static bool near(double a, double b) {
	return fabs(a - b) < 1e-6;
}

static char root[] = "/tmp/tw-battery-XXXXXX";

static void put(const char *dir, const char *name, const char *value) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s/%s", root, dir, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", value);
		fclose(f);
	}
}

int main(void) {
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}

	// reading the battery: the mouse's tells no charge, the mains are no battery
	int percent = -1;
	bool charging = true;
	check(!tw_battery_read(root, &percent, &charging), "no battery in an empty folder");
	put("AC", "type", "Mains");
	put("hidpp_battery_0", "type", "Battery");
	check(!tw_battery_read(root, &percent, &charging), "a mouse's battery is not the computer's");
	put("BAT0", "type", "Battery");
	put("BAT0", "capacity", "57");
	put("BAT0", "status", "Discharging");
	check(tw_battery_read(root, &percent, &charging) && percent == 57 && !charging,
		"the battery reads 57%, discharging");
	put("BAT0", "status", "Charging");
	check(tw_battery_read(root, &percent, &charging) && percent == 57 && charging,
		"and charging once plugged in");

	// taking readings down
	struct tw_battery_history h = { 0 };
	long long t0 = 1700000000;
	check(tw_battery_history_record(&h, t0, 80, false, TW_BATTERY_INTERVAL),
		"the first reading is taken down");
	check(!tw_battery_history_record(&h, t0 + 60, 80, false, TW_BATTERY_INTERVAL),
		"the same a minute later is not");
	check(tw_battery_history_record(&h, t0 + 120, 79, false, TW_BATTERY_INTERVAL),
		"a percent less is, at once");
	check(tw_battery_history_record(&h, t0 + 180, 79, true, TW_BATTERY_INTERVAL),
		"so is being plugged in");
	check(tw_battery_history_record(&h, t0 + 180 + TW_BATTERY_INTERVAL, 79, true,
		TW_BATTERY_INTERVAL), "and the same again after five minutes");
	check(h.count == 4, "four readings");
	// a clock set back: what came later goes
	check(tw_battery_history_record(&h, t0 + 150, 79, true, TW_BATTERY_INTERVAL) &&
		h.count == 3 && h.samples[2].time == t0 + 150, "a clock set back drops what came after");

	// written and read back, with junk lines and readings out of order skipped
	char *text = tw_battery_history_format(&h);
	struct tw_battery_history back = { 0 };
	tw_battery_history_parse(&back, text);
	check(back.count == h.count && memcmp(back.samples, h.samples,
		h.count * sizeof(*h.samples)) == 0, "read back as written");
	free(text);
	tw_battery_history_parse(&back, "# head\n100 50 -\nnonsense\n90 40 -\n200 101 -\n300 30 c\n"
		"400 20 x\n500 10 -");
	check(back.count == 3 && back.samples[0].time == 100 && back.samples[1].time == 300 &&
		back.samples[1].charging && back.samples[2].percent == 10,
		"junk, out of order and impossible lines are skipped, a last line without newline read");

	char path[600];
	snprintf(path, sizeof(path), "%s/state/battery-history", root);
	check(tw_battery_history_save(&h, path), "saved, with its folder made");
	check(tw_battery_history_load(&back, path) && back.count == h.count, "loaded again");
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
	check(access(tmp, F_OK) != 0, "no temporary file is left behind");

	// kept a day and a bit
	tw_battery_history_free(&h);
	for (long long t = 0; t <= 48 * 3600; t += 600) {
		tw_battery_history_record(&h, t0 + t, 50, false, TW_BATTERY_INTERVAL);
	}
	tw_battery_history_prune(&h, t0 + 48 * 3600, TW_BATTERY_KEEP);
	check(h.count > 0 && h.samples[0].time >= t0 + 48 * 3600 - TW_BATTERY_KEEP &&
		h.samples[0].time < t0 + 48 * 3600 - TW_BATTERY_KEEP + 600,
		"readings older than the time kept are dropped");

	// the chart: discharging, then off overnight, then charging
	tw_battery_history_free(&h);
	long long now = t0 + 24 * 3600;
	long long start = now - TW_BATTERY_SPAN;
	tw_battery_history_append(&h, (struct tw_battery_sample){ start - 3600, 100, false });
	tw_battery_history_append(&h, (struct tw_battery_sample){ start + 3600, 80, false });
	tw_battery_history_append(&h, (struct tw_battery_sample){ start + 3900, 79, false });
	// shut down here for ten hours, and found fuller: it charged while off
	tw_battery_history_append(&h, (struct tw_battery_sample){ start + 3900 + 36000, 90, true });
	tw_battery_history_append(&h, (struct tw_battery_sample){ start + 3900 + 36300, 92, true });
	// hibernated: found emptier
	tw_battery_history_append(&h, (struct tw_battery_sample){ now - 7200, 60, false });
	struct tw_battery_sample current = { now, 58, false };
	int count = 0;
	struct tw_battery_point *p = tw_battery_history_points(&h, now, TW_BATTERY_SPAN, &current,
		&count);
	check(count == 7, "seven points: the left edge, five readings and now");
	if (count == 7) {
		check(near(p[0].x, 0) && near(p[0].y, 0.90),
			"the line from before the day is cut at the left edge, halfway from 100% to 80%");
		check(near(p[1].x, 3600.0 / TW_BATTERY_SPAN) && near(p[1].y, 0.80), "a reading in place");
		check(!p[1].gap && !p[1].charging, "an hour of discharging is no gap");
		check(p[2].gap && p[2].charging,
			"ten hours off is a gap, bridged straight, and charging as it came back fuller");
		check(near(p[3].y, 0.90) && near(p[3].x, (3900.0 + 36000) / TW_BATTERY_SPAN),
			"the first reading after it, where it was taken");
		check(!p[3].gap && p[3].charging, "charging after it");
		check(p[4].gap && !p[4].charging, "hibernated and found emptier: a gap, not charging");
		check(near(p[6].x, 1) && near(p[6].y, 0.58), "now at the right edge");
		bool positive = true;
		for (int i = 0; i < count; i++) {
			positive = positive && p[i].y > 0.5 && p[i].x >= 0 && p[i].x <= 1;
		}
		check(positive, "no point drops to nothing or leaves the chart");
	}
	free(p);

	// no reading in the day yet, only an old one and now: one line across it
	tw_battery_history_free(&h);
	tw_battery_history_append(&h, (struct tw_battery_sample){ now - 2 * TW_BATTERY_SPAN, 40, false });
	p = tw_battery_history_points(&h, now, TW_BATTERY_SPAN, &current, &count);
	check(count == 2 && near(p[0].x, 0) && near(p[0].y, 0.49) && near(p[1].y, 0.58) &&
		p[0].gap, "a line from a day before is cut at the left edge");
	free(p);
	// nothing at all
	tw_battery_history_free(&h);
	p = tw_battery_history_points(&h, now, TW_BATTERY_SPAN, NULL, &count);
	check(!p && count == 0, "nothing to draw without readings");
	p = tw_battery_history_points(&h, now, TW_BATTERY_SPAN, &current, &count);
	check(count == 1 && near(p[0].x, 1), "only now: one point at the right edge");
	free(p);

	tw_battery_history_free(&h);
	tw_battery_history_free(&back);
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0) {
		fprintf(stderr, "could not remove %s\n", root);
	}
	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
