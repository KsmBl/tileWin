/*
 * Task view (Win+Tab): thumbnails of the windows of a desktop and a strip of
 * all desktops (workspaces) of the output. Click a window to switch to it,
 * click a desktop to go to it and see its windows (click it again to close
 * the view), drag windows onto desktops or "New desktop" to move them, close
 * windows and desktops.
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
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
#include "cairo_util.h"
#include "list.h"
#include "log.h"
#include "stringop.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MARGIN 48
#define GAP 32
#define TITLE_H 30
#define MIN_CARD_W 180
#define STRIP_H 180
#define DESK_W 200
#define DESK_GAP 24
#define LABEL_H 26
#define DRAG_THRESHOLD 8
#define REFRESH_MS 1000

struct tv_window {
	struct sway_container *con;
	struct wlr_box card;  // title bar and thumbnail, output-local
	struct wlr_box thumb;
	struct wlr_box close;
	struct wlr_scene_tree *tree;
};

struct tv_desk {
	struct sway_workspace *ws; // NULL for "New desktop" or a destroyed desktop
	bool is_new;
	struct wlr_box preview;
	struct wlr_box close;
	struct wlr_scene_tree *tree;
};

enum tv_target {
	TV_NONE,
	TV_WINDOW,
	TV_WINDOW_CLOSE,
	TV_DESK,
	TV_DESK_CLOSE,
};

static struct {
	bool active;
	struct sway_seat *seat;
	struct sway_output *output;
	struct sway_workspace *shown; // desktop whose windows are shown
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *chrome;
	struct wlr_scene_tree *thumbs;
	list_t *windows; // struct tv_window *
	list_t *desks;   // struct tv_desk *
	int columns;
	enum tv_target hover;
	int hover_index;
	int selected; // window selected with the keyboard, -1 for none
	struct {
		bool pressed;
		enum tv_target target;
		int index;
		double x, y;   // press position, output-local
		bool dragging;
		double dx, dy; // pointer offset in the dragged thumbnail
	} press;
	struct wl_event_source *refresh;
	struct wl_event_source *idle;
} tv;

static void rebuild(void);

bool tw_taskview_active(void) {
	return tv.active;
}

/* ---------- theme ---------- */

static uint32_t theme_color(const char *key, uint32_t fallback) {
	return tw_theme_color(tw_theme, key, fallback);
}

static bool classic_style(void) {
	return tw_theme && tw_theme->style && strcmp(tw_theme->style, "win95") == 0;
}

static double radius(void) {
	const char *style = tw_theme && tw_theme->style ? tw_theme->style : "";
	int fallback = strcmp(style, "win11") == 0 ? 8 :
		strcmp(style, "winxp") == 0 || strcmp(style, "win7") == 0 ? 6 : 0;
	return tw_theme_int(tw_theme, "taskview.radius", fallback);
}

static uint32_t accent(void) {
	return theme_color("taskview.accent", classic_style() ? 0x000080ff : 0x0078d7ff);
}

static const char *font(void) {
	return tw_theme_str(tw_theme, "taskview.font",
		tw_theme_str(tw_theme, "alttab.font", "Segoe UI, Noto Sans 10"));
}

/* ---------- drawing ---------- */

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r <= 0) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	r = fmin(r, fmin(w, h) / 2);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void draw_label(cairo_t *cr, const char *text, double x, double y, double w,
		double h, uint32_t color, bool center) {
	if (!text || !*text || w <= 4) {
		return;
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font());
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_single_paragraph_mode(layout, true);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_set_source_u32(cr, color);
	cairo_move_to(cr, center ? x + (w - tw) / 2 : x, y + floor((h - th) / 2));
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void draw_close(cairo_t *cr, const struct wlr_box *b, bool hot, uint32_t fg) {
	if (hot) {
		rounded(cr, b->x, b->y, b->width, b->height, radius() > 0 ? 4 : 0);
		cairo_set_source_u32(cr, 0xe81123ff);
		cairo_fill(cr);
		fg = 0xffffffff;
	}
	double cx = b->x + b->width / 2.0, cy = b->y + b->height / 2.0, s = 5;
	cairo_set_source_u32(cr, fg);
	cairo_set_line_width(cr, 1.5);
	cairo_move_to(cr, cx - s, cy - s);
	cairo_line_to(cr, cx + s, cy + s);
	cairo_move_to(cr, cx + s, cy - s);
	cairo_line_to(cr, cx - s, cy + s);
	cairo_stroke(cr);
}

static bool target_is(enum tv_target target, int index) {
	return tv.hover == target && tv.hover_index == index;
}

static void render_chrome(void) {
	if (!tv.active || !tv.chrome) {
		return;
	}
	int W = tv.output->width, H = tv.output->height;
	float scale = tv.output->wlr_output->scale;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)ceil(W * scale), (int)ceil(H * scale));
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	uint32_t fg = theme_color("taskview.fg", 0xffffffff);
	uint32_t hover_bg = theme_color("taskview.hover", 0xffffff26);
	cairo_set_source_u32(cr, theme_color("taskview.bg", 0x141414d8));
	cairo_paint(cr);

	for (int i = 0; i < tv.windows->length; i++) {
		struct tv_window *w = tv.windows->items[i];
		bool dragged = tv.press.dragging && tv.press.index == i;
		bool hot = !tv.press.dragging && (target_is(TV_WINDOW, i) || target_is(TV_WINDOW_CLOSE, i));
		bool selected = i == tv.selected;
		struct wlr_box c = w->card;
		if (hot || selected || dragged) {
			rounded(cr, c.x - 6, c.y - 4, c.width + 12, c.height + 10, radius());
			cairo_set_source_u32(cr, hover_bg);
			cairo_fill(cr);
		}
		if (selected) {
			rounded(cr, c.x - 6, c.y - 4, c.width + 12, c.height + 10, radius());
			cairo_set_source_u32(cr, accent());
			cairo_set_line_width(cr, 3);
			cairo_stroke(cr);
		}
		if (dragged) {
			continue;
		}
		// placeholder behind the thumbnail, visible while a window has no buffer
		cairo_rectangle(cr, w->thumb.x, w->thumb.y, w->thumb.width, w->thumb.height);
		cairo_set_source_u32(cr, 0x00000060);
		cairo_fill(cr);
		double tx = c.x + 4;
		cairo_surface_t *icon = w->con->view ?
			tw_icon_for_view(w->con->view, (int)ceil(20 * scale)) : NULL;
		if (icon) {
			int iw = cairo_image_surface_get_width(icon);
			cairo_save(cr);
			cairo_translate(cr, tx, c.y + (TITLE_H - 20) / 2.0);
			cairo_scale(cr, 20.0 / iw, 20.0 / iw);
			cairo_set_source_surface(cr, icon, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			tx += 26;
		}
		draw_label(cr, w->con->title, tx, c.y, c.x + c.width - tx - (hot ? 32 : 4), TITLE_H,
			fg, false);
		if (hot) {
			draw_close(cr, &w->close, target_is(TV_WINDOW_CLOSE, i), fg);
		}
	}
	if (tv.windows->length == 0) {
		draw_label(cr, "No windows on this desktop", 0, 0, W, H - STRIP_H, fg, true);
	}

	// desktop strip
	cairo_rectangle(cr, 0, H - STRIP_H, W, STRIP_H);
	cairo_set_source_u32(cr, theme_color("taskview.strip", 0x00000040));
	cairo_fill(cr);
	struct sway_workspace *active = output_get_active_workspace(tv.output);
	uint32_t wallpaper = tw_theme_color(tw_theme, "wallpaper.color", 0x3a6ea5ff);
	int number = 0;
	for (int i = 0; i < tv.desks->length; i++) {
		struct tv_desk *d = tv.desks->items[i];
		if (!d->ws && !d->is_new) {
			continue;
		}
		struct wlr_box p = d->preview;
		bool hot = target_is(TV_DESK, i) || target_is(TV_DESK_CLOSE, i);
		if (d->is_new) {
			rounded(cr, p.x + 0.5, p.y + 0.5, p.width - 1, p.height - 1, radius());
			cairo_set_source_u32(cr, hot ? 0xffffff40 : 0xffffff18);
			cairo_fill_preserve(cr);
			cairo_set_source_u32(cr, hot ? accent() : 0xffffff60);
			cairo_set_line_width(cr, hot ? 2 : 1);
			cairo_stroke(cr);
			double cx = p.x + p.width / 2.0, cy = p.y + p.height / 2.0;
			cairo_set_source_u32(cr, fg);
			cairo_set_line_width(cr, 2);
			cairo_move_to(cr, cx - 12, cy);
			cairo_line_to(cr, cx + 12, cy);
			cairo_move_to(cr, cx, cy - 12);
			cairo_line_to(cr, cx, cy + 12);
			cairo_stroke(cr);
			draw_label(cr, "New desktop", p.x, p.y + p.height, p.width, LABEL_H, fg, true);
			continue;
		}
		number++;
		cairo_rectangle(cr, p.x, p.y, p.width, p.height);
		cairo_set_source_u32(cr, wallpaper);
		cairo_fill(cr);
		bool current = d->ws == active, shown = d->ws == tv.shown;
		if (current || hot || shown) {
			cairo_rectangle(cr, p.x - 2, p.y - 2, p.width + 4, p.height + 4);
			cairo_set_source_u32(cr, hot || current ? accent() : 0xffffff90);
			cairo_set_line_width(cr, hot ? 4 : 3);
			cairo_stroke(cr);
		}
		char label[64];
		snprintf(label, sizeof(label), "Desktop %d", number);
		draw_label(cr, label, p.x, p.y + p.height, p.width, LABEL_H, fg, true);
		if (hot && !tv.press.dragging && tv.desks->length > 2) {
			rounded(cr, d->close.x, d->close.y, d->close.width, d->close.height, 4);
			cairo_set_source_u32(cr, target_is(TV_DESK_CLOSE, i) ? 0xe81123ff : 0x000000a0);
			cairo_fill(cr);
			draw_close(cr, &d->close, false, 0xffffffff);
		}
	}
	cairo_destroy(cr);
	tw_scene_buffer_set_surface(tv.chrome, surface, W, H);
}

/* ---------- thumbnails ---------- */

static void collect_view(struct sway_container *con, void *data) {
	list_t *list = data;
	if (con->view && con->view->surface && !con->node.destroying) {
		list_add(list, con);
	}
}

static void content_size(struct sway_container *con, double *w, double *h) {
	*w = con->pending.content_width > 0 ? con->pending.content_width : con->pending.width;
	*h = con->pending.content_height > 0 ? con->pending.content_height : con->pending.height;
	if (*w < 1) {
		*w = 1;
	}
	if (*h < 1) {
		*h = 1;
	}
}

static void layout_windows(void) {
	int n = tv.windows->length;
	if (n == 0) {
		return;
	}
	int W = tv.output->width, H = tv.output->height;
	struct wlr_box area = { MARGIN, MARGIN, W - 2 * MARGIN, H - STRIP_H - MARGIN - 16 };
	int best_cols = 1;
	double best = -1;
	for (int cols = 1; cols <= n; cols++) {
		int rows = (n + cols - 1) / cols;
		double cell_w = (area.width - (cols - 1) * GAP) / (double)cols;
		double cell_h = (area.height - (rows - 1) * GAP) / (double)rows - TITLE_H;
		if (cell_w < 40 || cell_h < 30) {
			break;
		}
		double worst = 1e9;
		for (int i = 0; i < n; i++) {
			double cw, ch;
			content_size(((struct tv_window *)tv.windows->items[i])->con, &cw, &ch);
			worst = fmin(worst, fmin(cell_w / cw, cell_h / ch));
		}
		if (worst > best) {
			best = worst;
			best_cols = cols;
		}
	}
	int cols = best_cols, rows = (n + cols - 1) / cols;
	tv.columns = cols;
	double cell_w = (area.width - (cols - 1) * GAP) / (double)cols;
	double cell_h = (area.height - (rows - 1) * GAP) / (double)rows - TITLE_H;
	double max_scale = tw_theme_double(tw_theme, "taskview.max_scale", 0.6);
	for (int i = 0; i < n; i++) {
		struct tv_window *w = tv.windows->items[i];
		int row = i / cols, col = i % cols;
		int in_row = row < rows - 1 ? cols : n - (rows - 1) * cols;
		double row_x = area.x + (area.width - (in_row * cell_w + (in_row - 1) * GAP)) / 2;
		double cw, ch;
		content_size(w->con, &cw, &ch);
		double s = fmin(fmin(cell_w / cw, cell_h / ch), max_scale);
		int tw = (int)round(cw * s), th = (int)round(ch * s);
		double cx = row_x + col * (cell_w + GAP) + cell_w / 2;
		double cy = area.y + row * (cell_h + TITLE_H + GAP) + TITLE_H + cell_h / 2;
		w->thumb = (struct wlr_box){ (int)round(cx - tw / 2.0), (int)round(cy - th / 2.0), tw, th };
		int card_w = tw > MIN_CARD_W ? tw : MIN_CARD_W;
		w->card = (struct wlr_box){ (int)round(cx - card_w / 2.0), w->thumb.y - TITLE_H,
			card_w, th + TITLE_H };
		w->close = (struct wlr_box){ w->card.x + w->card.width - 28, w->card.y + 3, 24, 24 };
	}
}

static void layout_desks(void) {
	int W = tv.output->width, H = tv.output->height;
	int n = tv.desks->length;
	int dw = DESK_W;
	int max_w = (W - 2 * MARGIN - (n - 1) * DESK_GAP) / (n > 0 ? n : 1);
	if (dw > max_w) {
		dw = max_w > 40 ? max_w : 40;
	}
	int dh = (int)round(dw * (double)H / W);
	if (dh > STRIP_H - LABEL_H - 32) {
		dh = STRIP_H - LABEL_H - 32;
		dw = (int)round(dh * (double)W / H);
	}
	int total = n * dw + (n - 1) * DESK_GAP;
	int x = (W - total) / 2;
	int y = H - STRIP_H + (STRIP_H - dh - LABEL_H) / 2;
	for (int i = 0; i < n; i++) {
		struct tv_desk *d = tv.desks->items[i];
		d->preview = (struct wlr_box){ x + i * (dw + DESK_GAP), y, dw, dh };
		d->close = (struct wlr_box){ d->preview.x + dw - 26, d->preview.y + 4, 22, 22 };
	}
}

static void snapshot_desk(struct tv_desk *d) {
	if (!d->ws) {
		return;
	}
	d->tree = wlr_scene_tree_create(tv.thumbs);
	if (!d->tree) {
		return;
	}
	wlr_scene_node_set_position(&d->tree->node, d->preview.x, d->preview.y);
	double s = (double)d->preview.width / tv.output->width;
	if (tv.output->tw_wallpaper) {
		tw_snapshot_tree(d->tree, &tv.output->tw_wallpaper->node, s, 0, 0);
	}
	list_t *cons = create_list();
	workspace_for_each_container(d->ws, collect_view, cons);
	for (int i = 0; i < cons->length; i++) {
		struct sway_container *con = cons->items[i];
		if (con->pending.tw_minimized) {
			continue;
		}
		double x = (con->pending.content_x - tv.output->lx) * s;
		double y = (con->pending.content_y - tv.output->ly) * s;
		if (x >= d->preview.width || y >= d->preview.height) {
			continue;
		}
		tw_snapshot_view(d->tree, con->view, s, s, x, y);
	}
	list_free(cons);
}

static void clear(void) {
	if (tv.thumbs) {
		wlr_scene_node_destroy(&tv.thumbs->node);
		tv.thumbs = NULL;
	}
	if (tv.windows) {
		list_free_items_and_destroy(tv.windows);
	}
	if (tv.desks) {
		list_free_items_and_destroy(tv.desks);
	}
	tv.windows = create_list();
	tv.desks = create_list();
}

static bool output_valid(void) {
	return tv.output && list_find(root->outputs, tv.output) >= 0 && tv.output->enabled;
}

static void rebuild(void) {
	if (!tv.active) {
		return;
	}
	if (!output_valid()) {
		tw_taskview_close();
		return;
	}
	struct sway_container *selected = tv.selected >= 0 && tv.selected < tv.windows->length ?
		((struct tv_window *)tv.windows->items[tv.selected])->con : NULL;
	clear();
	tv.thumbs = wlr_scene_tree_create(tv.tree);
	if (!tv.shown || tv.shown->node.destroying || tv.shown->output != tv.output) {
		tv.shown = output_get_active_workspace(tv.output);
	}

	for (int i = 0; i < tv.output->workspaces->length; i++) {
		struct tv_desk *d = calloc(1, sizeof(*d));
		d->ws = tv.output->workspaces->items[i];
		list_add(tv.desks, d);
	}
	struct tv_desk *add = calloc(1, sizeof(*add));
	add->is_new = true;
	list_add(tv.desks, add);
	layout_desks();

	list_t *cons = create_list();
	if (tv.shown) {
		workspace_for_each_container(tv.shown, collect_view, cons);
	}
	tv.selected = -1;
	for (int i = 0; i < cons->length; i++) {
		struct tv_window *w = calloc(1, sizeof(*w));
		w->con = cons->items[i];
		if (w->con == selected) {
			tv.selected = i;
		}
		list_add(tv.windows, w);
	}
	list_free(cons);
	layout_windows();

	if (tv.thumbs) {
		for (int i = 0; i < tv.desks->length; i++) {
			snapshot_desk(tv.desks->items[i]);
		}
		for (int i = 0; i < tv.windows->length; i++) {
			struct tv_window *w = tv.windows->items[i];
			w->tree = wlr_scene_tree_create(tv.thumbs);
			if (!w->tree) {
				continue;
			}
			wlr_scene_node_set_position(&w->tree->node, w->thumb.x, w->thumb.y);
			double cw, ch;
			content_size(w->con, &cw, &ch);
			tw_snapshot_view(w->tree, w->con->view, w->thumb.width / cw,
				w->thumb.width / cw, 0, 0);
		}
	}
	if (tv.hover_index >= (tv.hover == TV_DESK || tv.hover == TV_DESK_CLOSE ?
			tv.desks->length : tv.windows->length)) {
		tv.hover = TV_NONE;
	}
	render_chrome();
}

static void handle_idle(void *data) {
	tv.idle = NULL;
	rebuild();
}

static void rebuild_later(void) {
	if (tv.active && !tv.idle) {
		tv.idle = wl_event_loop_add_idle(server.wl_event_loop, handle_idle, NULL);
	}
}

static int handle_refresh(void *data) {
	// keep the thumbnails live, but never under a dragged window
	if (tv.active && !tv.press.pressed) {
		rebuild();
	}
	if (tv.active && tv.refresh) {
		wl_event_source_timer_update(tv.refresh, REFRESH_MS);
	}
	return 0;
}

/* ---------- desktops ---------- */

static void move_to_workspace(struct sway_container *con, struct sway_workspace *ws) {
	if (!con || !ws || con->pending.workspace == ws || strchr(ws->name, '"')) {
		return;
	}
	if (con->pending.workspace) {
		// the desktop the window leaves stays, even when it becomes empty
		con->pending.workspace->tw_keep = true;
	}
	char *cmd = format_str("move container to workspace \"%s\"", ws->name);
	list_t *results = execute_command(cmd, tv.active ? tv.seat : input_manager_current_seat(), con);
	while (results && results->length > 0) {
		free_cmd_results(results->items[0]);
		list_del(results, 0);
	}
	list_free(results);
	free(cmd);
}

struct sway_workspace *tw_desktop_new(struct sway_output *output) {
	if (!output) {
		return NULL;
	}
	char name[16];
	for (int n = 1; n < 10000; n++) {
		snprintf(name, sizeof(name), "%d", n);
		if (!workspace_by_name(name) && !workspace_by_number(name)) {
			break;
		}
	}
	struct sway_workspace *ws = workspace_create(output, name);
	if (ws) {
		ws->tw_keep = true;
	}
	return ws;
}

/*
 * Moves a desktop one place to the left or right. Desktops are numbered and
 * sorted by their number, so the two desktops swap numbers: the windows stay
 * where they are and the desktop lands in the other place.
 */
bool tw_desktop_move(struct sway_workspace *ws, int direction) {
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || direction == 0) {
		return false;
	}
	int index = list_find(output->workspaces, ws);
	int target = index + (direction < 0 ? -1 : 1);
	if (index < 0 || target < 0 || target >= output->workspaces->length) {
		return false;
	}
	struct sway_workspace *other = output->workspaces->items[target];
	if (!isdigit((unsigned char)ws->name[0]) || !isdigit((unsigned char)other->name[0])) {
		return false; // named workspaces keep their name and their place
	}
	char *name = ws->name;
	ws->name = other->name;
	other->name = name;
	wlr_ext_workspace_handle_v1_set_name(ws->ext_workspace, ws->name);
	wlr_ext_workspace_handle_v1_set_name(other->ext_workspace, other->name);
	output_sort_workspaces(output);
	ipc_event_workspace(NULL, ws, "rename");
	ipc_event_workspace(NULL, other, "rename");
	return true;
}

bool tw_desktop_close(struct sway_workspace *ws) {
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || output->workspaces->length < 2) {
		return false;
	}
	int index = list_find(output->workspaces, ws);
	struct sway_workspace *target = output->workspaces->items[index > 0 ? index - 1 : index + 1];
	if (workspace_is_visible(ws)) {
		workspace_switch(target);
	}
	list_t *cons = create_list();
	workspace_for_each_container(ws, collect_view, cons);
	for (int i = 0; i < cons->length; i++) {
		move_to_workspace(cons->items[i], target);
	}
	list_free(cons);
	ws->tw_keep = false;
	if (tv.shown == ws) {
		tv.shown = target;
	}
	if (!ws->node.destroying) {
		workspace_consider_destroy(ws);
	}
	transaction_commit_dirty();
	return true;
}

/* ---------- open and close ---------- */

void tw_taskview_open(struct sway_seat *seat) {
	if (tv.active) {
		return;
	}
	tw_alttab_cancel();
	tw_panel_command("close"); // the start menu and other taskbar popups
	struct sway_workspace *ws = seat_get_focused_workspace(seat);
	if (!ws || !ws->output) {
		return;
	}
	memset(&tv, 0, sizeof(tv));
	tv.active = true;
	tv.seat = seat;
	tv.output = ws->output;
	tv.shown = ws;
	tv.selected = -1;
	tv.tree = wlr_scene_tree_create(root->layers.seat);
	if (!tv.tree) {
		tv.active = false;
		return;
	}
	wlr_scene_node_set_position(&tv.tree->node, tv.output->lx, tv.output->ly);
	tv.chrome = wlr_scene_buffer_create(tv.tree, NULL);
	rebuild();
	struct sway_container *focus = seat_get_focused_container(seat);
	for (int i = 0; tv.windows && i < tv.windows->length; i++) {
		if (((struct tv_window *)tv.windows->items[i])->con == focus) {
			tv.selected = i;
		}
	}
	render_chrome();
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
	cursor_set_image(seat->cursor, "default", NULL);
	tv.refresh = wl_event_loop_add_timer(server.wl_event_loop, handle_refresh, NULL);
	if (tv.refresh) {
		wl_event_source_timer_update(tv.refresh, REFRESH_MS);
	}
}

void tw_taskview_close(void) {
	if (!tv.active) {
		return;
	}
	tv.active = false;
	if (tv.refresh) {
		wl_event_source_remove(tv.refresh);
	}
	if (tv.idle) {
		wl_event_source_remove(tv.idle);
	}
	if (tv.tree) {
		wlr_scene_node_destroy(&tv.tree->node);
	}
	if (tv.windows) {
		list_free_items_and_destroy(tv.windows);
	}
	if (tv.desks) {
		list_free_items_and_destroy(tv.desks);
	}
	struct sway_seat *seat = tv.seat;
	memset(&tv, 0, sizeof(tv));
	if (seat) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		seatop_rebase(seat, now.tv_sec * 1000 + now.tv_nsec / 1000000);
	}
}

void tw_taskview_toggle(struct sway_seat *seat) {
	if (tv.active) {
		tw_taskview_close();
	} else {
		tw_taskview_open(seat);
	}
}

static void activate(struct sway_container *con) {
	struct sway_seat *seat = tv.seat;
	tw_taskview_close();
	if (!con || con->node.destroying) {
		return;
	}
	struct sway_workspace *ws = con->pending.workspace;
	if (ws && !workspace_is_visible(ws)) {
		workspace_switch(ws);
	}
	if (con->pending.tw_minimized) {
		tw_minimize(con, false);
	}
	seat_set_focus_container(seat, con);
	container_raise_floating(con);
	transaction_commit_dirty();
}

/* ---------- input ---------- */

static bool in_box(const struct wlr_box *b, double x, double y) {
	return x >= b->x && y >= b->y && x < b->x + b->width && y < b->y + b->height;
}

static void hit_test(double x, double y, enum tv_target *target, int *index) {
	*target = TV_NONE;
	*index = -1;
	for (int i = 0; i < tv.desks->length; i++) {
		struct tv_desk *d = tv.desks->items[i];
		if (!d->ws && !d->is_new) {
			continue;
		}
		struct wlr_box area = d->preview;
		area.height += LABEL_H;
		if (in_box(&area, x, y)) {
			*index = i;
			*target = !d->is_new && tv.desks->length > 2 && in_box(&d->close, x, y) ?
				TV_DESK_CLOSE : TV_DESK;
			return;
		}
	}
	for (int i = 0; i < tv.windows->length; i++) {
		struct tv_window *w = tv.windows->items[i];
		if (in_box(&w->card, x, y)) {
			*index = i;
			*target = in_box(&w->close, x, y) ? TV_WINDOW_CLOSE : TV_WINDOW;
			return;
		}
	}
}

static void local_cursor(struct sway_seat *seat, double *x, double *y) {
	*x = seat->cursor->cursor->x - tv.output->lx;
	*y = seat->cursor->cursor->y - tv.output->ly;
}

void tw_taskview_motion(struct sway_seat *seat) {
	if (!tv.active || !output_valid()) {
		return;
	}
	double x, y;
	local_cursor(seat, &x, &y);
	if (tv.press.pressed && tv.press.target == TV_WINDOW && !tv.press.dragging &&
			hypot(x - tv.press.x, y - tv.press.y) > DRAG_THRESHOLD &&
			tv.press.index < tv.windows->length) {
		struct tv_window *w = tv.windows->items[tv.press.index];
		tv.press.dragging = true;
		tv.press.dx = tv.press.x - w->thumb.x;
		tv.press.dy = tv.press.y - w->thumb.y;
		if (w->tree) {
			wlr_scene_node_raise_to_top(&w->tree->node);
		}
	}
	enum tv_target target;
	int index;
	hit_test(x, y, &target, &index);
	if (tv.press.dragging) {
		struct tv_window *w = tv.windows->items[tv.press.index];
		if (w->tree) {
			wlr_scene_node_set_position(&w->tree->node, round(x - tv.press.dx),
				round(y - tv.press.dy));
		}
		if (target != TV_DESK && target != TV_DESK_CLOSE) {
			target = TV_NONE;
			index = -1;
		} else {
			target = TV_DESK;
		}
	}
	if (target == tv.hover && index == tv.hover_index) {
		return;
	}
	tv.hover = target;
	tv.hover_index = index;
	render_chrome();
}

static void drop(struct tv_window *w, int desk_index) {
	struct sway_container *con = w->con;
	struct tv_desk *d = desk_index >= 0 && desk_index < tv.desks->length ?
		tv.desks->items[desk_index] : NULL;
	struct sway_workspace *target = NULL;
	if (d && d->is_new) {
		target = tw_desktop_new(tv.output);
	} else if (d) {
		target = d->ws;
	}
	if (target) {
		move_to_workspace(con, target);
		transaction_commit_dirty();
	}
}

void tw_taskview_button(struct sway_seat *seat, uint32_t button, bool pressed) {
	if (!tv.active || !output_valid()) {
		return;
	}
	double x, y;
	local_cursor(seat, &x, &y);
	enum tv_target target;
	int index;
	hit_test(x, y, &target, &index);
	if (pressed) {
		if (button == BTN_LEFT || button == BTN_MIDDLE) {
			tv.press.pressed = true;
			tv.press.target = button == BTN_MIDDLE && target == TV_WINDOW ?
				TV_WINDOW_CLOSE : target;
			tv.press.index = index;
			tv.press.x = x;
			tv.press.y = y;
			tv.press.dragging = false;
		}
		return;
	}
	if (!tv.press.pressed) {
		return;
	}
	tv.press.pressed = false;
	if (tv.press.dragging) {
		tv.press.dragging = false;
		if (tv.press.index < tv.windows->length && (target == TV_DESK || target == TV_DESK_CLOSE)) {
			drop(tv.windows->items[tv.press.index], index);
		}
		rebuild();
		return;
	}
	if (target != tv.press.target || index != tv.press.index) {
		if (tv.press.target == TV_WINDOW_CLOSE && target == TV_WINDOW && index == tv.press.index &&
				button == BTN_MIDDLE) {
			target = TV_WINDOW_CLOSE; // middle click closes
		} else {
			return;
		}
	}
	switch (target) {
	case TV_NONE:
		tw_taskview_close();
		break;
	case TV_WINDOW:
		activate(((struct tv_window *)tv.windows->items[index])->con);
		break;
	case TV_WINDOW_CLOSE:
		view_close(((struct tv_window *)tv.windows->items[index])->con->view);
		break;
	case TV_DESK: {
		struct tv_desk *d = tv.desks->items[index];
		if (d->is_new) {
			struct sway_workspace *ws = tw_desktop_new(tv.output);
			if (ws) {
				tv.shown = ws;
			}
			rebuild();
		} else if (d->ws && d->ws == tv.shown) {
			struct sway_workspace *ws = d->ws;
			tw_taskview_close();
			workspace_switch(ws);
			transaction_commit_dirty();
		} else if (d->ws) {
			// go to the desktop and show its windows; the view stays open and
			// the desktop left stays, even when it is empty
			struct sway_workspace *left = seat_get_focused_workspace(tv.seat);
			if (left) {
				left->tw_keep = true;
			}
			workspace_switch(d->ws);
			transaction_commit_dirty();
			tv.shown = d->ws;
			tv.selected = -1;
			rebuild();
		}
		break;
	}
	case TV_DESK_CLOSE: {
		struct tv_desk *d = tv.desks->items[index];
		if (d->ws) {
			tw_desktop_close(d->ws);
			rebuild();
		}
		break;
	}
	}
}

bool tw_taskview_handle_key(xkb_keysym_t sym, bool pressed, uint32_t modifiers) {
	if (!tv.active) {
		return false;
	}
	if (!pressed) {
		return false;
	}
	int n = tv.windows->length;
	switch (sym) {
	case XKB_KEY_Escape:
		tw_taskview_close();
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		if (modifiers & WLR_MODIFIER_LOGO) {
			tw_taskview_close();
		} else if (n > 0) {
			tv.selected = (tv.selected + ((modifiers & WLR_MODIFIER_SHIFT) ? n - 1 : 1)) % n;
			render_chrome();
		}
		break;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		if (n > 0) {
			int step = sym == XKB_KEY_Left ? -1 : sym == XKB_KEY_Right ? 1 :
				sym == XKB_KEY_Up ? -tv.columns : tv.columns;
			int next = tv.selected < 0 ? 0 : tv.selected + step;
			if (next >= 0 && next < n) {
				tv.selected = next;
			}
			render_chrome();
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (tv.selected >= 0 && tv.selected < n) {
			activate(((struct tv_window *)tv.windows->items[tv.selected])->con);
		} else {
			tw_taskview_close();
		}
		break;
	case XKB_KEY_Delete:
		if (tv.selected >= 0 && tv.selected < n) {
			view_close(((struct tv_window *)tv.windows->items[tv.selected])->con->view);
		}
		break;
	default:
		break;
	}
	// keys never reach the windows behind the task view
	return true;
}

void tw_taskview_container_destroyed(struct sway_container *con) {
	if (!tv.active || !tv.windows) {
		return;
	}
	for (int i = 0; i < tv.windows->length; i++) {
		struct tv_window *w = tv.windows->items[i];
		if (w->con != con) {
			continue;
		}
		if (w->tree) {
			wlr_scene_node_destroy(&w->tree->node);
		}
		free(w);
		list_del(tv.windows, i);
		if (tv.press.pressed && tv.press.index >= i) {
			tv.press.pressed = false;
			tv.press.dragging = false;
		}
		tv.selected = tv.selected >= tv.windows->length ? tv.windows->length - 1 : tv.selected;
		tv.hover = TV_NONE;
		break;
	}
	rebuild_later();
}

void tw_taskview_workspace_destroyed(struct sway_workspace *ws) {
	if (!tv.active || !tv.desks) {
		return;
	}
	for (int i = 0; i < tv.desks->length; i++) {
		struct tv_desk *d = tv.desks->items[i];
		if (d->ws == ws) {
			d->ws = NULL;
		}
	}
	if (tv.shown == ws) {
		tv.shown = NULL;
	}
	rebuild_later();
}
