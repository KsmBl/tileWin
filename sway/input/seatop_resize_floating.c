#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/tilewin.h"
#include "sway/tree/arrange.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/tree/container.h"
#include "list.h"

struct seatop_resize_floating_event {
	struct sway_container *con;
	enum wlr_edges edge;
	bool preserve_ratio;
	double ref_lx, ref_ly;         // cursor's x/y at start of op
	double ref_width, ref_height;  // container's size at start of op
	double ref_con_lx, ref_con_ly; // container's x/y at start of op
};

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	struct sway_container *con = e->con;

	if (seat->cursor->pressed_button_count == 0) {
		container_set_resizing(con, false);
		arrange_container(con); // Send configure w/o resizing hint
		transaction_commit_dirty();
		tw_session_changed();
		seatop_begin_default(seat);
	}
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	struct sway_container *con = e->con;
	enum wlr_edges edge = e->edge;
	struct sway_cursor *cursor = seat->cursor;

	// The amount the mouse has moved since the start of the resize operation
	// Positive is down/right
	double mouse_move_x = cursor->cursor->x - e->ref_lx;
	double mouse_move_y = cursor->cursor->y - e->ref_ly;

	if (edge == WLR_EDGE_TOP || edge == WLR_EDGE_BOTTOM) {
		mouse_move_x = 0;
	}
	if (edge == WLR_EDGE_LEFT || edge == WLR_EDGE_RIGHT) {
		mouse_move_y = 0;
	}

	double grow_width = edge & WLR_EDGE_LEFT ? -mouse_move_x : mouse_move_x;
	double grow_height = edge & WLR_EDGE_TOP ? -mouse_move_y : mouse_move_y;

	if (!e->preserve_ratio) {
		struct wlr_box ref = { (int)e->ref_con_lx, (int)e->ref_con_ly,
			(int)e->ref_width, (int)e->ref_height };
		tw_stick_resize(con, edge, &ref, &grow_width, &grow_height);
	}

	if (e->preserve_ratio) {
		double x_multiplier = grow_width / e->ref_width;
		double y_multiplier = grow_height / e->ref_height;
		double max_multiplier = fmax(x_multiplier, y_multiplier);
		grow_width = e->ref_width * max_multiplier;
		grow_height = e->ref_height * max_multiplier;
	}

	struct sway_container_state *state = &con->current;
	double border_width = 0.0;
	if (con->current.border == B_NORMAL || con->current.border == B_PIXEL) {
		border_width = state->border_thickness * 2;
	}
	double border_height = 0.0;
	if (con->current.border == B_NORMAL) {
		border_height += container_titlebar_height();
		border_height += state->border_thickness;
	} else if (con->current.border == B_PIXEL) {
		border_height += state->border_thickness * 2;
	}
	if (state->tw_deco) {
		struct tw_insets in = tw_deco_insets(state->tw_maximized);
		border_width = in.left + in.right;
		border_height = in.top + in.bottom;
	}

	// Determine new width/height, and accommodate for floating min/max values
	double width = e->ref_width + grow_width;
	double height = e->ref_height + grow_height;
	int min_width, max_width, min_height, max_height;
	floating_calculate_constraints(&min_width, &max_width,
		&min_height, &max_height);
	width = fmin(width, max_width - border_width);
	width = fmax(width, min_width + border_width);
	width = fmax(width, 1);
	height = fmin(height, max_height - border_height);
	height = fmax(height, min_height + border_height);
	height = fmax(height, 1);

	// Apply the view's min/max size
	if (con->view) {
		double view_min_width, view_max_width, view_min_height, view_max_height;
		view_get_constraints(con->view, &view_min_width, &view_max_width,
				&view_min_height, &view_max_height);
		width = fmin(width, view_max_width - border_width);
		width = fmax(width, view_min_width + border_width);
		width = fmax(width, 1);
		height = fmin(height, view_max_height - border_height);
		height = fmax(height, view_min_height + border_height);
		height = fmax(height, 1);

	}

	// Recalculate these, in case we hit a min/max limit
	grow_width = width - e->ref_width;
	grow_height = height - e->ref_height;

	// Determine grow x/y values - these are relative to the container's x/y at
	// the start of the resize operation.
	double grow_x = 0, grow_y = 0;
	if (edge & WLR_EDGE_LEFT) {
		grow_x = -grow_width;
	} else if (edge & WLR_EDGE_RIGHT) {
		grow_x = 0;
	} else {
		grow_x = -grow_width / 2;
	}
	if (edge & WLR_EDGE_TOP) {
		grow_y = -grow_height;
	} else if (edge & WLR_EDGE_BOTTOM) {
		grow_y = 0;
	} else {
		grow_y = -grow_height / 2;
	}

	// Determine the amounts we need to bump everything relative to the current
	// size.
	int relative_grow_width = width - con->pending.width;
	int relative_grow_height = height - con->pending.height;
	int relative_grow_x = (e->ref_con_lx + grow_x) - con->pending.x;
	int relative_grow_y = (e->ref_con_ly + grow_y) - con->pending.y;

	// Actually resize stuff
	con->pending.x += relative_grow_x;
	con->pending.y += relative_grow_y;
	con->pending.width += relative_grow_width;
	con->pending.height += relative_grow_height;

	con->pending.content_x += relative_grow_x;
	con->pending.content_y += relative_grow_y;
	con->pending.content_width += relative_grow_width;
	con->pending.content_height += relative_grow_height;

	arrange_container(con);
	transaction_commit_dirty();
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	if (e->con == con) {
		tw_session_changed();
		seatop_begin_default(seat);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.unref = handle_unref,
};

/*
 * Snapped windows side by side resize together, as on Windows: dragging the
 * line between a left and a right half (or between quarters) moves it for all
 * the windows along it, and they all stay snapped. Only the sides a snapped
 * window shares with others go that way; its outer sides unsnap it as before.
 */

#define SPLIT_MIN 150 // the narrowest a snapped window gets

struct seatop_resize_snapped_event {
	struct sway_workspace *ws;
	struct wlr_box area;
	enum wlr_edges edge;
	double ref_lx, ref_ly;       // cursor's x/y at start of op
	int ref_line_x, ref_line_y;  // where the lines were at start of op
	int min_x, max_x, min_y, max_y; // where they may go
	list_t *across, *down; // struct sway_container *: along the vertical / horizontal line
};

static bool snapped_left(enum tw_snap snap) {
	return snap == TW_SNAP_LEFT || snap == TW_SNAP_TOPLEFT || snap == TW_SNAP_BOTTOMLEFT;
}

static bool snapped_top(enum tw_snap snap) {
	return snap == TW_SNAP_TOPLEFT || snap == TW_SNAP_TOPRIGHT;
}

static bool snapped_quarter(enum tw_snap snap) {
	return snap >= TW_SNAP_TOPLEFT && snap <= TW_SNAP_BOTTOMRIGHT;
}

static bool is_snapped(struct sway_container *con) {
	return con->view && con->tw.snap != TW_SNAP_NONE && con->tw.snap != TW_SNAP_TOP &&
		!con->pending.tw_minimized && !con->pending.tw_maximized &&
		con->pending.fullscreen_mode == FULLSCREEN_NONE;
}

/* Where the line between left and right is for this window. */
static int line_x(struct sway_container *con, struct wlr_box area) {
	struct wlr_box box = tw_container_snap_box(con, area);
	return snapped_left(con->tw.snap) ? box.x + box.width : box.x;
}

/* Where the line between top and bottom of its column is for this quarter. */
static int line_y(struct sway_container *con, struct wlr_box area) {
	struct wlr_box box = tw_container_snap_box(con, area);
	return snapped_top(con->tw.snap) ? box.y + box.height : box.y;
}

/* How narrow (or low) the window can get, frame included. */
static void min_size(struct sway_container *con, int *width, int *height) {
	double min_w, max_w, min_h, max_h;
	view_get_constraints(con->view, &min_w, &max_w, &min_h, &max_h);
	struct tw_insets in = con->pending.tw_deco ? tw_deco_insets(false) :
		(struct tw_insets){ 0 };
	*width = fmax(SPLIT_MIN, min_w + in.left + in.right);
	*height = fmax(SPLIT_MIN, min_h + in.top + in.bottom);
}

static void resize_snapped_apply(struct seatop_resize_snapped_event *e) {
	list_t *all[] = { e->across, e->down };
	for (int l = 0; l < 2; l++) {
		for (int i = 0; i < all[l]->length; i++) {
			struct sway_container *con = all[l]->items[i];
			struct wlr_box box = tw_container_snap_box(con, e->area);
			tw_set_box(con, &box);
		}
	}
	transaction_commit_dirty();
}

static void resize_snapped_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_resize_snapped_event *e = seat->seatop_data;
	if (e->edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
		double x = e->ref_line_x + seat->cursor->cursor->x - e->ref_lx;
		x = fmin(fmax(x, e->min_x), e->max_x);
		double f = (x - e->area.x) / e->area.width;
		for (int i = 0; i < e->across->length; i++) {
			((struct sway_container *)e->across->items[i])->tw.split_x = f;
		}
	}
	if (e->edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
		double y = e->ref_line_y + seat->cursor->cursor->y - e->ref_ly;
		y = fmin(fmax(y, e->min_y), e->max_y);
		double f = (y - e->area.y) / e->area.height;
		for (int i = 0; i < e->down->length; i++) {
			((struct sway_container *)e->down->items[i])->tw.split_y = f;
		}
	}
	resize_snapped_apply(e);
}

static void resize_snapped_set_resizing(struct seatop_resize_snapped_event *e, bool resizing) {
	list_t *all[] = { e->across, e->down };
	for (int l = 0; l < 2; l++) {
		for (int i = 0; i < all[l]->length; i++) {
			struct sway_container *con = all[l]->items[i];
			container_set_resizing(con, resizing);
			if (!resizing) {
				arrange_container(con); // Send configure w/o resizing hint
			}
		}
	}
}

static void resize_snapped_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	struct seatop_resize_snapped_event *e = seat->seatop_data;
	if (seat->cursor->pressed_button_count == 0) {
		resize_snapped_set_resizing(e, false);
		transaction_commit_dirty();
		tw_session_changed();
		seatop_begin_default(seat);
	}
}

static void resize_snapped_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_resize_snapped_event *e = seat->seatop_data;
	list_t *all[] = { e->across, e->down };
	for (int l = 0; l < 2; l++) {
		int i = list_find(all[l], con);
		if (i >= 0) {
			list_del(all[l], i);
		}
	}
}

static void resize_snapped_end(struct sway_seat *seat) {
	struct seatop_resize_snapped_event *e = seat->seatop_data;
	list_free(e->across);
	list_free(e->down);
}

static const struct sway_seatop_impl seatop_resize_snapped_impl = {
	.button = resize_snapped_button,
	.pointer_motion = resize_snapped_motion,
	.unref = resize_snapped_unref,
	.end = resize_snapped_end,
};

static bool begin_resize_snapped(struct sway_seat *seat, struct sway_container *con,
		enum wlr_edges edge) {
	struct sway_workspace *ws = con->pending.workspace;
	if (tw_mode != TW_MODE_WINDOW || !ws || !is_snapped(con) || edge == WLR_EDGE_NONE ||
			(edge & ~tw_snap_inner_edges(con->tw.snap))) {
		return false;
	}
	struct seatop_resize_snapped_event *e = calloc(1, sizeof(*e));
	if (!e) {
		return false;
	}
	e->ws = ws;
	e->area = tw_workarea(ws);
	e->edge = edge;
	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;
	e->ref_line_x = line_x(con, e->area);
	e->ref_line_y = line_y(con, e->area);
	e->across = create_list();
	e->down = create_list();
	int left = 0, right = 0, top = 0, bottom = 0;
	for (int i = 0; i < ws->floating->length; i++) {
		struct sway_container *c = ws->floating->items[i];
		if (!is_snapped(c)) {
			continue;
		}
		int w, h;
		min_size(c, &w, &h);
		if ((edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) &&
				abs(line_x(c, e->area) - e->ref_line_x) <= 2) {
			list_add(e->across, c);
			if (snapped_left(c->tw.snap)) {
				left = fmax(left, w);
			} else {
				right = fmax(right, w);
			}
		}
		// the line between the quarters of one column
		if ((edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && snapped_quarter(c->tw.snap) &&
				snapped_left(c->tw.snap) == snapped_left(con->tw.snap) &&
				abs(line_y(c, e->area) - e->ref_line_y) <= 2) {
			list_add(e->down, c);
			if (snapped_top(c->tw.snap)) {
				top = fmax(top, h);
			} else {
				bottom = fmax(bottom, h);
			}
		}
	}
	e->min_x = e->area.x + fmax(left, SPLIT_MIN);
	e->max_x = e->area.x + e->area.width - fmax(right, SPLIT_MIN);
	e->min_y = e->area.y + fmax(top, SPLIT_MIN);
	e->max_y = e->area.y + e->area.height - fmax(bottom, SPLIT_MIN);
	if (e->min_x > e->max_x || e->min_y > e->max_y) {
		// too little room to move the line at all
		e->max_x = e->min_x = e->ref_line_x;
		e->max_y = e->min_y = e->ref_line_y;
	}

	seatop_end(seat);
	seat->seatop_impl = &seatop_resize_snapped_impl;
	seat->seatop_data = e;
	resize_snapped_set_resizing(e, true);
	container_raise_floating(con);
	transaction_commit_dirty();
	cursor_set_image(seat->cursor, wlr_xcursor_get_resize_name(edge), NULL);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
	return true;
}

void seatop_begin_resize_floating(struct sway_seat *seat,
		struct sway_container *con, enum wlr_edges edge) {
	if (begin_resize_snapped(seat, con, edge)) {
		return;
	}
	seatop_end(seat);
	tw_forget_home(con);
	con->tw.snap = TW_SNAP_NONE; // resized on its own: no longer snapped

	struct seatop_resize_floating_event *e =
		calloc(1, sizeof(struct seatop_resize_floating_event));
	if (!e) {
		return;
	}
	e->con = con;

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	e->preserve_ratio = keyboard &&
		(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_SHIFT);

	e->edge = edge == WLR_EDGE_NONE ? WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT : edge;
	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;
	e->ref_con_lx = con->pending.x;
	e->ref_con_ly = con->pending.y;
	e->ref_width = con->pending.width;
	e->ref_height = con->pending.height;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_set_resizing(con, true);
	container_raise_floating(con);
	transaction_commit_dirty();

	const char *image = edge == WLR_EDGE_NONE ?
		"se-resize" : wlr_xcursor_get_resize_name(edge);
	cursor_set_image(seat->cursor, image, NULL);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
