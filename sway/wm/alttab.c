#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
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

static void render(void) {
	int n = state.items->length;
	struct sway_workspace *ws = seat_get_focused_workspace(state.seat);
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || n == 0) {
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
