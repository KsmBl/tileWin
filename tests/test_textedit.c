/*
 * The editing of the text fields of the taskbar, the lock screen and the login
 * screen (common/tw_textedit.c): the cursor moved by characters and words,
 * a selection made with Shift and Ctrl+A, typing over it, BackSpace and Delete,
 * characters of more than one byte, and a buffer that is full.
 */
#include <stdio.h>
#include <string.h>
#include "tw_textedit.h"

static int failures;

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", what); \
		failures++; \
	} \
} while (0)

static void key(char *text, size_t size, struct text_cursor *tc, xkb_keysym_t sym,
		uint32_t mods) {
	text_key(text, size, tc, sym, NULL, mods);
}

static void type(char *text, size_t size, struct text_cursor *tc, const char *utf8) {
	text_key(text, size, tc, XKB_KEY_NoSymbol, utf8, 0);
}

int main(void) {
	char text[32] = "";
	struct text_cursor tc = { 0 };

	type(text, sizeof(text), &tc, "a");
	type(text, sizeof(text), &tc, "c");
	key(text, sizeof(text), &tc, XKB_KEY_Left, 0);
	type(text, sizeof(text), &tc, "b");
	CHECK(strcmp(text, "abc") == 0, "typing after Left goes in at the cursor");
	CHECK(tc.cursor == 2 && tc.anchor == 2, "the cursor is after what was typed");

	key(text, sizeof(text), &tc, XKB_KEY_Home, 0);
	key(text, sizeof(text), &tc, XKB_KEY_Right, TEXT_MOD_SHIFT);
	key(text, sizeof(text), &tc, XKB_KEY_Right, TEXT_MOD_SHIFT);
	CHECK(tc.anchor == 0 && tc.cursor == 2, "Shift+Right selects");
	type(text, sizeof(text), &tc, "X");
	CHECK(strcmp(text, "Xc") == 0, "typing replaces the selection");

	key(text, sizeof(text), &tc, XKB_KEY_a, TEXT_MOD_CTRL);
	CHECK(tc.anchor == 0 && tc.cursor == strlen(text), "Ctrl+A selects everything");
	key(text, sizeof(text), &tc, XKB_KEY_BackSpace, 0);
	CHECK(text[0] == '\0' && tc.cursor == 0, "BackSpace takes the selection away");

	snprintf(text, sizeof(text), "one two three");
	text_cursor_end(text, &tc, false);
	key(text, sizeof(text), &tc, XKB_KEY_Left, TEXT_MOD_CTRL);
	CHECK(tc.cursor == 8, "Ctrl+Left goes to the start of the word");
	key(text, sizeof(text), &tc, XKB_KEY_BackSpace, TEXT_MOD_CTRL);
	CHECK(strcmp(text, "one three") == 0, "Ctrl+BackSpace takes the word before away");
	key(text, sizeof(text), &tc, XKB_KEY_End, 0);
	key(text, sizeof(text), &tc, XKB_KEY_Home, TEXT_MOD_SHIFT);
	CHECK(tc.anchor == strlen(text) && tc.cursor == 0, "Shift+Home selects to the start");
	key(text, sizeof(text), &tc, XKB_KEY_Right, 0);
	CHECK(tc.cursor == strlen(text) && tc.anchor == tc.cursor,
		"Right on a selection goes to its end");

	snprintf(text, sizeof(text), "aü€b");
	text_cursor_end(text, &tc, false);
	key(text, sizeof(text), &tc, XKB_KEY_Left, 0);
	key(text, sizeof(text), &tc, XKB_KEY_BackSpace, 0);
	CHECK(strcmp(text, "aüb") == 0, "BackSpace takes a character of three bytes whole");
	key(text, sizeof(text), &tc, XKB_KEY_Delete, 0);
	CHECK(strcmp(text, "aü") == 0, "Delete takes the character after the cursor");

	char small[4] = "ab";
	struct text_cursor stc;
	text_cursor_end(small, &stc, false);
	type(small, sizeof(small), &stc, "c");
	type(small, sizeof(small), &stc, "d");
	CHECK(strcmp(small, "abc") == 0, "a full buffer takes nothing more");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
