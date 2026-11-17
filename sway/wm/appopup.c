#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "twconf.h"

/*
 * App popups: an application that comes and goes with a shortcut, at a place
 * and size of its own, over the rest of the screen darkened, like the drop
 * down terminals of old. ~/.config/tileWin/popups.conf (the Popups page of
 * the settings) names them:
 *
 *   popup mail {
 *       app thunderbird.desktop   # the desktop entry: how it starts, its windows
 *       exec thunderbird          # instead of the entry's command (optional)
 *       app_id thunderbird        # windows of this app_id too (optional)
 *       position right            # left, right, top, bottom, center
 *       width 70%                 # of the screen's work area, or pixels
 *       height 100%
 *       dim 50                    # how dark the rest gets, percent; 0 for not at all
 *   }
 *
 * and "popup toggle mail" (bound to a key, "bindsym $mod+m popup toggle mail")
 * shows it: its window when it runs (one that was open already is taken in),
 * else it starts it and takes its first window. It floats on the screen with
 * the focus, in a layer of its own above the taskbar, over a dark sheet. The
 * shortcut again, a click on the darkened rest, or the focus going to another
 * app hides it to the scratchpad, where it keeps running for the next time.
 * Other windows of the same app (a message being written) come up with it.
 */

#define MAX_POPUPS 32
#define LAUNCH_WAIT_MS 30000

struct popup_def {
	char name[64];
	char desktop[256];
	char app_id[128];
	char exec[1024];
	char position[16];
	double width, height; // above 1: pixels, else a share of the work area
	int dim;              // percent
};

struct popup_state {
	char name[64];
	size_t con_id;         // its window, 0 for none yet
	int64_t waiting_until; // started, its window not there yet
	pid_t pid;             // what was started
	struct popup_def def;  // as it was read when it was asked for last
};

static struct {
	struct popup_state items[MAX_POPUPS];
	int count;
	struct popup_state *shown;
	struct wlr_scene_tree *layer; // above the taskbar: the sheets, then the popup
	struct wlr_scene_tree *sheets;
	struct wl_event_source *hide_later;
} ap;

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- the definitions ---------- */

static double parse_size(const char *v, double fallback) {
	if (!v || !*v) {
		return fallback;
	}
	char *end;
	double n = strtod(v, &end);
	if (end == v || n <= 0) {
		return fallback;
	}
	if (*end == '%') {
		return n > 100 ? 1 : n / 100;
	}
	return n < 2 ? fallback : n; // pixels
}

static bool load_def(const char *name, struct popup_def *def) {
	char *dir = tw_config_dir();
	char *path = NULL;
	if (dir && asprintf(&path, "%s/popups.conf", dir) < 0) {
		path = NULL;
	}
	free(dir);
	char *error = NULL;
	struct twconf_node *root = path ? twconf_parse_file(path, &error) : NULL;
	free(path);
	free(error);
	bool found = false;
	for (int i = 0; root && i < twconf_count(root) && !found; i++) {
		struct twconf_node *n = twconf_at(root, i);
		if (strcmp(n->name, "popup") != 0 || n->argc < 1 || strcmp(n->argv[0], name) != 0) {
			continue;
		}
		found = true;
		memset(def, 0, sizeof(*def));
		snprintf(def->name, sizeof(def->name), "%s", name);
		const char *v;
		snprintf(def->desktop, sizeof(def->desktop), "%s",
			(v = twconf_value(n, "app")) ? v : "");
		snprintf(def->app_id, sizeof(def->app_id), "%s",
			(v = twconf_value(n, "app_id")) ? v : "");
		struct twconf_node *exec = twconf_child(n, "exec");
		if (exec && exec->argc > 0) {
			char *joined = twconf_join(exec, 0);
			snprintf(def->exec, sizeof(def->exec), "%s", joined);
			free(joined);
		}
		snprintf(def->position, sizeof(def->position), "%s",
			(v = twconf_value(n, "position")) ? v : "center");
		def->width = parse_size(twconf_value(n, "width"), 0.6);
		def->height = parse_size(twconf_value(n, "height"), 0.7);
		v = twconf_value(n, "dim");
		def->dim = !v ? 50 : strcasecmp(v, "no") == 0 ? 0 : strcasecmp(v, "yes") == 0 ? 50 :
			atoi(v);
		def->dim = def->dim < 0 ? 0 : def->dim > 95 ? 95 : def->dim;
	}
	twconf_free(root);
	return found;
}

static struct popup_state *state_of(const char *name) {
	for (int i = 0; i < ap.count; i++) {
		if (strcmp(ap.items[i].name, name) == 0) {
			return &ap.items[i];
		}
	}
	if (ap.count == MAX_POPUPS) {
		return NULL;
	}
	struct popup_state *s = &ap.items[ap.count++];
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	return s;
}

/* ---------- which windows are its ---------- */

static bool has_id(struct sway_container *con, void *data) {
	return con->node.id == *(size_t *)data;
}

static struct sway_container *window_of(struct popup_state *s) {
	if (!s->con_id) {
		return NULL;
	}
	struct sway_container *con = root_find_container(has_id, &s->con_id);
	if (!con || !con->view || con->node.destroying) {
		s->con_id = 0;
		return NULL;
	}
	return con;
}

static bool taken(struct sway_container *con) {
	for (int i = 0; i < ap.count; i++) {
		if (ap.items[i].con_id == con->node.id) {
			return true;
		}
	}
	return false;
}

/* Whether pid is ancestor or the process itself, from /proc. */
static bool descends_from(pid_t pid, pid_t ancestor) {
	for (int depth = 0; pid > 1 && depth < 32; depth++) {
		if (pid == ancestor) {
			return true;
		}
		char path[64], buf[512];
		snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
		FILE *f = fopen(path, "r");
		if (!f) {
			return false;
		}
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = '\0';
		char *close = strrchr(buf, ')');
		int ppid = 0;
		if (!close || sscanf(close + 2, "%*c %d", &ppid) != 1) {
			return false;
		}
		pid = ppid;
	}
	return false;
}

static bool view_matches(struct sway_view *view, struct popup_state *s) {
	const char *app_id = view_get_app_id(view);
	if (!app_id) {
		app_id = view_get_class(view);
	}
	if (app_id && s->def.app_id[0] && strcasecmp(app_id, s->def.app_id) == 0) {
		return true;
	}
	if (app_id && s->def.desktop[0]) {
		struct tw_desktop_entry *entry = tw_desktop_find_for_app_id(app_id);
		bool same = entry && entry->id && strcmp(entry->id, s->def.desktop) == 0;
		tw_desktop_entry_free(entry);
		if (same) {
			return true;
		}
	}
	return s->pid > 0 && s->waiting_until > now_ms() && view->pid > 0 &&
		descends_from(view->pid, s->pid);
}

struct find_data {
	struct popup_state *s;
	struct sway_container *found;
};

static void find_window(struct sway_container *con, void *data) {
	struct find_data *d = data;
	if (!d->found && con->view && con->view->surface && !taken(con) &&
			view_matches(con->view, d->s)) {
		d->found = con;
	}
}

/* ---------- the layer and the sheet ---------- */

static void ensure_layer(void) {
	if (ap.layer) {
		return;
	}
	ap.layer = wlr_scene_tree_create(root->layer_tree);
	wlr_scene_node_place_above(&ap.layer->node, &root->layers.shell_top->node);
	ap.sheets = wlr_scene_tree_create(ap.layer);
}

static void clear_sheets(void) {
	if (!ap.sheets) {
		return;
	}
	struct wlr_scene_node *node, *tmp;
	wl_list_for_each_safe(node, tmp, &ap.sheets->children, link) {
		wlr_scene_node_destroy(node);
	}
}

/* A dark sheet over every screen, under the popup. */
static void draw_sheets(int dim) {
	clear_sheets();
	if (dim <= 0) {
		return;
	}
	float a = dim / 100.0f;
	const float color[4] = { 0, 0, 0, a };
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (!output->enabled || !output->wlr_output) {
			continue;
		}
		struct wlr_scene_rect *r = wlr_scene_rect_create(ap.sheets, output->width,
			output->height, color);
		if (r) {
			wlr_scene_node_set_position(&r->node, output->lx, output->ly);
		}
	}
	wlr_scene_node_lower_to_bottom(&ap.sheets->node);
}

static bool same_app(struct sway_container *con) {
	struct sway_container *popup = ap.shown ? window_of(ap.shown) : NULL;
	return popup && con && con->view && popup->view && con != popup &&
		con->view->pid > 0 && con->view->pid == popup->view->pid;
}

struct wlr_scene_tree *tw_appopup_layer_for(struct sway_container *con) {
	if (!ap.shown || !ap.layer || !con || !con->view) {
		return NULL;
	}
	struct sway_container *popup = window_of(ap.shown);
	if (!popup || (con != popup && !same_app(con))) {
		return NULL;
	}
	return ap.layer;
}

bool tw_appopup_is(struct sway_container *con) {
	return tw_appopup_layer_for(con) != NULL;
}

/* ---------- showing and hiding ---------- */

static struct wlr_box popup_box(struct popup_def *def, struct sway_workspace *ws) {
	struct wlr_box area = tw_workarea(ws);
	int w = def->width > 1 ? (int)def->width : (int)(area.width * def->width + 0.5);
	int h = def->height > 1 ? (int)def->height : (int)(area.height * def->height + 0.5);
	w = w > area.width ? area.width : w < 120 ? 120 : w;
	h = h > area.height ? area.height : h < 80 ? 80 : h;
	struct wlr_box b = { area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h };
	if (strcasecmp(def->position, "left") == 0) {
		b.x = area.x;
	} else if (strcasecmp(def->position, "right") == 0) {
		b.x = area.x + area.width - w;
	} else if (strcasecmp(def->position, "top") == 0) {
		b.y = area.y;
	} else if (strcasecmp(def->position, "bottom") == 0) {
		b.y = area.y + area.height - h;
	}
	return b;
}

static void commit(void) {
	transaction_commit_dirty();
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (output->wlr_output && output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void hide(struct popup_state *s) {
	struct sway_container *con = window_of(s);
	if (ap.shown == s) {
		ap.shown = NULL;
		clear_sheets();
	}
	if (con && !container_is_scratchpad_hidden(con)) {
		if (con->scratchpad) {
			root_scratchpad_hide(con);
		} else {
			struct sway_workspace *ws = con->pending.workspace;
			root_scratchpad_add_container(con, NULL);
			if (ws) {
				arrange_workspace(ws);
			}
		}
	}
	arrange_root();
	commit();
}

static void show(struct popup_state *s, struct sway_container *con) {
	struct sway_seat *seat = input_manager_current_seat();
	if (ap.shown && ap.shown != s) {
		hide(ap.shown);
	}
	if (con->pending.tw_minimized) {
		tw_minimize(con, false);
	}
	if (con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		container_fullscreen_disable(con);
	}
	if (con->pending.tw_maximized) {
		tw_maximize(con, false);
	}
	// to the workspace with the focus, by way of the scratchpad
	if (!con->scratchpad) {
		root_scratchpad_add_container(con, NULL);
	}
	if (container_is_scratchpad_hidden(con)) {
		root_scratchpad_show(con);
	}
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws) {
		return;
	}
	if (!container_is_floating(con)) {
		container_set_floating(con, true);
	}
	struct wlr_box box = popup_box(&s->def, ws);
	tw_set_box(con, &box);
	ap.shown = s;
	ensure_layer();
	draw_sheets(s->def.dim);
	container_raise_floating(con);
	seat_set_focus_container(seat, con);
	arrange_workspace(ws);
	arrange_root();
	commit();
}

static void start(struct popup_state *s) {
	char command[1100] = "";
	if (s->def.exec[0]) {
		snprintf(command, sizeof(command), "%s", s->def.exec);
	} else if (s->def.desktop[0]) {
		list_t *entries = tw_desktop_scan();
		for (int i = 0; entries && i < entries->length; i++) {
			struct tw_desktop_entry *e = entries->items[i];
			if (e->id && strcmp(e->id, s->def.desktop) == 0) {
				char *exec = tw_desktop_exec_command(e);
				snprintf(command, sizeof(command), "%s", exec ? exec : "");
				free(exec);
				break;
			}
		}
		tw_desktop_list_free(entries);
	}
	if (!command[0]) {
		sway_log(SWAY_ERROR, "popup %s: nothing to start", s->name);
		return;
	}
	// the pid of what runs, so its windows are known even with another app_id
	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	pid_t child = fork();
	if (child == 0) {
		setsid();
		close(fds[0]);
		pid_t grandchild = fork();
		if (grandchild == 0) {
			close(fds[1]);
			signal(SIGPIPE, SIG_DFL);
			execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
			_exit(127);
		}
		if (write(fds[1], &grandchild, sizeof(grandchild)) < 0) {
			_exit(1);
		}
		_exit(0);
	}
	close(fds[1]);
	pid_t pid = 0;
	if (child > 0) {
		if (read(fds[0], &pid, sizeof(pid)) != sizeof(pid)) {
			pid = 0;
		}
		waitpid(child, NULL, 0);
	}
	close(fds[0]);
	s->pid = pid;
	s->waiting_until = now_ms() + LAUNCH_WAIT_MS;
}

bool tw_appopup_toggle(const char *name, char **error) {
	struct popup_def def;
	if (!load_def(name, &def)) {
		*error = strdup("No such popup in popups.conf");
		return false;
	}
	struct popup_state *s = state_of(name);
	if (!s) {
		*error = strdup("Too many popups");
		return false;
	}
	s->def = def;
	struct sway_container *con = window_of(s);
	if (!con) {
		// one that is open already
		struct find_data d = { s, NULL };
		root_for_each_container(find_window, &d);
		con = d.found;
		if (con) {
			s->con_id = con->node.id;
		}
	}
	if (!con) {
		if (s->waiting_until <= now_ms()) {
			start(s);
		}
		return true;
	}
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_container *focused = seat_get_focused_container(seat);
	if (ap.shown == s && !container_is_scratchpad_hidden(con) &&
			(focused == con || same_app(focused))) {
		hide(s);
	} else {
		show(s, con);
	}
	return true;
}

/* ---------- what happens around it ---------- */

void tw_appopup_view_mapped(struct sway_view *view) {
	int64_t now = now_ms();
	for (int i = 0; i < ap.count; i++) {
		struct popup_state *s = &ap.items[i];
		if (s->waiting_until > now && !window_of(s) && view_matches(view, s)) {
			s->waiting_until = 0;
			s->con_id = view->container->node.id;
			show(s, view->container);
			return;
		}
	}
	if (same_app(view->container) && !container_is_floating(view->container)) {
		// a window of the app shown (a message being written): up with it
		container_set_floating(view->container, true);
		container_floating_move_to_center(view->container);
	}
}

void tw_appopup_container_destroyed(struct sway_container *con) {
	for (int i = 0; i < ap.count; i++) {
		struct popup_state *s = &ap.items[i];
		if (s->con_id == con->node.id) {
			s->con_id = 0;
			if (ap.shown == s) {
				ap.shown = NULL;
				clear_sheets();
			}
		}
	}
}

static int handle_hide_later(void *data) {
	ap.hide_later = NULL;
	if (ap.shown) {
		hide(ap.shown);
	}
	return 0;
}

static void hide_soon(void) {
	if (!ap.hide_later) {
		ap.hide_later = wl_event_loop_add_timer(server.wl_event_loop, handle_hide_later, NULL);
		if (ap.hide_later) {
			wl_event_source_timer_update(ap.hide_later, 1);
		}
	}
}

void tw_appopup_focus_changed(struct sway_container *con) {
	if (!ap.shown || !con || !con->view) {
		return;
	}
	struct sway_container *popup = window_of(ap.shown);
	if (popup && con != popup && !same_app(con)) {
		hide_soon(); // another app has the focus now
	}
}

bool tw_appopup_button(struct sway_node *node, struct wlr_surface *surface, bool pressed) {
	if (!ap.shown) {
		return false;
	}
	if (!node && surface) {
		return false; // a layer above it (a notification) or the taskbar, without a sheet
	}
	struct sway_container *popup = window_of(ap.shown);
	if (!popup) {
		ap.shown = NULL;
		clear_sheets();
		return false;
	}
	struct sway_container *con = node && node->type == N_CONTAINER ? node->sway_container : NULL;
	while (con && con->pending.parent) {
		con = con->pending.parent;
	}
	if (con && (con == popup || same_app(con))) {
		return false;
	}
	// on the darkened rest: away with it, and the click goes no further; without
	// a sheet the click goes on to what it was on, which takes the focus and so hides it
	if (pressed && ap.shown->def.dim > 0) {
		hide_soon();
	}
	return ap.shown->def.dim > 0;
}

void tw_appopup_outputs_changed(void) {
	if (ap.shown) {
		draw_sheets(ap.shown->def.dim);
	}
}
