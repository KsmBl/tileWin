#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "shortcuts_text.h"

const char *shortcut_group_title(enum shortcut_group group) {
	static const char *const titles[SHORTCUT_GROUPS] = {
		"Apps and tools", "Windows", "Desktops", "Session", "Other",
	};
	return titles[group];
}

static const struct {
	const char *command; // the start of the command, up to a space or its end
	const char *label;
	enum shortcut_group group;
} known[] = {
	{ "panel startmenu", "Start menu", SHORTCUT_APPS },
	{ "panel run", "Run dialog", SHORTCUT_APPS },
	{ "panel launcher", "App launcher", SHORTCUT_APPS },
	{ "launcher", "App launcher", SHORTCUT_APPS },
	{ "exec tilewin-settings", "Settings", SHORTCUT_APPS },
	{ "panel shortcuts", "This list", SHORTCUT_APPS },
	{ "panel quicksettings", "Quick settings", SHORTCUT_APPS },
	{ "panel notifications", "Notifications", SHORTCUT_APPS },
	{ "panel clipboard", "Clipboard history", SHORTCUT_APPS },
	{ "panel emoji", "Emoji picker", SHORTCUT_APPS },
	{ "panel snip", "Snipping tool", SHORTCUT_APPS },
	{ "exec tilewin-snip", "Screenshot", SHORTCUT_APPS },
	{ "panel activate", "Taskbar button", SHORTCUT_APPS },

	{ "kill", "Close the window", SHORTCUT_WINDOWS },
	{ "alttab next", "Switch windows", SHORTCUT_WINDOWS },
	{ "alttab prev", "Switch windows backwards", SHORTCUT_WINDOWS },
	{ "panel window_menu", "Window menu", SHORTCUT_WINDOWS },
	{ "snap left", "Snap left", SHORTCUT_WINDOWS },
	{ "snap right", "Snap right", SHORTCUT_WINDOWS },
	{ "snap up", "Snap up or maximize", SHORTCUT_WINDOWS },
	{ "snap down", "Snap down, restore or minimize", SHORTCUT_WINDOWS },
	{ "maximize", "Maximize", SHORTCUT_WINDOWS },
	{ "minimize", "Minimize", SHORTCUT_WINDOWS },
	{ "fullscreen", "Full screen", SHORTCUT_WINDOWS },
	{ "floating toggle", "Floating or tiled", SHORTCUT_WINDOWS },
	{ "focus left", "Focus left", SHORTCUT_WINDOWS },
	{ "focus right", "Focus right", SHORTCUT_WINDOWS },
	{ "focus up", "Focus up", SHORTCUT_WINDOWS },
	{ "focus down", "Focus down", SHORTCUT_WINDOWS },
	{ "focus parent", "Focus the parent", SHORTCUT_WINDOWS },
	{ "focus mode_toggle", "Focus floating or tiled", SHORTCUT_WINDOWS },
	{ "move left", "Move left", SHORTCUT_WINDOWS },
	{ "move right", "Move right", SHORTCUT_WINDOWS },
	{ "move up", "Move up", SHORTCUT_WINDOWS },
	{ "move down", "Move down", SHORTCUT_WINDOWS },
	{ "move output left", "To the screen on the left", SHORTCUT_WINDOWS },
	{ "move output right", "To the screen on the right", SHORTCUT_WINDOWS },
	{ "move output up", "To the screen above", SHORTCUT_WINDOWS },
	{ "move output down", "To the screen below", SHORTCUT_WINDOWS },
	{ "splith", "Split sideways", SHORTCUT_WINDOWS },
	{ "splitv", "Split up and down", SHORTCUT_WINDOWS },
	{ "layout stacking", "Stacked layout", SHORTCUT_WINDOWS },
	{ "layout tabbed", "Tabbed layout", SHORTCUT_WINDOWS },
	{ "layout toggle split", "Split layout", SHORTCUT_WINDOWS },
	{ "move scratchpad", "To the scratchpad", SHORTCUT_WINDOWS },
	{ "scratchpad show", "Show the scratchpad", SHORTCUT_WINDOWS },
	{ "mode \"resize\"", "Resize mode", SHORTCUT_WINDOWS },

	{ "taskview", "Task view", SHORTCUT_DESKTOPS },
	{ "showdesktop", "Show the desktop", SHORTCUT_DESKTOPS },
	{ "desktop new", "New desktop", SHORTCUT_DESKTOPS },
	{ "desktop close", "Close the desktop", SHORTCUT_DESKTOPS },
	{ "desktop move left", "Move the desktop left", SHORTCUT_DESKTOPS },
	{ "desktop move right", "Move the desktop right", SHORTCUT_DESKTOPS },
	{ "workspace prev_on_output", "Previous desktop", SHORTCUT_DESKTOPS },
	{ "workspace next_on_output", "Next desktop", SHORTCUT_DESKTOPS },
	{ "move container to workspace prev_on_output", "Window to the previous desktop",
		SHORTCUT_DESKTOPS },
	{ "move container to workspace next_on_output", "Window to the next desktop",
		SHORTCUT_DESKTOPS },
	{ "workspace number", "Go to a desktop", SHORTCUT_DESKTOPS },
	{ "move container to workspace number", "Window to a desktop", SHORTCUT_DESKTOPS },

	{ "wm_mode", "Window or tile mode", SHORTCUT_SESSION },
	{ "exec tilewin-lock", "Lock the screen", SHORTCUT_SESSION },
	{ "exec swaylock", "Lock the screen", SHORTCUT_SESSION },
	{ "reload", "Reload the config", SHORTCUT_SESSION },
	{ "restart panel", "Restart the taskbar", SHORTCUT_SESSION },
	{ "restart", "Restart tileWin", SHORTCUT_SESSION },
	{ "panel shutdown", "Shut down dialog", SHORTCUT_SESSION },
	{ "exec tilewin-nag", "Exit tileWin", SHORTCUT_SESSION },
	{ "exit", "Exit tileWin", SHORTCUT_SESSION },
};

bool shortcut_describe(const char *command, char *label, size_t size,
		enum shortcut_group *group) {
	if (strstr(command, "tilewin-media")) {
		return false;
	}
	size_t best = 0;
	int found = -1;
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		size_t len = strlen(known[i].command);
		if (len > best && strncmp(command, known[i].command, len) == 0 &&
				(command[len] == '\0' || command[len] == ' ' || command[len] == ',')) {
			best = len;
			found = (int)i;
		}
	}
	if (found >= 0) {
		snprintf(label, size, "%s", known[found].label);
		*group = known[found].group;
		return true;
	}
	if (strncmp(command, "exec ", 5) == 0) {
		// the program by its name: the terminal, the file manager, what you set
		const char *name = command + 5;
		while (strncmp(name, "--no-startup-id ", 16) == 0) {
			name += 16;
		}
		size_t len = strcspn(name, " ");
		const char *slash = memchr(name, '/', len);
		while (slash) {
			len -= slash + 1 - name;
			name = slash + 1;
			slash = memchr(name, '/', len);
		}
		snprintf(label, size, "Open %.*s", (int)len, name);
		*group = SHORTCUT_APPS;
		return true;
	}
	snprintf(label, size, "%s", command);
	*group = SHORTCUT_OTHER;
	return true;
}

/* Modifiers first and always in this order, whatever order they came in. */
static int key_order(const char *key, size_t len) {
	static const char *const mods[] = { "Mod4", "Control", "Ctrl", "Mod1", "Alt", "Shift" };
	for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		if (strlen(mods[i]) == len && strncmp(mods[i], key, len) == 0) {
			return (int)i;
		}
	}
	return 100;
}

static void append_key(const char *key, size_t len, char *out, size_t size, size_t *used) {
	static const struct {
		const char *name, *text;
	} names[] = {
		{ "Mod4", "Super" }, { "Mod1", "Alt" }, { "Control", "Ctrl" }, { "Ctrl", "Ctrl" },
		{ "Prior", "Page Up" }, { "Next", "Page Down" }, { "Left", "←" }, { "Right", "→" },
		{ "Up", "↑" }, { "Down", "↓" }, { "Return", "Enter" }, { "Escape", "Esc" },
		{ "space", "Space" }, { "period", "." }, { "semicolon", ";" }, { "slash", "/" },
		{ "minus", "-" }, { "Super_L", "Super" }, { "Super_R", "Super" },
	};
	const char *sep = *used ? "+" : "";
	const char *text = NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strlen(names[i].name) == len && strncmp(names[i].name, key, len) == 0) {
			text = names[i].text;
			break;
		}
	}
	int n;
	if (text) {
		n = snprintf(out + *used, size - *used, "%s%s", sep, text);
	} else if (len == 1) {
		n = snprintf(out + *used, size - *used, "%s%c", sep, toupper((unsigned char)*key));
	} else {
		n = snprintf(out + *used, size - *used, "%s%.*s", sep, (int)len, key);
	}
	if (n > 0) {
		*used += (size_t)n < size - *used ? (size_t)n : size - *used - 1;
	}
}

void shortcut_keys_text(const char *keys, char *out, size_t size) {
	size_t used = 0;
	out[0] = '\0';
	// each order in turn: at most the six names of modifiers, then the keys
	for (int order = 0; order <= 100; order = order < 5 ? order + 1 : order == 5 ? 100 : 101) {
		for (const char *p = keys; *p;) {
			size_t len = strcspn(p, "+");
			if (len && key_order(p, len) == order) {
				append_key(p, len, out, size, &used);
			}
			p += len;
			p += *p == '+';
		}
	}
}
