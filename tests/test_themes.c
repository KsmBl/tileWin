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
	if (!names || names->length < 11) {
		fail("themes", "fewer built-in themes than there are", NULL);
	}
	for (int i = 0; names && i < names->length; i++) {
		struct tw_theme *t = load(names->items[i]);
		if (t && (!t->title || !t->style)) {
			fail(t->name, "has no name or style", NULL);
		}
		tw_theme_free(t);
	}
	// in the order they came out: Windows 1 and the Atari ST in 1985, then Windows 3
	if (names && names->length > 2 && (strcmp(names->items[0], "win1") != 0 ||
			strcmp(names->items[1], "atari") != 0 || strcmp(names->items[2], "win3") != 0)) {
		fail("themes", "are not in the order they came out", names->items[0]);
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

	// the Atari ST: the green desktop, the menu bar along the top, black lines on
	// white (the bevels of the classic look are one black line), its own icons,
	// turned round in the dark scheme
	struct tw_theme *st = load("atari");
	if (st) {
		if (!st->style || strcmp(st->style, "atari") != 0) {
			fail("atari", "does not draw in the style of GEM", st->style);
		}
		check_color(st, "wallpaper.color", 0x00c000ff);
		check_str(st, "layout.window.position", "top");
		check_str(st, "start.label", "Desk");
		check_color(st, "decoration.highlight", 0x000000ff);
		check_color(st, "decoration.dark", 0x000000ff);
		check_color(st, "decoration.light", 0xffffffff);
		check_color(st, "menu.hl_bg", 0x000000ff);
		check_str(st, "icons.set", "atari");
		char *icons = tw_theme_icon_dir(st);
		if (!icons || !strstr(icons, "themes/atari/icons")) {
			fail("atari", "has no icons of its own", icons);
		}
		free(icons);
		tw_theme_free(st);
	}

	// Starship Console: always dark (its colours also in the dark block, so the
	// dark ones of Windows 10, which it inherits, never show), black, orange
	// frames, its own blocks for the start menu and its own icons
	struct tw_theme *sc = load("console");
	if (sc) {
		if (!sc->style || strcmp(sc->style, "console") != 0) {
			fail("console", "does not draw in the console style", sc->style);
		}
		check_str(sc, "scheme", "dark");
		check_color(sc, "wallpaper.color", 0x000000ff);
		check_color(sc, "decoration.active.title_bg", 0xff9c3aff);
		check_color(sc, "menu.bg", 0x000000ff);
		check_str(sc, "startmenu.layout", "blocks");
		check_str(sc, "icons.set", "console");
		tw_theme_free(sc);
	}

	// Windows 3.1 came without a wallpaper: the light grey of "Windows Default"
	struct tw_theme *t = load("win3");
	if (t) {
		check_str(t, "wallpaper.type", "solid");
		check_color(t, "wallpaper.color", 0xc0c0c0ff);
		check_color(t, "decoration.active.title_bg", 0x000080ff);
		tw_theme_free(t);
	}

	// Windows 2000: the warmer grey, the navy to light blue titles, the blue desktop
	struct tw_theme *w2k = load("win2000");
	if (w2k) {
		check_color(w2k, "decoration.face", 0xd4d0c8ff);
		check_color(w2k, "wallpaper.color", 0x3a6ea5ff);
		check_str(w2k, "decoration.active.title_gradient", "0:#0a246a 1:#a6caf0");
		tw_theme_free(w2k);
	}
	// Vista: Windows 7's glass with a taskbar of black glass and window titles on it
	struct tw_theme *vista = load("vista");
	if (vista) {
		check_str(vista, "taskbar.icons_only", "no");
		check_str(vista, "start.style", "orb");
		if (!vista->style || strcmp(vista->style, "win7") != 0) {
			fail("vista", "is not drawn with the glass of Windows 7", vista->style);
		}
		tw_theme_free(vista);
	}

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all themes load and look as they should\n");
	return 0;
}
