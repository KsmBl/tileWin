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
};

struct tw_claude_usage {
	char *dir;      // the Claude Code folder
	char *account;  // its email, NULL when not known
	time_t updated; // when Claude Code last showed the values; 0 for never
	struct tw_claude_window five_hour, seven_day;
	bool connected; // its status line writes the values down
};

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
/* A span of time, "2 d 4 h", "1 h 12 min", "12 min"; short gives "2d 4h", "1h 12m". */
void tw_claude_format_span(long seconds, bool brief, char *out, size_t size);

#endif
