#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "log.h"
#include "popup.h"
#include "stringop.h"
#include "tw_paths.h"

/* ================= generic popup machinery ================= */

static struct psurface *catcher = NULL;
static struct loop_timer *close_timer = NULL;

cairo_t *popup_scratch_cairo(void) {
	static cairo_t *cr = NULL;
	if (!cr) {
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		cr = cairo_create(surface);
		cairo_surface_destroy(surface);
	}
	return cr;
}

static struct popup *popup_from_surface(struct psurface *s) {
	return s->data;
}

static void ps_render(struct psurface *s, cairo_t *cr) {
	struct popup *p = popup_from_surface(s);
	if (p && p->vtable->render) {
		p->vtable->render(p, cr);
	}
}

static void ps_motion(struct psurface *s, double x, double y) {
	struct popup *p = popup_from_surface(s);
	if (p && p->vtable->motion) {
		p->vtable->motion(p, x, y);
	}
}

static void ps_leave(struct psurface *s) {
	struct popup *p = popup_from_surface(s);
	if (p && p->vtable->leave) {
		p->vtable->leave(p);
	}
}

static void ps_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	struct popup *p = popup_from_surface(s);
	if (p && p->vtable->button) {
		p->vtable->button(p, x, y, button, pressed);
	}
}

static void ps_axis(struct psurface *s, double x, double y, int direction) {
	struct popup *p = popup_from_surface(s);
	if (p && p->vtable->axis) {
		p->vtable->axis(p, x, y, direction);
	}
}

static void ps_key(struct psurface *s, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct popup *p = popup_from_surface(s);
	if (!p) {
		return;
	}
	// keys go to the innermost popup
	while (p->child) {
		p = p->child;
	}
	if (p->vtable->key) {
		p->vtable->key(p, sym, utf8, mods);
	} else if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	}
}

static void ps_closed(struct psurface *s) {
	popup_close_later(s->panel);
}

static const struct psurface_impl popup_surface_impl = {
	.render = ps_render,
	.pointer_motion = ps_motion,
	.pointer_leave = ps_leave,
	.pointer_button = ps_button,
	.pointer_axis = ps_axis,
	.key = ps_key,
	.closed = ps_closed,
};

static void catcher_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	if (pressed) {
		popup_close_later(s->panel);
	}
}

static void catcher_closed(struct psurface *s) {
	popup_close_later(s->panel);
}

static const struct psurface_impl catcher_impl = {
	.pointer_button = catcher_button,
	.closed = catcher_closed,
};

void popup_set_dirty(struct popup *p) {
	if (p && p->surface) {
		psurface_set_dirty(p->surface);
	}
}

struct popup *popup_create(struct panel *panel, enum popup_kind kind,
		struct popup *parent, struct panel_output *output, int x, int y, int width,
		int height, const struct popup_vtable *vtable, void *data) {
	if (!output) {
		return NULL;
	}
	if (!parent) {
		popup_close_all(panel);
		tooltip_cancel(panel);
		catcher = psurface_create_catcher(panel, output, &catcher_impl, NULL);
	}
	if (width > output->width) {
		width = output->width;
	}
	if (height > output->height) {
		height = output->height;
	}
	if (x + width > output->width) {
		x = output->width - width;
	}
	if (y + height > output->height) {
		y = output->height - height;
	}
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}

	struct popup *p = calloc(1, sizeof(*p));
	p->kind = kind;
	p->panel = panel;
	p->output = output;
	p->parent = parent;
	p->vtable = vtable;
	p->data = data;
	p->x = x;
	p->y = y;
	p->width = width;
	p->height = height;
	if (parent) {
		if (parent->child) {
			popup_destroy(parent->child);
		}
		parent->child = p;
	} else {
		panel->popup = p;
	}

	struct psurface *s = psurface_create(panel, output, &popup_surface_impl, p,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-popup");
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(s->layer_surface, y, 0, 0, x);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_surface, 1);
	psurface_set_size(s, width, height);
	wl_surface_commit(s->surface);
	p->surface = s;
	panel_set_dirty(panel);
	return p;
}

void popup_destroy(struct popup *p) {
	if (!p) {
		return;
	}
	if (p->child) {
		popup_destroy(p->child);
	}
	if (p->parent && p->parent->child == p) {
		p->parent->child = NULL;
	}
	if (p->panel->popup == p) {
		p->panel->popup = NULL;
	}
	if (p->vtable->destroy) {
		p->vtable->destroy(p);
	}
	if (p->surface) {
		p->surface->data = NULL;
		psurface_destroy(p->surface);
	}
	free(p);
}

void popup_close_all(struct panel *panel) {
	if (close_timer) {
		loop_remove_timer(panel->loop, close_timer);
		close_timer = NULL;
	}
	bool had_popup = panel->popup != NULL;
	popup_destroy(panel->popup);
	panel->popup = NULL;
	if (catcher) {
		psurface_destroy(catcher);
		catcher = NULL;
	}
	if (had_popup) {
		panel_set_dirty(panel);
	}
}

static void close_timer_fired(void *data) {
	struct panel *panel = data;
	close_timer = NULL;
	popup_close_all(panel);
}

void popup_close_later(struct panel *panel) {
	if (!close_timer) {
		close_timer = loop_add_timer(panel->loop, 0, close_timer_fired, panel);
	}
}

bool popup_is_open(struct panel *panel, enum popup_kind kind) {
	return panel->popup && panel->popup->kind == kind;
}

/* ================= shared look ================= */

int popup_shadow_margin(struct panel *panel) {
	switch (panel_style(panel)) {
	case PS_CLASSIC:
		return 0;
	case PS_LUNA:
		return 3;
	case PS_FLUENT:
		return 10;
	default:
		return 6;
	}
}

double popup_radius(struct panel *panel, const char *prefix) {
	char key[64];
	snprintf(key, sizeof(key), "%s.radius", prefix);
	return tw_theme_double(panel->theme, key, panel_style(panel) == PS_FLUENT ? 8 : 0);
}

void popup_draw_frame(struct panel *panel, cairo_t *cr, int width, int height,
		int margin, const char *prefix) {
	enum pstyle style = panel_style(panel);
	const struct tw_theme *t = panel->theme;
	char key[64];
	double r = popup_radius(panel, prefix);
	double x = margin, y = margin, w = width - 2 * margin, h = height - 2 * margin;

	for (int i = margin; i >= 1; i--) {
		double a = 0.16 * pow(1.0 - (double)(i - 1) / margin, 2);
		double dy = style == PS_LUNA ? 1 : 0.5 * margin / 3;
		pd_rounded(cr, x - i + (style == PS_LUNA ? i : 0) + 0.5, y - i + dy + 0.5,
			w + 2 * i - 1, h + 2 * i - 1, r + i);
		cairo_set_source_rgba(cr, 0, 0, 0, a);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}

	snprintf(key, sizeof(key), "%s.bg", prefix);
	uint32_t bg_fallback = style == PS_CLASSIC ? 0xc0c0c0ff : style == PS_LUNA ? 0xffffffff :
		style == PS_AERO ? 0xf0f0f0ff : style == PS_FLUENT ? 0xf9f9f9ff : 0xf2f2f2ff;
	pd_rounded(cr, x, y, w, h, r);
	pd_fill(cr, t, key, y, h, bg_fallback);

	if (style == PS_CLASSIC) {
		pd_bevel(cr, x, y, w, h, false);
		return;
	}
	snprintf(key, sizeof(key), "%s.border", prefix);
	uint32_t border = tw_theme_color(t, key, style == PS_LUNA ? 0x8a867aff :
		style == PS_AERO ? 0x979797ff : style == PS_FLUENT ? 0x00000024 : 0xccccccff);
	pd_rounded(cr, x + 0.5, y + 0.5, w - 1, h - 1, r);
	pd_color(cr, border);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* ================= menus ================= */

enum menu_hotspot {
	MENU_HS_ITEM = 1,
};

struct menu {
	list_t *items;
	bool owns;
	char *context_id;
	int selected;
	int *ys, *hs;
	int item_h, sep_h, pad, margin, icon_col, arrow_col;
	bool classic_start;
	int banner_w;
	bool above;
	struct loop_timer *submenu_timer;
	int pending_submenu;
	char *run_command; // executed after closing
};

static const char *menu_font(struct panel *panel) {
	return tw_theme_str(panel->theme, "menu.font", bar_font(panel));
}

static void menu_destroy(struct popup *p) {
	struct menu *m = p->data;
	if (m->submenu_timer) {
		loop_remove_timer(p->panel->loop, m->submenu_timer);
	}
	if (m->owns) {
		menu_items_free(m->items);
	}
	free(m->ys);
	free(m->hs);
	free(m->context_id);
	free(m);
}

static bool selectable(struct menu_item *item) {
	return !item->separator && !item->disabled;
}

static void draw_check(cairo_t *cr, double x, double y, double s, uint32_t color) {
	cairo_save(cr);
	pd_color(cr, color);
	cairo_set_line_width(cr, s * 0.14);
	cairo_move_to(cr, x + s * 0.2, y + s * 0.52);
	cairo_line_to(cr, x + s * 0.42, y + s * 0.74);
	cairo_line_to(cr, x + s * 0.82, y + s * 0.28);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void menu_render(struct popup *p, cairo_t *cr) {
	struct menu *m = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	int w = p->surface->width, h = p->surface->height;
	int M = m->margin;

	popup_draw_frame(panel, cr, w, h, M, "menu");

	int content_x = M + m->pad + m->banner_w;
	int content_w = w - 2 * M - 2 * m->pad - m->banner_w;
	if (m->banner_w > 0) {
		double bx = M + 3, by = M + 3, bh = h - 2 * M - 6;
		cairo_rectangle(cr, bx, by, m->banner_w - 2, bh);
		cairo_pattern_t *grad = pd_gradient(tw_theme_str(t, "startmenu.banner_gradient",
			"0:#000080 1:#1084d0"), 0, by + bh, 0, by);
		cairo_set_source(cr, grad);
		cairo_fill(cr);
		cairo_pattern_destroy(grad);
		cairo_save(cr);
		cairo_translate(cr, bx, by + bh - 6);
		cairo_rotate(cr, -M_PI / 2);
		pd_text(cr, "Arial Black, Noto Sans Bold 13", tw_theme_str(t, "startmenu.banner_text",
			"tileWin"), 0, 0, bh - 12, m->banner_w - 2, 0xc0c0c0ff, PD_LEFT);
		cairo_restore(cr);
	}
	if (style == PS_AERO) {
		pd_rect(cr, content_x + m->icon_col - 2, M + m->pad, 1, h - 2 * M - 2 * m->pad, 0xe2e3e3ff);
		pd_rect(cr, content_x + m->icon_col - 1, M + m->pad, 1, h - 2 * M - 2 * m->pad, 0xffffffff);
	}

	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t hl_bg = tw_theme_color(t, "menu.hl_bg", style == PS_CLASSIC ? 0x000080ff :
		style == PS_LUNA ? 0x316ac5ff : style == PS_AERO ? 0xd9e8fbff :
		style == PS_FLUENT ? 0x0000000f : 0xdadadaff);
	uint32_t hl_fg = tw_theme_color(t, "menu.hl_fg",
		style == PS_CLASSIC || style == PS_LUNA ? 0xffffffff : 0x000000ff);
	uint32_t disabled = tw_theme_color(t, "menu.disabled_fg", 0x808080ff);
	int icon_size = m->classic_start ? 24 : tw_theme_int(t, "menu.icon_size", 16);

	for (int i = 0; i < m->items->length; i++) {
		struct menu_item *item = m->items->items[i];
		int y = m->ys[i], ih = m->hs[i];
		if (item->separator) {
			double sy = y + ih / 2.0;
			if (style == PS_CLASSIC) {
				pd_rect(cr, content_x + 1, sy - 1, content_w - 2, 1, 0x808080ff);
				pd_rect(cr, content_x + 1, sy, content_w - 2, 1, 0xffffffff);
			} else {
				pd_rect(cr, content_x + (style == PS_AERO ? m->icon_col : 4), sy,
					content_w - (style == PS_AERO ? m->icon_col : 8), 1,
					tw_theme_color(t, "menu.separator", style == PS_AERO ? 0xe0e0e0ff : 0x00000020));
			}
			continue;
		}
		bool sel = i == m->selected && selectable(item);
		if (sel) {
			switch (style) {
			case PS_AERO:
				pd_rounded(cr, content_x + 0.5, y + 0.5, content_w - 1, ih - 1, 3);
				pd_color(cr, hl_bg);
				cairo_fill_preserve(cr);
				pd_color(cr, tw_theme_color(t, "menu.hl_border", 0xaecff7ff));
				cairo_set_line_width(cr, 1);
				cairo_stroke(cr);
				break;
			case PS_FLUENT:
				pd_rounded(cr, content_x, y + 2, content_w, ih - 4, 4);
				pd_color(cr, hl_bg);
				cairo_fill(cr);
				break;
			default:
				pd_rect(cr, content_x, y, content_w, ih, hl_bg);
				break;
			}
		}
		uint32_t color = item->disabled ? disabled : sel ? hl_fg : fg;
		if (item->icon) {
			cairo_surface_t *icon = apps_icon(panel, item->icon, icon_size * p->surface->scale);
			if (icon) {
				pd_icon(cr, icon, content_x + (m->icon_col - icon_size) / 2.0,
					y + (ih - icon_size) / 2.0, icon_size);
			}
		}
		if (item->checked) {
			draw_check(cr, content_x + (m->icon_col - 14) / 2.0, y + (ih - 14) / 2.0, 14, color);
		}
		const char *font = item->bold ? tw_theme_str(t, "menu.bold_font", bar_bold_font(panel)) :
			menu_font(panel);
		pd_text(cr, font, item->label, content_x + m->icon_col, y,
			content_w - m->icon_col - m->arrow_col, ih, color, PD_LEFT);
		if (item->children) {
			pd_glyph_arrow(cr, content_x + content_w - m->arrow_col + 2, y + (ih - 12) / 2.0,
				12, 0, color);
		}
		psurface_add_hotspot(p->surface, content_x, y, content_w, ih, NULL,
			MENU_HS_ITEM, i, NULL);
	}
}

static void menu_open_submenu(struct popup *p, int index, bool select_first) {
	struct menu *m = p->data;
	struct menu_item *item = m->items->items[index];
	if (!item->children) {
		return;
	}
	if (p->child) {
		struct menu *cm = p->child->data;
		if (cm->items == item->children) {
			return;
		}
	}
	int x = p->x + p->width - m->margin - 3;
	int y = p->y + m->ys[index] - m->pad - m->margin;
	struct popup *child = menu_open_at(p->panel, p, p->output, item->children, false,
		x, y, false, m->context_id);
	if (!child) {
		return;
	}
	if (child->x < x) {
		// no room on the right: open to the left of the parent
		struct menu *cm = child->data;
		int nx = p->x - child->width + m->margin + cm->margin + 3;
		if (nx >= 0) {
			zwlr_layer_surface_v1_set_margin(child->surface->layer_surface, child->y, 0, 0, nx);
			child->x = nx;
			wl_surface_commit(child->surface->surface);
		}
	}
	if (select_first) {
		struct menu *cm = child->data;
		for (int i = 0; i < cm->items->length; i++) {
			if (selectable(cm->items->items[i])) {
				cm->selected = i;
				break;
			}
		}
	}
}

static void submenu_timer_fired(void *data) {
	struct popup *p = data;
	struct menu *m = p->data;
	m->submenu_timer = NULL;
	if (m->pending_submenu >= 0 && m->pending_submenu == m->selected) {
		menu_open_submenu(p, m->pending_submenu, false);
	}
}

static void menu_select(struct popup *p, int index) {
	struct menu *m = p->data;
	if (m->selected == index) {
		return;
	}
	m->selected = index;
	if (p->child) {
		struct menu *cm = p->child->data;
		struct menu_item *item = index >= 0 ? m->items->items[index] : NULL;
		if (!item || cm->items != item->children) {
			popup_destroy(p->child);
		}
	}
	if (m->submenu_timer) {
		loop_remove_timer(p->panel->loop, m->submenu_timer);
		m->submenu_timer = NULL;
	}
	if (index >= 0 && ((struct menu_item *)m->items->items[index])->children) {
		m->pending_submenu = index;
		m->submenu_timer = loop_add_timer(p->panel->loop, 250, submenu_timer_fired, p);
	}
	popup_set_dirty(p);
}

static void menu_motion(struct popup *p, double x, double y) {
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	struct menu *m = p->data;
	int index = hs ? (int)hs->id : -1;
	if (index < 0 && p->child) {
		return; // keep the open submenu while crossing the gap
	}
	if (index >= 0 && !selectable(m->items->items[index])) {
		index = -1;
	}
	menu_select(p, index);
}

static void menu_leave(struct popup *p) {
	struct menu *m = p->data;
	if (!p->child && m->selected >= 0) {
		m->selected = -1;
		popup_set_dirty(p);
	}
}

static void run_command_later(void *data);

struct deferred_command {
	struct panel *panel;
	char *command;
	char *context_id;
};

static void run_command_later(void *data) {
	struct deferred_command *dc = data;
	bar_run_command(dc->panel, dc->command, dc->context_id);
	free(dc->command);
	free(dc->context_id);
	free(dc);
}

static void menu_activate(struct popup *p, int index) {
	struct menu *m = p->data;
	if (index < 0 || index >= m->items->length) {
		return;
	}
	struct menu_item *item = m->items->items[index];
	if (!selectable(item)) {
		return;
	}
	if (item->children) {
		menu_open_submenu(p, index, true);
		popup_set_dirty(p);
		return;
	}
	if (!item->command) {
		return;
	}
	struct deferred_command *dc = calloc(1, sizeof(*dc));
	dc->panel = p->panel;
	dc->command = strdup(item->command);
	dc->context_id = m->context_id ? strdup(m->context_id) : NULL;
	struct panel *panel = p->panel;
	popup_close_later(panel);
	loop_add_timer(panel->loop, 1, run_command_later, dc);
}

static void menu_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		popup_close_later(p->panel);
		return;
	}
	menu_activate(p, (int)hs->id);
}

static int menu_step(struct menu *m, int from, int dir) {
	int n = m->items->length;
	for (int k = 1; k <= n; k++) {
		int i = ((from + dir * k) % n + n) % n;
		if (selectable(m->items->items[i])) {
			return i;
		}
	}
	return -1;
}

static void menu_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct menu *m = p->data;
	switch (sym) {
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		menu_select(p, menu_step(m, m->selected < 0 ? -1 : m->selected, 1));
		break;
	case XKB_KEY_Up:
		menu_select(p, menu_step(m, m->selected < 0 ? m->items->length : m->selected, -1));
		break;
	case XKB_KEY_Right:
		if (m->selected >= 0) {
			menu_open_submenu(p, m->selected, true);
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		menu_activate(p, m->selected);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_Escape:
		if (p->parent) {
			popup_destroy(p);
		} else {
			popup_close_later(p->panel);
		}
		break;
	default:
		if (utf8 && utf8[0] && !(mods & 1)) {
			// jump to the next item starting with the typed letter
			int n = m->items->length;
			for (int k = 1; k <= n; k++) {
				int i = ((m->selected + k) % n + n) % n;
				struct menu_item *item = m->items->items[i];
				if (selectable(item) && item->label &&
						strncasecmp(item->label, utf8, strlen(utf8)) == 0) {
					menu_select(p, i);
					break;
				}
			}
		}
		break;
	}
}

static const struct popup_vtable menu_vtable = {
	.render = menu_render,
	.motion = menu_motion,
	.leave = menu_leave,
	.button = menu_button,
	.key = menu_key,
	.destroy = menu_destroy,
};

static struct popup *menu_create(struct panel *panel, struct popup *parent,
		struct panel_output *output, list_t *items, bool owns, int x, int y,
		bool above, const char *context_id, bool classic_start) {
	if (!items || items->length == 0) {
		if (owns) {
			menu_items_free(items);
		}
		return NULL;
	}
	enum pstyle style = panel_style(panel);
	const struct tw_theme *t = panel->theme;
	struct menu *m = calloc(1, sizeof(*m));
	m->items = items;
	m->owns = owns;
	m->context_id = context_id ? strdup(context_id) : NULL;
	m->selected = -1;
	m->pending_submenu = -1;
	m->classic_start = classic_start;
	m->margin = popup_shadow_margin(panel);
	m->pad = style == PS_CLASSIC ? 3 : style == PS_FLUENT ? 5 : 3;
	int default_item_h = style == PS_CLASSIC ? 20 : style == PS_LUNA ? 22 :
		style == PS_AERO ? 24 : 32;
	m->item_h = classic_start ? tw_theme_int(t, "startmenu.item_height", 32) :
		tw_theme_int(t, "menu.item_height", default_item_h);
	m->sep_h = style == PS_CLASSIC ? 9 : style == PS_FLAT || style == PS_FLUENT ? 11 : 8;
	m->icon_col = classic_start ? 36 : style == PS_AERO ? 30 : 26;
	m->arrow_col = 20;
	m->banner_w = classic_start ? tw_theme_int(t, "startmenu.banner_width", 22) : 0;

	cairo_t *cr = popup_scratch_cairo();
	int max_text = 60;
	m->ys = calloc(items->length, sizeof(int));
	m->hs = calloc(items->length, sizeof(int));
	int cy = m->margin + m->pad;
	for (int i = 0; i < items->length; i++) {
		struct menu_item *item = items->items[i];
		m->ys[i] = cy;
		m->hs[i] = item->separator ? m->sep_h : m->item_h;
		cy += m->hs[i];
		if (item->label) {
			int tw = 0;
			pd_text_size(cr, item->bold ? bar_bold_font(panel) : menu_font(panel),
				item->label, &tw, NULL);
			if (tw > max_text) {
				max_text = tw;
			}
		}
	}
	int width = m->margin * 2 + m->pad * 2 + m->banner_w + m->icon_col + max_text +
		m->arrow_col + 12;
	if (width < (classic_start ? 190 : 150)) {
		width = classic_start ? 190 : 150;
	}
	if (width > 480) {
		width = 480;
	}
	int height = cy + m->pad + m->margin;
	if (above) {
		y -= height;
	}
	x -= m->margin;
	return popup_create(panel, classic_start ? POPUP_STARTMENU : POPUP_MENU, parent,
		output, x, y, width, height, &menu_vtable, m);
}

struct popup *menu_open_at(struct panel *panel, struct popup *parent,
		struct panel_output *output, list_t *items, bool owns, int x, int y,
		bool above, const char *context_id) {
	return menu_create(panel, parent, output, items, owns, x, y, above, context_id, false);
}

void menu_open(struct panel *panel, list_t *items, bool owns,
		struct popup_anchor anchor, const char *context_id) {
	struct popup *p = menu_create(panel, NULL, anchor.output, items, owns, anchor.x,
		anchor.y, anchor.above, context_id, false);
	if (p && anchor.right_align) {
		int nx = anchor.x - p->width;
		zwlr_layer_surface_v1_set_margin(p->surface->layer_surface, p->y, 0, 0, nx < 0 ? 0 : nx);
		p->x = nx < 0 ? 0 : nx;
		wl_surface_commit(p->surface->surface);
	}
}

void menu_open_start_classic(struct panel *panel, struct panel_output *output,
		list_t *items, int x, int y) {
	menu_create(panel, NULL, output, items, true, x, y, true, NULL, true);
}

list_t *power_menu_items(struct panel *panel) {
	list_t *items = create_list();
	struct twconf_node *sm = panel->config ? panel->config->startmenu : NULL;
	for (int i = 0; sm && i < twconf_count(sm); i++) {
		struct twconf_node *child = twconf_at(sm, i);
		if (strcmp(child->name, "power") == 0 && child->argc >= 2) {
			list_add(items, menu_item_new(child->argv[0], child->argv[1]));
		}
	}
	if (items->length == 0) {
		list_add(items, menu_item_new("Lock", "exec swaylock -f -c 000000"));
		list_add(items, menu_item_new("Sign out", "exit"));
		list_add(items, menu_item_new("Restart tileWin", "restart"));
		list_add(items, menu_item_separator());
		list_add(items, menu_item_new("Restart", "exec systemctl reboot"));
		list_add(items, menu_item_new("Shut down", "exec systemctl poweroff"));
	}
	return items;
}

list_t *theme_menu_items(struct panel *panel) {
	list_t *items = create_list();
	list_t *names = tw_theme_list();
	for (int i = 0; i < names->length; i++) {
		const char *name = names->items[i];
		char *error = NULL;
		struct tw_theme *theme = tw_theme_load(name, &error);
		free(error);
		char *cmd = format_str("theme %s", name);
		struct menu_item *item = menu_item_new(theme ? theme->title : name, cmd);
		item->checked = panel->theme && strcmp(panel->theme->name, name) == 0;
		list_add(items, item);
		free(cmd);
		tw_theme_free(theme);
	}
	list_free_items_and_destroy(names);
	return items;
}

/* ================= calendar ================= */

struct calendar {
	int year, month; // month 0..11
};

enum {
	CAL_HS_PREV = 1,
	CAL_HS_NEXT,
	CAL_HS_TODAY,
};

static int days_in_month(int year, int month) {
	static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
		return 29;
	}
	return days[month];
}

static void calendar_render(struct popup *p, cairo_t *cr) {
	struct calendar *cal = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	int M = popup_shadow_margin(panel);
	int w = p->surface->width, h = p->surface->height;
	popup_draw_frame(panel, cr, w, h, M, "menu");
	const char *font = menu_font(panel);
	const char *bold = bar_bold_font(panel);
	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t dim = tw_theme_color(t, "menu.disabled_fg", 0x808080ff);
	uint32_t accent = style == PS_CLASSIC ? 0x000080ff : style == PS_LUNA ? 0x316ac5ff :
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff);
	int x0 = M + 12, y0 = M + 8, cw = (w - 2 * M - 24) / 7;

	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);

	if (style == PS_CLASSIC) {
		pd_rect(cr, M + 3, M + 3, w - 2 * M - 6, 26, 0x000080ff);
	}
	char title[64];
	struct tm first = { .tm_year = cal->year - 1900, .tm_mon = cal->month, .tm_mday = 1 };
	mktime(&first);
	strftime(title, sizeof(title), "%B %Y", &first);
	uint32_t title_fg = style == PS_CLASSIC ? 0xffffffff : fg;
	pd_text(cr, bold, title, x0 + 28, y0, w - 2 * M - 80, 24, title_fg, PD_CENTER);
	pd_glyph_arrow(cr, x0 + 4, y0 + 5, 14, 2, title_fg);
	pd_glyph_arrow(cr, w - M - 12 - 20, y0 + 5, 14, 0, title_fg);
	psurface_add_hotspot(p->surface, x0, y0, 28, 24, NULL, CAL_HS_PREV, 0, NULL);
	psurface_add_hotspot(p->surface, w - M - 12 - 28, y0, 28, 24, NULL, CAL_HS_NEXT, 0, NULL);

	static const char *weekdays[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
	int wy = y0 + 32;
	for (int i = 0; i < 7; i++) {
		pd_text(cr, bold, weekdays[i], x0 + i * cw, wy, cw, 20, dim, PD_CENTER);
	}
	int offset = (first.tm_wday + 6) % 7;
	int days = days_in_month(cal->year, cal->month);
	int prev_days = days_in_month(cal->month == 0 ? cal->year - 1 : cal->year,
		cal->month == 0 ? 11 : cal->month - 1);
	int gy = wy + 24, rh = 28;
	for (int cell = 0; cell < 42; cell++) {
		int day = cell - offset + 1;
		bool other = day < 1 || day > days;
		int shown = day < 1 ? prev_days + day : day > days ? day - days : day;
		double cx = x0 + (cell % 7) * cw, cy = gy + (cell / 7) * rh;
		bool is_today = !other && day == today.tm_mday && cal->month == today.tm_mon &&
			cal->year == today.tm_year + 1900;
		uint32_t color = other ? dim : fg;
		if (is_today) {
			if (style == PS_FLAT || style == PS_FLUENT) {
				cairo_arc(cr, cx + cw / 2.0, cy + rh / 2.0, rh / 2.0 - 2, 0, 2 * M_PI);
				pd_color(cr, accent);
				cairo_fill(cr);
			} else {
				pd_rect(cr, cx + 2, cy + 2, cw - 4, rh - 4, accent);
			}
			color = 0xffffffff;
		}
		char num[8];
		snprintf(num, sizeof(num), "%d", shown);
		pd_text(cr, font, num, cx, cy, cw, rh, color, PD_CENTER);
	}
	char footer[96];
	strftime(footer, sizeof(footer), "%A, %d %B %Y", &today);
	int fy = gy + 6 * rh + 4;
	pd_rect(cr, x0, fy, w - 2 * M - 24, 1, style == PS_CLASSIC ? 0x808080ff : 0x00000020);
	pd_text(cr, font, footer, x0, fy + 4, w - 2 * M - 24, 24, accent == 0x000080ff ? fg : accent,
		PD_CENTER);
	psurface_add_hotspot(p->surface, x0, fy, w - 2 * M - 24, 28, NULL, CAL_HS_TODAY, 0, NULL);
}

static void calendar_shift(struct popup *p, int months) {
	struct calendar *cal = p->data;
	cal->month += months;
	while (cal->month < 0) {
		cal->month += 12;
		cal->year--;
	}
	while (cal->month > 11) {
		cal->month -= 12;
		cal->year++;
	}
	popup_set_dirty(p);
}

static void calendar_today(struct calendar *cal) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	cal->year = tm.tm_year + 1900;
	cal->month = tm.tm_mon;
}

static void calendar_button(struct popup *p, double x, double y, uint32_t button,
		bool pressed) {
	if (pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	if (hs->kind == CAL_HS_PREV) {
		calendar_shift(p, -1);
	} else if (hs->kind == CAL_HS_NEXT) {
		calendar_shift(p, 1);
	} else if (hs->kind == CAL_HS_TODAY) {
		calendar_today(p->data);
		popup_set_dirty(p);
	}
}

static void calendar_axis(struct popup *p, double x, double y, int direction) {
	calendar_shift(p, direction);
}

static void calendar_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_Page_Up) {
		calendar_shift(p, -1);
	} else if (sym == XKB_KEY_Right || sym == XKB_KEY_Page_Down) {
		calendar_shift(p, 1);
	}
}

static void calendar_destroy(struct popup *p) {
	free(p->data);
}

static const struct popup_vtable calendar_vtable = {
	.render = calendar_render,
	.button = calendar_button,
	.axis = calendar_axis,
	.key = calendar_key,
	.destroy = calendar_destroy,
};

void calendar_toggle(struct panel *panel, struct popup_anchor anchor) {
	if (popup_is_open(panel, POPUP_CALENDAR)) {
		popup_close_all(panel);
		return;
	}
	struct calendar *cal = calloc(1, sizeof(*cal));
	calendar_today(cal);
	int M = popup_shadow_margin(panel);
	int width = 7 * 36 + 24 + 2 * M;
	int height = 8 + 32 + 24 + 6 * 28 + 40 + 2 * M;
	int x = anchor.right_align ? anchor.x - width : anchor.x;
	int y = anchor.above ? anchor.y - height : anchor.y;
	popup_create(panel, POPUP_CALENDAR, NULL, anchor.output, x, y, width, height,
		&calendar_vtable, cal);
}

/* ================= run dialog ================= */

struct rundialog {
	char text[1024];
	list_t *history;
	int history_pos;
};

enum {
	RUN_HS_OK = 1,
	RUN_HS_CANCEL,
	RUN_HS_CLOSE,
};

static char *history_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = format_str("%s/run-history", dir);
	free(dir);
	return path;
}

static list_t *load_history(void) {
	list_t *list = create_list();
	char *path = history_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	char line[1024];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (*line) {
			list_add(list, strdup(line));
		}
	}
	if (f) {
		fclose(f);
	}
	free(path);
	return list;
}

static void save_history(list_t *list, const char *entry) {
	for (int i = 0; i < list->length; i++) {
		if (strcmp(list->items[i], entry) == 0) {
			free(list->items[i]);
			list_del(list, i);
			break;
		}
	}
	list_insert(list, 0, strdup(entry));
	size_t size = 1;
	for (int i = 0; i < list->length && i < 25; i++) {
		size += strlen(list->items[i]) + 1;
	}
	char *content = calloc(1, size);
	for (int i = 0; i < list->length && i < 25; i++) {
		strcat(content, list->items[i]);
		strcat(content, "\n");
	}
	char *path = history_path();
	if (path) {
		tw_write_string(path, content);
	}
	free(path);
	free(content);
}

static void draw_dialog_button(struct panel *panel, cairo_t *cr, double x, double y,
		double w, double h, const char *label, bool is_default) {
	enum pstyle style = panel_style(panel);
	if (style == PS_CLASSIC) {
		pd_rect(cr, x, y, w, h, 0xc0c0c0ff);
		if (is_default) {
			cairo_set_source_u32(cr, 0x000000ff);
			cairo_rectangle(cr, x - 0.5, y - 0.5, w + 1, h + 1);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
		pd_bevel(cr, x, y, w, h, false);
		pd_text(cr, bar_font(panel), label, x, y, w, h, 0x000000ff, PD_CENTER);
		return;
	}
	double r = style == PS_LUNA ? 3 : style == PS_FLUENT ? 4 : 2;
	pd_rounded(cr, x + 0.5, y + 0.5, w - 1, h - 1, r);
	if (style == PS_LUNA || style == PS_AERO) {
		cairo_pattern_t *g = pd_gradient("0:#ffffff 0.5:#f0f0f0 0.51:#e5e5e5 1:#dcdcdc",
			0, y, 0, y + h);
		cairo_set_source(cr, g);
		cairo_pattern_destroy(g);
	} else {
		pd_color(cr, is_default ? 0x0078d4ff : 0xfbfbfbff);
	}
	cairo_fill_preserve(cr);
	pd_color(cr, is_default && style == PS_LUNA ? 0x003c74ff :
		style == PS_FLAT || style == PS_FLUENT ? (is_default ? 0x0067c0ff : 0x00000028) : 0x707070ff);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	bool light = is_default && (style == PS_FLAT || style == PS_FLUENT);
	pd_text(cr, bar_font(panel), label, x, y, w, h, light ? 0xffffffff : 0x000000ff, PD_CENTER);
}

static void rundialog_render(struct popup *p, cairo_t *cr) {
	struct rundialog *rd = p->data;
	struct panel *panel = p->panel;
	enum pstyle style = panel_style(panel);
	int M = popup_shadow_margin(panel);
	int w = p->surface->width - 2 * M, h = p->surface->height - 2 * M;
	cairo_translate(cr, M, M);

	// dialog frame and title bar in the theme's look
	int title_h = style == PS_CLASSIC ? 18 : style == PS_LUNA ? 28 : 32;
	uint32_t body = style == PS_CLASSIC ? 0xc0c0c0ff : style == PS_LUNA ? 0xece9d8ff :
		style == PS_AERO ? 0xf0f0f0ff : style == PS_FLUENT ? 0xf3f3f3ff : 0xffffffff;
	cairo_translate(cr, -M, -M);
	popup_draw_frame(panel, cr, w + 2 * M, h + 2 * M, M, "run");
	cairo_translate(cr, M, M);
	double r = style == PS_FLUENT ? 8 : 0;
	pd_rounded(cr, 1, 1, w - 2, h - 2, r);
	pd_color(cr, body);
	cairo_fill(cr);

	double ty = style == PS_CLASSIC ? 3 : 0, tx = style == PS_CLASSIC ? 3 : 0;
	double tw = w - 2 * tx;
	uint32_t title_fg = 0x000000ff;
	switch (style) {
	case PS_CLASSIC: {
		pd_bevel(cr, 0, 0, w, h, false);
		cairo_rectangle(cr, tx, ty, tw, title_h);
		cairo_pattern_t *g = pd_gradient("0:#000080 1:#1084d0", tx, 0, tx + tw, 0);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		title_fg = 0xffffffff;
		break;
	}
	case PS_LUNA: {
		pd_rounded4(cr, 0, 0, w, title_h + 4, 7, 7, 0, 0);
		cairo_pattern_t *g = pd_gradient("0:#3d8df5 0.1:#0a66f7 0.5:#0058ee 1:#0842c1", 0, 0, 0, title_h);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		pd_rect(cr, 0, title_h, 3, h - title_h, 0x0831d9ff);
		pd_rect(cr, w - 3, title_h, 3, h - title_h, 0x0831d9ff);
		pd_rect(cr, 0, h - 3, w, 3, 0x0831d9ff);
		title_fg = 0xffffffff;
		break;
	}
	case PS_AERO: {
		cairo_rectangle(cr, 0, 0, w, title_h);
		pd_color(cr, 0x8fb4dae8);
		cairo_fill(cr);
		break;
	}
	default:
		break;
	}
	pd_text(cr, bar_bold_font(panel), "Run", tx + 8, ty, tw - 40, title_h, title_fg, PD_LEFT);
	// close button
	double cbw = style == PS_CLASSIC ? 16 : 46;
	double cbx = style == PS_CLASSIC ? w - 3 - 2 - 16 : w - cbw;
	double cby = style == PS_CLASSIC ? ty + 2 : 0;
	double cbh = style == PS_CLASSIC ? 14 : title_h;
	if (style == PS_CLASSIC) {
		pd_rect(cr, cbx, cby, cbw, cbh, 0xc0c0c0ff);
		pd_bevel(cr, cbx, cby, cbw, cbh, false);
	}
	uint32_t xc = style == PS_CLASSIC ? 0x000000ff : title_fg;
	cairo_save(cr);
	pd_color(cr, xc);
	cairo_set_line_width(cr, style == PS_CLASSIC ? 1.6 : 1);
	double cx = cbx + cbw / 2, cy = cby + cbh / 2, s = style == PS_CLASSIC ? 3.5 : 5;
	cairo_move_to(cr, cx - s, cy - s);
	cairo_line_to(cr, cx + s, cy + s);
	cairo_move_to(cr, cx + s, cy - s);
	cairo_line_to(cr, cx - s, cy + s);
	cairo_stroke(cr);
	cairo_restore(cr);
	psurface_add_hotspot(p->surface, M + cbx, M + cby, cbw, cbh, NULL, RUN_HS_CLOSE, 0, NULL);

	double bx = 16, by = ty + title_h + 14;
	cairo_surface_t *icon = apps_icon(panel, "system-run", 32 * p->surface->scale);
	if (!icon) {
		icon = apps_icon(panel, "utilities-terminal", 32 * p->surface->scale);
	}
	pd_icon(cr, icon, bx, by, 32);
	cairo_save(cr);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(bar_font(panel));
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_width(layout, (w - bx - 60) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
	pango_layout_set_text(layout, "Type the name of a program, folder, document, or "
		"Internet resource, and tileWin will open it for you.", -1);
	pd_color(cr, 0x000000ff);
	cairo_move_to(cr, bx + 48, by);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	cairo_restore(cr);

	double fy = by + 52;
	pd_text(cr, bar_font(panel), "Open:", bx, fy, 50, 24, 0x000000ff, PD_LEFT);
	double fx = bx + 48, fw = w - fx - 16, fh = 24;
	pd_rect(cr, fx, fy, fw, fh, 0xffffffff);
	if (style == PS_CLASSIC) {
		pd_bevel(cr, fx, fy, fw, fh, true);
	} else if (style == PS_FLAT || style == PS_FLUENT) {
		cairo_rectangle(cr, fx + 0.5, fy + 0.5, fw - 1, fh - 1);
		pd_color(cr, 0x00000040);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_rect(cr, fx, fy + fh - 2, fw, 2, 0x0067c0ff);
	} else {
		cairo_rectangle(cr, fx + 0.5, fy + 0.5, fw - 1, fh - 1);
		pd_color(cr, 0x7f9db9ff);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	int text_w = 0;
	pd_text_size(cr, bar_font(panel), rd->text, &text_w, NULL);
	double shift = text_w > fw - 14 ? text_w - (fw - 14) : 0;
	cairo_save(cr);
	cairo_rectangle(cr, fx + 2, fy + 2, fw - 4, fh - 4);
	cairo_clip(cr);
	pd_text(cr, bar_font(panel), rd->text, fx + 5 - shift, fy, text_w + 10, fh, 0x000000ff, PD_LEFT);
	pd_rect(cr, fx + 5 - shift + text_w + 1, fy + 5, 1, fh - 10, 0x000000ff);
	cairo_restore(cr);

	double btn_w = 75, btn_h = style == PS_CLASSIC ? 23 : 26;
	double btn_y = h - btn_h - 14;
	draw_dialog_button(panel, cr, w - 16 - 2 * btn_w - 8, btn_y, btn_w, btn_h, "OK", true);
	draw_dialog_button(panel, cr, w - 16 - btn_w, btn_y, btn_w, btn_h, "Cancel", false);
	psurface_add_hotspot(p->surface, M + w - 16 - 2 * btn_w - 8, M + btn_y, btn_w, btn_h,
		NULL, RUN_HS_OK, 0, NULL);
	psurface_add_hotspot(p->surface, M + w - 16 - btn_w, M + btn_y, btn_w, btn_h,
		NULL, RUN_HS_CANCEL, 0, NULL);
}

static void rundialog_execute(struct popup *p) {
	struct rundialog *rd = p->data;
	char *text = rd->text;
	while (*text == ' ') {
		text++;
	}
	if (!*text) {
		return;
	}
	save_history(rd->history, text);
	char *expanded = tw_expand_home(text);
	struct stat st;
	bool open = strstr(text, "://") || (stat(expanded, &st) == 0 &&
		(S_ISDIR(st.st_mode) || !(st.st_mode & S_IXUSR)));
	char *cmd = open ? format_str("exec xdg-open '%s'", expanded) : format_str("exec %s", text);
	struct deferred_command *dc = calloc(1, sizeof(*dc));
	dc->panel = p->panel;
	dc->command = cmd;
	free(expanded);
	popup_close_later(p->panel);
	loop_add_timer(p->panel->loop, 1, run_command_later, dc);
}

static void rundialog_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	if (hs->kind == RUN_HS_OK) {
		rundialog_execute(p);
	} else if (hs->kind == RUN_HS_CANCEL || hs->kind == RUN_HS_CLOSE) {
		popup_close_later(p->panel);
	}
}

static void rundialog_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct rundialog *rd = p->data;
	size_t len = strlen(rd->text);
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		rundialog_execute(p);
		return;
	case XKB_KEY_BackSpace:
		while (len > 0) {
			unsigned char c = rd->text[--len];
			rd->text[len] = '\0';
			if ((c & 0xc0) != 0x80) {
				break;
			}
		}
		break;
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		if (rd->history->length > 0) {
			rd->history_pos += sym == XKB_KEY_Up ? 1 : -1;
			if (rd->history_pos < 0) {
				rd->history_pos = 0;
			}
			if (rd->history_pos > rd->history->length) {
				rd->history_pos = rd->history->length;
			}
			snprintf(rd->text, sizeof(rd->text), "%s", rd->history_pos == 0 ? "" :
				(char *)rd->history->items[rd->history_pos - 1]);
		}
		break;
	default:
		if ((mods & 1) && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
			rd->text[0] = '\0';
		} else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
				len + strlen(utf8) < sizeof(rd->text) - 1) {
			strcat(rd->text, utf8);
		}
		break;
	}
	popup_set_dirty(p);
}

static void rundialog_destroy(struct popup *p) {
	struct rundialog *rd = p->data;
	list_free_items_and_destroy(rd->history);
	free(rd);
}

static const struct popup_vtable rundialog_vtable = {
	.render = rundialog_render,
	.button = rundialog_button,
	.key = rundialog_key,
	.destroy = rundialog_destroy,
};

void rundialog_open(struct panel *panel, struct panel_output *output) {
	struct rundialog *rd = calloc(1, sizeof(*rd));
	rd->history = load_history();
	if (rd->history->length > 0) {
		snprintf(rd->text, sizeof(rd->text), "%s", (char *)rd->history->items[0]);
		rd->history_pos = 1;
	}
	int M = popup_shadow_margin(panel);
	int width = 420 + 2 * M, height = 200 + 2 * M;
	bool bottom = panel->config->layouts[panel->layout].bottom;
	int bar = output->bar ? output->bar->height : 0;
	int y = bottom ? output->height - bar - height - 8 : bar + 8;
	popup_create(panel, POPUP_RUN, NULL, output, 8, y, width, height, &rundialog_vtable, rd);
}
