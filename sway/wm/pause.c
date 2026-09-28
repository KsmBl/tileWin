#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "sway/config.h"
#include "sway/desktop/idle_inhibit_v1.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"

/*
 * Minimized apps stop ("pause_minimized <time>"): an app all of whose windows
 * have been minimized (or put in the scratchpad) for that long is stopped with
 * SIGSTOP, together with the processes it started, so it takes no processor
 * time and no power until one of its windows is shown again, focused or
 * closed. Its windows keep their last picture meanwhile.
 *
 * An app keeps running while
 *  - any process of it has a window that is shown (a game started by a
 *    launcher, say);
 *  - it is a terminal, or something else running a shell of its own (a build
 *    in a minimized terminal carries on), found by its desktop entry and by
 *    processes of it on a terminal other than its own;
 *  - it plays sound ("pause_minimized_sound keep", asked of the sound server
 *    with pactl) or keeps the screen on (a video);
 *  - it is listed in "pause_minimized_except".
 * Those are looked at again every half minute.
 *
 * The stopped processes are written to a file in XDG_RUNTIME_DIR, so if
 * tileWin itself ends without letting them go on, the next tileWin does.
 */

#define RECHECK_MS 30000 // an app that had to keep running is looked at again after this
#define SOUND_TIMEOUT_MS 3000
#define TREE_MAX 512

struct proc {
	pid_t pid;
	unsigned long long start;
};

struct app {
	pid_t pid;
	unsigned long long start;
	int64_t since_ms;      // all its windows hidden since
	int64_t next_check_ms; // not looked at before this
	bool paused;
	bool asking;           // waits for the answer about sound
	struct proc *tree;     // stopped processes, the app first
	int tree_len;
};

static list_t *apps; // struct app *
static struct wl_event_source *timer, *idle;

static struct {
	pid_t pid;
	int fd;
	struct wl_event_source *source, *timeout;
	char *out;
	size_t len, cap;
} sound = { .pid = -1, .fd = -1 };

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- processes ---------- */

/* Field 22 of /proc/<pid>/stat, and the parent in field 4; 0 if gone. */
static unsigned long long proc_start(pid_t pid, pid_t *parent) {
	char path[64], buf[1024];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	char *p = strrchr(buf, ')');
	if (!p) {
		return 0;
	}
	unsigned long long start = 0;
	int field = 2;
	char *save = NULL;
	for (char *tok = strtok_r(p + 1, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		field++;
		if (field == 4 && parent) {
			*parent = (pid_t)atoi(tok);
		} else if (field == 22) {
			start = strtoull(tok, NULL, 10);
			break;
		}
	}
	return start;
}

/* The process and all it started, the process first. */
static int proc_tree(pid_t root_pid, struct proc *out, int max) {
	struct { pid_t pid, parent; unsigned long long start; } *all = NULL;
	int count = 0, cap = 0;
	DIR *d = opendir("/proc");
	if (!d) {
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) {
			continue;
		}
		pid_t pid = (pid_t)atoi(e->d_name), parent = 0;
		unsigned long long start = proc_start(pid, &parent);
		if (!start) {
			continue;
		}
		if (count == cap) {
			cap = cap ? cap * 2 : 512;
			void *grown = realloc(all, cap * sizeof(*all));
			if (!grown) {
				break;
			}
			all = grown;
		}
		all[count].pid = pid;
		all[count].parent = parent;
		all[count].start = start;
		count++;
	}
	closedir(d);
	int n = 0;
	for (int i = 0; i < count; i++) {
		if (all[i].pid == root_pid) {
			out[n++] = (struct proc){ all[i].pid, all[i].start };
		}
	}
	// breadth first: the children of those found so far
	for (int k = 0; k < n && n < max; k++) {
		for (int i = 0; i < count && n < max; i++) {
			if (all[i].parent == out[k].pid) {
				out[n++] = (struct proc){ all[i].pid, all[i].start };
			}
		}
	}
	free(all);
	return n;
}

/* The terminal a process reads from, "" for none. */
static void proc_tty(pid_t pid, char *out, size_t size) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/fd/0", (int)pid);
	ssize_t n = readlink(path, out, size - 1);
	out[n > 0 ? n : 0] = 0;
	if (strncmp(out, "/dev/pts/", 9) != 0 && strncmp(out, "/dev/tty", 8) != 0) {
		out[0] = 0;
	}
}

/* ---------- the state file ---------- */

static char *state_path(void) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !server.socket) {
		return NULL;
	}
	return format_str("%s/tilewin-paused-%s", dir, server.socket);
}

static void save_state(void) {
	char *path = state_path();
	if (!path) {
		return;
	}
	FILE *f = NULL;
	for (int i = 0; apps && i < apps->length; i++) {
		struct app *a = apps->items[i];
		for (int j = 0; a->paused && j < a->tree_len; j++) {
			if (!f && !(f = fopen(path, "w"))) {
				free(path);
				return;
			}
			fprintf(f, "%d %llu\n", (int)a->tree[j].pid, a->tree[j].start);
		}
	}
	if (f) {
		fclose(f);
	} else {
		unlink(path);
	}
	free(path);
}

void tw_pause_recover(void) {
	char *path = state_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	if (f) {
		int pid;
		unsigned long long start;
		int count = 0;
		while (fscanf(f, "%d %llu", &pid, &start) == 2) {
			if (pid > 1 && proc_start(pid, NULL) == start) {
				kill(pid, SIGCONT);
				count++;
			}
		}
		fclose(f);
		unlink(path);
		if (count) {
			sway_log(SWAY_INFO, "Let %d processes stopped by a tileWin before go on", count);
		}
	}
	free(path);
}

/* ---------- stopping and going on ---------- */

static void resume(struct app *a) {
	if (!a->paused) {
		return;
	}
	// the app first, so it is not woken by its children's signals in a strange order
	for (int j = 0; j < a->tree_len; j++) {
		if (proc_start(a->tree[j].pid, NULL) == a->tree[j].start) {
			kill(a->tree[j].pid, SIGCONT);
		}
	}
	sway_log(SWAY_DEBUG, "App %d goes on", (int)a->pid);
	a->paused = false;
	free(a->tree);
	a->tree = NULL;
	a->tree_len = 0;
	save_state();
}

static void app_free(struct app *a) {
	resume(a);
	free(a->tree);
	free(a);
}

static struct app *find_app(pid_t pid) {
	for (int i = 0; apps && i < apps->length; i++) {
		struct app *a = apps->items[i];
		if (a->pid == pid) {
			return a;
		}
	}
	return NULL;
}

bool tw_pause_is_paused(struct sway_view *view) {
	if (!view || view->pid <= 0 || !apps) {
		return false;
	}
	for (int i = 0; i < apps->length; i++) {
		struct app *a = apps->items[i];
		for (int j = 0; a->paused && j < a->tree_len; j++) {
			if (a->tree[j].pid == view->pid) {
				return true;
			}
		}
	}
	return false;
}

void tw_pause_wake(struct sway_view *view) {
	if (!view || view->pid <= 0 || !apps) {
		return;
	}
	for (int i = 0; i < apps->length; i++) {
		struct app *a = apps->items[i];
		bool in_tree = a->pid == view->pid;
		for (int j = 0; !in_tree && a->paused && j < a->tree_len; j++) {
			in_tree = a->tree[j].pid == view->pid;
		}
		if (in_tree) {
			resume(a);
			a->since_ms = now_ms(); // a full while again before it stops
		}
	}
}

void tw_pause_forget(void) {
	for (int i = 0; apps && i < apps->length; i++) {
		resume(apps->items[i]);
	}
}

/* ---------- which windows are hidden ---------- */

struct window {
	pid_t pid;
	bool hidden;
	struct sway_view *view;
};

static bool view_hidden(struct sway_container *con) {
	return con->pending.tw_minimized || container_is_scratchpad_hidden_or_child(con);
}

static void collect(struct sway_container *con, void *data) {
	list_t *windows = data;
	if (!con->view || !con->view->surface || con->view->pid <= 1) {
		return;
	}
	struct window *w = malloc(sizeof(*w));
	if (w) {
		*w = (struct window){ con->view->pid, view_hidden(con), con->view };
		list_add(windows, w);
	}
}

static bool pid_all_hidden(list_t *windows, pid_t pid) {
	bool any = false;
	for (int i = 0; i < windows->length; i++) {
		struct window *w = windows->items[i];
		if (w->pid == pid) {
			if (!w->hidden) {
				return false;
			}
			any = true;
		}
	}
	return any;
}

/* ---------- which apps may stop ---------- */

static size_t without_desktop(const char *id) {
	size_t n = strlen(id);
	return n > 8 && strcasecmp(id + n - 8, ".desktop") == 0 ? n - 8 : n;
}

/* "firefox.desktop", "firefox" and "Firefox" are the same app. */
static bool id_matches(const char *listed, const char *id) {
	size_t n = without_desktop(listed);
	return without_desktop(id) == n && strncasecmp(listed, id, n) == 0;
}

struct app_kind {
	char *app_id;
	char *desktop_id;
	char *wm_class;
	bool terminal;
};
static list_t *kinds; // struct app_kind *, looked up once per app id

static struct app_kind *kind_of(const char *app_id) {
	if (!kinds) {
		kinds = create_list();
	}
	for (int i = 0; i < kinds->length; i++) {
		struct app_kind *k = kinds->items[i];
		if (strcmp(k->app_id, app_id) == 0) {
			return k;
		}
	}
	struct app_kind *k = calloc(1, sizeof(*k));
	k->app_id = strdup(app_id);
	struct tw_desktop_entry *e = tw_desktop_find_for_app_id(app_id);
	if (e) {
		k->desktop_id = e->id ? strdup(e->id) : NULL;
		k->wm_class = e->startup_wm_class ? strdup(e->startup_wm_class) : NULL;
		k->terminal = e->categories && strstr(e->categories, "TerminalEmulator");
		tw_desktop_entry_free(e);
	}
	list_add(kinds, k);
	return k;
}

bool tw_app_in_list(list_t *list, const char *app_id) {
	if (!app_id || !list) {
		return false;
	}
	struct app_kind *k = kind_of(app_id);
	for (int i = 0; i < list->length; i++) {
		const char *listed = list->items[i];
		if (id_matches(listed, app_id) || (k->desktop_id && id_matches(listed, k->desktop_id)) ||
				(k->wm_class && id_matches(listed, k->wm_class))) {
			return true;
		}
	}
	return false;
}

static bool excepted(const char *app_id) {
	return config && tw_app_in_list(config->tw_pause_except, app_id);
}

/* Why the app keeps running, NULL if it may stop (sound is asked later). */
static const char *keeps_running(struct app *a, list_t *windows, struct proc *tree, int n) {
	for (int i = 0; i < windows->length; i++) {
		struct window *w = windows->items[i];
		if (w->pid != a->pid) {
			continue;
		}
		const char *app_id = view_get_app_id(w->view);
		if (!app_id) {
			app_id = view_get_class(w->view);
		}
		if (app_id && excepted(app_id)) {
			return "listed in pause_minimized_except";
		}
		if (app_id && kind_of(app_id)->terminal) {
			return "a terminal";
		}
		if (sway_idle_inhibit_v1_application_inhibitor_for_view(w->view)) {
			return "keeping the screen on";
		}
	}
	// a process of it with a window that is shown
	for (int j = 1; j < n; j++) {
		for (int i = 0; i < windows->length; i++) {
			struct window *w = windows->items[i];
			if (w->pid == tree[j].pid && !w->hidden) {
				return "a process of it has a window shown";
			}
		}
	}
	// a shell of its own: a process on another terminal than the app's
	char own[128], tty[128];
	proc_tty(a->pid, own, sizeof(own));
	for (int j = 1; j < n; j++) {
		proc_tty(tree[j].pid, tty, sizeof(tty));
		if (tty[0] && strcmp(tty, own) != 0) {
			return "running a terminal";
		}
	}
	return NULL;
}

static void schedule(void);

static void stop_app(struct app *a, struct proc *tree, int n) {
	// the app last, so none of its children notices it gone first
	for (int j = n - 1; j >= 0; j--) {
		if (tree[j].pid > 1 && tree[j].pid != getpid()) {
			kill(tree[j].pid, SIGSTOP);
		}
	}
	a->tree = malloc(n * sizeof(*tree));
	if (a->tree) {
		memcpy(a->tree, tree, n * sizeof(*tree));
		a->tree_len = n;
	}
	a->paused = true;
	sway_log(SWAY_INFO, "Stopped the minimized app %d (%d processes)", (int)a->pid, n);
	save_state();
}

/* ---------- asking the sound server who plays ---------- */

static list_t *sound_waiting; // struct app *, asked about

static void sound_pids(const char *text, bool *known, list_t *pids) {
	*known = false;
	if (!text) {
		return;
	}
	json_object *root = json_tokener_parse(text);
	if (!root || !json_object_is_type(root, json_type_array)) {
		json_object_put(root);
		return;
	}
	*known = true;
	size_t n = json_object_array_length(root);
	for (size_t i = 0; i < n; i++) {
		json_object *input = json_object_array_get_idx(root, i), *props, *pid;
		if (json_object_object_get_ex(input, "properties", &props) &&
				json_object_object_get_ex(props, "application.process.id", &pid)) {
			int value = atoi(json_object_get_string(pid));
			if (value > 1) {
				list_add(pids, (void *)(intptr_t)value);
			}
		}
	}
	json_object_put(root);
}

static void try_stop(struct app *a, list_t *windows, bool sound_known, list_t *playing);

static list_t *current_windows(void) {
	list_t *windows = create_list();
	root_for_each_container(collect, windows);
	return windows;
}

static void sound_finished(void) {
	if (sound.source) {
		wl_event_source_remove(sound.source);
		sound.source = NULL;
	}
	if (sound.timeout) {
		wl_event_source_remove(sound.timeout);
		sound.timeout = NULL;
	}
	if (sound.fd >= 0) {
		close(sound.fd);
		sound.fd = -1;
	}
	int status = 0;
	bool missing = false;
	if (sound.pid > 0) {
		kill(sound.pid, SIGKILL); // (done already, unless it took too long)
		if (waitpid(sound.pid, &status, 0) > 0 && WIFEXITED(status) &&
				WEXITSTATUS(status) == 127) {
			missing = true; // no pactl: nothing is known about sound
		}
	}
	sound.pid = -1;
	if (sound.out) {
		sound.out[sound.len] = 0;
	}
	list_t *playing = create_list();
	bool known = false;
	sound_pids(sound.out, &known, playing);
	free(sound.out);
	sound.out = NULL;
	sound.len = sound.cap = 0;

	list_t *windows = current_windows();
	for (int i = 0; sound_waiting && i < sound_waiting->length; i++) {
		struct app *a = sound_waiting->items[i];
		if (list_find(apps, a) < 0) {
			continue; // gone meanwhile
		}
		a->asking = false;
		if (known || missing) {
			try_stop(a, windows, known, playing);
		} else {
			a->next_check_ms = now_ms() + RECHECK_MS;
		}
	}
	if (sound_waiting) {
		sound_waiting->length = 0;
	}
	list_free_items_and_destroy(windows);
	list_free(playing);
	schedule();
}

static int sound_readable(int fd, uint32_t mask, void *data) {
	if (sound.len + 4096 + 1 > sound.cap) {
		size_t cap = sound.cap ? sound.cap * 2 : 16384;
		char *grown = realloc(sound.out, cap);
		if (!grown) {
			sound_finished();
			return 0;
		}
		sound.out = grown;
		sound.cap = cap;
	}
	ssize_t n = read(fd, sound.out + sound.len, 4096);
	if (n > 0) {
		sound.len += n;
		return 0;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}
	sound_finished();
	return 0;
}

static int sound_took_too_long(void *data) {
	sway_log(SWAY_DEBUG, "pactl did not answer in time");
	free(sound.out);
	sound.out = NULL;
	sound.len = sound.cap = 0;
	sound_finished();
	return 0;
}

static void ask_sound(struct app *a) {
	if (!sound_waiting) {
		sound_waiting = create_list();
	}
	a->asking = true;
	list_add(sound_waiting, a);
	if (sound.pid > 0) {
		return; // asked already; the answer serves this one too
	}
	int fds[2];
	if (pipe(fds) != 0) {
		sound_finished();
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int null = open("/dev/null", O_RDWR);
		dup2(null, STDIN_FILENO);
		dup2(fds[1], STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		close(fds[0]);
		execlp("pactl", "pactl", "-f", "json", "list", "sink-inputs", (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		sound_finished();
		return;
	}
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	sound.pid = pid;
	sound.fd = fds[0];
	sound.source = wl_event_loop_add_fd(server.wl_event_loop, fds[0], WL_EVENT_READABLE,
		sound_readable, NULL);
	sound.timeout = wl_event_loop_add_timer(server.wl_event_loop, sound_took_too_long, NULL);
	if (sound.timeout) {
		wl_event_source_timer_update(sound.timeout, SOUND_TIMEOUT_MS);
	}
}

static void try_stop(struct app *a, list_t *windows, bool sound_known, list_t *playing) {
	if (a->paused || !pid_all_hidden(windows, a->pid)) {
		return;
	}
	if (proc_start(a->pid, NULL) != a->start) {
		return; // gone, and the number used again
	}
	struct proc *tree = malloc(TREE_MAX * sizeof(*tree));
	if (!tree) {
		return;
	}
	int n = proc_tree(a->pid, tree, TREE_MAX);
	const char *why = n > 0 ? keeps_running(a, windows, tree, n) : "gone";
	for (int j = 0; !why && sound_known && j < n; j++) {
		for (int i = 0; i < playing->length; i++) {
			if ((pid_t)(intptr_t)playing->items[i] == tree[j].pid) {
				why = "playing sound";
			}
		}
	}
	if (why) {
		sway_log(SWAY_DEBUG, "Minimized app %d keeps running: %s", (int)a->pid, why);
		a->next_check_ms = now_ms() + RECHECK_MS;
	} else {
		stop_app(a, tree, n);
	}
	free(tree);
}

/* ---------- looking at the windows ---------- */

static void evaluate(void) {
	int delay_ms = config ? config->tw_pause_minimized * 1000 : 0;
	if (!apps) {
		apps = create_list();
	}
	list_t *windows = current_windows();
	int64_t now = now_ms();
	for (int i = apps->length - 1; i >= 0; i--) {
		struct app *a = apps->items[i];
		if (delay_ms == 0 || !pid_all_hidden(windows, a->pid) ||
				proc_start(a->pid, NULL) != a->start) {
			list_del(apps, i);
			if (sound_waiting && list_find(sound_waiting, a) >= 0) {
				list_del(sound_waiting, list_find(sound_waiting, a));
			}
			app_free(a);
		}
	}
	for (int i = 0; delay_ms > 0 && i < windows->length; i++) {
		struct window *w = windows->items[i];
		if (w->pid == getpid() || w->pid == tw_panel_pid() || find_app(w->pid) ||
				!pid_all_hidden(windows, w->pid)) {
			continue;
		}
		struct app *a = calloc(1, sizeof(*a));
		if (!a) {
			break;
		}
		a->pid = w->pid;
		a->start = proc_start(w->pid, NULL);
		a->since_ms = now;
		list_add(apps, a);
	}
	for (int i = 0; delay_ms > 0 && i < apps->length; i++) {
		struct app *a = apps->items[i];
		if (a->paused || a->asking || now < a->since_ms + delay_ms || now < a->next_check_ms) {
			continue;
		}
		if (config->tw_pause_keep_sound) {
			ask_sound(a); // stops it when the answer comes
		} else {
			try_stop(a, windows, false, NULL);
		}
	}
	list_free_items_and_destroy(windows);
	schedule();
}

static int handle_timer(void *data) {
	evaluate();
	return 0;
}

static void schedule(void) {
	int delay_ms = config ? config->tw_pause_minimized * 1000 : 0;
	int64_t next = -1, now = now_ms();
	for (int i = 0; delay_ms > 0 && apps && i < apps->length; i++) {
		struct app *a = apps->items[i];
		if (a->paused || a->asking) {
			continue;
		}
		int64_t due = a->since_ms + delay_ms;
		if (a->next_check_ms > due) {
			due = a->next_check_ms;
		}
		if (next < 0 || due < next) {
			next = due;
		}
	}
	if (next < 0) {
		if (timer) {
			wl_event_source_timer_update(timer, 0);
		}
		return;
	}
	if (!timer) {
		timer = wl_event_loop_add_timer(server.wl_event_loop, handle_timer, NULL);
	}
	int64_t wait = next - now;
	if (timer) {
		wl_event_source_timer_update(timer, wait < 1 ? 1 : (int)(wait > 86400000 ? 86400000 : wait));
	}
}

static void handle_idle(void *data) {
	idle = NULL;
	evaluate();
}

void tw_pause_changed(void) {
	if (idle || !server.wl_event_loop) {
		return;
	}
	if (!apps && (!config || config->tw_pause_minimized == 0)) {
		return; // off, and nothing was stopped
	}
	idle = wl_event_loop_add_idle(server.wl_event_loop, handle_idle, NULL);
}
