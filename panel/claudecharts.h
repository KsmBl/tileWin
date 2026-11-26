#ifndef _TW_CLAUDECHARTS_H
#define _TW_CLAUDECHARTS_H
#include <cairo.h>
#include <stdbool.h>
#include <time.h>
#include "panel.h"
#include "tw_claude_usage.h"

/*
 * The charts of the Claude Code usage: the last three 5-hour windows along the
 * hours, and the week from its start to its reset with the line of now and the
 * pace ahead. The widget's flyout and its desktop styles draw them, each in
 * its colors.
 */
struct claude_charts {
	struct tw_claude_point *points;
	int count;
	struct tw_claude_window five_hour, seven_day; // as of now
	time_t now;
	bool have;
	// how many windows back each chart shows (0: the newest), scrolled in the flyout
	int back_five, back_week;
	struct pbox box_five, box_week; // where each was drawn last, for the scrolling
};

struct claude_look {
	uint32_t fg, dim, chart_bg, chart_grid, chart_line, label_bg;
	double chart_radius;
	bool dark;    // a dark background: the warning colors lighter
	bool sunken;  // the chart in a sunken edge (Windows 95)
	bool resets;  // a dashed line where a limit resets next (the charts on the desktop)
	const char *font, *bold;
};

/* Where the pointer is, for the reading under it. */
struct claude_hover {
	bool inside;
	double px, py;
};

void claude_chart_surface(cairo_t *cr, const struct claude_look *st, struct pbox box);
uint32_t claude_desktop_chart_bg(const struct gadget_palette *pal);
void claude_chart_five_hours(const struct claude_hover *f, const struct claude_charts *c,
	cairo_t *cr, const struct claude_look *st, struct pbox box);
void claude_chart_week(const struct claude_hover *f, const struct claude_charts *c,
	cairo_t *cr, const struct claude_look *st, struct pbox box);
/*
 * How many windows back a chart can go: older 5-hour windows (three are shown)
 * or older weeks in the history.
 */
int claude_charts_back_max(const struct claude_charts *c, bool week);
/* "Mon 3 Nov – Mon 10 Nov": the time a chart shows, as it is scrolled. */
void claude_charts_span(const struct claude_charts *c, bool week, char *out, size_t size);
/* Reads the history and the values of now into c (its points freed first). */
void claude_charts_load(struct claude_charts *c, const struct tw_claude_usage *u, time_t now);
/* The same, of readings in hand (Antigravity's, from agentusage.c), copied. */
void claude_charts_set(struct claude_charts *c, const struct tw_claude_point *points, int count,
	const struct tw_claude_usage *u, time_t now);

/*
 * How the charts lay out time: as the limits run (the last three 5-hour windows
 * along the hours, the week from its start to its reset), along the clock (a
 * day from midnight, a week from Monday, wherever the limits reset), or each
 * line from the start of its window to its reset (one account at day 2 of its
 * week, another at day 6).
 */
enum claude_scale {
	CLAUDE_SCALE_WINDOWS,
	CLAUDE_SCALE_ABSOLUTE,
	CLAUDE_SCALE_RELATIVE,
};

/* An account in a chart of several: its readings, its name and its color. */
struct claude_line {
	const struct claude_charts *c;
	const char *name;
	uint32_t color;
};

/* The color of an agent (enum tw_agent), for its nth account of that agent. */
uint32_t claude_agent_color(int agent, int nth);
/*
 * The 5-hour (week false) or the weekly limits of the lines in one chart, along
 * the clock or each in its window (scale not CLAUDE_SCALE_WINDOWS); back days
 * or weeks before now along the clock, windows before the newest else.
 */
void claude_chart_lines(const struct claude_hover *f, const struct claude_line *lines, int n,
	bool week, enum claude_scale scale, int back, time_t now, cairo_t *cr,
	const struct claude_look *st, struct pbox box);
int claude_lines_back_max(const struct claude_line *lines, int n, bool week,
	enum claude_scale scale, time_t now);
/* "Weekly limits this week, Mon 23 Nov – Sun 29 Nov": the title of such a chart. */
void claude_lines_span(bool week, enum claude_scale scale, int back, time_t now, char *out,
	size_t size);
/*
 * A dot of each line's color with its name and its two limits used now, in
 * rows of width; how high. With draw false it is only measured.
 */
double claude_lines_legend(cairo_t *cr, const struct claude_look *st,
	const struct claude_line *lines, int n, time_t now, double x, double y, double width,
	bool draw);

#endif
