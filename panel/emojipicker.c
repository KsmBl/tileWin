#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "draw.h"
#include "emoji.h"
#include "flyout.h"
#include "panel.h"
#include "popup.h"
#include "textfield.h"
#include "tw_paths.h"

/*
 * The emoji picker of Windows (Win+. or Win+;): the emoji of Unicode in their
 * groups and the ones used lately, then a tab of math symbols and one of text
 * emoticons (:-) and the like, in wider cells), all searched by name as soon
 * as one types. A
 * click or Enter types the emoji into the window that had the keyboard, by
 * way of the clipboard, which gets back what it held (clipboard.c); with Shift
 * the picker stays open for more. It takes the colors of the flyouts.
 */

#define COLUMNS 9
#define CELL 40
#define PAD 12
#define SEARCH_H 34
#define TABS_H 36
#define FOOTER_H 28
#define ROWS 7
#define RECENT_MAX 36
#define ASCII_COLUMNS 3
// the tabs: the recent ones, the groups of emoji_groups, math, text emoticons
#define TAB_RECENT 0
#define TAB_MATH (emoji_group_count + 1)
#define TAB_ASCII (emoji_group_count + 2)
#define TAB_COUNT (emoji_group_count + 3)

static const char *const tab_icons[] = {
	"\xf0\x9f\x95\x92", // the clock of "recent"
	"\xf0\x9f\x98\x80", "\xf0\x9f\x91\x8b", "\xf0\x9f\x90\xb6", "\xf0\x9f\x8d\x94",
	"\xf0\x9f\x9a\x97", "\xe2\x9a\xbd", "\xf0\x9f\x92\xa1", "\xe2\x9d\xa4\xef\xb8\x8f",
	"\xf0\x9f\x8f\x81", // math and the text emoticons are drawn as text
};

struct picker {
	struct panel *panel;
	struct popup *popup;
	char search[128];
	struct text_cursor tc;
	int tab;
	int scroll;   // first row shown
	int selected; // index into shown
	int hover;    // index into shown, -1 for none
	int hover_tab;
	list_t *shown; // const struct emoji *, borrowed
	struct pbox grid, tabs;
	double px, py;
	bool inside;
};

static struct picker *current;

/* Whether it is text rather than an emoji: drawn in the text color. */
static bool is_text(const struct emoji *e) {
	return (e >= math_symbols && e < math_symbols + math_symbol_count) ||
		(e >= ascii_emoticons && e < ascii_emoticons + ascii_emoticon_count);
}

/* The text emoticons are wide: their tab has fewer, wider cells. */
static int columns(const struct picker *pk) {
	return !pk->search[0] && pk->tab == TAB_ASCII ? ASCII_COLUMNS : COLUMNS;
}

static double cell_w(const struct picker *pk) {
	return (double)COLUMNS * CELL / columns(pk);
}
static list_t *recent; // char *, newest first

/* ---------- the recent ones ---------- */

static char *recent_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = malloc(strlen(dir) + 32);
	sprintf(path, "%s/emoji-recent", dir);
	free(dir);
	return path;
}

static void recent_load(void) {
	if (recent) {
		return;
	}
	recent = create_list();
	char *path = recent_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	char line[64];
	while (f && fgets(line, sizeof(line), f) && recent->length < RECENT_MAX) {
		line[strcspn(line, "\n")] = '\0';
		if (*line) {
			list_add(recent, strdup(line));
		}
	}
	if (f) {
		fclose(f);
	}
	free(path);
}

static void recent_use(const char *chars) {
	recent_load();
	for (int i = 0; i < recent->length; i++) {
		if (strcmp(recent->items[i], chars) == 0) {
			free(recent->items[i]);
			list_del(recent, i);
			break;
		}
	}
	list_insert(recent, 0, strdup(chars));
	while (recent->length > RECENT_MAX) {
		free(recent->items[recent->length - 1]);
		list_del(recent, recent->length - 1);
	}
	char *path = recent_path();
	if (path) {
		char *dir = tw_state_dir();
		if (dir) {
			tw_mkdir_p(dir);
		}
		free(dir);
		FILE *f = fopen(path, "w");
		for (int i = 0; f && i < recent->length; i++) {
			fprintf(f, "%s\n", (char *)recent->items[i]);
		}
		if (f) {
			fclose(f);
		}
	}
	free(path);
}

static const struct {
	const struct emoji *list;
	const int *count;
} all_lists[] = {
	{ emoji_list, &emoji_count },
	{ math_symbols, &math_symbol_count },
	{ ascii_emoticons, &ascii_emoticon_count },
};

static const struct emoji *emoji_by_chars(const char *chars) {
	for (size_t l = 0; l < sizeof(all_lists) / sizeof(all_lists[0]); l++) {
		for (int i = 0; i < *all_lists[l].count; i++) {
			if (strcmp(all_lists[l].list[i].chars, chars) == 0) {
				return &all_lists[l].list[i];
			}
		}
	}
	return NULL;
}

/* ---------- what is shown ---------- */

/* Every word typed is in the name, as the start of one of its words. */
static bool matches(const char *name, const char *search) {
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", search);
	char *save = NULL;
	for (char *word = strtok_r(copy, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
		size_t len = strlen(word);
		bool found = false;
		for (const char *p = name; *p && !found; p++) {
			if ((p == name || p[-1] == ' ' || p[-1] == '-') && strncasecmp(p, word, len) == 0) {
				found = true;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

/*
 * Whether the fonts have every character of a text item: kaomoji like
 * ¯\_(ツ)_/¯ need a Japanese font, and boxes in their place help nobody.
 * Asked once for each.
 */
static bool can_draw(const struct emoji *e) {
	if (!is_text(e)) {
		return true;
	}
	static signed char *known[2];
	bool math = e >= math_symbols && e < math_symbols + math_symbol_count;
	int count = math ? math_symbol_count : ascii_emoticon_count;
	int index = (int)(e - (math ? math_symbols : ascii_emoticons));
	if (!known[math]) {
		known[math] = calloc(count, 1);
	}
	if (!known[math][index]) {
		PangoFontMap *map = pango_cairo_font_map_get_default();
		PangoContext *context = pango_font_map_create_context(map);
		PangoLayout *layout = pango_layout_new(context);
		PangoFontDescription *desc = pango_font_description_from_string("Sans 13");
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, e->chars, -1);
		known[math][index] = pango_layout_get_unknown_glyphs_count(layout) == 0 ? 1 : -1;
		g_object_unref(layout);
		g_object_unref(context);
	}
	return known[math][index] > 0;
}

static void fill_shown(struct picker *pk) {
	pk->shown->length = 0;
	if (pk->search[0]) {
		for (size_t l = 0; l < sizeof(all_lists) / sizeof(all_lists[0]); l++) {
			for (int i = 0; i < *all_lists[l].count; i++) {
				if (matches(all_lists[l].list[i].name, pk->search) &&
						can_draw(&all_lists[l].list[i])) {
					list_add(pk->shown, (void *)&all_lists[l].list[i]);
				}
			}
		}
	} else if (pk->tab == TAB_MATH || pk->tab == TAB_ASCII) {
		const struct emoji *list = pk->tab == TAB_MATH ? math_symbols : ascii_emoticons;
		int count = pk->tab == TAB_MATH ? math_symbol_count : ascii_emoticon_count;
		for (int i = 0; i < count; i++) {
			if (can_draw(&list[i])) {
				list_add(pk->shown, (void *)&list[i]);
			}
		}
	} else if (pk->tab == TAB_RECENT) {
		recent_load();
		for (int i = 0; i < recent->length; i++) {
			const struct emoji *e = emoji_by_chars(recent->items[i]);
			if (e) {
				list_add(pk->shown, (void *)e);
			}
		}
	} else {
		const struct emoji_group *g = &emoji_groups[pk->tab - 1];
		for (int i = 0; i < g->count; i++) {
			list_add(pk->shown, (void *)&emoji_list[g->first + i]);
		}
	}
	pk->scroll = 0;
	pk->selected = pk->shown->length > 0 ? 0 : -1;
	pk->hover = -1;
}

static void keep_selected_in_view(struct picker *pk) {
	int row = pk->selected / columns(pk);
	if (row < pk->scroll) {
		pk->scroll = row;
	} else if (row >= pk->scroll + ROWS) {
		pk->scroll = row - ROWS + 1;
	}
}

/* ---------- drawing ---------- */

/* Text in the color of the flyout, made smaller until it fits the width. */
static void draw_text_item(cairo_t *cr, const char *chars, double x, double y, double width,
		double size, uint32_t color) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	int w = 0, h = 0;
	for (; size >= 6; size -= 1) {
		char font[64];
		snprintf(font, sizeof(font), "Sans %.0f", size);
		PangoFontDescription *desc = pango_font_description_from_string(font);
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, chars, -1);
		pango_layout_get_pixel_size(layout, &w, &h);
		if (w <= width) {
			break;
		}
	}
	if (w > width) {
		pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_get_pixel_size(layout, &w, &h);
	}
	cairo_move_to(cr, x - w / 2.0, y - h / 2.0);
	pd_color(cr, color);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void draw_emoji(cairo_t *cr, const char *chars, double x, double y, double size) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	char font[64];
	snprintf(font, sizeof(font), "Noto Color Emoji, Twemoji, Segoe UI Emoji %.0f", size);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, chars, -1);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	cairo_move_to(cr, x - w / 2.0, y - h / 2.0);
	cairo_set_source_rgb(cr, 0, 0, 0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void render(struct popup *p, cairo_t *cr) {
	struct picker *pk = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	fly_draw_frame(p, cr, &st);
	int M = popup_shadow_margin(p->panel);
	double x0 = M + PAD, w0 = COLUMNS * CELL;
	double y = M + PAD;

	// the search, typed into without clicking it first
	cairo_new_path(cr);
	pd_rounded(cr, x0 + 0.5, y + 0.5, w0 - 1, SEARCH_H - 1, st.style == PS_CLASSIC ? 0 : 4);
	pd_color(cr, st.field_bg);
	cairo_fill_preserve(cr);
	pd_color(cr, st.style == PS_CLASSIC ? 0x808080ff : st.accent);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	pd_glyph_search(cr, x0 + 10, y + (SEARCH_H - 16) / 2.0, 16, st.dim);
	if (!pk->search[0]) {
		pd_text(cr, st.font, "Search emoji", x0 + 34, y, w0 - 44, SEARCH_H, st.dim, PD_LEFT);
	}
	struct text_style ts = { .font = st.font, .fg = st.field_fg, .caret = true };
	text_style_colors(p->panel, &ts);
	text_draw(cr, &ts, pk->search, &pk->tc, x0 + 34, y, w0 - 44, SEARCH_H);
	y += SEARCH_H + 6;

	// the tabs: recent, the groups, math and text emoticons
	int tabs = TAB_COUNT;
	double tw = w0 / tabs;
	pk->tabs = (struct pbox){ x0, y, w0, TABS_H };
	for (int i = 0; i < tabs; i++) {
		struct pbox b = { x0 + i * tw, y, tw, TABS_H };
		bool on = !pk->search[0] && pk->tab == i;
		if (on || (pk->inside && pk->hover_tab == i)) {
			fill_hover(cr, &st, b);
		}
		if (on) {
			pd_rect(cr, b.x + 6, b.y + b.height - 3, b.width - 12, 3, st.accent);
		}
		if (i == TAB_MATH || i == TAB_ASCII) {
			draw_text_item(cr, i == TAB_MATH ? "∑" : ":-)", b.x + b.width / 2,
				b.y + TABS_H / 2.0 - 1, b.width - 4, i == TAB_MATH ? 14 : 10, st.fg);
		} else {
			draw_emoji(cr, tab_icons[i < (int)(sizeof(tab_icons) / sizeof(tab_icons[0])) ?
				i : 1], b.x + b.width / 2, b.y + TABS_H / 2.0 - 1, 14);
		}
	}
	y += TABS_H + 4;

	// the grid
	pk->grid = (struct pbox){ x0, y, w0, ROWS * CELL };
	if (pk->shown->length == 0) {
		pd_text(cr, st.font, pk->search[0] ? "Nothing by that name" :
			"The emoji you use show up here", x0, y, w0, ROWS * CELL, st.dim, PD_CENTER);
	}
	int cols = columns(pk);
	double cw = cell_w(pk);
	for (int i = pk->scroll * cols; i < pk->shown->length && i < (pk->scroll + ROWS) * cols;
			i++) {
		const struct emoji *e = pk->shown->items[i];
		int row = i / cols - pk->scroll, col = i % cols;
		struct pbox b = { x0 + col * cw, y + row * CELL, cw, CELL };
		if (i == pk->selected || i == pk->hover) {
			fill_hover(cr, &st, b);
		}
		if (i == pk->selected) {
			cairo_new_path(cr);
			pd_rounded(cr, b.x + 1.5, b.y + 1.5, b.width - 3, b.height - 3, 4);
			pd_color(cr, st.accent);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		if (is_text(e)) {
			draw_text_item(cr, e->chars, b.x + cw / 2.0, b.y + CELL / 2.0, cw - 6,
				cols == COLUMNS ? 17 : 13, st.fg);
		} else {
			draw_emoji(cr, e->chars, b.x + CELL / 2.0, b.y + CELL / 2.0, 19);
		}
	}
	// a scroll bar when there is more
	int rows = (pk->shown->length + cols - 1) / cols;
	if (rows > ROWS) {
		double track = ROWS * CELL, thumb = track * ROWS / rows;
		double top = y + (track - thumb) * pk->scroll / (rows - ROWS);
		pd_rect(cr, x0 + w0 + 3, top, 3, thumb, st.dim);
	}
	y += ROWS * CELL + 4;

	// the name of the emoji pointed at, or else the selected one
	int named = pk->hover >= 0 ? pk->hover : pk->selected;
	if (named >= 0 && named < pk->shown->length) {
		const struct emoji *e = pk->shown->items[named];
		pd_text(cr, st.font, e->name, x0 + 4, y, w0 - 8, FOOTER_H, st.fg, PD_LEFT);
	}
}

/* ---------- input ---------- */

static int cell_at(struct picker *pk, double x, double y) {
	if (!pbox_contains(&pk->grid, x, y)) {
		return -1;
	}
	int cols = columns(pk);
	int col = (int)((x - pk->grid.x) / cell_w(pk)), row = (int)((y - pk->grid.y) / CELL);
	int i = (row + pk->scroll) * cols + col;
	return col < cols && i < pk->shown->length ? i : -1;
}

static void pick(struct picker *pk, int index, bool stay) {
	if (index < 0 || index >= pk->shown->length) {
		return;
	}
	const struct emoji *e = pk->shown->items[index];
	recent_use(e->chars);
	struct panel *panel = pk->panel;
	if (!stay) {
		popup_close_later(panel);
	}
	clipboard_paste_text(panel, e->chars);
}

static void motion(struct popup *p, double x, double y) {
	struct picker *pk = p->data;
	pk->inside = true;
	pk->px = x;
	pk->py = y;
	pk->hover = cell_at(pk, x, y);
	pk->hover_tab = pbox_contains(&pk->tabs, x, y) ?
		(int)((x - pk->tabs.x) / (pk->tabs.width / TAB_COUNT)) : -1;
	popup_set_dirty(p);
}

static void leave(struct popup *p) {
	struct picker *pk = p->data;
	pk->inside = false;
	pk->hover = pk->hover_tab = -1;
	popup_set_dirty(p);
}

static void button(struct popup *p, double x, double y, uint32_t btn, bool pressed) {
	struct picker *pk = p->data;
	if (!pressed || btn != BTN_LEFT) {
		return;
	}
	if (pbox_contains(&pk->tabs, x, y)) {
		int tab = (int)((x - pk->tabs.x) / (pk->tabs.width / TAB_COUNT));
		pk->search[0] = '\0';
		text_cursor_end(pk->search, &pk->tc, false);
		pk->tab = tab;
		fill_shown(pk);
		popup_set_dirty(p);
		return;
	}
	pick(pk, cell_at(pk, x, y), false);
}

static void axis(struct popup *p, double x, double y, int direction) {
	struct picker *pk = p->data;
	int rows = (pk->shown->length + columns(pk) - 1) / columns(pk);
	pk->scroll += direction > 0 ? 2 : -2;
	pk->scroll = pk->scroll > rows - ROWS ? rows - ROWS : pk->scroll;
	pk->scroll = pk->scroll < 0 ? 0 : pk->scroll;
	pk->hover = cell_at(pk, x, y);
	popup_set_dirty(p);
}

static void key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct picker *pk = p->data;
	int n = pk->shown->length;
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		pick(pk, pk->selected, mods & TEXT_MOD_SHIFT);
		return;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		if (n > 0) {
			int step = sym == XKB_KEY_Left ? -1 : sym == XKB_KEY_Right ? 1 :
				sym == XKB_KEY_Up ? -columns(pk) : columns(pk);
			int next = pk->selected + step;
			pk->selected = next < 0 ? pk->selected : next >= n ? pk->selected : next;
			keep_selected_in_view(pk);
			popup_set_dirty(p);
		}
		return;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		pk->search[0] = '\0';
		pk->tab = (pk->tab + (sym == XKB_KEY_Tab ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
		text_cursor_end(pk->search, &pk->tc, false);
		fill_shown(pk);
		popup_set_dirty(p);
		return;
	default:
		break;
	}
	if (text_key(pk->search, sizeof(pk->search), &pk->tc, sym, utf8, mods) ==
			TEXT_KEY_CHANGED) {
		fill_shown(pk);
	}
	popup_set_dirty(p);
}

static void destroy(struct popup *p) {
	struct picker *pk = p->data;
	list_free(pk->shown);
	if (current == pk) {
		current = NULL;
	}
	free(pk);
}

static const struct popup_vtable vtable = {
	.render = render,
	.motion = motion,
	.leave = leave,
	.button = button,
	.axis = axis,
	.key = key,
	.destroy = destroy,
};

void emojipicker_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_EMOJI)) {
		popup_close_all(panel);
		return;
	}
	if (!output) {
		return;
	}
	recent_load();
	struct picker *pk = calloc(1, sizeof(*pk));
	pk->panel = panel;
	pk->shown = create_list();
	pk->hover = pk->hover_tab = -1;
	pk->tab = recent->length > 0 ? TAB_RECENT : 1;
	fill_shown(pk);
	int M = popup_shadow_margin(panel);
	int width = COLUMNS * CELL + 2 * PAD + 8 + 2 * M;
	int height = 2 * PAD + SEARCH_H + 6 + TABS_H + 4 + ROWS * CELL + 4 + FOOTER_H + 2 * M;
	int bar = output->bar ? output->bar->height : 0;
	bool bottom = panel->config ? panel->config->layouts[panel->layout].bottom : true;
	// above the taskbar on the right, where Windows opens it near the text
	int x = output->width - width - 24;
	int y = bottom ? output->height - bar - height - 12 : bar + 12;
	pk->popup = popup_create(panel, POPUP_EMOJI, NULL, output, x, y, width, height, &vtable, pk);
	if (!pk->popup) {
		list_free(pk->shown);
		free(pk);
		return;
	}
	current = pk;
}
