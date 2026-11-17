#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tw_command.h"
#include "tw_paths.h"

static bool starts_with(const char *s, const char *prefix) {
	return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static bool word_start(const char *s, size_t i) {
	return i == 0 || !isalnum((unsigned char)s[i - 1]) ||
		(isupper((unsigned char)s[i]) && islower((unsigned char)s[i - 1]));
}

/* The first letter of each word, "TS" for "Task Switcher" (and "TaskSwitcher"). */
static void initials(const char *s, char *out, size_t size) {
	size_t n = 0;
	for (size_t i = 0; s[i] && n + 1 < size; i++) {
		if (isalnum((unsigned char)s[i]) && word_start(s, i)) {
			out[n++] = (char)tolower((unsigned char)s[i]);
		}
	}
	out[n] = '\0';
}

static bool subsequence(const char *s, const char *q) {
	for (; *s && *q; s++) {
		if (tolower((unsigned char)*s) == tolower((unsigned char)*q)) {
			q++;
		}
	}
	return *q == '\0';
}

int tw_cmd_match(const char *name, const char *keywords, const char *query) {
	if (!query || !*query) {
		return 1;
	}
	if (!name) {
		name = "";
	}
	if (starts_with(name, query)) {
		return 100;
	}
	size_t ql = strlen(query);
	for (size_t i = 1; name[0] && name[i]; i++) {
		if (word_start(name, i) && strncasecmp(name + i, query, ql) == 0) {
			return 85;
		}
	}
	char ini[64];
	initials(name, ini, sizeof(ini));
	if (ql >= 2 && strncasecmp(ini, query, ql) == 0) {
		return 75;
	}
	if (strcasestr(name, query)) {
		return 55;
	}
	if (keywords && *keywords) {
		const char *k = keywords;
		while (*k) {
			while (*k && (isspace((unsigned char)*k) || *k == ';' || *k == ',')) {
				k++;
			}
			if (*k && strncasecmp(k, query, ql) == 0) {
				return 35;
			}
			while (*k && !isspace((unsigned char)*k) && *k != ';' && *k != ',') {
				k++;
			}
		}
	}
	// letters in order, from four on: fewer would find almost anything
	if (ql >= 4 && subsequence(name, query)) {
		return 15;
	}
	return 0;
}

void tw_cmd_history_free(struct tw_cmd_history *h) {
	for (int i = 0; i < h->count; i++) {
		free(h->items[i].key);
	}
	free(h->items);
	h->items = NULL;
	h->count = h->cap = 0;
}

static struct tw_cmd_use *find(const struct tw_cmd_history *h, const char *key) {
	for (int i = 0; i < h->count; i++) {
		if (strcmp(h->items[i].key, key) == 0) {
			return &h->items[i];
		}
	}
	return NULL;
}

static struct tw_cmd_use *add(struct tw_cmd_history *h, const char *key) {
	if (h->count == h->cap) {
		int cap = h->cap ? h->cap * 2 : 32;
		struct tw_cmd_use *items = realloc(h->items, cap * sizeof(*items));
		if (!items) {
			return NULL;
		}
		h->items = items;
		h->cap = cap;
	}
	struct tw_cmd_use *u = &h->items[h->count++];
	u->key = strdup(key);
	u->count = 0;
	u->last = 0;
	return u;
}

bool tw_cmd_history_load(struct tw_cmd_history *h, const char *path) {
	tw_cmd_history_free(h);
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, f) >= 0) {
		int count;
		long long last;
		int used = 0;
		if (sscanf(line, "%d %lld %n", &count, &last, &used) != 2 || used <= 0 || count <= 0) {
			continue;
		}
		char *key = line + used;
		key[strcspn(key, "\n")] = '\0';
		if (!*key || find(h, key)) {
			continue;
		}
		struct tw_cmd_use *u = add(h, key);
		if (u) {
			u->count = count;
			u->last = last;
		}
	}
	free(line);
	fclose(f);
	return true;
}

static int by_last(const void *a, const void *b) {
	const struct tw_cmd_use *x = a, *y = b;
	return x->last < y->last ? 1 : x->last > y->last ? -1 : 0;
}

bool tw_cmd_history_save(const struct tw_cmd_history *h, const char *path) {
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
	// the ones used last first, the oldest beyond the limit forgotten
	struct tw_cmd_use *sorted = malloc((h->count ? h->count : 1) * sizeof(*sorted));
	if (!sorted) {
		return false;
	}
	memcpy(sorted, h->items, h->count * sizeof(*sorted));
	qsort(sorted, h->count, sizeof(*sorted), by_last);
	char *tmp = NULL;
	if (asprintf(&tmp, "%s.new", path) < 0) {
		free(sorted);
		return false;
	}
	FILE *f = fopen(tmp, "w");
	bool ok = f != NULL;
	for (int i = 0; ok && i < h->count && i < TW_CMD_HISTORY_MAX; i++) {
		fprintf(f, "%d %lld %s\n", sorted[i].count, sorted[i].last, sorted[i].key);
	}
	if (f) {
		ok = fclose(f) == 0 && ok;
	}
	ok = ok && rename(tmp, path) == 0;
	if (!ok) {
		remove(tmp);
	}
	free(tmp);
	free(sorted);
	return ok;
}

void tw_cmd_history_note(struct tw_cmd_history *h, const char *key, long long now) {
	struct tw_cmd_use *u = find(h, key);
	if (!u) {
		u = add(h, key);
	}
	if (u) {
		u->count++;
		u->last = now;
	}
}

double tw_cmd_frecency(const struct tw_cmd_history *h, const char *key, long long now) {
	const struct tw_cmd_use *u = find(h, key);
	if (!u) {
		return 0;
	}
	double age = (double)(now - u->last);
	// half of it after a week: what was used lately counts more
	double recency = pow(0.5, (age < 0 ? 0 : age) / (7 * 86400.0));
	return log2(1 + u->count) * 10 * (0.4 + 0.6 * recency);
}

char *tw_cmd_history_path(void) {
	char *dir = tw_state_dir();
	char *path = NULL;
	if (dir && asprintf(&path, "%s/command-center", dir) < 0) {
		path = NULL;
	}
	free(dir);
	return path;
}
