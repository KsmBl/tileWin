#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"

static struct {
	bool active;
	struct sway_seat *seat;
	list_t *items; // struct sway_container *
	int index;
	uint32_t modifiers;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *buffer;
} state;

bool tw_alttab_active(void) {
	return state.active;
}

static void collect(struct sway_seat *seat) {
	struct sway_workspace *ws = seat_get_focused_workspace(seat);
	struct sway_seat_node *current;
	wl_list_for_each(current, &seat->focus_stack, link) {
		struct sway_node *node = current->node;
		if (node->type != N_CONTAINER) {
			continue;
		}
		struct sway_container *c = node->sway_container;
		if (!c->view || !c->view->surface || c->node.destroying) {
			continue;
		}
		if (ws && c->pending.workspace != ws && !container_is_sticky(c)) {
			continue;
		}
		list_add(state.items, c);
	}
}

static void finish(void) {
	state.active = false;
	if (state.items) {
		list_free(state.items);
		state.items = NULL;
	}
	if (state.tree) {
		wlr_scene_node_destroy(&state.tree->node);
		state.tree = NULL;
		state.buffer = NULL;
	}
}

/* ---------- Flip 3D: the windows themselves, stacked in perspective ---------- */

#define FLIP_CARDS 8     // windows of the stack that are drawn
#define FLIP_SQUASH 0.76 // how flat a card looks, as if it were tilted back
#define FLIP_STEP 0.87   // how much smaller each card behind the front one is
#define FLIP_TITLE 46

static void content_size(struct sway_container *con, double *w, double *h) {
	*w = con->pending.content_width > 0 ? con->pending.content_width : con->pending.width;
	*h = con->pending.content_height > 0 ? con->pending.content_height : con->pending.height;
	*w = *w < 1 ? 1 : *w;
	*h = *h < 1 ? 1 : *h;
}

static void color_floats(uint32_t color, float out[4]) {
	float alpha = (color & 0xff) / 255.0f;
	// the scene wants the color multiplied by its alpha
	out[0] = (color >> 24 & 0xff) / 255.0f * alpha;
	out[1] = (color >> 16 & 0xff) / 255.0f * alpha;
	out[2] = (color >> 8 & 0xff) / 255.0f * alpha;
	out[3] = alpha;
}

static void render_flip(struct sway_output *output, int n) {
	// the stack is built anew for every step, in the order it is shown in
	if (state.tree) {
		wlr_scene_node_destroy(&state.tree->node);
		state.tree = NULL;
		state.buffer = NULL;
	}
	state.tree = wlr_scene_tree_create(root->layers.seat);
	if (!state.tree) {
		return;
	}
	wlr_scene_node_set_position(&state.tree->node, output->lx, output->ly);

	float wash[4];
	color_floats(tw_style_alttab_wash(tw_theme), wash);
	wlr_scene_rect_create(state.tree, output->width, output->height, wash);

	int shown = n < FLIP_CARDS ? n : FLIP_CARDS;
	double max_w = output->width * 0.44, max_h = output->height * 0.5;
	double cx = output->width * 0.40, cy = output->height * 0.52;
	double dx = output->width * 0.045, dy = output->height * 0.055;
	float frame[4];
	color_floats(0xffffff60, frame);
	for (int j = shown - 1; j >= 0; j--) { // from the back to the front
		struct sway_container *con = state.items->items[(state.index + j) % n];
		double w, h;
		content_size(con, &w, &h);
		double fit = fmin(max_w / w, max_h / h) * pow(FLIP_STEP, j);
		double sx = fit, sy = fit * FLIP_SQUASH;
		double cw = w * sx, ch = h * sy;
		double x = cx + j * dx - cw / 2, y = cy - j * dy - ch / 2;
		struct wlr_scene_rect *edge = wlr_scene_rect_create(state.tree,
			(int)round(cw) + 4, (int)round(ch) + 4, frame);
		if (edge) {
			wlr_scene_node_set_position(&edge->node, (int)round(x) - 2, (int)round(y) - 2);
		}
		tw_snapshot_view(state.tree, con->view, sx, sy, x, y);
	}

	struct sway_container *front = state.items->items[state.index];
	float scale = output->wlr_output->scale;
	int pw = (int)ceil(output->width * scale), ph = (int)ceil(FLIP_TITLE * scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	tw_style_draw_alttab_title(cr, tw_theme, front->title ? front->title : "",
		output->width, FLIP_TITLE);
	cairo_destroy(cr);
	state.buffer = wlr_scene_buffer_create(state.tree, NULL);
	if (!state.buffer) {
		cairo_surface_destroy(surface);
		return;
	}
	tw_scene_buffer_set_surface(state.buffer, surface, output->width, FLIP_TITLE);
	wlr_scene_node_set_position(&state.buffer->node, 0,
		(int)(output->height * 0.82));
}

static void render(void) {
	int n = state.items->length;
	struct sway_workspace *ws = seat_get_focused_workspace(state.seat);
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || n == 0) {
		return;
	}
	// "alttab_style" wins over the theme when it was set
	bool flip = config && config->tw_alttab_style >= 0 ?
		config->tw_alttab_style == 1 : tw_style_alttab_flip(tw_theme);
	if (flip) {
		render_flip(output, n);
		return;
	}
	float scale = output->wlr_output->scale;
	int w, h, cols;
	tw_style_alttab_size(tw_theme, n, output->width - 80, &w, &h, &cols);

	struct tw_alttab_item *items = calloc(n, sizeof(*items));
	for (int i = 0; i < n; i++) {
		struct sway_container *c = state.items->items[i];
		items[i].title = c->title ? c->title : "";
		items[i].icon = tw_icon_for_view(c->view, (int)ceil(48 * scale));
	}
	int pw = (int)ceil(w * scale), ph = (int)ceil(h * scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	tw_style_draw_alttab(cr, tw_theme, items, n, state.index, w, h, cols);
	cairo_destroy(cr);
	free(items);

	if (!state.tree) {
		state.tree = wlr_scene_tree_create(root->layers.seat);
		if (!state.tree) {
			cairo_surface_destroy(surface);
			return;
		}
		state.buffer = wlr_scene_buffer_create(state.tree, NULL);
	}
	if (!state.buffer) {
		cairo_surface_destroy(surface);
		return;
	}
	tw_scene_buffer_set_surface(state.buffer, surface, w, h);
	wlr_scene_node_set_position(&state.tree->node,
		output->lx + (output->width - w) / 2, output->ly + (output->height - h) / 2);
}

void tw_alttab_commit(void) {
	if (!state.active) {
		return;
	}
	struct sway_seat *seat = state.seat;
	struct sway_container *target = NULL;
	if (state.items && state.index >= 0 && state.index < state.items->length) {
		target = state.items->items[state.index];
	}
	finish();
	if (target && !target->node.destroying) {
		seat_set_focus_container(seat, target);
		container_raise_floating(target);
		transaction_commit_dirty();
	}
}

void tw_alttab_cancel(void) {
	finish();
}

void tw_alttab_step(struct sway_seat *seat, int direction) {
	if (!state.active) {
		state.items = create_list();
		state.seat = seat;
		collect(seat);
		int n = state.items->length;
		tw_panel_command("close"); // the start menu and other taskbar popups
		if (n == 0) {
			finish();
			return;
		}
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
		state.modifiers = keyboard ?
			wlr_keyboard_get_modifiers(keyboard) & ~WLR_MODIFIER_SHIFT : 0;
		state.index = 0;
		state.active = true;
		if (!state.modifiers) {
			// invoked without a held modifier (e.g. over IPC): switch directly
			state.index = ((direction > 0 ? 1 : -1) % n + n) % n;
			tw_alttab_commit();
			return;
		}
	}
	int n = state.items->length;
	state.index = ((state.index + direction) % n + n) % n;
	render();
}

void tw_alttab_modifiers(uint32_t modifiers) {
	if (state.active && (modifiers & state.modifiers) == 0) {
		tw_alttab_commit();
	}
}

bool tw_alttab_handle_key(xkb_keysym_t sym, bool pressed) {
	if (!state.active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		if (pressed) {
			tw_alttab_cancel();
		}
		return true;
	case XKB_KEY_Left:
	case XKB_KEY_Up:
		if (pressed) {
			tw_alttab_step(state.seat, -1);
		}
		return true;
	case XKB_KEY_Right:
	case XKB_KEY_Down:
		if (pressed) {
			tw_alttab_step(state.seat, 1);
		}
		return true;
	case XKB_KEY_Return:
		if (pressed) {
			tw_alttab_commit();
		}
		return true;
	default:
		return false;
	}
}

void tw_alttab_container_destroyed(struct sway_container *con) {
	if (!state.active || !state.items) {
		return;
	}
	int idx = list_find(state.items, con);
	if (idx < 0) {
		return;
	}
	list_del(state.items, idx);
	if (state.items->length == 0) {
		finish();
		return;
	}
	if (state.index >= state.items->length) {
		state.index = state.items->length - 1;
	}
	render();
}
