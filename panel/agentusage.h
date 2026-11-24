#ifndef TW_AGENTUSAGE_H
#define TW_AGENTUSAGE_H

#include <glib.h>
#include "claudecharts.h"

enum agent_kind { AGENT_CLAUDE, AGENT_CODEX, AGENT_COPILOT, AGENT_AGY, AGENT_COUNT };

#define AGENT_DAYS 30

struct agent_worker {
	enum agent_kind kind;
	int pid, parent;
	time_t started;
	bool terminal;
	char *session, *model, *effort, *tty;
};

struct agent_model {
	enum agent_kind kind;
	char *name;
	double tokens[AGENT_DAYS];
};

struct agent_limit {
	enum agent_kind kind;
	char *name;
	struct tw_claude_usage usage;
	struct claude_charts chart;
	time_t unattributed_at, unattributed_from;
	double unattributed;
	bool live_account, account_blocked;
	time_t idle_resets_at; // nothing used of it: the reset it tells (each reading a new one)
};

struct agent_usage {
	gatomicrefcount refs;
	time_t now;
	bool installed[AGENT_COUNT];
	bool copilot_auth_required;
	bool copilot_cli_missing; // only the one of VS Code, which starts a Copilot CLI not there
	struct tw_claude_window copilot_billing;
	char *copilot_bucket;
	GPtrArray *workers, *models, *limits;
};

extern const char *const agent_names[AGENT_COUNT];
/*
 * What the agents used, from their logs and running programs; with
 * account_checks also what their accounts say, each agent's own program asked
 * (codex app-server, copilot --headless, agy), at most every five minutes.
 */
struct agent_usage *agent_usage_collect(bool account_checks);
struct agent_usage *agent_usage_ref(const struct agent_usage *usage);
void agent_usage_free(struct agent_usage *usage);

#endif
