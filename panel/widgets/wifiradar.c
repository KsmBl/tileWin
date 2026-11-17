/*
 * The Wi-Fi radar widget: the networks in reach as contacts on a sensor whose
 * sweep goes round (wifiradar.c), in the colors of every theme.
 *
 *   desktop_widgets {
 *       wifiradar { style radar; size 3x3 }
 *   }
 *   widget wifiradar {
 *       text count|name|none # on the taskbar: how many, the one connected to, the radar alone
 *       turn 6               # seconds for a round of the sweep
 *       color #3b82f6        # of the sweep; the accent of the theme by default
 *   }
 *
 * On the desktop the pointer on a contact shows its card (signal, security,
 * band) and a click connects to it; a click on the empty radar sends a ping
 * and looks again. On the taskbar a small radar with the strongest networks
 * where they are, and a click opens the network flyout.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"
#include "tw_theme.h"
#include "wifiradar.h"

#define FRAME_MS 70        // a frame of the sweep
#define FRAME_SAVER_MS 400 // with the battery saver on
#define FRAME_STILL_MS 1000 // Wi-Fi off, or none: nothing goes round

struct radar_widget {
	struct loop_timer *timer;
	bool held;
	// the radar as last drawn, for the pointer
	bool drawn;
	struct radar_geom geom;
	struct radar_look look;
};

static const char *place_conf(struct widget *w, const char *key, const char *fallback) {
	if (w->desk) {
		struct twconf_node *node = twconf_child(w->desk, key);
		if (node && node->argc > 0) {
			return node->argv[0];
		}
	}
	return widget_conf(w, key, fallback);
}

static bool radar_style(struct widget *w) {
	if (!w->desk) {
		return false;
	}
	// (without one it is the first, the taskbar look; the settings put "radar" in)
	struct twconf_node *node = twconf_child(w->desk, "style");
	return node && node->argc > 0 && strcmp(node->argv[0], "radar") == 0;
}

static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		double ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

static uint32_t with_alpha(uint32_t c, int a) {
	return (c & 0xffffff00) | (uint32_t)a;
}

static uint32_t conf_color(struct widget *w, const char *key, uint32_t fallback) {
	const char *v = place_conf(w, key, NULL);
	uint32_t c;
	return v && tw_parse_color(v, &c) ? c : fallback;
}

static double turn_of(struct widget *w) {
	const char *v = place_conf(w, "turn", NULL);
	double t = v ? atof(v) : 6;
	return t < 1 ? 1 : t > 60 ? 60 : t;
}

/* The colors of the radar, from those the desktop widgets have in this theme. */
static void look_of(struct widget *w, const struct gadget_ctx *ctx,
		const struct gadget_palette *pal, struct radar_look *look) {
	struct panel *panel = w->panel;
	memset(look, 0, sizeof(*look));
	snprintf(look->family, sizeof(look->family), "%s", pal->family);
	look->scale = ctx->scale;
	look->turn = turn_of(w);
	look->shadow = pal->shadow && !ctx->card;
	look->bad = pal->critical;
	if (panel_console(panel)) {
		// the Starship Console: its colors on a dark screen, the names in capitals
		uint32_t frame = panel_console_color(panel, 0);
		look->disc = 0x000000a8;
		look->ring = look->sweep = frame;
		look->fg = tw_theme_color(panel->theme, "panel.fg", 0xd3d6ffff);
		look->dim = panel_console_color(panel, 2);
		look->blip = panel_console_color(panel, 4);
		look->linked = panel_console_color(panel, 1);
		look->bad = 0xe0606aff;
		look->card_bg = 0x000000e8;
		look->card_fg = look->fg;
		look->card_border = frame;
		look->upper = true;
		look->bearings = ctx->width >= 300 * ctx->scale;
		look->shadow = false;
		return;
	}
	uint32_t accent = conf_color(w, "color", pal->accent);
	if (pal->mono) {
		// the Atari ST: the ink on the paper
		look->disc = 0;
		look->ring = look->sweep = look->fg = look->blip = look->linked = look->bad = pal->fg;
		look->dim = pal->fg;
		look->card_bg = with_alpha(pal->bg, 0xff);
		look->card_fg = look->card_border = pal->fg;
		look->mono = true;
		return;
	}
	bool dark = pal->dark || !ctx->card;
	look->ring = look->sweep = accent;
	look->fg = pal->fg;
	look->dim = pal->dim;
	look->blip = mix(pal->fg, accent, 0.3);
	look->linked = pal->look == GADGET_METRO ? pal->fg : dark ? 0x4ade80ff : 0x15803dff;
	look->disc = !ctx->card ? 0x00000078 : dark ? 0x00000048 : with_alpha(accent, 0x14);
	look->card_bg = dark ? 0x1c1c22f2 : 0xfffffff6;
	look->card_fg = dark ? 0xf2f2f2ff : 0x1b1b1bff;
	look->card_border = with_alpha(accent, 0xa0);
}

/* ---------- life ---------- */

static void frame_tick(void *data) {
	struct widget *w = data;
	struct radar_widget *s = w->data;
	s->timer = NULL;
	if (!w->active) {
		return;
	}
	// the next frame first: drawing may happen right away, and must find it there
	enum radar_status st = wifiradar_status();
	bool still = st != RADAR_READY && st != RADAR_LOOKING && !wifiradar_busy();
	s->timer = loop_add_timer_lazy(w->panel->loop, still ? FRAME_STILL_MS :
		w->panel->state.battery_saver && !wifiradar_busy() ? FRAME_SAVER_MS : FRAME_MS,
		frame_tick, w);
	widget_set_dirty(w);
}

static void radar_changed(void *data) {
	widget_set_dirty(data);
}

static void radar_set_active(struct widget *w, bool active) {
	struct radar_widget *s = w->data;
	if (active && !s->held) {
		s->held = true;
		wifiradar_hold(w->panel);
		wifiradar_listen(radar_changed, w);
	} else if (!active && s->held) {
		s->held = false;
		wifiradar_unlisten(radar_changed, w);
		wifiradar_release();
	}
	if (active && !s->timer && radar_style(w)) {
		frame_tick(w);
	} else if (!active && s->timer) {
		loop_remove_timer(w->panel->loop, s->timer);
		s->timer = NULL;
	}
}

static void radar_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct radar_widget));
}

static void radar_destroy(struct widget *w) {
	radar_set_active(w, false);
	free(w->data);
}

/* ---------- the taskbar ---------- */

static void taskbar_text(struct widget *w, char *out, size_t size) {
	const char *how = place_conf(w, "text", "count");
	out[0] = '\0';
	if (strcmp(how, "none") == 0) {
		return;
	}
	enum radar_status st = wifiradar_status();
	if (st == RADAR_OFF) {
		snprintf(out, size, "Off");
	} else if (st != RADAR_READY) {
		return;
	} else if (strcmp(how, "name") == 0 && wifiradar_linked()) {
		snprintf(out, size, "%s", wifiradar_linked());
	} else {
		snprintf(out, size, "%d", wifiradar_count());
	}
}

static int radar_measure(struct widget *w, struct render_ctx *ctx) {
	char text[128];
	taskbar_text(w, text, sizeof(text));
	int icon = (int)(ctx->height * 0.6);
	int width = 16 + icon;
	if (*text) {
		width += 6 + render_text_width(ctx, bar_font(ctx->panel), text);
	}
	return width;
}

static void radar_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	uint32_t fg = widget_fg(ctx->panel, "wifiradar");
	uint32_t accent = panel_atari(w->panel) ? fg :
		conf_color(w, "color", tw_theme_color(w->panel->theme, "flyout.accent", 0x0078d4ff));
	if (panel_console(w->panel)) {
		accent = panel_console_color(w->panel, 1);
	}
	double icon = floor(b.height * 0.6);
	double x = b.x + 8;
	wifiradar_draw_icon(ctx->cairo, x, b.y + (b.height - icon) / 2, icon, fg, accent,
		ctx->style == PSV_CLASSIC || panel_atari(w->panel));
	x += icon + 6;
	char text[128];
	taskbar_text(w, text, sizeof(text));
	if (*text) {
		const char *font = bar_font(ctx->panel);
		int tw = render_text_width(ctx, font, text);
		pd_text(ctx->cairo, font, text, x, b.y, tw, b.height, fg, PD_LEFT);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- the desktop ---------- */

static bool radar_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	struct radar_widget *s = w->data;
	if (!ctx->style || strcmp(ctx->style, "radar") != 0) {
		return false; // compact and tile: the taskbar look and the sample
	}
	if (!s->timer && w->active) {
		frame_tick(w); // the style was changed to this one
	}
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, pad = 10 * sc;
	struct radar_look look;
	look_of(w, ctx, pal, &look);
	char bold[224], small[224];
	snprintf(bold, sizeof(bold), "%s Bold %dpx", look.family, (int)lround(13 * sc));
	snprintf(small, sizeof(small), "%s %dpx", look.family, (int)lround(11 * sc));
	double head = 18 * sc;
	bool room = ctx->height > 160 * sc; // the heading and the count, when there is room
	// the heading: Wi-Fi, and the network connected to
	if (room) {
		const char *title = look.upper ? "WI-FI SENSOR" : "Wi-Fi";
		int tw = 0;
		pd_text_size(cr, bold, title, &tw, NULL);
		if (look.shadow) {
			pd_text(cr, bold, title, pad + 1, pad + 1, tw, head, 0x00000090, PD_LEFT);
		}
		pd_text(cr, bold, title, pad, pad, tw, head, look.fg, PD_LEFT);
		const char *linked = wifiradar_linked();
		const char *connecting = wifiradar_connecting();
		char right[160];
		uint32_t color = look.dim;
		if (connecting) {
			snprintf(right, sizeof(right), "Connecting to %s...", connecting);
			color = look.sweep;
		} else if (linked) {
			snprintf(right, sizeof(right), "%s", linked);
			color = look.linked;
		} else {
			right[0] = '\0';
		}
		if (look.upper) {
			for (char *c = right; *c; c++) {
				*c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
			}
		}
		double rx = pad + tw + 10 * sc, rw = ctx->width - pad - rx;
		if (*right && rw > 20 * sc) {
			if (look.shadow) {
				pd_text(cr, small, right, rx + 1, pad + 1, rw, head, 0x00000090, PD_RIGHT);
			}
			pd_text(cr, small, right, rx, pad, rw, head, color, PD_RIGHT);
		}
		char count[64];
		int n = wifiradar_count();
		enum radar_status st = wifiradar_status();
		if (st == RADAR_READY) {
			snprintf(count, sizeof(count), look.upper ? "%d CONTACTS" : n == 1 ?
				"%d network" : "%d networks", n);
		} else {
			count[0] = '\0';
		}
		double fy = ctx->height - pad - head;
		if (look.shadow) {
			pd_text(cr, small, count, pad + 1, fy + 1, ctx->width - 2 * pad, head, 0x00000090,
				PD_LEFT);
		}
		pd_text(cr, small, count, pad, fy, ctx->width - 2 * pad, head, look.dim, PD_LEFT);
	}
	double top = room ? pad + head : pad, bottom = room ? pad + head : pad;
	struct radar_geom g;
	wifiradar_geom(pad, top, ctx->width - 2 * pad, ctx->height - top - bottom, &look, &g);
	look.cards_x = 4 * sc;
	look.cards_y = 4 * sc;
	look.cards_w = ctx->width - 8 * sc;
	look.cards_h = ctx->height - 8 * sc;
	int hover = ctx->pointer ? wifiradar_hit(&g, &look, ctx->px, ctx->py) : -1;
	wifiradar_draw(cr, &g, &look, hover);
	s->geom = g;
	s->look = look;
	s->drawn = true;
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static char *radar_tooltip(struct widget *w, struct hotspot *hs) {
	if (radar_style(w)) {
		return NULL; // the radar names what is under the pointer itself
	}
	enum radar_status st = wifiradar_status();
	if (st == RADAR_OFF) {
		return strdup("Wi-Fi is off");
	}
	if (st == RADAR_NO_DEVICE) {
		return strdup("No Wi-Fi adapter");
	}
	int n = wifiradar_count();
	const char *linked = wifiradar_linked();
	return format_str("%d Wi-Fi network%s in reach%s%s", n, n == 1 ? "" : "s",
		linked ? "\nConnected to " : "", linked ? linked : "");
}

/* Where the flyout of a contact opens: by it, below or above. */
static struct popup_anchor anchor_at(struct psurface *s, double x, double y) {
	struct pbox placed;
	if (deskwidget_place(s, (struct pbox){ (int)x, (int)y, 1, 1 }, &placed)) {
		struct panel_output *output = s->output;
		bool above = output && placed.y > output->height / 2;
		return (struct popup_anchor){
			.output = output,
			.x = placed.x,
			.y = above ? placed.y - 16 : placed.y + 16,
			.above = above,
		};
	}
	return popup_anchor_for_bar(s, (int)x, 0);
}

static bool radar_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct radar_widget *st = w->data;
	if (button == BTN_RIGHT && radar_style(w) && st->drawn) {
		// a contact's own menu; elsewhere the widget's
		return wifiradar_menu(&st->geom, &st->look, x, y, anchor_at(s, x, y));
	}
	if (button != BTN_LEFT) {
		return false;
	}
	if (radar_style(w) && st->drawn) {
		if (!wifiradar_in_disc(&st->geom, x, y) &&
				wifiradar_hit(&st->geom, &st->look, x, y) < 0) {
			return false;
		}
		wifiradar_click(&st->geom, &st->look, x, y, anchor_at(s, x, y));
		widget_set_dirty(w);
		return true;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	flyout_network_toggle(w->panel, anchor, NULL);
	return true;
}

static bool radar_sample(struct widget *w, struct widget_sample *out) {
	out->percent = -1;
	enum radar_status st = wifiradar_status();
	if (st == RADAR_NO_DEVICE || st == RADAR_NO_NM) {
		return false;
	}
	if (st == RADAR_OFF) {
		snprintf(out->value, sizeof(out->value), "Off");
		snprintf(out->detail, sizeof(out->detail), "Wi-Fi is off");
		return true;
	}
	int n = wifiradar_count();
	snprintf(out->value, sizeof(out->value), "%d", n);
	const char *linked = wifiradar_linked();
	snprintf(out->detail, sizeof(out->detail), "%s%s", linked ? "Connected to " :
		"Not connected", linked ? linked : "");
	return true;
}

const struct widget_impl widget_wifiradar = {
	.type = "wifiradar",
	.init = radar_init,
	.destroy = radar_destroy,
	.measure = radar_measure,
	.render = radar_render,
	.click = radar_click,
	.tooltip = radar_tooltip,
	.set_active = radar_set_active,
	.sample = radar_sample,
	.draw_gadget = radar_draw_gadget,
};
