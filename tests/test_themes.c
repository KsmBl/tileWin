/*
 * The built-in themes: each loads, and the old Windows look as it was, down to
 * the colors of the original. Run with TILEWIN_DATADIR at the source tree.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_theme.h"

static int failures;

static void fail(const char *theme, const char *what, const char *detail) {
	fprintf(stderr, "FAIL: %s: %s (%s)\n", theme, what, detail ? detail : "");
	failures++;
}

static void check_color(const struct tw_theme *t, const char *key, uint32_t want) {
	uint32_t got = tw_theme_color(t, key, 0x12345600);
	if (got != want) {
		char detail[64];
		snprintf(detail, sizeof(detail), "got #%08x, wanted #%08x", got, want);
		fail(t->name, key, detail);
	}
}

static void check_str(const struct tw_theme *t, const char *key, const char *want) {
	const char *got = tw_theme_str(t, key, NULL);
	if (!got || strcmp(got, want) != 0) {
		char detail[128];
		snprintf(detail, sizeof(detail), "got \"%s\", wanted \"%s\"", got ? got : "(none)", want);
		fail(t->name, key, detail);
	}
}

static struct tw_theme *load(const char *name) {
	char *error = NULL;
	struct tw_theme *t = tw_theme_load(name, &error);
	if (!t) {
		fail(name, "does not load", error);
	}
	free(error);
	return t;
}

int main(void) {
	// every theme there is loads, with a name and a style
	list_t *names = tw_theme_list();
	if (!names || names->length < 9) {
		fail("themes", "fewer built-in themes than there are", NULL);
	}
	for (int i = 0; names && i < names->length; i++) {
		struct tw_theme *t = load(names->items[i]);
		if (t && (!t->title || !t->style)) {
			fail(t->name, "has no name or style", NULL);
		}
		tw_theme_free(t);
	}
	// in the order Windows came out
	if (names && names->length > 1 && (strcmp(names->items[0], "win1") != 0 ||
			strcmp(names->items[1], "win3") != 0)) {
		fail("themes", "are not in the order Windows came out", names->items[0]);
	}
	if (names) {
		list_free_items_and_destroy(names);
	}

	// Windows 1 on EGA: bright blue title bars, yellow menus, the cyan icon area
	struct tw_theme *w1 = load("win1");
	if (w1) {
		check_str(w1, "decoration.active.title_bg", "#5555ff");
		check_color(w1, "wallpaper.color", 0x55ffffff);
		check_color(w1, "panel.bg", 0x55ffffff);
		check_color(w1, "menu.bg", 0xffff55ff);
		check_color(w1, "decoration.title_box", 0x000000ff);
		if (!w1->style || strcmp(w1->style, "win1") != 0) {
			fail("win1", "does not draw its windows in the style of Windows 1", w1->style);
		}
		tw_theme_free(w1);
	}

	// Windows 3.1 came without a wallpaper: the light grey of "Windows Default"
	struct tw_theme *t = load("win3");
	if (t) {
		check_str(t, "wallpaper.type", "solid");
		check_color(t, "wallpaper.color", 0xc0c0c0ff);
		check_color(t, "decoration.active.title_bg", 0x000080ff);
		tw_theme_free(t);
	}

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all themes load and look as they should\n");
	return 0;
}
