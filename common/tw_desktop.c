#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "stringop.h"
#include "tw_desktop.h"

static list_t *application_dirs(void) {
	list_t *dirs = create_list();
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (data_home && *data_home) {
		list_add(dirs, format_str("%s/applications", data_home));
	} else if (home) {
		list_add(dirs, format_str("%s/.local/share/applications", home));
	}
	if (home) {
		list_add(dirs, format_str("%s/.local/share/flatpak/exports/share/applications", home));
	}
	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (!data_dirs || !*data_dirs) {
		data_dirs = "/usr/local/share:/usr/share";
	}
	char *copy = strdup(data_dirs);
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		list_add(dirs, format_str("%s/applications", dir));
	}
	free(copy);
	list_add(dirs, strdup("/var/lib/flatpak/exports/share/applications"));
	return dirs;
}

static char *unescape(const char *value) {
	char *out = malloc(strlen(value) + 1);
	char *o = out;
	for (const char *p = value; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			*o++ = *p == 's' ? ' ' : *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
		} else {
			*o++ = *p;
		}
	}
	*o = '\0';
	return out;
}

static bool locale_matches(const char *key_locale, int *score) {
	static char lang[32];
	static bool init = false;
	if (!init) {
		const char *env = getenv("LC_ALL");
		if (!env || !*env) {
			env = getenv("LC_MESSAGES");
		}
		if (!env || !*env) {
			env = getenv("LANG");
		}
		snprintf(lang, sizeof(lang), "%s", env ? env : "");
		char *dot = strpbrk(lang, ".@");
		if (dot) {
			*dot = '\0';
		}
		init = true;
	}
	if (!*lang) {
		return false;
	}
	if (strcmp(key_locale, lang) == 0) {
		*score = 2;
		return true;
	}
	size_t len = strcspn(lang, "_");
	if (strlen(key_locale) == len && strncmp(key_locale, lang, len) == 0) {
		*score = 1;
		return true;
	}
	return false;
}

struct tw_desktop_entry *tw_desktop_load(const char *path, const char *id) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	struct tw_desktop_entry *e = calloc(1, sizeof(*e));
	e->path = strdup(path);
	e->id = strdup(id);
	bool in_main = false, is_app = false;
	int name_score = -1, generic_score = -1, comment_score = -1;
	char *line = NULL;
	size_t size = 0;
	ssize_t n;
	while ((n = getline(&line, &size, f)) > 0) {
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = '\0';
		}
		if (line[0] == '[') {
			in_main = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		if (!in_main || line[0] == '#' || !line[0]) {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		char *kend = key + strlen(key);
		while (kend > key && isspace((unsigned char)kend[-1])) {
			*--kend = '\0';
		}
		while (isspace((unsigned char)*value)) {
			value++;
		}

		int score = 0;
		char *bracket = strchr(key, '[');
		if (bracket) {
			char *close = strchr(bracket, ']');
			if (!close) {
				continue;
			}
			*close = '\0';
			if (!locale_matches(bracket + 1, &score)) {
				continue;
			}
			*bracket = '\0';
		}

		char **target = NULL;
		int *target_score = NULL;
		if (strcmp(key, "Type") == 0) {
			is_app = strcmp(value, "Application") == 0;
		} else if (strcmp(key, "Name") == 0) {
			target = &e->name;
			target_score = &name_score;
		} else if (strcmp(key, "GenericName") == 0) {
			target = &e->generic_name;
			target_score = &generic_score;
		} else if (strcmp(key, "Comment") == 0) {
			target = &e->comment;
			target_score = &comment_score;
		} else if (strcmp(key, "Exec") == 0 && !bracket) {
			target = &e->exec;
		} else if (strcmp(key, "Icon") == 0 && !bracket) {
			target = &e->icon;
		} else if (strcmp(key, "Categories") == 0) {
			target = &e->categories;
		} else if (strcmp(key, "Keywords") == 0 && !bracket) {
			target = &e->keywords;
		} else if (strcmp(key, "StartupWMClass") == 0) {
			target = &e->startup_wm_class;
		} else if (strcmp(key, "Terminal") == 0) {
			e->terminal = strcmp(value, "true") == 0;
		} else if (strcmp(key, "NoDisplay") == 0) {
			e->no_display = strcmp(value, "true") == 0;
		} else if (strcmp(key, "Hidden") == 0) {
			e->hidden = strcmp(value, "true") == 0;
		}
		if (target) {
			if (target_score) {
				if (score <= *target_score) {
					continue;
				}
				*target_score = score;
			}
			free(*target);
			*target = unescape(value);
		}
	}
	free(line);
	fclose(f);
	if (!is_app || !e->name) {
		tw_desktop_entry_free(e);
		return NULL;
	}
	return e;
}

void tw_desktop_entry_free(struct tw_desktop_entry *e) {
	if (!e) {
		return;
	}
	free(e->id);
	free(e->path);
	free(e->name);
	free(e->generic_name);
	free(e->comment);
	free(e->exec);
	free(e->icon);
	free(e->categories);
	free(e->keywords);
	free(e->startup_wm_class);
	free(e);
}

void tw_desktop_list_free(list_t *entries) {
	if (!entries) {
		return;
	}
	for (int i = 0; i < entries->length; i++) {
		tw_desktop_entry_free(entries->items[i]);
	}
	list_free(entries);
}

static bool has_id(list_t *seen, const char *id) {
	for (int i = 0; i < seen->length; i++) {
		if (strcmp(seen->items[i], id) == 0) {
			return true;
		}
	}
	return false;
}

static void scan_dir(list_t *entries, list_t *seen, const char *dir, const char *prefix) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char *path = format_str("%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0) {
			free(path);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			char *sub_prefix = format_str("%s%s-", prefix, de->d_name);
			scan_dir(entries, seen, path, sub_prefix);
			free(sub_prefix);
		} else {
			size_t len = strlen(de->d_name);
			if (len > 8 && strcmp(de->d_name + len - 8, ".desktop") == 0) {
				char *id = format_str("%s%s", prefix, de->d_name);
				if (!has_id(seen, id)) {
					list_add(seen, strdup(id));
					struct tw_desktop_entry *e = tw_desktop_load(path, id);
					if (e) {
						list_add(entries, e);
					}
				}
				free(id);
			}
		}
		free(path);
	}
	closedir(d);
}

static int entry_cmp(const void *a, const void *b) {
	const struct tw_desktop_entry *ea = *(struct tw_desktop_entry **)a;
	const struct tw_desktop_entry *eb = *(struct tw_desktop_entry **)b;
	return strcasecmp(ea->name, eb->name);
}

list_t *tw_desktop_scan(void) {
	list_t *entries = create_list();
	list_t *seen = create_list();
	list_t *dirs = application_dirs();
	for (int i = 0; i < dirs->length; i++) {
		scan_dir(entries, seen, dirs->items[i], "");
	}
	list_free_items_and_destroy(dirs);
	list_free_items_and_destroy(seen);
	list_qsort(entries, entry_cmp);
	return entries;
}

struct tw_desktop_entry *tw_desktop_find_for_app_id(const char *app_id) {
	if (!app_id || !*app_id) {
		return NULL;
	}
	list_t *dirs = application_dirs();
	struct tw_desktop_entry *found = NULL;

	char *lower = strdup(app_id);
	for (char *p = lower; *p; p++) {
		*p = tolower((unsigned char)*p);
	}
	const char *candidates[] = { app_id, lower };
	for (size_t c = 0; c < 2 && !found; c++) {
		for (int i = 0; i < dirs->length && !found; i++) {
			char *path = format_str("%s/%s.desktop", (char *)dirs->items[i], candidates[c]);
			char *id = format_str("%s.desktop", candidates[c]);
			found = tw_desktop_load(path, id);
			free(id);
			free(path);
		}
	}

	if (!found) {
		// slow path: match StartupWMClass or a reverse-DNS suffix
		list_t *all = tw_desktop_scan();
		for (int i = 0; i < all->length; i++) {
			struct tw_desktop_entry *e = all->items[i];
			bool match = e->startup_wm_class &&
				strcasecmp(e->startup_wm_class, app_id) == 0;
			if (!match) {
				size_t idlen = strlen(e->id) - 8; // strip .desktop
				size_t alen = strlen(lower);
				if (idlen > alen && e->id[idlen - alen - 1] == '.' &&
						strncasecmp(e->id + idlen - alen, lower, alen) == 0) {
					match = true;
				}
			}
			if (match) {
				found = e;
				all->items[i] = NULL;
				break;
			}
		}
		for (int i = 0; i < all->length; i++) {
			tw_desktop_entry_free(all->items[i]);
		}
		list_free(all);
	}
	free(lower);
	list_free_items_and_destroy(dirs);
	return found;
}

char *tw_desktop_exec_command(const struct tw_desktop_entry *entry) {
	if (!entry->exec) {
		return NULL;
	}
	size_t len = strlen(entry->exec);
	char *out = calloc(1, len + 1);
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		if (entry->exec[i] == '%' && i + 1 < len) {
			char code = entry->exec[++i];
			if (code == '%') {
				out[o++] = '%';
			}
			// all other field codes (%f %u %i %c %k ...) are dropped
			continue;
		}
		out[o++] = entry->exec[i];
	}
	out[o] = '\0';
	// trim trailing spaces left by removed field codes
	while (o > 0 && out[o - 1] == ' ') {
		out[--o] = '\0';
	}
	return out;
}
