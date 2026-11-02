#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sessions.h"

bool dm_session_id_ok(const char *id) {
	if (!id || !*id || *id == '.' || strlen(id) > 128) {
		return false;
	}
	for (const char *p = id; *p; p++) {
		if (!isalnum((unsigned char)*p) && !strchr("._+-", *p)) {
			return false;
		}
	}
	return true;
}

/* Is the program of a command there? Sessions whose program is gone are left out. */
static bool program_exists(const char *exec) {
	char program[512];
	size_t len = strcspn(exec, " \t");
	if (len == 0 || len >= sizeof(program)) {
		return false;
	}
	memcpy(program, exec, len);
	program[len] = '\0';
	if (strchr(program, '/')) {
		return access(program, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	if (!path || !*path) {
		path = "/usr/local/bin:/usr/bin:/bin";
	}
	char *copy = strdup(path), *save = NULL;
	bool found = false;
	for (char *dir = strtok_r(copy, ":", &save); dir && !found;
			dir = strtok_r(NULL, ":", &save)) {
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", dir, program);
		found = access(full, X_OK) == 0;
	}
	free(copy);
	return found;
}

static void read_file(struct dm_session_list *list, const char *dir, const char *file) {
	size_t len = strlen(file);
	if (len <= 8 || strcmp(file + len - 8, ".desktop") != 0) {
		return;
	}
	char id[256];
	if (len - 8 >= sizeof(id)) {
		return;
	}
	memcpy(id, file, len - 8);
	id[len - 8] = '\0';
	if (!dm_session_id_ok(id) || dm_session_find(list, id)) {
		return; // a folder that comes earlier has it already
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[2048], *name = NULL, *exec = NULL, *desktop = NULL;
	bool in_entry = false, hidden = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_entry = strcmp(line, "[Desktop Entry]") == 0;
		} else if (!in_entry) {
			continue;
		} else if (strncmp(line, "Name=", 5) == 0 && !name) {
			name = strdup(line + 5);
		} else if (strncmp(line, "Exec=", 5) == 0 && !exec) {
			exec = strdup(line + 5);
		} else if (strncmp(line, "DesktopNames=", 13) == 0 && !desktop) {
			desktop = strdup(line + 13);
			desktop[strcspn(desktop, ";")] = '\0';
		} else if (strcmp(line, "Hidden=true") == 0 || strcmp(line, "NoDisplay=true") == 0) {
			hidden = true;
		}
	}
	fclose(f);
	if (hidden || !name || !exec || !program_exists(exec)) {
		free(name);
		free(exec);
		free(desktop);
		return;
	}
	list->items = realloc(list->items, (list->count + 1) * sizeof(*list->items));
	struct dm_session *s = &list->items[list->count++];
	s->id = strdup(id);
	s->name = name;
	s->exec = exec;
	s->desktop = desktop;
}

static int session_cmp(const void *a, const void *b) {
	const struct dm_session *x = a, *y = b;
	return strcasecmp(x->name, y->name);
}

struct dm_session_list *dm_sessions_read(void) {
	struct dm_session_list *list = calloc(1, sizeof(*list));
	const char *only = getenv("TILEWIN_DM_SESSIONS");
	const char *dirs[] = { only && *only ? only : "/usr/local/share/wayland-sessions",
		only && *only ? NULL : "/usr/share/wayland-sessions", NULL };
	for (int i = 0; dirs[i]; i++) {
		DIR *d = opendir(dirs[i]);
		struct dirent *entry;
		while (d && (entry = readdir(d))) {
			read_file(list, dirs[i], entry->d_name);
		}
		if (d) {
			closedir(d);
		}
	}
	if (list->count > 1) {
		qsort(list->items, list->count, sizeof(*list->items), session_cmp);
	}
	return list;
}

void dm_sessions_free(struct dm_session_list *list) {
	if (!list) {
		return;
	}
	for (size_t i = 0; i < list->count; i++) {
		free(list->items[i].id);
		free(list->items[i].name);
		free(list->items[i].exec);
		free(list->items[i].desktop);
	}
	free(list->items);
	free(list);
}

const struct dm_session *dm_session_find(const struct dm_session_list *list, const char *id) {
	for (size_t i = 0; list && id && i < list->count; i++) {
		if (strcmp(list->items[i].id, id) == 0) {
			return &list->items[i];
		}
	}
	return NULL;
}

char *dm_session_command(const char *exec) {
	char *out = malloc(strlen(exec) + 1), *o = out;
	for (const char *p = exec; *p; p++) {
		if (*p == '%' && p[1]) {
			p++;
			if (*p == '%') {
				*o++ = '%';
			}
			continue;
		}
		*o++ = *p;
	}
	*o = '\0';
	return out;
}
