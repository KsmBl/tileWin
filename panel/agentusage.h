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
};

struct agent_usage {
	gatomicrefcount refs;
	time_t now;
	bool installed[AGENT_COUNT];
	bool copilot_auth_required;
	struct tw_claude_window copilot_billing;
	char *copilot_bucket;
	GPtrArray *workers, *models, *limits;
};

extern const char *const agent_names[AGENT_COUNT];
struct agent_usage *agent_usage_collect(bool poll_codex);
struct agent_usage *agent_usage_ref(const struct agent_usage *usage);
void agent_usage_free(struct agent_usage *usage);
const struct agent_usage *agent_widget_usage(struct widget *w);

#endif
