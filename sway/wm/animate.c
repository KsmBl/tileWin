/*
 * Animations of windows and desktops, like on Windows: new windows fade in and
 * rise a little, closing windows shrink and fade out, minimized windows fly to
 * the taskbar and back, maximizing and snapping stretch a window to its new
 * place, and switching desktops slides them. "animations disable" turns them
 * off, "animation_speed <factor>" makes them faster (2) or slower (0.5).
 *
 * Closing, minimizing and resizing animate a copy of the window's buffers (a
 * snapshot) while the real window is hidden or already gone. Opening windows
 * and the desktop that slides in move the real windows, so they stay live.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include "sway/config.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "list.h"
#include "log.h"

#define OPEN_MS 180
#define CLOSE_MS 160
#define MINIMIZE_MS 240
#define RESIZE_MS 180
#define SLIDE_MS 280
#define OPEN_RISE 18
#define FRAME_MS 8

enum ease {
	EASE_OUT,
	EASE_IN,
	EASE_IN_OUT,
};

enum kind {
	ANIM_OPEN,     // a live window fades in and rises
	ANIM_SNAPSHOT, // a copy of a window moves between two boxes
	ANIM_SLIDE,    // desktop switch: a copy of the old desktop, the new one offset
};

struct piece {
	struct wlr_scene_node *node;
	bool rect;
	float color[4];
	double x, y, width, height; // layout coordinates when captured
};

struct fbox {
	double x, y, width, height;
};

struct anim {
	enum kind kind;
	enum ease ease;
	struct timespec start;
	double duration; // ms
	struct sway_container *con; // the live window, or the window hidden during the copy
	struct sway_workspace *ws;  // ANIM_SLIDE: the desktop sliding in
	struct wlr_scene_tree *tree;
	list_t *pieces;             // struct piece *
	struct fbox captured;       // bounding box of the pieces
	struct fbox box0, box1;     // where the copy moves
	double alpha0, alpha1;
	float alpha;                // read by output_configure_scene
	double dx0, dx;             // ANIM_SLIDE: offset of the incoming desktop
	bool reveal;                // show con again at the end
};

static list_t *anims;
static struct wl_event_source *timer;
static bool shutting_down;

static bool enabled(void) {
	return !shutting_down && config && config->active && !config->reading &&
		config->tw_animations && server.wl_event_loop;
}

static double scaled_duration(int ms) {
	double speed = config && config->tw_animation_speed > 0 ? config->tw_animation_speed : 1;
	return ms / speed;
}

static double ease(enum ease e, double t) {
	switch (e) {
	case EASE_OUT:
		return 1 - pow(1 - t, 3);
	case EASE_IN:
		return t * t * t;
	case EASE_IN_OUT:
		return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
	}
	return t;
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

static void schedule_frames(void) {
	for (int i = 0; root && i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (output->wlr_output && output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

/* ---------- copies of scene nodes ---------- */

static void add_piece(struct anim *a, struct wlr_scene_node *node, bool rect,
		const float *color, double x, double y, double w, double h) {
	struct piece *p = calloc(1, sizeof(*p));
	if (!p) {
		wlr_scene_node_destroy(node);
		return;
	}
	p->node = node;
	p->rect = rect;
	if (color) {
		for (int i = 0; i < 4; i++) {
			p->color[i] = color[i];
		}
	}
	p->x = x;
	p->y = y;
	p->width = w;
	p->height = h;
	list_add(a->pieces, p);
}

static void capture_node(struct anim *a, struct wlr_scene_node *node, double x, double y,
		bool top) {
	if (!node->enabled && !top) {
		return;
	}
	if (!top) {
		x += node->x;
		y += node->y;
	}
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			capture_node(a, child, x, y, false);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (!buffer->buffer) {
			break;
		}
		int w = buffer->dst_width > 0 ? buffer->dst_width : buffer->buffer->width;
		int h = buffer->dst_height > 0 ? buffer->dst_height : buffer->buffer->height;
		struct wlr_scene_buffer *copy = wlr_scene_buffer_create(a->tree, NULL);
		if (!copy) {
			break;
		}
		wlr_scene_buffer_set_dest_size(copy, w, h);
		wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
		wlr_scene_buffer_set_transfer_function(copy, buffer->transfer_function);
		wlr_scene_buffer_set_primaries(copy, buffer->primaries);
		wlr_scene_buffer_set_source_box(copy, &buffer->src_box);
		wlr_scene_buffer_set_transform(copy, buffer->transform);
		wlr_scene_node_set_position(&copy->node, (int)x, (int)y);
		wlr_scene_buffer_set_buffer(copy, buffer->buffer);
		add_piece(a, &copy->node, false, NULL, x, y, w, h);
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (rect->width <= 0 || rect->height <= 0 || rect->color[3] <= 0) {
			break;
		}
		struct wlr_scene_rect *copy = wlr_scene_rect_create(a->tree, rect->width,
			rect->height, rect->color);
		if (!copy) {
			break;
		}
		wlr_scene_node_set_position(&copy->node, (int)x, (int)y);
		add_piece(a, &copy->node, true, rect->color, x, y, rect->width, rect->height);
		break;
	}
	}
}

static bool make_tree(struct anim *a) {
	a->tree = wlr_scene_tree_create(root->layers.floating);
	if (!a->tree) {
		return false;
	}
	scene_descriptor_assign(&a->tree->node, SWAY_SCENE_DESC_TW_ANIMATION, &a->alpha);
	scene_descriptor_assign(&a->tree->node, SWAY_SCENE_DESC_NON_INTERACTIVE, (void *)1);
	a->pieces = create_list();
	return true;
}

static void capture_container(struct anim *a, struct sway_container *con) {
	int lx, ly;
	wlr_scene_node_coords(&con->scene_tree->node, &lx, &ly);
	capture_node(a, &con->scene_tree->node, lx, ly, true);
}

static void measure(struct anim *a) {
	double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
	for (int i = 0; i < a->pieces->length; i++) {
		struct piece *p = a->pieces->items[i];
		x0 = fmin(x0, p->x);
		y0 = fmin(y0, p->y);
		x1 = fmax(x1, p->x + p->width);
		y1 = fmax(y1, p->y + p->height);
	}
	a->captured = a->pieces->length ? (struct fbox){ x0, y0, x1 - x0, y1 - y0 } :
		(struct fbox){ 0, 0, 1, 1 };
	a->box0 = a->box1 = a->captured;
}

static void apply_copy(struct anim *a, double e) {
	struct fbox b = {
		lerp(a->box0.x, a->box1.x, e), lerp(a->box0.y, a->box1.y, e),
		lerp(a->box0.width, a->box1.width, e), lerp(a->box0.height, a->box1.height, e),
	};
	double sx = a->captured.width > 0 ? b.width / a->captured.width : 1;
	double sy = a->captured.height > 0 ? b.height / a->captured.height : 1;
	a->alpha = lerp(a->alpha0, a->alpha1, e);
	for (int i = 0; a->pieces && i < a->pieces->length; i++) {
		struct piece *p = a->pieces->items[i];
		double x = b.x + (p->x - a->captured.x) * sx;
		double y = b.y + (p->y - a->captured.y) * sy;
		int w = (int)fmax(1, round(p->width * sx)), h = (int)fmax(1, round(p->height * sy));
		wlr_scene_node_set_position(p->node, (int)round(x), (int)round(y));
		if (p->rect) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(p->node);
			float color[4] = { p->color[0], p->color[1], p->color[2], p->color[3] * a->alpha };
			wlr_scene_rect_set_size(rect, w, h);
			wlr_scene_rect_set_color(rect, color);
		} else {
			wlr_scene_buffer_set_dest_size(wlr_scene_buffer_from_node(p->node), w, h);
		}
	}
}

/* ---------- live windows ---------- */

static struct anim *slide_for(struct sway_workspace *ws) {
	for (int i = 0; ws && anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->kind == ANIM_SLIDE && a->ws == ws) {
			return a;
		}
	}
	return NULL;
}

int tw_animate_workspace_dx(struct sway_workspace *ws) {
	struct anim *a = slide_for(ws);
	return a ? (int)round(a->dx) : 0;
}

double tw_animate_container_dy(struct sway_container *con) {
	return con && con->tw.anim.active ? con->tw.anim.dy : 0;
}

float tw_animate_container_alpha(struct sway_container *con) {
	return con && con->tw.anim.active ? con->tw.anim.alpha : 1.0f;
}

bool tw_animate_hides(struct sway_container *con) {
	return con && con->tw.anim.hidden;
}

static bool floating_shown(struct sway_container *con) {
	struct sway_workspace *ws = con->current.workspace;
	return con->scene_tree && ws && ws->output && workspace_is_visible(ws) &&
		container_is_floating(con) && !con->current.tw_minimized &&
		con->current.fullscreen_mode == FULLSCREEN_NONE &&
		con->scene_tree->node.parent == root->layers.floating;
}

/* Puts a floating window where arrange put it, plus the animation offsets. */
static void place_floating(struct sway_container *con) {
	if (floating_shown(con)) {
		wlr_scene_node_set_position(&con->scene_tree->node,
			con->current.x + tw_animate_workspace_dx(con->current.workspace),
			con->current.y + tw_animate_container_dy(con));
	}
}

static void place_workspace(struct sway_workspace *ws) {
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || output->current.active_workspace != ws || ws->current.fullscreen) {
		return; // arrange applies the offset once the desktop is shown
	}
	struct wlr_box *area = &output->usable_area;
	wlr_scene_node_set_position(&ws->layers.tiling->node,
		ws->current_gaps.left + area->x + tw_animate_workspace_dx(ws),
		ws->current_gaps.top + area->y);
	for (int i = 0; i < ws->current.floating->length; i++) {
		place_floating(ws->current.floating->items[i]);
	}
}

static void reveal(struct sway_container *con) {
	con->tw.anim.hidden = false;
	if (floating_shown(con)) {
		wlr_scene_node_set_enabled(&con->scene_tree->node, true);
	}
}

/* ---------- the animation list ---------- */

static void finish(struct anim *a) {
	if (a->con) {
		if (a->kind == ANIM_OPEN) {
			a->con->tw.anim.active = false;
			a->con->tw.anim.alpha = 1;
			a->con->tw.anim.dy = 0;
			place_floating(a->con);
		}
		if (a->reveal) {
			reveal(a->con);
		}
	}
	if (a->kind == ANIM_SLIDE) {
		a->dx = 0;
		place_workspace(a->ws);
	}
	if (a->tree) {
		wlr_scene_node_destroy(&a->tree->node);
	}
	if (a->pieces) {
		list_free_items_and_destroy(a->pieces);
	}
	free(a);
}

static void remove_anim(struct anim *a) {
	int index = list_find(anims, a);
	if (index >= 0) {
		list_del(anims, index);
	}
	finish(a);
}

static int tick(void *data) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	for (int i = anims ? anims->length - 1 : -1; i >= 0; i--) {
		struct anim *a = anims->items[i];
		double ms = (now.tv_sec - a->start.tv_sec) * 1000.0 +
			(now.tv_nsec - a->start.tv_nsec) / 1e6;
		double t = a->duration > 0 ? fmin(1, ms / a->duration) : 1;
		double e = ease(a->ease, t);
		switch (a->kind) {
		case ANIM_OPEN:
			if (a->con) {
				a->con->tw.anim.alpha = e;
				a->con->tw.anim.dy = a->con->tw.anim.dy > 0 || e < 1 ?
					OPEN_RISE * (1 - e) * (container_is_floating(a->con) ? 1 : 0) : 0;
				place_floating(a->con);
			}
			break;
		case ANIM_SNAPSHOT:
			apply_copy(a, e);
			break;
		case ANIM_SLIDE:
			apply_copy(a, e);
			a->dx = a->dx0 * (1 - e);
			place_workspace(a->ws);
			break;
		}
		if (t >= 1 || (a->kind == ANIM_OPEN && !a->con)) {
			list_del(anims, i);
			finish(a);
		}
	}
	schedule_frames();
	if (anims && anims->length) {
		wl_event_source_timer_update(timer, FRAME_MS);
	}
	return 0;
}

static struct anim *anim_new(enum kind kind, int ms, enum ease e) {
	if (!anims) {
		anims = create_list();
	}
	if (!timer) {
		timer = wl_event_loop_add_timer(server.wl_event_loop, tick, NULL);
		if (!timer) {
			return NULL;
		}
	}
	struct anim *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->kind = kind;
	a->ease = e;
	a->duration = scaled_duration(ms);
	a->alpha = 1;
	a->alpha0 = a->alpha1 = 1;
	clock_gettime(CLOCK_MONOTONIC, &a->start);
	list_add(anims, a);
	wl_event_source_timer_update(timer, 1);
	return a;
}

/* Ends the animations of a window right away (e.g. before starting another). */
static void cancel_for(struct sway_container *con) {
	for (int i = anims ? anims->length - 1 : -1; i >= 0; i--) {
		struct anim *a = anims->items[i];
		if (a->con == con) {
			list_del(anims, i);
			finish(a);
		}
	}
}

/* ---------- starting animations ---------- */

void tw_animate_open(struct sway_container *con) {
	if (!enabled() || !con || !con->view || con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return;
	}
	cancel_for(con);
	struct anim *a = anim_new(ANIM_OPEN, OPEN_MS, EASE_OUT);
	if (!a) {
		return;
	}
	a->con = con;
	con->tw.anim.active = true;
	con->tw.anim.alpha = 0;
	con->tw.anim.dy = container_is_floating(con) ? OPEN_RISE : 0;
}

void tw_animate_close(struct sway_container *con) {
	if (!enabled() || !con || !con->scene_tree || !con->scene_tree->node.enabled ||
			con->current.fullscreen_mode != FULLSCREEN_NONE || con->current.tw_minimized ||
			!con->current.workspace || !workspace_is_visible(con->current.workspace)) {
		return;
	}
	cancel_for(con);
	struct anim *a = anim_new(ANIM_SNAPSHOT, CLOSE_MS, EASE_IN);
	if (!a || !make_tree(a)) {
		if (a) {
			remove_anim(a);
		}
		return;
	}
	capture_container(a, con);
	measure(a);
	double shrink = 0.9;
	a->box1 = (struct fbox){
		a->captured.x + a->captured.width * (1 - shrink) / 2,
		a->captured.y + a->captured.height * (1 - shrink) / 2,
		a->captured.width * shrink, a->captured.height * shrink,
	};
	a->alpha1 = 0;
	apply_copy(a, 0);
}

void tw_animate_minimize(struct sway_container *con, bool minimize) {
	struct sway_workspace *ws = con ? con->current.workspace : NULL;
	if (!enabled() || !con || !con->scene_tree || !container_is_floating(con) || !ws ||
			!ws->output || !workspace_is_visible(ws) ||
			con->current.fullscreen_mode != FULLSCREEN_NONE) {
		return;
	}
	cancel_for(con);
	struct anim *a = anim_new(ANIM_SNAPSHOT, MINIMIZE_MS, minimize ? EASE_IN : EASE_OUT);
	if (!a || !make_tree(a)) {
		if (a) {
			remove_anim(a);
		}
		return;
	}
	capture_container(a, con);
	measure(a);
	struct sway_output *output = ws->output;
	double w = fmax(40, a->captured.width * 0.12), h = fmax(30, a->captured.height * 0.12);
	struct fbox taskbar = {
		output->lx + output->width / 2.0 - w / 2, output->ly + output->height - h, w, h,
	};
	if (minimize) {
		a->box1 = taskbar;
		a->alpha1 = 0;
	} else {
		a->box0 = taskbar;
		a->alpha0 = 0;
		a->con = con;
		a->reveal = true;
		con->tw.anim.hidden = true;
		wlr_scene_node_set_enabled(&con->scene_tree->node, false);
	}
	apply_copy(a, 0);
}

void tw_animate_resize(struct sway_container *con) {
	if (!enabled() || !con || !floating_shown(con)) {
		return;
	}
	struct wlr_box old = { con->current.x, con->current.y, con->current.width,
		con->current.height };
	struct wlr_box new = { con->pending.x, con->pending.y, con->pending.width,
		con->pending.height };
	if (wlr_box_equal(&old, &new) || old.width <= 0 || old.height <= 0 || new.width <= 0 ||
			new.height <= 0) {
		return;
	}
	cancel_for(con);
	struct anim *a = anim_new(ANIM_SNAPSHOT, RESIZE_MS, EASE_OUT);
	if (!a || !make_tree(a)) {
		if (a) {
			remove_anim(a);
		}
		return;
	}
	capture_container(a, con);
	measure(a);
	// keep the margins (shadow, borders) of the captured frame around the new box
	double left = old.x - a->captured.x, top = old.y - a->captured.y;
	double right = a->captured.x + a->captured.width - (old.x + old.width);
	double bottom = a->captured.y + a->captured.height - (old.y + old.height);
	a->box1 = (struct fbox){ new.x - left, new.y - top, new.width + left + right,
		new.height + top + bottom };
	a->con = con;
	a->reveal = true;
	con->tw.anim.hidden = true;
	wlr_scene_node_set_enabled(&con->scene_tree->node, false);
	apply_copy(a, 0);
}

void tw_animate_workspace_switch(struct sway_workspace *ws) {
	struct sway_output *output = ws ? ws->output : NULL;
	struct sway_workspace *old = output ? output->current.active_workspace : NULL;
	if (!enabled() || !old || old == ws || old->output != output || old->current.fullscreen ||
			ws->current.fullscreen) {
		return;
	}
	int old_index = list_find(output->workspaces, old);
	int new_index = list_find(output->workspaces, ws);
	if (old_index < 0 || new_index < 0) {
		return;
	}
	for (int i = anims ? anims->length - 1 : -1; i >= 0; i--) {
		struct anim *a = anims->items[i];
		if (a->kind == ANIM_SLIDE) {
			list_del(anims, i);
			finish(a);
		}
	}
	struct anim *a = anim_new(ANIM_SLIDE, SLIDE_MS, EASE_IN_OUT);
	if (!a || !make_tree(a)) {
		if (a) {
			remove_anim(a);
		}
		return;
	}
	for (int i = 0; i < old->current.tiling->length; i++) {
		struct sway_container *con = old->current.tiling->items[i];
		if (con->scene_tree && con->scene_tree->node.enabled) {
			capture_container(a, con);
		}
	}
	for (int i = 0; i < old->current.floating->length; i++) {
		struct sway_container *con = old->current.floating->items[i];
		if (con->scene_tree && con->scene_tree->node.enabled && !con->current.tw_minimized &&
				!container_is_sticky(con)) {
			capture_container(a, con);
		}
	}
	measure(a);
	int direction = new_index > old_index ? 1 : -1;
	a->box1.x -= direction * output->width;
	a->ws = ws;
	a->dx0 = a->dx = direction * output->width;
	apply_copy(a, 0);
	place_workspace(ws);
}

/* ---------- cleanup ---------- */

void tw_animate_container_destroyed(struct sway_container *con) {
	for (int i = 0; anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->con == con) {
			a->con = NULL;
		}
	}
}

void tw_animate_workspace_destroyed(struct sway_workspace *ws) {
	for (int i = 0; anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->ws == ws) {
			a->ws = NULL;
		}
	}
}

void tw_animate_shutdown(void) {
	// windows closing while tileWin exits (or restarts) are not animated
	shutting_down = true;
	while (anims && anims->length) {
		struct anim *a = anims->items[anims->length - 1];
		list_del(anims, anims->length - 1);
		finish(a);
	}
	if (timer) {
		wl_event_source_remove(timer);
		timer = NULL;
	}
}

void tw_animate_fini(void) {
	// the scene and the event loop are gone by now: only free what is left
	for (int i = 0; anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->pieces) {
			list_free_items_and_destroy(a->pieces);
		}
		free(a);
	}
	list_free(anims);
	anims = NULL;
	timer = NULL;
}
