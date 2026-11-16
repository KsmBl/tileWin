#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_paths.h"
#include "tw_portrules.h"

static bool same(const char *a, const char *b) {
	return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) {
		s++;
	}
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[--n] = '\0';
	}
	return s;
}

static void add(struct tw_port_rules *r, const char *program, const char *network, bool allow) {
	if (r->count == r->cap) {
		int cap = r->cap ? r->cap * 2 : 16;
		struct tw_port_rule *items = realloc(r->items, cap * sizeof(*items));
		if (!items) {
			return;
		}
		r->items = items;
		r->cap = cap;
	}
	struct tw_port_rule *rule = &r->items[r->count++];
	rule->program = strdup(program);
	rule->network = network && *network ? strdup(network) : NULL;
	rule->allow = allow;
}

void tw_port_rules_name_network(struct tw_port_rules *r, const char *uuid, const char *name) {
	if (!uuid || !*uuid || !name) {
		return;
	}
	for (int i = 0; i < r->network_count; i++) {
		if (strcmp(r->networks[i].uuid, uuid) == 0) {
			free(r->networks[i].name);
			r->networks[i].name = strdup(name);
			return;
		}
	}
	if (r->network_count == r->network_cap) {
		int cap = r->network_cap ? r->network_cap * 2 : 4;
		struct tw_port_network *n = realloc(r->networks, cap * sizeof(*n));
		if (!n) {
			return;
		}
		r->networks = n;
		r->network_cap = cap;
	}
	r->networks[r->network_count].uuid = strdup(uuid);
	r->networks[r->network_count].name = strdup(name);
	r->network_count++;
}

const char *tw_port_rules_network_name(const struct tw_port_rules *r, const char *uuid) {
	for (int i = 0; uuid && i < r->network_count; i++) {
		if (strcmp(r->networks[i].uuid, uuid) == 0) {
			return r->networks[i].name;
		}
	}
	return NULL;
}

bool tw_port_rules_read(struct tw_port_rules *r, const char *path) {
	tw_port_rules_free(r);
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, f) >= 0) {
		char *s = trim(line);
		if (*s == '#' || !*s) {
			continue;
		}
		if (strncmp(s, "guard ", 6) == 0) {
			char *v = trim(s + 6);
			r->guard = strcmp(v, "yes") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0;
		} else if (strncmp(s, "network ", 8) == 0) {
			char *uuid = trim(s + 8);
			char *space = strpbrk(uuid, " \t");
			if (space) {
				*space = '\0';
				tw_port_rules_name_network(r, uuid, trim(space + 1));
			}
		} else if (strncmp(s, "allow", 5) == 0 || strncmp(s, "block", 5) == 0) {
			bool allow = s[0] == 'a';
			char *rest = s + 5, *network = NULL;
			if (*rest == '@') {
				network = rest + 1;
				rest = strpbrk(network, " \t");
				if (!rest) {
					continue;
				}
				*rest++ = '\0';
				if (!*network) {
					continue; // "block@ x": a network without a name
				}
			} else if (*rest != ' ' && *rest != '\t') {
				continue;
			}
			char *program = trim(rest);
			if (*program) {
				tw_port_rules_set(r, program, network, allow);
			}
		}
	}
	free(line);
	fclose(f);
	return true;
}

bool tw_port_rules_write(const struct tw_port_rules *r, const char *path) {
	if (!path) {
		return false;
	}
	char *dir = strdup(path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		tw_mkdir_p(dir);
	}
	free(dir);
	char *tmp = NULL;
	if (asprintf(&tmp, "%s.new", path) < 0) {
		return false;
	}
	FILE *f = fopen(tmp, "w");
	if (!f) {
		free(tmp);
		return false;
	}
	fprintf(f, "# Which programs other computers may reach: tileWin asks before one opens a\n"
		"# port to the network (guard yes, the switch of the Network page) and keeps\n"
		"# the answer for the program, everywhere or with @ on one network only.\n");
	fprintf(f, "guard %s\n", r->guard ? "yes" : "no");
	for (int i = 0; i < r->network_count; i++) {
		fprintf(f, "network %s %s\n", r->networks[i].uuid, r->networks[i].name);
	}
	for (int i = 0; i < r->count; i++) {
		const struct tw_port_rule *rule = &r->items[i];
		fprintf(f, "%s%s%s %s\n", rule->allow ? "allow" : "block", rule->network ? "@" : "",
			rule->network ? rule->network : "", rule->program);
	}
	bool ok = fflush(f) == 0;
	ok = fclose(f) == 0 && ok;
	ok = ok && rename(tmp, path) == 0;
	if (!ok) {
		remove(tmp);
	}
	free(tmp);
	return ok;
}

void tw_port_rules_free(struct tw_port_rules *r) {
	for (int i = 0; i < r->count; i++) {
		free(r->items[i].program);
		free(r->items[i].network);
	}
	for (int i = 0; i < r->network_count; i++) {
		free(r->networks[i].uuid);
		free(r->networks[i].name);
	}
	free(r->items);
	free(r->networks);
	memset(r, 0, sizeof(*r));
}

const struct tw_port_rule *tw_port_rules_find(const struct tw_port_rules *r,
		const char *program, const char *network) {
	const struct tw_port_rule *everywhere = NULL;
	for (int i = 0; program && i < r->count; i++) {
		const struct tw_port_rule *rule = &r->items[i];
		if (strcmp(rule->program, program) != 0) {
			continue;
		}
		if (rule->network && network && strcmp(rule->network, network) == 0) {
			return rule;
		}
		if (!rule->network) {
			everywhere = rule;
		}
	}
	return everywhere;
}

void tw_port_rules_set(struct tw_port_rules *r, const char *program, const char *network,
		bool allow) {
	if (network && !*network) {
		network = NULL;
	}
	for (int i = 0; i < r->count; i++) {
		struct tw_port_rule *rule = &r->items[i];
		if (strcmp(rule->program, program) == 0 && same(rule->network, network)) {
			rule->allow = allow;
			return;
		}
	}
	add(r, program, network, allow);
}

void tw_port_rules_forget(struct tw_port_rules *r, const char *program, const char *network) {
	if (network && !*network) {
		network = NULL;
	}
	for (int i = 0; i < r->count; i++) {
		struct tw_port_rule *rule = &r->items[i];
		if (strcmp(rule->program, program) == 0 && same(rule->network, network)) {
			free(rule->program);
			free(rule->network);
			memmove(rule, rule + 1, (r->count - i - 1) * sizeof(*rule));
			r->count--;
			return;
		}
	}
}

char *tw_port_rules_path(void) {
	char *dir = tw_config_dir();
	char *path = NULL;
	if (dir && asprintf(&path, "%s/ports.conf", dir) < 0) {
		path = NULL;
	}
	free(dir);
	return path;
}
