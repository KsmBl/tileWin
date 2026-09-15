#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "draw.h"
#include "popup.h"
#include "textfield.h"
#include "stringop.h"
#include "tw_desktop.h"

/*
 * Built-in application launcher: a centered search box with a filtered list
 * of applications, driven by the keyboard. Enter with no matching app runs
 * the typed text as a command.
 */

#define LAUNCHER_ROWS 8
#define ROW_HEIGHT 44
#define SEARCH_HEIGHT 52
#define LAUNCHER_WIDTH 620

enum {
	HS_ROW = 1,
};

struct launcher {
	list_t *apps;    // struct tw_desktop_entry *, owned
	list_t *results; // borrowed from apps
	char query[256];
	struct text_cursor tc;
	int selected;
	int scroll;
	bool inside;
	double px, py;
};

static int entry_name_cmp(const void *a, const void *b) {
	const struct tw_desktop_entry *ea = *(struct tw_desktop_entry **)a;
	const struct tw_desktop_entry *eb = *(struct tw_desktop_entry **)b;
	return strcasecmp(ea->name, eb->name);
}

static void update_results(struct launcher *l) {
	l->results->length = 0;
	// best matches first; an empty query scores every app 1 (alphabetical)
	for (int score = 5; score >= 1; score--) {
		for (int i = 0; i < l->apps->length; i++) {
			struct tw_desktop_entry *e = l->apps->items[i];
			if (apps_match_score(e, l->query) == score) {
				list_add(l->results, e);
			}
		}
	}
	l->selected = 0;
	l->scroll = 0;
}

static void launcher_render(struct popup *p, cairo_t *cr) {
	struct launcher *l = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	int M = popup_shadow_margin(panel);
	double W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "menu");

	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t dim = tw_theme_color(t, "menu.disabled_fg", 0x808080ff);
	uint32_t hl_bg = tw_theme_color(t, "menu.hl_bg", style == PS_CLASSIC ? 0x000080ff :
		style == PS_LUNA ? 0x316ac5ff : style == PS_AERO ? 0xd9e8fbff : 0x0000001a);
	uint32_t hl_fg = tw_theme_color(t, "menu.hl_fg",
		style == PS_CLASSIC || style == PS_LUNA ? 0xffffffff : 0x000000ff);
	const char *font = tw_theme_str(t, "menu.font", bar_font(panel));
	double radius = popup_radius(panel, "menu");
	uint32_t field_bg = tw_theme_color(t, "menu.field_bg", 0xffffffff);
	uint32_t field_fg = tw_theme_color(t, "menu.field_fg", 0x000000ff);

	// search field
	double sx = M + 10, sy = M + 10, sw = W - 2 * M - 20, sh = SEARCH_HEIGHT - 12;
	if (style == PS_CLASSIC) {
		pd_rect(cr, sx, sy, sw, sh, field_bg);
		pd_bevel(cr, sx, sy, sw, sh, true);
	} else {
		pd_rounded(cr, sx + 0.5, sy + 0.5, sw - 1, sh - 1, radius > 0 ? 6 : 2);
		pd_color(cr, field_bg);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000030);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_rect(cr, sx + 1, sy + sh - 2, sw - 2, 2,
			tw_theme_color(t, "taskbar.indicator", 0x0078d4ff));
	}
	pd_glyph_search(cr, sx + 12, sy + (sh - 18) / 2, 18, field_fg);
	if (!l->query[0]) {
		pd_text(cr, font, "Type to search apps, or enter a command", sx + 42, sy, sw - 52, sh,
			dim, PD_LEFT);
	}
	struct text_style ts = { .font = font, .fg = field_fg, .caret = true };
	text_style_colors(p->panel, &ts);
	text_draw(cr, &ts, l->query, &l->tc, sx + 42, sy, sw - 52, sh);

	// results
	double ly = M + SEARCH_HEIGHT;
	int n = l->results->length;
	if (n == 0) {
		char hint[320];
		snprintf(hint, sizeof(hint), "Press Enter to run \"%s\"", l->query);
		pd_text(cr, font, l->query[0] ? hint : "No applications found", M + 20, ly,
			W - 2 * M - 40, ROW_HEIGHT, dim, PD_LEFT);
		return;
	}
	if (l->selected < l->scroll) {
		l->scroll = l->selected;
	}
	if (l->selected >= l->scroll + LAUNCHER_ROWS) {
		l->scroll = l->selected - LAUNCHER_ROWS + 1;
	}
	for (int i = l->scroll; i < n && i < l->scroll + LAUNCHER_ROWS; i++) {
		struct tw_desktop_entry *e = l->results->items[i];
		double ry = ly + (i - l->scroll) * ROW_HEIGHT;
		double rx = M + 6, rw = W - 2 * M - 12;
		bool hover = l->inside && l->px >= rx && l->px < rx + rw &&
			l->py >= ry && l->py < ry + ROW_HEIGHT;
		bool sel = i == l->selected;
		if (sel || hover) {
			pd_rounded(cr, rx, ry + 2, rw, ROW_HEIGHT - 4, style == PS_CLASSIC ? 0 : 4);
			pd_color(cr, sel ? hl_bg : (hl_bg & 0xffffff00) | ((hl_bg & 0xff) / 2));
			cairo_fill(cr);
		}
		uint32_t color = sel ? hl_fg : fg;
		cairo_surface_t *icon = e->icon ? apps_icon(panel, e->icon, 32 * p->surface->scale) : NULL;
		pd_icon(cr, icon, rx + 8, ry + (ROW_HEIGHT - 32) / 2.0, 32);
		double tx = rx + 52;
		const char *subtitle = e->generic_name ? e->generic_name : e->comment;
		if (subtitle) {
			pd_text(cr, bar_bold_font(panel), e->name, tx, ry + 3, rw - 60, ROW_HEIGHT / 2.0,
				color, PD_LEFT);
			pd_text(cr, font, subtitle, tx, ry + ROW_HEIGHT / 2.0 - 3, rw - 60,
				ROW_HEIGHT / 2.0, sel ? hl_fg : dim, PD_LEFT);
		} else {
			pd_text(cr, bar_bold_font(panel), e->name, tx, ry, rw - 60, ROW_HEIGHT, color, PD_LEFT);
		}
		psurface_add_hotspot(p->surface, rx, ry, rw, ROW_HEIGHT, NULL, HS_ROW, i, NULL);
	}
}

struct deferred_exec {
	struct panel *panel;
	char *command;
};

static void run_later(void *data) {
	struct deferred_exec *d = data;
	ipc_panel_command(d->panel, d->command);
	free(d->command);
	free(d);
}

static void activate(struct popup *p, int index) {
	struct launcher *l = p->data;
	struct panel *panel = p->panel;
	if (index >= 0 && index < l->results->length) {
		apps_launch(panel, l->results->items[index]);
	} else if (l->query[0]) {
		struct deferred_exec *d = calloc(1, sizeof(*d));
		d->panel = panel;
		d->command = format_str("exec %s", l->query);
		loop_add_timer(panel->loop, 1, run_later, d);
	} else {
		return;
	}
	popup_close_later(panel);
}

static void launcher_motion(struct popup *p, double x, double y) {
	struct launcher *l = p->data;
	l->px = x;
	l->py = y;
	l->inside = true;
	popup_set_dirty(p);
}

static void launcher_leave(struct popup *p) {
	struct launcher *l = p->data;
	l->inside = false;
	popup_set_dirty(p);
}

static void launcher_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == HS_ROW) {
		activate(p, (int)hs->id);
	}
}

static void launcher_axis(struct popup *p, double x, double y, int direction) {
	struct launcher *l = p->data;
	int n = l->results->length;
	l->selected += direction;
	if (l->selected < 0) {
		l->selected = 0;
	}
	if (l->selected >= n) {
		l->selected = n - 1;
	}
	popup_set_dirty(p);
}

static void launcher_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct launcher *l = p->data;
	int n = l->results->length;
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		activate(p, n > 0 ? l->selected : -1);
		return;
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		if (l->selected < n - 1) {
			l->selected++;
		}
		break;
	case XKB_KEY_Up:
	case XKB_KEY_ISO_Left_Tab:
		if (l->selected > 0) {
			l->selected--;
		}
		break;
	case XKB_KEY_Page_Down:
		l->selected = l->selected + LAUNCHER_ROWS < n ? l->selected + LAUNCHER_ROWS : n - 1;
		break;
	case XKB_KEY_Page_Up:
		l->selected = l->selected > LAUNCHER_ROWS ? l->selected - LAUNCHER_ROWS : 0;
		break;
	default:
		switch (text_key(l->query, sizeof(l->query), &l->tc, sym, utf8, mods)) {
		case TEXT_KEY_IGNORED:
			return;
		case TEXT_KEY_CHANGED:
			update_results(l);
			break;
		case TEXT_KEY_MOVED:
			break;
		}
		break;
	}
	popup_set_dirty(p);
}

static void launcher_destroy(struct popup *p) {
	struct launcher *l = p->data;
	list_free(l->results);
	tw_desktop_list_free(l->apps);
	free(l);
}

static const struct popup_vtable launcher_vtable = {
	.render = launcher_render,
	.motion = launcher_motion,
	.leave = launcher_leave,
	.button = launcher_button,
	.axis = launcher_axis,
	.key = launcher_key,
	.destroy = launcher_destroy,
};

void launcher_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_LAUNCHER)) {
		popup_close_all(panel);
		return;
	}
	struct launcher *l = calloc(1, sizeof(*l));
	list_t *all = tw_desktop_scan();
	l->apps = create_list();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (e->no_display || e->hidden || !e->exec) {
			tw_desktop_entry_free(e);
		} else {
			list_add(l->apps, e);
		}
	}
	list_free(all);
	list_qsort(l->apps, entry_name_cmp);
	l->results = create_list();
	update_results(l);

	int M = popup_shadow_margin(panel);
	int width = LAUNCHER_WIDTH + 2 * M;
	int height = SEARCH_HEIGHT + LAUNCHER_ROWS * ROW_HEIGHT + 8 + 2 * M;
	int x = (output->width - width) / 2;
	int y = output->height / 5;
	if (y + height > output->height) {
		y = output->height - height;
	}
	popup_create(panel, POPUP_LAUNCHER, NULL, output, x, y, width, height,
		&launcher_vtable, l);
}
