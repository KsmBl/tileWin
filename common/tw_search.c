#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tw_search.h"

const struct tw_settings_page tw_settings_pages[] = {
	{ "theme", "Theme", "appearance look style dark light colors mode night scheme accent",
		"preferences-desktop-appearance-symbolic" },
	{ "wallpaper", "Wallpaper", "background desktop picture",
		"preferences-desktop-wallpaper-symbolic" },
	{ "desktop", "Desktop", "icons grid cells widgets gadgets clock analog binary digital chart "
		"gauge ring bar cpu memory power",
		"user-desktop-symbolic" },
	{ "animations", "Animations", "effects motion speed fade zoom slide minimize maximize open close "
		"desktop switch",
		"media-playlist-repeat-symbolic" },
	{ "windows", "Window behavior", "snap stick drag move together group modifier stretch double click "
		"focus follows mouse attention activation priority nice cpu fast",
		"focus-windows-symbolic" },
	{ "appwindows", "App windows", "not responding hang hung frozen freeze end task kill minimized pause "
		"stop suspend battery efficiency sound remember position place size "
		"open where rules for_window always on top maximized desktop workspace",
		"application-x-executable-symbolic" },
	{ "screen", "Screen", "display monitor resolution refresh scale rotation brightness night "
		"light sleep lock lid power",
		"video-display-symbolic" },
	{ "screensaver", "Screen saver", "screensaver bubbles mystify ribbons 3d text photos slideshow "
		"starfield pipes maze flying windows aurora word clock tiling diggers "
		"miners doomsday end of the world apocalypse hellfire fire burn "
		"thunderstorm storm rain lightning blizzard snow ice frost icicles "
		"winter decay time dust age cracks ruin jungle plants vines leaves "
		"flowers moss butterflies gravity privacy falling matrix microslop "
		"copilot ad commercial idle lock fps frames per second cpu usage "
		"source code password resume",
		"preferences-desktop-screensaver-symbolic" },
	{ "sound", "Sound", "volume audio speakers headphones microphone mute",
		"audio-speakers-symbolic" },
	{ "datetime", "Date & time", "clock calendar time zone timezone ntp hour format 12 24 seconds "
		"automatic",
		"preferences-system-time-symbolic" },
	{ "bluetooth", "Bluetooth", "headphones mouse keyboard pair devices wireless",
		"bluetooth-symbolic" },
	{ "network", "Network", "wifi wi-fi wlan ethernet cable ip address dhcp fixed static gateway "
		"subnet mask router password key dns servers",
		"network-wireless-symbolic" },
	{ "dns", "DNS", "dns servers name resolver cache prefetch fastest block lists pihole "
		"pi-hole ads adblock trackers hosts allow",
		"network-server-symbolic" },
	{ "taskbar", "Taskbar", "panel bar widgets tray clock notifications clipboard history right "
		"click context menu",
		"view-continuous-symbolic" },
	{ "startmenu", "Start menu", "start pinned apps places power tiles layout",
		"start-here-symbolic" },
	{ "launcher", "Launcher & apps", "run search applications",
		"system-search-symbolic" },
	{ "devices", "Device manager", "device manager devmgmt hardware drivers driver missing firmware pci "
		"usb disable enable restart unknown other devices properties display "
		"adapters network adapters disk drives sound processors",
		"preferences-system-devices-symbolic" },
	{ "keyboard", "Keyboard", "shortcuts keys bindings layout hotkeys",
		"input-keyboard-symbolic" },
	{ "mouse", "Mouse & touchpad", "pointer cursor touchpad scrolling tap magnifier zoom magnify enlarge",
		"input-mouse-symbolic" },
	{ "apps", "Apps", "default browser email startup autostart programs",
		"applications-system-symbolic" },
	{ "account", "Account", "user picture photo avatar profile name",
		"avatar-default-symbolic" },
	{ "backup", "Backup", "restore save export import copy settings file another computer",
		"document-save-symbolic" },
	{ "kernel", "Kernel", "linux kernel build compile version latest longterm live patching "
		"livepatch kpatch kexec load boot grub uki efi modules drivers "
		"compression zstd xz lz4 gzip lzo localmodconfig",
		"applications-engineering-symbolic" },
	{ "about", "About", "system info fetch version update upgrade release welcome kernel cpu memory logo uwu",
		"help-about-symbolic" },
};

const size_t tw_settings_page_count = sizeof(tw_settings_pages) / sizeof(tw_settings_pages[0]);

/* The text holds a word that starts with these letters. */
static bool has_word_start(const char *text, const char *word, size_t len) {
	for (const char *p = text; *p; p++) {
		bool start = p == text || !isalnum((unsigned char)p[-1]);
		if (start && strncasecmp(p, word, len) == 0) {
			return true;
		}
	}
	return false;
}

int tw_settings_page_score(const struct tw_settings_page *page, const char *query) {
	while (*query == ' ') {
		query++;
	}
	size_t total = strlen(query);
	if (total < 2) {
		return 0;
	}
	if (strncasecmp(page->title, query, total) == 0) {
		return 3;
	}
	const char *p = query;
	while (*p) {
		size_t len = strcspn(p, " ");
		if (len && !has_word_start(page->title, p, len) &&
				!has_word_start(page->keywords, p, len)) {
			return 0;
		}
		p += len;
		p += strspn(p, " ");
	}
	return has_word_start(page->title, query, strcspn(query, " ")) ? 2 : 1;
}

/* ---------- sums ---------- */

struct calc {
	const char *p;
	bool bad;
	int operators;
};

static double calc_sum(struct calc *c);

static void calc_space(struct calc *c) {
	while (*c->p == ' ') {
		c->p++;
	}
}

/* By hand, since strtod would read a comma or a point by the locale. */
static double calc_number(struct calc *c) {
	double value = 0;
	bool digits = false;
	while (isdigit((unsigned char)*c->p)) {
		value = value * 10 + (*c->p++ - '0');
		digits = true;
	}
	if (*c->p == '.' || *c->p == ',') {
		c->p++;
		double place = 0.1;
		while (isdigit((unsigned char)*c->p)) {
			value += (*c->p++ - '0') * place;
			place /= 10;
			digits = true;
		}
	}
	if (!digits) {
		c->bad = true;
	}
	return value;
}

static double calc_atom(struct calc *c) {
	calc_space(c);
	if (*c->p == '-' || *c->p == '+') {
		bool minus = *c->p++ == '-';
		double value = calc_atom(c);
		return minus ? -value : value;
	}
	if (*c->p == '(') {
		c->p++;
		double value = calc_sum(c);
		calc_space(c);
		if (*c->p != ')') {
			c->bad = true;
			return 0;
		}
		c->p++;
		return value;
	}
	return calc_number(c);
}

static double calc_power(struct calc *c) {
	double value = calc_atom(c);
	calc_space(c);
	if (*c->p == '^') {
		c->p++;
		c->operators++;
		return pow(value, calc_power(c)); // 2^3^2 is 2^9
	}
	return value;
}

static double calc_product(struct calc *c) {
	double value = calc_power(c);
	while (!c->bad) {
		calc_space(c);
		char op = *c->p;
		if (op == '%') {
			c->p++;
			c->operators++;
			calc_space(c);
			if (isdigit((unsigned char)*c->p) || *c->p == '(' || *c->p == '.') {
				double by = calc_power(c);
				if (by == 0) {
					c->bad = true;
					return 0;
				}
				value = fmod(value, by);
			} else {
				value /= 100;
			}
		} else if (op == '*' || op == '/') {
			c->p++;
			c->operators++;
			double by = calc_power(c);
			if (op == '/' && by == 0) {
				c->bad = true;
				return 0;
			}
			value = op == '*' ? value * by : value / by;
		} else {
			break;
		}
	}
	return value;
}

static double calc_sum(struct calc *c) {
	double value = calc_product(c);
	while (!c->bad) {
		calc_space(c);
		char op = *c->p;
		if (op != '+' && op != '-') {
			break;
		}
		c->p++;
		c->operators++;
		double by = calc_product(c);
		value = op == '+' ? value + by : value - by;
	}
	return value;
}

bool tw_calc(const char *text, double *result) {
	struct calc c = { .p = text };
	calc_space(&c);
	bool asked = *c.p == '=';
	if (asked) {
		c.p++;
	}
	double value = calc_sum(&c);
	calc_space(&c);
	if (c.bad || *c.p || !isfinite(value) || (!asked && c.operators == 0)) {
		return false;
	}
	*result = value;
	return true;
}

void tw_calc_format(double value, char *buf, size_t size) {
	snprintf(buf, size, "%.12g", value == 0 ? 0 : value);
	// whatever the locale made of the point
	for (char *p = buf; *p; p++) {
		if (*p == ',') {
			*p = '.';
		}
	}
}

char *tw_shell_quote(const char *text) {
	size_t len = 3;
	for (const char *p = text; *p; p++) {
		len += *p == '\'' ? 4 : 1;
	}
	char *out = malloc(len);
	char *o = out;
	*o++ = '\'';
	for (const char *p = text; *p; p++) {
		if (*p == '\'') {
			memcpy(o, "'\\''", 4);
			o += 4;
		} else {
			*o++ = *p;
		}
	}
	*o++ = '\'';
	*o = '\0';
	return out;
}

bool tw_time_parse(const char *text, int *minutes) {
	int h, m;
	char end;
	if (!text || sscanf(text, "%d:%d%c", &h, &m, &end) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
		return false;
	}
	*minutes = h * 60 + m;
	return true;
}

bool tw_time_between(int now, int from, int to) {
	if (from == to) {
		return false;
	}
	return from < to ? now >= from && now < to : now >= from || now < to;
}
