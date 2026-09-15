#ifndef _TILEWIN_PANEL_TEXTFIELD_H
#define _TILEWIN_PANEL_TEXTFIELD_H
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct panel;

/* modifier bits of the popup key handlers */
#define TEXT_MOD_CTRL 1
#define TEXT_MOD_SHIFT 2

/*
 * Editing of the single line text fields (start menu search, run dialog,
 * launcher, desktop dialogs, Wi-Fi password): a cursor and a selection in a
 * NUL-terminated UTF-8 buffer. The selection is the text between anchor and
 * cursor (byte offsets); both are clamped to the text, so callers may change
 * the buffer directly.
 */
struct text_cursor {
	size_t cursor, anchor;
};

enum text_key_result {
	TEXT_KEY_IGNORED, // not an editing key, the caller handles it
	TEXT_KEY_MOVED,   // the cursor or selection changed
	TEXT_KEY_CHANGED, // the text changed
};

/* Puts the cursor at the end; select selects the whole text. */
void text_cursor_end(const char *text, struct text_cursor *tc, bool select);

/*
 * Left/Right (Ctrl: by word), Home/End, each with Shift to select, Ctrl+A,
 * BackSpace/Delete (Ctrl: by word), Ctrl+U and typed text, which replaces
 * the selection.
 */
enum text_key_result text_key(char *text, size_t size, struct text_cursor *tc,
	xkb_keysym_t sym, const char *utf8, uint32_t mods);

struct text_style {
	const char *font;
	uint32_t fg, selection_bg, selection_fg;
	bool caret; // show the cursor
	bool mask;  // password: a dot for every character
};

/* Sets the selection colors of the panel theme (text.selection_bg/fg). */
void text_style_colors(struct panel *panel, struct text_style *style);

/*
 * Draws the text into the box, scrolled so the cursor stays visible, with the
 * selection highlighted and the cursor.
 */
void text_draw(cairo_t *cr, const struct text_style *style, const char *text,
	const struct text_cursor *tc, double x, double y, double w, double h);

#endif
