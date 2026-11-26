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

void claude_charts_set(struct claude_charts *c, const struct tw_claude_point *points, int count,
		const struct tw_claude_usage *u, time_t now) {
	claude_charts_load(c, NULL, now);
	if (count > 0 && points) {
		c->points = malloc(count * sizeof(*points));
		if (c->points) {
			memcpy(c->points, points, count * sizeof(*points));
			c->count = count;
		}
	}
	c->five_hour = u ? u->five_hour : (struct tw_claude_window){ 0 };
	c->seven_day = u ? u->seven_day : (struct tw_claude_window){ 0 };
	c->have = u && u->updated && u->seven_day.resets_at > 0;
}

/*
 * How far past its edges a chart draws: none, but the chart of all accounts lets
 * its lines go on beyond and clips them (-1: as far again on either side).
 */
static double chart_reach = 0;

static double chart_x(struct pbox g, time_t start, time_t end, time_t t) {
	double share = end > start ? (double)(t - start) / (double)(end - start) : 0;
	return g.x + g.width * fmin(fmax(share, chart_reach), 1 - chart_reach);
}

static double chart_y(struct pbox g, double used) {
	return g.y + g.height - g.height * fmin(fmax(used, 0), 100) / 100;
}

/*
 * The windows there were: the resets in the history and the one of now, each
 * once, oldest first, in a new array; how many.
 */
static int windows_of(const struct claude_charts *c, bool week, time_t **out) {
	time_t *resets = malloc((c->count + 1) * sizeof(*resets));
	int nr = 0;
	const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
	for (int i = -1; resets && i < c->count; i++) {
		time_t r = i < 0 ? (cur->known ? cur->resets_at : 0) :
			(week ? c->points[i].seven_day.resets_at : c->points[i].five_hour.resets_at);
		bool seen = r <= 0;
		for (int k = nr - 1; k >= 0 && !seen; k--) {
			seen = tw_claude_same_window(resets[k], r);
		}
		if (!seen) {
			resets[nr++] = r;
		}
	}
	for (int i = 1; i < nr; i++) {
		for (int k = i; k > 0 && resets[k] < resets[k - 1]; k--) {
			time_t t = resets[k];
			resets[k] = resets[k - 1];
			resets[k - 1] = t;
		}
	}
	*out = resets;
	return nr;
}

int claude_charts_back_max(const struct claude_charts *c, bool week) {
	time_t *resets;
	int nr = windows_of(c, week, &resets);
	free(resets);
	return week ? (nr > 1 ? nr - 1 : 0) : (nr > 3 ? nr - 3 : 0);
}

void claude_charts_span(const struct claude_charts *c, bool week, char *out, size_t size) {
	time_t *resets;
	int all = windows_of(c, week, &resets);
	time_t start = 0, end = 0;
	if (week) {
		int newest = all - 1;
		if (all > 0 && !(c->seven_day.known &&
				tw_claude_same_window(resets[newest], c->seven_day.resets_at))) {
			newest = all;
		}
		int back = c->back_week > newest ? newest : c->back_week;
		end = back > 0 && newest - back < all ? resets[newest - back] : c->seven_day.resets_at;
		start = end - TW_CLAUDE_SEVEN_DAYS;
	} else if (all > 0) {
		int back = c->back_five < 0 ? 0 : c->back_five > all - 3 ? (all > 3 ? all - 3 : 0) :
			c->back_five;
		int nr = all - back, first = nr > 3 ? nr - 3 : 0;
		start = resets[first] - TW_CLAUDE_FIVE_HOURS;
		end = resets[nr - 1];
	}
	free(resets);
	if (end <= 0) {
		snprintf(out, size, "%s", "");
		return;
	}
	char a[32], b[32];
	struct tm tm;
	localtime_r(&start, &tm);
	strftime(a, sizeof(a), week ? "%a %e %b" : "%a %e %b %H:%M", &tm);
	localtime_r(&end, &tm);
	strftime(b, sizeof(b), week ? "%a %e %b" : "%a %H:%M", &tm);
	snprintf(out, size, "%s – %s", a, b);
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
 * to where it runs out; in color, or 0 for the warning colors of where it leads.
 */
static void draw_pace(cairo_t *cr, const struct claude_look *st, struct pbox g, time_t start,
		time_t end, time_t now, time_t over, double used, double pace, uint32_t color) {
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
	// (see-through it is lost on a dark chart)
	pd_color(cr, color ? color : usage_color(st, at_end));
	cairo_set_line_width(cr, 1.25);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);
}

/* The line and the area under it (fill its alpha), through the points of the series. */
static void draw_series(cairo_t *cr, struct pbox g, time_t start, time_t end,
		const time_t *ts, const double *vs, int n, uint32_t color, uint32_t fill) {
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
	pd_color(cr, (color & 0xffffff00) | fill);
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

/*
 * Where a limit resets next: a dashed line across the chart in color, with
 * label ("resets 19:00") at its top, beside it on the side with room. Of k
 * lines at the same place this is the i-th: their dashes take turns, so each
 * color shows.
 */
static void draw_reset_shared(cairo_t *cr, const struct claude_look *st, struct pbox g,
		double x, uint32_t color, const char *label, int i, int k) {
	if (x < g.x + 1 || x > g.x + g.width - 1) {
		return;
	}
	k = k < 1 ? 1 : k;
	double dash[] = { 4, k > 1 ? 4.0 * (k - 1) : 3 };
	cairo_new_path(cr);
	cairo_move_to(cr, round(x) + 0.5, g.y);
	cairo_line_to(cr, round(x) + 0.5, g.y + g.height);
	cairo_set_dash(cr, dash, 2, k > 1 ? -4.0 * i : 0);
	pd_color(cr, color);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);
	if (label) {
		int tw = 0;
		pd_text_size(cr, st->font, label, &tw, NULL);
		double lx = x + 4 + tw > g.x + g.width - 2 ? x - 4 - tw : x + 4;
		if (lx >= g.x + 2) {
			// on a patch of the label color: readable over the lines and the shading
			cairo_new_path(cr);
			pd_rounded(cr, lx - 3, g.y + g.height - 19, tw + 6, 17, 3);
			pd_color(cr, (st->label_bg & 0xffffff00) | 0xd8);
			cairo_fill(cr);
			pd_text(cr, st->font, label, lx, g.y + g.height - 18, tw + 2, 16, st->fg, PD_LEFT);
		}
	}
}

static void draw_reset(cairo_t *cr, const struct claude_look *st, struct pbox g, double x,
		uint32_t color, const char *label) {
	draw_reset_shared(cr, st, g, x, color, label, 0, 1);
}

/*
 * A reset after the end of the chart: at its right edge a small arrow in color
 * with when ("Tue", "01:30"), the k-th of them one under the other.
 */
static void draw_reset_later(cairo_t *cr, const struct claude_look *st, struct pbox g,
		time_t reset, bool week, uint32_t color, int k) {
	char when[24];
	struct tm tm;
	localtime_r(&reset, &tm);
	strftime(when, sizeof(when), week ? "%a" : "%H:%M", &tm);
	int tw = 0;
	pd_text_size(cr, st->font, when, &tw, NULL);
	double y = g.y + 4 + 18 * k, right = g.x + g.width - 3;
	if (y + 16 > g.y + g.height) {
		return;
	}
	double lx = right - 9 - tw - 3;
	cairo_new_path(cr);
	pd_rounded(cr, lx - 3, y, tw + 18, 16, 3);
	pd_color(cr, (st->label_bg & 0xffffff00) | 0xd8);
	cairo_fill(cr);
	pd_text(cr, st->font, when, lx, y, tw + 2, 16, st->fg, PD_LEFT);
	cairo_new_path(cr);
	cairo_move_to(cr, right - 9, y + 3);
	cairo_line_to(cr, right - 1, y + 8);
	cairo_line_to(cr, right - 9, y + 13);
	cairo_close_path(cr);
	pd_color(cr, color);
	cairo_fill(cr);
}

/* "resets 19:00", "resets Wed 14:00" further off. */
static void reset_words(time_t reset, time_t now, const char *what, char *out, size_t size) {
	char when[32];
	struct tm tm;
	localtime_r(&reset, &tm);
	strftime(when, sizeof(when), reset - now < 20 * 3600 ? "%H:%M" : "%a %H:%M", &tm);
	snprintf(out, size, "%s%s%s", what ? what : "", what ? " resets " : "resets ", when);
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
	// the windows there were: the resets in the history and the one now, oldest first;
	// the three newest, or as many back as it is scrolled
	time_t *resets;
	int all = windows_of(c, false, &resets);
	int back = c->back_five < 0 ? 0 : c->back_five > all - 3 ? (all > 3 ? all - 3 : 0) :
		c->back_five;
	int nr = all - back; // the newest shown is resets[nr - 1]
	int first = nr > 3 ? nr - 3 : 0;
	struct pbox g = { box.x, box.y, box.width, box.height - 20 };
	chart_bg(cr, st, g);
	if (nr <= 0) {
		pd_text(cr, st->font, "Nothing written down yet", g.x, g.y, g.width, g.height, st->dim,
			PD_CENTER);
		free(resets);
		return;
	}
	time_t start = resets[first] - TW_CLAUDE_FIVE_HOURS;
	// the newest windows: on to now; older ones: to where the last of them ended
	time_t end = back > 0 ? window_end(c, false, resets[nr - 1]) :
		resets[nr - 1] > c->now ? resets[nr - 1] : c->now;
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
	if (back == 0) {
		draw_behind(cr, st, g, now_x);
	}
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
		draw_series(cr, g, start, end, ts, vs, n, color, 0x48);
		bool running = c->now >= wstart && c->now < over;
		if (running && n > 0) {
			now_used = vs[n - 1];
			now_color = color;
			// on at the pace of the window so far, to its reset or where it runs out
			if (c->five_hour.known && tw_claude_same_window(c->five_hour.resets_at, reset)) {
				draw_pace(cr, st, g, start, end, c->now, over, now_used,
					tw_claude_window_pace(&c->five_hour, TW_CLAUDE_FIVE_HOURS, c->now,
						now_used), 0);
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
	if (back == 0 && st->resets) {
		char label[48];
		if (c->five_hour.resets_at > c->now && c->five_hour.resets_at <= end) {
			reset_words(c->five_hour.resets_at, c->now, NULL, label, sizeof(label));
			draw_reset(cr, st, g, chart_x(g, start, end, c->five_hour.resets_at), now_color,
				label);
		}
		if (c->seven_day.resets_at > c->now && c->seven_day.resets_at < end) {
			reset_words(c->seven_day.resets_at, c->now, "week", label, sizeof(label));
			draw_reset(cr, st, g, chart_x(g, start, end, c->seven_day.resets_at), st->fg,
				label);
		}
	}
	if (back == 0) {
		draw_now(cr, st, g, now_x, now_used, now_color);
	}
	// what was used at the time under the pointer
	if (f->inside && pbox_contains(&g, f->px, f->py)) {
		time_t t = time_at(g, start, end, f->px);
		char when[32], text[96];
		when_words(t, back > 0, when, sizeof(when)); // (older windows: of which day)
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
	free(resets);
}

void claude_chart_week(const struct claude_hover *f, const struct claude_charts *c, cairo_t *cr,
		const struct claude_look *st, struct pbox box) {
	struct pbox g = { box.x, box.y, box.width, box.height - 20 };
	chart_bg(cr, st, g);
	// this week, or an older one as far back as it is scrolled
	time_t end = c->seven_day.resets_at;
	int back = 0;
	if (c->back_week > 0) {
		time_t *resets;
		int all = windows_of(c, true, &resets);
		int newest = all - 1;
		// (the week of now is the newest of them, when it is known)
		if (all > 0 && !(c->seven_day.known &&
				tw_claude_same_window(resets[newest], c->seven_day.resets_at))) {
			newest = all; // not among them: the newest of them is one back
		}
		back = c->back_week > newest ? newest : c->back_week;
		if (back > 0 && newest - back < all) {
			end = resets[newest - back];
		} else {
			back = 0;
		}
		free(resets);
	}
	time_t start = end - TW_CLAUDE_SEVEN_DAYS;
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
	if (back == 0) {
		draw_behind(cr, st, g, now_x);
	}
	draw_series(cr, g, start, end, ts, vs, n, color, 0x48);
	// on at the pace of the week so far: dashed, to its end or to where it runs out
	// (since the week began, or since it was reset by hand: as the flyout's text says)
	double pace = back == 0 && c->seven_day.known ? tw_claude_window_pace(&c->seven_day,
		TW_CLAUDE_SEVEN_DAYS, c->now, used) : 0;
	if (back == 0 && c->now > start) {
		draw_pace(cr, st, g, start, end, c->now, end, used, pace, 0);
	}
	if (back == 0 && st->resets && c->five_hour.resets_at > c->now &&
			c->five_hour.resets_at < end) {
		char label[48];
		reset_words(c->five_hour.resets_at, c->now, "5h", label, sizeof(label));
		draw_reset(cr, st, g, chart_x(g, start, end, c->five_hour.resets_at), color, label);
	}
	if (back == 0) {
		draw_now(cr, st, g, now_x, used, color);
	}
	// what now is, by the line: on its left near the end, else on its right; an older
	// week what it came to, at its end
	char label[48];
	snprintf(label, sizeof(label), back == 0 ? "now %.0f%%" : "came to %.0f%%", used);
	int tw = 0;
	pd_text_size(cr, st->font, label, &tw, NULL);
	double lx = back > 0 ? g.x + g.width - 6 - tw :
		now_x + 6 + tw > g.x + g.width - 4 ? now_x - 6 - tw : now_x + 6;
	// over the "100%" of the scale (a week that only starts now): a line lower
	int scale_w = 0;
	pd_text_size(cr, st->font, "100%", &scale_w, NULL);
	double ly = lx < g.x + 4 + scale_w + 4 ? g.y + 18 : g.y + 2;
	bool hovering = f->inside && pbox_contains(&g, f->px, f->py);
	if (!hovering) { // (the reading under the pointer says it then)
		pd_text(cr, st->font, label, lx, ly, tw + 2, 16, st->fg, PD_LEFT);
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


/* ---------- every account in one chart, or one along the clock ---------- */

uint32_t claude_agent_color(int agent, int nth) {
	// each agent its color, a second account of it a lighter or darker one
	static const uint32_t base[][3] = {
		{ 0xd97757ff, 0xf0a283ff, 0xa8492eff }, // Claude Code
		{ 0x10a37fff, 0x5fd3b0ff, 0x0b6e56ff }, // Codex
		{ 0x8957e5ff, 0xb894f5ff, 0x5f33b0ff }, // Copilot
		{ 0x4285f4ff, 0x8ab4f8ff, 0x1a5bc4ff }, // Antigravity
	};
	int n = (int)(sizeof(base) / sizeof(base[0]));
	int a = agent >= 0 && agent < n ? agent : (agent < 0 ? 0 : agent % n);
	return base[a][(nth < 0 ? 0 : nth) % 3];
}

static time_t length_of(bool week) {
	return week ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
}

/* Along the clock: the day (5-hour limits) or the week from Monday, back as many before now. */
static void clock_span(bool week, int back, time_t now, time_t *start, time_t *end) {
	struct tm tm;
	localtime_r(&now, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	if (week) {
		tm.tm_mday -= (tm.tm_wday + 6) % 7; // back to Monday
	}
	tm.tm_mday -= back * (week ? 7 : 1);
	struct tm a = tm, b = tm;
	*start = mktime(&a);
	b.tm_mday += week ? 7 : 1;
	b.tm_isdst = -1;
	*end = mktime(&b);
}

/* The reset of the window of a line, back windows before its newest; 0 for none. */
static time_t line_window(const struct claude_charts *c, bool week, int back) {
	time_t *resets;
	int all = windows_of(c, week, &resets);
	time_t reset = back >= 0 && back < all ? resets[all - 1 - back] : 0;
	free(resets);
	return reset;
}

static time_t oldest_of(const struct claude_line *lines, int n) {
	time_t oldest = 0;
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < lines[i].c->count; k++) {
			time_t at = lines[i].c->points[k].at;
			if (at > 0 && (oldest == 0 || at < oldest)) {
				oldest = at;
			}
		}
	}
	return oldest;
}

int claude_lines_back_max(const struct claude_line *lines, int n, bool week,
		enum claude_scale scale, time_t now) {
	int most = 0;
	if (scale == CLAUDE_SCALE_RELATIVE) {
		for (int i = 0; i < n; i++) {
			time_t *resets;
			int all = windows_of(lines[i].c, week, &resets);
			free(resets);
			most = all - 1 > most ? all - 1 : most;
		}
		return most;
	}
	time_t oldest = oldest_of(lines, n);
	for (int back = 1; oldest > 0 && back < 4000; back++) {
		time_t start, end;
		clock_span(week, back, now, &start, &end);
		if (end <= oldest) {
			break;
		}
		most = back;
	}
	return most;
}

void claude_lines_span(bool week, enum claude_scale scale, int back, time_t now, char *out,
		size_t size) {
	const char *what = week ? "Weekly limits" : "5-hour limits";
	if (scale == CLAUDE_SCALE_RELATIVE) {
		if (back <= 0) {
			snprintf(out, size, "%s, from start to reset", what);
		} else {
			snprintf(out, size, "%s, %d %s before", what, back, week ? (back == 1 ? "week" :
				"weeks") : (back == 1 ? "window" : "windows"));
		}
		return;
	}
	time_t start, end;
	clock_span(week, back, now, &start, &end);
	char a[32], b[32];
	struct tm tm;
	localtime_r(&start, &tm);
	strftime(a, sizeof(a), "%a %e %b", &tm);
	if (!week) {
		snprintf(out, size, back == 0 ? "%s today" : back == 1 ? "%s yesterday" : "%s, %s",
			what, a);
		return;
	}
	time_t last = end - 12 * 3600; // (Sunday, whatever a change of the clock did)
	localtime_r(&last, &tm);
	strftime(b, sizeof(b), "%a %e %b", &tm);
	snprintf(out, size, back == 0 ? "%s this week, %s – %s" : "%s, %s – %s", what, a, b);
}

/* What a line had used at a time, or where its pace leads; below 0 when nothing. */
static double line_value(const struct claude_charts *c, bool week, time_t t, bool *ahead) {
	time_t length = length_of(week);
	time_t *resets;
	int all = windows_of(c, week, &resets);
	double value = -1;
	*ahead = false;
	for (int i = all - 1; i >= 0 && value < 0; i--) {
		time_t reset = resets[i];
		if (t < reset - length || t > reset) {
			continue;
		}
		time_t ts[512];
		double vs[512];
		int n = window_series(c, week, reset, length, ts, vs, 512);
		double lo, hi;
		if (t <= c->now && range_at(ts, vs, n, t, &lo, &hi)) {
			value = hi;
		} else if (t > c->now && n > 0) {
			const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
			double pace = cur->known && tw_claude_same_window(cur->resets_at, reset) ?
				tw_claude_window_pace(cur, length, c->now, vs[n - 1]) : 0;
			if (pace > 0) {
				value = fmin(vs[n - 1] + pace * (double)(t - c->now), 100);
				*ahead = true;
			}
		}
	}
	free(resets);
	return value;
}

/* A window of a line, from start to end of the chart: its usage, now and the pace on. */
static void draw_line_window(cairo_t *cr, const struct claude_look *st, struct pbox g,
		const struct claude_line *line, bool week, time_t reset, time_t start, time_t end,
		uint32_t fill, double *now_x, double *now_y) {
	const struct claude_charts *c = line->c;
	time_t length = length_of(week);
	time_t ts[512];
	double vs[512];
	int n = window_series(c, week, reset, length, ts, vs, 512);
	draw_series(cr, g, start, end, ts, vs, n, line->color, fill);
	time_t over = window_end(c, week, reset);
	const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
	if (n > 0 && c->now >= reset - length && c->now < over) {
		double used = vs[n - 1];
		if (cur->known && tw_claude_same_window(cur->resets_at, reset)) {
			draw_pace(cr, st, g, start, end, c->now, over, used,
				tw_claude_window_pace(cur, length, c->now, used), line->color);
		}
		*now_x = chart_x(g, start, end, c->now);
		*now_y = chart_y(g, used);
	}
}

void claude_chart_lines(const struct claude_hover *f, const struct claude_line *lines, int n,
		bool week, enum claude_scale scale, int back, time_t now, cairo_t *cr,
		const struct claude_look *st, struct pbox box) {
	struct pbox g = { box.x, box.y, box.width, box.height - 20 };
	chart_bg(cr, st, g);
	bool relative = scale == CLAUDE_SCALE_RELATIVE;
	time_t length = length_of(week), start = 0, end = length;
	if (!relative) {
		clock_span(week, back, now, &start, &end);
	}
	// the scale: the days of the week or the hours of the day; else the share of each window
	if (relative) {
		int steps = week ? 7 : 5;
		for (int i = 0; i < steps; i++) {
			double x0 = g.x + g.width * i / steps, x1 = g.x + g.width * (i + 1) / steps;
			if (i > 0) {
				pd_rect(cr, round(x0), g.y, 1, g.height, st->chart_grid);
			}
			char label[24];
			snprintf(label, sizeof(label), week ? "day %d" : "hour %d", i + 1);
			if (x1 - x0 > 30) {
				pd_text(cr, st->font, label, x0, g.y + g.height + 2, x1 - x0, 18, st->dim,
					PD_CENTER);
			}
		}
	} else if (week) {
		struct tm tm;
		localtime_r(&start, &tm);
		tm.tm_isdst = -1;
		for (int d = 0; d < 7; d++) {
			struct tm day = tm, next = tm;
			day.tm_mday += d;
			next.tm_mday += d + 1;
			time_t t0 = mktime(&day), t1 = mktime(&next);
			double x0 = chart_x(g, start, end, t0), x1 = chart_x(g, start, end, t1);
			if (d > 0) {
				pd_rect(cr, round(x0), g.y, 1, g.height, st->chart_grid);
			}
			char name[16];
			strftime(name, sizeof(name), x1 - x0 > 60 ? "%a %e" : "%a", &day);
			if (x1 - x0 > 18) {
				pd_text(cr, st->font, name, x0, g.y + g.height + 2, x1 - x0, 18, st->dim,
					PD_CENTER);
			}
		}
	} else {
		struct tm tm;
		localtime_r(&start, &tm);
		tm.tm_isdst = -1;
		int every = g.width >= 360 ? 3 : 6;
		for (int h = 1; h < 24; h++) {
			struct tm at = tm;
			at.tm_hour += h;
			time_t t = mktime(&at);
			double x = chart_x(g, start, end, t);
			bool labelled = h % every == 0;
			pd_rect(cr, round(x), g.y, 1, g.height, labelled ? st->chart_grid :
				(st->chart_grid & 0xffffff00) | ((st->chart_grid & 0xff) / 2));
			if (labelled) {
				char label[16];
				strftime(label, sizeof(label), "%H:%M", &at);
				pd_text(cr, st->font, label, x - 30, g.y + g.height + 2, 60, 18, st->dim,
					PD_CENTER);
			}
		}
	}
	pd_rect(cr, g.x, g.y + g.height / 2, g.width, 1, st->chart_grid);
	pd_text(cr, st->font, "100%", g.x + 4, g.y + 1, 60, 16, st->dim, PD_LEFT);
	pd_text(cr, st->font, "50%", g.x + 4, g.y + g.height / 2 + 1, 60, 16, st->dim, PD_LEFT);
	bool now_in = !relative && now >= start && now < end;
	if (now_in) {
		draw_behind(cr, st, g, chart_x(g, start, end, now));
	}
	// the lines, clipped to the chart: windows that began before it or go on after it
	double xs[16], ys[16];
	int dots = 0;
	uint32_t fill = n > 1 ? 0x1c : 0x48;
	cairo_save(cr);
	cairo_rectangle(cr, g.x, g.y, g.width, g.height);
	cairo_clip(cr);
	chart_reach = -1;
	for (int i = 0; i < n && i < 16; i++) {
		const struct claude_charts *c = lines[i].c;
		double nx = -1, ny = -1;
		if (relative) {
			time_t reset = line_window(c, week, back);
			if (reset > 0) {
				draw_line_window(cr, st, g, &lines[i], week, reset, reset - length, reset,
					fill, &nx, &ny);
			}
		} else {
			time_t *resets;
			int all = windows_of(c, week, &resets);
			for (int k = 0; k < all; k++) {
				if (resets[k] - length < end && window_end(c, week, resets[k]) > start) {
					draw_line_window(cr, st, g, &lines[i], week, resets[k], start, end, fill,
						&nx, &ny);
				}
			}
			free(resets);
		}
		xs[i] = nx;
		ys[i] = ny;
		dots = i + 1;
	}
	chart_reach = 0;
	cairo_restore(cr);
	// along the clock: where each line's limit resets next, dashed in its color (lines
	// at the same place taking turns in their dashes); after the chart, at its edge
	int later = 0;
	for (int i = 0; !relative && i < n && i < 16; i++) {
		const struct claude_charts *c = lines[i].c;
		const struct tw_claude_window *cur = week ? &c->seven_day : &c->five_hour;
		if (cur->resets_at <= c->now || cur->resets_at <= start) {
			continue;
		}
		if (cur->resets_at >= end) {
			draw_reset_later(cr, st, g, cur->resets_at, week, lines[i].color, later++);
			continue;
		}
		double x = round(chart_x(g, start, end, cur->resets_at));
		int at = 0, same = 0;
		for (int k = 0; k < n && k < 16; k++) {
			const struct claude_charts *o = lines[k].c;
			const struct tw_claude_window *w = week ? &o->seven_day : &o->five_hour;
			if (w->resets_at > o->now && w->resets_at > start && w->resets_at < end &&
					fabs(round(chart_x(g, start, end, w->resets_at)) - x) < 2) {
				at += k < i;
				same++;
			}
		}
		draw_reset_shared(cr, st, g, x, lines[i].color, NULL, at, same);
	}
	// now: across the chart along the clock; else where each line is in its window
	if (now_in) {
		pd_rect(cr, round(chart_x(g, start, end, now)) - 0.5, g.y, 1, g.height, st->fg);
	}
	for (int i = 0; i < dots; i++) {
		if (xs[i] < g.x || xs[i] > g.x + g.width || ys[i] < 0) {
			continue;
		}
		if (relative) {
			pd_rect(cr, round(xs[i]) - 0.5, g.y, 1, g.height, (lines[i].color & 0xffffff00) | 0x90);
		}
		cairo_new_path(cr);
		cairo_arc(cr, xs[i], ys[i], 3.5, 0, 2 * M_PI);
		pd_color(cr, lines[i].color);
		cairo_fill_preserve(cr);
		pd_color(cr, st->fg);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	// under the pointer: the time, and of each line what it had used (or where it goes)
	if (f->inside && pbox_contains(&g, f->px, f->py)) {
		double share = g.width > 0 ? (f->px - g.x) / g.width : 0;
		share = share < 0 ? 0 : share > 1 ? 1 : share;
		char text[600];
		int len;
		if (relative) {
			long into = (long)((double)length * share);
			char span[48];
			tw_claude_format_span(into, false, span, sizeof(span));
			len = snprintf(text, sizeof(text), "%s into the window", into < 60 ? "0 min" : span);
		} else {
			time_t t = time_at(g, start, end, f->px);
			char when[32];
			when_words(t, week, when, sizeof(when));
			len = snprintf(text, sizeof(text), "%s", when);
		}
		double hy[16];
		uint32_t hc[16];
		int k = 0;
		for (int i = 0; i < n && i < 16 && len < (int)sizeof(text) - 1; i++) {
			const struct claude_charts *c = lines[i].c;
			time_t t;
			if (relative) {
				time_t reset = line_window(c, week, back);
				if (reset <= 0) {
					continue;
				}
				t = reset - length + (time_t)((double)length * share);
			} else {
				t = time_at(g, start, end, f->px);
			}
			bool ahead;
			double v = line_value(c, week, t, &ahead);
			hy[k] = v >= 0 ? chart_y(g, v) : -1;
			hc[k++] = lines[i].color;
			if (v >= 0) {
				len += snprintf(text + len, sizeof(text) - len, "\n%s %s%.0f%%", lines[i].name,
					ahead ? "at this pace " : "", v);
			} else {
				len += snprintf(text + len, sizeof(text) - len, "\n%s –", lines[i].name);
			}
		}
		pd_chart_hover(cr, st->font, g.x, g.y, g.width, g.height, f->px, hy, hc, k, text,
			st->fg, st->label_bg, st->dim);
	}
}

double claude_lines_legend(cairo_t *cr, const struct claude_look *st,
		const struct claude_line *lines, int n, time_t now, double x, double y, double width,
		bool draw) {
	double cx = x, row = 18, height = 0;
	for (int i = 0; i < n; i++) {
		const struct claude_charts *c = lines[i].c;
		char text[128];
		double five = tw_claude_window_now(&c->five_hour, now);
		double week = tw_claude_window_now(&c->seven_day, now);
		char a[16] = "–", b[16] = "–"; // (a limit it does not have: Argon's 5 hours)
		if (five >= 0) {
			snprintf(a, sizeof(a), "%.0f%%", five);
		}
		if (week >= 0) {
			snprintf(b, sizeof(b), "%.0f%%", week);
		}
		if (five >= 0 || week >= 0) {
			snprintf(text, sizeof(text), "%s %s · %s", lines[i].name, a, b);
		} else {
			snprintf(text, sizeof(text), "%s", lines[i].name);
		}
		int tw = 0;
		pd_text_size(cr, st->font, text, &tw, NULL);
		double w = 14 + tw + 14;
		if (cx > x && cx + w > x + width) {
			cx = x;
			height += row;
		}
		if (draw) {
			cairo_new_path(cr);
			cairo_arc(cr, cx + 5, y + height + row / 2, 4, 0, 2 * M_PI);
			pd_color(cr, lines[i].color);
			cairo_fill(cr);
			pd_text(cr, st->font, text, cx + 14, y + height, tw + 2, row, st->fg, PD_LEFT);
		}
		cx += w;
	}
	return n > 0 ? height + row : 0;
}
