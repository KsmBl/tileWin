#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "flyout.h"
#include "panel.h"
#include "popup.h"
#include "stringop.h"
#include "tw_desktop.h"

/*
 * Snap Layouts, as on Windows 11: resting on the maximize button of a window
 * (the compositor says so with the tilewin event "snap_layouts") shows small
 * pictures of the layouts it can go into. A click on a part of one snaps the
 * window there.
 */

#define TILE_W 64
#define TILE_H 44
#define GAP 10
#define PAD 12
#define LEAVE_MS 450 // the pointer may take this long from the button to the popup

struct slot {
	const char *name; // snap slot: left, right, topleft, ...
	double fx, fy;    // where the lines between the parts are
};

struct layout {
	int count;
	struct slot slots[4];
};

static const struct layout layouts[] = {
	{ 2, { { "left", 0.5, 0.5 }, { "right", 0.5, 0.5 } } },
	{ 2, { { "left", 0.66, 0.5 }, { "right", 0.66, 0.5 } } },
	{ 2, { { "left", 0.34, 0.5 }, { "right", 0.34, 0.5 } } },
	{ 4, { { "topleft", 0.5, 0.5 }, { "topright", 0.5, 0.5 }, { "bottomleft", 0.5, 0.5 },
		{ "bottomright", 0.5, 0.5 } } },
	{ 3, { { "left", 0.5, 0.5 }, { "topright", 0.5, 0.5 }, { "bottomright", 0.5, 0.5 } } },
};
#define LAYOUT_COUNT (int)(sizeof(layouts) / sizeof(layouts[0]))

struct snaplayouts {
	struct panel *panel;
	struct popup *popup;
	int64_t con_id;
	int hover_layout, hover_slot;
	bool inside, left_button;
	struct loop_timer *close_timer;
};

static struct snaplayouts *current;

/* The part of a w x h box a slot takes. */
static struct pbox slot_box(const struct slot *s, double x, double y, double w, double h) {
	double lx = w * s->fx, ly = h * s->fy;
	if (strcmp(s->name, "left") == 0) {
		return (struct pbox){ x, y, lx, h };
	} else if (strcmp(s->name, "right") == 0) {
		return (struct pbox){ x + lx, y, w - lx, h };
	} else if (strcmp(s->name, "topleft") == 0) {
		return (struct pbox){ x, y, lx, ly };
	} else if (strcmp(s->name, "topright") == 0) {
		return (struct pbox){ x + lx, y, w - lx, ly };
	} else if (strcmp(s->name, "bottomleft") == 0) {
		return (struct pbox){ x, y + ly, lx, h - ly };
	}
	return (struct pbox){ x + lx, y + ly, w - lx, h - ly };
}

static struct pbox tile_box(struct popup *p, int layout) {
	int M = popup_shadow_margin(p->panel);
	return (struct pbox){ M + PAD + layout * (TILE_W + GAP), M + PAD, TILE_W, TILE_H };
}

static void hit(struct popup *p, double x, double y, int *layout, int *slot) {
	*layout = *slot = -1;
	for (int l = 0; l < LAYOUT_COUNT; l++) {
		struct pbox t = tile_box(p, l);
		for (int s = 0; s < layouts[l].count; s++) {
			struct pbox b = slot_box(&layouts[l].slots[s], t.x, t.y, t.width, t.height);
			if (pbox_contains(&b, x, y)) {
				*layout = l;
				*slot = s;
				return;
			}
		}
	}
}

static void render(struct popup *p, cairo_t *cr) {
	struct snaplayouts *sl = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	fly_draw_frame(p, cr, &st);
	for (int l = 0; l < LAYOUT_COUNT; l++) {
		struct pbox t = tile_box(p, l);
		for (int s = 0; s < layouts[l].count; s++) {
			struct pbox b = slot_box(&layouts[l].slots[s], t.x, t.y, t.width, t.height);
			bool on = sl->hover_layout == l && sl->hover_slot == s;
			// two pixels between the parts, as on Windows
			double x = b.x + 1, y = b.y + 1, w = b.width - 2, h = b.height - 2;
			if (st.style == PS_CLASSIC) {
				pd_rect(cr, x, y, w, h, on ? st.accent : st.field_bg);
				pd_bevel(cr, x, y, w, h, !on);
			} else {
				cairo_new_path(cr);
				pd_rounded(cr, x, y, w, h, 3);
				pd_color(cr, on ? st.accent : st.button_bg);
				cairo_fill_preserve(cr);
				pd_color(cr, on ? st.accent : st.button_border);
				cairo_set_line_width(cr, 1);
				cairo_stroke(cr);
			}
		}
	}
}

static void stop_close_timer(struct snaplayouts *sl) {
	if (sl->close_timer) {
		loop_remove_timer(sl->panel->loop, sl->close_timer);
		sl->close_timer = NULL;
	}
}

static void close_now(void *data) {
	struct snaplayouts *sl = current;
	if (!sl) {
		return;
	}
	sl->close_timer = NULL;
	if (!sl->inside) {
		popup_close_later(sl->panel);
	}
}

static void close_soon(struct snaplayouts *sl) {
	stop_close_timer(sl);
	sl->close_timer = loop_add_timer(sl->panel->loop, LEAVE_MS, close_now, NULL);
}

static void motion(struct popup *p, double x, double y) {
	struct snaplayouts *sl = p->data;
	sl->inside = true;
	stop_close_timer(sl);
	int layout, slot;
	hit(p, x, y, &layout, &slot);
	if (layout != sl->hover_layout || slot != sl->hover_slot) {
		sl->hover_layout = layout;
		sl->hover_slot = slot;
		popup_set_dirty(p);
	}
}

static void leave(struct popup *p) {
	struct snaplayouts *sl = p->data;
	sl->inside = false;
	sl->hover_layout = sl->hover_slot = -1;
	popup_set_dirty(p);
	close_soon(sl);
}

/* Snaps that window into a slot of a layout. */
static void snap_into(struct panel *panel, int64_t con_id, int layout, int slot) {
	const struct slot *s = &layouts[layout].slots[slot];
	ipc_panel_commandf(panel, "[con_id=%lld] snap %s %.2f %.2f", (long long)con_id, s->name,
		s->fx, s->fy);
}

static void button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct snaplayouts *sl = p->data;
	if (button != BTN_LEFT || pressed) {
		return;
	}
	int layout, slot;
	hit(p, x, y, &layout, &slot);
	if (layout < 0) {
		return;
	}
	struct panel *panel = sl->panel;
	int64_t con_id = sl->con_id;
	popup_close_later(panel);
	snap_into(panel, con_id, layout, slot);
}

static void destroy(struct popup *p) {
	struct snaplayouts *sl = p->data;
	stop_close_timer(sl);
	if (current == sl) {
		current = NULL;
	}
	free(sl);
}

static const struct popup_vtable vtable = {
	.render = render,
	.motion = motion,
	.leave = leave,
	.button = button,
	.destroy = destroy,
};

void snaplayouts_open(struct panel *panel, struct panel_output *output, int64_t con_id,
		int x, int y, int button_width) {
	if (!output) {
		return;
	}
	if (current && current->con_id == con_id) {
		stop_close_timer(current);
		return;
	}
	popup_close_all(panel);
	struct snaplayouts *sl = calloc(1, sizeof(*sl));
	if (!sl) {
		return;
	}
	sl->panel = panel;
	sl->con_id = con_id;
	sl->hover_layout = sl->hover_slot = -1;
	int M = popup_shadow_margin(panel);
	int width = 2 * M + 2 * PAD + LAYOUT_COUNT * TILE_W + (LAYOUT_COUNT - 1) * GAP;
	int height = 2 * M + 2 * PAD + TILE_H;
	// under the button, its middle over the middle of the button, kept on the screen
	int px = x + button_width / 2 - width / 2;
	px = px + width > output->width ? output->width - width : px;
	px = px < 0 ? 0 : px;
	int py = y + 2 - M;
	sl->popup = popup_create(panel, POPUP_SNAPLAYOUTS, NULL, output, px, py, width, height,
		&vtable, sl);
	if (!sl->popup) {
		free(sl);
		return;
	}
	current = sl;
}

void snaplayouts_leave(struct panel *panel, int64_t con_id) {
	if (current && current->con_id == con_id && !current->inside) {
		close_soon(current);
	}
}

