/*
 * The ports widget: the ports this computer listens on, and whether each can
 * only be reached from here (local), from a private network (LAN) or from
 * every network the computer is on (public). On the taskbar a socket in the
 * color of the widest of them (green: all local, amber: a LAN one, red: a
 * public one) and how many there are:
 *
 *   widget ports {
 *       text full|short|none  # "2 public · 9 local", "2", the socket alone
 *       hide_local yes|no     # nothing on the taskbar while all are local
 *       interval 5            # seconds between looks
 *   }
 *
 * On the desktop the style "list" draws a line for each port with its program
 * and how far it reaches. A click opens a flyout with all of them (infoflyouts.c).
 * It reads /proc/net (tw_ports.h); the programs only where they are shown.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "tw_ports.h"
#include "tw_theme.h"

struct ports_state {
	struct tw_ports ports;
	struct loop_timer *timer;
	bool programs; // the desktop list shows them, so they are looked up
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

static bool place_bool(struct widget *w, const char *key, bool fallback) {
	const char *v = place_conf(w, key, NULL);
	if (!v) {
		return fallback;
	}
	return strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

static int interval_ms(struct widget *w) {
	const char *v = place_conf(w, "interval", NULL);
	int s = v ? atoi(v) : 5;
	return (s < 1 ? 1 : s > 600 ? 600 : s) * 1000;
}

/* Something to tell apart from what was drawn: the ports and their scopes. */
static unsigned long fingerprint(const struct tw_ports *p) {
	unsigned long h = 5381;
	for (int i = 0; i < p->count; i++) {
		const struct tw_port *e = &p->items[i];
		h = h * 33 + e->port;
		h = h * 33 + e->scope * 4 + e->tcp * 2 + e->udp;
		for (const char *c = e->program; *c; c++) {
			h = h * 33 + (unsigned char)*c;
		}
	}
	return h * 33 + p->temporary;
}

static void reload(struct widget *w) {
	struct ports_state *s = w->data;
	tw_ports_read(&s->ports, s->programs);
}

static void tick(void *data) {
	struct widget *w = data;
	struct ports_state *s = w->data;
	s->timer = NULL;
	if (!w->active) {
		return;
	}
	unsigned long before = fingerprint(&s->ports);
	reload(w);
	if (fingerprint(&s->ports) != before) {
		widget_set_dirty(w);
	}
	s->timer = loop_add_timer_lazy(w->panel->loop, interval_ms(w), tick, w);
}

/* The widest scope there is. */
static enum tw_port_scope widest(const struct tw_ports *p) {
	return p->public_ ? TW_PORT_PUBLIC : p->lan ? TW_PORT_LAN : TW_PORT_LOCAL;
}

static uint32_t scope_color(struct widget *w, enum tw_port_scope scope, bool dark) {
	if (panel_atari(w->panel)) {
		return tw_theme_color(w->panel->theme, "menu.fg", 0x000000ff);
	}
	switch (scope) {
	case TW_PORT_PUBLIC:
		return widget_conf_color(w, "public_fg", dark ? 0xff7b72ff : 0xd83b3bff);
	case TW_PORT_LAN:
		return widget_conf_color(w, "lan_fg", dark ? 0xf2b25cff : 0xd9822bff);
	default:
		return widget_conf_color(w, "local_fg", dark ? 0x6ccf8aff : 0x2e9e4fff);
	}
}

/* Whether a color is light, so what is drawn on it is dark. */
static bool light(uint32_t c) {
	return ((c >> 24 & 0xff) * 299 + (c >> 16 & 0xff) * 587 + (c >> 8 & 0xff) * 114) / 1000 > 140;
}

/* ---------- the socket ---------- */

/*
 * A network socket in a size x size square: a rounded square with the opening
 * of a plug in it (wide, a tab below, the contacts along its top), its
 * opening left out so whatever is behind shows through.
 */
static void draw_socket(cairo_t *cr, double x, double y, double s, uint32_t color, bool square) {
	cairo_save(cr);
	cairo_new_path(cr);
	if (square) {
		cairo_rectangle(cr, x + s * 0.06, y + s * 0.06, s * 0.88, s * 0.88);
	} else {
		pd_rounded(cr, x + s * 0.06, y + s * 0.06, s * 0.88, s * 0.88, s * 0.18);
	}
	// the opening, as one path: the wide part with the tab below it
	double l = x + s * 0.24, r = x + s * 0.76, t = y + s * 0.26, m = y + s * 0.58;
	double tl = x + s * 0.37, tr = x + s * 0.63, b = y + s * 0.76;
	cairo_move_to(cr, l, t);
	cairo_line_to(cr, r, t);
	cairo_line_to(cr, r, m);
	cairo_line_to(cr, tr, m);
	cairo_line_to(cr, tr, b);
	cairo_line_to(cr, tl, b);
	cairo_line_to(cr, tl, m);
	cairo_line_to(cr, l, m);
	cairo_close_path(cr);
	// the contacts, filled again inside the opening
	double pw = s * 0.055, ph = s * 0.13;
	for (int i = 0; i < 4; i++) {
		double px = l + (r - l) * (i + 1) / 5.0 - pw / 2;
		cairo_rectangle(cr, px, t, pw, ph);
	}
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	pd_color(cr, color);
	cairo_fill(cr);
	cairo_restore(cr);
}

/* ---------- the taskbar ---------- */

static void taskbar_text(struct widget *w, char *out, size_t size) {
	struct ports_state *s = w->data;
	const struct tw_ports *p = &s->ports;
	const char *how = place_conf(w, "text", "full");
	if (strcmp(how, "none") == 0) {
		out[0] = '\0';
	} else if (strcmp(how, "short") == 0) {
		snprintf(out, size, "%d", p->public_ + p->lan);
	} else {
		size_t n = 0;
		out[0] = '\0';
		if (p->public_) {
			n += snprintf(out + n, size - n, "%d public · ", p->public_);
		}
		if (p->lan && n < size) {
			n += snprintf(out + n, size - n, "%d LAN · ", p->lan);
		}
		if (n < size) {
			snprintf(out + n, size - n, "%d local", p->local);
		}
	}
}

static bool hidden(struct widget *w) {
	struct ports_state *s = w->data;
	return place_bool(w, "hide_local", false) && s->ports.public_ + s->ports.lan == 0;
}

static int ports_measure(struct widget *w, struct render_ctx *ctx) {
	if (hidden(w)) {
		return 0;
	}
	char text[64];
	taskbar_text(w, text, sizeof(text));
	int icon = (int)(ctx->height * 0.55);
	int width = 16 + icon;
	if (*text) {
		width += 6 + render_text_width(ctx, bar_font(ctx->panel), text);
	}
	return width;
}

static void ports_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct ports_state *s = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	uint32_t fg = widget_fg(ctx->panel, "ports");
	enum tw_port_scope scope = widest(&s->ports);
	// the color of the scope, lighter on a dark taskbar (whose text is light)
	uint32_t color = scope_color(w, scope, light(fg));
	double icon = floor(b.height * 0.55);
	double x = b.x + 8;
	draw_socket(ctx->cairo, x, b.y + (b.height - icon) / 2, icon, color,
		ctx->style == PSV_CLASSIC);
	x += icon + 6;
	char text[64];
	taskbar_text(w, text, sizeof(text));
	if (*text) {
		const char *font = bar_font(ctx->panel);
		int tw = render_text_width(ctx, font, text);
		pd_text(ctx->cairo, font, text, x, b.y, tw, b.height, fg, PD_LEFT);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- the desktop ---------- */

/* A line for each port: a dot in its color, the port, the program, how far. */
static void desk_list(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	struct ports_state *s = w->data;
	const struct tw_ports *p = &s->ports;
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, pad = 12 * sc;
	const char *size = place_conf(w, "text_size", "13");
	double text = fmin(fmax(atoi(size), 8), 40) * sc;
	char font[224], bold[224], small[224];
	snprintf(font, sizeof(font), "%s %dpx", pal->family, (int)(text + 0.5));
	snprintf(bold, sizeof(bold), "%s Bold %dpx", pal->family, (int)(text + 0.5));
	snprintf(small, sizeof(small), "%s %dpx", pal->family, (int)(text * 0.85 + 0.5));
	double y = pad, head = text * 1.6;
	double icon = text * 1.3;
	uint32_t top = pal->mono ? pal->fg : scope_color(w, widest(p), pal->dark);
	draw_socket(cr, pad, y + (head - icon) / 2, icon, top, pal->look == GADGET_CLASSIC);
	char title[64];
	if (p->public_ + p->lan) {
		snprintf(title, sizeof(title), "%d open to the network", p->public_ + p->lan);
	} else {
		snprintf(title, sizeof(title), "Nothing open to the network");
	}
	pd_text(cr, bold, title, pad + icon + 6 * sc, y, ctx->width - 2 * pad - icon, head, pal->fg,
		PD_LEFT);
	y += head + 2 * sc;
	double row = text * 1.45;
	int room = (int)((ctx->height - pad - y) / row);
	if (room < 1) {
		return;
	}
	bool more = p->count > room;
	int shown = more ? room - 1 : p->count;
	double col_port = text * 3.6;
	for (int i = 0; i < shown; i++) {
		const struct tw_port *e = &p->items[i];
		uint32_t c = pal->mono ? pal->fg : scope_color(w, e->scope, pal->dark);
		double d = text * 0.45;
		cairo_new_path(cr);
		cairo_arc(cr, pad + d / 2, y + row / 2, d / 2, 0, 2 * M_PI);
		pd_color(cr, c);
		if (pal->mono && e->scope == TW_PORT_LOCAL) {
			cairo_set_line_width(cr, fmax(sc, 1));
			cairo_stroke(cr); // an empty ring for local, filled for the network
		} else {
			cairo_fill(cr);
		}
		char port[16], name[64];
		snprintf(port, sizeof(port), "%u", e->port);
		double x = pad + d + 6 * sc;
		pd_text(cr, bold, port, x, y, col_port, row, pal->fg, PD_LEFT);
		const char *scope = tw_port_scope_name(e->scope);
		int sw = 0;
		pd_text_size(cr, small, scope, &sw, NULL);
		pd_text(cr, small, scope, ctx->width - pad - sw, y, sw, row, c, PD_LEFT);
		pd_text(cr, font, tw_port_name(e, name, sizeof(name)), x + col_port, y,
			ctx->width - pad - sw - 8 * sc - x - col_port, row, pal->dim, PD_LEFT);
		y += row;
	}
	if (more) {
		char line[64];
		snprintf(line, sizeof(line), "and %d more", p->count - shown);
		pd_text(cr, small, line, pad, y, ctx->width - 2 * pad, row, pal->dim, PD_LEFT);
	}
}

static bool ports_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	struct ports_state *s = w->data;
	const char *style = ctx->style ? ctx->style : "";
	if (strcmp(style, "list") != 0) {
		return false; // compact and tile: the taskbar look and the sample
	}
	if (!s->programs) {
		s->programs = true;
		reload(w);
	}
	desk_list(w, ctx, pal);
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static char *ports_tooltip(struct widget *w, struct hotspot *hs) {
	struct ports_state *s = w->data;
	reload(w);
	const struct tw_ports *p = &s->ports;
	GString *t = g_string_new(NULL);
	g_string_append_printf(t, "%d port%s open", p->count, p->count == 1 ? "" : "s");
	if (p->public_) {
		g_string_append_printf(t, "\n%d public: every network this computer is on", p->public_);
	}
	if (p->lan) {
		g_string_append_printf(t, "\n%d on a local network (LAN)", p->lan);
	}
	g_string_append_printf(t, "\n%d only for this computer", p->local);
	return g_string_free(t, FALSE);
}

static bool ports_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
	return true;
}

static bool ports_sample(struct widget *w, struct widget_sample *out) {
	struct ports_state *s = w->data;
	const struct tw_ports *p = &s->ports;
	out->percent = -1;
	int open = p->public_ + p->lan;
	if (open) {
		snprintf(out->value, sizeof(out->value), "%d open", open);
	} else {
		snprintf(out->value, sizeof(out->value), "%d local", p->local);
	}
	snprintf(out->detail, sizeof(out->detail), "%d public · %d LAN · %d local", p->public_,
		p->lan, p->local);
	out->warning = p->lan > 0 && p->public_ == 0;
	out->critical = p->public_ > 0;
	return true;
}

/* What the widget shows, for its flyout: read again now, with the programs. */
const struct tw_ports *ports_widget_state(struct widget *w) {
	struct ports_state *s = w && w->impl == &widget_ports ? w->data : NULL;
	if (!s) {
		return NULL;
	}
	tw_ports_read(&s->ports, true);
	return &s->ports;
}

/* ---------- life ---------- */

static void ports_set_active(struct widget *w, bool active) {
	struct ports_state *s = w->data;
	if (active && !s->timer) {
		tick(w);
	} else if (!active && s->timer) {
		loop_remove_timer(w->panel->loop, s->timer);
		s->timer = NULL;
	}
}

static void ports_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct ports_state));
	reload(w);
}

static void ports_destroy(struct widget *w) {
	struct ports_state *s = w->data;
	if (s->timer) {
		loop_remove_timer(w->panel->loop, s->timer);
	}
	tw_ports_free(&s->ports);
	free(s);
}

const struct widget_impl widget_ports = {
	.type = "ports",
	.init = ports_init,
	.destroy = ports_destroy,
	.measure = ports_measure,
	.render = ports_render,
	.click = ports_click,
	.tooltip = ports_tooltip,
	.set_active = ports_set_active,
	.sample = ports_sample,
	.draw_gadget = ports_draw_gadget,
};
