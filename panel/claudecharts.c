/* The Claude Code usage charts (claudecharts.h), moved out of the flyout. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "claudecharts.h"
#include "draw.h"

void claude_chart_surface(cairo_t *cr, const struct claude_look *st, struct pbox g) {
	cairo_new_path(cr);
	pd_rounded(cr, g.x, g.y, g.width, g.height, st->chart_radius);
	pd_color(cr, st->chart_bg);
	cairo_fill(cr);
	if (st->sunken) {
		pd_bevel(cr, g.x, g.y, g.width, g.height, true);
	}
}

uint32_t claude_desktop_chart_bg(const struct gadget_palette *pal) {
	return pal->mono ? pal->bg : (pal->track & 0xffffff00) | 0x50;
}

static void chart_bg(cairo_t *cr, const struct claude_look *st, struct pbox g) {
	claude_chart_surface(cr, st, g);
	for (int i = 1; i < 4; i++) {
		pd_rect(cr, g.x + (st->sunken ? 1 : 0), g.y + g.height * i / 4,
			g.width - (st->sunken ? 2 : 0), 1, st->chart_grid);
	}
}

void claude_charts_load(struct claude_charts *c, const struct tw_claude_usage *u, time_t now) {
	free(c->points);
	c->points = NULL;
	c->count = u && u->history ? tw_claude_history_read(u->history, &c->points) : 0;
	c->now = now;
	c->five_hour = u ? u->five_hour : (struct tw_claude_window){ 0 };
	c->seven_day = u ? u->seven_day : (struct tw_claude_window){ 0 };
	c->have = u && u->updated && u->seven_day.resets_at > 0;
}

static double chart_x(struct pbox g, time_t start, time_t end, time_t t) {
	double share = end > start ? (double)(t - start) / (double)(end - start) : 0;
	return g.x + g.width * fmin(fmax(share, 0), 1);
}

static double chart_y(struct pbox g, double used) {
	return g.y + g.height - g.height * fmin(fmax(used, 0), 100) / 100;
}

/* The usage of one window: 0 at its start, its points, now while it runs. */
/*
 * When a window was over: at its reset, or earlier when a newer one began
 * before it (usage reset by hand), at the first reading of that one.
 */
static time_t window_end(const struct claude_charts *c, bool week, time_t reset) {
	time_t end = reset;
	for (int i = 0; i < c->count; i++) {
		const struct tw_claude_window *w = week ? &c->points[i].seven_day :
			&c->points[i].five_hour;
		if (w->known && w->resets_at > reset + 300 && c->points[i].at < end) {
			end = c->points[i].at;
		}
	}
	const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
	if (cur->known && cur->resets_at > reset + 300 && c->now < end) {
		end = c->now;
	}
	return end;
}

static int window_series(const struct claude_charts *c, bool week, time_t reset, time_t length,
		time_t *ts, double *vs, int max) {
	time_t end = window_end(c, week, reset);
	int n = 0;
	ts[n] = reset - length;
	vs[n++] = 0;
	for (int i = 0; i < c->count && n < max - 1; i++) {
		const struct tw_claude_window *w = week ? &c->points[i].seven_day :
			&c->points[i].five_hour;
		if (!w->known || !tw_claude_same_window(w->resets_at, reset) ||
				c->points[i].at < reset - length || c->points[i].at > end) {
			continue;
		}
		// a run of the same value is its first and last reading: the line is the
		// same, and a week of readings (most of them changes of the other window)
		// fits instead of running out of room before now
		if (n >= 2 && vs[n - 1] == w->used && vs[n - 2] == w->used) {
			ts[n - 1] = c->points[i].at;
			continue;
		}
		ts[n] = c->points[i].at;
		vs[n] = w->used; // (lower in the same window: reset by hand)
		n++;
	}
	const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
	if (cur->known && tw_claude_same_window(cur->resets_at, reset) && c->now < end &&
			n < max) {
		ts[n] = c->now;
		vs[n] = cur->used;
		n++;
	} else if (c->now >= end && n < max) {
		// over: what was used stays until it ended
		ts[n] = end;
		vs[n] = vs[n - 1];
		n++;
	}
	return n;
}

/* The usage at a time between the points of a series; below 0 outside it. */
/*
 * Readings far apart that rose meanwhile: what was used in between was not seen
 * from here (on another computer, or before the history), only that it was
 * somewhere between the two, and when is not known.
 */
#define GAP_S (15 * 60)
#define GAP_RISE 3.0 // percent; less is a long answer here

static bool is_gap(const time_t *ts, const double *vs, int i) {
	return i > 0 && ts[i] - ts[i - 1] > GAP_S && vs[i] - vs[i - 1] >= GAP_RISE;
}

/*
 * What was used at a time: *lo and *hi the same where it is known (between
 * readings close together), the two readings around a gap; false outside the series.
 */
static bool range_at(const time_t *ts, const double *vs, int n, time_t t, double *lo,
		double *hi) {
	if (n < 1 || t < ts[0] || t > ts[n - 1]) {
		return false;
	}
	for (int i = 1; i < n; i++) {
		if (t <= ts[i]) {
			if (is_gap(ts, vs, i) && t < ts[i]) {
				*lo = vs[i - 1];
				*hi = vs[i];
				return true;
			}
			double span = (double)(ts[i] - ts[i - 1]);
			double share = span > 0 ? (double)(t - ts[i - 1]) / span : 1;
			*lo = *hi = vs[i - 1] + (vs[i] - vs[i - 1]) * share;
			return true;
		}
	}
	*lo = *hi = vs[n - 1];
	return true;
}

/* The time under the pointer, for a chart from start to end. */
static time_t time_at(struct pbox g, time_t start, time_t end, double px) {
	double share = g.width > 0 ? (px - g.x) / g.width : 0;
	share = share < 0 ? 0 : share > 1 ? 1 : share;
	return start + (time_t)((double)(end - start) * share);
}

static void when_words(time_t t, bool weekday, char *out, size_t size) {
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, size, weekday ? "%a %H:%M" : "%H:%M", &tm);
}

static uint32_t usage_color(const struct claude_look *st, double used) {
	if (used >= 90) {
		return st->dark ? 0xff7b72ff : 0xc42b1cff;
	}
	if (used >= 75) {
		return st->dark ? 0xf2b25cff : 0xb3600aff;
	}
	return st->chart_line;
}

/*
 * On from now at a pace (percent a second): dashed, to the end of the window or
 * to where it runs out.
 */
static void draw_pace(cairo_t *cr, const struct claude_look *st, struct pbox g, time_t start,
		time_t end, time_t now, time_t over, double used, double pace) {
	if (pace <= 0 || now >= over) {
		return;
	}
	time_t until = over;
	double at_end = used + pace * (double)(over - now);
	if (at_end > 100) {
		until = now + (time_t)((100 - used) / pace);
		at_end = 100;
	}
	double dash[] = { 4, 3 };
	cairo_new_path(cr);
	cairo_move_to(cr, chart_x(g, start, end, now), chart_y(g, used));
	cairo_line_to(cr, chart_x(g, start, end, until), chart_y(g, at_end));
	cairo_set_dash(cr, dash, 2, 0);
	pd_color(cr, usage_color(st, at_end)); // (see-through it is lost on a dark chart)
	cairo_set_line_width(cr, 1.25);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);
}

/* The line and the area under it, through the points of the series. */
static void draw_series(cairo_t *cr, struct pbox g, time_t start, time_t end,
		const time_t *ts, const double *vs, int n, uint32_t color) {
	if (n < 1) {
		return;
	}
	// from the start of the window to the first value written down, when that came
	// much later (the history began after it): only a dashed guess, nothing filled
	if (n > 1 && ts[1] - ts[0] > 3600) {
		double dash[] = { 3, 3 };
		cairo_new_path(cr);
		cairo_move_to(cr, chart_x(g, start, end, ts[0]), chart_y(g, vs[0]));
		cairo_line_to(cr, chart_x(g, start, end, ts[1]), chart_y(g, vs[1]));
		cairo_set_dash(cr, dash, 2, 0);
		pd_color(cr, (color & 0xffffff00) | 0x80);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_set_dash(cr, NULL, 0, 0);
		ts++;
		vs++;
		n--;
	}
	// the area under what is known: across a gap only what was used before it
	cairo_new_path(cr);
	cairo_move_to(cr, chart_x(g, start, end, ts[0]), g.y + g.height);
	for (int i = 0; i < n; i++) {
		if (is_gap(ts, vs, i)) {
			cairo_line_to(cr, chart_x(g, start, end, ts[i]), chart_y(g, vs[i - 1]));
		}
		cairo_line_to(cr, chart_x(g, start, end, ts[i]), chart_y(g, vs[i]));
	}
	cairo_line_to(cr, chart_x(g, start, end, ts[n - 1]), g.y + g.height);
	cairo_close_path(cr);
	pd_color(cr, (color & 0xffffff00) | 0x48);
	cairo_fill(cr);
	// a gap: hatched between the two readings, "used somewhere in here"
	for (int i = 1; i < n; i++) {
		if (!is_gap(ts, vs, i)) {
			continue;
		}
		double x0 = chart_x(g, start, end, ts[i - 1]), x1 = chart_x(g, start, end, ts[i]);
		double y0 = chart_y(g, vs[i]), y1 = chart_y(g, vs[i - 1]);
		cairo_save(cr);
		cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
		cairo_clip(cr);
		pd_color(cr, (color & 0xffffff00) | 0x14);
		cairo_paint(cr);
		cairo_new_path(cr);
		for (double d = -(y1 - y0); d < x1 - x0; d += 5) {
			cairo_move_to(cr, x0 + d, y1);
			cairo_line_to(cr, x0 + d + (y1 - y0), y0);
		}
		pd_color(cr, (color & 0xffffff00) | 0x50);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_restore(cr);
	}
	// the line: solid where it is known, dashed across a gap (held, then risen)
	cairo_new_path(cr);
	for (int i = 0; i < n; i++) {
		double x = chart_x(g, start, end, ts[i]), y = chart_y(g, vs[i]);
		if (i == 0) {
			cairo_move_to(cr, x, y);
		} else if (is_gap(ts, vs, i)) {
			pd_color(cr, color);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
			double dash[] = { 3, 3 };
			double hold = chart_y(g, vs[i - 1]);
			cairo_move_to(cr, chart_x(g, start, end, ts[i - 1]), hold);
			cairo_line_to(cr, x, hold);
			cairo_line_to(cr, x, y);
			cairo_set_dash(cr, dash, 2, 0);
			pd_color(cr, (color & 0xffffff00) | 0xa0);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			cairo_set_dash(cr, NULL, 0, 0);
			cairo_move_to(cr, x, y);
		} else {
			cairo_line_to(cr, x, y);
		}
	}
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
}

/* What is behind now, shaded, as a copy shows how far it is. */
static void draw_behind(cairo_t *cr, const struct claude_look *st, struct pbox g, double x) {
	pd_rect(cr, g.x, g.y, x - g.x, g.height, (st->chart_line & 0xffffff00) | 0x14);
}

/* The line of now: across the chart, with a dot where the usage is. */
static void draw_now(cairo_t *cr, const struct claude_look *st, struct pbox g, double x, double used,
		uint32_t color) {
	pd_rect(cr, round(x) - 0.5, g.y, 1, g.height, st->fg);
	if (used >= 0) {
		cairo_new_path(cr);
		cairo_arc(cr, x, chart_y(g, used), 3.5, 0, 2 * M_PI);
		pd_color(cr, color);
		cairo_fill_preserve(cr);
		pd_color(cr, st->fg);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
}

/*
 * The last three 5-hour windows in one chart, along the hours from the start
 * of the oldest to the reset of the newest (or now, when none runs): each
 * window a band with its usage rising in it and what it came to at its top,
 * the time between them empty, and the line of now.
 */
void claude_chart_five_hours(const struct claude_hover *f, const struct claude_charts *c, cairo_t *cr,
		const struct claude_look *st, struct pbox box) {
	// the windows there were: the resets in the history and the one now, newest last
	time_t resets[64];
	int nr = 0;
	for (int i = -1; i < c->count && nr < 64; i++) {
		time_t r = i < 0 ? (c->five_hour.known ? c->five_hour.resets_at : 0) :
			c->points[i].five_hour.resets_at;
		bool seen = r <= 0;
		for (int k = 0; k < nr && !seen; k++) {
			seen = tw_claude_same_window(resets[k], r);
		}
		if (!seen) {
			resets[nr++] = r;
		}
	}
	for (int i = 1; i < nr; i++) { // oldest first
		for (int k = i; k > 0 && resets[k] < resets[k - 1]; k--) {
			time_t t = resets[k];
			resets[k] = resets[k - 1];
			resets[k - 1] = t;
		}
	}
	int first = nr > 3 ? nr - 3 : 0;
	struct pbox g = { box.x, box.y, box.width, box.height - 20 };
	chart_bg(cr, st, g);
	if (nr == 0) {
		pd_text(cr, st->font, "Nothing written down yet", g.x, g.y, g.width, g.height, st->dim,
			PD_CENTER);
		return;
	}
	time_t start = resets[first] - TW_CLAUDE_FIVE_HOURS;
	time_t end = resets[nr - 1] > c->now ? resets[nr - 1] : c->now;
	// the hours: a faint line at each, a time under every few
	long span_h = (long)((end - start) / 3600) + 1;
	int every = span_h <= 16 ? 3 : span_h <= 30 ? 6 : 12;
	struct tm tm;
	localtime_r(&start, &tm);
	tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	for (long h = 1; h <= span_h + 1; h++) {
		struct tm at = tm;
		at.tm_hour += (int)h;
		time_t t = mktime(&at);
		if (t <= start || t >= end) {
			continue;
		}
		double x = chart_x(g, start, end, t);
		bool labelled = at.tm_hour % every == 0;
		pd_rect(cr, round(x), g.y, 1, g.height, labelled ? st->chart_grid :
			(st->chart_grid & 0xffffff00) | ((st->chart_grid & 0xff) / 2));
		if (labelled) {
			char label[16];
			strftime(label, sizeof(label), "%H:%M", &at);
			pd_text(cr, st->font, label, x - 30, g.y + g.height + 2, 60, 18, st->dim, PD_CENTER);
		}
	}
	pd_rect(cr, g.x, g.y + g.height / 2, g.width, 1, st->chart_grid);
	double now_x = chart_x(g, start, end, c->now);
	draw_behind(cr, st, g, now_x);
	double now_used = -1;
	uint32_t now_color = st->chart_line;
	for (int i = first; i < nr; i++) {
		time_t reset = resets[i], wstart = reset - TW_CLAUDE_FIVE_HOURS;
		// (a window reset by hand ends where the next one began)
		time_t over = window_end(c, false, reset);
		double x0 = chart_x(g, start, end, wstart), x1 = chart_x(g, start, end, over);
		// the window: a band, its edges marked
		pd_rect(cr, x0, g.y, x1 - x0, g.height, (st->chart_line & 0xffffff00) | 0x10);
		pd_rect(cr, round(x0), g.y, 1, g.height, (st->chart_line & 0xffffff00) | 0x60);
		pd_rect(cr, round(x1) - 1, g.y, 1, g.height, (st->chart_line & 0xffffff00) | 0x60);
		time_t ts[256];
		double vs[256];
		int n = window_series(c, false, reset, TW_CLAUDE_FIVE_HOURS, ts, vs, 256);
		double peak = 0;
		for (int k = 0; k < n; k++) {
			peak = fmax(peak, vs[k]);
		}
		uint32_t color = usage_color(st, peak);
		draw_series(cr, g, start, end, ts, vs, n, color);
		bool running = c->now >= wstart && c->now < over;
		if (running && n > 0) {
			now_used = vs[n - 1];
			now_color = color;
			// on at the pace of the window so far, to its reset or where it runs out
			if (c->five_hour.known && tw_claude_same_window(c->five_hour.resets_at, reset)) {
				draw_pace(cr, st, g, start, end, c->now, over, now_used,
					tw_claude_window_pace(&c->five_hour, TW_CLAUDE_FIVE_HOURS, c->now,
						now_used));
			}
		}
		// what it came to, at its top
		char label[16];
		snprintf(label, sizeof(label), "%.0f%%", running && n > 0 ? vs[n - 1] : peak);
		if (x1 - x0 > 30) {
			pd_text(cr, running ? st->bold : st->font, label, x0, g.y + 2, x1 - x0 - 4, 16,
				running ? st->fg : st->dim, PD_RIGHT);
		}
	}
	draw_now(cr, st, g, now_x, now_used, now_color);
	// what was used at the time under the pointer
	if (f->inside && pbox_contains(&g, f->px, f->py)) {
		time_t t = time_at(g, start, end, f->px);
		char when[32], text[96];
		when_words(t, false, when, sizeof(when));
		double ys[1] = { -1 };
		uint32_t colors[1] = { st->chart_line };
		snprintf(text, sizeof(text), "%s\nnot in a 5-hour window", when);
		for (int i = first; i < nr; i++) {
			time_t reset = resets[i];
			if (t < reset - TW_CLAUDE_FIVE_HOURS || t > reset) {
				continue;
			}
			time_t ts[256];
			double vs[256];
			int n = window_series(c, false, reset, TW_CLAUDE_FIVE_HOURS, ts, vs, 256);
			double lo, hi;
			if (range_at(ts, vs, n, t, &lo, &hi) && hi - lo >= 0.5) {
				ys[0] = chart_y(g, lo);
				colors[0] = usage_color(st, hi);
				snprintf(text, sizeof(text), "%s\nbetween %.0f%% and %.0f%%, when is not known",
					when, lo, hi);
			} else if (range_at(ts, vs, n, t, &lo, &hi)) {
				ys[0] = chart_y(g, lo);
				colors[0] = usage_color(st, lo);
				snprintf(text, sizeof(text), "%s\n%.0f%% used", when, lo);
			} else if (t > c->now && c->five_hour.known &&
					tw_claude_same_window(c->five_hour.resets_at, reset) && n > 0 &&
					tw_claude_window_pace(&c->five_hour, TW_CLAUDE_FIVE_HOURS, c->now,
						vs[n - 1]) > 0) {
				double pace = tw_claude_window_pace(&c->five_hour, TW_CLAUDE_FIVE_HOURS,
					c->now, vs[n - 1]);
				double ahead = fmin(vs[n - 1] + pace * (double)(t - c->now), 100);
				ys[0] = chart_y(g, ahead);
				colors[0] = usage_color(st, ahead);
				snprintf(text, sizeof(text), "%s\nat this pace %.0f%%", when, ahead);
			} else {
				snprintf(text, sizeof(text), "%s\nstill to come", when);
			}
		}
		pd_chart_hover(cr, st->font, g.x, g.y, g.width, g.height, f->px, ys, colors, 1, text,
			st->fg, st->label_bg, st->dim);
	}
}

void claude_chart_week(const struct claude_hover *f, const struct claude_charts *c, cairo_t *cr,
		const struct claude_look *st, struct pbox box) {
	struct pbox g = { box.x, box.y, box.width, box.height - 20 };
	chart_bg(cr, st, g);
	time_t end = c->seven_day.resets_at, start = end - TW_CLAUDE_SEVEN_DAYS;
	// the days: a line at each midnight, the name of the day under it
	struct tm tm;
	localtime_r(&start, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_mday++;
	tm.tm_isdst = -1;
	for (int d = 0; d < 8; d++, tm.tm_mday++) {
		struct tm day = tm;
		time_t midnight = mktime(&day);
		if (midnight <= start || midnight >= end) {
			continue;
		}
		double x = chart_x(g, start, end, midnight);
		pd_rect(cr, round(x), g.y, 1, g.height, st->chart_grid);
		char name[16];
		strftime(name, sizeof(name), "%a", &day);
		double next = chart_x(g, start, end, midnight + 24 * 3600);
		double w = fmin(next, g.x + g.width) - x;
		if (w > 18) {
			pd_text(cr, st->font, name, x, g.y + g.height + 2, w, 18, st->dim, PD_CENTER);
		}
	}
	pd_rect(cr, g.x, g.y + g.height / 2, g.width, 1, st->chart_grid);
	pd_text(cr, st->font, "100%", g.x + 4, g.y + 1, 60, 16, st->dim, PD_LEFT);
	pd_text(cr, st->font, "50%", g.x + 4, g.y + g.height / 2 + 1, 60, 16, st->dim, PD_LEFT);
	time_t ts[512];
	double vs[512];
	int n = window_series(c, true, end, TW_CLAUDE_SEVEN_DAYS, ts, vs, 512);
	double used = n > 0 ? vs[n - 1] : 0;
	uint32_t color = usage_color(st, used);
	double now_x = chart_x(g, start, end, c->now);
	draw_behind(cr, st, g, now_x);
	draw_series(cr, g, start, end, ts, vs, n, color);
	// on at the pace of the week so far: dashed, to its end or to where it runs out
	// (since the week began, or since it was reset by hand: as the flyout's text says)
	double pace = c->seven_day.known ? tw_claude_window_pace(&c->seven_day,
		TW_CLAUDE_SEVEN_DAYS, c->now, used) : 0;
	if (c->now > start) {
		draw_pace(cr, st, g, start, end, c->now, end, used, pace);
	}
	draw_now(cr, st, g, now_x, used, color);
	// what now is, by the line: on its left near the end, else on its right
	char label[48];
	snprintf(label, sizeof(label), "now %.0f%%", used);
	int tw = 0;
	pd_text_size(cr, st->font, label, &tw, NULL);
	double lx = now_x + 6 + tw > g.x + g.width - 4 ? now_x - 6 - tw : now_x + 6;
	bool hovering = f->inside && pbox_contains(&g, f->px, f->py);
	if (!hovering) { // (the reading under the pointer says it then)
		pd_text(cr, st->font, label, lx, g.y + 2, tw + 2, 16, st->fg, PD_LEFT);
	}
	// at the time under the pointer: what was used, or where this pace leads
	if (f->inside && pbox_contains(&g, f->px, f->py)) {
		time_t t = time_at(g, start, end, f->px);
		char when[32], text[96];
		when_words(t, true, when, sizeof(when));
		double ys[1] = { -1 };
		uint32_t colors[1] = { color };
		double lo = -1, hi = -1;
		bool known = t <= c->now && range_at(ts, vs, n, t, &lo, &hi);
		if (known && hi - lo >= 0.5) {
			ys[0] = chart_y(g, lo);
			colors[0] = usage_color(st, hi);
			snprintf(text, sizeof(text), "%s\nbetween %.0f%% and %.0f%%, when is not known",
				when, lo, hi);
		} else if (known) {
			ys[0] = chart_y(g, lo);
			colors[0] = usage_color(st, lo);
			snprintf(text, sizeof(text), "%s\n%.0f%% used", when, lo);
		} else if (t > c->now && c->now > start && pace > 0) {
			double ahead = fmin(used + pace * (double)(t - c->now), 100);
			ys[0] = chart_y(g, ahead);
			colors[0] = usage_color(st, ahead);
			snprintf(text, sizeof(text), "%s\nat this pace %.0f%%", when, ahead);
		} else {
			snprintf(text, sizeof(text), "%s", when);
		}
		pd_chart_hover(cr, st->font, g.x, g.y, g.width, g.height, f->px, ys, colors, 1, text,
			st->fg, st->label_bg, st->dim);
	}
}

