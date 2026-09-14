#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "stringop.h"
#include "tw_paths.h"

#ifndef TILEWIN_DATADIR
#define TILEWIN_DATADIR "/usr/local/share/tileWin"
#endif

char *tw_expand_home(const char *path) {
	if (!path) {
		return NULL;
	}
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home) {
			return format_str("%s%s", home, path + 1);
		}
	}
	return strdup(path);
}

static char *xdg_dir(const char *env, const char *fallback) {
	const char *base = getenv(env);
	if (base && base[0] == '/') {
		return format_str("%s/tileWin", base);
	}
	const char *home = getenv("HOME");
	if (!home) {
		return NULL;
	}
	return format_str("%s/%s/tileWin", home, fallback);
}

char *tw_config_dir(void) {
	return xdg_dir("XDG_CONFIG_HOME", ".config");
}

char *tw_state_dir(void) {
	return xdg_dir("XDG_STATE_HOME", ".local/state");
}

const char *tw_data_dir(void) {
	const char *env = getenv("TILEWIN_DATADIR");
	return env && *env ? env : TILEWIN_DATADIR;
}

bool tw_mkdir_p(const char *path) {
	char *tmp = strdup(path);
	if (!tmp) {
		return false;
	}
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
			free(tmp);
			return false;
		}
		*p = '/';
	}
	bool ok = mkdir(tmp, 0755) == 0 || errno == EEXIST;
	free(tmp);
	return ok;
}

char *tw_read_first_line(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char *line = NULL;
	size_t size = 0;
	ssize_t n = getline(&line, &size, f);
	fclose(f);
	if (n <= 0) {
		free(line);
		return NULL;
	}
	char *start = line;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
	char *result = strdup(start);
	free(line);
	return result;
}

bool tw_write_string(const char *path, const char *content) {
	char *dir = strdup(path);
	char *slash = dir ? strrchr(dir, '/') : NULL;
	if (slash && slash != dir) {
		*slash = '\0';
		tw_mkdir_p(dir);
	}
	free(dir);

	char *tmp = format_str("%s.tmp.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f) {
		free(tmp);
		return false;
	}
	bool ok = fputs(content, f) >= 0;
	ok = fclose(f) == 0 && ok;
	ok = ok && rename(tmp, path) == 0;
	if (!ok) {
		unlink(tmp);
	}
	free(tmp);
	return ok;
}

char *tw_find_data_file(const char *relpath) {
	char *config_dir = tw_config_dir();
	if (config_dir) {
		char *path = format_str("%s/%s", config_dir, relpath);
		free(config_dir);
		if (access(path, R_OK) == 0) {
			return path;
		}
		free(path);
	}
	char *path = format_str("%s/%s", tw_data_dir(), relpath);
	if (access(path, R_OK) == 0) {
		return path;
	}
	free(path);
	return NULL;
}
