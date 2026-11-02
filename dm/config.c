#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"

static const char *const color_schemes[] = { "follow", "light", "dark", NULL };
static const char *const card_styles[] = { "window", "plain", NULL };
static const char *const positions[] = { "center", "left", "right", NULL };
static const char *const clock_places[] = { "top", "bottom-left", "bottom-right", "none", NULL };
static const char *const password_styles[] = { "dots", "stars", "hidden", NULL };

#define BOOL(k, g, l, h, d) { k, DM_BOOL, g, l, h, d, 0, 0, NULL }
#define INT(k, g, l, h, d, a, b) { k, DM_INT, g, l, h, d, a, b, NULL }
#define CHOICE(k, g, l, h, d, c) { k, DM_CHOICE, g, l, h, d, 0, 0, c }
#define TEXT(k, g, l, h, d) { k, DM_TEXT, g, l, h, d, 0, 0, NULL }

const struct dm_option dm_options[] = {
	// ---- logging in
	TEXT("default_user", "Logging in", "User picked at first",
		"When no one logged in before, or remembering is off", ""),
	BOOL("remember_user", "Logging in", "Remember the last user", NULL, "yes"),
	TEXT("default_session", "Logging in", "Session picked at first",
		"The name of its file in wayland-sessions, without .desktop", "tilewin"),
	BOOL("remember_session", "Logging in", "Remember the last session", NULL, "yes"),
	TEXT("autologin_user", "Logging in", "Log this user in without a password",
		"Anyone at this computer is then that user. Empty for nobody.", ""),
	TEXT("autologin_session", "Logging in", "Session of the automatic login",
		"Empty for the session picked at first", ""),
	BOOL("autologin_once", "Logging in", "Automatic login only once per start of the computer",
		"Turned off, logging out logs in again right away", "yes"),
	BOOL("allow_root", "Logging in", "Allow logging in as root", NULL, "no"),
	INT("fail_delay", "Logging in", "Seconds to wait after a wrong password", NULL, "2", 0, 60),
	INT("clear_password_after", "Logging in", "Forget a half typed password after",
		"Seconds without a key, 0 for never", "60", 0, 3600),
	TEXT("session_path", "Logging in", "PATH of a session", NULL,
		"/usr/local/sbin:/usr/local/bin:/usr/bin"),
	INT("vt", "Logging in", "Virtual terminal", "Where the login screen and the sessions run",
		"1", 1, 63),

	// ---- look
	TEXT("theme", "Look", "Theme",
		"The name of a tileWin theme, or follow for the theme of whoever logged in last", "follow"),
	CHOICE("color_scheme", "Look", "Color scheme",
		"Follow takes it from whoever logged in last", "follow", color_schemes),
	TEXT("wallpaper", "Look", "Wallpaper",
		"theme, a picture every user can read, or a color like #204070", "theme"),
	INT("dim", "Look", "Darken the wallpaper", "In percent", "30", 0, 90),
	TEXT("accent", "Look", "Accent color", "theme or a color like #0078d4", "theme"),
	TEXT("font", "Look", "Font", "theme or a font like Noto Sans 11", "theme"),
	CHOICE("card_style", "Look", "Login card",
		"window has the title bar of the theme", "window", card_styles),
	CHOICE("card_position", "Look", "Place of the login card", NULL, "center", positions),
	INT("card_width", "Look", "Width of the login card", "In pixels", "420", 280, 900),
	TEXT("card_title", "Look", "Title of the login card", "%h is the name of the computer",
		"Log in to %h"),
	BOOL("card_on_all_screens", "Look", "Login card on every screen",
		"Turned off, only on the first", "yes"),

	// ---- what is shown
	CHOICE("clock", "Shown", "Clock", NULL, "top", clock_places),
	TEXT("clock_format", "Shown", "Format of the clock", "As strftime", "%H:%M"),
	TEXT("date_format", "Shown", "Format of the date", "As strftime; empty for no date",
		"%A, %e %B"),
	TEXT("greeting", "Shown", "Message", "A line under the title, e.g. who to call for help", ""),
	BOOL("show_users", "Shown", "List the users",
		"Turned off, the name is typed", "yes"),
	INT("min_uid", "Shown", "Lowest user ID listed", NULL, "1000", 0, 65535),
	INT("max_uid", "Shown", "Highest user ID listed", NULL, "60000", 0, 65535),
	TEXT("hide_users", "Shown", "Users not listed", "Names with spaces between them", ""),
	BOOL("show_full_names", "Shown", "Show full names", NULL, "yes"),
	BOOL("show_avatars", "Shown", "Show pictures of users",
		"/var/lib/AccountsService/icons/<user>, or .face if it can be read", "yes"),
	BOOL("show_sessions", "Shown", "Show the session to pick", NULL, "yes"),
	BOOL("show_caps_lock", "Shown", "Say that Caps Lock is on", NULL, "yes"),
	BOOL("show_layout", "Shown", "Show the keyboard layout to pick",
		"With more than one layout", "yes"),
	CHOICE("password_style", "Shown", "Password shown as", NULL, "dots", password_styles),
	BOOL("password_reveal", "Shown", "A button to show the password", NULL, "yes"),

	// ---- power
	BOOL("show_power", "Power", "Show the power buttons", NULL, "yes"),
	BOOL("allow_poweroff", "Power", "Allow shutting down", NULL, "yes"),
	BOOL("allow_reboot", "Power", "Allow restarting", NULL, "yes"),
	BOOL("allow_suspend", "Power", "Allow sleeping", NULL, "yes"),
	BOOL("allow_hibernate", "Power", "Allow hibernating", NULL, "no"),
	INT("screen_off_after", "Power", "Turn the screens off after",
		"Seconds without input, 0 for never", "300", 0, 86400),

	// ---- input
	TEXT("keyboard_layout", "Input", "Keyboard layouts",
		"As xkb, like de,us. Empty for the default.", ""),
	TEXT("keyboard_variant", "Input", "Keyboard variants", "As xkb, like nodeadkeys,", ""),
	TEXT("keyboard_options", "Input", "Keyboard options", "As xkb, like ctrl:nocaps", ""),
	BOOL("numlock", "Input", "Num Lock on", NULL, "yes"),
	INT("repeat_delay", "Input", "Delay before a key repeats", "In milliseconds", "400", 100, 2000),
	INT("repeat_rate", "Input", "Key repeats per second", NULL, "30", 1, 100),
	BOOL("tap_to_click", "Input", "Tap the touchpad to click", NULL, "yes"),
	TEXT("cursor_theme", "Input", "Pointer theme", "Empty for the default", ""),
	INT("cursor_size", "Input", "Pointer size", NULL, "24", 8, 128),
	BOOL("hide_cursor", "Input", "Hide the pointer until it moves", NULL, "no"),
	TEXT("screen_scale", "Input", "Scale of the screens", "auto, or like 1.5", "auto"),
};

const size_t dm_option_count = sizeof(dm_options) / sizeof(dm_options[0]);

static int option_index(const char *key) {
	for (size_t i = 0; i < dm_option_count; i++) {
		if (strcmp(dm_options[i].key, key) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static bool parse_bool(const char *text, bool *out) {
	if (strcasecmp(text, "yes") == 0 || strcasecmp(text, "true") == 0 ||
			strcasecmp(text, "on") == 0 || strcmp(text, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcasecmp(text, "no") == 0 || strcasecmp(text, "false") == 0 ||
			strcasecmp(text, "off") == 0 || strcmp(text, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

/* NULL when the value will do, or else what is wrong with it. */
static const char *value_problem(const struct dm_option *option, const char *value) {
	switch (option->type) {
	case DM_BOOL: {
		bool ignored;
		return parse_bool(value, &ignored) ? NULL : "is neither yes nor no";
	}
	case DM_INT: {
		char *end = NULL;
		long number = strtol(value, &end, 10);
		if (!*value || *end) {
			return "is no number";
		}
		return number < option->min || number > option->max ? "is out of range" : NULL;
	}
	case DM_CHOICE:
		for (const char *const *c = option->choices; *c; c++) {
			if (strcmp(*c, value) == 0) {
				return NULL;
			}
		}
		return "is not one of the choices";
	case DM_TEXT:
		return NULL;
	}
	return NULL;
}

/* Splits a line in key and value in place; false for a blank line or a comment. */
static bool split_line(char *line, char **key, char **value) {
	line[strcspn(line, "\r\n")] = '\0';
	while (isspace((unsigned char)*line)) {
		line++;
	}
	if (!*line || *line == '#') {
		return false;
	}
	*key = line;
	char *p = line;
	while (*p && !isspace((unsigned char)*p)) {
		p++;
	}
	if (*p) {
		*p++ = '\0';
	}
	while (isspace((unsigned char)*p)) {
		p++;
	}
	char *end = p + strlen(p);
	while (end > p && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
	// quotes keep spaces at the ends, and let a value be empty on purpose
	if (end - p >= 2 && *p == '"' && end[-1] == '"') {
		end[-1] = '\0';
		p++;
	}
	*value = p;
	return true;
}

/* An environment line is KEY=value with a KEY a shell would take. */
static bool env_line_ok(const char *text) {
	const char *eq = strchr(text, '=');
	if (!eq || eq == text || isdigit((unsigned char)*text)) {
		return false;
	}
	for (const char *p = text; p < eq; p++) {
		if (!isalnum((unsigned char)*p) && *p != '_') {
			return false;
		}
	}
	return true;
}

struct dm_config *dm_config_load(const char *path) {
	struct dm_config *config = calloc(1, sizeof(*config));
	config->values = calloc(dm_option_count, sizeof(char *));
	for (size_t i = 0; i < dm_option_count; i++) {
		config->values[i] = strdup(dm_options[i].fallback);
	}
	config->env = calloc(1, sizeof(char *));
	FILE *f = path ? fopen(path, "r") : NULL;
	char buffer[2048];
	while (f && fgets(buffer, sizeof(buffer), f)) {
		char *key, *value;
		if (!split_line(buffer, &key, &value)) {
			continue;
		}
		if (strcmp(key, "env") == 0) {
			if (env_line_ok(value)) {
				config->env = realloc(config->env, (config->env_count + 2) * sizeof(char *));
				config->env[config->env_count++] = strdup(value);
				config->env[config->env_count] = NULL;
			}
			continue;
		}
		int index = option_index(key);
		if (index >= 0 && !value_problem(&dm_options[index], value)) {
			free(config->values[index]);
			config->values[index] = strdup(value);
		}
	}
	if (f) {
		fclose(f);
	}
	return config;
}

void dm_config_free(struct dm_config *config) {
	if (!config) {
		return;
	}
	for (size_t i = 0; i < dm_option_count; i++) {
		free(config->values[i]);
	}
	for (size_t i = 0; i < config->env_count; i++) {
		free(config->env[i]);
	}
	free(config->values);
	free(config->env);
	free(config);
}

const char *dm_config_str(const struct dm_config *config, const char *key) {
	int index = option_index(key);
	return index >= 0 ? config->values[index] : "";
}

int dm_config_int(const struct dm_config *config, const char *key) {
	return atoi(dm_config_str(config, key));
}

bool dm_config_bool(const struct dm_config *config, const char *key) {
	bool out = false;
	parse_bool(dm_config_str(config, key), &out);
	return out;
}

int dm_config_check(const char *path, void (*report)(const char *problem, void *data), void *data) {
	FILE *f = fopen(path, "r");
	if (!f) {
		report("the file cannot be read", data);
		return 1;
	}
	int problems = 0, number = 0;
	char buffer[2048], text[2300];
	while (fgets(buffer, sizeof(buffer), f)) {
		number++;
		char *key, *value;
		if (!split_line(buffer, &key, &value)) {
			continue;
		}
		const char *problem = NULL;
		if (strcmp(key, "env") == 0) {
			problem = env_line_ok(value) ? NULL : "is no KEY=value";
		} else {
			int index = option_index(key);
			problem = index < 0 ? "is no setting" : value_problem(&dm_options[index], value);
		}
		if (problem) {
			snprintf(text, sizeof(text), "line %d: %s \"%s\" %s", number, key, value, problem);
			report(text, data);
			problems++;
		}
	}
	fclose(f);
	return problems;
}

bool dm_word_in_list(const char *list, const char *word) {
	size_t len = strlen(word);
	for (const char *p = list; len && *p;) {
		p += strspn(p, " \t,");
		size_t n = strcspn(p, " \t,");
		if (n == len && strncmp(p, word, len) == 0) {
			return true;
		}
		p += n;
	}
	return false;
}
