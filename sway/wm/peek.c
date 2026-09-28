#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

/*
 * Peek, like Aero Peek on Windows 7: while the pointer rests on the preview
 * of a window above the taskbar, only that window is shown and every other
 * one turns into an outline of glass; resting on "Show desktop" does that
 * with all of them, so the desktop shows. The taskbar asks for it with
 * "peek <con_id>|desktop|off".
 *
 * The hidden windows stay where they are and keep running; a minimized
 * window that is peeked at shows its last picture where it would open.
 * It ends by itself after a while, and with any click, so a taskbar that went
 * away while peeking cannot leave the windows hidden.
 */

#define PEEK_MAX_MS 30000

// the glass: a dark line, a light line inside it and a faint sheet (premultiplied)
static const float edge_dark[4] = { 0.0f, 0.0f, 0.0f, 0.45f };
static const float edge_light[4] = { 0.55f, 0.55f, 0.55f, 0.55f };
static const float sheet[4] = { 0.07f, 0.08f, 0.09f, 0.09f };

static struct {
	bool active, desktop;
	size_t target; // node id of the window looked at, 0 for the desktop
	struct wlr_scene_tree *glass;    // outlines, above the tiled windows
	struct wlr_scene_tree *picture;  // a minimized window's picture, above all windows
	struct wl_event_source *timeout;
} peek;

bool tw_peek_active(void) {
	return peek.active;
}

bool tw_peek_hides(struct sway_container *con) {
	return peek.active && con && con->view && con->node.id != peek.target;
}

static bool shown(struct sway_container *con) {
	struct sway_workspace *ws = con->pending.workspace;
	return con->view && ws && workspace_is_visible(ws) && !con->pending.tw_minimized &&
		!container_is_scratchpad_hidden_or_child(con);
}

static void outline(struct wlr_scene_tree *parent, struct wlr_box b) {
	if (b.width < 4 || b.height < 4) {
		return;
	}
	wlr_scene_node_set_position(&wlr_scene_rect_create(parent, b.width - 4, b.height - 4,
		sheet)->node, b.x + 2, b.y + 2);
	const struct { int x, y, w, h; const float *c; } lines[] = {
		{ 0, 0, b.width, 1, edge_dark },
		{ 0, b.height - 1, b.width, 1, edge_dark },
		{ 0, 1, 1, b.height - 2, edge_dark },
		{ b.width - 1, 1, 1, b.height - 2, edge_dark },
		{ 1, 1, b.width - 2, 1, edge_light },
		{ 1, b.height - 2, b.width - 2, 1, edge_light },
		{ 1, 2, 1, b.height - 4, edge_light },
		{ b.width - 2, 2, 1, b.height - 4, edge_light },
	};
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		struct wlr_scene_rect *r = wlr_scene_rect_create(parent, lines[i].w, lines[i].h,
			lines[i].c);
		if (r) {
			wlr_scene_node_set_position(&r->node, b.x + lines[i].x, b.y + lines[i].y);
		}
	}
}

static void add_glass(struct sway_container *con, void *data) {
	if (!shown(con) || con->node.id == peek.target || tw_animate_hides(con)) {
		return;
	}
	outline(peek.glass, (struct wlr_box){ con->pending.x, con->pending.y, con->pending.width,
		con->pending.height });
}

static void clear(void) {
	if (peek.glass) {
		wlr_scene_node_destroy(&peek.glass->node);
		peek.glass = NULL;
	}
	if (peek.picture) {
		wlr_scene_node_destroy(&peek.picture->node);
		peek.picture = NULL;
	}
	if (peek.timeout) {
		wl_event_source_remove(peek.timeout);
		peek.timeout = NULL;
	}
}

static int handle_timeout(void *data) {
	peek.timeout = NULL;
	tw_peek(NULL, false);
	return 0;
}

static void schedule_frames(void) {
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (output->wlr_output && output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

void tw_peek(struct sway_container *con, bool desktop) {
	bool active = con != NULL || desktop;
	size_t target = con ? con->node.id : 0;
	if (active == peek.active && target == peek.target && desktop == peek.desktop) {
		if (peek.timeout) {
			wl_event_source_timer_update(peek.timeout, PEEK_MAX_MS);
		}
		return;
	}
	clear();
	peek.active = active;
	peek.desktop = desktop && !con;
	peek.target = target;
	if (active) {
		peek.glass = wlr_scene_tree_create(root->layers.tiling);
		if (peek.glass) {
			wlr_scene_node_raise_to_top(&peek.glass->node);
			root_for_each_container(add_glass, NULL);
		}
		if (con && con->view && (con->pending.tw_minimized ||
				container_is_scratchpad_hidden_or_child(con))) {
			// its last picture, where it would open
			peek.picture = wlr_scene_tree_create(root->layers.floating);
			if (peek.picture) {
				wlr_scene_node_raise_to_top(&peek.picture->node);
				tw_snapshot_view(peek.picture, con->view, 1, 1, con->pending.content_x,
					con->pending.content_y);
			}
		}
		peek.timeout = wl_event_loop_add_timer(server.wl_event_loop, handle_timeout, NULL);
		if (peek.timeout) {
			wl_event_source_timer_update(peek.timeout, PEEK_MAX_MS);
		}
	}
	// the windows are hidden and shown again the way sway shows them
	arrange_root();
	transaction_commit_dirty();
	schedule_frames();
}

void tw_peek_container_destroyed(struct sway_container *con) {
	if (peek.active && con->node.id == peek.target) {
		clear();
		peek.active = false;
		peek.target = 0;
		// (the arrange after the window is gone shows the others again)
	}
}
