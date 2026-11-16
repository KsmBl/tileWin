#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_news.h"
#include "tw_paths.h"

void tw_news_release(const char *version, char *out, size_t size) {
	size_t n = version ? strcspn(version, "- (") : 0;
	snprintf(out, size, "%.*s", (int)n, version ? version : "");
}

char **tw_news_items(const char *path, const char *release) {
	char *own = NULL;
	if (!path) {
		if (asprintf(&own, "%s/NEWS.md", tw_data_dir()) < 0) {
			return NULL;
		}
		path = own;
	}
	FILE *f = fopen(path, "r");
	free(own);
	if (!f) {
		return NULL;
	}
	char **items = NULL;
	size_t count = 0;
	bool in = false, found = false;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, f)) >= 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (strncmp(line, "## ", 3) == 0) {
			if (in) {
				break; // the next release
			}
			const char *v = line + 3;
			in = strcmp(v, release) == 0;
			found = found || in;
			continue;
		}
		if (in && strncmp(line, "- ", 2) == 0) {
			char **grown = realloc(items, (count + 2) * sizeof(char *));
			if (!grown) {
				break;
			}
			items = grown;
			items[count++] = strdup(line + 2);
			items[count] = NULL;
		}
	}
	free(line);
	fclose(f);
	if (found && !items) {
		items = calloc(1, sizeof(char *));
	}
	return items;
}

void tw_news_free(char **items) {
	for (char **i = items; i && *i; i++) {
		free(*i);
	}
	free(items);
}

void tw_news_title(const char *item, char *out, size_t size) {
	out[0] = '\0';
	if (strncmp(item, "**", 2) != 0) {
		return;
	}
	const char *end = strstr(item + 2, "**");
	if (!end) {
		return;
	}
	int n = (int)(end - item - 2);
	// "Lists that scroll:" and "Emoji picker:" without their colon or full stop
	while (n > 0 && (item[2 + n - 1] == ':' || item[2 + n - 1] == '.')) {
		n--;
	}
	snprintf(out, size, "%.*s", n, item + 2);
}
