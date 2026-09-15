/*
 * The snipping toolbar (Win+Shift+S, "panel snip"): rectangle, window and
 * full screen buttons at the top of the screen, like on Windows. A button runs
 * tilewin-snip, which selects, captures, copies and notifies.
 */
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <stdio.h>
#include "draw.h"
#include "flyout.h"
#include "popup.h"

#define BUTTON_W 76
#define BUTTON_H 56
#define CLOSE_W 44
#define GAP 4
#define PAD 6

enum snip_button {
	SNIP_AREA,
	SNIP_WINDOW,
	SNIP_SCREEN,
	SNIP_CLOSE,
	SNIP_COUNT,
};

static const char *const labels[] = { "Rectangle", "Window", "Full screen", "" };
static const char *const modes[] = { "area", "window", "screen", NULL };

struct snip {
	struct pbox buttons[SNIP_COUNT];
	int selected; // keyboard selection
	double px, py;
	bool inside;
};

static void draw_icon(cairo_t *cr, enum snip_button b, double cx, double cy, uint32_t color) {
	cairo_new_path(cr);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.5);
	switch (b) {
	case SNIP_AREA: {
		static const double dash[] = { 3, 2 };
		cairo_set_dash(cr, dash, 2, 0);
		cairo_rectangle(cr, cx - 9.5, cy - 7.5, 19, 15);
		cairo_stroke(cr);
		cairo_set_dash(cr, NULL, 0, 0);
		break;
	}
	case SNIP_WINDOW:
		cairo_rectangle(cr, cx - 9.5, cy - 7.5, 19, 15);
		cairo_stroke(cr);
		pd_rect(cr, cx - 9.5, cy - 7.5, 19, 4, color);
		break;
	case SNIP_SCREEN:
		cairo_rectangle(cr, cx - 10.5, cy - 8.5, 21, 14);
		cairo_stroke(cr);
		cairo_move_to(cr, cx, cy + 5.5);
		cairo_line_to(cr, cx, cy + 9);
		cairo_move_to(cr, cx - 5, cy + 9.5);
		cairo_line_to(cr, cx + 5, cy + 9.5);
		cairo_stroke(cr);
		break;
	case SNIP_CLOSE:
		cairo_move_to(cr, cx - 6, cy - 6);
		cairo_line_to(cr, cx + 6, cy + 6);
		cairo_move_to(cr, cx + 6, cy - 6);
		cairo_line_to(cr, cx - 6, cy + 6);
		cairo_stroke(cr);
		break;
	case SNIP_COUNT:
		break;
	}
}

static void snip_render(struct popup *p, cairo_t *cr) {
	struct snip *s = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	int M = popup_shadow_margin(p->panel);
	popup_draw_frame(p->panel, cr, p->surface->width, p->surface->height, M, "menu");
	int x = M + PAD, y = M + PAD;
	for (int i = 0; i < SNIP_COUNT; i++) {
		int w = i == SNIP_CLOSE ? CLOSE_W : BUTTON_W;
		if (i == SNIP_CLOSE) {
			pd_rect(cr, x, y + 10, 1, BUTTON_H - 20, st.line);
			x += GAP + 1;
		}
		struct pbox b = { x, y, w, BUTTON_H };
		s->buttons[i] = b;
		bool hover = s->inside && pbox_contains(&b, s->px, s->py);
		if (hover || i == s->selected) {
			fill_hover(cr, &st, b);
		}
		if (i == SNIP_CLOSE) {
			draw_icon(cr, i, b.x + w / 2.0, b.y + BUTTON_H / 2.0, st.fg);
		} else {
			draw_icon(cr, i, b.x + w / 2.0, b.y + 20, st.fg);
			pd_text(cr, st.font, labels[i], b.x, b.y + 34, w, 18, st.fg, PD_CENTER);
		}
		x += w + GAP;
	}
}

static void choose(struct popup *p, int index) {
	struct panel *panel = p->panel;
	popup_close_later(panel);
	if (index >= 0 && index < SNIP_CLOSE) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "exec tilewin-snip %s", modes[index]);
		bar_run_command(panel, cmd, NULL);
	}
}

static void snip_motion(struct popup *p, double x, double y) {
	struct snip *s = p->data;
	s->px = x;
	s->py = y;
	s->inside = true;
	s->selected = -1;
	popup_set_dirty(p);
}

static void snip_leave(struct popup *p) {
	struct snip *s = p->data;
	s->inside = false;
	popup_set_dirty(p);
}

static void snip_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct snip *s = p->data;
	if (!pressed || button != BTN_LEFT) {
		return;
	}
	for (int i = 0; i < SNIP_COUNT; i++) {
		if (pbox_contains(&s->buttons[i], x, y)) {
			choose(p, i);
			return;
		}
	}
}

static void snip_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct snip *s = p->data;
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Tab) {
		int step = sym == XKB_KEY_Left ? -1 : 1;
		s->selected = ((s->selected < 0 ? 0 : s->selected) + step + SNIP_COUNT) % SNIP_COUNT;
		popup_set_dirty(p);
	} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
		choose(p, s->selected < 0 ? SNIP_AREA : s->selected);
	} else if (utf8 && *utf8 >= '1' && *utf8 <= '3') {
		choose(p, *utf8 - '1');
	}
}

static void snip_destroy(struct popup *p) {
	free(p->data);
}

static const struct popup_vtable snip_vtable = {
	.render = snip_render,
	.motion = snip_motion,
	.leave = snip_leave,
	.button = snip_button,
	.key = snip_key,
	.destroy = snip_destroy,
};

void snip_toolbar_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_SNIP)) {
		popup_close_all(panel);
		return;
	}
	if (!output) {
		return;
	}
	struct snip *s = calloc(1, sizeof(*s));
	s->selected = SNIP_AREA;
	int M = popup_shadow_margin(panel);
	int width = 3 * BUTTON_W + CLOSE_W + 4 * GAP + 1 + 2 * PAD + 2 * M;
	int height = BUTTON_H + 2 * PAD + 2 * M;
	bool bottom = panel->config ? panel->config->layouts[panel->layout].bottom : true;
	int bar = output->bar ? output->bar->height : 0;
	int y = (bottom ? 0 : bar) + 16 - M;
	if (!popup_create(panel, POPUP_SNIP, NULL, output, (output->width - width) / 2, y, width,
			height, &snip_vtable, s)) {
		free(s);
	}
}
