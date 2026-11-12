#ifndef _TW_BATTERY_H
#define _TW_BATTERY_H
#include <stdbool.h>

/*
 * The battery, and what it said over the last day. The taskbar takes down the
 * charge every few minutes and whenever it changes, for as long as the session
 * runs, in a small file in the state folder, so the battery flyout can draw a
 * chart of it that survives a restart, a shutdown and hibernation.
 *
 * Nothing is taken down while the machine is off, asleep or hibernating: the
 * chart bridges such a gap with a straight line from the last reading before
 * it to the first after it.
 */

#define TW_POWER_SUPPLY_DIR "/sys/class/power_supply"
#define TW_BATTERY_INTERVAL 300     // a reading at least every 5 minutes
#define TW_BATTERY_GAP 900          // readings further apart had a gap between them
#define TW_BATTERY_KEEP (48 * 3600) // how long readings are kept
#define TW_BATTERY_SPAN (24 * 3600) // what the chart shows

/* $TILEWIN_SYS_POWER (a copy for the tests) or /sys/class/power_supply. */
const char *tw_power_supply_dir(void);

/*
 * The first battery under root (tw_power_supply_dir()) that tells its charge:
 * the batteries of mice and keyboards tell none. False without one.
 */
bool tw_battery_read(const char *root, int *percent, bool *charging);

struct tw_battery_sample {
	long long time; // unix time
	int percent;
	bool charging;
};

struct tw_battery_history {
	struct tw_battery_sample *samples; // oldest first
	int count, capacity;
};

void tw_battery_history_free(struct tw_battery_history *h);

/* Appends a sample; one no later than the last one is dropped. */
void tw_battery_history_append(struct tw_battery_history *h, struct tw_battery_sample s);

/*
 * Reads the lines "<time> <percent> c|-" of the file's text, skipping those
 * that are no such line or out of order. Replaces what h held.
 */
void tw_battery_history_parse(struct tw_battery_history *h, const char *text);
char *tw_battery_history_format(const struct tw_battery_history *h);
bool tw_battery_history_load(struct tw_battery_history *h, const char *path);
/* Written to a temporary file that then replaces it. */
bool tw_battery_history_save(const struct tw_battery_history *h, const char *path);

/* Drops the samples older than keep seconds before now, and those after now
 * (a clock set back). */
void tw_battery_history_prune(struct tw_battery_history *h, long long now, long long keep);

/*
 * Takes down a reading when it is due: the first one, one interval seconds
 * after the last, or when the charge or charging changed. Returns whether it
 * was taken down.
 */
bool tw_battery_history_record(struct tw_battery_history *h, long long now, int percent,
	bool charging, long long interval);

/*
 * A point of the chart: x from 0 (span seconds ago) to 1 (now), y from 0 (empty)
 * to 1 (full). charging and gap tell of the line from it to the next point:
 * whether the battery charged on it, and whether nothing was taken down on it
 * (the machine was off or asleep), which is drawn straight all the same.
 */
struct tw_battery_point {
	double x, y;
	bool charging, gap;
};

/*
 * The points of the chart of the last span seconds before now, with the
 * reading of now (current, NULL for none) at the right edge, and a line that
 * comes from before the span cut at its left edge. Newly allocated, count of
 * them in *count; NULL with *count 0 when there is nothing to draw.
 */
struct tw_battery_point *tw_battery_history_points(const struct tw_battery_history *h,
	long long now, long long span, const struct tw_battery_sample *current, int *count);

/* $XDG_STATE_HOME/tileWin/battery-history. Newly allocated. */
char *tw_battery_history_path(void);

#endif
