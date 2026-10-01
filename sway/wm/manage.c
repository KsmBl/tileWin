#include <wlr/types/wlr_subcompositor.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <wlr/config.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif
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
#include "list.h"
#include "tw_grid.h"
#include "log.h"
#include "stringop.h"

struct wlr_box tw_workarea(struct sway_workspace *ws) {
	struct wlr_box area = { 0 };
	if (ws && ws->output) {
		area = ws->output->usable_area;
		area.x += ws->output->lx;
		area.y += ws->output->ly;
	} else if (ws) {
		workspace_get_box(ws, &area);
	}
	return area;
}

void tw_view_notify_maximized(struct sway_view *view, bool maximized) {
	if (!view || !view->surface) {
		return;
	}
	switch (view->type) {
	case SWAY_VIEW_XDG_SHELL:
		if (view->wlr_xdg_toplevel->base->initialized) {
			wlr_xdg_toplevel_set_maximized(view->wlr_xdg_toplevel, maximized);
		}
		break;
#if WLR_HAS_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		wlr_xwayland_surface_set_maximized(view->wlr_xwayland_surface,
			maximized, maximized);
		break;
#endif
	}
	if (view->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign_toplevel, maximized);
	}
}

static void view_notify_minimized(struct sway_view *view, bool minimized) {
	if (!view || !view->surface) {
		return;
	}
#if WLR_HAS_XWAYLAND
	if (view->type == SWAY_VIEW_XWAYLAND) {
		wlr_xwayland_surface_set_minimized(view->wlr_xwayland_surface, minimized);
	}
#endif
	if (view->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->foreign_toplevel, minimized);
	}
}

void tw_set_box(struct sway_container *con, const struct wlr_box *box) {
	tw_session_changed();
	con->pending.x = box->x;
	con->pending.y = box->y;
	con->pending.width = box->width;
	con->pending.height = box->height;
	tw_container_update_deco_state(con);
	if (con->view) {
		view_autoconfigure(con->view);
	}
	node_set_dirty(&con->node);
	if (con->pending.workspace) {
		node_set_dirty(&con->pending.workspace->node);
	}
}

static struct wlr_box current_box(struct sway_container *con) {
	return (struct wlr_box){
		(int)con->pending.x, (int)con->pending.y,
		(int)con->pending.width, (int)con->pending.height,
	};
}

struct wlr_box tw_fit_box(struct wlr_box box, struct wlr_box area) {
	if (box.width > area.width) {
		box.width = area.width;
	}
	if (box.height > area.height) {
		box.height = area.height;
	}
	if (box.x < area.x) {
		box.x = area.x;
	}
	if (box.y < area.y) {
		box.y = area.y;
	}
	if (box.x + box.width > area.x + area.width) {
		box.x = area.x + area.width - box.width;
	}
	if (box.y + box.height > area.y + area.height) {
		box.y = area.y + area.height - box.height;
	}
	return box;
}

static struct wlr_box default_restore_box(struct sway_container *con) {
	struct wlr_box area = tw_workarea(con->pending.workspace);
	int w = area.width * 0.6, h = area.height * 0.65;
	return (struct wlr_box){
		area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h,
	};
}

void tw_maximize(struct sway_container *con, bool enable) {
	if (!con || !con->view || !container_is_floating(con) ||
			!con->pending.workspace) {
		return;
	}
	if (con->pending.tw_maximized == enable) {
		return;
	}
	if (enable) {
		if (con->tw.snap == TW_SNAP_NONE) {
			con->tw.restore_box = current_box(con);
		}
		con->tw.snap = TW_SNAP_NONE;
		con->pending.tw_maximized = true;
		struct wlr_box area = tw_workarea(con->pending.workspace);
		tw_set_box(con, &area);
	} else {
		con->pending.tw_maximized = false;
		struct wlr_box box = con->tw.restore_box;
		if (box.width <= 0 || box.height <= 0) {
			box = default_restore_box(con);
		}
		box = tw_fit_box(box, tw_workarea(con->pending.workspace));
		tw_set_box(con, &box);
	}
	tw_animate_resize(con);
	tw_view_notify_maximized(con->view, enable);
	ipc_event_window(con, enable ? "maximize" : "restore");
}

struct sway_container *tw_next_focus_candidate(struct sway_seat *seat,
		struct sway_workspace *ws, struct sway_container *exclude) {
	struct sway_seat_node *current;
	wl_list_for_each(current, &seat->focus_stack, link) {
		struct sway_node *node = current->node;
		if (node->type != N_CONTAINER || !node->sway_container->view) {
			continue;
		}
		struct sway_container *c = node->sway_container;
		if (c == exclude || c->pending.tw_minimized || c->node.destroying) {
			continue;
		}
		if (ws && c->pending.workspace != ws) {
			continue;
		}
		return c;
	}
	return NULL;
}

void tw_minimize(struct sway_container *con, bool enable) {
	if (!con || !con->view || con->pending.tw_minimized == enable) {
		return;
	}
	struct sway_workspace *ws = con->pending.workspace;
	if (enable && !container_is_floating(con)) {
		// Tiled windows have no taskbar-only state; use the scratchpad.
		if (!con->scratchpad) {
			root_scratchpad_add_container(con, NULL);
		}
		return;
	}
	con->pending.tw_minimized = enable;
	view_notify_minimized(con->view, enable);
	tw_animate_minimize(con, enable);

	if (enable) {
		struct sway_seat *seat = input_manager_current_seat();
		if (seat_get_focused_container(seat) == con) {
			struct sway_container *next = tw_next_focus_candidate(seat, ws, con);
			if (next) {
				seat_set_focus_container(seat, next);
			} else if (ws) {
				seat_set_focus_workspace(seat, ws);
			}
		}
	}
	node_set_dirty(&con->node);
	if (ws) {
		node_set_dirty(&ws->node);
		arrange_workspace(ws);
	}
	ipc_event_window(con, enable ? "minimize" : "unminimize");
	if (!enable) {
		tw_pause_wake(con->view); // it draws again at once
	}
	tw_pause_changed();
}

/* The slot of a snapped window, with the lines between the slots at fx of the
 * width and fy of the height of the work area. */
static struct wlr_box split_box(struct wlr_box area, enum tw_snap snap, double fx, double fy) {
	int hw = (int)(area.width * fx), hh = (int)(area.height * fy);
	switch (snap) {
	case TW_SNAP_LEFT:
		return (struct wlr_box){ area.x, area.y, hw, area.height };
	case TW_SNAP_RIGHT:
		return (struct wlr_box){ area.x + hw, area.y, area.width - hw, area.height };
	case TW_SNAP_TOPLEFT:
		return (struct wlr_box){ area.x, area.y, hw, hh };
	case TW_SNAP_TOPRIGHT:
		return (struct wlr_box){ area.x + hw, area.y, area.width - hw, hh };
	case TW_SNAP_BOTTOMLEFT:
		return (struct wlr_box){ area.x, area.y + hh, hw, area.height - hh };
	case TW_SNAP_BOTTOMRIGHT:
		return (struct wlr_box){ area.x + hw, area.y + hh, area.width - hw, area.height - hh };
	case TW_SNAP_TOP:
	case TW_SNAP_NONE:
		break;
	}
	return area;
}

struct wlr_box tw_snap_box(struct wlr_box area, enum tw_snap snap) {
	return split_box(area, snap, 0.5, 0.5);
}

static double split_or_half(double f) {
	return f > 0 && f < 1 ? f : 0.5;
}

struct wlr_box tw_container_snap_box(struct sway_container *con, struct wlr_box area) {
	return split_box(area, con->tw.snap, split_or_half(con->tw.split_x),
		split_or_half(con->tw.split_y));
}

static bool snap_left(enum tw_snap snap) {
	return snap == TW_SNAP_LEFT || snap == TW_SNAP_TOPLEFT || snap == TW_SNAP_BOTTOMLEFT;
}

static bool snap_right(enum tw_snap snap) {
	return snap == TW_SNAP_RIGHT || snap == TW_SNAP_TOPRIGHT || snap == TW_SNAP_BOTTOMRIGHT;
}

static bool snap_quarter(enum tw_snap snap) {
	return snap >= TW_SNAP_TOPLEFT && snap <= TW_SNAP_BOTTOMRIGHT;
}

enum wlr_edges tw_snap_inner_edges(enum tw_snap snap) {
	switch (snap) {
	case TW_SNAP_LEFT:
		return WLR_EDGE_RIGHT;
	case TW_SNAP_RIGHT:
		return WLR_EDGE_LEFT;
	case TW_SNAP_TOPLEFT:
		return WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
	case TW_SNAP_TOPRIGHT:
		return WLR_EDGE_LEFT | WLR_EDGE_BOTTOM;
	case TW_SNAP_BOTTOMLEFT:
		return WLR_EDGE_RIGHT | WLR_EDGE_TOP;
	case TW_SNAP_BOTTOMRIGHT:
		return WLR_EDGE_LEFT | WLR_EDGE_TOP;
	case TW_SNAP_TOP:
	case TW_SNAP_NONE:
		break;
	}
	return WLR_EDGE_NONE;
}

/*
 * A window snapped next to others takes the lines between them as they are,
 * as on Windows: next to a left half made wider it gets the rest of the width,
 * under a quarter made taller the rest of the height.
 */
static void take_splits(struct sway_container *con, enum tw_snap snap) {
	con->tw.split_x = con->tw.split_y = 0;
	struct sway_workspace *ws = con->pending.workspace;
	for (int i = 0; ws && i < ws->floating->length; i++) {
		struct sway_container *other = ws->floating->items[i];
		if (other == con || !other->view || other->pending.tw_minimized ||
				other->pending.tw_maximized) {
			continue;
		}
		enum tw_snap o = other->tw.snap;
		if ((snap_left(snap) && snap_right(o)) || (snap_right(snap) && snap_left(o))) {
			con->tw.split_x = other->tw.split_x;
		}
		if (snap_quarter(snap) && snap_quarter(o) &&
				snap_left(snap) == snap_left(o) && snap != o) {
			con->tw.split_y = other->tw.split_y;
		}
	}
}

bool tw_container_fills_slot(struct sway_container *con) {
	return con && con->view && container_is_floating(con) && con->pending.tw_deco &&
		con->pending.fullscreen_mode == FULLSCREEN_NONE &&
		(con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE);
}

/*
 * A window that asks for another size on its own (a media player starting the
 * next video) keeps the corner it is at instead of jumping to the middle of
 * the screen, and stays on the screen it is on.
 */
void tw_floating_resize_in_place(struct sway_container *con) {
	double x = con->pending.x, y = con->pending.y;
	container_floating_resize_and_center(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (ws) {
		double max_x = ws->x + ws->width - con->pending.width;
		double max_y = ws->y + ws->height - con->pending.height;
		x = fmin(fmax(x, ws->x), fmax(ws->x, max_x));
		y = fmin(fmax(y, ws->y), fmax(ws->y, max_y));
	}
	container_floating_translate(con, x - con->pending.x, y - con->pending.y);
}

enum fill {
	FILL_OPAQUE,      // color holds the client's own background color
	FILL_TRANSLUCENT, // the client is see-through: the desktop stays behind it
	FILL_UNKNOWN,     // nothing drawn yet: ask again next time
};

/*
 * Color for the empty part of the slot of a snapped or maximized window: the
 * color of the client's bottom right corner, so the fill looks like its
 * background. A client that is see-through anywhere (a terminal with a
 * transparent background) gets no fill at all, which would cover the desktop
 * showing through it.
 */
static enum fill fill_color(struct sway_view *view, float color[4]) {
	struct wlr_texture *texture = view->surface ? wlr_surface_get_texture(view->surface) : NULL;
	if (!texture || view->geometry.width < 4 || view->geometry.height < 4) {
		return FILL_UNKNOWN;
	}
	int scale = view->surface->current.scale > 0 ? view->surface->current.scale : 1;
	// the bottom right corner first, then the other corners and the middle;
	// far enough inside to miss rounded corners
	static const double points[][2] = {
		{ 0.94, 0.94 }, { 0.06, 0.06 }, { 0.94, 0.06 }, { 0.06, 0.94 }, { 0.5, 0.5 },
	};
	bool found = false;
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
		int x = (int)((view->geometry.x + points[i][0] * view->geometry.width) * scale);
		int y = (int)((view->geometry.y + points[i][1] * view->geometry.height) * scale);
		if (x < 0 || y < 0 || x >= (int)texture->width || y >= (int)texture->height) {
			continue;
		}
		uint8_t pixel[4];
		struct wlr_texture_read_pixels_options options = {
			.data = pixel,
			.format = DRM_FORMAT_ARGB8888,
			.stride = 4,
			.src_box = { x, y, 1, 1 },
		};
		if (!wlr_texture_read_pixels(texture, &options)) {
			continue;
		}
		float alpha = pixel[3] / 255.0f;
		if (alpha < 0.95f) {
			return FILL_TRANSLUCENT;
		}
		if (!found) {
			// ARGB8888 is stored as B, G, R, A; the values are premultiplied
			color[0] = pixel[2] / 255.0f / alpha;
			color[1] = pixel[1] / 255.0f / alpha;
			color[2] = pixel[0] / 255.0f / alpha;
			color[3] = 1.0f;
			found = true;
		}
	}
	return found ? FILL_OPAQUE : FILL_UNKNOWN;
}

void tw_update_content_fill(struct sway_container *con) {
	struct sway_view *view = con->view;
	wlr_scene_node_set_position(&view->content_tree->node, 0, 0);
	int width = con->current.content_width, height = con->current.content_height;
	if (!wl_list_empty(&view->content_tree->children)) {
		struct wlr_box clip = view->using_csd ? (struct wlr_box){0} : (struct wlr_box){
			.x = view->geometry.x,
			.y = view->geometry.y,
			.width = width,
			.height = height,
		};
		wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, &clip);
	}
	struct wlr_scene_rect *bg = con->tw.content_bg;
	if (!bg) {
		return;
	}
	bool gap = view->geometry.width < width || view->geometry.height < height;
	// the view tree sits below the title bar inside the content tree; while a
	// window is still being mapped (e.g. snapped by session restore) it is not
	// attached there yet, and the fill follows on the next update
	struct wlr_scene_node *view_node = &view->scene_tree->node;
	if (!gap || view_node->parent != bg->node.parent) {
		wlr_scene_node_set_enabled(&bg->node, false);
		con->tw.content_bg_width = con->tw.content_bg_height = 0;
		return;
	}
	if (con->tw.content_bg_width != view->geometry.width ||
			con->tw.content_bg_height != view->geometry.height) {
		float color[4];
		enum fill fill = fill_color(view, color);
		con->tw.content_translucent = fill != FILL_OPAQUE;
		if (fill == FILL_OPAQUE) {
			wlr_scene_rect_set_color(bg, color);
			con->tw.content_bg_width = view->geometry.width;
			con->tw.content_bg_height = view->geometry.height;
		} else if (fill == FILL_TRANSLUCENT) {
			con->tw.content_bg_width = view->geometry.width;
			con->tw.content_bg_height = view->geometry.height;
		}
	}
	if (con->tw.content_translucent) {
		wlr_scene_node_set_enabled(&bg->node, false);
		return; // the desktop keeps showing through the client
	}
	wlr_scene_node_set_enabled(&bg->node, true);
	wlr_scene_node_set_position(&bg->node, view_node->x, view_node->y);
	wlr_scene_node_place_below(&bg->node, view_node);
	wlr_scene_rect_set_size(bg, width, height);
}

void tw_snap_to(struct sway_container *con, enum tw_snap snap) {
	if (!con || !con->view || !container_is_floating(con) || !con->pending.workspace) {
		return;
	}
	if (snap == TW_SNAP_TOP) {
		tw_maximize(con, true);
		return;
	}
	if (snap == TW_SNAP_NONE) {
		tw_restore(con);
		return;
	}
	if (!con->pending.tw_maximized && con->tw.snap == TW_SNAP_NONE) {
		con->tw.restore_box = current_box(con);
	}
	if (con->pending.tw_maximized) {
		con->pending.tw_maximized = false;
		tw_view_notify_maximized(con->view, false);
	}
	take_splits(con, snap);
	con->tw.snap = snap;
	struct wlr_box box = tw_container_snap_box(con, tw_workarea(con->pending.workspace));
	tw_set_box(con, &box);
	ipc_event_window(con, "snap");
	tw_animate_resize(con);
}

void tw_restore(struct sway_container *con) {
	if (!con || !con->view) {
		return;
	}
	if (con->pending.tw_maximized) {
		tw_maximize(con, false);
		return;
	}
	if (con->tw.snap != TW_SNAP_NONE) {
		con->tw.snap = TW_SNAP_NONE;
		struct wlr_box box = con->tw.restore_box;
		if (box.width <= 0 || box.height <= 0) {
			box = default_restore_box(con);
		}
		box = tw_fit_box(box, tw_workarea(con->pending.workspace));
		tw_set_box(con, &box);
		ipc_event_window(con, "restore");
	}
}

/*
 * A snapped window that was carried somewhere else, e.g. by the stick group,
 * is no longer at its edge. It keeps the size it has instead of falling back
 * to the one it had before it was snapped.
 */
void tw_unsnap_in_place(struct sway_container *con) {
	if (!con || !con->view || con->tw.snap == TW_SNAP_NONE) {
		return;
	}
	struct wlr_box box = current_box(con);
	con->tw.snap = TW_SNAP_NONE;
	tw_set_box(con, &box);
	ipc_event_window(con, "restore");
}

/*
 * Windows that are always on top go back over the others whenever anything is
 * raised, keeping the order they have among themselves.
 */
void tw_raise_above_windows(struct sway_workspace *ws) {
	if (!ws || !ws->floating) {
		return;
	}
	bool any = false;
	for (int i = 0; i < ws->floating->length && !any; i++) {
		any = ((struct sway_container *)ws->floating->items[i])->tw.above;
	}
	if (!any) {
		return;
	}
	list_t *above = create_list();
	for (int i = 0; i < ws->floating->length;) {
		struct sway_container *con = ws->floating->items[i];
		if (con->tw.above) {
			list_add(above, con);
			list_del(ws->floating, i);
		} else {
			i++;
		}
	}
	for (int i = 0; i < above->length; i++) {
		struct sway_container *con = above->items[i];
		list_add(ws->floating, con);
		if (con->scene_tree) {
			wlr_scene_node_raise_to_top(&con->scene_tree->node);
		}
	}
	list_free(above);
}

bool tw_snap(struct sway_container *con, const char *direction, char **error) {
	if (!con || !con->view) {
		*error = strdup("No window to snap");
		return false;
	}
	if (!container_is_floating(con)) {
		*error = strdup("snap only works on floating windows (window mode)");
		return false;
	}
	enum tw_snap current = con->pending.tw_maximized ? TW_SNAP_TOP : con->tw.snap;
	if (strcasecmp(direction, "left") == 0) {
		if (!tw_snap_across(con, WLR_DIRECTION_LEFT)) {
			tw_snap_to(con, current == TW_SNAP_RIGHT ? TW_SNAP_NONE : TW_SNAP_LEFT);
		}
	} else if (strcasecmp(direction, "right") == 0) {
		if (!tw_snap_across(con, WLR_DIRECTION_RIGHT)) {
			tw_snap_to(con, current == TW_SNAP_LEFT ? TW_SNAP_NONE : TW_SNAP_RIGHT);
		}
	} else if (strcasecmp(direction, "up") == 0) {
		if (current == TW_SNAP_BOTTOMLEFT) {
			tw_snap_to(con, TW_SNAP_LEFT);
		} else if (current == TW_SNAP_BOTTOMRIGHT) {
			tw_snap_to(con, TW_SNAP_RIGHT);
		} else if (current == TW_SNAP_LEFT) {
			tw_snap_to(con, TW_SNAP_TOPLEFT);
		} else if (current == TW_SNAP_RIGHT) {
			tw_snap_to(con, TW_SNAP_TOPRIGHT);
		} else {
			tw_snap_to(con, TW_SNAP_TOP);
		}
	} else if (strcasecmp(direction, "down") == 0) {
		if (current == TW_SNAP_TOPLEFT) {
			tw_snap_to(con, TW_SNAP_LEFT);
		} else if (current == TW_SNAP_TOPRIGHT) {
			tw_snap_to(con, TW_SNAP_RIGHT);
		} else if (current == TW_SNAP_LEFT) {
			tw_snap_to(con, TW_SNAP_BOTTOMLEFT);
		} else if (current == TW_SNAP_RIGHT) {
			tw_snap_to(con, TW_SNAP_BOTTOMRIGHT);
		} else if (current != TW_SNAP_NONE) {
			tw_restore(con);
		} else {
			tw_minimize(con, true);
		}
	} else if (strcasecmp(direction, "topleft") == 0) {
		tw_snap_to(con, TW_SNAP_TOPLEFT);
	} else if (strcasecmp(direction, "topright") == 0) {
		tw_snap_to(con, TW_SNAP_TOPRIGHT);
	} else if (strcasecmp(direction, "bottomleft") == 0) {
		tw_snap_to(con, TW_SNAP_BOTTOMLEFT);
	} else if (strcasecmp(direction, "bottomright") == 0) {
		tw_snap_to(con, TW_SNAP_BOTTOMRIGHT);
	} else if (strcasecmp(direction, "restore") == 0) {
		tw_restore(con);
	} else {
		*error = strdup("Expected snap left|right|up|down|topleft|topright|"
			"bottomleft|bottomright|restore");
		return false;
	}
	return true;
}

/*
 * The windows snapped beside one put into a part of a layout move their line
 * with it, as on Windows: the window on the other side gets the rest of the
 * width, the quarter above or below it the rest of the height, so nothing is
 * left covering the part just chosen.
 */
static void move_neighbours(struct sway_container *con, struct wlr_box area) {
	struct sway_workspace *ws = con->pending.workspace;
	enum tw_snap snap = con->tw.snap;
	for (int i = 0; ws && i < ws->floating->length; i++) {
		struct sway_container *other = ws->floating->items[i];
		if (other == con || !other->view || other->pending.tw_minimized ||
				other->pending.tw_maximized || other->tw.snap == TW_SNAP_NONE) {
			continue;
		}
		enum tw_snap o = other->tw.snap;
		bool moved = false;
		if ((snap_left(snap) && snap_right(o)) || (snap_right(snap) && snap_left(o))) {
			other->tw.split_x = con->tw.split_x;
			moved = true;
		}
		if (snap_quarter(snap) && snap_quarter(o) && snap_left(snap) == snap_left(o) &&
				snap != o) {
			other->tw.split_y = con->tw.split_y;
			moved = true;
		}
		if (moved) {
			struct wlr_box box = tw_container_snap_box(other, area);
			tw_set_box(other, &box);
		}
	}
}

bool tw_snap_at(struct sway_container *con, const char *slot, double fx, double fy,
		char **error) {
	static const struct {
		const char *name;
		enum tw_snap snap;
	} slots[] = {
		{ "left", TW_SNAP_LEFT }, { "right", TW_SNAP_RIGHT },
		{ "topleft", TW_SNAP_TOPLEFT }, { "topright", TW_SNAP_TOPRIGHT },
		{ "bottomleft", TW_SNAP_BOTTOMLEFT }, { "bottomright", TW_SNAP_BOTTOMRIGHT },
	};
	if (!con || !con->view || !container_is_floating(con) || !con->pending.workspace) {
		*error = strdup("snap only works on floating windows (window mode)");
		return false;
	}
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if (strcasecmp(slot, slots[i].name) == 0) {
			tw_snap_to(con, slots[i].snap);
			con->tw.split_x = fx;
			con->tw.split_y = fy;
			struct wlr_box area = tw_workarea(con->pending.workspace);
			struct wlr_box box = tw_container_snap_box(con, area);
			tw_set_box(con, &box);
			move_neighbours(con, area);
			return true;
		}
	}
	*error = strdup("Expected snap left|right|topleft|topright|bottomleft|bottomright "
		"<fraction> [<fraction>]");
	return false;
}

/* A dialog, a splash screen or anything else of a size of its own. */
bool tw_view_is_dialog(struct sway_view *view) {
	switch (view->type) {
	case SWAY_VIEW_XDG_SHELL:
		if (view->wlr_xdg_toplevel->parent) {
			return true;
		}
		break;
#if WLR_HAS_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		if (view->wlr_xwayland_surface->parent || view->wlr_xwayland_surface->modal) {
			return true;
		}
		break;
#endif
	}
	double min_w, max_w, min_h, max_h;
	view_get_constraints(view, &min_w, &max_w, &min_h, &max_h);
	return min_w > 0 && min_w == max_w && min_h > 0 && min_h == max_h;
}

#define NEARLY_FULL 0.9 // of the work area, in both directions

/*
 * A new window about as big as the screen opens maximized, as on Windows,
 * instead of a few pixels short of it (an app asking for its size on a bigger
 * screen, or for the whole screen before the taskbar is taken off). Restoring
 * it gives a window of the usual size in the middle.
 */
void tw_maximize_if_nearly_full(struct sway_container *con) {
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || !con->view || !container_is_floating(con) || con->pending.tw_maximized ||
			con->tw.snap != TW_SNAP_NONE ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE ||
			tw_view_is_dialog(con->view)) {
		return;
	}
	struct wlr_box area = tw_workarea(ws);
	if (con->pending.width < area.width * NEARLY_FULL ||
			con->pending.height < area.height * NEARLY_FULL) {
		return;
	}
	tw_maximize(con, true);
	con->tw.restore_box = default_restore_box(con);
}

/* A new window gets a place and size of its own after all (remembered or
 * from the session): it was maximized only for being nearly full. */
void tw_unmaximize_new(struct sway_container *con) {
	if (con->pending.tw_maximized) {
		con->pending.tw_maximized = false;
		tw_view_notify_maximized(con->view, false);
	}
}

void tw_place_new_window(struct sway_container *con) {
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || !ws->output || !con->view) {
		return;
	}
	static int cascade = 0;
	struct wlr_box area = tw_workarea(ws);
	con->pending.tw_maximized = false;
	con->tw.snap = TW_SNAP_NONE;
	tw_container_update_deco_state(con);
	struct tw_insets in = con->pending.tw_deco ? tw_deco_insets(false) :
		(struct tw_insets){ 0 };

	struct sway_view *view = con->view;
	int cw = view->natural_width, ch = view->natural_height;
	if (cw < 120 || ch < 80) {
		cw = area.width * 0.6;
		ch = area.height * 0.65;
	}
	int max_w = area.width - in.left - in.right;
	int max_h = area.height - in.top - in.bottom;
	cw = cw > max_w ? max_w : cw;
	ch = ch > max_h ? max_h : ch;
	int w = cw + in.left + in.right;
	int h = ch + in.top + in.bottom;

	int step = in.top > 0 ? in.top : 26;
	struct wlr_box box = {
		area.x + (area.width - w) / 2 + (cascade - 2) * step,
		area.y + (area.height - h) / 2 + (cascade - 2) * step,
		w, h,
	};
	cascade = (cascade + 1) % 5;
	box = tw_fit_box(box, area);
	tw_set_box(con, &box);
	tw_maximize_if_nearly_full(con);
}

/* ---------- arranging ---------- */

static list_t *collect_floating_windows(struct sway_workspace *ws) {
	list_t *list = create_list();
	for (int i = 0; i < ws->floating->length; i++) {
		struct sway_container *c = ws->floating->items[i];
		if (c->view && !c->pending.tw_minimized &&
				c->pending.fullscreen_mode == FULLSCREEN_NONE) {
			list_add(list, c);
		}
	}
	return list;
}

static void collect_tiling_views(struct sway_container *con, void *data) {
	if (con->view && !container_is_floating_or_child(con)) {
		list_add(data, con);
	}
}

static void floating_arrange(list_t *windows, struct wlr_box area, const char *how) {
	int n = windows->length;
	if (n == 0) {
		return;
	}
	for (int i = 0; i < n; i++) {
		struct sway_container *c = windows->items[i];
		if (c->pending.tw_maximized) {
			c->pending.tw_maximized = false;
			tw_view_notify_maximized(c->view, false);
		}
		c->tw.snap = TW_SNAP_NONE;
	}

	if (strcasecmp(how, "cascade") == 0) {
		struct tw_insets in = tw_deco_insets(false);
		int step = in.top > 0 ? in.top : 26;
		int w = area.width * 0.6, h = area.height * 0.6;
		int steps_x = (area.width - w) / step + 1;
		int steps_y = (area.height - h) / step + 1;
		int max_steps = steps_x < steps_y ? steps_x : steps_y;
		if (max_steps < 1) {
			max_steps = 1;
		}
		for (int i = 0; i < n; i++) {
			int k = i % max_steps;
			struct wlr_box box = { area.x + k * step, area.y + k * step, w, h };
			tw_set_box(windows->items[i], &box);
			container_raise_floating(windows->items[i]);
		}
		return;
	}

	int cols, rows;
	if (strcasecmp(how, "horizontal") == 0) {
		cols = n;
		rows = 1;
	} else if (strcasecmp(how, "vertical") == 0) {
		cols = 1;
		rows = n;
	} else {
		tw_grid_choose(n, area.width, area.height, TW_GRID_ASPECT, &cols, &rows);
	}

	for (int i = 0; i < n; i++) {
		int row = i / cols;
		int col = i % cols;
		int in_row = row == rows - 1 ? n - cols * (rows - 1) : cols;
		int cell_w = area.width / in_row;
		int cell_h = area.height / rows;
		struct wlr_box box = {
			area.x + col * cell_w,
			area.y + row * cell_h,
			col == in_row - 1 ? area.width - col * cell_w : cell_w,
			row == rows - 1 ? area.height - row * cell_h : cell_h,
		};
		tw_set_box(windows->items[i], &box);
	}
}

static void destroy_empty_tree(struct sway_container *con) {
	while (con->pending.children && con->pending.children->length) {
		destroy_empty_tree(con->pending.children->items[0]);
	}
	container_begin_destroy(con);
}

static void tiling_arrange(struct sway_workspace *ws, const char *how) {
	list_t *views = create_list();
	workspace_for_each_container(ws, collect_tiling_views, views);
	int n = views->length;
	if (n == 0) {
		list_free(views);
		return;
	}

	if (strcasecmp(how, "cascade") == 0) {
		for (int i = 0; i < n; i++) {
			container_set_floating(views->items[i], true);
		}
		floating_arrange(views, tw_workarea(ws), "cascade");
		list_free(views);
		arrange_workspace(ws);
		return;
	}

	for (int i = 0; i < n; i++) {
		container_detach(views->items[i]);
	}
	list_t *leftovers = create_list();
	list_cat(leftovers, ws->tiling);
	for (int i = 0; i < leftovers->length; i++) {
		struct sway_container *c = leftovers->items[i];
		if (!c->view && !c->node.destroying) {
			destroy_empty_tree(c);
		}
	}
	list_free(leftovers);

	int cols = n, rows = 1;
	if (strcasecmp(how, "vertical") == 0) {
		cols = 1;
		rows = n;
	} else if (strcasecmp(how, "optimal") == 0) {
		struct wlr_box area = tw_workarea(ws);
		tw_grid_choose(n, area.width, area.height, TW_GRID_ASPECT, &cols, &rows);
	}

	if (rows == 1 || cols == 1) {
		ws->layout = rows == 1 ? L_HORIZ : L_VERT;
		for (int i = 0; i < n; i++) {
			struct sway_container *c = views->items[i];
			c->width_fraction = 0;
			c->height_fraction = 0;
			workspace_add_tiling(ws, c);
		}
	} else {
		ws->layout = L_VERT;
		for (int r = 0; r < rows; r++) {
			struct sway_container *row = container_create(NULL);
			row->pending.layout = L_HORIZ;
			workspace_add_tiling(ws, row);
			for (int c = 0; c < cols; c++) {
				int idx = r * cols + c;
				if (idx >= n) {
					break;
				}
				struct sway_container *con = views->items[idx];
				con->width_fraction = 0;
				con->height_fraction = 0;
				container_add_child(row, con);
			}
		}
	}
	list_free(views);
	arrange_workspace(ws);
}

bool tw_arrange_workspace(struct sway_workspace *ws, const char *how, char **error) {
	if (!ws) {
		*error = strdup("No workspace");
		return false;
	}
	if (strcasecmp(how, "cascade") != 0 && strcasecmp(how, "horizontal") != 0 &&
			strcasecmp(how, "vertical") != 0 && strcasecmp(how, "optimal") != 0) {
		*error = strdup("Expected arrange cascade|horizontal|vertical|optimal");
		return false;
	}
	if (tw_mode == TW_MODE_WINDOW) {
		list_t *windows = collect_floating_windows(ws);
		floating_arrange(windows, tw_workarea(ws), how);
		list_free(windows);
		arrange_workspace(ws);
	} else {
		tiling_arrange(ws, how);
	}
	return true;
}

/* ---------- show desktop ---------- */

static list_t *desktop_hidden = NULL; // size_t node ids

static bool match_id(struct sway_container *con, void *data) {
	return con->node.id == *(size_t *)data;
}

bool tw_show_desktop(struct sway_workspace *ws, char **error) {
	if (!ws) {
		*error = strdup("No workspace");
		return false;
	}
	if (!desktop_hidden) {
		desktop_hidden = create_list();
	}
	// every screen shows the desktop at once, as on Windows
	list_t *visible = create_list();
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_workspace *shown = output_get_active_workspace(root->outputs->items[i]);
		if (shown) {
			list_t *windows = collect_floating_windows(shown);
			list_cat(visible, windows);
			list_free(windows);
		}
	}
	if (visible->length > 0) {
		list_free_items_and_destroy(desktop_hidden);
		desktop_hidden = create_list();
		for (int i = 0; i < visible->length; i++) {
			struct sway_container *c = visible->items[i];
			size_t *id = malloc(sizeof(size_t));
			*id = c->node.id;
			list_add(desktop_hidden, id);
			tw_minimize(c, true);
		}
	} else {
		for (int i = 0; i < desktop_hidden->length; i++) {
			struct sway_container *c = root_find_container(match_id, desktop_hidden->items[i]);
			if (c && c->pending.tw_minimized) {
				tw_minimize(c, false);
				container_raise_floating(c);
			}
		}
		list_free_items_and_destroy(desktop_hidden);
		desktop_hidden = create_list();
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_container *next = tw_next_focus_candidate(seat, ws, NULL);
		if (next) {
			seat_set_focus_container(seat, next);
		}
	}
	list_free(visible);
	return true;
}

/* ---------- mode conversion ---------- */

static void for_each_workspace(void (*fn)(struct sway_workspace *ws)) {
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		for (int j = 0; j < output->workspaces->length; j++) {
			fn(output->workspaces->items[j]);
		}
	}
}

static void workspace_to_window_mode(struct sway_workspace *ws) {
	list_t *views = create_list();
	workspace_for_each_container(ws, collect_tiling_views, views);
	for (int i = 0; i < views->length; i++) {
		struct sway_container *con = views->items[i];
		if (con->pending.fullscreen_mode != FULLSCREEN_NONE) {
			continue;
		}
		con->tw.auto_floated = true;
		container_set_floating(con, true);
		if (con->tw.has_window_geometry) {
			struct wlr_box box = tw_fit_box(con->tw.window_geometry, tw_workarea(ws));
			tw_set_box(con, &box);
			if (con->tw.window_geometry_maximized) {
				tw_maximize(con, true);
			} else if (con->tw.window_geometry_snap != TW_SNAP_NONE) {
				tw_snap_to(con, con->tw.window_geometry_snap);
			}
		} else {
			tw_place_new_window(con);
		}
	}
	list_free(views);
	for (int i = 0; i < ws->floating->length; i++) {
		struct sway_container *con = ws->floating->items[i];
		tw_container_update_deco_state(con);
		if (con->view) {
			view_autoconfigure(con->view);
		}
		node_set_dirty(&con->node);
	}
	arrange_workspace(ws);
}

static void workspace_to_tile_mode(struct sway_workspace *ws) {
	list_t *floaters = create_list();
	list_cat(floaters, ws->floating);
	for (int i = 0; i < floaters->length; i++) {
		struct sway_container *con = floaters->items[i];
		if (!con->view) {
			continue;
		}
		if (con->pending.tw_minimized) {
			con->pending.tw_minimized = false;
			view_notify_minimized(con->view, false);
		}
		if (!con->tw.auto_floated) {
			tw_container_update_deco_state(con);
			view_autoconfigure(con->view);
			node_set_dirty(&con->node);
			continue;
		}
		bool maximized = con->pending.tw_maximized;
		con->tw.has_window_geometry = true;
		con->tw.window_geometry_maximized = maximized;
		con->tw.window_geometry_snap = maximized ? TW_SNAP_NONE : con->tw.snap;
		con->tw.window_geometry = maximized || con->tw.snap != TW_SNAP_NONE ?
			con->tw.restore_box : current_box(con);
		if (maximized) {
			con->pending.tw_maximized = false;
			tw_view_notify_maximized(con->view, false);
		}
		con->tw.snap = TW_SNAP_NONE;
		con->tw.auto_floated = false;
		container_set_floating(con, false);
	}
	list_free(floaters);
	arrange_workspace(ws);
}

/*
 * A window gets its border style when it opens. Switching mode swaps the
 * config, so windows that were already open would keep the other mode's style
 * (a tiled window with the title bar of window mode): give them the new one.
 */
static void reset_border(struct sway_container *con, void *data) {
	if (!con->view) {
		return;
	}
	bool floating = container_is_floating(con);
	enum sway_container_border border = floating ? config->floating_border : config->border;
	con->saved_border = border;
	if (con->pending.border == B_CSD) {
		return; // the client draws its own frame
	}
	con->pending.border = border;
	con->pending.border_thickness = floating ? config->floating_border_thickness :
		config->border_thickness;
	tw_container_update_deco_state(con);
	view_autoconfigure(con->view);
	node_set_dirty(&con->node);
}

void tw_reset_borders(void) {
	root_for_each_container(reset_border, NULL);
}

static struct timespec window_mode_since;

void tw_convert_to_window_mode(void) {
	clock_gettime(CLOCK_MONOTONIC, &window_mode_since);
	for_each_workspace(workspace_to_window_mode);
}

void tw_workarea_changed(struct sway_output *output) {
	if (tw_mode != TW_MODE_WINDOW) {
		return;
	}
	// The taskbar moves its exclusive zone just after a mode switch, which
	// shifts floating windows: put them back where they were.
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	bool after_switch = now.tv_sec - window_mode_since.tv_sec < 3;
	for (int i = 0; i < output->workspaces->length; i++) {
		struct sway_workspace *ws = output->workspaces->items[i];
		struct wlr_box area = tw_workarea(ws);
		bool changed = false;
		for (int j = 0; j < ws->floating->length; j++) {
			struct sway_container *con = ws->floating->items[j];
			if (!con->view) {
				continue;
			}
			struct wlr_box box;
			if (con->pending.tw_maximized) {
				box = area;
			} else if (con->tw.snap != TW_SNAP_NONE) {
				box = tw_container_snap_box(con, area);
			} else if (after_switch && con->tw.has_window_geometry) {
				box = tw_fit_box(con->tw.window_geometry, area);
			} else {
				continue;
			}
			tw_set_box(con, &box);
			changed = true;
		}
		if (changed) {
			arrange_workspace(ws);
		}
	}
}

void tw_convert_to_tile_mode(void) {
	for_each_workspace(workspace_to_tile_mode);
}

/* ---------- drag snapping ---------- */

/*
 * Where a dragged window snaps. The outer edges of the screens snap at once:
 * the sides for halves, the top to maximize, and the corners (along any of
 * the edges that meet there) for quarters. An edge shared with another screen
 * lets the pointer through, so a window carried across does not snap on the
 * way; holding the pointer at such an edge for a moment snaps there all the
 * same, so the halves and quarters of every screen can be reached. The window
 * snaps on the screen the pointer is on.
 */
#define SNAP_EDGE 2       // pixels from an outer edge that snap
#define SNAP_SHARED 10    // pixels from an edge between screens where holding counts
#define SNAP_CORNER 64    // how far along an edge from a corner still means the corner
#define SNAP_DWELL_MS 400 // how long to hold at an edge between screens

static struct {
	struct wlr_scene_rect *rect;
	enum tw_snap snap;
	struct sway_container *con;
	struct sway_output *output; // where it snaps
	double lx, ly;              // the pointer, for looking again after holding
	bool dwelling;              // held at an edge between screens
	struct timespec dwell_since;
	struct wl_event_source *dwell_timer;
} preview;

static bool screen_beyond(double lx, double ly) {
	return wlr_output_layout_output_at(root->output_layout, lx, ly) != NULL;
}

static int64_t dwell_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - preview.dwell_since.tv_sec) * 1000 +
		(now.tv_nsec - preview.dwell_since.tv_nsec) / 1000000;
}

static int handle_dwell(void *data) {
	if (preview.con) {
		tw_snap_preview_update(preview.con, preview.lx, preview.ly); // held long enough now
	}
	return 0;
}

/*
 * The zone of the pointer. *at_shared tells whether it sits at an edge between
 * screens, *near whether it is close to any edge; once held, the edges stay as
 * wide as those between screens, so sliding along one keeps it.
 */
static enum tw_snap snap_zone(double lx, double ly, bool held_long, bool *at_shared,
		bool *near) {
	*at_shared = *near = false;
	struct wlr_output *wlr_output = wlr_output_layout_output_at(root->output_layout, lx, ly);
	if (!wlr_output) {
		return TW_SNAP_NONE;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(root->output_layout, wlr_output, &box);
	double x0 = box.x, y0 = box.y, x1 = box.x + box.width - 1, y1 = box.y + box.height - 1;
	bool outer_left = !screen_beyond(box.x - 1, ly);
	bool outer_right = !screen_beyond(box.x + box.width, ly);
	bool outer_top = !screen_beyond(lx, box.y - 1);
	bool outer_bottom = !screen_beyond(lx, box.y + box.height);
	*near = lx <= x0 + SNAP_SHARED || lx >= x1 - SNAP_SHARED || ly <= y0 + SNAP_SHARED ||
		ly >= y1 - SNAP_SHARED;
	int outer = held_long ? SNAP_SHARED : SNAP_EDGE;
	bool near_left = lx <= x0 + (outer_left ? outer : SNAP_SHARED);
	bool near_right = lx >= x1 - (outer_right ? outer : SNAP_SHARED);
	bool near_top = ly <= y0 + (outer_top ? outer : SNAP_SHARED);
	bool near_bottom = ly >= y1 - (outer_bottom ? outer : SNAP_SHARED);
	*at_shared = (near_left && !outer_left) || (near_right && !outer_right) ||
		(near_top && !outer_top) || (near_bottom && !outer_bottom);
	bool left = near_left && (outer_left || held_long);
	bool right = near_right && (outer_right || held_long);
	bool top = near_top && (outer_top || held_long);
	bool bottom = near_bottom && (outer_bottom || held_long);
	int corner = SNAP_CORNER;
	if (corner > box.width / 4) {
		corner = box.width / 4;
	}
	if (corner > box.height / 4) {
		corner = box.height / 4;
	}
	bool left_part = lx <= x0 + corner, right_part = lx >= x1 - corner;
	bool top_part = ly <= y0 + corner, bottom_part = ly >= y1 - corner;
	if ((left || top) && left_part && top_part) {
		return TW_SNAP_TOPLEFT;
	}
	if ((right || top) && right_part && top_part) {
		return TW_SNAP_TOPRIGHT;
	}
	if ((left || bottom) && left_part && bottom_part) {
		return TW_SNAP_BOTTOMLEFT;
	}
	if ((right || bottom) && right_part && bottom_part) {
		return TW_SNAP_BOTTOMRIGHT;
	}
	if (left) {
		return TW_SNAP_LEFT;
	}
	if (right) {
		return TW_SNAP_RIGHT;
	}
	if (top) {
		return TW_SNAP_TOP;
	}
	return TW_SNAP_NONE;
}

enum tw_snap tw_snap_zone(double lx, double ly) {
	bool shared, near;
	return snap_zone(lx, ly, false, &shared, &near);
}

void tw_snap_preview_update(struct sway_container *con, double lx, double ly) {
	enum tw_snap snap = TW_SNAP_NONE;
	preview.con = con;
	preview.lx = lx;
	preview.ly = ly;
	if (tw_mode == TW_MODE_WINDOW && config->tw_snap) {
		bool shared, near;
		snap = snap_zone(lx, ly, preview.dwelling && dwell_ms() >= SNAP_DWELL_MS, &shared,
			&near);
		if (shared && !preview.dwelling) {
			// held at an edge between screens: it snaps there after a moment
			preview.dwelling = true;
			clock_gettime(CLOCK_MONOTONIC, &preview.dwell_since);
			if (!preview.dwell_timer) {
				preview.dwell_timer = wl_event_loop_add_timer(server.wl_event_loop,
					handle_dwell, NULL);
			}
			if (preview.dwell_timer) {
				wl_event_source_timer_update(preview.dwell_timer, SNAP_DWELL_MS + 10);
			}
		} else if (!near && preview.dwelling) {
			// (holding lasts while it slides along the edges)
			preview.dwelling = false;
			if (preview.dwell_timer) {
				wl_event_source_timer_update(preview.dwell_timer, 0);
			}
		}
	}
	struct sway_workspace *ws = NULL;
	struct wlr_output *wlr_output = wlr_output_layout_output_at(root->output_layout, lx, ly);
	struct sway_output *output = wlr_output ? output_from_wlr_output(wlr_output) : NULL;
	if (snap == preview.snap && (snap == TW_SNAP_NONE || output == preview.output)) {
		return;
	}
	preview.snap = snap;
	preview.output = snap == TW_SNAP_NONE ? NULL : output;
	if (snap == TW_SNAP_NONE) {
		if (preview.rect) {
			wlr_scene_node_set_enabled(&preview.rect->node, false);
		}
		return;
	}
	if (output) {
		ws = output_get_active_workspace(output);
	}
	if (!ws) {
		preview.snap = TW_SNAP_NONE;
		preview.output = NULL;
		return;
	}
	struct wlr_box area = tw_workarea(ws);
	struct wlr_box box = snap == TW_SNAP_TOP ? area : tw_snap_box(area, snap);
	uint32_t c = tw_style_snap_color(tw_theme);
	float a = (c & 0xff) / 255.0f;
	float color[4] = {
		(c >> 24 & 0xff) / 255.0f * a, (c >> 16 & 0xff) / 255.0f * a,
		(c >> 8 & 0xff) / 255.0f * a, a,
	};
	if (!preview.rect) {
		preview.rect = wlr_scene_rect_create(root->layers.floating, 1, 1, color);
		if (!preview.rect) {
			return;
		}
	}
	wlr_scene_rect_set_color(preview.rect, color);
	wlr_scene_rect_set_size(preview.rect, box.width, box.height);
	wlr_scene_node_set_position(&preview.rect->node, box.x, box.y);
	wlr_scene_node_set_enabled(&preview.rect->node, true);
	if (con && con->scene_tree->node.parent == root->layers.floating) {
		wlr_scene_node_place_below(&preview.rect->node, &con->scene_tree->node);
	}
}

enum tw_snap tw_snap_preview_finish(struct sway_output **output) {
	enum tw_snap snap = preview.snap;
	if (output) {
		*output = preview.output;
	}
	preview.snap = TW_SNAP_NONE;
	preview.con = NULL;
	preview.output = NULL;
	preview.dwelling = false;
	if (preview.dwell_timer) {
		wl_event_source_timer_update(preview.dwell_timer, 0);
	}
	if (preview.rect) {
		wlr_scene_node_set_enabled(&preview.rect->node, false);
	}
	return snap;
}
