/*
 * The widget list (common/tw_widgets.c) as the desktop sees it: every widget
 * that can go on the desktop has styles, the first of them the taskbar look,
 * and each style is one the panel knows how to draw (gadgets.c draws the
 * others, deskwidgets.c "compact" and "tile"). A style added to the list and
 * forgotten in the panel would show as the default without a word.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_widgets.h"

static int failures;

static void check(bool ok, const char *what, const char *widget) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s: %s\n", widget, what);
		failures++;
	}
}

/* The styles the panel draws. */
static const char *const drawn[] = {
	"compact", "tile", "chart", "gauge", "ring", "bar", "digital", "analog", "binary", NULL,
};

static bool is_drawn(const char *name) {
	for (int i = 0; drawn[i]; i++) {
		if (strcmp(drawn[i], name) == 0) {
			return true;
		}
	}
	return false;
}

int main(void) {
	for (size_t i = 0; i < tw_widget_count; i++) {
		const struct tw_widget_info *info = &tw_widgets[i];
		if (!(info->flags & TW_WIDGET_DESKTOP)) {
			check(info->styles == NULL, "has styles but cannot go on the desktop", info->type);
			continue;
		}
		check(info->styles != NULL, "can go on the desktop but has no style", info->type);
		if (!info->styles) {
			continue;
		}
		check(strcmp(info->styles[0].name, "compact") == 0,
			"the default style is not the taskbar look", info->type);
		for (const struct tw_widget_style *style = info->styles; style->name; style++) {
			check(style->title && *style->title, "a style without a title", info->type);
			check(is_drawn(style->name), "a style the panel does not draw", info->type);
			check(style->rows >= 1 && style->columns >= 0, "a style without cells", info->type);
			for (const struct tw_widget_style *other = style + 1; other->name; other++) {
				check(strcmp(style->name, other->name) != 0, "a style twice", info->type);
			}
			check(tw_widget_style_find(info, style->name) == style, "a style not found",
				info->type);
		}
		check(tw_widget_style_find(info, NULL) == info->styles,
			"no style does not give the default", info->type);
		check(tw_widget_style_find(info, "no-such-style") == info->styles,
			"an unknown style does not give the default", info->type);
	}

	// the clocks are the only ones with clock faces, and every meter has a chart
	const struct tw_widget_info *clock = tw_widget_find("clock:digital");
	check(clock && strcmp(clock->type, "clock") == 0, "not found by its name", "clock:digital");
	check(clock && strcmp(tw_widget_style_find(clock, "analog")->name, "analog") == 0,
		"has no analog style", "clock");
	check(strcmp(tw_widget_style_find(tw_widget_find("cpu"), "analog")->name, "compact") == 0,
		"has a clock face", "cpu");
	static const char *const meters[] = { "cpu", "memory", "gpu", "net", "storage", "power" };
	for (size_t i = 0; i < sizeof(meters) / sizeof(meters[0]); i++) {
		const struct tw_widget_info *info = tw_widget_find(meters[i]);
		check(info && strcmp(tw_widget_style_find(info, "chart")->name, "chart") == 0,
			"has no chart", meters[i]);
	}

	// where a widget sits on the desktop is described once, for the settings too
	static const char *const place[] = { "style", "column", "row", "size", "output" };
	for (size_t i = 0; i < sizeof(place) / sizeof(place[0]); i++) {
		bool found = false;
		for (const struct tw_widget_option *opt = tw_widget_desktop_options; opt->key; opt++) {
			found = found || strcmp(opt->key, place[i]) == 0;
		}
		check(found, "is not a desktop option", place[i]);
	}

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("widgets: every desktop style is drawn\n");
	return 0;
}
