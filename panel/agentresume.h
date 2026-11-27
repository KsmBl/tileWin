#ifndef TW_AGENTRESUME_H
#define TW_AGENTRESUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "tw_agent_accounts.h"
#include "tw_claude_usage.h"

struct panel;
struct agent_usage;

#define AGENT_RESUME_DEFAULT_DELAY 30
#define AGENT_RESUME_DEFAULT_COMMAND "continue"
#define AGENT_RESUME_MAX_DELAY 3600

struct agent_resume_config {
	bool enabled;
	int delay_s;         // seconds to wait after resets_at (0..3600)
	int threshold;       // usage percent at or above which a window arms auto-resume
	const char *command; // text typed into each interactive agent terminal, followed by Enter
};

struct agent_resume_window_input {
	enum tw_agent agent;
	const char *account_id;  // e.g. "claude:~/.claude", "codex:~/.codex", "antigravity"
	const char *account_dir; // normalized folder for claude/codex, else NULL
	const char *bucket;      // "5h", "7d", or Antigravity bucket name (e.g. "gemini-5h")
	bool seven_day;
	struct tw_claude_window window;
	bool weekly_blocked;     // true when a corresponding weekly window is still at 100%
};

struct agent_resume_status {
	bool enabled;
	bool scheduled; // an armed window is waiting for reset + delay
	bool deferred;  // reset passed while screen locked; will run on unlock
	time_t fire_at; // earliest scheduled time for this account, 0 for none
	int delay_s;
	int terminals;  // open interactive terminal windows currently matched for this account
};

void agent_resume_init(struct panel *panel);
void agent_resume_fini(void);

/* Updates auto-resume settings from the AI agent usage widget config. */
void agent_resume_configure(struct panel *panel, const struct agent_resume_config *cfg);

/* Records one observed usage window and arms or updates its reset timer. */
void agent_resume_observe(struct panel *panel, const struct agent_resume_window_input *in,
	const struct agent_usage *collected, time_t now);

/* Called when the compositor locks or unlocks the session. */
void agent_resume_lock_changed(struct panel *panel);

/* Current status for one account (for the flyout and tooltip). */
void agent_resume_account_status(struct panel *panel, enum tw_agent agent,
	const char *account_id, const char *account_dir, const struct agent_usage *collected,
	struct agent_resume_status *out);

/* ---------- Helpers (also tested directly) ---------- */

/* True when a window's app_id/class is a terminal emulator (never an editor or browser). */
bool agent_resume_is_terminal_app(const char *app_id);

/*
 * Returns the enum tw_agent of an interactive Claude Code, Codex or Antigravity
 * process from its NUL-separated /proc/<pid>/cmdline, or -1 if it is not an
 * interactive session (e.g. claude -p, codex app-server/exec, agy --settings).
 */
int agent_resume_interactive_agent(const char *cmdline, size_t len);

/*
 * True when an Antigravity quota bucket name (e.g. "gemini-5h", "3p-5h",
 * "gemini-argon-wtu-weekly") applies to a worker's model id; NULL/empty matches any.
 */
bool agent_resume_agy_bucket_matches_model(const char *bucket, const char *model);

/* Maps a printable ASCII character to a US keymap evdev keycode and Shift state. */
bool agent_resume_ascii_key(char ch, uint32_t *keycode, bool *shift);

/*
 * Inspects a single agent session's log to check whether that session stopped
 * due to a usage/rate limit and has not progressed since. Idle sessions that
 * ended on a normal response return false.
 */
bool agent_resume_claude_log_hit_limit(const char *path);
bool agent_resume_codex_log_hit_limit(const char *path);
bool agent_resume_agy_session_hit_limit(const char *cli_log_path,
	const char *transcript_path, const char *bucket);

#endif
