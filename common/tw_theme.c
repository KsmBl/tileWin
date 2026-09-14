#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "stringop.h"
#include "tw_paths.h"
#include "tw_theme.h"
#include "twconf.h"

static int kv_cmp(const void *a, const void *b) {
	const struct tw_theme_kv *ka = *(struct tw_theme_kv **)a;
	const struct tw_theme_kv *kb = *(struct tw_theme_kv **)b;
	return strcmp(ka->key, kb->key);
}

static void kv_free(struct tw_theme_kv *kv) {
	free(kv->key);
	free(kv->value);
	free(kv);
}

static void flatten(list_t *kv, const struct twconf_node *node, const char *prefix) {
	for (int i = 0; i < twconf_count(node); i++) {
		struct twconf_node *child = twconf_at(node, i);
		if (!prefix && strcmp(child->name, "inherit") == 0) {
			continue;
		}
		char *key = prefix ? format_str("%s.%s", prefix, child->name) :
			strdup(child->name);
		if (child->argc > 0) {
			struct tw_theme_kv *entry = calloc(1, sizeof(*entry));
			entry->key = strdup(key);
			entry->value = twconf_join(child, 0);
			list_add(kv, entry);
		}
		if (child->children) {
			flatten(kv, child, key);
		}
		free(key);
	}
}

char *tw_theme_find_dir(const char *name) {
	if (!name || !*name) {
		return NULL;
	}
	if (strchr(name, '/')) {
		char *path = tw_expand_home(name);
		char *file = format_str("%s/theme.conf", path);
		bool ok = access(file, R_OK) == 0;
		free(file);
		if (ok) {
			return path;
		}
		free(path);
		return NULL;
	}
	char *rel = format_str("themes/%s/theme.conf", name);
	char *file = tw_find_data_file(rel);
	free(rel);
	if (!file) {
		return NULL;
	}
	char *slash = strrchr(file, '/');
	*slash = '\0';
	return file;
}

static bool load_into(struct tw_theme *theme, const char *name, int depth,
		char **error) {
	if (depth > 5) {
		if (error && !*error) {
			*error = strdup("theme inheritance too deep");
		}
		return false;
	}
	char *dir = tw_theme_find_dir(name);
	if (!dir) {
		if (error && !*error) {
			*error = format_str("theme '%s' not found", name);
		}
		return false;
	}
	char *file = format_str("%s/theme.conf", dir);
	struct twconf_node *root = twconf_parse_file(file, error);
	free(file);
	if (!root) {
		free(dir);
		return false;
	}

	const char *base = twconf_value(root, "inherit");
	if (base && !load_into(theme, base, depth + 1, error)) {
		twconf_free(root);
		free(dir);
		return false;
	}

	if (depth == 0) {
		theme->dir = dir;
	} else {
		free(dir);
	}
	flatten(theme->kv, root, NULL);
	twconf_free(root);
	return true;
}

struct tw_theme *tw_theme_load(const char *name, char **error) {
	struct tw_theme *theme = calloc(1, sizeof(*theme));
	theme->kv = create_list();
	if (!load_into(theme, name, 0, error)) {
		tw_theme_free(theme);
		return NULL;
	}

	// stable sort keeps definition order for equal keys: keep the last one
	list_stable_sort(theme->kv, kv_cmp);
	for (int i = theme->kv->length - 2; i >= 0; i--) {
		struct tw_theme_kv *a = theme->kv->items[i];
		struct tw_theme_kv *b = theme->kv->items[i + 1];
		if (strcmp(a->key, b->key) == 0) {
			kv_free(a);
			list_del(theme->kv, i);
		}
	}

	const char *slash = strrchr(name, '/');
	theme->name = strdup(slash ? slash + 1 : name);
	theme->title = strdup(tw_theme_str(theme, "name", theme->name));
	theme->style = strdup(tw_theme_str(theme, "style", "win10"));
	return theme;
}

void tw_theme_free(struct tw_theme *theme) {
	if (!theme) {
		return;
	}
	if (theme->kv) {
		for (int i = 0; i < theme->kv->length; i++) {
			kv_free(theme->kv->items[i]);
		}
		list_free(theme->kv);
	}
	free(theme->name);
	free(theme->dir);
	free(theme->title);
	free(theme->style);
	free(theme);
}

const char *tw_theme_str(const struct tw_theme *theme, const char *key,
		const char *fallback) {
	if (!theme || !theme->kv) {
		return fallback;
	}
	struct tw_theme_kv query = { .key = (char *)key };
	struct tw_theme_kv *qp = &query;
	struct tw_theme_kv **found = bsearch(&qp, theme->kv->items,
		theme->kv->length, sizeof(void *), kv_cmp);
	return found ? (*found)->value : fallback;
}

int tw_theme_int(const struct tw_theme *theme, const char *key, int fallback) {
	const char *value = tw_theme_str(theme, key, NULL);
	if (!value) {
		return fallback;
	}
	char *end;
	long result = strtol(value, &end, 10);
	return end == value ? fallback : (int)result;
}

double tw_theme_double(const struct tw_theme *theme, const char *key,
		double fallback) {
	const char *value = tw_theme_str(theme, key, NULL);
	if (!value) {
		return fallback;
	}
	char *end;
	double result = strtod(value, &end);
	return end == value ? fallback : result;
}

bool tw_theme_bool(const struct tw_theme *theme, const char *key, bool fallback) {
	return twconf_parse_bool(tw_theme_str(theme, key, NULL), fallback);
}

bool tw_parse_color(const char *str, uint32_t *color) {
	if (!str) {
		return false;
	}
	if (strcasecmp(str, "transparent") == 0 || strcasecmp(str, "none") == 0) {
		*color = 0;
		return true;
	}
	if (str[0] == '#') {
		str++;
	}
	size_t len = strlen(str);
	char *end;
	unsigned long value = strtoul(str, &end, 16);
	if (*end != '\0') {
		return false;
	}
	switch (len) {
	case 3:
		*color = ((value >> 8 & 0xf) * 0x11) << 24 | ((value >> 4 & 0xf) * 0x11) << 16 |
			((value & 0xf) * 0x11) << 8 | 0xff;
		return true;
	case 6:
		*color = (uint32_t)(value << 8 | 0xff);
		return true;
	case 8:
		*color = (uint32_t)value;
		return true;
	default:
		return false;
	}
}

uint32_t tw_theme_color(const struct tw_theme *theme, const char *key,
		uint32_t fallback) {
	uint32_t color;
	if (tw_parse_color(tw_theme_str(theme, key, NULL), &color)) {
		return color;
	}
	return fallback;
}

static int str_cmp(const void *a, const void *b) {
	return strcmp(*(char **)a, *(char **)b);
}

static void scan_theme_dir(list_t *names, const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d))) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char *file = format_str("%s/%s/theme.conf", dir, entry->d_name);
		bool exists = access(file, R_OK) == 0;
		free(file);
		if (!exists) {
			continue;
		}
		bool dup = false;
		for (int i = 0; i < names->length; i++) {
			if (strcmp(names->items[i], entry->d_name) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			list_add(names, strdup(entry->d_name));
		}
	}
	closedir(d);
}

list_t *tw_theme_list(void) {
	list_t *names = create_list();
	char *config_dir = tw_config_dir();
	if (config_dir) {
		char *dir = format_str("%s/themes", config_dir);
		scan_theme_dir(names, dir);
		free(dir);
		free(config_dir);
	}
	char *dir = format_str("%s/themes", tw_data_dir());
	scan_theme_dir(names, dir);
	free(dir);
	list_qsort(names, str_cmp);
	return names;
}

char *tw_theme_current_name(void) {
	char *config_dir = tw_config_dir();
	char *name = NULL;
	if (config_dir) {
		char *path = format_str("%s/current-theme", config_dir);
		name = tw_read_first_line(path);
		free(path);
		free(config_dir);
	}
	if (!name || !*name) {
		free(name);
		name = strdup(TW_DEFAULT_THEME);
	}
	return name;
}

bool tw_theme_save_current(const char *name) {
	char *config_dir = tw_config_dir();
	if (!config_dir) {
		return false;
	}
	char *path = format_str("%s/current-theme", config_dir);
	char *content = format_str("%s\n", name);
	bool ok = tw_write_string(path, content);
	free(content);
	free(path);
	free(config_dir);
	return ok;
}

char *tw_theme_file(const struct tw_theme *theme, const char *file) {
	return format_str("%s/%s", theme->dir, file);
}
