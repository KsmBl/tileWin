#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/tilewin.h"

#define RESTORE_DRAG_THRESHOLD 6

struct seatop_move_floating_event {
	struct sway_container *con;
	double dx, dy; // cursor offset in container
	double start_x, start_y;
	bool restore_on_drag; // maximized or snapped window being dragged
	struct wlr_box before; // where the window was before the drag
	double con_x, con_y; // window position at the start
	list_t *group; // struct group_member: windows touching it at the start
};

struct group_member {
	struct sway_container *con;
	double x, y; // position at the start
};

static void free_group(struct seatop_move_floating_event *e) {
	if (e->group) {
		list_free_items_and_destroy(e->group);
		e->group = NULL;
	}
}

static void handle_end(struct sway_seat *seat) {
	free_group(seat->seatop_data);
}

static void finalize_move(struct sway_seat *seat) {
	struct seatop_move_floating_event *e = seat->seatop_data;

	// We "move" the container to its own location
	// so it discovers its output again.
	container_floating_move_to(e->con, e->con->pending.x, e->con->pending.y);

	enum tw_snap snap = tw_snap_preview_finish();
	if (snap != TW_SNAP_NONE && !e->restore_on_drag) {
		tw_snap_to(e->con, snap);
		// restoring brings the window back to where the drag started, not
		// to the edge it was dropped at
		if (e->before.width > 0 && e->before.height > 0) {
			e->con->tw.restore_box = e->before;
		}
	}
	transaction_commit_dirty();

	tw_session_changed();

	seatop_begin_default(seat);
}

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	if (seat->cursor->pressed_button_count == 0) {
		finalize_move(seat);
	}
}

static void handle_tablet_tool_tip(struct sway_seat *seat,
		struct sway_tablet_tool *tool, uint32_t time_msec,
		enum wlr_tablet_tool_tip_state state) {
	if (state == WLR_TABLET_TOOL_TIP_UP) {
		finalize_move(seat);
	}
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	struct wlr_cursor *cursor = seat->cursor->cursor;

	if (e->restore_on_drag) {
		if (fabs(cursor->x - e->start_x) < RESTORE_DRAG_THRESHOLD &&
				fabs(cursor->y - e->start_y) < RESTORE_DRAG_THRESHOLD) {
			return;
		}
		// Keep the cursor at the same relative spot of the title bar.
		double frac = e->con->pending.width > 0 ? e->dx / e->con->pending.width : 0.5;
		tw_restore(e->con);
		e->before = e->con->tw.restore_box;
		e->dx = frac * e->con->pending.width;
		struct tw_insets in = tw_deco_insets(false);
		if (e->dy > in.top) {
			e->dy = in.top / 2.0;
		}
		e->restore_on_drag = false;
	}

	// holding the group modifier moves the windows stuck to it along; letting
	// go of it leaves them where they were
	bool together = e->group && tw_stick_group_modifier_held(seat);
	list_t *exclude = NULL;
	if (together) {
		exclude = create_list();
		for (int i = 0; i < e->group->length; i++) {
			struct group_member *m = e->group->items[i];
			list_add(exclude, m->con);
		}
	}
	double x = cursor->x - e->dx, y = cursor->y - e->dy;
	tw_stick_move(e->con, exclude, &x, &y);
	list_free(exclude);
	container_floating_move_to(e->con, x, y);
	for (int i = 0; e->group && i < e->group->length; i++) {
		struct group_member *m = e->group->items[i];
		double mx = together ? m->x + x - e->con_x : m->x;
		double my = together ? m->y + y - e->con_y : m->y;
		if (m->con->pending.x != mx || m->con->pending.y != my) {
			container_floating_move_to(m->con, mx, my);
		}
	}
	if (together) {
		tw_snap_preview_finish(); // a group is not snapped to the screen edge
	} else {
		tw_snap_preview_update(e->con, cursor->x, cursor->y);
	}
	transaction_commit_dirty();
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	for (int i = 0; e->group && i < e->group->length; i++) {
		struct group_member *m = e->group->items[i];
		if (m->con == con) {
			free(m);
			list_del(e->group, i);
			break;
		}
	}
	if (e->con == con) {
		tw_snap_preview_finish();
		tw_session_changed();
		seatop_begin_default(seat);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.tablet_tool_tip = handle_tablet_tool_tip,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_move_floating(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_end(seat);

	struct sway_cursor *cursor = seat->cursor;
	struct seatop_move_floating_event *e =
		calloc(1, sizeof(struct seatop_move_floating_event));
	if (!e) {
		return;
	}
	e->con = con;
	e->dx = cursor->cursor->x - con->pending.x;
	e->dy = cursor->cursor->y - con->pending.y;
	e->start_x = cursor->cursor->x;
	e->start_y = cursor->cursor->y;
	e->restore_on_drag = con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE;
	if (!e->restore_on_drag) {
		e->before = (struct wlr_box){ (int)con->pending.x, (int)con->pending.y,
			(int)con->pending.width, (int)con->pending.height };
		list_t *group = tw_stick_group(con);
		if (group) {
			e->group = create_list();
			for (int i = 0; i < group->length; i++) {
				struct sway_container *other = group->items[i];
				struct group_member *m = calloc(1, sizeof(*m));
				if (m) {
					*m = (struct group_member){ other, other->pending.x, other->pending.y };
					list_add(e->group, m);
				}
			}
			list_free(group);
		}
	}
	e->con_x = con->pending.x;
	e->con_y = con->pending.y;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_raise_floating(con);
	transaction_commit_dirty();

	cursor_set_image(cursor, "grab", NULL);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
