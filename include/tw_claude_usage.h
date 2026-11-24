#ifndef _TW_CLAUDE_USAGE_H
#define _TW_CLAUDE_USAGE_H

#include <stdbool.h>
#include <time.h>
#include "list.h"

/*
 * The usage limits of Claude Code accounts (the 5-hour and the 7-day window),
 * as Claude Code itself hands them to its status line command. tileWin never
 * reads or keeps a login of Claude's: tilewin-claude-usage, put before the
 * status line of a Claude Code folder ("connect"), writes down what Claude
 * Code shows, in ~/.cache/tileWin/claude-usage/, and the widget reads that.
 *
 * An account is a Claude Code folder: ~/.claude for the installed one
 * ($CLAUDE_CONFIG_DIR when set), any other for a portable one.
 */

struct tw_claude_window {
	bool known;
	double used;      // percent of the window used, 0 to 100
	time_t resets_at; // 0 when not known
	// where its pace is measured from: the start of the window (0), or when its
	// usage was last reset by hand, then at pace_base percent (tw_claude_usage_pace)
	time_t pace_from;
	double pace_base;
};

struct tw_claude_usage {
	char *dir;      // the Claude Code folder
	char *account;  // its email, NULL when not known
	time_t updated; // when Claude Code last showed the values; 0 for never
	struct tw_claude_window five_hour, seven_day;
	bool connected; // its status line writes the values down (Codex: it has session logs)
	char *history;  // the file of its history (tw_claude_history_*)
};

/*
 * A coding agent whose usage limits the widgets show: Claude Code (claude) or
 * Codex (codex, tw_codex_usage.h). Both have the same two windows, so the
 * widgets, their flyout and their charts are the same for each.
 */
struct tw_usage_tool {
	const char *type;       // the widget, "claude"
	const char *name;       // "Claude Code"
	const char *usage_site; // "claude.ai", its page of the limits
	const char *usage_url;
	bool connects;          // a folder is connected first (tilewin-claude-usage), else read as it is
	char *(*default_dir)(void);
	bool (*read)(const char *dir, struct tw_claude_usage *out);
	list_t *(*all)(void);
	// fills the history of a folder from what the tool kept itself, NULL for none
	int (*backfill)(const char *dir, time_t now);
};
extern const struct tw_usage_tool tw_claude_tool;
char *tw_usage_select_dir(const struct tw_usage_tool *tool, const char *account);

/* The folder of the installed Claude Code: $CLAUDE_CONFIG_DIR or ~/.claude. */
char *tw_claude_default_dir(void);
/* A folder as it is kept: absolute, without a slash at the end. */
char *tw_claude_normalize_dir(const char *dir);
/* The email of the account logged in there, from its .claude.json; NULL if none. */
char *tw_claude_account(const char *dir);
/* Where the values of that folder are written down. */
char *tw_claude_cache_path(const char *dir);
/* Whether the status line of that folder is tilewin-claude-usage. */
bool tw_claude_connected(const char *dir);

/* The values of that folder; false when none were written down yet. */
bool tw_claude_usage_read(const char *dir, struct tw_claude_usage *out);
void tw_claude_usage_finish(struct tw_claude_usage *usage);
/*
 * The folders known: the installed one when it is there, and every one with
 * values or a connection; struct tw_claude_usage *, newest values first.
 */
list_t *tw_claude_usage_all(void);
void tw_claude_usage_list_free(list_t *list);

/*
 * The share of a window used as of now: 0 once its reset time has passed (the
 * window started over), -1 when not known.
 */
double tw_claude_window_now(const struct tw_claude_window *window, time_t now);

/* ---------- what was used when: the history ---------- */

/*
 * Each time the values change, tilewin-claude-usage adds a line to
 * ~/.local/state/tileWin/claude-usage/<folder>.history; the days that
 * tw_usage_history_days says are kept. The flyout of the widget draws its
 * charts from it, the older windows too.
 */
struct tw_claude_point {
	time_t at;
	struct tw_claude_window five_hour, seven_day;
};
char *tw_claude_history_path(const char *dir);
/*
 * How many days of the usage are kept: "history_days" of the AI agent usage
 * widget in taskbar.conf (its block "widget agents"), 8 by default and at least,
 * at most 3650. Read again when the file changed.
 */
#define TW_USAGE_HISTORY_DAYS 8
int tw_usage_history_days(void);
/* The file of a history of a folder in ~/.local/state/tileWin/<kind>/. */
char *tw_usage_history_path(const char *kind, const char *dir);
/* Adds the values as of now to that history file, when they differ from the last line. */
void tw_claude_history_add(const char *path, const struct tw_claude_usage *usage, time_t now);
/*
 * Writes the points (oldest first) as the start of a history file that is not
 * there yet, in one go, by the rules of tw_claude_history_add: a point the same
 * as the one before is left out, and a window's usage only grows.
 */
bool tw_claude_history_write(const char *path, const struct tw_claude_point *points, int n);
/* The points of a history file, oldest first, in a new array; how many (0 for none, *out NULL). */
int tw_claude_history_read(const char *path, struct tw_claude_point **out);
/* Whether two reset times are those of the same window (they may wobble by a minute). */
bool tw_claude_same_window(time_t a, time_t b);
/*
 * Whether window a is older than window b: it resets earlier, so b came after it
 * (its reset, or one started by hand). A session of Claude Code still running
 * shows the window it saw last, so values of an older window are stale.
 */
bool tw_claude_window_older(const struct tw_claude_window *a, const struct tw_claude_window *b);

/* ---------- what the widget shows ---------- */

/*
 * The things the widget can show, in the order "show" lists them (taskbar.conf,
 * widget claude { show usage_5h reset_5h }): ids, what the settings call them,
 * and the short name the taskbar puts before each.
 */
struct tw_claude_item {
	const char *id, *title, *label;
	bool seven_day; // of the weekly window, else of the 5-hour one
	enum tw_claude_kind { TW_CLAUDE_USAGE, TW_CLAUDE_RESET, TW_CLAUDE_LEFT } kind;
};
extern const struct tw_claude_item tw_claude_items[];
extern const size_t tw_claude_item_count;
const struct tw_claude_item *tw_claude_item_find(const char *id);

/* What it shows when "show" is not set: little on the taskbar, all on the desktop. */
#define TW_CLAUDE_SHOW_TASKBAR "usage_5h usage_7d"
#define TW_CLAUDE_SHOW_DESKTOP "usage_5h reset_5h left_5h usage_7d reset_7d left_7d"

struct tw_claude_preset {
	const char *title, *show;
};
/* Ready-made choices for the taskbar and the desktop; each ends with an empty entry. */
extern const struct tw_claude_preset tw_claude_taskbar_presets[];
extern const struct tw_claude_preset tw_claude_desktop_presets[];

#define TW_CLAUDE_FIVE_HOURS (5 * 3600)
#define TW_CLAUDE_SEVEN_DAYS (7 * 24 * 3600)

/*
 * How long the window lasts at the pace it was used at since it started:
 * seconds until it is used up, or -1 when it lasts until it resets (or nothing
 * is known yet to tell a pace from). *known is false without values.
 */
long tw_claude_window_left(const struct tw_claude_window *window, long length, time_t now,
	bool *known);
/*
 * How fast the window is used, in percent a second: since it started, or since
 * its usage was last reset by hand. used is what is used now.
 */
double tw_claude_window_pace(const struct tw_claude_window *window, long length, time_t now,
	double used);
/*
 * A drop of this many points within one window is its usage reset by hand;
 * less is the wobble of sessions a point apart.
 */
#define TW_CLAUDE_RESET_DROP 5.0
/* Finds in the history where the pace of each window of u is measured from. */
void tw_claude_usage_pace(struct tw_claude_usage *u);
/* A span of time, "2 d 4 h", "1 h 12 min", "12 min"; short gives "2d 4h", "1h 12m". */
void tw_claude_format_span(long seconds, bool brief, char *out, size_t size);

#endif
