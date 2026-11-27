/*
 * "Start in the background when idle" (idlestart.h). The list is
 * ~/.config/tileWin/idle-start, a desktop entry id a line; the window menu
 * puts an app on it or takes it off. Once a session, a while after the taskbar
 * started, it looks every few seconds how busy the processor and the disks
 * are (tw_load.h); when both have stayed below their limits for half a minute
 * it starts the next app of the list that does not run yet, minimized and
 * without the focus, and waits for quiet again before the one after it.
 *
 * What was started is written to a file of the session in $XDG_RUNTIME_DIR,
 * so that a taskbar that restarts does not start it again. In taskbar.conf:
 *
 *   idle_start_cpu 20    # percent of the processor, below which it is quiet
 *   idle_start_disk 10   # percent of the time the busiest disk works
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "idlestart.h"
#include "log.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "tw_load.h"
#include "tw_paths.h"

#define LOOK_MS 5000
#define FIRST_S 60  // after the taskbar starts: logging in is busy of its own
#define QUIET_S 30  // how long it must have been quiet

static struct {
	struct panel *panel;
	struct loop_timer *timer;
	list_t *queue; // ids still to start in this session
	struct tw_load last;
	double started, quiet_since;
	int look_ms, first_s, quiet_s;
} is;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *list_path(void) {
	char *dir = tw_config_dir();
	char *path = dir ? format_str("%s/idle-start", dir) : NULL;
	free(dir);
	return path;
}

/* The file of this session: the compositor's socket names it. */
static char *done_path(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *sock = getenv("SWAYSOCK");
	if (!runtime || !sock) {
		return NULL;
	}
	unsigned long hash = 5381;
	for (const char *p = sock; *p; p++) {
		hash = hash * 33 + (unsigned char)*p;
	}
	return format_str("%s/tilewin-idle-started-%lx", runtime, hash);
}

static list_t *read_ids(const char *path) {
	list_t *ids = create_list();
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return ids;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *id = line;
		while (isspace((unsigned char)*id)) {
			id++;
		}
		if (*id && *id != '#') {
			list_add(ids, strdup(id));
		}
	}
	fclose(f);
	return ids;
}

static int find_id(list_t *ids, const char *id) {
	for (int i = 0; ids && i < ids->length; i++) {
		if (strcmp(ids->items[i], id) == 0) {
			return i;
		}
	}
	return -1;
}

static void free_ids(list_t *ids) {
	if (ids) {
		list_free_items_and_destroy(ids);
	}
}

bool idlestart_listed(const char *id) {
	if (!id) {
		return false;
	}
	char *path = list_path();
	list_t *ids = read_ids(path);
	free(path);
	bool listed = find_id(ids, id) >= 0;
	free_ids(ids);
	return listed;
}

/* ---------- running already ---------- */

/* The program of an Exec line: its file name, past env and its settings. */
static char *program_of(const char *exec) {
	int argc = 0;
	char **argv = split_args(exec, &argc);
	char *found = NULL;
	for (int i = 0; i < argc && !found; i++) {
		const char *a = argv[i];
		if (strcmp(a, "env") == 0 || strchr(a, '=')) {
			continue;
		}
		char *word = strdup(a);
		strip_quotes(word);
		const char *slash = strrchr(word, '/');
		found = strdup(slash ? slash + 1 : word);
		free(word);
	}
	free_argv(argc, argv);
	return found;
}

/* Whether a process of the user's has that name (as /proc/<pid>/comm cuts it). */
static bool process_runs(const char *program) {
	if (!program || !*program || strcmp(program, "flatpak") == 0 ||
			strcmp(program, "sh") == 0 || strcmp(program, "bash") == 0) {
		return false; // tells nothing of the app
	}
	DIR *dir = opendir("/proc");
	if (!dir) {
		return false;
	}
	bool runs = false;
	struct dirent *d;
	uid_t me = getuid();
	while (!runs && (d = readdir(dir))) {
		if (!isdigit((unsigned char)d->d_name[0])) {
			continue;
		}
		char path[300], comm[64] = "";
		struct stat st;
		snprintf(path, sizeof(path), "/proc/%s", d->d_name);
		if (stat(path, &st) != 0 || st.st_uid != me) {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/%s/comm", d->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f)) {
				comm[strcspn(comm, "\n")] = '\0';
			}
			fclose(f);
		}
		runs = comm[0] && strncmp(comm, program, 15) == 0 &&
			(strlen(program) <= 15 ? strlen(comm) == strlen(program) : true);
	}
	closedir(dir);
	return runs;
}

static bool app_runs(struct panel *panel, const struct tw_desktop_entry *entry) {
	for (int i = 0; panel->state.windows && i < panel->state.windows->length; i++) {
		struct pwindow *w = panel->state.windows->items[i];
		struct tw_desktop_entry *e = apps_find(w->app_id);
		if (e && strcmp(e->id, entry->id) == 0) {
			return true;
		}
	}
	char *program = entry->exec ? program_of(entry->exec) : NULL;
	bool runs = process_runs(program);
	free(program);
	return runs;
}

/* ---------- starting ---------- */

static void note_done(const char *id) {
	char *path = done_path();
	FILE *f = path ? fopen(path, "a") : NULL;
	if (f) {
		fprintf(f, "%s\n", id);
		fclose(f);
	}
	free(path);
}

/* The app id its windows will likely have, for the compositor to know them by. */
static char *window_app_of(const struct tw_desktop_entry *entry) {
	if (entry->startup_wm_class && *entry->startup_wm_class) {
		return strdup(entry->startup_wm_class);
	}
	char *id = strdup(entry->id);
	char *dot = strstr(id, ".desktop");
	if (dot && !dot[8]) {
		*dot = '\0';
	}
	return id;
}

static void start_next(void) {
	while (is.queue->length > 0) {
		char *id = is.queue->items[0];
		list_del(is.queue, 0);
		note_done(id);
		struct tw_desktop_entry *entry = apps_find(id);
		char *cmd = entry ? tw_desktop_exec_command(entry) : NULL;
		if (!cmd || app_runs(is.panel, entry)) {
			sway_log(SWAY_DEBUG, "Idle start: %s %s", id, cmd ? "runs already" : "is not there");
			free(cmd);
			free(id);
			continue;
		}
		if (entry->terminal && is.panel->config && is.panel->config->terminal) {
			char *wrapped = format_str("%s %s", is.panel->config->terminal, cmd);
			free(cmd);
			cmd = wrapped;
		}
		char *app = window_app_of(entry);
		sway_log(SWAY_INFO, "Idle start: %s (%s)", id, cmd);
		// exec takes its words as they are: an app id has no spaces to quote
		for (char *p = app; *p; p++) {
			if (isspace((unsigned char)*p)) {
				*p = '_';
			}
		}
		ipc_panel_commandf(is.panel, "exec --background --app %s %s", app, cmd);
		free(app);
		free(cmd);
		free(id);
		return; // the next one when it is quiet again
	}
}

static int conf_int(const char *key, int fallback) {
	struct twconf_node *root = is.panel->config ? is.panel->config->root : NULL;
	const char *v = root ? twconf_value(root, key) : NULL;
	int n = v ? atoi(v) : fallback;
	return n < 1 || n > 100 ? fallback : n;
}

static void tick(void *data) {
	is.timer = NULL;
	if (!is.queue || is.queue->length == 0) {
		return;
	}
	double now = now_s();
	struct tw_load load;
	const char *proc = getenv("TILEWIN_IDLE_START_PROC");
	tw_load_read(&load, proc ? proc : "/proc", now);
	int cpu, disk;
	tw_load_between(&is.last, &load, &cpu, &disk);
	is.last = load;
	bool quiet = cpu >= 0 && disk >= 0 && cpu < conf_int("idle_start_cpu", 20) &&
		disk < conf_int("idle_start_disk", 10);
	if (!quiet) {
		is.quiet_since = 0;
	} else if (is.quiet_since == 0) {
		is.quiet_since = now;
	}
	sway_log(SWAY_DEBUG, "Idle start: cpu %d%%, disk %d%%, quiet for %.0f s", cpu, disk,
		is.quiet_since ? now - is.quiet_since : 0);
	if (now - is.started >= is.first_s && is.quiet_since &&
			now - is.quiet_since >= is.quiet_s) {
		start_next();
		is.quiet_since = 0;
	}
	if (is.queue->length > 0) {
		is.timer = loop_add_timer(is.panel->loop, is.look_ms, tick, NULL);
	}
}

/* What is still to start in this session: the list less what was started. */
static void load_queue(void) {
	free_ids(is.queue);
	char *path = list_path();
	is.queue = read_ids(path);
	free(path);
	path = done_path();
	list_t *done = read_ids(path);
	free(path);
	for (int i = is.queue->length - 1; i >= 0; i--) {
		if (find_id(done, is.queue->items[i]) >= 0) {
			free(is.queue->items[i]);
			list_del(is.queue, i);
		}
	}
	free_ids(done);
}

void idlestart_init(struct panel *panel) {
	is.panel = panel;
	is.started = now_s();
	is.look_ms = LOOK_MS;
	is.first_s = FIRST_S;
	is.quiet_s = QUIET_S;
	// the tests: "look_ms first_s quiet_s"
	const char *times = getenv("TILEWIN_IDLE_START_TIMES");
	if (times) {
		sscanf(times, "%d %d %d", &is.look_ms, &is.first_s, &is.quiet_s);
	}
	load_queue();
	if (is.queue->length > 0) {
		tick(NULL);
	}
}

void idlestart_fini(void) {
	if (is.timer && is.panel) {
		loop_remove_timer(is.panel->loop, is.timer);
	}
	is.timer = NULL;
	free_ids(is.queue);
	is.queue = NULL;
}

/* ---------- the list ---------- */

static void write_ids(list_t *ids) {
	char *path = list_path();
	if (!path) {
		return;
	}
	char *dir = tw_config_dir();
	if (dir) {
		tw_mkdir_p(dir);
	}
	free(dir);
	GString *text = g_string_new("# Started in the background when the computer is idle "
		"(the window menu)\n");
	for (int i = 0; i < ids->length; i++) {
		g_string_append_printf(text, "%s\n", (char *)ids->items[i]);
	}
	tw_write_string(path, text->str);
	g_string_free(text, TRUE);
	free(path);
}

void idlestart_command(int argc, char **argv) {
	// argv[0] is "idlestart"
	if (argc < 3) {
		return;
	}
	const char *how = argv[1], *id = argv[2];
	char *path = list_path();
	list_t *ids = read_ids(path);
	free(path);
	int at = find_id(ids, id);
	bool want = strcmp(how, "on") == 0 || (strcmp(how, "toggle") == 0 && at < 0);
	if (want && at < 0) {
		list_add(ids, strdup(id));
		write_ids(ids);
	} else if (!want && at >= 0) {
		free(ids->items[at]);
		list_del(ids, at);
		write_ids(ids);
	}
	free_ids(ids);
}
