/* How the list of keyboard shortcuts says what a binding does and its keys. */
#include <stdio.h>
#include <string.h>
#include "shortcuts_text.h"

static int failures;

static void check_keys(const char *keys, const char *want) {
	char out[64];
	shortcut_keys_text(keys, out, sizeof(out));
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "FAIL: %s reads %s, not %s\n", keys, out, want);
		failures++;
	}
}

static void check_command(const char *command, const char *want, enum shortcut_group group_want) {
	char label[160];
	enum shortcut_group group;
	if (!shortcut_describe(command, label, sizeof(label), &group)) {
		fprintf(stderr, "FAIL: %s is left out\n", command);
		failures++;
	} else if (strcmp(label, want) != 0 || group != group_want) {
		fprintf(stderr, "FAIL: %s reads %s in %s\n", command, label, shortcut_group_title(group));
		failures++;
	}
}

int main(void) {
	check_keys("Mod4+Shift+w", "Super+Shift+W");
	check_keys("Shift+Mod4+w", "Super+Shift+W");
	check_keys("Mod4+Prior", "Super+Page Up");
	check_keys("Mod1+Control+Delete", "Ctrl+Alt+Delete");
	check_keys("Mod4+slash", "Super+/");
	check_keys("Mod1+F4", "Alt+F4");
	check_keys("Print", "Print");
	check_keys("Super_L", "Super");
	check_keys("Mod4+Left", "Super+←");

	check_command("wm_mode toggle", "Window or tile mode", SHORTCUT_SESSION);
	check_command("restart", "Restart tileWin", SHORTCUT_SESSION);
	check_command("restart panel", "Restart the taskbar", SHORTCUT_SESSION);
	check_command("workspace number 3", "Go to a desktop", SHORTCUT_DESKTOPS);
	check_command("move container to workspace number 3", "Window to a desktop",
		SHORTCUT_DESKTOPS);
	check_command("move container to workspace next_on_output, workspace next_on_output",
		"Window to the next desktop", SHORTCUT_DESKTOPS);
	check_command("move output left", "To the screen on the left", SHORTCUT_WINDOWS);
	check_command("exec xfce4-terminal", "Open xfce4-terminal", SHORTCUT_APPS);
	check_command("exec /usr/bin/thunar --daemon", "Open thunar", SHORTCUT_APPS);
	check_command("exec tilewin-lock -f", "Lock the screen", SHORTCUT_SESSION);
	check_command("opacity plus 0.1", "opacity plus 0.1", SHORTCUT_OTHER);

	char label[160];
	enum shortcut_group group;
	if (shortcut_describe("exec tilewin-media volume-up", label, sizeof(label), &group)) {
		fprintf(stderr, "FAIL: the media keys are listed\n");
		failures++;
	}
	return failures ? 1 : 0;
}
