#ifndef _TW_CODEX_USAGE_H
#define _TW_CODEX_USAGE_H

#include <stdbool.h>
#include <time.h>
#include "list.h"
#include "tw_claude_usage.h"

/*
 * The usage limits of Codex (the 5-hour and the weekly window of the ChatGPT
 * plan it works with), as Codex itself writes them into its session logs with
 * each answer: ~/.codex/sessions/<year>/<month>/<day>/rollout-*.jsonl, lines
 * of "type":"token_count" with "rate_limits": { "primary": { "used_percent",
 * "window_minutes", "resets_at" (or "resets_in_seconds" in older Codex) },
 * "secondary": {...}, "plan_type" }. Nothing is connected and the login of
 * Codex is never read.
 *
 * An account is a Codex folder: ~/.codex for the installed one ($CODEX_HOME
 * when set). The values go into the same struct tw_claude_usage, the windows
 * the same; the account is the plan ("ChatGPT Plus").
 */

/* The folder of the installed Codex: $CODEX_HOME or ~/.codex. */
char *tw_codex_default_dir(void);
/*
 * The values of the newest line with limits in the session logs of that
 * folder, of whichever session answered last; false when there is none. Within
 * a window what was used only grows: a higher value its history has stays.
 */
bool tw_codex_usage_read(const char *dir, struct tw_claude_usage *out);
/* The folders known: the installed one when it is there; struct tw_claude_usage *. */
list_t *tw_codex_usage_all(void);

/* ~/.local/state/tileWin/codex-usage/<folder>.history */
char *tw_codex_history_path(const char *dir);
/*
 * A history that is not there yet is filled from the session logs of the last
 * eight days, in the order of time; how many readings it took (0 when it was
 * there already).
 */
int tw_codex_history_backfill(const char *dir, time_t now);
/* Quota observations from recent session logs, oldest first. The caller frees *out. */
int tw_codex_history_points(const char *dir, time_t now, struct tw_claude_point **out);

extern const struct tw_usage_tool tw_codex_tool;

#endif
