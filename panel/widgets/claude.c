/*
 * The claude widget: the usage limits of a Claude Code account, the 5-hour and
 * the weekly window, on the taskbar and on the desktop.
 *
 *   widget claude {
 *       account ~/.claude                 # a Claude Code folder; the one with the
 *                                         # newest values by default
 *       show usage_5h reset_5h usage_7d   # what, in this order (tw_claude_items)
 *   }
 *
 * The values come from tilewin-claude-usage, which Claude Code runs as its status
 * line once the folder is connected: Claude Code itself shows them, tileWin never
 * touches a login of Claude's. A folder that is not connected says so, and a
 * click connects it. On the desktop "show" of its desktop_widgets entry counts
 * (more by default), and the style "list" gives each item a line of its own.
 *
 * What it shows:
 *   usage   the share of the window used, with a bar
 *   reset   the time until the window starts over
 *   left    the time until it is used up at the pace since it started, or that
 *           it lasts until it resets
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "draw.h"
#include "panel.h"
#include "tw_claude_usage.h"
#include "tw_theme.h"

#define RELOAD_MS 5000
#define MAX_ITEMS 12
#define BAR_W 34
#define GAP 12

struct claude_state {
	struct tw_claude_usage usage;
	bool have; // values written down at least once
	struct loop_timer *timer;
};

/* ---------- the values ---------- */

/* The folder to show: the one set, else the one Claude Code showed values for last. */
static char *chosen_dir(struct widget *w) {
	const char *set = widget_conf(w, "account", NULL);
	if (set && *set) {
		return tw_claude_normalize_dir(set);
	}
	list_t *all = tw_claude_usage_all();
	char *dir = all->length > 0 ?
		strdup(((struct tw_claude_usage *)all->items[0])->dir) : tw_claude_default_dir();
	tw_claude_usage_list_free(all);
	return dir;
}

static void reload(struct widget *w) {
	struct claude_state *c = w->data;
	tw_claude_usage_finish(&c->usage);
	char *dir = chosen_dir(w);
	c->have = dir && tw_claude_usage_read(dir, &c->usage);
	if (!c->usage.dir && dir) {
		c->usage.dir = strdup(dir);
	}
	free(dir);
}

static void tick(void *data) {
	struct widget *w = data;
	struct claude_state *c = w->data;
	c->timer = NULL;
	if (!w->active) {
		return;
	}
	reload(w);
	widget_set_dirty(w); // the times count down also without new values
	c->timer = loop_add_timer_lazy(w->panel->loop, RELOAD_MS, tick, w);
}

/* ---------- what to show ---------- */

static int show_list(struct widget *w, const struct tw_claude_item **items) {
	const char *show = NULL;
	char joined[512] = "";
	if (w->desk) {
		// the desktop has its own list, more by default, apart from the taskbar's
		struct twconf_node *node = twconf_child(w->desk, "show");
		for (int i = 0; node && i < node->argc; i++) {
			strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
			strncat(joined, node->argv[i], sizeof(joined) - strlen(joined) - 1);
		}
		show = *joined ? joined : TW_CLAUDE_SHOW_DESKTOP;
	} else {
		show = widget_conf(w, "show", TW_CLAUDE_SHOW_TASKBAR);
	}
	char *copy = strdup(show), *save = NULL;
	int count = 0;
	for (char *id = strtok_r(copy, " ,\t", &save); id && count < MAX_ITEMS;
			id = strtok_r(NULL, " ,\t", &save)) {
		const struct tw_claude_item *item = tw_claude_item_find(id);
		if (item) {
			items[count++] = item;
		}
	}
	free(copy);
	return count;
}

static const struct tw_claude_window *window_of(struct claude_state *c,
		const struct tw_claude_item *item, long *length) {
	*length = item->seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	return item->seven_day ? &c->usage.seven_day : &c->usage.five_hour;
}

/*
 * The value of an item as text, brief for the taskbar; the share for a bar,
 * -1 when it has none.
 */
static double item_value(struct claude_state *c, const struct tw_claude_item *item, bool brief,
		char *out, size_t size) {
	long length;
	const struct tw_claude_window *win = window_of(c, item, &length);
	time_t now = time(NULL);
	double used = c->have ? tw_claude_window_now(win, now) : -1;
	if (used < 0) {
		snprintf(out, size, "–");
		return item->kind == TW_CLAUDE_USAGE ? 0 : -1;
	}
	char span[48];
	switch (item->kind) {
	case TW_CLAUDE_USAGE:
		snprintf(out, size, "%.0f%%", used);
		return used;
	case TW_CLAUDE_RESET:
		if (win->resets_at <= now) {
			snprintf(out, size, brief ? "reset" : "has reset");
		} else {
			tw_claude_format_span(win->resets_at - now, brief, span, sizeof(span));
			snprintf(out, size, brief ? "↻ %s" : "in %s", span);
		}
		return -1;
	case TW_CLAUDE_LEFT: {
		bool known;
		long left = tw_claude_window_left(win, length, now, &known);
		if (left < 0) {
			snprintf(out, size, brief ? "lasts" : "until it resets");
		} else if (left == 0) {
			snprintf(out, size, brief ? "used up" : "used up");
		} else {
			tw_claude_format_span(left, brief, span, sizeof(span));
			snprintf(out, size, brief ? "≈%s left" : "about %s", span);
		}
		return -1;
	}
	}
	return -1;
}

static uint32_t level_color(struct widget *w, double used, uint32_t normal) {
	if (used >= widget_conf_int(w, "critical", 95)) {
		return widget_conf_color(w, "critical_fg", 0xe04040ff);
	}
	if (used >= widget_conf_int(w, "warning", 80)) {
		return widget_conf_color(w, "warning_fg", 0xf0a030ff);
	}
	return normal;
}

/* ---------- the taskbar ---------- */

static const char *not_connected_text = "Claude: click to connect";

/* The width of one item: its name, a bar for a usage, its value. */
static int item_width(struct widget *w, struct render_ctx *ctx, const struct tw_claude_item *item) {
	struct claude_state *c = w->data;
	const char *font = bar_font(ctx->panel);
	char value[64];
	double share = item_value(c, item, true, value, sizeof(value));
	int width = render_text_width(ctx, font, item->label) + 4 +
		render_text_width(ctx, font, value);
	if (share >= 0) {
		width += BAR_W + 4;
	}
	return width;
}

static bool show_connect(struct claude_state *c) {
	return !c->have && !c->usage.connected;
}

static int claude_measure(struct widget *w, struct render_ctx *ctx) {
	struct claude_state *c = w->data;
	if (show_connect(c)) {
		return render_text_width(ctx, bar_font(ctx->panel), not_connected_text) + 16;
	}
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	int width = 16;
	for (int i = 0; i < count; i++) {
		width += item_width(w, ctx, items[i]) + (i > 0 ? GAP : 0);
	}
	return count > 0 ? width : 0;
}

static void claude_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct claude_state *c = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	const char *font = bar_font(ctx->panel);
	uint32_t fg = widget_fg(ctx->panel, "claude");
	uint32_t dim = (fg & 0xffffff00) | 0xa0;
	uint32_t accent = tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x0078d4ff) | 0xff;
	double x = b.x + 8, end = b.x + b.width - 8;
	if (show_connect(c)) {
		pd_text(ctx->cairo, font, not_connected_text, x, b.y, end - x, b.height, dim, PD_LEFT);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
		return;
	}
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	for (int i = 0; i < count && x < end; i++) {
		char value[64];
		double share = item_value(c, items[i], true, value, sizeof(value));
		int lw = render_text_width(ctx, font, items[i]->label);
		pd_text(ctx->cairo, font, items[i]->label, x, b.y, lw, b.height, dim, PD_LEFT);
		x += lw + 4;
		if (share >= 0) {
			double by = b.y + b.height / 2.0 - 3;
			cairo_t *cr = ctx->cairo;
			cairo_rectangle(cr, x, by, BAR_W, 6);
			cairo_set_source_rgba(cr, (fg >> 24 & 0xff) / 255.0, (fg >> 16 & 0xff) / 255.0,
				(fg >> 8 & 0xff) / 255.0, 0.22);
			cairo_fill(cr);
			uint32_t color = level_color(w, share, accent);
			cairo_rectangle(cr, x, by, BAR_W * share / 100.0, 6);
			cairo_set_source_rgba(cr, (color >> 24 & 0xff) / 255.0, (color >> 16 & 0xff) / 255.0,
				(color >> 8 & 0xff) / 255.0, 1);
			cairo_fill(cr);
			x += BAR_W + 4;
		}
		int vw = render_text_width(ctx, font, value);
		uint32_t color = share >= 0 ? level_color(w, share, fg) : fg;
		pd_text(ctx->cairo, font, value, x, b.y, end - x < vw ? end - x : vw, b.height, color,
			PD_LEFT);
		x += vw + GAP;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- the desktop: a line for each item ---------- */

static bool claude_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	if (!ctx->style || strcmp(ctx->style, "list") != 0) {
		return false;
	}
	struct claude_state *c = w->data;
	cairo_t *cr = ctx->cairo;
	double sc = ctx->scale, pad = 12 * sc;
	char font[224], bold[224];
	snprintf(font, sizeof(font), "%s %dpx", pal->family, (int)(13 * sc + 0.5));
	snprintf(bold, sizeof(bold), "%s Bold %dpx", pal->family, (int)(13 * sc + 0.5));
	double y = pad;
	// the account at the top
	char title[160];
	snprintf(title, sizeof(title), "Claude Code%s%s", c->usage.account ? " · " : "",
		c->usage.account ? c->usage.account : "");
	pd_text(cr, bold, title, pad, y, ctx->width - 2 * pad, 18 * sc, pal->fg, PD_LEFT);
	y += 24 * sc;
	if (show_connect(c)) {
		pd_text(cr, font, "Not connected: click to let Claude Code show its limits here",
			pad, y, ctx->width - 2 * pad, 36 * sc, pal->dim, PD_LEFT);
		return true;
	}
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = show_list(w, items);
	double foot = c->have && c->usage.updated ? 18 * sc : 0;
	double row = count > 0 ? (ctx->height - y - pad - foot) / count : 0;
	for (int i = 0; i < count; i++) {
		char value[64];
		double share = item_value(c, items[i], false, value, sizeof(value));
		const char *name = items[i]->kind == TW_CLAUDE_USAGE ?
			(items[i]->seven_day ? "Weekly limit" : "5-hour limit") :
			items[i]->kind == TW_CLAUDE_RESET ?
			(items[i]->seven_day ? "Weekly limit resets" : "5-hour limit resets") :
			(items[i]->seven_day ? "Weekly limit lasts" : "5-hour limit lasts");
		double line = share >= 0 ? row * 0.62 : row;
		pd_text(cr, font, name, pad, y, ctx->width * 0.6, line, pal->dim, PD_LEFT);
		uint32_t color = share >= 0 ? level_color(w, share, pal->fg) : pal->fg;
		pd_text(cr, bold, value, pad, y, ctx->width - 2 * pad, line, color, PD_RIGHT);
		if (share >= 0) {
			double bh = row * 0.18 < 4 * sc ? 4 * sc : row * 0.18;
			double by = y + line;
			double bw = ctx->width - 2 * pad;
			cairo_rectangle(cr, pad, by, bw, bh);
			cairo_set_source_rgba(cr, (pal->track >> 24 & 0xff) / 255.0,
				(pal->track >> 16 & 0xff) / 255.0, (pal->track >> 8 & 0xff) / 255.0,
				(pal->track & 0xff) / 255.0);
			cairo_fill(cr);
			uint32_t fill = level_color(w, share, pal->accent);
			cairo_rectangle(cr, pad, by, bw * share / 100.0, bh);
			cairo_set_source_rgba(cr, (fill >> 24 & 0xff) / 255.0, (fill >> 16 & 0xff) / 255.0,
				(fill >> 8 & 0xff) / 255.0, 1);
			cairo_fill(cr);
		}
		y += row;
	}
	if (foot > 0) {
		char when[64];
		long ago = (long)(time(NULL) - c->usage.updated);
		if (ago < 90) {
			snprintf(when, sizeof(when), "As of just now");
		} else {
			char span[48];
			tw_claude_format_span(ago, false, span, sizeof(span));
			snprintf(when, sizeof(when), "As of %s ago", span);
		}
		char small[224];
		snprintf(small, sizeof(small), "%s %dpx", pal->family, (int)(11 * sc + 0.5));
		pd_text(cr, small, when, pad, ctx->height - pad - foot, ctx->width - 2 * pad, foot,
			pal->dim, PD_LEFT);
	}
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static void append_window(GString *t, struct claude_state *c, bool seven_day) {
	const struct tw_claude_window *win = seven_day ? &c->usage.seven_day : &c->usage.five_hour;
	long length = seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	time_t now = time(NULL);
	double used = tw_claude_window_now(win, now);
	if (used < 0) {
		return;
	}
	g_string_append_printf(t, "\n%s: %.0f%% used", seven_day ? "Weekly limit" : "5-hour limit",
		used);
	if (win->resets_at > now) {
		char span[48], when[48];
		struct tm tm;
		localtime_r(&win->resets_at, &tm);
		strftime(when, sizeof(when), win->resets_at - now < 24 * 3600 ? "%H:%M" : "%a %H:%M", &tm);
		tw_claude_format_span(win->resets_at - now, false, span, sizeof(span));
		g_string_append_printf(t, ", resets at %s (in %s)", when, span);
	}
	bool known;
	long left = tw_claude_window_left(win, length, now, &known);
	if (known && left > 0) {
		char span[48];
		tw_claude_format_span(left, false, span, sizeof(span));
		g_string_append_printf(t, "\n    at this pace used up in about %s", span);
	} else if (known && left < 0) {
		g_string_append(t, "\n    at this pace it lasts until it resets");
	}
}

static char *claude_tooltip(struct widget *w, struct hotspot *hs) {
	struct claude_state *c = w->data;
	GString *t = g_string_new("Claude Code");
	if (c->usage.account) {
		g_string_append_printf(t, " · %s", c->usage.account);
	}
	if (show_connect(c)) {
		g_string_append_printf(t, "\n%s does not show its usage limits to tileWin yet: "
			"click to connect it (tileWin goes before its status line).",
			c->usage.dir ? c->usage.dir : "This Claude Code");
		return g_string_free(t, FALSE);
	}
	if (!c->have) {
		g_string_append(t, "\nConnected: the limits come with the next answer of Claude Code.");
		return g_string_free(t, FALSE);
	}
	append_window(t, c, false);
	append_window(t, c, true);
	long ago = (long)(time(NULL) - c->usage.updated);
	char span[48];
	tw_claude_format_span(ago, false, span, sizeof(span));
	g_string_append_printf(t, "\nAs Claude Code showed them %s ago", span);
	return g_string_free(t, FALSE);
}

static bool claude_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct claude_state *c = w->data;
	if (button != BTN_LEFT || !show_connect(c) || !c->usage.dir) {
		return false;
	}
	char *quoted = g_shell_quote(c->usage.dir);
	char *cmd = g_strdup_printf("tilewin-claude-usage connect %s", quoted);
	proc_spawn(cmd);
	g_free(cmd);
	g_free(quoted);
	return true;
}

static bool claude_sample(struct widget *w, struct widget_sample *out) {
	struct claude_state *c = w->data;
	if (!c->have) {
		return false;
	}
	time_t now = time(NULL);
	double five = tw_claude_window_now(&c->usage.five_hour, now);
	double seven = tw_claude_window_now(&c->usage.seven_day, now);
	out->percent = (int)(five >= 0 ? five : seven >= 0 ? seven : 0);
	snprintf(out->value, sizeof(out->value), "%.0f%%", five >= 0 ? five : 0);
	if (seven >= 0) {
		snprintf(out->detail, sizeof(out->detail), "Week %.0f%%", seven);
	}
	out->warning = out->percent >= widget_conf_int(w, "warning", 80);
	out->critical = out->percent >= widget_conf_int(w, "critical", 95);
	return true;
}

/* ---------- life ---------- */

static void claude_set_active(struct widget *w, bool active) {
	struct claude_state *c = w->data;
	if (active && !c->timer) {
		tick(w);
	} else if (!active && c->timer) {
		loop_remove_timer(w->panel->loop, c->timer);
		c->timer = NULL;
	}
}

static void claude_init(struct widget *w) {
	w->data = calloc(1, sizeof(struct claude_state));
	reload(w);
}

static void claude_destroy(struct widget *w) {
	struct claude_state *c = w->data;
	if (c->timer) {
		loop_remove_timer(w->panel->loop, c->timer);
	}
	tw_claude_usage_finish(&c->usage);
	free(c);
}

const struct widget_impl widget_claude = {
	.type = "claude",
	.init = claude_init,
	.destroy = claude_destroy,
	.measure = claude_measure,
	.render = claude_render,
	.click = claude_click,
	.tooltip = claude_tooltip,
	.set_active = claude_set_active,
	.sample = claude_sample,
	.draw_gadget = claude_draw_gadget,
};
