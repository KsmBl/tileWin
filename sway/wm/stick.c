/*
 * Window mode: windows stick to the edges of the screen and of other windows
 * while they are moved or resized, windows that touch each other move together
 * while the group modifier (window_group_modifier) is held, and double-clicking
 * a side of a frame stretches the window to the next window or the screen edge.
 */
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include "sway/config.h"
#include "sway/input/seat.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"

#define TOUCH 2 // windows this close to each other count as touching

struct rect {
	double x0, y0, x1, y1;
};

static struct rect rect_of(struct sway_container *con) {
	return (struct rect){
		con->pending.x, con->pending.y,
		con->pending.x + con->pending.width, con->pending.y + con->pending.height,
	};
}

/* Length of the overlap of [a0, a1] and [b0, b1]; negative: the gap between them. */
static double overlap(double a0, double a1, double b0, double b1) {
	return fmin(a1, b1) - fmax(a0, b0);
}

static bool in_list(list_t *list, struct sway_container *con) {
	for (int i = 0; list && i < list->length; i++) {
		if (list->items[i] == con) {
			return true;
		}
	}
	return false;
}

/* A visible window of the workspace another window can stick to. */
static bool is_target(struct sway_container *con, struct sway_container *other) {
	return other != con && other->view && !other->node.destroying &&
		!other->pending.tw_minimized && !other->pending.tw_maximized &&
		other->pending.fullscreen_mode == FULLSCREEN_NONE;
}

static bool active(struct sway_container *con) {
	return tw_mode == TW_MODE_WINDOW && config->tw_stick && config->tw_stick_distance > 0 &&
		con && con->pending.workspace && container_is_floating(con);
}

static void consider(double edge, double target, double *delta) {
	double d = target - edge;
	if (fabs(d) < fabs(*delta)) {
		*delta = d;
	}
}

/*
 * How far the given edges of r have to move to stick to the work area or a
 * window edge within the stick distance (0 if nothing is near).
 */
static void stick_deltas(struct sway_container *con, list_t *exclude, struct rect r,
		enum wlr_edges edges, double *dx, double *dy) {
	double dist = config->tw_stick_distance;
	*dx = *dy = dist + 1;
	struct wlr_box area = tw_workarea(con->pending.workspace);
	if (edges & WLR_EDGE_LEFT) {
		consider(r.x0, area.x, dx);
	}
	if (edges & WLR_EDGE_RIGHT) {
		consider(r.x1, area.x + area.width, dx);
	}
	if (edges & WLR_EDGE_TOP) {
		consider(r.y0, area.y, dy);
	}
	if (edges & WLR_EDGE_BOTTOM) {
		consider(r.y1, area.y + area.height, dy);
	}

	list_t *floating = con->pending.workspace->floating;
	for (int i = 0; i < floating->length; i++) {
		struct sway_container *other = floating->items[i];
		if (!is_target(con, other) || in_list(exclude, other)) {
			continue;
		}
		struct rect o = rect_of(other);
		double ov = overlap(r.y0, r.y1, o.y0, o.y1);
		double oh = overlap(r.x0, r.x1, o.x0, o.x1);
		// side by side: an edge meets the opposite edge of the other window
		if (ov > 0) {
			if (edges & WLR_EDGE_LEFT) {
				consider(r.x0, o.x1, dx);
			}
			if (edges & WLR_EDGE_RIGHT) {
				consider(r.x1, o.x0, dx);
			}
		}
		if (oh > 0) {
			if (edges & WLR_EDGE_TOP) {
				consider(r.y0, o.y1, dy);
			}
			if (edges & WLR_EDGE_BOTTOM) {
				consider(r.y1, o.y0, dy);
			}
		}
		// stacked: line up with the same edge of the window above or below
		if (fabs(ov) <= dist && oh > -dist) {
			if (edges & WLR_EDGE_LEFT) {
				consider(r.x0, o.x0, dx);
			}
			if (edges & WLR_EDGE_RIGHT) {
				consider(r.x1, o.x1, dx);
			}
		}
		if (fabs(oh) <= dist && ov > -dist) {
			if (edges & WLR_EDGE_TOP) {
				consider(r.y0, o.y0, dy);
			}
			if (edges & WLR_EDGE_BOTTOM) {
				consider(r.y1, o.y1, dy);
			}
		}
	}
	if (fabs(*dx) > dist) {
		*dx = 0;
	}
	if (fabs(*dy) > dist) {
		*dy = 0;
	}
}

void tw_stick_move(struct sway_container *con, list_t *exclude, double *x, double *y) {
	if (!active(con) || con->pending.tw_maximized) {
		return;
	}
	struct rect r = { *x, *y, *x + con->pending.width, *y + con->pending.height };
	double dx, dy;
	stick_deltas(con, exclude, r, WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP |
		WLR_EDGE_BOTTOM, &dx, &dy);
	*x += dx;
	*y += dy;
}

void tw_stick_resize(struct sway_container *con, enum wlr_edges edges,
		const struct wlr_box *ref, double *grow_width, double *grow_height) {
	if (!active(con)) {
		return;
	}
	struct rect r = { ref->x, ref->y, ref->x + ref->width, ref->y + ref->height };
	if (edges & WLR_EDGE_LEFT) {
		r.x0 -= *grow_width;
	} else if (edges & WLR_EDGE_RIGHT) {
		r.x1 += *grow_width;
	}
	if (edges & WLR_EDGE_TOP) {
		r.y0 -= *grow_height;
	} else if (edges & WLR_EDGE_BOTTOM) {
		r.y1 += *grow_height;
	}
	double dx, dy;
	stick_deltas(con, NULL, r, edges, &dx, &dy);
	*grow_width += edges & WLR_EDGE_LEFT ? -dx : dx;
	*grow_height += edges & WLR_EDGE_TOP ? -dy : dy;
}

static bool touching(struct rect a, struct rect b) {
	bool side = overlap(a.y0, a.y1, b.y0, b.y1) > 0 &&
		(fabs(a.x1 - b.x0) <= TOUCH || fabs(b.x1 - a.x0) <= TOUCH);
	bool stacked = overlap(a.x0, a.x1, b.x0, b.x1) > 0 &&
		(fabs(a.y1 - b.y0) <= TOUCH || fabs(b.y1 - a.y0) <= TOUCH);
	return side || stacked;
}

list_t *tw_stick_group(struct sway_container *con) {
	if (tw_mode != TW_MODE_WINDOW || !config->tw_group_modifier ||
			!con || !con->pending.workspace || !container_is_floating(con)) {
		return NULL;
	}
	// windows that touch the window, the windows touching those, and so on;
	// windows snapped to an edge or a corner belong to the group as well and
	// come along in the size they have
	list_t *group = create_list();
	list_add(group, con);
	list_t *floating = con->pending.workspace->floating;
	for (int i = 0; i < group->length; i++) {
		struct rect r = rect_of(group->items[i]);
		for (int j = 0; j < floating->length; j++) {
			struct sway_container *other = floating->items[j];
			if (is_target(con, other) && !in_list(group, other) &&
					touching(r, rect_of(other))) {
				list_add(group, other);
			}
		}
	}
	list_del(group, 0);
	if (group->length == 0) {
		list_free(group);
		return NULL;
	}
	return group;
}

bool tw_stick_group_modifier_held(struct sway_seat *seat) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	uint32_t mod = config->tw_group_modifier;
	return mod && keyboard && (wlr_keyboard_get_modifiers(keyboard) & mod) == mod;
}

static bool box_equal(const struct wlr_box *a, const struct wlr_box *b) {
	return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

bool tw_expand(struct sway_container *con, enum wlr_edges edge) {
	bool horizontal = edge == WLR_EDGE_LEFT || edge == WLR_EDGE_RIGHT;
	if (!horizontal && edge != WLR_EDGE_TOP && edge != WLR_EDGE_BOTTOM) {
		return false;
	}
	if (!con || !con->view || !container_is_floating(con) || !con->pending.workspace ||
			con->pending.tw_maximized) {
		return false;
	}
	struct wlr_box box = {
		(int)con->pending.x, (int)con->pending.y,
		(int)con->pending.width, (int)con->pending.height,
	};
	// a second double-click on the same axis brings the old size back
	int axis = horizontal ? 1 : 2;
	if (con->tw.expand_axis == axis && box_equal(&box, &con->tw.expanded)) {
		con->tw.expand_axis = 0;
		con->tw.snap = TW_SNAP_NONE;
		tw_set_box(con, &con->tw.expand_restore);
		tw_animate_resize(con);
		return true;
	}

	struct wlr_box area = tw_workarea(con->pending.workspace);
	struct rect r = rect_of(con);
	double lo = horizontal ? area.x : area.y;
	double hi = horizontal ? area.x + area.width : area.y + area.height;
	double start = horizontal ? r.x0 : r.y0, end = horizontal ? r.x1 : r.y1;
	list_t *floating = con->pending.workspace->floating;
	for (int i = 0; i < floating->length; i++) {
		struct sway_container *other = floating->items[i];
		if (!is_target(con, other)) {
			continue;
		}
		struct rect o = rect_of(other);
		// only windows beside it on that axis stop it; overlapping ones don't
		if ((horizontal ? overlap(r.y0, r.y1, o.y0, o.y1) : overlap(r.x0, r.x1, o.x0, o.x1)) <= 0) {
			continue;
		}
		double o0 = horizontal ? o.x0 : o.y0, o1 = horizontal ? o.x1 : o.y1;
		if (o1 <= start + TOUCH) {
			lo = fmax(lo, o1);
		} else if (o0 >= end - TOUCH) {
			hi = fmin(hi, o0);
		}
	}
	// grow only: a window already past the limit keeps that side
	double new_start = fmin(start, lo), new_end = fmax(end, hi);
	struct wlr_box expanded = box;
	if (horizontal) {
		expanded.x = (int)round(new_start);
		expanded.width = (int)round(new_end) - expanded.x;
	} else {
		expanded.y = (int)round(new_start);
		expanded.height = (int)round(new_end) - expanded.y;
	}
	if (box_equal(&box, &expanded)) {
		return false;
	}
	con->tw.expand_restore = box;
	con->tw.expanded = expanded;
	con->tw.expand_axis = axis;
	con->tw.snap = TW_SNAP_NONE;
	tw_set_box(con, &expanded);
	tw_animate_resize(con);
	return true;
}
