/*
 * Several screens in window mode, handled the way Windows handles them:
 *
 * - A window taken to another screen keeps what it was: maximized stays
 *   maximized and snapped stays snapped, now on the new screen, and the size it
 *   goes back to comes along to the same place there. A window too big for the
 *   new screen is made to fit.
 * - A screen that goes away hands the windows it showed to the desktop shown on
 *   another screen, instead of leaving them on a desktop nobody looks at. When
 *   the screen comes back they return to it, where they were and as they were,
 *   unless they were put somewhere else in the meantime.
 * - Win+Left on a window snapped to the left of its screen carries it on to the
 *   right half of the screen to the left, and Win+Right the other way round.
 * - The main display (main_output) is where the desktop icons live.
 *
 * Tile mode keeps the behavior of sway: workspaces follow their outputs.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "log.h"

static bool is_window(struct sway_container *con) {
	return con && con->view && container_is_floating(con) && !con->node.destroying;
}

/* Where box lies in old, carried over to the same place in new. */
static struct wlr_box carry_box(struct wlr_box box, const struct wlr_box *old,
		const struct wlr_box *new) {
	if (old->width <= 0 || old->height <= 0 || box.width <= 0 || box.height <= 0) {
		return box;
	}
	double cx = box.x - old->x + box.width / 2.0;
	double cy = box.y - old->y + box.height / 2.0;
	box.x = new->x + (int)(cx * new->width / old->width - box.width / 2.0);
	box.y = new->y + (int)(cy * new->height / old->height - box.height / 2.0);
	return box;
}

void tw_floating_screen_changed(struct sway_container *con, const struct wlr_box *old,
		const struct wlr_box *new) {
	if (tw_mode != TW_MODE_WINDOW || !is_window(con) || !con->pending.workspace ||
			!con->pending.workspace->output || con->pending.fullscreen_mode) {
		return;
	}
	struct wlr_box area = tw_workarea(con->pending.workspace);
	if (area.width <= 0 || area.height <= 0) {
		return;
	}
	if (con->tw.restore_box.width > 0 && con->tw.restore_box.height > 0) {
		con->tw.restore_box = tw_fit_box(carry_box(con->tw.restore_box, old, new), area);
	}
	if (con->tw.has_window_geometry) {
		con->tw.window_geometry = tw_fit_box(carry_box(con->tw.window_geometry, old, new), area);
	}
	struct wlr_box box;
	if (con->pending.tw_maximized) {
		box = area;
	} else if (con->tw.snap != TW_SNAP_NONE) {
		box = tw_snap_box(area, con->tw.snap);
	} else {
		box = tw_fit_box((struct wlr_box){ (int)con->pending.x, (int)con->pending.y,
			(int)con->pending.width, (int)con->pending.height }, area);
	}
	tw_set_box(con, &box);
}

/* The desktop a window put onto that screen lands on. */
static struct sway_workspace *screen_desktop(struct sway_output *output) {
	return output && output->enabled ? output_get_active_workspace(output) : NULL;
}

static void move_to_workspace(struct sway_container *con, struct sway_workspace *ws) {
	struct sway_workspace *old = con->pending.workspace;
	if (!old || old == ws) {
		return;
	}
	struct wlr_box old_box, new_box;
	workspace_get_box(old, &old_box);
	container_detach(con);
	workspace_add_floating(ws, con);
	workspace_get_box(ws, &new_box);
	floating_fix_coordinates(con, &old_box, &new_box);
	node_set_dirty(&con->node);
	arrange_workspace(old);
	arrange_workspace(ws);
	workspace_consider_destroy(old);
	ipc_event_window(con, "move");
}

void tw_move_to_screen(struct sway_container *con, struct sway_output *output) {
	struct sway_workspace *ws = screen_desktop(output);
	if (!is_window(con) || !ws || con->pending.workspace == ws) {
		return;
	}
	struct sway_seat *seat = input_manager_current_seat();
	bool focused = seat_get_focused_container(seat) == con;
	tw_forget_home(con);
	move_to_workspace(con, ws);
	if (focused) {
		seat_set_focus_container(seat, con);
	}
	container_raise_floating(con);
}

static enum tw_snap mirror(enum tw_snap snap) {
	switch (snap) {
	case TW_SNAP_LEFT:
		return TW_SNAP_RIGHT;
	case TW_SNAP_RIGHT:
		return TW_SNAP_LEFT;
	case TW_SNAP_TOPLEFT:
		return TW_SNAP_TOPRIGHT;
	case TW_SNAP_TOPRIGHT:
		return TW_SNAP_TOPLEFT;
	case TW_SNAP_BOTTOMLEFT:
		return TW_SNAP_BOTTOMRIGHT;
	case TW_SNAP_BOTTOMRIGHT:
		return TW_SNAP_BOTTOMLEFT;
	case TW_SNAP_TOP:
	case TW_SNAP_NONE:
		break;
	}
	return snap;
}

bool tw_snap_across(struct sway_container *con, int direction) {
	if (!is_window(con) || con->pending.tw_maximized || !con->pending.workspace ||
			!con->pending.workspace->output) {
		return false;
	}
	enum tw_snap snap = con->tw.snap;
	bool at_left = snap == TW_SNAP_LEFT || snap == TW_SNAP_TOPLEFT ||
		snap == TW_SNAP_BOTTOMLEFT;
	bool at_right = snap == TW_SNAP_RIGHT || snap == TW_SNAP_TOPRIGHT ||
		snap == TW_SNAP_BOTTOMRIGHT;
	if (!(direction == WLR_DIRECTION_LEFT ? at_left : direction == WLR_DIRECTION_RIGHT && at_right)) {
		return false;
	}
	struct sway_output *here = con->pending.workspace->output;
	struct wlr_output *next = wlr_output_layout_adjacent_output(root->output_layout,
		direction, here->wlr_output, con->pending.x + con->pending.width / 2,
		con->pending.y + con->pending.height / 2);
	struct sway_output *output = next ? output_from_wlr_output(next) : NULL;
	if (!screen_desktop(output)) {
		return false;
	}
	tw_move_to_screen(con, output);
	tw_snap_to(con, mirror(snap));
	return true;
}

/* ---------- screens going away and coming back ---------- */

static void output_id(struct sway_output *output, char *id, size_t size) {
	output_get_identifier(id, size, output);
}

/*
 * Screens are known by make, model and serial, which stay the same on another
 * connector; only when those say nothing (virtual screens) the name decides.
 */
static bool is_home(struct sway_container *con, struct sway_output *output) {
	if (!con->tw.home.output) {
		return false;
	}
	char id[256];
	output_id(output, id, sizeof(id));
	bool known = strcmp(id, "Unknown Unknown Unknown") != 0;
	if (known && con->tw.home.id && strcmp(con->tw.home.id, id) == 0) {
		return true;
	}
	return !known && strcmp(con->tw.home.output, output->wlr_output->name) == 0;
}

void tw_forget_home(struct sway_container *con) {
	if (!con) {
		return;
	}
	free(con->tw.home.output);
	free(con->tw.home.id);
	con->tw.home.output = con->tw.home.id = NULL;
}

static struct wlr_box relative(struct wlr_box box, struct sway_output *output) {
	box.x -= output->lx;
	box.y -= output->ly;
	return box;
}

static void remember_home(struct sway_container *con, void *data) {
	struct sway_output *output = data;
	if (!is_window(con) || con->pending.fullscreen_mode) {
		return;
	}
	char id[256];
	output_id(output, id, sizeof(id));
	tw_forget_home(con);
	con->tw.home.output = strdup(output->wlr_output->name);
	con->tw.home.id = strdup(id);
	con->tw.home.width = output->width;
	con->tw.home.height = output->height;
	con->tw.home.maximized = con->pending.tw_maximized;
	con->tw.home.snap = con->pending.tw_maximized ? TW_SNAP_NONE : con->tw.snap;
	con->tw.home.box = relative((struct wlr_box){ (int)con->pending.x, (int)con->pending.y,
		(int)con->pending.width, (int)con->pending.height }, output);
	bool placed = con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE;
	con->tw.home.restore = placed && con->tw.restore_box.width > 0 ?
		relative(con->tw.restore_box, output) : con->tw.home.box;
}

static struct sway_workspace *leaving_shown; // the desktop the leaving screen showed

void tw_screen_leaving(struct sway_output *output) {
	leaving_shown = NULL;
	if (tw_mode != TW_MODE_WINDOW) {
		return;
	}
	for (int i = 0; i < output->workspaces->length; i++) {
		struct sway_workspace *ws = output->workspaces->items[i];
		workspace_for_each_container(ws, remember_home, output);
	}
	leaving_shown = output_get_active_workspace(output);
}

void tw_screen_left(struct sway_output *output) {
	struct sway_workspace *ws = leaving_shown;
	leaving_shown = NULL;
	tw_main_output_changed();
	if (!ws || ws->node.destroying || !ws->output || ws->output == output ||
			!ws->output->enabled) {
		return;
	}
	struct sway_workspace *shown = output_get_active_workspace(ws->output);
	if (!shown || shown == ws) {
		return; // the desktop is on view on the other screen already
	}
	// the windows the screen showed join those on view on the screen they went to
	list_t *windows = create_list();
	list_cat(windows, ws->floating);
	for (int i = 0; i < windows->length; i++) {
		struct sway_container *con = windows->items[i];
		if (is_window(con) && !con->pending.fullscreen_mode) {
			move_to_workspace(con, shown);
		}
	}
	list_free(windows);
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *focus = seat_get_focus(seat);
	if (focus && focus->type == N_WORKSPACE && focus->sway_workspace == ws) {
		struct sway_container *next = tw_next_focus_candidate(seat, shown, NULL);
		if (next) {
			seat_set_focus_container(seat, next);
		} else {
			seat_set_focus_workspace(seat, shown);
		}
	}
	if (!ws->node.destroying) {
		workspace_consider_destroy(ws);
	}
}

/* The saved box, from a screen of the saved size to one of the current size. */
static struct wlr_box home_box(struct wlr_box box, struct sway_container *con,
		struct sway_output *output) {
	struct wlr_box old = { 0, 0, con->tw.home.width, con->tw.home.height };
	struct wlr_box now = { 0, 0, output->width, output->height };
	if (old.width != now.width || old.height != now.height) {
		box = carry_box(box, &old, &now);
	}
	box.x += output->lx;
	box.y += output->ly;
	return box;
}

static void return_home(struct sway_container *con, struct sway_output *output) {
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || ws->output != output) {
		ws = screen_desktop(output);
		if (!ws) {
			return;
		}
		move_to_workspace(con, ws);
	}
	struct wlr_box area = tw_workarea(ws);
	struct wlr_box restore = tw_fit_box(home_box(con->tw.home.restore, con, output), area);
	bool maximized = con->tw.home.maximized;
	enum tw_snap snap = con->tw.home.snap;
	struct wlr_box box = home_box(con->tw.home.box, con, output);
	tw_forget_home(con);
	// start from the restored size, so maximizing or snapping keeps it to go back to
	con->pending.tw_maximized = false;
	con->tw.snap = TW_SNAP_NONE;
	tw_set_box(con, &restore);
	if (maximized) {
		tw_maximize(con, true);
	} else if (snap != TW_SNAP_NONE) {
		tw_snap_to(con, snap);
	} else {
		box = tw_fit_box(box, area);
		tw_set_box(con, &box);
		tw_view_notify_maximized(con->view, false);
	}
}

struct home_search {
	struct sway_output *output;
	list_t *found;
};

static void find_returning(struct sway_container *con, void *data) {
	struct home_search *search = data;
	if (is_window(con) && is_home(con, search->output)) {
		list_add(search->found, con);
	}
}

static struct wl_event_source *return_idle;
static list_t *returning_outputs; // sway_output *, waiting for the idle callback

static void return_windows(void *data) {
	return_idle = NULL;
	list_t *outputs = returning_outputs;
	returning_outputs = NULL;
	for (int i = 0; outputs && i < outputs->length; i++) {
		struct sway_output *output = outputs->items[i];
		if (list_find(root->outputs, output) < 0 || !output->enabled) {
			continue; // gone again before its windows could return
		}
		struct home_search search = { output, create_list() };
		root_for_each_container(find_returning, &search);
		for (int j = 0; j < search.found->length; j++) {
			return_home(search.found->items[j], output);
		}
		if (search.found->length) {
			sway_log(SWAY_DEBUG, "%d windows went back to %s", search.found->length,
				output->wlr_output->name);
			arrange_root();
		}
		list_free(search.found);
	}
	list_free(outputs);
	transaction_commit_dirty();
}

void tw_screen_added(struct sway_output *output) {
	tw_main_output_changed();
	if (tw_mode != TW_MODE_WINDOW) {
		return;
	}
	// the screen gets its place in the layout and its taskbar first
	if (!returning_outputs) {
		returning_outputs = create_list();
	}
	if (list_find(returning_outputs, output) < 0) {
		list_add(returning_outputs, output);
	}
	if (!return_idle) {
		return_idle = wl_event_loop_add_idle(server.wl_event_loop, return_windows, NULL);
	}
}

/* ---------- the main display ---------- */

struct sway_output *tw_main_output(void) {
	if (config && config->tw_main_output) {
		struct sway_output *output = output_by_name_or_id(config->tw_main_output);
		if (output && output->enabled) {
			return output;
		}
	}
	// the one at the top left of the layout, like the first screen Windows found
	struct sway_output *best = NULL;
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (!output->enabled) {
			continue;
		}
		if (!best || output->ly < best->ly || (output->ly == best->ly && output->lx < best->lx)) {
			best = output;
		}
	}
	return best;
}

static char *announced_main;

void tw_main_output_changed(void) {
	if (!root || (config && config->reading)) {
		return;
	}
	struct sway_output *output = tw_main_output();
	const char *name = output ? output->wlr_output->name : "";
	if (announced_main && strcmp(announced_main, name) == 0) {
		return;
	}
	free(announced_main);
	announced_main = strdup(name);
	ipc_event_tilewin("main_output", tw_describe_state());
}
