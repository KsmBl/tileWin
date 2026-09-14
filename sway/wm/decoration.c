#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "log.h"

#define DOUBLE_CLICK_MS 450

// Decoration button currently held down, and the container whose buttons
// are hovered. Pointers are only compared after destruction checks.
static struct {
	struct sway_container *con;
	enum tw_hit hit;
} press;

static struct sway_container *hover_con = NULL;

static struct {
	struct sway_container *con;
	enum tw_hit hit;
	uint32_t time;
} last_click;

void tw_container_update_deco_state(struct sway_container *con) {
	con->pending.tw_deco = tw_mode == TW_MODE_WINDOW && con->view &&
		container_is_floating(con) &&
		con->pending.border != B_CSD && con->pending.border != B_NONE &&
		con->pending.fullscreen_mode == FULLSCREEN_NONE;
}

struct tw_insets tw_deco_insets(bool maximized) {
	struct tw_insets insets;
	tw_style_insets(tw_theme, maximized, &insets, NULL);
	return insets;
}

static float container_scale(struct sway_container *con) {
	struct sway_workspace *ws = con->current.workspace ?
		con->current.workspace : con->pending.workspace;
	if (ws && ws->output && ws->output->wlr_output) {
		return ws->output->wlr_output->scale;
	}
	return 1.0f;
}

static void render_strip(struct sway_container *con, enum tw_deco_strip index,
		const struct wlr_box *box, const struct tw_frame *frame, float scale) {
	struct wlr_scene_buffer *node = con->tw.strips[index];
	if (!node) {
		node = wlr_scene_buffer_create(con->tw.deco_tree, NULL);
		if (!node) {
			return;
		}
		con->tw.strips[index] = node;
	}
	if (box->width <= 0 || box->height <= 0) {
		wlr_scene_node_set_enabled(&node->node, false);
		return;
	}
	int pw = (int)ceil(box->width * scale);
	int ph = (int)ceil(box->height * scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, (double)pw / box->width, (double)ph / box->height);
	cairo_translate(cr, -box->x, -box->y);
	cairo_rectangle(cr, box->x, box->y, box->width, box->height);
	cairo_clip(cr);
	tw_style_draw_frame(cr, tw_theme, frame);
	cairo_destroy(cr);

	tw_scene_buffer_set_surface(node, surface, box->width, box->height);
	wlr_scene_node_set_position(&node->node, box->x, box->y);
	wlr_scene_node_set_enabled(&node->node, true);
}

static void release_strips(struct sway_container *con) {
	for (int i = 0; i < TW_STRIP_COUNT; i++) {
		if (con->tw.strips[i]) {
			wlr_scene_buffer_set_buffer(con->tw.strips[i], NULL);
		}
	}
	con->tw.frame_cache.valid = false;
	con->tw.top_cache.valid = false;
}

void tw_deco_arrange(struct sway_container *con, int width, int height) {
	struct tw_container *tw = &con->tw;
	if (!tw->deco_tree || !con->view) {
		return;
	}
	wlr_scene_node_set_enabled(&tw->deco_tree->node, true);

	bool maximized = con->current.tw_maximized;
	bool focused = con->current.focused;
	float scale = container_scale(con);
	const char *title = con->title ? con->title : "";
	cairo_surface_t *icon = tw_icon_for_view(con->view, (int)ceil(16 * scale));

	bool frame_ok = tw->frame_cache.valid &&
		tw->frame_cache.width == width && tw->frame_cache.height == height &&
		tw->frame_cache.focused == focused && tw->frame_cache.maximized == maximized &&
		tw->frame_cache.scale == scale &&
		tw->frame_cache.theme_generation == tw_theme_generation;
	bool top_ok = frame_ok && tw->top_cache.valid &&
		tw->top_cache.hover == tw->hover && tw->top_cache.pressed == tw->pressed &&
		tw->top_cache.icon == icon && tw->top_cache.title &&
		strcmp(tw->top_cache.title, title) == 0;
	if (top_ok) {
		return;
	}

	struct tw_insets in;
	int grab;
	tw_style_insets(tw_theme, maximized, &in, &grab);
	struct tw_frame frame = {
		.width = width,
		.height = height,
		.focused = focused,
		.maximized = maximized,
		.title = title,
		.icon = icon,
		.hover = tw->hover,
		.pressed = tw->pressed,
	};

	struct wlr_box top = { -grab, -grab, width + 2 * grab, in.top + grab };
	render_strip(con, TW_STRIP_TOP, &top, &frame, scale);
	if (!frame_ok) {
		int mid_h = height - in.top - in.bottom;
		struct wlr_box bottom = { -grab, height - in.bottom, width + 2 * grab, in.bottom + grab };
		struct wlr_box left = { -grab, in.top, in.left + grab, mid_h };
		struct wlr_box right = { width - in.right, in.top, in.right + grab, mid_h };
		render_strip(con, TW_STRIP_BOTTOM, &bottom, &frame, scale);
		render_strip(con, TW_STRIP_LEFT, &left, &frame, scale);
		render_strip(con, TW_STRIP_RIGHT, &right, &frame, scale);
	}

	tw->frame_cache.valid = true;
	tw->frame_cache.width = width;
	tw->frame_cache.height = height;
	tw->frame_cache.focused = focused;
	tw->frame_cache.maximized = maximized;
	tw->frame_cache.scale = scale;
	tw->frame_cache.theme_generation = tw_theme_generation;
	tw->top_cache.valid = true;
	free(tw->top_cache.title);
	tw->top_cache.title = strdup(title);
	tw->top_cache.hover = tw->hover;
	tw->top_cache.pressed = tw->pressed;
	tw->top_cache.icon = icon;
}

void tw_deco_disable(struct sway_container *con) {
	if (!con->tw.deco_tree || !con->tw.deco_tree->node.enabled) {
		return;
	}
	wlr_scene_node_set_enabled(&con->tw.deco_tree->node, false);
	release_strips(con);
}

void tw_container_destroy(struct sway_container *con) {
	free(con->tw.top_cache.title);
	con->tw.top_cache.title = NULL;
	if (press.con == con) {
		press.con = NULL;
	}
	if (hover_con == con) {
		hover_con = NULL;
	}
	if (last_click.con == con) {
		last_click.con = NULL;
	}
	tw_alttab_container_destroyed(con);
}

static void refresh_top(struct sway_container *con) {
	if (con->node.destroying || !con->current.tw_deco) {
		return;
	}
	tw_deco_arrange(con, con->current.width, con->current.height);
}

static bool box_contains(const struct wlr_box *box, double x, double y) {
	return x >= box->x && y >= box->y &&
		x < box->x + box->width && y < box->y + box->height;
}

enum tw_hit tw_deco_hit_test(struct sway_container *con, double lx, double ly,
		enum wlr_edges *edges) {
	*edges = WLR_EDGE_NONE;
	if (!con || !con->view || !con->current.tw_deco) {
		return TW_HIT_NONE;
	}
	bool maximized = con->current.tw_maximized;
	struct tw_insets in;
	int grab;
	tw_style_insets(tw_theme, maximized, &in, &grab);
	double x = lx - con->current.x;
	double y = ly - con->current.y;
	int W = con->current.width, H = con->current.height;
	if (x < -grab || y < -grab || x >= W + grab || y >= H + grab) {
		return TW_HIT_NONE;
	}

	struct tw_buttons b;
	tw_style_buttons(tw_theme, W, maximized, &b);
	if (box_contains(&b.close, x, y)) {
		return TW_HIT_CLOSE;
	}
	if (box_contains(&b.maximize, x, y)) {
		return TW_HIT_MAXIMIZE;
	}
	if (box_contains(&b.minimize, x, y)) {
		return TW_HIT_MINIMIZE;
	}

	if (!maximized) {
		const int corner = 16;
		int side = in.left > 0 ? in.left : 1;
		int top_zone = side > 4 ? side : 4;
		if (top_zone > in.top) {
			top_zone = in.top;
		}
		int bottom_zone = in.bottom > 0 ? in.bottom : 1;
		enum wlr_edges e = WLR_EDGE_NONE;
		if (x < side) {
			e |= WLR_EDGE_LEFT;
		} else if (x >= W - side) {
			e |= WLR_EDGE_RIGHT;
		}
		if (y < top_zone) {
			e |= WLR_EDGE_TOP;
		} else if (y >= H - bottom_zone) {
			e |= WLR_EDGE_BOTTOM;
		}
		if ((e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) && !(e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
			if (y < corner) {
				e |= WLR_EDGE_TOP;
			} else if (y >= H - corner) {
				e |= WLR_EDGE_BOTTOM;
			}
		}
		if ((e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && !(e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
			if (x < corner) {
				e |= WLR_EDGE_LEFT;
			} else if (x >= W - corner) {
				e |= WLR_EDGE_RIGHT;
			}
		}
		if (e != WLR_EDGE_NONE) {
			*edges = e;
			return TW_HIT_EDGE;
		}
	}

	if (y < in.top) {
		if (x >= in.left && x < in.left + 26) {
			return TW_HIT_ICON;
		}
		return TW_HIT_TITLE;
	}
	return TW_HIT_CLIENT;
}

static void set_pressed(struct sway_container *con, enum tw_hit hit) {
	if (press.con && press.con != con && !press.con->node.destroying) {
		press.con->tw.pressed = TW_HIT_NONE;
		refresh_top(press.con);
	}
	press.con = con;
	press.hit = hit;
	if (con) {
		con->tw.pressed = hit;
		refresh_top(con);
	}
}

static void send_window_menu(struct sway_seat *seat, struct sway_container *con) {
	json_object *data = json_object_new_object();
	json_object_object_add(data, "con_id", json_object_new_int64(con->node.id));
	double lx = seat->cursor->cursor->x, ly = seat->cursor->cursor->y;
	struct sway_output *output = con->pending.workspace ?
		con->pending.workspace->output : NULL;
	if (output) {
		json_object_object_add(data, "output",
			json_object_new_string(output->wlr_output->name));
		lx -= output->lx;
		ly -= output->ly;
	}
	json_object_object_add(data, "x", json_object_new_int((int)lx));
	json_object_object_add(data, "y", json_object_new_int((int)ly));
	json_object_object_add(data, "maximized",
		json_object_new_boolean(con->pending.tw_maximized));
	ipc_event_tilewin("window_menu", data);
}

bool tw_handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct sway_container *cont, struct wlr_surface *surface,
		uint32_t button, bool pressed) {
	if (!pressed) {
		if (!press.con) {
			return false;
		}
		struct sway_container *target = press.con;
		enum tw_hit hit = press.hit;
		bool same = cont == target && !surface;
		if (same) {
			enum wlr_edges edges;
			same = tw_deco_hit_test(target, seat->cursor->cursor->x,
				seat->cursor->cursor->y, &edges) == hit;
		}
		set_pressed(NULL, TW_HIT_NONE);
		target->tw.pressed = TW_HIT_NONE;
		refresh_top(target);
		if (same && !target->node.destroying) {
			switch (hit) {
			case TW_HIT_MINIMIZE:
				tw_minimize(target, true);
				break;
			case TW_HIT_MAXIMIZE:
				tw_maximize(target, !target->pending.tw_maximized);
				break;
			case TW_HIT_CLOSE:
				view_close(target->view);
				break;
			default:
				break;
			}
			transaction_commit_dirty();
		}
		return true;
	}

	if (!cont || !cont->view || !cont->current.tw_deco) {
		return false;
	}
	if (surface) {
		// click inside the client: raise the window like Windows does
		container_raise_floating(cont);
		return false;
	}

	enum wlr_edges edges;
	enum tw_hit hit = tw_deco_hit_test(cont, seat->cursor->cursor->x,
		seat->cursor->cursor->y, &edges);
	if (hit == TW_HIT_NONE || hit == TW_HIT_CLIENT) {
		return false;
	}

	seat_set_focus_container(seat, cont);
	container_raise_floating(cont);
	transaction_commit_dirty();

	switch (hit) {
	case TW_HIT_MINIMIZE:
	case TW_HIT_MAXIMIZE:
	case TW_HIT_CLOSE:
		if (button == BTN_LEFT) {
			set_pressed(cont, hit);
		}
		return true;
	case TW_HIT_EDGE:
		if (button == BTN_LEFT) {
			cont->tw.snap = TW_SNAP_NONE;
			seatop_begin_resize_floating(seat, cont, edges);
		}
		return true;
	case TW_HIT_TITLE:
	case TW_HIT_ICON:
		if (button == BTN_RIGHT) {
			send_window_menu(seat, cont);
			return true;
		}
		if (button != BTN_LEFT) {
			return true;
		}
		if (last_click.con == cont && last_click.hit == hit &&
				time_msec - last_click.time < DOUBLE_CLICK_MS) {
			last_click.con = NULL;
			if (hit == TW_HIT_ICON) {
				view_close(cont->view);
			} else {
				tw_maximize(cont, !cont->pending.tw_maximized);
			}
			transaction_commit_dirty();
			return true;
		}
		last_click.con = cont;
		last_click.hit = hit;
		last_click.time = time_msec;
		if (hit == TW_HIT_ICON) {
			send_window_menu(seat, cont);
		} else {
			seatop_begin_move_floating(seat, cont);
		}
		return true;
	default:
		return false;
	}
}

bool tw_handle_motion(struct sway_seat *seat, struct sway_container *cont,
		struct wlr_surface *surface) {
	struct sway_container *target = cont && !surface && cont->view &&
		cont->current.tw_deco ? cont : NULL;
	enum wlr_edges edges = WLR_EDGE_NONE;
	enum tw_hit hit = target ? tw_deco_hit_test(target, seat->cursor->cursor->x,
		seat->cursor->cursor->y, &edges) : TW_HIT_NONE;

	enum tw_hit button_hit = hit == TW_HIT_MINIMIZE || hit == TW_HIT_MAXIMIZE ||
		hit == TW_HIT_CLOSE ? hit : TW_HIT_NONE;
	struct sway_container *new_hover = button_hit ? target : NULL;
	if (hover_con && hover_con != new_hover) {
		hover_con->tw.hover = TW_HIT_NONE;
		refresh_top(hover_con);
	}
	hover_con = new_hover;
	if (new_hover && new_hover->tw.hover != button_hit) {
		new_hover->tw.hover = button_hit;
		refresh_top(new_hover);
	}

	if (hit == TW_HIT_NONE || hit == TW_HIT_CLIENT) {
		return false;
	}
	cursor_set_image(seat->cursor, hit == TW_HIT_EDGE ?
		wlr_xcursor_get_resize_name(edges) : "default", NULL);
	return true;
}
