/*
 * Animations of windows and desktops, like on Windows: new windows fade in and
 * rise a little, closing windows shrink and fade out, minimized windows fly to
 * the taskbar and back, maximizing and snapping stretch a window to its new
 * place, and switching desktops slides them. "animations disable" turns them
 * all off, "animation_speed <factor>" makes them faster (2) or slower (0.5) and
 * "animation <kind> <style>|disable" picks the style of one of them.
 *
 * Closing, minimizing and resizing animate a copy of the window's buffers (a
 * snapshot) while the real window is hidden or already gone. Windows that fade
 * or rise in and the desktop that slides in move the real windows, so they stay
 * live. Windows that zoom in wait for their first frame and animate a copy of it.
 * Windows that explode are handled by explode.c.
 */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
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
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"
#include "log.h"

#define OPEN_MS 180
#define ZOOM_MS 200
#define POP_MS 300
#define CLOSE_MS 160
#define MINIMIZE_MS 240
#define RESIZE_MS 180
#define BOUNCE_MS 320
#define SLIDE_MS 280
#define FADE_MS 220
#define RISE_PX 18
#define DROP_PX 36
#define OPEN_WAIT_MS 400 // longest wait for the first frame of a zooming window
#define FRAME_MS 8

/* Styles, in the order of the names below; the first one is the default. */
enum { OPEN_RISE, OPEN_FADE, OPEN_ZOOM, OPEN_POP, OPEN_DROP };
enum { CLOSE_SHRINK, CLOSE_FADE, CLOSE_GROW, CLOSE_DROP, CLOSE_EXPLODE };
enum { MINIMIZE_TASKBAR, MINIMIZE_FADE, MINIMIZE_SHRINK, MINIMIZE_DROP };
enum { MAXIMIZE_MORPH, MAXIMIZE_BOUNCE, MAXIMIZE_FADE };
enum { DESKTOP_SLIDE, DESKTOP_VERTICAL, DESKTOP_FADE, DESKTOP_ZOOM };

static const char *const kind_names[TW_ANIM_KIND_COUNT] = {
	"open", "close", "minimize", "maximize", "desktop",
};
static const char *const open_styles[] = { "rise", "fade", "zoom", "pop", "drop", NULL };
static const char *const close_styles[] = { "shrink", "fade", "grow", "drop", "explode", NULL };
static const char *const minimize_styles[] = { "taskbar", "fade", "shrink", "drop", NULL };
static const char *const maximize_styles[] = { "morph", "bounce", "fade", NULL };
static const char *const desktop_styles[] = { "slide", "vertical", "fade", "zoom", NULL };
static const char *const *const style_names[TW_ANIM_KIND_COUNT] = {
	open_styles, close_styles, minimize_styles, maximize_styles, desktop_styles,
};

enum ease {
	EASE_OUT,
	EASE_IN,
	EASE_IN_OUT,
	EASE_OUT_BACK, // overshoots a little and settles
};

enum kind {
	ANIM_LIVE,     // a live window fades in and rises or drops
	ANIM_GROW,     // a copy of a new window's first frame zooms in
	ANIM_SNAPSHOT, // a copy of a window moves between two boxes
	ANIM_DESKTOP,  // desktop switch: a copy of the old desktop, the new one offset
	ANIM_EXPLODE,  // a closing window blowing up (explode.c)
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
	struct sway_workspace *ws;  // ANIM_DESKTOP: the desktop coming in
	struct wlr_scene_tree *tree;
	list_t *pieces;             // struct piece *
	struct fbox captured;       // bounding box of the pieces
	struct fbox box0, box1;     // where the copy moves
	double alpha0, alpha1;
	float alpha;                // read by output_configure_scene
	double dx0, dy0, dx, dy;    // offset of the incoming desktop, or ANIM_LIVE's rise
	double scale0;              // ANIM_GROW: size of the first frame at the start
	bool has_frame;             // ANIM_GROW: the first frame was captured
	bool reveal;                // show con again at the end
	struct tw_explosion *explosion;
};

static list_t *anims;
static struct wl_event_source *timer;
static bool shutting_down;
static bool shaken; // the screen is moved by an explosion

bool tw_animation_parse_kind(const char *name, int *kind) {
	for (int i = 0; i < TW_ANIM_KIND_COUNT; i++) {
		if (strcasecmp(name, kind_names[i]) == 0) {
			*kind = i;
			return true;
		}
	}
	return false;
}

int tw_animation_parse_style(int kind, const char *name) {
	for (int i = 0; kind >= 0 && kind < TW_ANIM_KIND_COUNT && style_names[kind][i]; i++) {
		if (strcasecmp(name, style_names[kind][i]) == 0) {
			return i;
		}
	}
	return -1;
}

const char *const *tw_animation_styles(int kind) {
	return kind >= 0 && kind < TW_ANIM_KIND_COUNT ? style_names[kind] : NULL;
}

static bool enabled(enum tw_anim_kind kind) {
	return !shutting_down && config && config->active && !config->reading &&
		config->tw_animations && config->tw_animation_on[kind] && server.wl_event_loop;
}

static int style(enum tw_anim_kind kind) {
	return config->tw_animation_style[kind];
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
	case EASE_OUT_BACK:;
		const double c1 = 1.70158, c3 = c1 + 1;
		return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
	}
	return t;
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

static struct fbox scale_box(struct fbox b, double scale) {
	return (struct fbox){
		b.x + b.width * (1 - scale) / 2, b.y + b.height * (1 - scale) / 2,
		b.width * scale, b.height * scale,
	};
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
	// an overshooting curve moves past the end, the opacity stops there
	a->alpha = lerp(a->alpha0, a->alpha1, fmin(1, fmax(0, e)));
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

static struct anim *desktop_anim_for(struct sway_workspace *ws) {
	for (int i = 0; ws && anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->kind == ANIM_DESKTOP && a->ws == ws) {
			return a;
		}
	}
	return NULL;
}

int tw_animate_workspace_dx(struct sway_workspace *ws) {
	struct anim *a = desktop_anim_for(ws);
	return a ? (int)round(a->dx) : 0;
}

int tw_animate_workspace_dy(struct sway_workspace *ws) {
	struct anim *a = desktop_anim_for(ws);
	return a ? (int)round(a->dy) : 0;
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
		struct sway_workspace *ws = con->current.workspace;
		wlr_scene_node_set_position(&con->scene_tree->node,
			con->current.x + tw_animate_workspace_dx(ws),
			con->current.y + tw_animate_workspace_dy(ws) + tw_animate_container_dy(con));
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
		ws->current_gaps.top + area->y + tw_animate_workspace_dy(ws));
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
	if (a->explosion) {
		tw_explosion_destroy(a->explosion);
		if (root && root->layer_tree) {
			wlr_scene_node_set_position(&root->layer_tree->node, 0, 0);
		}
		shaken = false;
	}
	if (a->con) {
		if (a->kind == ANIM_LIVE) {
			a->con->tw.anim.active = false;
			a->con->tw.anim.alpha = 1;
			a->con->tw.anim.dy = 0;
			place_floating(a->con);
		}
		if (a->reveal) {
			reveal(a->con);
		}
	}
	if (a->kind == ANIM_DESKTOP) {
		a->dx = a->dy = 0;
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

/* A new window zooming in has its place and a first frame to copy. */
static bool first_frame_ready(struct sway_container *con) {
	return floating_shown(con) && con->current.width > 0 && con->current.height > 0 &&
		con->view && con->view->surface && wlr_surface_has_buffer(con->view->surface);
}

static void capture_first_frame(struct anim *a) {
	capture_container(a, a->con);
	measure(a);
	a->box0 = scale_box(a->captured, a->scale0);
	a->has_frame = true;
	apply_copy(a, 0);
}

static int tick(void *data) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double shake_x = 0, shake_y = 0;
	for (int i = anims ? anims->length - 1 : -1; i >= 0; i--) {
		struct anim *a = anims->items[i];
		double ms = (now.tv_sec - a->start.tv_sec) * 1000.0 +
			(now.tv_nsec - a->start.tv_nsec) / 1e6;
		if (a->kind == ANIM_EXPLODE) {
			double speed = config->tw_animation_speed > 0 ? config->tw_animation_speed : 1;
			if (!tw_explosion_update(a->explosion, ms * speed, &shake_x, &shake_y)) {
				list_del(anims, i);
				finish(a);
			}
			continue;
		}
		if (a->kind == ANIM_GROW && !a->has_frame) {
			if (a->con && ms < OPEN_WAIT_MS) {
				if (first_frame_ready(a->con)) {
					capture_first_frame(a);
					a->start = now;
				}
				continue;
			}
			list_del(anims, i);
			finish(a); // no frame in time: just show the window
			continue;
		}
		double t = a->duration > 0 ? fmin(1, ms / a->duration) : 1;
		double e = ease(a->ease, t);
		switch (a->kind) {
		case ANIM_LIVE:
			if (a->con) {
				a->con->tw.anim.alpha = fmin(1, e);
				a->con->tw.anim.dy = container_is_floating(a->con) ? a->dy0 * (1 - e) : 0;
				place_floating(a->con);
			}
			break;
		case ANIM_GROW:
		case ANIM_SNAPSHOT:
			apply_copy(a, e);
			break;
		case ANIM_DESKTOP:
			apply_copy(a, e);
			a->dx = a->dx0 * (1 - e);
			a->dy = a->dy0 * (1 - e);
			place_workspace(a->ws);
			break;
		case ANIM_EXPLODE:
			break; // handled above
		}
		if (t >= 1 || ((a->kind == ANIM_LIVE || a->kind == ANIM_GROW) && !a->con)) {
			list_del(anims, i);
			finish(a);
		}
	}
	if (shake_x != 0 || shake_y != 0 || shaken) {
		wlr_scene_node_set_position(&root->layer_tree->node, (int)round(shake_x),
			(int)round(shake_y));
		shaken = shake_x != 0 || shake_y != 0;
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

/* An animation that copies nodes: NULL (and nothing left behind) if that fails. */
static struct anim *copy_anim_new(enum kind kind, int ms, enum ease e) {
	struct anim *a = anim_new(kind, ms, e);
	if (a && !make_tree(a)) {
		remove_anim(a);
		return NULL;
	}
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
	if (!enabled(TW_ANIM_OPEN) || !con || !con->view ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return;
	}
	cancel_for(con);
	int s = style(TW_ANIM_OPEN);
	bool floating = container_is_floating(con);
	if ((s == OPEN_ZOOM || s == OPEN_POP) && floating) {
		bool pop = s == OPEN_POP;
		struct anim *a = copy_anim_new(ANIM_GROW, pop ? POP_MS : ZOOM_MS,
			pop ? EASE_OUT_BACK : EASE_OUT);
		if (!a) {
			return;
		}
		a->con = con;
		a->scale0 = pop ? 0.7 : 0.88;
		a->alpha0 = 0;
		a->alpha = 0;
		a->reveal = true;
		con->tw.anim.hidden = true;
		if (con->scene_tree) {
			wlr_scene_node_set_enabled(&con->scene_tree->node, false);
		}
		return;
	}
	// tiled windows only fade: they have no room to move in
	struct anim *a = anim_new(ANIM_LIVE, OPEN_MS, EASE_OUT);
	if (!a) {
		return;
	}
	a->con = con;
	a->dy0 = !floating ? 0 : s == OPEN_RISE ? RISE_PX : s == OPEN_DROP ? -DROP_PX : 0;
	con->tw.anim.active = true;
	con->tw.anim.alpha = 0;
	con->tw.anim.dy = a->dy0;
}

void tw_animate_close(struct sway_container *con) {
	if (!enabled(TW_ANIM_CLOSE) || !con || !con->scene_tree || !con->scene_tree->node.enabled ||
			con->current.fullscreen_mode != FULLSCREEN_NONE || con->current.tw_minimized ||
			!con->current.workspace || !workspace_is_visible(con->current.workspace)) {
		return;
	}
	cancel_for(con);
	if (style(TW_ANIM_CLOSE) == CLOSE_EXPLODE) {
		struct tw_explosion *explosion = tw_explosion_create(con, root->layers.floating);
		if (explosion) {
			struct anim *a = anim_new(ANIM_EXPLODE, 0, EASE_OUT);
			if (a) {
				a->explosion = explosion;
			} else {
				tw_explosion_destroy(explosion);
			}
			return;
		}
		// the fire images are still being made: shrink this once
	}
	struct anim *a = copy_anim_new(ANIM_SNAPSHOT, CLOSE_MS, EASE_IN);
	if (!a) {
		return;
	}
	capture_container(a, con);
	measure(a);
	switch (style(TW_ANIM_CLOSE)) {
	case CLOSE_SHRINK:
		a->box1 = scale_box(a->captured, 0.9);
		break;
	case CLOSE_GROW:
		a->box1 = scale_box(a->captured, 1.08);
		break;
	case CLOSE_DROP:
		a->box1.y += DROP_PX * 2;
		break;
	}
	a->alpha1 = 0;
	apply_copy(a, 0);
}

void tw_animate_minimize(struct sway_container *con, bool minimize) {
	struct sway_workspace *ws = con ? con->current.workspace : NULL;
	if (!enabled(TW_ANIM_MINIMIZE) || !con || !con->scene_tree || !container_is_floating(con) ||
			!ws || !ws->output || !workspace_is_visible(ws) ||
			con->current.fullscreen_mode != FULLSCREEN_NONE) {
		return;
	}
	cancel_for(con);
	struct anim *a = copy_anim_new(ANIM_SNAPSHOT, MINIMIZE_MS, minimize ? EASE_IN : EASE_OUT);
	if (!a) {
		return;
	}
	capture_container(a, con);
	measure(a);
	struct sway_output *output = ws->output;
	struct fbox away = a->captured;
	switch (style(TW_ANIM_MINIMIZE)) {
	case MINIMIZE_TASKBAR:;
		double w = fmax(40, a->captured.width * 0.12), h = fmax(30, a->captured.height * 0.12);
		away = (struct fbox){
			output->lx + output->width / 2.0 - w / 2, output->ly + output->height - h, w, h,
		};
		break;
	case MINIMIZE_SHRINK:
		away = scale_box(a->captured, 0.6);
		break;
	case MINIMIZE_DROP:
		away.y = output->ly + output->height;
		break;
	}
	if (minimize) {
		a->box1 = away;
		a->alpha1 = 0;
	} else {
		a->box0 = away;
		a->alpha0 = 0;
		a->con = con;
		a->reveal = true;
		con->tw.anim.hidden = true;
		wlr_scene_node_set_enabled(&con->scene_tree->node, false);
	}
	apply_copy(a, 0);
}

void tw_animate_resize(struct sway_container *con) {
	if (!enabled(TW_ANIM_MAXIMIZE) || !con || !floating_shown(con)) {
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
	int s = style(TW_ANIM_MAXIMIZE);
	struct anim *a = copy_anim_new(ANIM_SNAPSHOT, s == MAXIMIZE_BOUNCE ? BOUNCE_MS :
		s == MAXIMIZE_FADE ? FADE_MS : RESIZE_MS,
		s == MAXIMIZE_BOUNCE ? EASE_OUT_BACK : s == MAXIMIZE_FADE ? EASE_IN_OUT : EASE_OUT);
	if (!a) {
		return;
	}
	capture_container(a, con);
	measure(a);
	if (s == MAXIMIZE_FADE) {
		// the old look fades out over the window at its new place
		a->alpha1 = 0;
		apply_copy(a, 0);
		return;
	}
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
	if (!enabled(TW_ANIM_DESKTOP) || !old || old == ws || old->output != output ||
			old->current.fullscreen || ws->current.fullscreen) {
		return;
	}
	int old_index = list_find(output->workspaces, old);
	int new_index = list_find(output->workspaces, ws);
	if (old_index < 0 || new_index < 0) {
		return;
	}
	for (int i = anims ? anims->length - 1 : -1; i >= 0; i--) {
		struct anim *a = anims->items[i];
		if (a->kind == ANIM_DESKTOP) {
			list_del(anims, i);
			finish(a);
		}
	}
	int s = style(TW_ANIM_DESKTOP);
	bool slide = s == DESKTOP_SLIDE || s == DESKTOP_VERTICAL;
	struct anim *a = copy_anim_new(ANIM_DESKTOP, slide ? SLIDE_MS : FADE_MS,
		slide ? EASE_IN_OUT : EASE_OUT);
	if (!a) {
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
	switch (s) {
	case DESKTOP_SLIDE:
		a->box1.x -= direction * output->width;
		a->dx0 = direction * output->width;
		break;
	case DESKTOP_VERTICAL:
		a->box1.y -= direction * output->height;
		a->dy0 = direction * output->height;
		break;
	case DESKTOP_FADE:
		a->alpha1 = 0;
		break;
	case DESKTOP_ZOOM:
		a->box1 = scale_box(a->captured, 0.85);
		a->alpha1 = 0;
		break;
	}
	a->ws = ws;
	a->dx = a->dx0;
	a->dy = a->dy0;
	apply_copy(a, 0);
	place_workspace(ws);
}

void tw_animate_raise(void) {
	// windows raised since (e.g. the one focused after closing) stay below
	for (int i = 0; anims && i < anims->length; i++) {
		struct anim *a = anims->items[i];
		if (a->tree) {
			wlr_scene_node_raise_to_top(&a->tree->node);
		}
		if (a->explosion) {
			tw_explosion_raise(a->explosion);
		}
	}
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
	// the renderer still exists: the images' textures can go now
	tw_explosion_release();
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
