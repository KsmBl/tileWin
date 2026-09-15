/*
 * XDG autostart: when tileWin starts, it runs the apps of ~/.config/autostart
 * and $XDG_CONFIG_DIRS/autostart (/etc/xdg/autostart) like other desktops do,
 * unless the config says "xdg_autostart disable". Entries that are hidden,
 * turned off or meant for other desktops are skipped, and so are programs that
 * already run (for example started by an exec line of the config).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sway/config.h"
#include "sway/tilewin.h"
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "tw_paths.h"

struct entry {
	char *exec, *try_exec, *only_show_in, *not_show_in;
	bool application, hidden, disabled, terminal;
};

static void entry_clear(struct entry *e) {
	free(e->exec);
	free(e->try_exec);
	free(e->only_show_in);
	free(e->not_show_in);
}

static bool parse_bool(const char *value) {
	return strcasecmp(value, "true") == 0;
}

static void parse(const char *path, struct entry *e) {
	memset(e, 0, sizeof(*e));
	e->application = true;
	FILE *f = fopen(path, "r");
	if (!f) {
		e->hidden = true;
		return;
	}
	char line[4096];
	bool in_group = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_group = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		char *eq = strchr(line, '=');
		if (!in_group || line[0] == '#' || !eq) {
			continue;
		}
		*eq = '\0';
		char *key = line, *value = eq + 1;
		char *end = eq;
		while (end > key && isspace((unsigned char)end[-1])) {
			*--end = '\0';
		}
		while (isspace((unsigned char)*value)) {
			value++;
		}
		if (strcmp(key, "Type") == 0) {
			e->application = strcmp(value, "Application") == 0;
		} else if (strcmp(key, "Exec") == 0) {
			free(e->exec);
			e->exec = strdup(value);
		} else if (strcmp(key, "TryExec") == 0) {
			free(e->try_exec);
			e->try_exec = strdup(value);
		} else if (strcmp(key, "Hidden") == 0) {
			e->hidden = parse_bool(value);
		} else if (strcmp(key, "X-GNOME-Autostart-enabled") == 0) {
			e->disabled = !parse_bool(value);
		} else if (strcmp(key, "Terminal") == 0) {
			e->terminal = parse_bool(value);
		} else if (strcmp(key, "OnlyShowIn") == 0) {
			free(e->only_show_in);
			e->only_show_in = strdup(value);
		} else if (strcmp(key, "NotShowIn") == 0) {
			free(e->not_show_in);
			e->not_show_in = strdup(value);
		}
	}
	fclose(f);
}

/* True if one of the current desktop names is in a "A;B;" list. */
static bool desktop_listed(const char *list) {
	const char *current = getenv("XDG_CURRENT_DESKTOP");
	char *names = strdup(current && *current ? current : "tileWin");
	bool found = false;
	char *save_names = NULL;
	for (char *name = strtok_r(names, ":", &save_names); name && !found;
			name = strtok_r(NULL, ":", &save_names)) {
		char *copy = strdup(list);
		char *save = NULL;
		for (char *item = strtok_r(copy, ";", &save); item && !found;
				item = strtok_r(NULL, ";", &save)) {
			found = strcasecmp(item, name) == 0 || strcasecmp(item, "tileWin") == 0;
		}
		free(copy);
	}
	free(names);
	return found;
}

/* The Exec line without field codes (%f, %U, ...). */
static char *strip_field_codes(const char *exec) {
	char *out = malloc(strlen(exec) + 1);
	char *o = out;
	for (const char *p = exec; *p; p++) {
		if (*p == '%' && p[1]) {
			if (p[1] == '%') {
				*o++ = '%';
			}
			p++;
			continue;
		}
		*o++ = *p;
	}
	*o = '\0';
	return out;
}

static char *first_word(const char *command) {
	while (isspace((unsigned char)*command)) {
		command++;
	}
	char quote = *command == '"' || *command == '\'' ? *command : '\0';
	const char *start = quote ? command + 1 : command;
	const char *end = start;
	while (*end && (quote ? *end != quote : !isspace((unsigned char)*end))) {
		end++;
	}
	return strndup(start, end - start);
}

static bool program_found(const char *program) {
	if (strchr(program, '/')) {
		return access(program, X_OK) == 0;
	}
	return tw_in_path(program);
}

/* A process of that program runs already (compares /proc/<pid>/comm). */
static bool already_running(const char *program) {
	const char *base = strrchr(program, '/');
	base = base ? base + 1 : program;
	DIR *dir = opendir("/proc");
	struct dirent *de;
	bool found = false;
	while (dir && !found && (de = readdir(dir))) {
		if (!isdigit((unsigned char)de->d_name[0])) {
			continue;
		}
		char path[300], comm[64];
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (f && fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\n")] = '\0';
			found = strncmp(comm, base, 15) == 0 && (strlen(base) >= 15 || strlen(comm) == strlen(base));
		}
		if (f) {
			fclose(f);
		}
	}
	if (dir) {
		closedir(dir);
	}
	return found;
}

static void spawn(const char *command) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

static void run_directory(const char *dir, list_t *seen) {
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		size_t len = strlen(de->d_name);
		if (len < 9 || strcmp(de->d_name + len - 8, ".desktop") != 0) {
			continue;
		}
		bool done = false;
		for (int i = 0; i < seen->length && !done; i++) {
			done = strcmp(seen->items[i], de->d_name) == 0;
		}
		if (done) {
			continue; // a user entry of the same name replaces the system one
		}
		list_add(seen, strdup(de->d_name));
		char *path = format_str("%s/%s", dir, de->d_name);
		struct entry e;
		parse(path, &e);
		bool run = e.application && !e.hidden && !e.disabled && !e.terminal && e.exec &&
			(!e.only_show_in || desktop_listed(e.only_show_in)) &&
			(!e.not_show_in || !desktop_listed(e.not_show_in)) &&
			(!e.try_exec || program_found(e.try_exec));
		if (run) {
			char *command = strip_field_codes(e.exec);
			char *program = first_word(command);
			if (!*program || !program_found(program)) {
				sway_log(SWAY_DEBUG, "Autostart: %s is not installed", program);
			} else if (already_running(program)) {
				sway_log(SWAY_INFO, "Autostart: %s runs already", program);
			} else {
				sway_log(SWAY_INFO, "Autostart: %s", command);
				spawn(command);
			}
			free(program);
			free(command);
		}
		entry_clear(&e);
		free(path);
	}
	if (d) {
		closedir(d);
	}
}

void tw_xdg_autostart(void) {
	if (!config || !config->tw_xdg_autostart) {
		return;
	}
	list_t *seen = create_list();
	const char *config_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *user = config_home && *config_home ? format_str("%s/autostart", config_home) :
		home ? format_str("%s/.config/autostart", home) : NULL;
	if (user) {
		run_directory(user, seen);
		free(user);
	}
	const char *dirs = getenv("XDG_CONFIG_DIRS");
	char *copy = strdup(dirs && *dirs ? dirs : "/etc/xdg");
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char *path = format_str("%s/autostart", dir);
		run_directory(path, seen);
		free(path);
	}
	free(copy);
	list_free_items_and_destroy(seen);
}
