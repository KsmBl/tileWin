#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "list.h"
#include "tw_agent_accounts.h"
#include "tw_claude_usage.h"
#include "tw_codex_usage.h"
#include "tw_paths.h"

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

/* ---------- kept from before ---------- */

static char *known_path(void) {
	char *state = tw_state_dir();
	char *path = NULL;
	if (state && asprintf(&path, "%s/agent-accounts", state) < 0) {
		path = NULL;
	}
	free(state);
	return path;
}

/* The lines of the file: an account id each, or "command <agent> <path>". */
static char **known_lines(int *count) {
	*count = 0;
	char *path = known_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	char **lines = NULL, *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while (f && (len = getline(&line, &cap, f)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		char **grown = len > 0 ? realloc(lines, (*count + 1) * sizeof(*lines)) : NULL;
		if (grown) {
			lines = grown;
			lines[(*count)++] = strdup(line);
		}
	}
	free(line);
	if (f) {
		fclose(f);
	}
	return lines;
}

static void free_lines(char **lines, int count) {
	for (int i = 0; i < count; i++) {
		free(lines[i]);
	}
	free(lines);
}

/* Adds a line to the file unless it has it already (a command line: replaced). */
static void keep_line(const char *keep, const char *replaces_prefix) {
	int count;
	char **lines = known_lines(&count);
	for (int i = 0; i < count; i++) {
		if (strcmp(lines[i], keep) == 0) {
			free_lines(lines, count);
			return;
		}
	}
	char *path = known_path();
	char *tmp = NULL;
	if (!path || asprintf(&tmp, "%s.new", path) < 0) {
		free(path);
		free_lines(lines, count);
		return;
	}
	char *dir = strdup(path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0700);
	}
	free(dir);
	FILE *f = fopen(tmp, "w");
	if (f) {
		for (int i = 0; i < count; i++) {
			if (!replaces_prefix || strncmp(lines[i], replaces_prefix, strlen(replaces_prefix)) != 0) {
				fprintf(f, "%s\n", lines[i]);
			}
		}
		fprintf(f, "%s\n", keep);
		if (fclose(f) == 0) {
			rename(tmp, path);
		}
	}
	free(tmp);
	free(path);
	free_lines(lines, count);
}

void tw_agent_accounts_remember(const char *id) {
	struct tw_agent_account a;
	if (tw_agent_account_parse(id, &a)) {
		keep_line(a.id, NULL);
		tw_agent_account_finish(&a);
	}
}

bool tw_agent_folder_is_codex(const char *dir) {
	const char *marks[] = { "sessions", "config.toml", "auth.json" };
	for (size_t i = 0; dir && i < sizeof(marks) / sizeof(marks[0]); i++) {
		char *path;
		if (asprintf(&path, "%s/%s", dir, marks[i]) >= 0) {
			bool there = access(path, F_OK) == 0;
			free(path);
			if (there) {
				return true;
			}
		}
	}
	return false;
}

/* ---------- agents running now ---------- */

static char *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);
	size_t got;
	while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
		n += got;
		if (n == cap) {
			char *grown = cap < (1 << 20) ? realloc(buf, cap * 2) : NULL;
			if (!grown) {
				break;
			}
			buf = grown;
			cap *= 2;
		}
	}
	fclose(f);
	*len = n;
	return buf;
}

/* The value of a variable in a NUL-separated environment. */
static const char *env_value(const char *env, size_t len, const char *name) {
	size_t nl = strlen(name);
	for (size_t i = 0; i < len; i += strlen(env + i) + 1) {
		if (strncmp(env + i, name, nl) == 0 && env[i + nl] == '=') {
			return env + i + nl + 1;
		}
	}
	return NULL;
}

/* Which agent a command line runs, by its words: -1 for none. */
static int agent_of(const char *cmd, size_t len) {
	for (size_t i = 0; i < len; i += strlen(cmd + i) + 1) {
		const char *word = cmd + i, *base = strrchr(word, '/');
		base = base ? base + 1 : word;
		if (strstr(word, "@anthropic-ai/claude-code") || strcmp(base, "claude") == 0) {
			return TW_AGENT_CLAUDE;
		}
		if (strstr(word, "@openai/codex") || strcmp(base, "codex") == 0) {
			return TW_AGENT_CODEX;
		}
		if (strstr(word, "@github/copilot") || strcmp(base, "copilot") == 0) {
			return TW_AGENT_COPILOT;
		}
		if (strcmp(base, "agy") == 0 || strstr(word, "antigravity-cli")) {
			return TW_AGENT_ANTIGRAVITY;
		}
		if (i > 0) {
			break; // the program and the script it runs (node cli.js), not its arguments
		}
	}
	return -1;
}

static void add(struct tw_agent_account **all, int *count, enum tw_agent agent, const char *dir);

/*
 * The agents running now, of this user: their accounts, kept for later, and
 * where their programs are.
 */
static void running_agents(struct tw_agent_account **all, int *count) {
	// the tests: what runs on the computer they run on is not theirs
	const char *off = getenv("TILEWIN_NO_AGENT_SCAN");
	if (off && *off && strcmp(off, "0") != 0) {
		return;
	}
	DIR *proc = opendir("/proc");
	struct dirent *de;
	uid_t me = getuid();
	while (proc && (de = readdir(proc))) {
		if (de->d_name[0] < '1' || de->d_name[0] > '9') {
			continue;
		}
		char path[64];
		struct stat st;
		snprintf(path, sizeof(path), "/proc/%s", de->d_name);
		if (stat(path, &st) != 0 || st.st_uid != me) {
			continue;
		}
		size_t clen = 0, elen = 0;
		snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
		char *cmd = read_file(path, &clen);
		int agent = cmd ? agent_of(cmd, clen) : -1;
		if (agent < 0) {
			free(cmd);
			continue;
		}
		snprintf(path, sizeof(path), "/proc/%s/environ", de->d_name);
		char *env = read_file(path, &elen);
		const char *home = env ? env_value(env, elen, "HOME") : NULL;
		char *dir = NULL;
		if (agent == TW_AGENT_CLAUDE || agent == TW_AGENT_CODEX) {
			const char *own = env ? env_value(env, elen, agent == TW_AGENT_CLAUDE ?
				"CLAUDE_CONFIG_DIR" : "CODEX_HOME") : NULL;
			if (own && *own) {
				dir = tw_claude_normalize_dir(own);
			} else if (home && *home) {
				char *in_home;
				if (asprintf(&in_home, "%s/%s", home, agent == TW_AGENT_CLAUDE ? ".claude" :
						".codex") >= 0) {
					dir = tw_claude_normalize_dir(in_home);
					free(in_home);
				}
			}
		}
		if (dir || (agent != TW_AGENT_CLAUDE && agent != TW_AGENT_CODEX)) {
			add(all, count, agent, dir);
			char id[300];
			tw_agent_account_id(agent, dir, id, sizeof(id));
			keep_line(id, NULL);
		}
		// where its program is, for asking its account (codex, copilot, agy)
		const char *first = cmd;
		const char *base = strrchr(first, '/');
		if (agent != TW_AGENT_CLAUDE && base && strcmp(base + 1, prefixes[agent]) == 0 &&
				access(first, X_OK) == 0 && first[0] == '/') {
			char line[400], prefix[64];
			snprintf(prefix, sizeof(prefix), "command %s ", prefixes[agent]);
			snprintf(line, sizeof(line), "%s%s", prefix, first);
			keep_line(line, prefix);
		}
		free(dir);
		free(env);
		free(cmd);
	}
	if (proc) {
		closedir(proc);
	}
}

char *tw_agent_command(enum tw_agent agent) {
	static const char *const names[TW_AGENT_COUNT] = { "claude", "codex", "copilot", "agy" };
	if (agent >= TW_AGENT_COUNT) {
		return NULL;
	}
	const char *path = getenv("PATH");
	char *copy = path ? strdup(path) : NULL, *save = NULL;
	for (char *dir = copy ? strtok_r(copy, ":", &save) : NULL; dir;
			dir = strtok_r(NULL, ":", &save)) {
		char *file;
		if (asprintf(&file, "%s/%s", *dir ? dir : ".", names[agent]) >= 0) {
			if (access(file, X_OK) == 0) {
				free(copy);
				return file;
			}
			free(file);
		}
	}
	free(copy);
	// where it was found running before
	int count;
	char **lines = known_lines(&count);
	char prefix[64];
	snprintf(prefix, sizeof(prefix), "command %s ", prefixes[agent]);
	char *found = NULL;
	for (int i = 0; i < count && !found; i++) {
		if (strncmp(lines[i], prefix, strlen(prefix)) == 0 &&
				access(lines[i] + strlen(prefix), X_OK) == 0) {
			found = strdup(lines[i] + strlen(prefix));
		}
	}
	free_lines(lines, count);
	// Copilot comes with VS Code
	const char *shims[] = { "/usr/share/code/bin/copilot-shim/copilot",
		"/opt/visual-studio-code/bin/copilot-shim/copilot", "/usr/lib/code/bin/copilot-shim/copilot" };
	for (size_t i = 0; !found && agent == TW_AGENT_COPILOT && i < sizeof(shims) / sizeof(shims[0]); i++) {
		if (access(shims[i], X_OK) == 0) {
			found = strdup(shims[i]);
		}
	}
	return found;
}

/* A folder of the home that is there: "~/.copilot". */
static bool home_has(const char *sub) {
	const char *home = getenv("HOME");
	char *path;
	if (!home || asprintf(&path, "%s/%s", home, sub) < 0) {
		return false;
	}
	struct stat st;
	bool there = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
	free(path);
	return there;
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
	char *copilot = tw_agent_command(TW_AGENT_COPILOT);
	const char *copilot_home = getenv("COPILOT_HOME");
	if (copilot || home_has(".copilot") || (copilot_home && *copilot_home)) {
		add(&all, &count, TW_AGENT_COPILOT, NULL);
	}
	free(copilot);
	char *agy = tw_agent_command(TW_AGENT_ANTIGRAVITY);
	if (agy || home_has(".gemini/antigravity-cli")) {
		add(&all, &count, TW_AGENT_ANTIGRAVITY, NULL);
	}
	free(agy);
	// the ones running now (portable ones too), and those found or added before
	running_agents(&all, &count);
	int kept;
	char **lines = known_lines(&kept);
	for (int i = 0; i < kept; i++) {
		struct tw_agent_account a;
		if (strncmp(lines[i], "command ", 8) != 0 && tw_agent_account_parse(lines[i], &a)) {
			add(&all, &count, a.agent, a.dir);
			tw_agent_account_finish(&a);
		}
	}
	free_lines(lines, kept);
	*out = all;
	return count;
}

void tw_agent_accounts_free(struct tw_agent_account *accounts, int count) {
	for (int i = 0; i < count; i++) {
		tw_agent_account_finish(&accounts[i]);
	}
	free(accounts);
}
