/*
 * The open and save dialogs of the apps: GTK's own, unless failBrauwser is the
 * default file manager, then failBrauwser's.
 *
 * failBrauwser's install.sh gives every GTK 3 app a module (GTK3_MODULES, from
 * ~/.config/environment.d, which tilewin-session reads) that asks the portal
 * for its dialogs instead, and routes the portal's file chooser to it
 * (~/.config/xdg-desktop-portal/portals.conf). That would give its dialogs
 * whatever the default file manager is. So tileWin, at the start and whenever
 * a mimeapps.list changes:
 *
 *  - sets FAILBRAUWSER_NO_CHOOSER to 0 or 1 for the apps it starts and for
 *    those D-Bus and systemd start: the module leaves GTK 3's dialogs alone
 *    with 1 (it reads it once, when an app starts);
 *  - for GTK 4, Qt, Flatpak and the browsers, which ask the portal: while
 *    failBrauwser is not the default, a tilewin-portals.conf of its own next to
 *    failBrauwser's portals.conf, the same but with GTK's file chooser. The
 *    portal reads the one named after the desktop first. It goes again once
 *    failBrauwser is the default, and is only ever written over or removed
 *    when tileWin wrote it.
 *
 * Without failBrauwser installed it does nothing.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "sway/server.h"
#include "sway/tilewin.h"
#include "log.h"
#include "tw_paths.h"

#define MARK "# Written by tileWin"

static struct wl_event_source *watch_source, *debounce;
static int watch_fd = -1;
static int last_state = -1; // 1 failBrauwser's dialogs, 0 GTK's, -1 not known yet

static char *read_all(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char *text = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&text, &size);
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		fwrite(buf, 1, n, out);
	}
	fclose(out);
	fclose(f);
	return text;
}

static const char *home_dir(void) {
	const char *home = getenv("HOME");
	return home ? home : "/";
}

static void config_home(char *out, size_t size) {
	const char *c = getenv("XDG_CONFIG_HOME");
	if (c && *c) {
		snprintf(out, size, "%s", c);
	} else {
		snprintf(out, size, "%s/.config", home_dir());
	}
}

static void data_home(char *out, size_t size) {
	const char *d = getenv("XDG_DATA_HOME");
	if (d && *d) {
		snprintf(out, size, "%s", d);
	} else {
		snprintf(out, size, "%s/.local/share", home_dir());
	}
}

static const char *data_dirs(void) {
	const char *d = getenv("XDG_DATA_DIRS");
	return d && *d ? d : "/usr/local/share:/usr/share";
}

/* The first entry of a key in a group of a .desktop-style file, newly allocated. */
static char *ini_value(const char *text, const char *group, const char *key) {
	bool in_group = false;
	size_t klen = strlen(key);
	for (const char *line = text; line && *line;) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		if (line[0] == '[') {
			in_group = len == strlen(group) + 2 && strncmp(line + 1, group, strlen(group)) == 0;
		} else if (in_group && len > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
			return strndup(line + klen + 1, len - klen - 1);
		}
		line = end ? end + 1 : NULL;
	}
	return NULL;
}

/* Whether the .desktop file of that id starts failBrauwser. */
/* The Exec line of an app by its desktop file id, the first found. */
static char *desktop_exec(const char *id) {
	if (!id || !*id || strchr(id, '/')) {
		return NULL;
	}
	char dirs[PATH_MAX * 4];
	char home[PATH_MAX];
	data_home(home, sizeof(home));
	snprintf(dirs, sizeof(dirs), "%s:%s", home, data_dirs());
	char *save = NULL;
	for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char path[PATH_MAX * 2];
		snprintf(path, sizeof(path), "%s/applications/%s", dir, id);
		char *text = read_all(path);
		if (!text) {
			continue;
		}
		char *exec = ini_value(text, "Desktop Entry", "Exec");
		free(text);
		return exec; // the first one found is the one that counts
	}
	return NULL;
}

/*
 * The default app for folders, as xdg-mime finds it: the first mimeapps.list
 * with an entry for inode/directory decides, the desktop's own before the
 * plain one, the user's before the system's.
 */
char *tw_folder_default_exec(void) {
	char cfg[PATH_MAX], data[PATH_MAX];
	config_home(cfg, sizeof(cfg));
	data_home(data, sizeof(data));
	char *files[16];
	int n = 0;
	char path[PATH_MAX * 2];
	snprintf(path, sizeof(path), "%s/tilewin-mimeapps.list", cfg);
	files[n++] = strdup(path);
	snprintf(path, sizeof(path), "%s/mimeapps.list", cfg);
	files[n++] = strdup(path);
	files[n++] = strdup("/etc/xdg/tilewin-mimeapps.list");
	files[n++] = strdup("/etc/xdg/mimeapps.list");
	snprintf(path, sizeof(path), "%s/applications/mimeapps.list", data);
	files[n++] = strdup(path);
	char dirs[PATH_MAX * 4];
	snprintf(dirs, sizeof(dirs), "%s", data_dirs());
	char *save = NULL;
	for (char *dir = strtok_r(dirs, ":", &save); dir && n < 16; dir = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof(path), "%s/applications/mimeapps.list", dir);
		files[n++] = strdup(path);
	}
	char *result = NULL;
	bool decided = false;
	for (int i = 0; i < n; i++) {
		char *text = decided ? NULL : read_all(files[i]);
		char *value = text ? ini_value(text, "Default Applications", "inode/directory") : NULL;
		if (value) {
			// the first of the list that is installed
			char *s2 = NULL;
			for (char *id = strtok_r(value, ";", &s2); id && !decided; id = strtok_r(NULL, ";", &s2)) {
				while (isspace((unsigned char)*id)) {
					id++;
				}
				if (!*id) {
					continue;
				}
				result = desktop_exec(id);
				decided = true;
			}
		}
		free(value);
		free(text);
		free(files[i]);
	}
	return result;
}

bool tw_failbrauwser_is_default(void) {
	char *exec = tw_folder_default_exec();
	bool yes = exec && strcasestr(exec, "failbrauwser");
	free(exec);
	return yes;
}

/* Whether failBrauwser's dialogs are installed at all. */
static bool failbrauwser_installed(const char *their_portals) {
	const char *modules = getenv("GTK3_MODULES");
	return (modules && strstr(modules, "failbrauwser")) ||
		(their_portals && strstr(their_portals, "FileChooser=failbrauwser"));
}

/* The portal's choice of file chooser; whether the file of tileWin changed. */
static bool update_portals(bool failbrauwser, const char *their_text, const char *ours_path) {
	char *ours = read_all(ours_path);
	bool ours_is_ours = !ours || strncmp(ours, MARK, strlen(MARK)) == 0;
	bool changed = false;
	if (!ours_is_ours) {
		free(ours); // the user's own: left alone
		return false;
	}
	bool want = their_text && strstr(their_text, "FileChooser=failbrauwser") && !failbrauwser;
	if (!want) {
		if (ours) {
			changed = unlink(ours_path) == 0;
		}
		free(ours);
		return changed;
	}
	// failBrauwser's portals.conf, with GTK's file chooser
	char *text = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&text, &size);
	fprintf(out, "%s while failBrauwser is not the default file manager: the dialogs of\n"
		"# GTK. It goes again once failBrauwser is the default (see portals.conf).\n", MARK);
	for (const char *line = their_text; *line;) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		if (strncmp(line, "org.freedesktop.impl.portal.FileChooser=", 40) == 0) {
			fprintf(out, "org.freedesktop.impl.portal.FileChooser=gtk\n");
		} else if (len && line[0] != '#') {
			fprintf(out, "%.*s\n", (int)len, line);
		}
		line = end ? end + 1 : line + len;
	}
	fclose(out);
	if (!ours || strcmp(ours, text) != 0) {
		changed = tw_write_string(ours_path, text);
	}
	free(text);
	free(ours);
	return changed;
}

static void spawn_shell(const char *script) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", script, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

void tw_file_dialogs_update(void) {
	char cfg[PATH_MAX], their_path[PATH_MAX + 64], ours_path[PATH_MAX + 64];
	config_home(cfg, sizeof(cfg));
	snprintf(their_path, sizeof(their_path), "%s/xdg-desktop-portal/portals.conf", cfg);
	snprintf(ours_path, sizeof(ours_path), "%s/xdg-desktop-portal/tilewin-portals.conf", cfg);
	char *theirs = read_all(their_path);
	if (!failbrauwser_installed(theirs)) {
		update_portals(true, NULL, ours_path); // takes away one tileWin wrote before
		free(theirs);
		return;
	}
	bool failbrauwser = tw_failbrauwser_is_default();
	setenv("FAILBRAUWSER_NO_CHOOSER", failbrauwser ? "0" : "1", 1);
	bool portals_changed = update_portals(failbrauwser, theirs, ours_path);
	free(theirs);
	if (last_state != failbrauwser) {
		sway_log(SWAY_INFO, "File dialogs: %s", failbrauwser ?
			"failBrauwser's, as it is the default file manager" : "GTK's");
	}
	bool state_changed = last_state != failbrauwser;
	last_state = failbrauwser;
	if (getenv("TILEWIN_NO_APP_TWEAKS")) {
		return; // nested or test instances leave the session's services alone
	}
	if (state_changed || portals_changed) {
		// the apps D-Bus and systemd start (and the portal reads its config once)
		spawn_shell(portals_changed ?
			"dbus-update-activation-environment --systemd FAILBRAUWSER_NO_CHOOSER "
			"2>/dev/null; systemctl --user try-restart xdg-desktop-portal.service 2>/dev/null" :
			"dbus-update-activation-environment --systemd FAILBRAUWSER_NO_CHOOSER 2>/dev/null");
	}
}

static int on_debounce(void *data) {
	tw_file_dialogs_update();
	return 0;
}

static int on_watch(int fd, uint32_t mask, void *data) {
	char buf[4096];
	bool relevant = false;
	ssize_t len;
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + len;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && (strstr(ev->name, "mimeapps.list") ||
					strcmp(ev->name, "portals.conf") == 0)) {
				relevant = true;
			}
			p += sizeof(*ev) + ev->len;
		}
	}
	if (relevant && debounce) {
		// written in more steps by some tools: once they are done
		wl_event_source_timer_update(debounce, 300);
	}
	return 0;
}

/* Watches the files that decide it, for a change of the default file manager. */
void tw_file_dialogs_start(void) {
	tw_file_dialogs_update();
	if (watch_fd >= 0) {
		return;
	}
	watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch_fd < 0) {
		return;
	}
	char cfg[PATH_MAX], data[PATH_MAX], path[PATH_MAX + 64];
	config_home(cfg, sizeof(cfg));
	data_home(data, sizeof(data));
	uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE;
	inotify_add_watch(watch_fd, cfg, mask);
	snprintf(path, sizeof(path), "%s/xdg-desktop-portal", cfg);
	inotify_add_watch(watch_fd, path, mask);
	snprintf(path, sizeof(path), "%s/applications", data);
	inotify_add_watch(watch_fd, path, mask);
	watch_source = wl_event_loop_add_fd(server.wl_event_loop, watch_fd, WL_EVENT_READABLE,
		on_watch, NULL);
	debounce = wl_event_loop_add_timer(server.wl_event_loop, on_debounce, NULL);
}
