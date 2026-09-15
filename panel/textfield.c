#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"
#include "textfield.h"

static bool continuation(unsigned char c) {
	return (c & 0xc0) == 0x80;
}

static size_t clamp(const char *text, size_t pos) {
	size_t len = strlen(text);
	if (pos > len) {
		pos = len;
	}
	while (pos > 0 && continuation(text[pos])) {
		pos--;
	}
	return pos;
}

static size_t prev_char(const char *text, size_t pos) {
	if (pos == 0) {
		return 0;
	}
	pos--;
	while (pos > 0 && continuation(text[pos])) {
		pos--;
	}
	return pos;
}

static size_t next_char(const char *text, size_t pos) {
	size_t len = strlen(text);
	if (pos >= len) {
		return len;
	}
	pos++;
	while (pos < len && continuation(text[pos])) {
		pos++;
	}
	return pos;
}

static bool word_char(unsigned char c) {
	return c >= 0x80 || isalnum(c) || c == '_';
}

/* Start of the word before pos, like Ctrl+Left on Windows. */
static size_t prev_word(const char *text, size_t pos) {
	while (pos > 0 && !word_char(text[prev_char(text, pos)])) {
		pos = prev_char(text, pos);
	}
	while (pos > 0 && word_char(text[prev_char(text, pos)])) {
		pos = prev_char(text, pos);
	}
	return pos;
}

/* Start of the next word, like Ctrl+Right on Windows. */
static size_t next_word(const char *text, size_t pos) {
	size_t len = strlen(text);
	while (pos < len && word_char(text[pos])) {
		pos = next_char(text, pos);
	}
	while (pos < len && !word_char(text[pos])) {
		pos = next_char(text, pos);
	}
	return pos;
}

void text_cursor_end(const char *text, struct text_cursor *tc, bool select) {
	tc->cursor = strlen(text);
	tc->anchor = select ? 0 : tc->cursor;
}

enum text_key_result text_key(char *text, size_t size, struct text_cursor *tc,
		xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	tc->cursor = clamp(text, tc->cursor);
	tc->anchor = clamp(text, tc->anchor);
	bool ctrl = mods & TEXT_MOD_CTRL, shift = mods & TEXT_MOD_SHIFT;
	size_t len = strlen(text);
	size_t start = tc->cursor < tc->anchor ? tc->cursor : tc->anchor;
	size_t end = tc->cursor < tc->anchor ? tc->anchor : tc->cursor;
	bool selection = start != end;
	size_t to;

	switch (sym) {
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		to = selection && !shift ? start :
			ctrl ? prev_word(text, tc->cursor) : prev_char(text, tc->cursor);
		goto move;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		to = selection && !shift ? end :
			ctrl ? next_word(text, tc->cursor) : next_char(text, tc->cursor);
		goto move;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		to = 0;
		goto move;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		to = len;
		goto move;
	case XKB_KEY_BackSpace:
		if (!selection) {
			start = ctrl ? prev_word(text, tc->cursor) : prev_char(text, tc->cursor);
			end = tc->cursor;
		}
		goto erase;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (!selection) {
			start = tc->cursor;
			end = ctrl ? next_word(text, tc->cursor) : next_char(text, tc->cursor);
		}
		goto erase;
	}
	if (ctrl && (sym == XKB_KEY_a || sym == XKB_KEY_A)) {
		tc->anchor = 0;
		tc->cursor = len;
		return TEXT_KEY_MOVED;
	}
	if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
		text[0] = '\0';
		tc->cursor = tc->anchor = 0;
		return TEXT_KEY_CHANGED;
	}
	if (!ctrl && utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
		size_t add = strlen(utf8);
		if (len - (end - start) + add >= size) {
			return TEXT_KEY_MOVED; // full
		}
		memmove(text + start, text + end, len - end + 1);
		len -= end - start;
		memmove(text + start + add, text + start, len - start + 1);
		memcpy(text + start, utf8, add);
		tc->cursor = tc->anchor = start + add;
		return TEXT_KEY_CHANGED;
	}
	return TEXT_KEY_IGNORED;

move:
	tc->cursor = to;
	if (!shift) {
		tc->anchor = to;
	}
	return TEXT_KEY_MOVED;

erase:
	if (start == end) {
		return TEXT_KEY_MOVED;
	}
	memmove(text + start, text + end, len - end + 1);
	tc->cursor = tc->anchor = start;
	return TEXT_KEY_CHANGED;
}

void text_style_colors(struct panel *panel, struct text_style *style) {
	uint32_t bg;
	switch (panel_style(panel)) {
	case PS_CLASSIC:
		bg = 0x000080ff;
		break;
	case PS_LUNA:
		bg = 0x316ac5ff;
		break;
	case PS_AERO:
		bg = 0x3399ffff;
		break;
	case PS_FLAT:
	case PS_FLUENT:
	default:
		bg = 0x0078d7ff;
		break;
	}
	style->selection_bg = tw_theme_color(panel->theme, "text.selection_bg", bg);
	style->selection_fg = tw_theme_color(panel->theme, "text.selection_fg", 0xffffffff);
}

static int prefix_width(cairo_t *cr, const char *font, const char *text, size_t bytes) {
	char *prefix = strndup(text, bytes);
	int w = 0;
	pd_text_size(cr, font, prefix, &w, NULL);
	free(prefix);
	return w;
}

void text_draw(cairo_t *cr, const struct text_style *style, const char *text,
		const struct text_cursor *tc, double x, double y, double w, double h) {
	size_t cursor = clamp(text, tc->cursor), anchor = clamp(text, tc->anchor);
	const char *shown = text;
	char *masked = NULL;
	if (style->mask) {
		// one U+25CF per character; offsets count characters
		size_t chars = 0, cursor_chars = 0, anchor_chars = 0;
		for (size_t i = 0; text[i]; i++) {
			if (!continuation(text[i])) {
				cursor_chars += i < cursor;
				anchor_chars += i < anchor;
				chars++;
			}
		}
		masked = malloc(chars * 3 + 1);
		for (size_t i = 0; i < chars; i++) {
			memcpy(masked + i * 3, "\xe2\x97\x8f", 3);
		}
		masked[chars * 3] = '\0';
		shown = masked;
		cursor = cursor_chars * 3;
		anchor = anchor_chars * 3;
	}
	size_t start = cursor < anchor ? cursor : anchor;
	size_t end = cursor < anchor ? anchor : cursor;

	int total = 0;
	pd_text_size(cr, style->font, shown, &total, NULL);
	int cursor_x = prefix_width(cr, style->font, shown, cursor);
	double scroll = cursor_x > w - 2 ? cursor_x - (w - 2) : 0;
	double tx = x - scroll;

	cairo_save(cr);
	cairo_rectangle(cr, x - 2, y, w + 4, h);
	cairo_clip(cr);
	pd_text(cr, style->font, shown, tx, y, total + 10, h, style->fg, PD_LEFT);
	if (start != end) {
		double sx = tx + prefix_width(cr, style->font, shown, start);
		double ex = tx + prefix_width(cr, style->font, shown, end);
		double pad = h * 0.16;
		pd_rect(cr, sx, y + pad, ex - sx, h - 2 * pad, style->selection_bg);
		cairo_save(cr);
		cairo_rectangle(cr, sx, y, ex - sx, h);
		cairo_clip(cr);
		pd_text(cr, style->font, shown, tx, y, total + 10, h, style->selection_fg, PD_LEFT);
		cairo_restore(cr);
	}
	if (style->caret) {
		pd_rect(cr, floor(tx + cursor_x), y + h * 0.22, 1, h * 0.56, style->fg);
	}
	cairo_restore(cr);
	free(masked);
}
