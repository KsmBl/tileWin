#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "domainset.h"
#include "wire.h"

static uint32_t hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	return h;
}

void domainset_init(struct domainset *set) {
	memset(set, 0, sizeof(*set));
}

void domainset_finish(struct domainset *set) {
	free(set->arena);
	free(set->slots);
	memset(set, 0, sizeof(*set));
}

static bool grow_slots(struct domainset *set) {
	size_t n = set->nslots ? set->nslots * 2 : 1024;
	uint32_t *slots = calloc(n, sizeof(*slots));
	if (!slots) {
		return false;
	}
	for (size_t i = 0; i < set->nslots; i++) {
		uint32_t at = set->slots[i];
		if (!at) {
			continue;
		}
		size_t j = hash(set->arena + at - 1) & (n - 1);
		while (slots[j]) {
			j = (j + 1) & (n - 1);
		}
		slots[j] = at;
	}
	free(set->slots);
	set->slots = slots;
	set->nslots = n;
	return true;
}

bool domainset_has(const struct domainset *set, const char *name) {
	if (!set->nslots) {
		return false;
	}
	size_t j = hash(name) & (set->nslots - 1);
	while (set->slots[j]) {
		if (strcmp(set->arena + set->slots[j] - 1, name) == 0) {
			return true;
		}
		j = (j + 1) & (set->nslots - 1);
	}
	return false;
}

bool domainset_add(struct domainset *set, const char *name) {
	if ((set->count + 1) * 10 > set->nslots * 6 && !grow_slots(set)) {
		return false;
	}
	size_t j = hash(name) & (set->nslots - 1);
	while (set->slots[j]) {
		if (strcmp(set->arena + set->slots[j] - 1, name) == 0) {
			return false;
		}
		j = (j + 1) & (set->nslots - 1);
	}
	size_t len = strlen(name) + 1;
	if (set->used + len > set->size) {
		size_t size = set->size ? set->size * 2 : 64 * 1024;
		while (size < set->used + len) {
			size *= 2;
		}
		if (size > UINT32_MAX) {
			return false;
		}
		char *arena = realloc(set->arena, size);
		if (!arena) {
			return false;
		}
		set->arena = arena;
		set->size = size;
	}
	memcpy(set->arena + set->used, name, len);
	set->slots[j] = (uint32_t)set->used + 1;
	set->used += len;
	set->count++;
	return true;
}

bool domainset_add_rule(struct domainset *set, const char *name, bool below_too) {
	char key[DNS_NAME_MAX + 1];
	size_t len = strlen(name);
	if (len == 0 || len >= DNS_NAME_MAX) {
		return false;
	}
	key[0] = below_too ? '*' : '=';
	memcpy(key + 1, name, len + 1);
	return domainset_add(set, key);
}

bool domainset_matches(const struct domainset *set, const char *name) {
	if (set->count == 0) {
		return false;
	}
	char key[DNS_NAME_MAX + 1];
	size_t len = strlen(name);
	if (len >= DNS_NAME_MAX) {
		return false;
	}
	key[0] = '=';
	memcpy(key + 1, name, len + 1);
	if (domainset_has(set, key)) {
		return true;
	}
	for (const char *p = name; p; ) {
		key[0] = '*';
		memmove(key + 1, p, strlen(p) + 1);
		if (domainset_has(set, key)) {
			return true;
		}
		p = strchr(p, '.');
		if (p) {
			p++;
		}
	}
	return false;
}

/* A name as the lists write it, made into the form of the service; NULL if it is none. */
static char *clean_name(char *s) {
	size_t len = strlen(s);
	while (len > 0 && s[len - 1] == '.') {
		s[--len] = '\0';
	}
	if (len == 0 || len > 253) {
		return NULL;
	}
	bool dot = false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		s[i] = (char)tolower(c);
		if (c == '.') {
			if (i == 0 || s[i - 1] == '.') {
				return NULL;
			}
			dot = true;
		} else if (!isalnum(c) && c != '-' && c != '_') {
			return NULL;
		}
	}
	return dot ? s : NULL; // a single word is a host of the list's own network
}

int domainset_add_list_line(struct domainset *set, char *line) {
	char *hash_at = strchr(line, '#');
	if (hash_at) {
		*hash_at = '\0';
	}
	char *p = line;
	while (isspace((unsigned char)*p)) {
		p++;
	}
	if (*p == '\0' || *p == '!' || *p == '[') {
		return 0; // empty, or a comment or header of an adblock list
	}
	if (p[0] == '|' && p[1] == '|') {
		// adblock: ||ads.example.com^ with no path and no options that limit it
		p += 2;
		char *end = strchr(p, '^');
		if (!end || (end[1] && end[1] != '$' && !isspace((unsigned char)end[1]))) {
			return 0;
		}
		if (end[1] == '$' && strncmp(end + 1, "$important", 10) != 0) {
			return 0;
		}
		*end = '\0';
		char *name = clean_name(p);
		return name && domainset_add_rule(set, name, true) ? 1 : 0;
	}
	if (*p == '@' || *p == '/' || *p == '|') {
		return 0; // exceptions and patterns of adblock lists
	}
	int added = 0;
	char *save = NULL;
	char *first = strtok_r(p, " \t\r\n", &save);
	if (!first) {
		return 0;
	}
	char *word = first;
	bool hosts = strcmp(first, "0.0.0.0") == 0 || strcmp(first, "127.0.0.1") == 0 ||
		strcmp(first, "::") == 0 || strcmp(first, "::1") == 0;
	if (hosts) {
		word = strtok_r(NULL, " \t\r\n", &save);
	}
	for (; word; word = hosts ? strtok_r(NULL, " \t\r\n", &save) : NULL) {
		if (strcmp(word, "localhost") == 0 || strcmp(word, "localhost.localdomain") == 0 ||
				strcmp(word, "0.0.0.0") == 0 || strcmp(word, "broadcasthost") == 0) {
			continue;
		}
		char *name = clean_name(word);
		if (name && domainset_add_rule(set, name, false)) {
			added++;
		}
	}
	return added;
}
