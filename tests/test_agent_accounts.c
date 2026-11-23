/*
 * The accounts the AI agent usage widget offers (tw_agent_accounts.h), in a
 * home of its own with no agent on $PATH, as on a computer whose agents are
 * portable: a Claude Code and a Codex found while they run, by the folders
 * their environment names, and kept once they are gone; Copilot by its folder;
 * a folder added by hand; the ids written with ~ for the home.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "tw_agent_accounts.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static bool listed(const char *id) {
	struct tw_agent_account *found;
	int n = tw_agent_accounts_found(&found);
	bool there = false;
	for (int i = 0; i < n; i++) {
		there = there || strcmp(found[i].id, id) == 0;
	}
	tw_agent_accounts_free(found, n);
	return there;
}

static void run(const char *cmd) {
	if (system(cmd) != 0) {
		fprintf(stderr, "failed: %s\n", cmd);
	}
}

/* A program called as an agent (a copy of sleep by its name), its folder in its environment. */
static pid_t start_agent(const char *program, const char *variable, const char *dir) {
	pid_t pid = fork();
	if (pid == 0) {
		setenv(variable, dir, 1);
		execl(program, program, "60", (char *)NULL);
		_exit(127);
	}
	return pid;
}

int main(void) {
	char root[] = "/tmp/tw-agent-accounts-XXXXXX";
	if (!mkdtemp(root)) {
		return 1;
	}
	char *cmd;
	if (asprintf(&cmd, "mkdir -p %s/home/bin %s/state %s/home/Portable/profiles/default/"
			"claude-config %s/home/Portable/codex-config/sessions && "
			"cp \"$(command -v sleep)\" %s/home/bin/claude && "
			"cp \"$(command -v sleep)\" %s/home/bin/codex", root, root, root, root, root,
			root) < 0) {
		return 1;
	}
	run(cmd);
	free(cmd);
	char home[256], bin[300], state[300];
	snprintf(home, sizeof(home), "%s/home", root);
	snprintf(bin, sizeof(bin), "%s/home/bin", root);
	snprintf(state, sizeof(state), "%s/state", root);
	setenv("HOME", home, 1);
	setenv("PATH", "/nonexistent", 1); // no agent on $PATH
	setenv("XDG_STATE_HOME", state, 1);
	setenv("XDG_CACHE_HOME", state, 1);
	unsetenv("CLAUDE_CONFIG_DIR");
	unsetenv("CODEX_HOME");
	unsetenv("COPILOT_HOME");

	check(!listed("copilot"), "Copilot listed with no program and no folder of it");

	// ---- portable agents found while they run, by the folders of their environment
	char claude_prog[400], codex_prog[400], claude_dir[400], codex_dir[400];
	snprintf(claude_prog, sizeof(claude_prog), "%s/claude", bin);
	snprintf(codex_prog, sizeof(codex_prog), "%s/codex", bin);
	snprintf(claude_dir, sizeof(claude_dir), "%s/Portable/profiles/default/claude-config", home);
	snprintf(codex_dir, sizeof(codex_dir), "%s/Portable/codex-config", home);
	pid_t claude = start_agent(claude_prog, "CLAUDE_CONFIG_DIR", claude_dir);
	pid_t codex = start_agent(codex_prog, "CODEX_HOME", codex_dir);
	struct timespec pause = { 0, 300 * 1000 * 1000 };
	nanosleep(&pause, NULL);
	check(listed("claude:~/Portable/profiles/default/claude-config"),
		"a running portable Claude Code is not found by its CLAUDE_CONFIG_DIR");
	check(listed("codex:~/Portable/codex-config"),
		"a running portable Codex is not found by its CODEX_HOME");
	char *program = tw_agent_command(TW_AGENT_CODEX);
	check(program && strcmp(program, codex_prog) == 0,
		"the program of a running Codex is not known to ask its account");
	free(program);
	kill(claude, SIGTERM);
	kill(codex, SIGTERM);
	waitpid(claude, NULL, 0);
	waitpid(codex, NULL, 0);
	check(listed("claude:~/Portable/profiles/default/claude-config") &&
		listed("codex:~/Portable/codex-config"), "the accounts are gone once their agents stop");

	// ---- Copilot by its folder
	if (asprintf(&cmd, "/usr/bin/mkdir -p %s/.copilot", home) >= 0) {
		run(cmd);
		free(cmd);
	}
	check(listed("copilot"), "Copilot is not listed with its folder ~/.copilot");

	// ---- a folder added by hand, told Codex's from Claude Code's
	char other[400];
	snprintf(other, sizeof(other), "%s/elsewhere/.claude", root);
	if (asprintf(&cmd, "/usr/bin/mkdir -p %s", other) >= 0) {
		run(cmd);
		free(cmd);
	}
	check(!tw_agent_folder_is_codex(other), "a Claude Code folder is taken for Codex's");
	check(tw_agent_folder_is_codex(codex_dir), "a Codex folder is not told by its sessions");
	char id[400];
	tw_agent_account_id(TW_AGENT_CLAUDE, other, id, sizeof(id));
	check(strncmp(id, "claude:/", 8) == 0, "a folder outside the home is not kept as it is");
	tw_agent_accounts_remember(id);
	check(listed(id), "a folder added by hand is not listed");
	struct tw_agent_account a;
	check(tw_agent_account_parse(id, &a) && a.agent == TW_AGENT_CLAUDE && a.dir &&
		strcmp(a.dir, other) == 0, "the id of a folder does not lead back to it");
	tw_agent_account_finish(&a);

	if (asprintf(&cmd, "/usr/bin/rm -rf %s", root) >= 0) {
		run(cmd);
		free(cmd);
	}
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
