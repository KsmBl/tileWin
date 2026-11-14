#ifndef _TW_TEXTEDIT_H
#define _TW_TEXTEDIT_H
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/*
 * The single line text fields of tileWin, the same in the taskbar (start menu
 * search, Run dialog, launcher, Wi-Fi password), the lock screen and the login
 * screen: a cursor that moves, a selection made with Shift, Ctrl+A, words with
 * Ctrl, and the text drawn with both.
 */

/* modifier bits of the popup key handlers */
#define TEXT_MOD_CTRL 1
#define TEXT_MOD_SHIFT 2
#define TEXT_MOD_ALT 4

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
	const char *mask_glyph; // what stands for each character, "●" when NULL, "" for nothing
};

/*
 * Draws the text into the box, scrolled so the cursor stays visible, with the
 * selection highlighted and the cursor.
 */
void text_draw(cairo_t *cr, const struct text_style *style, const char *text,
	const struct text_cursor *tc, double x, double y, double w, double h);

#endif
