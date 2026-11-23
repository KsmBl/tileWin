#ifndef TW_AGENT_ACCOUNTS_H
#define TW_AGENT_ACCOUNTS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The accounts of AI agents the usage widget can show, each by an id that
 * goes into its "accounts" option:
 *
 *   claude:~/.claude   a Claude Code folder (a portable one by its own folder)
 *   codex:~/.codex     a Codex folder
 *   copilot            GitHub Copilot CLI, as it is signed in
 *   antigravity        Antigravity, as it is signed in
 *
 * The folders are written with ~ for the home, so the config stays the same
 * on another computer.
 */
enum tw_agent {
	TW_AGENT_CLAUDE,
	TW_AGENT_CODEX,
	TW_AGENT_COPILOT,
	TW_AGENT_ANTIGRAVITY,
	TW_AGENT_COUNT,
};

struct tw_agent_account {
	enum tw_agent agent;
	char id[300];
	char *dir; // the folder of a Claude Code or Codex account, else NULL
};

/* "Claude Code", "Codex", "Copilot", "Antigravity". */
extern const char *const tw_agent_names[TW_AGENT_COUNT];

/* An account by its id; false for an id of nothing known. Free with tw_agent_account_finish. */
bool tw_agent_account_parse(const char *id, struct tw_agent_account *out);
/* The id of a folder's account, with ~ for the home. */
void tw_agent_account_id(enum tw_agent agent, const char *dir, char *out, size_t size);
void tw_agent_account_finish(struct tw_agent_account *account);

/*
 * The accounts on this computer: every Claude Code folder something was
 * written down for and the installed one, the Codex folder, Copilot and
 * Antigravity when their command is installed. *out is an array to free with
 * tw_agent_accounts_free; returns how many.
 */
int tw_agent_accounts_found(struct tw_agent_account **out);
void tw_agent_accounts_free(struct tw_agent_account *accounts, int count);

/* Whether a command is on $PATH. */
bool tw_agent_command_installed(const char *command);

#endif
