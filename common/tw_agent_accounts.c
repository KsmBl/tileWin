#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "list.h"
#include "tw_agent_accounts.h"
#include "tw_claude_usage.h"
#include "tw_codex_usage.h"

const char *const tw_agent_names[TW_AGENT_COUNT] = {
	"Claude Code", "Codex", "Copilot", "Antigravity",
};

static const char *const prefixes[TW_AGENT_COUNT] = { "claude", "codex", "copilot", "antigravity" };

bool tw_agent_command_installed(const char *command) {
	const char *path = getenv("PATH");
	if (!path || !command) {
		return false;
	}
	char *copy = strdup(path), *save = NULL;
	bool found = false;
	for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		char *file;
		if (asprintf(&file, "%s/%s", *dir ? dir : ".", command) >= 0) {
			found = access(file, X_OK) == 0;
			free(file);
		}
	}
	free(copy);
	return found;
}

void tw_agent_account_id(enum tw_agent agent, const char *dir, char *out, size_t size) {
	if (agent >= TW_AGENT_COUNT) {
		snprintf(out, size, "%s", "");
		return;
	}
	if (!dir || (agent != TW_AGENT_CLAUDE && agent != TW_AGENT_CODEX)) {
		snprintf(out, size, "%s", prefixes[agent]);
		return;
	}
	const char *home = getenv("HOME");
	size_t hl = home ? strlen(home) : 0;
	if (hl > 1 && strncmp(dir, home, hl) == 0 && (dir[hl] == '/' || !dir[hl])) {
		snprintf(out, size, "%s:~%s", prefixes[agent], dir + hl);
	} else {
		snprintf(out, size, "%s:%s", prefixes[agent], dir);
	}
}

bool tw_agent_account_parse(const char *id, struct tw_agent_account *out) {
	*out = (struct tw_agent_account){ 0 };
	if (!id || !*id) {
		return false;
	}
	for (int a = 0; a < TW_AGENT_COUNT; a++) {
		size_t len = strlen(prefixes[a]);
		if (strncmp(id, prefixes[a], len) != 0 || (id[len] && id[len] != ':')) {
			continue;
		}
		out->agent = a;
		const char *dir = id[len] == ':' ? id + len + 1 : NULL;
		if (a == TW_AGENT_CLAUDE || a == TW_AGENT_CODEX) {
			// a folder of its own, else the installed one
			out->dir = dir && *dir ? tw_claude_normalize_dir(dir) :
				a == TW_AGENT_CLAUDE ? tw_claude_default_dir() : tw_codex_default_dir();
			if (!out->dir) {
				return false;
			}
		}
		tw_agent_account_id(a, out->dir, out->id, sizeof(out->id));
		return true;
	}
	return false;
}

void tw_agent_account_finish(struct tw_agent_account *account) {
	free(account->dir);
	account->dir = NULL;
}

static void add(struct tw_agent_account **all, int *count, enum tw_agent agent, const char *dir) {
	char id[300];
	tw_agent_account_id(agent, dir, id, sizeof(id));
	for (int i = 0; i < *count; i++) {
		if (strcmp((*all)[i].id, id) == 0) {
			return;
		}
	}
	struct tw_agent_account *grown = realloc(*all, (*count + 1) * sizeof(**all));
	if (!grown) {
		return;
	}
	*all = grown;
	struct tw_agent_account *a = &grown[(*count)++];
	*a = (struct tw_agent_account){ .agent = agent, .dir = dir ? strdup(dir) : NULL };
	snprintf(a->id, sizeof(a->id), "%s", id);
}

int tw_agent_accounts_found(struct tw_agent_account **out) {
	struct tw_agent_account *all = NULL;
	int count = 0;
	list_t *claude = tw_claude_usage_all();
	for (int i = 0; i < claude->length; i++) {
		struct tw_claude_usage *u = claude->items[i];
		if (u->dir) {
			add(&all, &count, TW_AGENT_CLAUDE, u->dir);
		}
	}
	tw_claude_usage_list_free(claude);
	// Codex: its folder when there is one (tw_codex_usage_all would read its logs)
	char *codex = tw_codex_default_dir();
	struct stat st;
	if (codex && stat(codex, &st) == 0 && S_ISDIR(st.st_mode)) {
		add(&all, &count, TW_AGENT_CODEX, codex);
	}
	free(codex);
	if (tw_agent_command_installed("copilot")) {
		add(&all, &count, TW_AGENT_COPILOT, NULL);
	}
	if (tw_agent_command_installed("agy")) {
		add(&all, &count, TW_AGENT_ANTIGRAVITY, NULL);
	}
	*out = all;
	return count;
}

void tw_agent_accounts_free(struct tw_agent_account *accounts, int count) {
	for (int i = 0; i < count; i++) {
		tw_agent_account_finish(&accounts[i]);
	}
	free(accounts);
}
