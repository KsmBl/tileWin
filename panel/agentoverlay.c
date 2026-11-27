/* A common time axis for the configured accounts' quota histories. */
#include <math.h>
#include <stdio.h>
#include "agentoverlay.h"
#include "draw.h"

struct quota_axis {
	time_t start, now;
	long span;
	struct gadget_ctx *ctx;
	GPtrArray *hovered;
};

static double quota_x(struct pbox box, const struct quota_axis *axis, double at) {
	return box.x + box.width * (at - axis->start) / axis->span;
}

static void quota_hover_segment(const struct quota_axis *axis, struct pbox box,
		const struct agent_overlay_series *limit, double from, double before, double to, double after,
		bool forecast) {
	struct gadget_ctx *ctx = axis->ctx;
	if (!ctx->pointer || ctx->px < from || ctx->px > to || ctx->py < box.y ||
			ctx->py > box.y + box.height || to <= from) {
		return;
	}
	double y = before + (after - before) * (ctx->px - from) / (to - from);
	if ((forecast ? fabs(ctx->py - y) <= 7 : ctx->py >= y - 2) &&
			!g_ptr_array_find(axis->hovered, limit, NULL)) {
		g_ptr_array_add(axis->hovered, (gpointer)limit);
	}
}

struct quota_reset {
	const struct agent_overlay_series *limit;
	time_t at;
	bool weekly_recovery;
};

static void quota_segment(cairo_t *cr, struct pbox box, const struct quota_axis *axis,
		const struct agent_overlay_series *limit, time_t from, double before, time_t to, double after) {
	if (before == 0 && after == 0) {
		return;
	}
	double x1 = quota_x(box, axis, from), x2 = quota_x(box, axis, to);
	uint32_t color = limit->color;
	double bottom = box.y + box.height;
	double y1 = bottom - box.height * CLAMP(before, 0, 100) / 100;
	double y2 = bottom - box.height * CLAMP(after, 0, 100) / 100;
	quota_hover_segment(axis, box, limit, x1, y1, x2, y2, false);
	cairo_move_to(cr, x1, bottom);
	cairo_line_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_line_to(cr, x2, bottom);
	cairo_close_path(cr);
	pd_color(cr, (color & 0xffffff00) | 0x32);
	cairo_fill(cr);
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	pd_color(cr, color);
	cairo_stroke(cr);
}

static bool quota_trace(cairo_t *cr, struct pbox box, const struct agent_overlay_series *l,
		bool weekly, const struct quota_axis *axis) {
	time_t start = axis->start, now = axis->now, previous = 0;
	struct tw_claude_window prev = { 0 };
	bool visible = false;
	uint32_t color = l->color;
	for (int i = 0; i <= l->count; i++) {
		struct tw_claude_point p = i < l->count ? l->points[i] :
			(struct tw_claude_point){ l->usage->updated, l->usage->five_hour, l->usage->seven_day };
		struct tw_claude_window win = weekly ? p.seven_day : p.five_hour;
		if (!win.known || !p.at || p.at < previous || p.at > now) {
			continue;
		}
		if (prev.known) {
			bool same = tw_claude_same_window(prev.resets_at, win.resets_at);
			time_t end = prev.resets_at ? MIN(prev.resets_at, p.at) : p.at;
			if (end > MAX(previous, start)) {
				quota_segment(cr, box, axis, l, previous, prev.used, end,
					same ? win.used : prev.used);
				visible = true;
			}
		}
		prev = win;
		previous = p.at;
	}
	if (prev.known) {
		time_t end = prev.resets_at ? MIN(now, prev.resets_at) : now;
		if (end > MAX(previous, start)) {
			quota_segment(cr, box, axis, l, previous, prev.used, end, prev.used);
			visible = true;
		}
		if (previous >= start) {
			double x = quota_x(box, axis, previous);
			double y = box.y + box.height * (1 - CLAMP(prev.used, 0, 100) / 100);
			pd_rect(cr, CLAMP(x - 2, box.x, box.x + box.width - 4),
				CLAMP(y - 2, box.y, box.y + box.height - 4), 4, 4, color);
			visible = true;
		}
	}
	return visible;
}

static int reset_order(gconstpointer left, gconstpointer right) {
	const struct quota_reset *a = left, *b = right;
	return (a->at > b->at) - (a->at < b->at);
}

static void quota_forecast(cairo_t *cr, struct pbox box, const struct agent_overlay_series *l,
		const struct tw_claude_window *win, const struct quota_axis *axis, long window, bool weekly) {
	time_t now = axis->now;
	time_t since = win->resets_at - window;
	double base = 0, last = -1;
	for (int i = 0; i < l->count; i++) {
		const struct tw_claude_point *point = &l->points[i];
		const struct tw_claude_window *p = weekly ? &point->seven_day : &point->five_hour;
		if (!p->known || point->at < since || point->at > now ||
				!tw_claude_same_window(p->resets_at, win->resets_at)) {
			continue;
		}
		if (last >= 0 && p->used < last - TW_CLAUDE_RESET_DROP) {
			since = point->at;
			base = p->used;
		}
		last = p->used;
	}
	double used = CLAMP(win->used, 0, 100);
	double elapsed = difftime(now, since);
	double pace = elapsed >= 60 ? fmax(used - base, 0) / fmax(elapsed, weekly ? 3600 : 60) : 0;
	if (used >= 100 || pace <= 0) {
		return;
	}
	double seconds = fmin(difftime(win->resets_at, now), (100 - used) / pace);
	double end_usage = fmin(100, used + pace * seconds);
	double x = quota_x(box, axis, now), x2 = quota_x(box, axis, now + seconds);
	double y = box.y + box.height * (1 - used / 100);
	double y2 = box.y + box.height * (1 - end_usage / 100);
	quota_hover_segment(axis, box, l, x, y, x2, y2, true);
	cairo_save(cr);
	const double dash[] = { 5, 3 };
	cairo_set_dash(cr, dash, 2, 0);
	pd_color(cr, l->color);
	cairo_move_to(cr, x, y);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void quota_reset_markers(const struct quota_axis *axis, const struct gadget_palette *pal,
		struct pbox chart, const char *font, GArray *resets) {
	struct gadget_ctx *ctx = axis->ctx;
	cairo_t *cr = ctx->cairo;
	g_array_sort(resets, reset_order);
	double label_end = chart.x, label_right = chart.x + chart.width;
	for (guint i = 0; i < resets->len; i++) {
		const struct quota_reset *reset = &g_array_index(resets, struct quota_reset, i);
		double x = quota_x(chart, axis, reset->at);
		uint32_t color = reset->limit->color;
		pd_rect(cr, CLAMP(x - 1, chart.x, chart.x + chart.width - 2), chart.y, 2,
			chart.height, (color & 0xffffff00) | 0xc8);
		struct tm tm;
		localtime_r(&reset->at, &tm);
		char label[32];
		strftime(label, sizeof(label), "%H:%M", &tm);
		int width = 0;
		pd_text_size(cr, font, label, &width, NULL);
		double left = fmax(label_end, fmin(x - width / 2.0, label_right - width));
		if (left + width <= label_right && fabs(left + width / 2.0 - x) < width) {
			pd_text(cr, font, label, left, chart.y - 19, width + 1, 17, color, PD_LEFT);
			label_end = left + width + 5;
		}
	}
	if (!ctx->pointer || ctx->px < chart.x || ctx->px > chart.x + chart.width ||
			ctx->py < chart.y - 19 || ctx->py > chart.y + chart.height) {
		return;
	}
	const struct quota_reset *nearest = NULL;
	double distance = 6;
	for (guint i = 0; i < resets->len; i++) {
		const struct quota_reset *r = &g_array_index(resets, struct quota_reset, i);
		double d = fabs(ctx->px - quota_x(chart, axis, r->at));
		if (d < distance) {
			distance = d;
			nearest = r;
		}
	}
	if (!nearest && !axis->hovered->len) {
		return;
	}
	int y = chart.y + 3;
	cairo_save(cr);
	cairo_rectangle(cr, chart.x, chart.y, chart.width, chart.height);
	cairo_clip(cr);
	if (nearest) {
		for (guint i = 0; i < resets->len && y + 18 <= chart.y + chart.height; i++) {
			const struct quota_reset *r = &g_array_index(resets, struct quota_reset, i);
			if (!tw_claude_same_window(r->at, nearest->at)) {
				continue;
			}
			struct tm tm;
			localtime_r(&r->at, &tm);
			char when[48], tip[272];
			strftime(when, sizeof(when), "%a %d %b %H:%M", &tm);
			if (r->weekly_recovery) {
				snprintf(tip, sizeof(tip), "Weekly reset %s · %s", when, r->limit->name);
			} else {
				snprintf(tip, sizeof(tip), "%s · reset %s", r->limit->name, when);
			}
			pd_rect(cr, chart.x + 3, y, chart.width - 6, 18, pal->bg);
			pd_rect(cr, chart.x + 7, y + 4, 3, 10, r->limit->color);
			pd_text(cr, font, tip, chart.x + 14, y, chart.width - 20, 18, pal->fg, PD_LEFT);
			y += 18;
		}
	} else {
		for (guint i = 0; i < axis->hovered->len && y + 18 <= chart.y + chart.height; i++) {
			const struct agent_overlay_series *l = axis->hovered->pdata[i];
			pd_rect(cr, chart.x + 3, y, chart.width - 6, 18, pal->bg);
			pd_rect(cr, chart.x + 7, y + 4, 3, 10, l->color);
			pd_text(cr, font, l->name, chart.x + 14, y, chart.width - 20, 18, pal->fg, PD_LEFT);
			y += 18;
		}
	}
	cairo_restore(cr);
}

static void quota_grid(cairo_t *cr, const struct gadget_palette *pal, struct pbox chart,
		const struct quota_axis *axis, const char *font, bool weekly) {
	struct tm boundary;
	localtime_r(&axis->start, &boundary);
	boundary.tm_sec = boundary.tm_min = 0;
	boundary.tm_hour = weekly ? 0 : boundary.tm_hour / 4 * 4;
	for (int i = 0; i < 20; i++) {
		boundary.tm_isdst = -1;
		time_t start = mktime(&boundary);
		if (start >= axis->start + axis->span) {
			break;
		}
		struct tm label_time = boundary;
		boundary.tm_hour += weekly ? 24 : 4;
		boundary.tm_isdst = -1;
		time_t end = mktime(&boundary);
		double x = fmax(chart.x, quota_x(chart, axis, start));
		double right = fmin(chart.x + chart.width, quota_x(chart, axis, end));
		if (right <= x) {
			continue;
		}
		if (start >= axis->start) {
			pd_rect(cr, x, chart.y, 1, chart.height, pal->grid);
		}
		for (int tick = 0; tick < 3; tick++) {
			int y = chart.y + tick * (chart.height - 1) / 2;
			pd_rect(cr, x + 2, y, fmax(right - x - 4, 0), 1, pal->grid);
		}
		char label[24];
		strftime(label, sizeof(label), weekly ? "%a" : "%H:%M", &label_time);
		int width = 0;
		pd_text_size(cr, font, label, &width, NULL);
		if (weekly && right - x < width + 2 && g_utf8_validate(label, -1, NULL) &&
				g_utf8_strlen(label, -1) > 2) {
			*g_utf8_offset_to_pointer(label, 2) = '\0';
			pd_text_size(cr, font, label, &width, NULL);
		}
		if (right - x >= width + 2) {
			pd_text(cr, font, label, x, chart.y + chart.height + 2, right - x, 16, pal->dim,
				PD_CENTER);
		}
	}
}

static void quota_overlay(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const GArray *series, struct pbox b, const char *font, bool weekly, const char *title) {
	cairo_t *cr = ctx->cairo;
	const int axis = 48;
	char axis_font[192];
	snprintf(axis_font, sizeof(axis_font), "%s 9", pal->family);
	pd_text(cr, font, title, b.x, b.y, b.width, 20, pal->fg, PD_LEFT);
	struct pbox chart = { b.x + axis, b.y + 39, b.width - axis, b.height - 60 };
	if (chart.width < 12 || chart.height < 12) {
		return;
	}
	time_t now = time(NULL);
	long window = weekly ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	long history = weekly ? TW_CLAUDE_SEVEN_DAYS : 24 * 3600;
	struct quota_axis timeline = { now - history, now, history + window, ctx, g_ptr_array_new() };
	const struct claude_look surface = {
		.chart_bg = claude_desktop_chart_bg(pal),
		.chart_radius = pal->radius > 0 ? 4 : 0,
		.sunken = pal->look == GADGET_CLASSIC,
	};
	claude_chart_surface(cr, &surface, chart);
	quota_grid(cr, pal, chart, &timeline, axis_font, weekly);
	for (int tick = 0; tick < 3; tick++) {
		int y = chart.y + tick * (chart.height - 1) / 2;
		const char *label = tick == 0 ? "100%" : tick == 1 ? "50%" : "0%";
		pd_text(cr, font, label, b.x, y - 7, axis - 4, 14, pal->dim, PD_RIGHT);
	}
	double now_x = quota_x(chart, &timeline, now);
	pd_rect(cr, now_x, chart.y, 1, chart.height, pal->dim);
	int title_width = 0;
	pd_text_size(cr, font, title, &title_width, NULL);
	if (b.x + title_width + 22 < now_x) {
		pd_text(cr, font, "Now", now_x - 22, b.y, 44, 18, pal->dim, PD_CENTER);
	}
	cairo_save(cr);
	cairo_rectangle(cr, chart.x, chart.y, chart.width, chart.height);
	cairo_clip(cr);
	cairo_set_line_width(cr, 1.7);
	bool visible = false;
	GArray *resets = g_array_new(false, false, sizeof(struct quota_reset));
	for (guint i = 0; i < series->len; i++) {
		const struct agent_overlay_series *l = &g_array_index(series, struct agent_overlay_series, i);
		if (!l->usage) {
			continue; // an allowance or a configured account without a time window
		}
		const struct tw_claude_window *win = weekly ? &l->usage->seven_day : &l->usage->five_hour;
		visible |= quota_trace(cr, chart, l, weekly, &timeline);
		bool weekly_recovery = !weekly && l->weekly_gate &&
			tw_claude_window_now(l->weekly_gate, now) >= 100;
		const struct tw_claude_window *next = weekly_recovery ? l->weekly_gate : win;
		if (!next->known || next->resets_at <= now || next->resets_at > now + window) {
			continue;
		}
		struct quota_reset reset = { .limit = l, .at = next->resets_at,
			.weekly_recovery = weekly_recovery };
		g_array_append_val(resets, reset);
		if (!weekly_recovery) {
			quota_forecast(cr, chart, l, win, &timeline, window, weekly);
		}
		visible = true;
	}
	if (!visible) {
		pd_text(cr, font, "No quota samples in this period", chart.x, chart.y, chart.width,
			chart.height, pal->dim, PD_CENTER);
	}
	cairo_restore(cr);
	quota_reset_markers(&timeline, pal, chart, axis_font, resets);
	g_array_unref(resets);
	g_ptr_array_unref(timeline.hovered);
}

void agent_overlay_draw(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const GArray *series) {
	const int inset = 10;
	int width = ctx->width - 2 * inset;
	int columns = MAX(1, width / 110);
	int count = 0;
	for (guint i = 0; i < series->len; i++) {
		count += *g_array_index(series, struct agent_overlay_series, i).legend != '\0';
	}
	int rows = (count + columns - 1) / columns;
	int legend_height = rows * 20;
	int height = (ctx->height - legend_height - 24) / 2;
	char font[192];
	snprintf(font, sizeof(font), "%s %.1f", pal->family, CLAMP(ctx->height / 23.0, 8, 12));
	quota_overlay(ctx, pal, series, (struct pbox){ inset, 8, width, height }, font, false,
		"Five-hour quota");
	quota_overlay(ctx, pal, series, (struct pbox){ inset, 12 + height, width, height }, font, true,
		"Weekly quota");
	int cell = width / columns;
	int legend_y = ctx->height - legend_height - 4;
	int slot = 0;
	for (guint i = 0; i < series->len; i++) {
		const struct agent_overlay_series *l = &g_array_index(series, struct agent_overlay_series, i);
		if (!*l->legend) {
			continue;
		}
		int x = inset + (slot % columns) * cell;
		int y = legend_y + (slot / columns) * 20;
		slot++;
		pd_rect(ctx->cairo, x, y + 8, 8, 4, l->color);
		pd_text(ctx->cairo, font, l->legend, x + 12, y, cell - 14, 20, pal->fg, PD_LEFT);
	}
}
