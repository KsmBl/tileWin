#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>
#include "agentusage.h"
#include "draw.h"

#define POLL_MS 1000
#define SAMPLE_SECONDS 10

struct agents_state {
	struct agent_usage *usage;
	GAsyncQueue *pending;
	struct loop_timer *timer;
	time_t sampled;
};

struct agents_request {
	GAsyncQueue *queue;
	bool poll_codex;
};

static gpointer collect_thread(gpointer data) {
	struct agents_request *request = data;
	GAsyncQueue *queue = request->queue;
	g_async_queue_push(queue, agent_usage_collect(request->poll_codex));
	g_free(request);
	g_async_queue_unref(queue);
	return NULL;
}

static void agents_tick(void *data) {
	struct widget *w = data;
	struct agents_state *s = w->data;
	s->timer = NULL;
	if (!w->active) {
		return;
	}
	if (s->pending) {
		struct agent_usage *usage = g_async_queue_try_pop(s->pending);
		if (usage) {
			agent_usage_free(s->usage);
			s->usage = usage;
			g_async_queue_unref(s->pending);
			s->pending = NULL;
			widget_set_dirty(w);
		}
	}
	time_t now = time(NULL);
	if (!s->pending && (now - s->sampled >= SAMPLE_SECONDS || now < s->sampled)) {
		s->pending = g_async_queue_new_full((GDestroyNotify)agent_usage_free);
		struct agents_request *request = g_new(struct agents_request, 1);
		request->queue = g_async_queue_ref(s->pending);
		request->poll_codex = widget_conf_bool(w, "codex_poll", true);
		GThread *thread = g_thread_try_new("agent-usage", collect_thread, request, NULL);
		if (thread) {
			g_thread_unref(thread);
		} else {
			g_async_queue_unref(request->queue);
			g_free(request);
			g_clear_pointer(&s->pending, g_async_queue_unref);
		}
		s->sampled = now;
	}
	s->timer = loop_add_timer_lazy(w->panel->loop, POLL_MS, agents_tick, w);
}

static void agents_active(struct widget *w, bool active) {
	struct agents_state *s = w->data;
	if (active && !s->timer) {
		agents_tick(w);
	} else if (!active && s->timer) {
		loop_remove_timer(w->panel->loop, s->timer);
		s->timer = NULL;
	}
}

static void agents_init(struct widget *w) {
	w->data = g_new0(struct agents_state, 1);
}

static void agents_destroy(struct widget *w) {
	struct agents_state *s = w->data;
	agents_active(w, false);
	g_clear_pointer(&s->pending, g_async_queue_unref);
	agent_usage_free(s->usage);
	g_free(s);
}

const struct agent_usage *agent_widget_usage(struct widget *w) {
	struct agents_state *s = w->data;
	return s->usage;
}

static void agent_summary(const struct agent_usage *u, enum agent_kind kind, char *out, size_t size,
		double quota_used[2]) {
	unsigned local = 0;
	double used = -1, week = -1;
	bool remote = false;
	if (u) {
		time_t now = time(NULL);
		for (guint i = 0; i < u->workers->len; i++) {
			struct agent_worker *worker = u->workers->pdata[i];
			local += worker->kind == kind;
		}
		for (guint i = 0; i < u->limits->len; i++) {
			struct agent_limit *l = u->limits->pdata[i];
			if (l->kind == kind) {
				if (!l->live_account) {
					used = MAX(used, tw_claude_window_now(&l->usage.five_hour, now));
					week = MAX(week, tw_claude_window_now(&l->usage.seven_day, now));
				}
				remote |= l->unattributed_at && now - l->unattributed_at < 5 * 3600;
			}
		}
	}
	bool billing = kind == AGENT_COPILOT && u && u->copilot_billing.known;
	if (billing) {
		used = u->copilot_billing.used;
	}
	if (quota_used) {
		quota_used[0] = used;
		quota_used[1] = week;
	}
	char quota[32] = "–";
	if (used >= 0 && week >= 0) {
		snprintf(quota, sizeof(quota), "%.0f%%/%.0f%%", used, week);
	} else if (used >= 0 || week >= 0) {
		snprintf(quota, sizeof(quota), "%.0f%%%s", used >= 0 ? used : week,
			billing ? " billing" : used >= 0 ? "" : " wk");
	}
	snprintf(out, size, "%s %s · %u%s", agent_names[kind], quota, local, remote ? " + ?" : "");
}

static void agents_bars(cairo_t *cr, double x, double y, double width, double height,
		const double quota[2], uint32_t track, uint32_t accent) {
	for (int i = 0; i < 2; i++) {
		if (quota[i] >= 0) {
			pd_share_bar(cr, x, y + (height - 9) / 2 + 5 * i, width, 4, quota[i],
				track, 0.28, quota[i] >= 95 ? 0xe05050ff : quota[i] >= 80 ? 0xd88a28ff : accent,
				1, PD_BAR_SMOOTH, false);
		}
	}
}

static int agents_measure(struct widget *w, struct render_ctx *ctx) {
	int width = 12;
	for (enum agent_kind i = 0; i < AGENT_COUNT; i++) {
		char line[72];
		double quota[2];
		agent_summary(agent_widget_usage(w), i, line, sizeof(line), quota);
		width += render_text_width(ctx, bar_font(ctx->panel), line) + 13 +
			(quota[0] >= 0 || quota[1] >= 0 ? 28 : 0);
	}
	return width;
}

static void agents_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	int x = b.x + 6;
	for (enum agent_kind i = 0; i < AGENT_COUNT; i++) {
		char line[72];
		double quota[2];
		agent_summary(agent_widget_usage(w), i, line, sizeof(line), quota);
		uint32_t fg = widget_fg(ctx->panel, "agents");
		if (quota[0] >= 0 || quota[1] >= 0) {
			agents_bars(ctx->cairo, x, b.y, 23, b.height, quota, fg, 0x3395dcff);
			x += 28;
		}
		const char *font = bar_font(ctx->panel);
		int width = render_text_width(ctx, font, line);
		pd_text(ctx->cairo, font, line, x, b.y, width + 1, b.height,
			widget_fg(ctx->panel, "agents"), PD_LEFT);
		x += width + 13;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static const uint32_t agent_chart_colors[] = {
	0xe78959ff, 0x34b58dff, 0xbd83e8ff, 0x5c9ef0ff,
};

static uint32_t quota_color(const struct agent_limit *limit) {
	if (limit->kind == AGENT_AGY) {
		if (g_str_has_prefix(limit->name, "3p-")) {
			return 0x45c6e8ff;
		}
		if (g_str_has_prefix(limit->name, "gemini-argon-")) {
			return 0x888cf6ff;
		}
	}
	return agent_chart_colors[limit->kind];
}

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
		const struct agent_limit *limit, double from, double before, double to, double after,
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
	const struct agent_limit *limit;
	time_t at;
	bool weekly_recovery, beyond_plot;
	int edge_offset;
};

static void quota_add_reset(GArray *resets, const struct quota_axis *axis,
		const struct agent_limit *limit, time_t when, bool weekly_recovery) {
	bool beyond_plot = when > axis->start + axis->span;
	if (when < axis->start || (beyond_plot && !weekly_recovery)) {
		return;
	}
	for (guint i = 0; i < resets->len; i++) {
		struct quota_reset r = g_array_index(resets, struct quota_reset, i);
		if (r.limit == limit && tw_claude_same_window(r.at, when)) {
			return;
		}
	}
	struct quota_reset reset = { .limit = limit, .at = when,
		.weekly_recovery = weekly_recovery, .beyond_plot = beyond_plot };
	g_array_append_val(resets, reset);
}

static void quota_segment(cairo_t *cr, struct pbox box, const struct quota_axis *axis,
		const struct agent_limit *limit, time_t from, double before, time_t to, double after) {
	if (before == 0 && after == 0) {
		return;
	}
	double x1 = quota_x(box, axis, from), x2 = quota_x(box, axis, to);
	uint32_t color = quota_color(limit);
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

static bool quota_trace(cairo_t *cr, struct pbox box, const struct agent_limit *l,
		bool weekly, const struct quota_axis *axis, GArray *resets) {
	time_t start = axis->start, now = axis->now, previous = 0;
	struct tw_claude_window prev = { 0 };
	bool visible = false;
	uint32_t color = quota_color(l);
	for (int i = 0; i <= l->chart.count; i++) {
		struct tw_claude_point p = i < l->chart.count ? l->chart.points[i] :
			(struct tw_claude_point){ l->usage.updated, l->usage.five_hour, l->usage.seven_day };
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
		if (win.resets_at > p.at && win.resets_at <= now) {
			quota_add_reset(resets, axis, l, win.resets_at, false);
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

static void quota_forecast(cairo_t *cr, struct pbox box, const struct agent_limit *l,
		const struct tw_claude_window *win, const struct quota_axis *axis, long window, bool weekly) {
	time_t now = axis->now;
	time_t since = win->resets_at - window;
	double base = 0, last = -1;
	for (int i = 0; i < l->chart.count; i++) {
		const struct tw_claude_point *point = &l->chart.points[i];
		const struct tw_claude_window *p = weekly ? &point->seven_day : &point->five_hour;
		if (!p->known || point->at < since || point->at > now ||
				!tw_claude_same_window(p->resets_at, win->resets_at)) {
			continue;
		}
		if (last >= 0 && p->used < last - 5) {
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
	pd_color(cr, quota_color(l));
	cairo_move_to(cr, x, y);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static const char *quota_name(const struct agent_limit *limit) {
	if (limit->kind == AGENT_AGY) {
		if (g_str_has_prefix(limit->name, "3p-")) {
			return "Antigravity · Claude and GPT models";
		}
		if (g_str_has_prefix(limit->name, "gemini-argon-")) {
			return "Antigravity · Gemini Argon";
		}
		if (g_str_has_prefix(limit->name, "gemini-")) {
			return "Antigravity · Gemini models";
		}
		return limit->name;
	}
	return limit->live_account ? limit->name : limit->kind == AGENT_CLAUDE ?
		"Claude · shared model quota" : limit->kind == AGENT_CODEX ?
		"Codex · shared model quota" : limit->name;
}

static double quota_reset_x(struct pbox chart, const struct quota_axis *axis,
		const struct quota_reset *reset) {
	return reset->beyond_plot ? chart.x + chart.width - reset->edge_offset :
		quota_x(chart, axis, reset->at);
}

static void quota_reset_markers(const struct quota_axis *axis, const struct gadget_palette *pal,
		struct pbox chart, const char *font, GArray *resets) {
	struct gadget_ctx *ctx = axis->ctx;
	cairo_t *cr = ctx->cairo;
	g_array_sort(resets, reset_order);
	double label_end = chart.x, label_right = chart.x + chart.width;
	guint first_beyond = 0, beyond_count = 0;
	for (guint i = 0; i < resets->len; i++) {
		if (g_array_index(resets, struct quota_reset, i).beyond_plot) {
			if (!beyond_count) {
				first_beyond = i;
			}
			beyond_count++;
		}
	}
	if (beyond_count) {
		const struct quota_reset *next = &g_array_index(resets, struct quota_reset, first_beyond);
		struct tm tm;
		localtime_r(&next->at, &tm);
		char when[32], label[48];
		strftime(when, sizeof(when), "%a %H:%M", &tm);
		if (beyond_count > 1) {
			snprintf(label, sizeof(label), "» %s +%u", when, beyond_count - 1);
		} else {
			snprintf(label, sizeof(label), "» %s", when);
		}
		int width = 0;
		pd_text_size(cr, font, label, &width, NULL);
		width = MIN(width + 1, chart.width);
		label_right -= width;
		pd_text(cr, font, label, label_right, chart.y - 19, width, 17, quota_color(next->limit),
			PD_RIGHT);
		label_right -= 4;
	}
	int edge_offset = 2;
	for (guint i = 0; i < resets->len; i++) {
		struct quota_reset *reset = &g_array_index(resets, struct quota_reset, i);
		if (reset->beyond_plot) {
			reset->edge_offset = edge_offset;
			edge_offset = MIN(chart.width / 4, edge_offset + 3);
		}
		double x = quota_reset_x(chart, axis, reset);
		uint32_t color = quota_color(reset->limit);
		pd_rect(cr, CLAMP(x - 1, chart.x, chart.x + chart.width - 2), chart.y, 2,
			chart.height, (color & 0xffffff00) | (reset->at > axis->now ? 0xc8 : 0x88));
		if (reset->beyond_plot) {
			continue;
		}
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
		double d = fabs(ctx->px - quota_reset_x(chart, axis, r));
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
			if (!(r->beyond_plot && nearest->beyond_plot) &&
					!tw_claude_same_window(r->at, nearest->at)) {
				continue;
			}
			struct tm tm;
			localtime_r(&r->at, &tm);
			char when[48], tip[160];
			strftime(when, sizeof(when), "%a %d %b %H:%M", &tm);
			if (r->weekly_recovery) {
				snprintf(tip, sizeof(tip), "Weekly reset %s · %s", when, quota_name(r->limit));
			} else {
				snprintf(tip, sizeof(tip), "%s · reset %s", quota_name(r->limit), when);
			}
			pd_rect(cr, chart.x + 3, y, chart.width - 6, 18, pal->bg);
			pd_rect(cr, chart.x + 7, y + 4, 3, 10, quota_color(r->limit));
			pd_text(cr, font, tip, chart.x + 14, y, chart.width - 20, 18, pal->fg, PD_LEFT);
			y += 18;
		}
	} else {
		for (guint i = 0; i < axis->hovered->len && y + 18 <= chart.y + chart.height; i++) {
			const struct agent_limit *l = axis->hovered->pdata[i];
			pd_rect(cr, chart.x + 3, y, chart.width - 6, 18, pal->bg);
			pd_rect(cr, chart.x + 7, y + 4, 3, 10, quota_color(l));
			pd_text(cr, font, quota_name(l), chart.x + 14, y, chart.width - 20, 18, pal->fg, PD_LEFT);
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
		const GPtrArray *limits, struct pbox b, const char *font, bool weekly, const char *title) {
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
	bool codex_session_active = false;
	for (guint i = 0; limits && i < limits->len; i++) {
		const struct agent_limit *l = limits->pdata[i];
		const struct tw_claude_window *win = weekly ? &l->usage.seven_day : &l->usage.five_hour;
		if (l->kind == AGENT_CODEX && !l->live_account && win->known &&
				(win->resets_at > now || (!weekly && agent_five_hour_blocker(limits, l, now)))) {
			codex_session_active = true;
		}
	}
	for (guint i = 0; limits && i < limits->len; i++) {
		const struct agent_limit *l = limits->pdata[i];
		const struct tw_claude_window *win = weekly ? &l->usage.seven_day : &l->usage.five_hour;
		if (!l->live_account) {
			visible |= quota_trace(cr, chart, l, weekly, &timeline, resets);
		}
		// A fresh account window can provide a reset after the session window expires.
		if (!win->known || (l->live_account && codex_session_active)) {
			continue;
		}
		const struct tw_claude_window *blocker = weekly ? NULL :
			agent_five_hour_blocker(limits, l, now);
		if (blocker) {
			if (blocker->resets_at > now) {
				quota_add_reset(resets, &timeline, l, blocker->resets_at, true);
				visible = true;
			}
		} else if (win->resets_at > now && win->resets_at <= now + window) {
			quota_add_reset(resets, &timeline, l, win->resets_at, false);
			quota_forecast(cr, chart, l, win, &timeline, window, weekly);
			visible = true;
		}
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

static bool agents_overlay(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	int inset = 10, width = ctx->width - 2 * inset;
	char font[192];
	snprintf(font, sizeof(font), "%s %.1f", pal->family, CLAMP(ctx->height / 23.0, 7, 12));
	int height = (ctx->height - 44) / 2;
	const struct agent_usage *usage = agent_widget_usage(w);
	const GPtrArray *limits = usage ? usage->limits : NULL;
	quota_overlay(ctx, pal, limits, (struct pbox){ inset, 8, width, height }, font, false,
		"Five-hour quota");
	quota_overlay(ctx, pal, limits, (struct pbox){ inset, 12 + height, width, height }, font, true,
		"Weekly quota");

	int legend_y = ctx->height - 26;
	int cell = width / AGENT_COUNT;
	for (enum agent_kind i = 0; i < AGENT_COUNT; i++) {
		int x = inset + i * cell;
		pd_rect(ctx->cairo, x, legend_y + 8, 9, 4, agent_chart_colors[i]);
		pd_text(ctx->cairo, font, agent_names[i], x + 13, legend_y, cell - 13, 20,
			pal->fg, PD_LEFT);
	}
	return true;
}

struct agy_quota_group {
	char *name;
	const struct agent_limit *window[2];
};

static void agy_group_free(gpointer data) {
	struct agy_quota_group *group = data;
	g_free(group->name);
	g_free(group);
}

static char *agy_group_name(const struct agent_limit *limit) {
	const char *name = quota_name(limit);
	if (g_str_has_prefix(name, "Antigravity · ")) {
		return g_strdup(name + strlen("Antigravity · "));
	}
	const char *end = strstr(name, " · ");
	return end ? g_strndup(name, end - name) : g_strdup(name);
}

static GPtrArray *agy_chart_groups(const struct agent_usage *usage) {
	GPtrArray *groups = g_ptr_array_new_with_free_func(agy_group_free);
	for (guint i = 0; usage && i < usage->limits->len; i++) {
		const struct agent_limit *limit = usage->limits->pdata[i];
		if (limit->kind != AGENT_AGY) {
			continue;
		}
		char *name = agy_group_name(limit);
		int window = limit->usage.seven_day.known ? 1 : 0;
		struct agy_quota_group *group = NULL;
		for (guint j = 0; j < groups->len; j++) {
			struct agy_quota_group *candidate = groups->pdata[j];
			if (!candidate->window[window] && strcmp(candidate->name, name) == 0) {
				group = candidate;
				break;
			}
		}
		if (!group) {
			group = g_new0(struct agy_quota_group, 1);
			group->name = g_steal_pointer(&name);
			g_ptr_array_add(groups, group);
		}
		g_free(name);
		group->window[window] = limit;
	}
	return groups;
}

static void agy_chart_title(char *out, size_t size, const GPtrArray *series,
		const struct agent_limit *limit, bool weekly) {
	const char *period = weekly ? "Weekly quota" : "5-hour quota";
	const struct tw_claude_window *win = weekly ? &limit->usage.seven_day : &limit->usage.five_hour;
	time_t now = time(NULL);
	double used = tw_claude_window_now(win, now);
	const struct tw_claude_window *blocker = weekly ? NULL : agent_five_hour_blocker(series, limit, now);
	if (blocker) {
		if (blocker->resets_at > now) {
			char remaining[32];
			tw_claude_format_span(blocker->resets_at - now, true, remaining, sizeof(remaining));
			snprintf(out, size, "%s · weekly limit resets in %s", period, remaining);
		} else {
			snprintf(out, size, "%s · weekly limit reached", period);
		}
	} else if (win->resets_at > now) {
		char remaining[32];
		tw_claude_format_span(win->resets_at - now, true, remaining, sizeof(remaining));
		snprintf(out, size, "%s · %.1f%% used · resets in %s", period, used, remaining);
	} else if (!win->resets_at && used == 0) {
		snprintf(out, size, "%s · 0.0%% used · no active reset", period);
	} else {
		snprintf(out, size, "%s · %.1f%% used", period, used);
	}
}

static bool agents_agy_quotas(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	GPtrArray *groups = agy_chart_groups(agent_widget_usage(w));
	char font[192], title_font[192];
	snprintf(font, sizeof(font), "%s 9", pal->family);
	snprintf(title_font, sizeof(title_font), "%s Bold 11", pal->family);
	const int pad = 10, gap = 12;
	if (!groups->len) {
		pd_text(ctx->cairo, title_font, "Awaiting Antigravity quota readings", pad, pad,
			ctx->width - 2 * pad, ctx->height - 2 * pad, pal->dim, PD_CENTER);
		g_ptr_array_unref(groups);
		return true;
	}
	int row_height = (ctx->height - 2 * pad) / groups->len;
	int column_width = (ctx->width - 2 * pad - gap) / 2;
	GPtrArray *series = g_ptr_array_sized_new(1);
	for (guint i = 0; i < groups->len; i++) {
		struct agy_quota_group *group = groups->pdata[i];
		int y = pad + i * row_height;
		const struct agent_limit *tint = group->window[0] ? group->window[0] : group->window[1];
		pd_rect(ctx->cairo, pad, y + 6, 8, 4, quota_color(tint));
		pd_text(ctx->cairo, title_font, group->name, pad + 13, y, ctx->width - 2 * pad - 13,
			18, pal->fg, PD_LEFT);
		for (int j = 0; j < 2; j++) {
			const struct agent_limit *limit = group->window[j];
			int x = pad + j * (column_width + gap);
			if (!limit) {
				pd_text(ctx->cairo, font, j ? "Weekly quota unavailable" : "No five-hour quota reported",
					x, y + 22, column_width, 20, pal->dim, PD_LEFT);
				continue;
			}
			char title[112];
			g_ptr_array_add(series, (gpointer)limit);
			if (!j && group->window[1]) {
				g_ptr_array_add(series, (gpointer)group->window[1]);
			}
			agy_chart_title(title, sizeof(title), series, limit, j);
			quota_overlay(ctx, pal, series, (struct pbox){ x, y + 20, column_width, row_height - 24 },
				font, j, title);
			g_ptr_array_set_size(series, 0);
		}
	}
	g_ptr_array_unref(series);
	g_ptr_array_unref(groups);
	return true;
}

static bool agents_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	if (strcmp(ctx->style, "overlay") == 0) {
		return agents_overlay(w, ctx, pal);
	}
	if (strcmp(ctx->style, "agy-quotas") == 0) {
		return agents_agy_quotas(w, ctx, pal);
	}
	char font[192];
	snprintf(font, sizeof(font), "%s %.1f", pal->family, CLAMP(ctx->height / 9.0, 7, 15));
	for (enum agent_kind i = 0; i < AGENT_COUNT; i++) {
		char line[72];
		double quota[2];
		agent_summary(agent_widget_usage(w), i, line, sizeof(line), quota);
		double height = ctx->height / AGENT_COUNT;
		int bar_width = quota[0] >= 0 || quota[1] >= 0 ? 38 : 0;
		if (bar_width) {
			agents_bars(ctx->cairo, 8, i * height, bar_width - 6, height, quota, pal->fg, pal->accent);
		}
		pd_text(ctx->cairo, font, line, 8 + bar_width, i * height, ctx->width - 16 - bar_width,
			height, pal->fg, PD_LEFT);
	}
	return true;
}

static bool agents_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor a = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	a.right_align = true;
	info_flyout_toggle(w, a);
	return true;
}

/*
 * TODO(KsmBl, before merge): Redesign this tooltip for a quick read.
 * Give each provider a clearly labeled line with its quota windows and local
 * worker count. Spell out possible remote usage wherever the signal appears.
 */
static char *agents_tooltip(struct widget *w, struct hotspot *hotspot) {
	GString *text = g_string_new("5-hour / weekly quota used · local terminal workers");
	for (enum agent_kind i = 0; i < AGENT_COUNT; i++) {
		char line[72];
		agent_summary(agent_widget_usage(w), i, line, sizeof(line), NULL);
		g_string_append_printf(text, "\n%s", line);
	}
	g_string_append(text, "\n? indicates possible remote usage");
	return g_string_free(text, false);
}

const struct widget_impl widget_agents = {
	.type = "agents",
	.init = agents_init,
	.destroy = agents_destroy,
	.set_active = agents_active,
	.measure = agents_measure,
	.render = agents_render,
	.draw_gadget = agents_gadget,
	.click = agents_click,
	.tooltip = agents_tooltip,
};
