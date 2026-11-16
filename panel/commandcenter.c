/*
 * The Command Center of modernCC: one place to do anything. Super (or the
 * capsule on the top bar) opens it in the middle of the screen; what is typed
 * finds, in one list:
 *
 *   - the open windows: Enter switches to one, its actions close or minimize it;
 *   - the apps: an app with a window open finds that window first;
 *   - actions: lock, sleep, shut down, dark mode, Wi-Fi, do not disturb, task
 *     view, show the desktop, a screenshot, the clipboard, emoji, another theme…;
 *   - settings pages, files and sums (searchextras.c);
 *   - a command to run, when nothing else matches.
 *
 * With nothing typed it shows the open windows and what is used most and
 * lately (tw_command.h), so the usual things take no typing at all. Typed
 * first, ">" keeps to the actions, "@" to the windows, "#" to the settings.
 * The chosen row shows what else can be done with it as chips: Tab steps
 * through them, Alt+1 to Alt+3 do one at once, Enter does the one marked.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "draw.h"
#include "popup.h"
#include "stringop.h"
#include "textfield.h"
#include "tw_command.h"
#include "tw_desktop.h"
#include "tw_theme.h"

#define CC_WIDTH 680
#define FIELD_H 60
#define ROW_H 48
#define HEAD_H 26
#define FOOT_H 34
#define ROWS 9
#define MAX_ALTS 3

enum cc_kind { CC_WINDOW, CC_APP, CC_ACTION, CC_SETTING, CC_FILE, CC_SUM, CC_RUN };

static const char *const kind_names[] = { "Window", "App", "Action", "Setting", "File", "Sum",
	"Command" };

struct cc_alt {
	char label[32];
	char *command;
};

struct cc_item {
	enum cc_kind kind;
	char *key; // for the history: "app:firefox", "win:org.gnome.Nautilus", "act:lock"
	char *name, *detail, *icon, *keywords;
	char *command;                     // run by Enter (bar_run_command)
	const struct tw_desktop_entry *entry; // an app or what the extras found: launched
	struct cc_alt alts[MAX_ALTS];
	int alt_count;
	double score;
	bool heading; // a line that names the rows below it
};

struct cc {
	struct panel *panel;
	list_t *apps;       // struct tw_desktop_entry *, owned
	list_t *items;      // struct cc_item *: what can be found, owned
	list_t *shown;      // struct cc_item *: what the list shows now, borrowed
	list_t *headings;   // struct cc_item *, owned
	struct search_extras *extras;
	list_t *extra_items; // struct cc_item * made of the extras, owned
	struct cc_item run;
	struct tw_cmd_history history;
	char query[256];
	struct text_cursor tc;
	int selected, alt, scroll;
	bool inside;
	double px, py;
};

/* ---------- items ---------- */

static struct cc_item *item_new(enum cc_kind kind, const char *key, const char *name,
		const char *detail, const char *icon, const char *command) {
	struct cc_item *it = calloc(1, sizeof(*it));
	it->kind = kind;
	it->key = strdup(key ? key : "");
	it->name = strdup(name ? name : "");
	it->detail = detail ? strdup(detail) : NULL;
	it->icon = icon ? strdup(icon) : NULL;
	it->command = command ? strdup(command) : NULL;
	return it;
}

static void item_alt(struct cc_item *it, const char *label, const char *command) {
	if (it->alt_count == MAX_ALTS) {
		return;
	}
	struct cc_alt *a = &it->alts[it->alt_count++];
	snprintf(a->label, sizeof(a->label), "%s", label);
	a->command = strdup(command);
}

static void item_free(struct cc_item *it) {
	if (!it) {
		return;
	}
	free(it->key);
	free(it->name);
	free(it->detail);
	free(it->icon);
	free(it->keywords);
	free(it->command);
	for (int i = 0; i < it->alt_count; i++) {
		free(it->alts[i].command);
	}
	free(it);
}

static void items_free(list_t *list) {
	for (int i = 0; list && i < list->length; i++) {
		item_free(list->items[i]);
	}
	list_free(list);
}

/* The app a window belongs to, for its name and icon. */
static const struct tw_desktop_entry *app_of(struct cc *c, const char *app_id) {
	for (int i = 0; app_id && i < c->apps->length; i++) {
		const struct tw_desktop_entry *e = c->apps->items[i];
		const char *wm = e->startup_wm_class;
		if ((e->id && strncasecmp(e->id, app_id, strlen(app_id)) == 0) ||
				(wm && strcasecmp(wm, app_id) == 0) ||
				(e->name && strcasecmp(e->name, app_id) == 0)) {
			return e;
		}
	}
	return NULL;
}

static void add_windows(struct cc *c) {
	list_t *windows = c->panel->state.windows;
	// the window in front last: the first one offered is one to switch to
	list_t *order = create_list();
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (!w->focused) {
			list_add(order, w);
		}
	}
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (w->focused) {
			list_add(order, w);
		}
	}
	for (int i = 0; i < order->length; i++) {
		struct pwindow *w = order->items[i];
		if (w->parent) {
			continue; // dialogs come with their window
		}
		const struct tw_desktop_entry *app = app_of(c, w->app_id);
		char key[160], cmd[96], detail[200];
		snprintf(key, sizeof(key), "win:%s", w->app_id ? w->app_id : "");
		snprintf(cmd, sizeof(cmd), w->minimized ? "[con_id=%" PRId64 "] minimize disable, focus" :
			"[con_id=%" PRId64 "] focus", w->id);
		snprintf(detail, sizeof(detail), "%s%s%s", app && app->name ? app->name :
			w->app_id ? w->app_id : "Window", w->workspace ? " · desktop " : "",
			w->workspace ? w->workspace : "");
		struct cc_item *it = item_new(CC_WINDOW, key, w->title && *w->title ? w->title :
			(app ? app->name : w->app_id), detail, app ? app->icon : w->app_id, cmd);
		it->keywords = strdup(app && app->name ? app->name : w->app_id ? w->app_id : "");
		snprintf(cmd, sizeof(cmd), "[con_id=%" PRId64 "] kill", w->id);
		item_alt(it, "Close", cmd);
		snprintf(cmd, sizeof(cmd), "[con_id=%" PRId64 "] minimize enable", w->id);
		item_alt(it, "Minimize", cmd);
		list_add(c->items, it);
	}
	list_free(order);
}

static void add_apps(struct cc *c) {
	for (int i = 0; i < c->apps->length; i++) {
		const struct tw_desktop_entry *e = c->apps->items[i];
		char key[300];
		snprintf(key, sizeof(key), "app:%s", e->id ? e->id : e->name);
		struct cc_item *it = item_new(CC_APP, key, e->name, e->generic_name ? e->generic_name :
			e->comment, e->icon, NULL);
		it->entry = e;
		it->keywords = strdup(e->keywords ? e->keywords : "");
		list_add(c->items, it);
	}
}

/* What tileWin can do, in words: the actions of the Command Center. */
static const struct {
	const char *key, *name, *detail, *icon, *keywords, *command;
} actions[] = {
	{ "lock", "Lock the screen", "Keep it running, ask for the password", "system-lock-screen",
		"lock away leave", "exec tilewin-lock -f" },
	{ "sleep", "Sleep", "Stand by in memory", "system-suspend", "suspend standby",
		"exec systemctl suspend" },
	{ "shutdown", "Shut down or restart…", NULL, "system-shutdown", "power off reboot restart",
		"panel shutdown" },
	{ "logoff", "Sign out…", NULL, "system-log-out", "log out logout leave", "panel shutdown logoff" },
	{ "dark", "Dark or light mode", "Switch the colour scheme", "weather-clear-night",
		"theme night colour color scheme", "color_scheme toggle" },
	{ "wifi-on", "Turn Wi-Fi on", NULL, "network-wireless", "wlan wireless radio",
		"exec nmcli radio wifi on" },
	{ "wifi-off", "Turn Wi-Fi off", NULL, "network-wireless-offline", "wlan wireless radio airplane",
		"exec nmcli radio wifi off" },
	{ "network", "Networks", "Join a Wi-Fi network", "network-wireless", "wifi wlan connect",
		"panel network" },
	{ "bluetooth", "Bluetooth", "Devices nearby", "bluetooth", "headphones pair", "panel bluetooth" },
	{ "dnd", "Do not disturb", "Keep notifications quiet", "notifications-disabled",
		"focus quiet silence notifications", "panel dnd toggle" },
	{ "notifications", "Notifications", "What came in lately", "preferences-system-notifications",
		"action center messages", "panel notifications" },
	{ "quick", "Quick settings", "Volume, brightness, toggles", "preferences-system",
		"control center volume brightness", "panel quicksettings" },
	{ "taskview", "Task view", "Every window and desktop", "view-grid", "overview expose windows",
		"taskview" },
	{ "desktop", "Show the desktop", "Every window out of the way", "user-desktop",
		"minimize all", "showdesktop" },
	{ "mode", "Switch tile and window mode", NULL, "view-app-grid", "tiling floating layout",
		"wm_mode toggle" },
	{ "snip", "Take a screenshot", "A part of the screen, a window or all of it",
		"applets-screenshooter", "screen capture snip print", "panel snip" },
	{ "clipboard", "Clipboard history", "What was copied lately", "edit-paste",
		"paste copied", "panel clipboard" },
	{ "emoji", "Emoji and symbols", NULL, "face-smile", "smiley symbol character",
		"panel emoji" },
	{ "shortcuts", "Keyboard shortcuts", "Every key worth knowing", "input-keyboard",
		"keys bindings help", "panel shortcuts" },
	{ "settings", "Settings", NULL, "preferences-system", "preferences options control panel",
		"exec tilewin-settings" },
	{ "terminal", "Terminal", NULL, "utilities-terminal", "shell console command line",
		"exec $term" },
};

static void add_actions(struct cc *c) {
	for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
		char key[64];
		snprintf(key, sizeof(key), "act:%s", actions[i].key);
		const char *command = actions[i].command;
		char *own = NULL;
		if (strcmp(command, "exec $term") == 0) {
			// the terminal of taskbar.conf
			own = format_str("exec %s", c->panel->config && c->panel->config->terminal ?
				c->panel->config->terminal : "xfce4-terminal");
			command = own;
		}
		struct cc_item *it = item_new(CC_ACTION, key, actions[i].name, actions[i].detail,
			actions[i].icon, command);
		it->keywords = strdup(actions[i].keywords);
		list_add(c->items, it);
		free(own);
	}
	// another theme, one action each
	list_t *themes = tw_theme_list();
	for (int i = 0; themes && i < themes->length; i++) {
		const char *name = themes->items[i];
		char *error = NULL;
		struct tw_theme *t = tw_theme_load(name, &error);
		free(error);
		char key[160], title[200], cmd[160];
		snprintf(key, sizeof(key), "theme:%s", name);
		snprintf(title, sizeof(title), "Theme: %s", t && t->title ? t->title : name);
		snprintf(cmd, sizeof(cmd), "exec tilewin-theme set %s", name);
		struct cc_item *it = item_new(CC_ACTION, key, title, "Change the look",
			"preferences-desktop-theme", cmd);
		it->keywords = strdup("theme look style");
		list_add(c->items, it);
		if (t) {
			tw_theme_free(t);
		}
	}
	if (themes) {
		list_free_items_and_destroy(themes);
	}
}

/* ---------- finding ---------- */

static int item_cmp(const void *a, const void *b) {
	const struct cc_item *x = *(struct cc_item **)a, *y = *(struct cc_item **)b;
	return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}

static void add_heading(struct cc *c, const char *text) {
	struct cc_item *h = item_new(CC_ACTION, "", text, NULL, NULL, NULL);
	h->heading = true;
	list_add(c->headings, h);
	list_add(c->shown, h);
}

/* What the extras found (sums, settings pages, files), as items of the list. */
static void take_extras(struct cc *c, bool top) {
	const list_t *found = top ? search_extras_top(c->extras) : search_extras_below(c->extras);
	for (int i = 0; i < found->length; i++) {
		const struct tw_desktop_entry *e = found->items[i];
		enum cc_kind kind = top ? CC_SUM : e->id && strstr(e->id, "settings") ?
			CC_SETTING : CC_FILE;
		// the same id for every page or file: the name makes them apart in the history
		char *key = format_str("%s:%s", e->id ? e->id : "extra", e->name ? e->name : "");
		struct cc_item *it = item_new(kind, key, e->name, e->generic_name ? e->generic_name :
			e->comment, e->icon, NULL);
		free(key);
		it->entry = e;
		list_add(c->extra_items, it);
	}
}

static void update(struct cc *c, bool typed) {
	c->shown->length = 0;
	items_free(c->headings);
	c->headings = create_list();
	items_free(c->extra_items);
	c->extra_items = create_list();
	long long now = (long long)time(NULL);

	// a sign before the words keeps to one kind
	const char *q = c->query;
	int only = -1;
	if (q[0] == '>' || q[0] == '@' || q[0] == '#') {
		only = q[0] == '>' ? CC_ACTION : q[0] == '@' ? CC_WINDOW : CC_SETTING;
		q++;
		while (*q == ' ') {
			q++;
		}
	}

	if (!c->query[0]) {
		// nothing typed: the open windows, then what is used most and lately
		bool any = false;
		for (int i = 0; i < c->items->length; i++) {
			struct cc_item *it = c->items->items[i];
			if (it->kind == CC_WINDOW) {
				if (!any) {
					add_heading(c, "Open windows");
					any = true;
				}
				list_add(c->shown, it);
			}
		}
		list_t *frequent = create_list();
		for (int i = 0; i < c->items->length; i++) {
			struct cc_item *it = c->items->items[i];
			it->score = tw_cmd_frecency(&c->history, it->key, now);
			if (it->kind != CC_WINDOW && it->score > 0) {
				list_add(frequent, it);
			}
		}
		list_qsort(frequent, item_cmp);
		if (frequent->length) {
			add_heading(c, "Used often");
			for (int i = 0; i < frequent->length && i < 8; i++) {
				list_add(c->shown, frequent->items[i]);
			}
		}
		list_free(frequent);
		// for a start: a few things worth knowing it can do
		if (c->shown->length < ROWS) {
			add_heading(c, "Try");
			static const char *const tries[] = { "act:taskview", "act:snip", "act:dark",
				"act:settings", "act:lock" };
			for (size_t k = 0; k < sizeof(tries) / sizeof(tries[0]); k++) {
				for (int i = 0; i < c->items->length; i++) {
					struct cc_item *it = c->items->items[i];
					if (strcmp(it->key, tries[k]) == 0 && list_find(c->shown, it) < 0) {
						list_add(c->shown, it);
					}
				}
			}
		}
	} else {
		search_extras_set_query(c->extras, only < 0 ? q : only == CC_SETTING ? q : "");
		list_t *found = create_list();
		for (int i = 0; i < c->items->length; i++) {
			struct cc_item *it = c->items->items[i];
			if (only >= 0 && (int)it->kind != only) {
				continue;
			}
			int match = tw_cmd_match(it->name, it->keywords, q);
			if (match == 0 && it->detail) {
				match = tw_cmd_match(it->detail, NULL, q) / 3;
			}
			if (match == 0) {
				continue;
			}
			it->score = match + tw_cmd_frecency(&c->history, it->key, now);
			// a window of what is meant comes before starting it again
			if (it->kind == CC_WINDOW) {
				it->score += 12;
			}
			list_add(found, it);
		}
		take_extras(c, true);
		take_extras(c, false);
		for (int i = 0; i < c->extra_items->length; i++) {
			struct cc_item *it = c->extra_items->items[i];
			if (only >= 0 && (int)it->kind != only) {
				continue;
			}
			// a sum is what was meant when it works out; settings and files below the apps
			it->score = it->kind == CC_SUM ? 1000 : it->kind == CC_SETTING ? 50 : 20;
			it->score += tw_cmd_frecency(&c->history, it->key, now);
			list_add(found, it);
		}
		list_qsort(found, item_cmp);
		for (int i = 0; i < found->length; i++) {
			list_add(c->shown, found->items[i]);
		}
		list_free(found);
		if (only < 0) {
			free(c->run.name);
			c->run.name = format_str("Run \"%s\"", q);
			free(c->run.command);
			c->run.command = format_str("exec %s", q);
			list_add(c->shown, &c->run);
		}
	}
	if (typed) {
		c->selected = 0;
		c->scroll = 0;
	}
	// the first row that can be chosen
	while (c->selected < c->shown->length &&
			((struct cc_item *)c->shown->items[c->selected])->heading) {
		c->selected++;
	}
	if (c->selected >= c->shown->length) {
		c->selected = c->shown->length - 1;
	}
	c->alt = -1;
}

static void extras_updated(void *data) {
	struct popup *p = data;
	struct cc *c = p->data;
	if (c->query[0]) {
		int selected = c->selected;
		update(c, false);
		c->selected = selected < c->shown->length ? selected : 0;
	}
	popup_set_dirty(p);
}

/* ---------- doing ---------- */

static void run(struct popup *p, int index, int alt) {
	struct cc *c = p->data;
	if (index < 0 || index >= c->shown->length) {
		return;
	}
	struct cc_item *it = c->shown->items[index];
	if (it->heading) {
		return;
	}
	if (it->key && *it->key) {
		tw_cmd_history_note(&c->history, it->key, (long long)time(NULL));
		char *path = tw_cmd_history_path();
		tw_cmd_history_save(&c->history, path);
		free(path);
	}
	if (alt >= 0 && alt < it->alt_count) {
		bar_run_command(c->panel, it->alts[alt].command, NULL);
	} else if (it->entry) {
		apps_launch(c->panel, it->entry);
	} else if (it->command) {
		bar_run_command(c->panel, it->command, NULL);
	}
	popup_close_later(c->panel);
}

static void move(struct cc *c, int step) {
	int n = c->shown->length, i = c->selected;
	do {
		i += step;
	} while (i >= 0 && i < n && ((struct cc_item *)c->shown->items[i])->heading);
	if (i >= 0 && i < n) {
		c->selected = i;
		c->alt = -1;
	}
}

/* ---------- drawing ---------- */

static int row_h(const struct cc_item *it) {
	return it->heading ? HEAD_H : ROW_H;
}

static void cc_render(struct popup *p, cairo_t *cr) {
	struct cc *c = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	int M = popup_shadow_margin(panel);
	double W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "menu");
	uint32_t fg = tw_theme_color(t, "menu.fg", 0x1b1d22ff);
	uint32_t dim = tw_theme_color(t, "menu.disabled_fg", 0x8a8f99ff);
	uint32_t accent = tw_theme_color(t, "accent.color", tw_theme_color(t, "taskbar.indicator",
		0x5b6cf0ff)) | 0xff;
	uint32_t hl = tw_theme_color(t, "menu.hl_bg", (accent & 0xffffff00) | 0x26);
	const char *font = tw_theme_str(t, "menu.font", bar_font(panel));
	const char *bold = bar_bold_font(panel);
	char big[160];
	snprintf(big, sizeof(big), "%s", tw_theme_str(t, "commandcenter.field_font", font));

	// the field: what was typed, big
	double fx = M + 18, fy = M, fw = W - 2 * M - 36;
	pd_glyph_search(cr, fx, fy + (FIELD_H - 20) / 2.0, 20, dim);
	if (!c->query[0]) {
		pd_text(cr, big, "Type to do anything: apps, windows, settings, actions, files, sums",
			fx + 34, fy, fw - 34, FIELD_H, dim, PD_LEFT);
	}
	struct text_style ts = { .font = big, .fg = fg, .caret = true };
	text_style_colors(panel, &ts);
	text_draw(cr, &ts, c->query, &c->tc, fx + 34, fy, fw - 34, FIELD_H);
	pd_rect(cr, M, M + FIELD_H, W - 2 * M, 1, (fg & 0xffffff00) | 0x1a);

	// the rows
	double y = M + FIELD_H + 6;
	double list_bottom = H - M - FOOT_H - 4;
	if (c->selected < c->scroll) {
		c->scroll = c->selected;
	}
	// keep the chosen row in sight
	for (;;) {
		double h = 0;
		for (int i = c->scroll; i <= c->selected && i < c->shown->length; i++) {
			h += row_h(c->shown->items[i]);
		}
		if (y + h <= list_bottom || c->scroll >= c->selected) {
			break;
		}
		c->scroll++;
	}
	if (c->shown->length == 0) {
		pd_text(cr, font, "Nothing found", M + 24, y, W - 2 * M - 48, ROW_H, dim, PD_LEFT);
	}
	for (int i = c->scroll; i < c->shown->length; i++) {
		struct cc_item *it = c->shown->items[i];
		double h = row_h(it);
		if (y + h > list_bottom) {
			break;
		}
		double rx = M + 8, rw = W - 2 * M - 16;
		if (it->heading) {
			char *up = g_utf8_strup(it->name, -1);
			pd_text(cr, font, up, rx + 12, y + 4, rw - 24, h - 4, dim, PD_LEFT);
			g_free(up);
			y += h;
			continue;
		}
		bool sel = i == c->selected;
		bool hover = c->inside && c->px >= rx && c->px < rx + rw && c->py >= y && c->py < y + h;
		if (sel || hover) {
			pd_rounded(cr, rx, y + 2, rw, h - 4, 10);
			pd_color(cr, sel ? hl : (hl & 0xffffff00) | ((hl & 0xff) / 2));
			cairo_fill(cr);
		}
		cairo_surface_t *icon = it->icon ? apps_icon(panel, it->icon, 28 * p->surface->scale) :
			NULL;
		if (icon) {
			pd_icon(cr, icon, rx + 12, y + (h - 28) / 2, 28);
		} else {
			pd_rounded(cr, rx + 12, y + (h - 28) / 2, 28, 28, 8);
			pd_color(cr, (accent & 0xffffff00) | 0x40);
			cairo_fill(cr);
		}
		double tx = rx + 52;
		// at the right: what it is, or for the chosen row what else it can do
		double right = rx + rw - 12;
		if (sel && it->alt_count > 0) {
			for (int k = it->alt_count - 1; k >= 0; k--) {
				char label[48];
				snprintf(label, sizeof(label), "%s  Alt+%d", it->alts[k].label, k + 1);
				int lw = 0;
				pd_text_size(cr, font, label, &lw, NULL);
				double cw = lw + 20, cx = right - cw;
				bool on = c->alt == k;
				pd_rounded(cr, cx, y + (h - 26) / 2, cw, 26, 13);
				pd_color(cr, on ? accent : (fg & 0xffffff00) | 0x14);
				cairo_fill(cr);
				pd_text(cr, font, label, cx, y + (h - 26) / 2, cw, 26, on ? 0xffffffff : fg,
					PD_CENTER);
				psurface_add_hotspot(p->surface, cx, y, cw, h, NULL, 2, i * 8 + k, NULL);
				right = cx - 6;
			}
		} else {
			const char *kind = kind_names[it->kind];
			int kw = 0;
			pd_text_size(cr, font, kind, &kw, NULL);
			pd_text(cr, font, kind, right - kw, y, kw, h, dim, PD_LEFT);
			right -= kw + 12;
		}
		double tw = right - tx - 8;
		if (it->detail && *it->detail) {
			pd_text(cr, bold, it->name, tx, y + 4, tw, h / 2.0, fg, PD_LEFT);
			pd_text(cr, font, it->detail, tx, y + h / 2.0 - 3, tw, h / 2.0, dim, PD_LEFT);
		} else {
			pd_text(cr, bold, it->name, tx, y, tw, h, fg, PD_LEFT);
		}
		psurface_add_hotspot(p->surface, rx, y, right - rx, h, NULL, 1, i, NULL);
		y += h;
	}

	// the keys, at the bottom
	double by = H - M - FOOT_H;
	pd_rect(cr, M, by, W - 2 * M, 1, (fg & 0xffffff00) | 0x1a);
	pd_text(cr, font, "Enter do it  ·  Tab more  ·  > actions   @ windows   # settings   = sums"
		"  ·  Esc close", M + 18, by, W - 2 * M - 36, FOOT_H, dim, PD_LEFT);
}

/* ---------- input ---------- */

static void cc_motion(struct popup *p, double x, double y) {
	struct cc *c = p->data;
	c->px = x;
	c->py = y;
	c->inside = true;
	popup_set_dirty(p);
}

static void cc_leave(struct popup *p) {
	struct cc *c = p->data;
	c->inside = false;
	popup_set_dirty(p);
}

static void cc_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == 1) {
		run(p, (int)hs->id, -1);
	} else if (hs && hs->kind == 2) {
		run(p, (int)hs->id / 8, (int)hs->id % 8);
	}
}

static void cc_axis(struct popup *p, double x, double y, int direction) {
	move(p->data, direction > 0 ? 1 : -1);
	popup_set_dirty(p);
}

static void cc_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct cc *c = p->data;
	struct cc_item *sel = c->selected >= 0 && c->selected < c->shown->length ?
		c->shown->items[c->selected] : NULL;
	bool alt = mods & TEXT_MOD_ALT;
	if (alt && sym >= XKB_KEY_1 && sym <= XKB_KEY_3) {
		run(p, c->selected, (int)(sym - XKB_KEY_1));
		return;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		if (c->query[0]) {
			c->query[0] = '\0';
			c->tc = (struct text_cursor){ 0 };
			update(c, true);
			break;
		}
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		run(p, c->selected, c->alt);
		return;
	case XKB_KEY_Down:
		move(c, 1);
		break;
	case XKB_KEY_Up:
		move(c, -1);
		break;
	case XKB_KEY_Page_Down:
		for (int i = 0; i < ROWS; i++) {
			move(c, 1);
		}
		break;
	case XKB_KEY_Page_Up:
		for (int i = 0; i < ROWS; i++) {
			move(c, -1);
		}
		break;
	case XKB_KEY_Tab:
		// through what else the chosen row can do, and back to what Enter does
		if (sel && sel->alt_count > 0) {
			c->alt = c->alt + 1 < sel->alt_count ? c->alt + 1 : -1;
		} else {
			move(c, 1);
		}
		break;
	case XKB_KEY_ISO_Left_Tab:
		if (sel && sel->alt_count > 0) {
			c->alt = c->alt >= 0 ? c->alt - 1 : sel->alt_count - 1;
		} else {
			move(c, -1);
		}
		break;
	default:
		switch (text_key(c->query, sizeof(c->query), &c->tc, sym, utf8, mods)) {
		case TEXT_KEY_IGNORED:
			return;
		case TEXT_KEY_CHANGED:
			update(c, true);
			break;
		case TEXT_KEY_MOVED:
			break;
		}
		break;
	}
	popup_set_dirty(p);
}

static void cc_destroy(struct popup *p) {
	struct cc *c = p->data;
	search_extras_free(c->extras);
	items_free(c->items);
	items_free(c->headings);
	items_free(c->extra_items);
	list_free(c->shown);
	free(c->run.name);
	free(c->run.command);
	tw_desktop_list_free(c->apps);
	tw_cmd_history_free(&c->history);
	free(c);
}

static const struct popup_vtable cc_vtable = {
	.render = cc_render,
	.motion = cc_motion,
	.leave = cc_leave,
	.button = cc_button,
	.axis = cc_axis,
	.key = cc_key,
	.destroy = cc_destroy,
};

void command_center_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_LAUNCHER)) {
		popup_close_all(panel);
		return;
	}
	if (!output) {
		return;
	}
	popup_close_all(panel);
	struct cc *c = calloc(1, sizeof(*c));
	c->panel = panel;
	list_t *all = tw_desktop_scan();
	c->apps = create_list();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (e->no_display || e->hidden || !e->exec) {
			tw_desktop_entry_free(e);
		} else {
			list_add(c->apps, e);
		}
	}
	list_free(all);
	c->items = create_list();
	c->shown = create_list();
	c->headings = create_list();
	c->extra_items = create_list();
	c->run.kind = CC_RUN;
	c->run.key = strdup("");
	c->run.icon = strdup("system-run");
	c->run.detail = strdup("Run this as a command");
	char *path = tw_cmd_history_path();
	tw_cmd_history_load(&c->history, path);
	free(path);
	add_windows(c);
	add_apps(c);
	add_actions(c);
	c->extras = search_extras_new(panel, NULL, NULL);
	update(c, true);

	int M = popup_shadow_margin(panel);
	int width = CC_WIDTH + 2 * M;
	int height = FIELD_H + 6 + ROWS * ROW_H + 2 * HEAD_H + FOOT_H + 8 + 2 * M;
	if (height > output->height - 40) {
		height = output->height - 40;
	}
	int x = (output->width - width) / 2;
	int y = output->height / 7;
	if (y + height > output->height) {
		y = output->height - height;
	}
	struct popup *p = popup_create(panel, POPUP_LAUNCHER, NULL, output, x, y, width, height,
		&cc_vtable, c);
	if (p) {
		search_extras_set_listener(c->extras, extras_updated, p);
	}
}
