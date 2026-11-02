#ifndef _TILEWIN_PANEL_SHORTCUTS_TEXT_H
#define _TILEWIN_PANEL_SHORTCUTS_TEXT_H
#include <stdbool.h>
#include <stddef.h>

/*
 * How the list of keyboard shortcuts says a binding, without the panel, so a
 * test can read it: what a command does and which keys it takes.
 */
enum shortcut_group {
	SHORTCUT_APPS,
	SHORTCUT_WINDOWS,
	SHORTCUT_DESKTOPS,
	SHORTCUT_SESSION,
	SHORTCUT_OTHER,
	SHORTCUT_GROUPS,
};

const char *shortcut_group_title(enum shortcut_group group);

/*
 * What the command does, in words, into label; false for a binding the list
 * leaves out (the media keys and the keys of a resize mode have their own
 * ways of being found). A command nobody knows is shown as it is, in Other.
 */
bool shortcut_describe(const char *command, char *label, size_t size,
	enum shortcut_group *group);

/* "Mod4+Shift+Prior" as "Super+Shift+Page Up". */
void shortcut_keys_text(const char *keys, char *out, size_t size);

#endif
