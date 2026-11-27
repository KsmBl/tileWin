/* A common time axis for the configured accounts' quota histories. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "agentoverlay.h"
#include "draw.h"

#define OVERLAY_GAP_S (15 * 60)
#define OVERLAY_GAP_RISE 3.0
#define OVERLAY_WEEK_FUTURE (2 * 86400)

struct quota_axis {
	time_t start, now;
	long span;
	struct gadget_ctx *ctx;
};

struct quota_reset {
	const struct agent_overlay_series *limit;
	time_t at;
	bool weekly_recovery;
};

struct quota_pill {
	struct pbox box;
	struct quota_reset reset;
};

struct quota_tip_row {
	uint32_t color;
	bool has_dot;
	char text[272];
};

static double quota_x(struct pbox box, const struct quota_axis *axis, double at) {
	return box.x + box.width * (at - axis->start) / axis->span;
}

static double quota_y(struct pbox box, double used) {
	return box.y + box.height * (1.0 - CLAMP(used, 0.0, 100.0) / 100.0);
}

static bool quota_is_gap(time_t from, double before, time_t to, double after) {
	return to - from > OVERLAY_GAP_S && after - before >= OVERLAY_GAP_RISE;
}

static void draw_now_dot(cairo_t *cr, double x, double y, uint32_t color) {
	cairo_new_path(cr);
	cairo_arc(cr, x, y, 3.5, 0, 2 * M_PI);
	pd_color(cr, color);
	cairo_fill_preserve(cr);
	pd_color(cr, 0xffffffff);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
}

static void quota_segment(cairo_t *cr, struct pbox box, const struct quota_axis *axis,
		const struct agent_overlay_series *limit, time_t from, double before, time_t to,
		double after, uint32_t fill_alpha) {
	if (before == 0 && after == 0) {
		return;
	}
	double x1 = quota_x(box, axis, from), x2 = quota_x(box, axis, to);
	uint32_t color = limit->color;
	double bottom = box.y + box.height;
	double y1 = quota_y(box, before);
	double y2 = quota_y(box, after);
	if (quota_is_gap(from, before, to, after)) {
		// Area known before the gap: flat at y1
		if (before > 0) {
			cairo_move_to(cr, x1, bottom);
			cairo_line_to(cr, x1, y1);
			cairo_line_to(cr, x2, y1);
			cairo_line_to(cr, x2, bottom);
			cairo_close_path(cr);
			pd_color(cr, (color & 0xffffff00) | fill_alpha);
			cairo_fill(cr);
		}
		// Hatched band between y2 (higher usage) and y1 (lower usage)
		cairo_save(cr);
		cairo_rectangle(cr, x1, y2, x2 - x1, y1 - y2);
		cairo_clip(cr);
		pd_color(cr, (color & 0xffffff00) | 0x14);
		cairo_paint(cr);
		cairo_new_path(cr);
		for (double d = -(y1 - y2); d < x2 - x1; d += 5) {
			cairo_move_to(cr, x1 + d, y1);
			cairo_line_to(cr, x1 + d + (y1 - y2), y2);
		}
		pd_color(cr, (color & 0xffffff00) | 0x50);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
		cairo_restore(cr);
		// Stepped dashed line across the gap
		cairo_save(cr);
		const double dash[] = { 3, 3 };
		cairo_set_dash(cr, dash, 2, 0);
		cairo_set_line_width(cr, 1.1);
		pd_color(cr, (color & 0xffffff00) | 0xa8);
		cairo_move_to(cr, x1, y1);
		cairo_line_to(cr, x2, y1);
		cairo_line_to(cr, x2, y2);
		cairo_stroke(cr);
		cairo_restore(cr);
		return;
	}
	cairo_move_to(cr, x1, bottom);
	cairo_line_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_line_to(cr, x2, bottom);
	cairo_close_path(cr);
	pd_color(cr, (color & 0xffffff00) | fill_alpha);
	cairo_fill(cr);
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	pd_color(cr, color);
	cairo_stroke(cr);
}

static bool quota_trace(cairo_t *cr, struct pbox box, const struct agent_overlay_series *l,
		bool weekly, const struct quota_axis *axis, uint32_t fill_alpha) {
	time_t start = axis->start, now = axis->now, previous = 0;
	struct tw_claude_window prev = { 0 };
	bool visible = false;
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
					same ? win.used : prev.used, fill_alpha);
				visible = true;
			}
		}
		prev = win;
		previous = p.at;
	}
	if (prev.known) {
		time_t end = prev.resets_at ? MIN(now, prev.resets_at) : now;
		if (end > MAX(previous, start)) {
			quota_segment(cr, box, axis, l, previous, prev.used, end, prev.used, fill_alpha);
			visible = true;
		}
	}
	return visible;
}

static double quota_pace(const struct agent_overlay_series *l, const struct tw_claude_window *win,
		time_t now, long window, bool weekly) {
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
	double used = CLAMP(win->used, 0.0, 100.0);
	double elapsed = difftime(now, since);
	return elapsed >= 60 ? fmax(used - base, 0.0) / fmax(elapsed, weekly ? 3600.0 : 60.0) : 0.0;
}

static void quota_forecast(cairo_t *cr, struct pbox box, const struct agent_overlay_series *l,
		const struct tw_claude_window *win, const struct quota_axis *axis, long window,
		bool weekly) {
	time_t now = axis->now;
	double used = CLAMP(win->used, 0.0, 100.0);
	double pace = quota_pace(l, win, now, window, weekly);
	if (used >= 100.0 || pace <= 0.0) {
		return;
	}
	double seconds = fmin(difftime(win->resets_at, now), (100.0 - used) / pace);
	double end_usage = fmin(100.0, used + pace * seconds);
	double x = quota_x(box, axis, now), x2 = quota_x(box, axis, now + seconds);
	double y = quota_y(box, used);
	double y2 = quota_y(box, end_usage);
	cairo_save(cr);
	const double dot[] = { 0.5, 4.5 };
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, 2.0);
	cairo_set_dash(cr, dot, 2, 0);
	pd_color(cr, l->color);
	cairo_move_to(cr, x, y);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* Evaluate a series at time t for the hover crosshair: recorded, gap range, or forecast. */
static bool quota_eval_at(const struct agent_overlay_series *l, bool weekly, time_t now,
		long window, time_t t, double *lo, double *hi, bool *forecast) {
	*forecast = false;
	if (!l->usage) {
		return false;
	}
	const struct tw_claude_window *cur = weekly ? &l->usage->seven_day : &l->usage->five_hour;
	if (t > now) {
		bool weekly_recovery = !weekly && l->weekly_gate &&
			tw_claude_window_now(l->weekly_gate, now) >= 100.0;
		if (weekly_recovery || !cur->known || cur->resets_at <= now || t > cur->resets_at) {
			return false;
		}
		double used = CLAMP(cur->used, 0.0, 100.0);
		double pace = quota_pace(l, cur, now, window, weekly);
		if (used >= 100.0 || pace <= 0.0) {
			return false;
		}
		double seconds = fmin(difftime(cur->resets_at, now), (100.0 - used) / pace);
		double dt = difftime(t, now);
		if (dt > seconds) {
			return false;
		}
		*lo = *hi = fmin(100.0, used + pace * dt);
		*forecast = true;
		return true;
	}
	time_t previous = 0;
	struct tw_claude_window prev = { 0 };
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
			if (t >= previous && t <= end && end > previous) {
				double after = same ? win.used : prev.used;
				if (quota_is_gap(previous, prev.used, end, after) && t < end) {
					*lo = prev.used;
					*hi = after;
					return true;
				}
				double share = (double)(t - previous) / (double)(end - previous);
				*lo = *hi = prev.used + (after - prev.used) * share;
				return true;
			}
		}
		prev = win;
		previous = p.at;
	}
	if (prev.known) {
		time_t end = prev.resets_at ? MIN(now, prev.resets_at) : now;
		if (t >= previous && t <= end) {
			*lo = *hi = prev.used;
			return true;
		}
	}
	return false;
}

static int reset_order(gconstpointer left, gconstpointer right) {
	const struct quota_reset *a = left, *b = right;
	return (a->at > b->at) - (a->at < b->at);
}

/* Alternating-dash vertical reset line so overlapping resets all show their colors. */
static void draw_reset_line(cairo_t *cr, struct pbox chart, double x, uint32_t color, int idx,
		int count) {
	double cx = CLAMP(round(x) + 0.5, chart.x + 1.0, chart.x + chart.width - 1.0);
	count = count < 1 ? 1 : count;
	double dash[] = { 4.0, count > 1 ? 4.0 * (count - 1) : 3.0 };
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_move_to(cr, cx, chart.y);
	cairo_line_to(cr, cx, chart.y + chart.height);
	cairo_set_dash(cr, dash, 2, count > 1 ? -4.0 * idx : 0.0);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* Right-edge flowing pill with colored arrow for weekly resets beyond the +2d horizon. */
static bool draw_later_pill(cairo_t *cr, const struct gadget_palette *pal, struct pbox chart,
		const char *font, const struct quota_reset *reset, int k, struct quota_pill *out) {
	struct tm tm;
	localtime_r(&reset->at, &tm);
	char when[16];
	strftime(when, sizeof(when), "%a", &tm);
	int tw = 0;
	pd_text_size(cr, font, when, &tw, NULL);
	double y = chart.y + 4 + 18.0 * k;
	if (y + 16 > chart.y + chart.height) {
		return false;
	}
	double pill_w = tw + 18;
	double right = chart.x + chart.width - 3;
	double px = right - pill_w;
	if (px < chart.x + 2) {
		return false;
	}
	cairo_new_path(cr);
	pd_rounded(cr, px, y, pill_w, 16, 3);
	pd_color(cr, (pal->bg & 0xffffff00) | 0xd8);
	cairo_fill(cr);
	pd_text(cr, font, when, px + 4, y, tw + 2, 16, pal->fg, PD_LEFT);
	cairo_new_path(cr);
	cairo_move_to(cr, right - 9, y + 3.5);
	cairo_line_to(cr, right - 2, y + 8.0);
	cairo_line_to(cr, right - 9, y + 12.5);
	cairo_close_path(cr);
	pd_color(cr, reset->limit->color);
	cairo_fill(cr);
	out->box = (struct pbox){ (int)px, (int)y, (int)ceil(pill_w), 16 };
	out->reset = *reset;
	return true;
}

static void draw_tooltip_card(cairo_t *cr, const struct gadget_palette *pal, struct pbox chart,
		const char *font, double anchor_x, double anchor_y, bool prefer_left,
		const struct quota_tip_row *rows, int count) {
	if (count <= 0) {
		return;
	}
	int max_w = 0, total_h = 0;
	int row_h[16];
	for (int i = 0; i < count && i < 16; i++) {
		int w = 0, h = 0;
		pd_text_size(cr, font, rows[i].text, &w, &h);
		if (h < 15) {
			h = 15;
		}
		row_h[i] = h;
		int full_w = w + (rows[i].has_dot ? 14 : 0);
		if (full_w > max_w) {
			max_w = full_w;
		}
		total_h += h;
	}
	int n = MIN(count, 16);
	double bw = fmin(max_w + 14.0, fmax(40.0, chart.width - 8.0));
	double bh = total_h + 8.0;
	double bx = prefer_left ? anchor_x - 8.0 - bw : anchor_x + 10.0;
	if (!prefer_left && bx + bw > chart.x + chart.width - 2.0) {
		bx = anchor_x - 10.0 - bw;
	} else if (prefer_left && bx < chart.x + 2.0) {
		bx = anchor_x + 8.0;
	}
	bx = CLAMP(bx, chart.x + 2.0, chart.x + chart.width - bw - 2.0);
	double by = CLAMP(anchor_y, chart.y + 3.0,
		fmax(chart.y + 3.0, chart.y + chart.height - bh - 3.0));
	cairo_save(cr);
	cairo_rectangle(cr, chart.x, chart.y, chart.width, chart.height);
	cairo_clip(cr);
	cairo_new_path(cr);
	pd_rounded(cr, bx, by, bw, bh, 4);
	pd_color(cr, (pal->bg & 0xffffff00) | 0xeb);
	cairo_fill_preserve(cr);
	pd_color(cr, (pal->fg & 0xffffff00) | 0x3c);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);
	double ty = by + 4.0;
	for (int i = 0; i < n; i++) {
		double tx = bx + 7.0;
		double tw = bw - 14.0;
		if (rows[i].has_dot) {
			cairo_new_path(cr);
			cairo_arc(cr, tx + 3.5, ty + row_h[i] / 2.0, 3.5, 0, 2 * M_PI);
			pd_color(cr, rows[i].color);
			cairo_fill(cr);
			tx += 12.0;
			tw -= 12.0;
		}
		pd_text(cr, font, rows[i].text, tx, ty, tw, row_h[i],
			rows[i].has_dot ? pal->fg : pal->dim, PD_LEFT);
		ty += row_h[i];
	}
	cairo_restore(cr);
}

static void quota_resets_and_hover(const struct quota_axis *axis, const struct gadget_palette *pal,
		struct pbox chart, const char *font, const GArray *series, bool weekly, long window,
		GArray *resets, GArray *later) {
	struct gadget_ctx *ctx = axis->ctx;
	cairo_t *cr = ctx->cairo;
	g_array_sort(resets, reset_order);
	g_array_sort(later, reset_order);
	struct tm now_tm;
	localtime_r(&axis->now, &now_tm);
	double label_end = chart.x, label_right = chart.x + chart.width;
	for (guint i = 0; i < resets->len; i++) {
		const struct quota_reset *reset = &g_array_index(resets, struct quota_reset, i);
		double x = quota_x(chart, axis, reset->at);
		double rx = round(x);
		int idx = 0, same = 0;
		for (guint k = 0; k < resets->len; k++) {
			const struct quota_reset *other = &g_array_index(resets, struct quota_reset, k);
			if (fabs(round(quota_x(chart, axis, other->at)) - rx) < 2.0) {
				idx += k < i;
				same++;
			}
		}
		draw_reset_line(cr, chart, x, reset->limit->color, idx, same);
		if (idx > 0) {
			continue; // shared timestamp already labeled
		}
		struct tm tm;
		localtime_r(&reset->at, &tm);
		char label[32];
		bool same_day = tm.tm_yday == now_tm.tm_yday && tm.tm_year == now_tm.tm_year;
		strftime(label, sizeof(label), (weekly && !same_day) ? "%a %H:%M" : "%H:%M", &tm);
		int width = 0;
		pd_text_size(cr, font, label, &width, NULL);
		double left = fmax(label_end, fmin(x - width / 2.0, label_right - width));
		if (left + width <= label_right && fabs(left + width / 2.0 - x) < width) {
			pd_text(cr, font, label, left, chart.y - 19, width + 1, 17, reset->limit->color,
				PD_LEFT);
			label_end = left + width + 5;
		}
	}
	// Right-edge pills for out-of-horizon resets on the 7d chart
	struct quota_pill pills[16];
	int pill_count = 0;
	for (guint i = 0; weekly && i < later->len && pill_count < 16; i++) {
		const struct quota_reset *reset = &g_array_index(later, struct quota_reset, i);
		if (draw_later_pill(cr, pal, chart, font, reset, pill_count, &pills[pill_count])) {
			pill_count++;
		}
	}
	if (!ctx->pointer || ctx->px < chart.x || ctx->px > chart.x + chart.width ||
			ctx->py < chart.y - 19 || ctx->py > chart.y + chart.height) {
		return;
	}
	// Hovering over a right-edge pill on the 7d chart: show its exact reset timestamp
	for (int i = 0; i < pill_count; i++) {
		if (pbox_contains(&pills[i].box, ctx->px, ctx->py)) {
			struct quota_tip_row rows[16];
			int n = 0;
			for (int k = 0; k < pill_count && n < 16; k++) {
				if (!tw_claude_same_window(pills[k].reset.at, pills[i].reset.at)) {
					continue;
				}
				struct tm tm;
				localtime_r(&pills[k].reset.at, &tm);
				char when[48];
				strftime(when, sizeof(when), "%a %d %b %H:%M", &tm);
				rows[n].color = pills[k].reset.limit->color;
				rows[n].has_dot = true;
				snprintf(rows[n].text, sizeof(rows[n].text), "%s · reset %s",
					pills[k].reset.limit->name, when);
				n++;
			}
			draw_tooltip_card(cr, pal, chart, font, pills[i].box.x, pills[i].box.y, true, rows, n);
			return;
		}
	}
	const struct quota_reset *nearest = NULL;
	double distance = 7.0;
	for (guint i = 0; i < resets->len; i++) {
		const struct quota_reset *r = &g_array_index(resets, struct quota_reset, i);
		double d = fabs(ctx->px - quota_x(chart, axis, r->at));
		if (d < distance) {
			distance = d;
			nearest = r;
		}
	}
	bool in_plot = ctx->py >= chart.y && ctx->py <= chart.y + chart.height;
	if (!in_plot && !nearest) {
		return;
	}
	// Crosshair + per-curve dots + floating tooltip
	double share = CLAMP((ctx->px - chart.x) / chart.width, 0.0, 1.0);
	time_t t = axis->start + (time_t)((double)axis->span * share);
	if (in_plot) {
		pd_rect(cr, floor(ctx->px), chart.y, 1, chart.height, (pal->dim & 0xffffff00) | 0xb0);
	}
	struct quota_tip_row rows[16];
	int n = 0;
	if (in_plot) {
		struct tm tm;
		localtime_r(&t, &tm);
		char when[32];
		strftime(when, sizeof(when), weekly ? "%a %H:%M" : "%H:%M", &tm);
		rows[n].color = 0;
		rows[n].has_dot = false;
		snprintf(rows[n].text, sizeof(rows[n].text), "%s%s", when, t > axis->now ? " (pace)" : "");
		n++;
	}
	if (nearest) {
		for (guint i = 0; i < resets->len && n < 16; i++) {
			const struct quota_reset *r = &g_array_index(resets, struct quota_reset, i);
			if (!tw_claude_same_window(r->at, nearest->at)) {
				continue;
			}
			struct tm tm;
			localtime_r(&r->at, &tm);
			char when[48];
			strftime(when, sizeof(when), "%a %d %b %H:%M", &tm);
			rows[n].color = r->limit->color;
			rows[n].has_dot = true;
			if (r->weekly_recovery) {
				snprintf(rows[n].text, sizeof(rows[n].text), "Weekly reset %s · %s", when,
					r->limit->name);
			} else {
				snprintf(rows[n].text, sizeof(rows[n].text), "%s · reset %s", r->limit->name, when);
			}
			n++;
		}
	}
	if (in_plot) {
		for (guint i = 0; i < series->len && n < 16; i++) {
			const struct agent_overlay_series *l =
				&g_array_index(series, struct agent_overlay_series, i);
			double lo = 0, hi = 0;
			bool forecast = false;
			if (!quota_eval_at(l, weekly, axis->now, window, t, &lo, &hi, &forecast)) {
				continue;
			}
			draw_now_dot(cr, ctx->px + 0.5, quota_y(chart, hi), l->color);
			rows[n].color = l->color;
			rows[n].has_dot = true;
			if (hi - lo >= 0.5) {
				snprintf(rows[n].text, sizeof(rows[n].text), "%s  %.0f–%.0f%%", l->name, lo, hi);
			} else if (forecast) {
				snprintf(rows[n].text, sizeof(rows[n].text), "%s  ~%.0f%%", l->name, hi);
			} else {
				snprintf(rows[n].text, sizeof(rows[n].text), "%s  %.0f%%", l->name, hi);
			}
			n++;
		}
	}
	if (n > 0) {
		draw_tooltip_card(cr, pal, chart, font, ctx->px, chart.y + 4, false, rows, n);
	}
}

static void quota_grid(cairo_t *cr, const struct gadget_palette *pal, struct pbox chart,
		const struct quota_axis *axis, const char *font, bool weekly) {
	// Horizontal 25%, 50%, 75% gridlines
	for (int tick = 1; tick < 4; tick++) {
		int y = chart.y + chart.height * tick / 4;
		pd_rect(cr, chart.x, y, chart.width, 1, tick == 2 ? pal->grid :
			(pal->grid & 0xffffff00) | ((pal->grid & 0xff) / 2));
	}
	struct tm boundary;
	localtime_r(&axis->start, &boundary);
	boundary.tm_sec = boundary.tm_min = 0;
	boundary.tm_hour = weekly ? 0 : boundary.tm_hour / 4 * 4;
	for (int i = 0; i < 24; i++) {
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
		if (start > axis->start) {
			pd_rect(cr, round(x), chart.y, 1, chart.height, pal->grid);
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
	pd_text(cr, font, "100%", chart.x + 4, chart.y + 1, 40, 14, pal->dim, PD_LEFT);
	pd_text(cr, font, "50%", chart.x + 4, chart.y + chart.height / 2 + 1, 40, 14, pal->dim, PD_LEFT);
}

static void quota_overlay(struct gadget_ctx *ctx, const struct gadget_palette *pal,
		const GArray *series, struct pbox b, const char *font, bool weekly, const char *title) {
	cairo_t *cr = ctx->cairo;
	char axis_font[192];
	snprintf(axis_font, sizeof(axis_font), "%s 9", pal->family);
	pd_text(cr, font, title, b.x, b.y, b.width, 20, pal->fg, PD_LEFT);
	struct pbox chart = { b.x, b.y + 39, b.width, b.height - 60 };
	if (chart.width < 12 || chart.height < 12) {
		return;
	}
	time_t now = time(NULL);
	long window = weekly ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	long history = weekly ? TW_CLAUDE_SEVEN_DAYS : 24 * 3600;
	long future = weekly ? OVERLAY_WEEK_FUTURE : TW_CLAUDE_FIVE_HOURS;
	struct quota_axis timeline = { now - history, now, history + future, ctx };
	const struct claude_look surface = {
		.chart_bg = claude_desktop_chart_bg(pal),
		.chart_radius = pal->radius > 0 ? 4 : 0,
		.sunken = pal->look == GADGET_CLASSIC,
	};
	claude_chart_surface(cr, &surface, chart);
	// Subtle shading behind Now to distinguish history from future forecast
	double now_x = quota_x(chart, &timeline, now);
	pd_rect(cr, chart.x, chart.y, now_x - chart.x, chart.height, (pal->accent & 0xffffff00) | 0x10);
	quota_grid(cr, pal, chart, &timeline, axis_font, weekly);
	int title_width = 0;
	pd_text_size(cr, font, title, &title_width, NULL);
	if (b.x + title_width + 22 < now_x) {
		pd_text(cr, font, "Now", now_x - 22, b.y, 44, 18, pal->dim, PD_CENTER);
	}
	int active_series = 0;
	for (guint i = 0; i < series->len; i++) {
		const struct agent_overlay_series *l = &g_array_index(series, struct agent_overlay_series, i);
		if (l->usage && (weekly ? l->usage->seven_day.known : l->usage->five_hour.known)) {
			active_series++;
		}
	}
	uint32_t fill_alpha = active_series > 1 ? 0x1c : 0x32;
	cairo_save(cr);
	cairo_rectangle(cr, chart.x, chart.y, chart.width, chart.height);
	cairo_clip(cr);
	cairo_set_line_width(cr, 1.7);
	bool visible = false;
	GArray *resets = g_array_new(false, false, sizeof(struct quota_reset));
	GArray *later = g_array_new(false, false, sizeof(struct quota_reset));
	struct {
		double y;
		uint32_t color;
	} now_dots[16];
	int now_dot_count = 0;
	for (guint i = 0; i < series->len; i++) {
		const struct agent_overlay_series *l = &g_array_index(series, struct agent_overlay_series, i);
		if (!l->usage) {
			continue; // an allowance or a configured account without a time window
		}
		const struct tw_claude_window *win = weekly ? &l->usage->seven_day : &l->usage->five_hour;
		bool traced = quota_trace(cr, chart, l, weekly, &timeline, fill_alpha);
		visible |= traced;
		if (win->known && now_dot_count < 16) {
			double cur_used = tw_claude_window_now(win, now);
			if (cur_used >= 0 && (win->resets_at > now || traced)) {
				now_dots[now_dot_count].y = quota_y(chart, cur_used);
				now_dots[now_dot_count].color = l->color;
				now_dot_count++;
				visible = true;
			}
		}
		bool weekly_recovery = !weekly && l->weekly_gate &&
			tw_claude_window_now(l->weekly_gate, now) >= 100.0;
		const struct tw_claude_window *next = weekly_recovery ? l->weekly_gate : win;
		if (!next->known || next->resets_at <= now) {
			continue;
		}
		if (!weekly_recovery) {
			quota_forecast(cr, chart, l, win, &timeline, window, weekly);
		}
		if (next->resets_at <= now + future) {
			struct quota_reset reset = { .limit = l, .at = next->resets_at,
				.weekly_recovery = weekly_recovery };
			g_array_append_val(resets, reset);
			visible = true;
		} else if (weekly) {
			struct quota_reset reset = { .limit = l, .at = next->resets_at,
				.weekly_recovery = false };
			g_array_append_val(later, reset);
			visible = true;
		}
	}
	if (!visible) {
		pd_text(cr, font, "No quota samples in this period", chart.x, chart.y, chart.width,
			chart.height, pal->dim, PD_CENTER);
	}
	cairo_restore(cr);
	// Now line and white-circled current dots on top of curves
	pd_rect(cr, round(now_x) - 0.5, chart.y, 1, chart.height, pal->fg);
	for (int i = 0; i < now_dot_count; i++) {
		draw_now_dot(cr, now_x, now_dots[i].y, now_dots[i].color);
	}
	quota_resets_and_hover(&timeline, pal, chart, axis_font, series, weekly, window, resets, later);
	g_array_unref(resets);
	g_array_unref(later);
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
