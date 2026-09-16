#define _GNU_SOURCE
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "draw.h"
#include "log.h"
#include "popup.h"
#include "textfield.h"
#include "stringop.h"
#include "tw_desktop.h"

/*
 * Start menu with four theme-dependent layouts:
 *   classic   - Windows 95 cascading menu with a side banner
 *   twocolumn - Windows XP / 7: pinned apps left, places right
 *   list      - Windows 10: side strip + alphabetical list
 *   centered  - Windows 11: search box, pinned grid, footer
 *   tiles     - Windows 8: a start screen of colored tiles over the whole screen
 */

enum sm_layout {
	SM_CLASSIC,
	SM_TWOCOLUMN,
	SM_LIST,
	SM_CENTERED,
	SM_TILES,
};

enum sm_hotspot {
	HS_APP = 1,
	HS_PLACE,
	HS_POWER,
	HS_ALLAPPS,
	HS_BACK,
	HS_SEARCH,
	HS_THEMES,
	HS_RUN,
};

struct place {
	char *label;
	char *icon;
	char *command;
};

struct startmenu {
	enum sm_layout layout;
	list_t *apps;   // struct tw_desktop_entry *, owned
	list_t *pinned; // borrowed from apps
	list_t *places; // struct place *
	char user[128];
	char search[256];
	struct text_cursor tc;
	bool show_search;
	bool all_apps;
	int scroll;
	int max_scroll;
	int selected;
	bool scroll_to_selected; // the keyboard moved the selection
	list_t *hits; // borrowed entries in display order
	bool inside;
	double px, py;
};

/* ---------- data ---------- */

static int entry_name_cmp(const void *a, const void *b) {
	const struct tw_desktop_entry *ea = *(struct tw_desktop_entry **)a;
	const struct tw_desktop_entry *eb = *(struct tw_desktop_entry **)b;
	return strcasecmp(ea->name, eb->name);
}

static list_t *load_apps(void) {
	list_t *all = tw_desktop_scan();
	list_t *apps = create_list();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (e->no_display || e->hidden || !e->exec) {
			tw_desktop_entry_free(e);
		} else {
			list_add(apps, e);
		}
	}
	list_free(all);
	list_qsort(apps, entry_name_cmp);
	return apps;
}

static struct tw_desktop_entry *find_app(list_t *apps, const char *id) {
	size_t len = strlen(id);
	for (int i = 0; i < apps->length; i++) {
		struct tw_desktop_entry *e = apps->items[i];
		if (strcmp(e->id, id) == 0 || (strlen(e->id) == len + 8 &&
				strncmp(e->id, id, len) == 0)) {
			return e;
		}
	}
	return NULL;
}

static void load_config(struct panel *panel, struct startmenu *sm) {
	sm->pinned = create_list();
	sm->places = create_list();
	struct twconf_node *conf = panel->config ? panel->config->startmenu : NULL;
	for (int i = 0; conf && i < twconf_count(conf); i++) {
		struct twconf_node *child = twconf_at(conf, i);
		if (strcmp(child->name, "pinned") == 0) {
			for (int j = 0; j < child->argc; j++) {
				struct tw_desktop_entry *e = find_app(sm->apps, child->argv[j]);
				if (e && list_find(sm->pinned, e) < 0) {
					list_add(sm->pinned, e);
				}
			}
		} else if (strcmp(child->name, "place") == 0 && child->argc >= 3) {
			struct place *pl = calloc(1, sizeof(*pl));
			pl->label = strdup(child->argv[0]);
			pl->icon = strdup(child->argv[1]);
			pl->command = twconf_join(child, 2);
			if (strcmp(pl->command, "exec xdg-open ~/.config/tileWin") == 0) {
				free(pl->command); // older default, before the settings app
				pl->command = strdup("exec tilewin-settings");
			}
			list_add(sm->places, pl);
		} else if (strcmp(child->name, "user_name") == 0 && child->argc >= 1) {
			snprintf(sm->user, sizeof(sm->user), "%s", child->argv[0]);
		}
	}
	static const char *default_pinned[] = {
		"xfce4-terminal.desktop", "thunar.desktop", "org.xfce.mousepad.desktop",
		"firefox.desktop", "chromium.desktop", "libreoffice-startcenter.desktop",
		"gimp.desktop", "btop.desktop",
	};
	if (!conf || sm->pinned->length == 0) {
		for (size_t i = 0; i < sizeof(default_pinned) / sizeof(default_pinned[0]); i++) {
			struct tw_desktop_entry *e = find_app(sm->apps, default_pinned[i]);
			if (e && list_find(sm->pinned, e) < 0) {
				list_add(sm->pinned, e);
			}
		}
	}
	for (int i = 0; i < sm->apps->length && sm->pinned->length < 12; i++) {
		if (list_find(sm->pinned, sm->apps->items[i]) < 0) {
			list_add(sm->pinned, sm->apps->items[i]);
		}
	}
	if (sm->places->length == 0) {
		static const char *defaults[][3] = {
			{ "Home", "user-home", "exec xdg-open ~" },
			{ "Documents", "folder-documents", "exec xdg-open ~/Documents" },
			{ "Pictures", "folder-pictures", "exec xdg-open ~/Pictures" },
			{ "Music", "folder-music", "exec xdg-open ~/Music" },
			{ "Downloads", "folder-download", "exec xdg-open ~/Downloads" },
			{ "Settings", "preferences-system", "exec tilewin-settings" },
		};
		for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
			struct place *pl = calloc(1, sizeof(*pl));
			pl->label = strdup(defaults[i][0]);
			pl->icon = strdup(defaults[i][1]);
			pl->command = strdup(defaults[i][2]);
			list_add(sm->places, pl);
		}
	}
	if (!sm->user[0]) {
		struct passwd *pw = getpwuid(getuid());
		const char *name = NULL;
		if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',') {
			snprintf(sm->user, sizeof(sm->user), "%s", pw->pw_gecos);
			char *comma = strchr(sm->user, ',');
			if (comma) {
				*comma = '\0';
			}
		} else {
			name = pw ? pw->pw_name : getenv("USER");
			snprintf(sm->user, sizeof(sm->user), "%s", name ? name : "User");
		}
	}
}

static int match_score(const struct tw_desktop_entry *e, const char *q) {
	if (!*q) {
		return 1;
	}
	if (strncasecmp(e->name, q, strlen(q)) == 0) {
		return 4;
	}
	if (strcasestr(e->name, q)) {
		return 3;
	}
	if ((e->generic_name && strcasestr(e->generic_name, q)) ||
			(e->keywords && strcasestr(e->keywords, q))) {
		return 2;
	}
	if (e->exec && strcasestr(e->exec, q)) {
		return 1;
	}
	return 0;
}

static list_t *search_results(struct startmenu *sm) {
	list_t *results = create_list();
	for (int score = 4; score >= 1; score--) {
		for (int i = 0; i < sm->apps->length; i++) {
			struct tw_desktop_entry *e = sm->apps->items[i];
			if (match_score(e, sm->search) == score) {
				list_add(results, e);
			}
		}
	}
	return results;
}

static char *app_command(struct panel *panel, const struct tw_desktop_entry *e) {
	char *cmd = tw_desktop_exec_command(e);
	if (!cmd) {
		return NULL;
	}
	char *full = e->terminal && panel->config ?
		format_str("exec %s %s", panel->config->terminal, cmd) : format_str("exec %s", cmd);
	free(cmd);
	return full;
}

/*
 * Windows 95 and XP show a shut down dialog, the newer looks a menu.
 * startmenu.power_dialog in the theme changes that.
 */
static bool power_dialog(struct panel *panel) {
	enum pstyle style = panel_style(panel);
	return tw_theme_bool(panel->theme, "startmenu.power_dialog",
		style == PS_CLASSIC || style == PS_LUNA);
}

static void open_shutdown_later(void *data) {
	shutdown_dialog_open(data, NULL, false);
}

static void open_logoff_later(void *data) {
	shutdown_dialog_open(data, NULL, true);
}

/* ---------- classic (Windows 95) ---------- */

static const struct {
	const char *category;
	const char *label;
	const char *icon;
} categories[] = {
	{ "Utility", "Accessories", "applications-accessories" },
	{ "Development", "Development", "applications-development" },
	{ "Education", "Education", "applications-science" },
	{ "Game", "Games", "applications-games" },
	{ "Graphics", "Graphics", "applications-graphics" },
	{ "Network", "Internet", "applications-internet" },
	{ "AudioVideo", "Multimedia", "applications-multimedia" },
	{ "Audio", "Multimedia", "applications-multimedia" },
	{ "Video", "Multimedia", "applications-multimedia" },
	{ "Office", "Office", "applications-office" },
	{ "Science", "Science", "applications-science" },
	{ "Settings", "Settings", "preferences-system" },
	{ "System", "System Tools", "applications-system" },
};

static const char *category_label(const struct tw_desktop_entry *e, const char **icon) {
	if (e->categories) {
		for (size_t c = 0; c < sizeof(categories) / sizeof(categories[0]); c++) {
			char *copy = strdup(e->categories);
			char *save = NULL;
			bool found = false;
			for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
				if (strcmp(tok, categories[c].category) == 0) {
					found = true;
					break;
				}
			}
			free(copy);
			if (found) {
				*icon = categories[c].icon;
				return categories[c].label;
			}
		}
	}
	*icon = "applications-other";
	return "Other";
}

static void open_classic(struct panel *panel, struct panel_output *output) {
	list_t *apps = load_apps();
	struct startmenu tmp = { .apps = apps };
	load_config(panel, &tmp);

	struct menu_item *programs = menu_item_new("Programs", NULL);
	programs->icon = strdup("applications-other");
	programs->children = create_list();
	for (int i = 0; i < apps->length; i++) {
		struct tw_desktop_entry *e = apps->items[i];
		const char *icon;
		const char *label = category_label(e, &icon);
		struct menu_item *group = NULL;
		for (int j = 0; j < programs->children->length; j++) {
			struct menu_item *g = programs->children->items[j];
			if (strcmp(g->label, label) == 0) {
				group = g;
				break;
			}
		}
		if (!group) {
			group = menu_item_new(label, NULL);
			group->icon = strdup(icon);
			group->children = create_list();
			// keep groups alphabetical
			int pos = 0;
			while (pos < programs->children->length && strcasecmp(
					((struct menu_item *)programs->children->items[pos])->label, label) < 0) {
				pos++;
			}
			list_insert(programs->children, pos, group);
		}
		char *cmd = app_command(panel, e);
		struct menu_item *item = menu_item_new(e->name, cmd);
		item->icon = e->icon ? strdup(e->icon) : NULL;
		list_add(group->children, item);
		free(cmd);
	}

	struct menu_item *places = menu_item_new("Documents", NULL);
	places->icon = strdup("folder-documents");
	places->children = create_list();
	for (int i = 0; i < tmp.places->length; i++) {
		struct place *pl = tmp.places->items[i];
		struct menu_item *item = menu_item_new(pl->label, pl->command);
		item->icon = strdup(pl->icon);
		list_add(places->children, item);
	}

	struct menu_item *settings = menu_item_new("Settings", NULL);
	settings->icon = strdup("preferences-system");
	settings->children = create_list();
	list_add(settings->children, menu_item_new("tileWin Settings...", "exec tilewin-settings"));
	list_add(settings->children, menu_item_new("Taskbar...",
		"exec xdg-open ~/.config/tileWin/taskbar.conf"));
	list_add(settings->children, menu_item_new("Window mode shortcuts...",
		"exec xdg-open ~/.config/tileWin/windowmode.conf"));
	list_add(settings->children, menu_item_new("Tile mode config...",
		"exec xdg-open ~/.config/tileWin/tilemode.conf"));
	struct menu_item *themes = menu_item_new("Theme", NULL);
	themes->children = theme_menu_items(panel);
	list_add(settings->children, themes);

	bool dialog = power_dialog(panel);
	struct menu_item *power = menu_item_new("Shut Down...", dialog ? "panel shutdown" : NULL);
	power->icon = strdup("system-shutdown");
	if (!dialog) {
		power->children = power_menu_items(panel);
	}

	list_t *items = create_list();
	list_add(items, programs);
	list_add(items, places);
	list_add(items, settings);
	struct menu_item *run = menu_item_new("Run...", "panel run");
	run->icon = strdup("system-run");
	list_add(items, run);
	list_add(items, menu_item_separator());
	list_add(items, power);

	for (int i = 0; i < tmp.places->length; i++) {
		struct place *pl = tmp.places->items[i];
		free(pl->label);
		free(pl->icon);
		free(pl->command);
		free(pl);
	}
	list_free(tmp.places);
	list_free(tmp.pinned);
	tw_desktop_list_free(apps);

	int bar = output->bar ? output->bar->height : 28;
	bool bottom = panel->config->layouts[panel->layout].bottom;
	menu_open_start_classic(panel, output, items, 2, bottom ? output->height - bar + 2 : bar + 300);
}

/* ---------- drawing helpers ---------- */

struct sm_ctx {
	struct popup *p;
	struct startmenu *sm;
	struct panel *panel;
	cairo_t *cr;
	int scale;
};

static bool hovered(struct sm_ctx *c, double x, double y, double w, double h) {
	return c->sm->inside && c->sm->px >= x && c->sm->py >= y &&
		c->sm->px < x + w && c->sm->py < y + h;
}

static void draw_avatar(cairo_t *cr, double x, double y, double size, uint32_t bg,
		uint32_t fg, bool round) {
	if (round) {
		cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
	} else {
		pd_rounded(cr, x, y, size, size, size * 0.12);
	}
	pd_color(cr, bg);
	cairo_fill(cr);
	pd_color(cr, fg);
	cairo_arc(cr, x + size / 2, y + size * 0.38, size * 0.17, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_save(cr);
	if (round) {
		cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
		cairo_clip(cr);
	}
	cairo_arc(cr, x + size / 2, y + size * 0.95, size * 0.33, M_PI, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void add_hit(struct sm_ctx *c, struct tw_desktop_entry *e) {
	list_add(c->sm->hits, e);
}

/* Draws one app row and registers its hotspot. Returns the row height. */
static void draw_app_row(struct sm_ctx *c, struct tw_desktop_entry *e, double x, double y,
		double w, double h, int icon_size, const char *font, uint32_t fg, uint32_t hl_bg,
		uint32_t hl_fg, double radius, const char *subtitle, uint32_t sub_fg) {
	int index = list_find(c->sm->apps, e);
	int hit_index = c->sm->hits->length;
	add_hit(c, e);
	bool sel = hovered(c, x, y, w, h) || hit_index == c->sm->selected;
	if (sel) {
		pd_rounded(c->cr, x, y + 1, w, h - 2, radius);
		pd_color(c->cr, hl_bg);
		cairo_fill(c->cr);
	}
	cairo_surface_t *icon = e->icon ? apps_icon(c->panel, e->icon, icon_size * c->scale) : NULL;
	pd_icon(c->cr, icon, x + 6, y + (h - icon_size) / 2.0, icon_size);
	double tx = x + icon_size + 14;
	uint32_t color = sel ? hl_fg : fg;
	if (subtitle && *subtitle) {
		pd_text(c->cr, font, e->name, tx, y + 2, w - (tx - x) - 6, h / 2.0, color, PD_LEFT);
		pd_text(c->cr, bar_font(c->panel), subtitle, tx, y + h / 2.0 - 2, w - (tx - x) - 6,
			h / 2.0, sel ? hl_fg : sub_fg, PD_LEFT);
	} else {
		pd_text(c->cr, font, e->name, tx, y, w - (tx - x) - 6, h, color, PD_LEFT);
	}
	psurface_add_hotspot(c->p->surface, x, y, w, h, NULL, HS_APP, index, NULL);
}

static void draw_simple_row(struct sm_ctx *c, int kind, int64_t id, const char *icon_name,
		const char *label, double x, double y, double w, double h, int icon_size,
		const char *font, uint32_t fg, uint32_t hl_bg, uint32_t hl_fg, double radius,
		bool arrow) {
	bool sel = hovered(c, x, y, w, h);
	if (sel) {
		pd_rounded(c->cr, x, y + 1, w, h - 2, radius);
		pd_color(c->cr, hl_bg);
		cairo_fill(c->cr);
	}
	double tx = x + 8;
	if (icon_size > 0) {
		cairo_surface_t *icon = icon_name ? apps_icon(c->panel, icon_name, icon_size * c->scale) : NULL;
		if (icon) {
			pd_icon(c->cr, icon, x + 6, y + (h - icon_size) / 2.0, icon_size);
		}
		tx = x + icon_size + 14;
	}
	uint32_t color = sel ? hl_fg : fg;
	pd_text(c->cr, font, label, tx, y, w - (tx - x) - (arrow ? 22 : 6), h, color, PD_LEFT);
	if (arrow) {
		pd_glyph_arrow(c->cr, x + w - 18, y + (h - 12) / 2.0, 12, 0, color);
	}
	psurface_add_hotspot(c->p->surface, x, y, w, h, NULL, kind, id, NULL);
}

static char header_letter(const struct tw_desktop_entry *e) {
	char letter = (char)toupper((unsigned char)e->name[0]);
	return isalpha((unsigned char)letter) ? letter : '#';
}

/* Scrollable list of apps with letter headers; returns content height. */
static int draw_app_list(struct sm_ctx *c, list_t *entries, bool headers, double x, double y,
		double w, double h, int row_h, int icon_size, uint32_t fg, uint32_t header_fg,
		uint32_t hl_bg, uint32_t hl_fg, double radius) {
	struct startmenu *sm = c->sm;
	cairo_save(c->cr);
	cairo_rectangle(c->cr, x, y, w, h);
	cairo_clip(c->cr);
	int base = sm->hits->length;
	if (sm->scroll_to_selected && sm->selected >= base &&
			sm->selected < base + entries->length) {
		// keep the row selected with the arrow keys visible
		double top = 0;
		char prev = 0;
		for (int i = 0; i <= sm->selected - base; i++) {
			char letter = header_letter(entries->items[i]);
			if (headers && letter != prev) {
				prev = letter;
				top += row_h;
			}
			if (i < sm->selected - base) {
				top += row_h;
			}
		}
		if (top < sm->scroll) {
			sm->scroll = (int)top;
		} else if (top + row_h > sm->scroll + h) {
			sm->scroll = (int)ceil(top + row_h - h);
		}
		sm->scroll_to_selected = false;
	}
	double cy = y - sm->scroll;
	char last = 0;
	for (int i = 0; i < entries->length; i++) {
		struct tw_desktop_entry *e = entries->items[i];
		if (headers) {
			char letter = header_letter(e);
			if (letter != last) {
				last = letter;
				char text[2] = { letter, 0 };
				if (cy + row_h > y && cy < y + h) {
					pd_text(c->cr, bar_bold_font(c->panel), text, x + 10, cy, 40, row_h,
						header_fg, PD_LEFT);
				}
				cy += row_h;
			}
		}
		if (cy + row_h > y && cy < y + h) {
			draw_app_row(c, e, x, cy, w, row_h, icon_size, bar_font(c->panel), fg, hl_bg,
				hl_fg, radius, NULL, 0);
		} else {
			add_hit(c, e);
		}
		cy += row_h;
	}
	cairo_restore(c->cr);
	int content = (int)(cy + sm->scroll - y);
	sm->max_scroll = content > h ? content - (int)h : 0;
	if (sm->scroll > sm->max_scroll) {
		sm->scroll = sm->max_scroll;
	}
	return content;
}

static void draw_search_box(struct sm_ctx *c, double x, double y, double w, double h,
		uint32_t bg, uint32_t fg, uint32_t border, double radius, const char *placeholder) {
	struct startmenu *sm = c->sm;
	pd_rounded(c->cr, x + 0.5, y + 0.5, w - 1, h - 1, radius);
	pd_color(c->cr, bg);
	cairo_fill_preserve(c->cr);
	pd_color(c->cr, border);
	cairo_set_line_width(c->cr, 1);
	cairo_stroke(c->cr);
	pd_glyph_search(c->cr, x + 10, y + (h - 16) / 2.0, 16, fg);
	bool empty = !sm->search[0];
	if (empty) {
		pd_text(c->cr, bar_font(c->panel), placeholder, x + 34, y, w - 44, h,
			(fg & 0xffffff00) | 0x99, PD_LEFT);
	}
	struct text_style ts = { .font = bar_font(c->panel), .fg = fg,
		.caret = sm->show_search || !empty };
	text_style_colors(c->panel, &ts);
	text_draw(c->cr, &ts, sm->search, &sm->tc, x + 34, y, w - 44, h);
	psurface_add_hotspot(c->p->surface, x, y, w, h, NULL, HS_SEARCH, 0, NULL);
}

/* ---------- two column (XP / 7) ---------- */

static void render_twocolumn(struct popup *p, cairo_t *cr) {
	struct startmenu *sm = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	struct sm_ctx c = { p, sm, panel, cr, p->surface->scale };
	int M = popup_shadow_margin(panel);
	double W = p->surface->width - 2 * M, H = p->surface->height - 2 * M;
	bool xp = style != PS_AERO;
	double r = tw_theme_double(t, "startmenu.radius", xp ? 7 : 6);
	cairo_translate(cr, M, M);

	uint32_t left_fg = tw_theme_color(t, "startmenu.left_fg", 0x000000ff);
	uint32_t right_fg = tw_theme_color(t, "startmenu.right_fg", xp ? 0x0a246aff : 0xffffffff);
	uint32_t hl_bg = tw_theme_color(t, "startmenu.hl_bg", xp ? 0x316ac5ff : 0xd9e8fbff);
	uint32_t hl_fg = tw_theme_color(t, "startmenu.hl_fg", xp ? 0xffffffff : 0x000000ff);
	const char *bold = bar_bold_font(panel);

	double lx, ly, lw, lh, rx, ry, rw;
	if (xp) {
		// shadow
		cairo_translate(cr, -M, -M);
		for (int i = M; i >= 1; i--) {
			pd_rounded4(cr, M - i + 3, M - i + 3, W + 2 * i - 3, H + 2 * i - 3, r + i, r + i, 0, 0);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.12 * (1.0 - (double)(i - 1) / M));
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
		cairo_translate(cr, M, M);
		pd_rounded4(cr, 0, 0, W, H, r, r, 0, 0);
		pd_color(cr, tw_theme_color(t, "startmenu.border", 0x1c52b8ff));
		cairo_fill(cr);
		// header
		double hh = 64;
		pd_rounded4(cr, 1, 1, W - 2, hh, r, r, 0, 0);
		pd_fill(cr, t, "startmenu.header", 1, hh, 0x2468d4ff);
		draw_avatar(cr, 10, 8, 48, 0xffffffff, 0x5a8ee0ff, false);
		pd_text(cr, "Trebuchet MS, Noto Sans Bold 13", sm->user, 67, 9, W - 80, hh - 16,
			0x00000060, PD_LEFT);
		pd_text(cr, "Trebuchet MS, Noto Sans Bold 13", sm->user, 66, 8, W - 80, hh - 16,
			tw_theme_color(t, "startmenu.header_fg", 0xffffffff), PD_LEFT);
		// footer
		double fh = 44;
		cairo_rectangle(cr, 1, H - fh, W - 2, fh - 1);
		pd_fill(cr, t, "startmenu.footer", H - fh, fh, 0x2163d8ff);
		double bx = W - 12;
		const char *labels[] = { "Turn Off Computer", "Log Off" };
		uint32_t colors[] = { 0xe0532bff, 0xf2b82bff };
		for (int i = 0; i < 2; i++) {
			int tw = 0;
			pd_text_size(cr, bar_font(panel), labels[i], &tw, NULL);
			double bw = tw + 34;
			bx -= bw;
			bool hov = hovered(&c, M + bx, M + H - fh, bw, fh);
			if (hov) {
				pd_rounded(cr, bx, H - fh + 6, bw, fh - 12, 3);
				pd_color(cr, 0xffffff30);
				cairo_fill(cr);
			}
			pd_rounded(cr, bx + 4, H - fh / 2 - 11, 22, 22, 4);
			pd_color(cr, colors[i]);
			cairo_fill(cr);
			pd_glyph_power(cr, bx + 7, H - fh / 2 - 8, 16, 0xffffffff);
			pd_text(cr, bar_font(panel), labels[i], bx + 30, H - fh, tw + 4, fh,
				tw_theme_color(t, "startmenu.footer_fg", 0xffffffff), PD_LEFT);
			psurface_add_hotspot(p->surface, M + bx, M + H - fh, bw, fh, NULL, HS_POWER, i, NULL);
			bx -= 6;
		}
		lx = 1;
		ly = hh + 1;
		lw = (W - 2) * 0.52;
		lh = H - hh - fh - 1;
		pd_rect(cr, lx, ly, lw, lh, tw_theme_color(t, "startmenu.left_bg", 0xffffffff));
		rx = lx + lw;
		ry = ly;
		rw = W - 1 - rx;
		pd_rect(cr, rx, ry, rw, lh, tw_theme_color(t, "startmenu.right_bg", 0xd3e5faff));
		pd_rect(cr, rx, ry, 1, lh, 0x95bdeeff);
		pd_rect(cr, lx, ly, W - 2, 2, 0xf5a14dff);
	} else {
		cairo_translate(cr, -M, -M);
		popup_draw_frame(panel, cr, W + 2 * M, H + 2 * M, M, "startmenu");
		cairo_translate(cr, M, M);
		pd_rounded(cr, 0, 0, W, H, r);
		pd_color(cr, tw_theme_color(t, "startmenu.glass", 0x1f3c5ce0));
		cairo_fill(cr);
		pd_rounded(cr, 0, 0, W, H * 0.4, r);
		cairo_pattern_t *g = pd_gradient("0:#ffffff30 1:#ffffff00", 0, 0, 0, H * 0.4);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		lx = 8;
		ly = 8;
		lw = W * 0.6;
		lh = H - 16 - 40;
		pd_rounded(cr, lx, ly, lw, lh, 4);
		pd_color(cr, tw_theme_color(t, "startmenu.left_bg", 0xffffffff));
		cairo_fill(cr);
		draw_search_box(&c, lx, H - 38, lw, 28, tw_theme_color(t, "startmenu.search_bg", 0xffffffff),
			tw_theme_color(t, "startmenu.search_fg", 0x6d6d6dff), 0x00000060, 3,
			"Search programs and files");
		rx = lx + lw + 10;
		ry = 70;
		rw = W - rx - 8;
		draw_avatar(cr, rx + (rw - 52) / 2, 10, 52, 0xffffffff, 0x3a6ea5ff, false);
		// shut down button
		double sbw = 100, sbh = 26, sbx = rx + rw - sbw, sby = H - 40;
		bool hov = hovered(&c, M + sbx, M + sby, sbw, sbh);
		pd_rounded(cr, sbx + 0.5, sby + 0.5, sbw - 1, sbh - 1, 3);
		cairo_pattern_t *sg = pd_gradient(hov ? "0:#fbb9a8 0.5:#e8401a 1:#f39a78" :
			"0:#ffffff60 0.5:#ffffff20 1:#ffffff40", 0, sby, 0, sby + sbh);
		cairo_set_source(cr, sg);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(sg);
		pd_color(cr, 0x00000090);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_text(cr, bar_font(panel), "Shut down", sbx, sby, sbw, sbh, 0xffffffff, PD_CENTER);
		psurface_add_hotspot(p->surface, M + sbx, M + sby, sbw, sbh, NULL, HS_POWER, 0, NULL);
	}

	// left column content (hotspots are in surface coordinates)
	cairo_save(cr);
	cairo_translate(cr, -M, -M);
	c.sm->hits->length = 0;
	double col_x = M + lx + 4, col_y = M + ly + 6, col_w = lw - 8, col_h = lh - 12;
	if (sm->search[0] || (xp && sm->show_search)) {
		list_t *results = search_results(sm);
		double top = col_y;
		if (xp) {
			// the XP menu has no search box of its own: show what is typed
			draw_search_box(&c, col_x + 2, top, col_w - 4, 26,
				tw_theme_color(t, "startmenu.search_bg",
					tw_theme_color(t, "menu.field_bg", 0xffffffff)),
				tw_theme_color(t, "startmenu.search_fg",
					tw_theme_color(t, "menu.field_fg", 0x000000ff)),
				tw_theme_color(t, "startmenu.search_border", 0x7f9db9ff), 0,
				"Type to search programs");
			top += 32;
		}
		pd_text(cr, bold, "Programs", col_x + 6, top, col_w, 22, xp ? 0x6d6d6dff : 0x1e395bff, PD_LEFT);
		draw_app_list(&c, results, false, col_x, top + 24, col_w, col_h - (top - col_y) - 24, 30, 24,
			left_fg, 0, hl_bg, hl_fg, xp ? 0 : 3);
		list_free(results);
	} else if (sm->all_apps) {
		draw_app_list(&c, sm->apps, false, col_x, col_y, col_w, col_h - 34, 24, 16,
			left_fg, 0, hl_bg, hl_fg, xp ? 0 : 3);
		pd_rect(cr, col_x + 10, col_y + col_h - 32, col_w - 20, 1, 0xd0d0d0ff);
		draw_simple_row(&c, HS_BACK, 0, NULL, "Back", col_x, col_y + col_h - 30, col_w, 30,
			0, bold, left_fg, hl_bg, hl_fg, xp ? 0 : 3, false);
	} else {
		double cy = col_y;
		int max_rows = (int)((col_h - 40) / 42);
		for (int i = 0; i < sm->pinned->length && i < max_rows; i++) {
			struct tw_desktop_entry *e = sm->pinned->items[i];
			draw_app_row(&c, e, col_x, cy, col_w, 42, 32, i < 2 && xp ? bold : bar_font(panel),
				left_fg, hl_bg, hl_fg, xp ? 0 : 3, i < 2 && xp ? e->generic_name : NULL,
				0x6d6d6dff);
			cy += 42;
		}
		pd_rect(cr, col_x + 10, col_y + col_h - 32, col_w - 20, 1, 0xd0d0d0ff);
		draw_simple_row(&c, HS_ALLAPPS, 0, NULL, "All Programs", col_x, col_y + col_h - 30,
			col_w, 30, 0, bold, left_fg, hl_bg, hl_fg, xp ? 0 : 3, true);
	}

	// right column
	double rcx = M + rx + 6, rcy = M + ry + 8, rcw = rw - 12;
	for (int i = 0; i < sm->places->length; i++) {
		struct place *pl = sm->places->items[i];
		draw_simple_row(&c, HS_PLACE, i, pl->icon, pl->label, rcx, rcy, rcw, 32, xp ? 24 : 0,
			xp ? bold : bar_font(panel), right_fg, xp ? 0x316ac5ff : 0xffffff30,
			xp ? 0xffffffff : right_fg, xp ? 0 : 3, false);
		rcy += 32;
	}
	pd_rect(cr, rcx + 6, rcy + 3, rcw - 12, 1, xp ? 0x95bdeeff : 0xffffff40);
	rcy += 8;
	draw_simple_row(&c, HS_THEMES, 0, "preferences-desktop-theme", "Change theme", rcx, rcy,
		rcw, 32, xp ? 24 : 0, bar_font(panel), right_fg, xp ? 0x316ac5ff : 0xffffff30,
		xp ? 0xffffffff : right_fg, xp ? 0 : 3, true);
	rcy += 32;
	draw_simple_row(&c, HS_RUN, 0, "system-run", "Run...", rcx, rcy, rcw, 32, xp ? 24 : 0,
		bar_font(panel), right_fg, xp ? 0x316ac5ff : 0xffffff30, xp ? 0xffffffff : right_fg,
		xp ? 0 : 3, false);
	cairo_restore(cr);
}

/* ---------- list (Windows 10) ---------- */

static void render_list(struct popup *p, cairo_t *cr) {
	struct startmenu *sm = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	struct sm_ctx c = { p, sm, panel, cr, p->surface->scale };
	double W = p->surface->width, H = p->surface->height;
	uint32_t fg = tw_theme_color(t, "startmenu.fg", 0xffffffff);
	uint32_t hl_bg = tw_theme_color(t, "startmenu.hl_bg", 0xffffff1f);
	uint32_t hl_fg = tw_theme_color(t, "startmenu.hl_fg", fg);

	pd_rect(cr, 0, 0, W, H, tw_theme_color(t, "startmenu.bg", 0x1f1f1ff2));
	double strip = 48;
	pd_rect(cr, 0, 0, strip, H, tw_theme_color(t, "startmenu.strip_bg", 0x1a1a1af2));
	// strip icons from the bottom: power, settings, pictures, documents, user
	double sy = H - strip;
	struct {
		int kind;
		int64_t id;
	} buttons[5] = { { HS_POWER, 0 }, { HS_PLACE, sm->places->length - 1 },
		{ HS_PLACE, 2 }, { HS_PLACE, 1 }, { HS_PLACE, 0 } };
	for (int i = 0; i < 5; i++) {
		if (buttons[i].kind == HS_PLACE && (buttons[i].id < 0 ||
				buttons[i].id >= sm->places->length)) {
			continue;
		}
		if (hovered(&c, 0, sy, strip, strip)) {
			pd_rect(cr, 0, sy, strip, strip, hl_bg);
		}
		if (buttons[i].kind == HS_POWER) {
			pd_glyph_power(cr, 15, sy + 15, 18, fg);
		} else if (i == 4) {
			draw_avatar(cr, 12, sy + 12, 24, 0x5a5a5aff, 0xd0d0d0ff, true);
		} else {
			struct place *pl = sm->places->items[buttons[i].id];
			cairo_surface_t *icon = apps_icon(panel, pl->icon, 20 * c.scale);
			if (icon) {
				pd_icon(cr, icon, 14, sy + 14, 20);
			} else {
				pd_text(cr, bar_font(panel), pl->label, 2, sy, strip - 4, strip, fg, PD_CENTER);
			}
		}
		psurface_add_hotspot(p->surface, 0, sy, strip, strip, NULL, buttons[i].kind,
			buttons[i].id, NULL);
		sy -= strip;
	}
	// start glyph in the top of the strip
	pd_glyph_windows(cr, 16, 16, 16, fg, fg, fg, fg, false);

	sm->hits->length = 0;
	double mx = strip + 8, my = 8, mw = W - strip - 16, mh = H - 16;
	if (sm->show_search || sm->search[0]) {
		draw_search_box(&c, mx, my, mw, 36, tw_theme_color(t, "startmenu.search_bg", 0xffffffff),
			tw_theme_color(t, "startmenu.search_fg", 0x000000ff), 0x0078d7ff, 0,
			"Type here to search");
		my += 44;
		mh -= 44;
	}
	if (sm->search[0]) {
		list_t *results = search_results(sm);
		pd_text(cr, bar_bold_font(panel), "Best match", mx + 6, my, mw, 28, fg, PD_LEFT);
		draw_app_list(&c, results, false, mx, my + 30, mw, mh - 30, 40, 32, fg, fg, hl_bg, hl_fg, 0);
		list_free(results);
		return;
	}
	pd_text(cr, bar_bold_font(panel), "Pinned", mx + 6, my, mw, 28, fg, PD_LEFT);
	double py = my + 30;
	int pinned_rows = sm->pinned->length < 4 ? sm->pinned->length : 4;
	for (int i = 0; i < pinned_rows; i++) {
		draw_app_row(&c, sm->pinned->items[i], mx, py, mw, 36, 24, bar_font(panel), fg,
			hl_bg, hl_fg, 0, NULL, 0);
		py += 36;
	}
	pd_rect(cr, mx + 6, py + 4, mw - 12, 1, 0xffffff20);
	py += 10;
	draw_app_list(&c, sm->apps, true, mx, py, mw, H - 8 - py, 36, 24, fg, fg, hl_bg, hl_fg, 0);
}

/* ---------- centered (Windows 11) ---------- */

static void render_centered(struct popup *p, cairo_t *cr) {
	struct startmenu *sm = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	struct sm_ctx c = { p, sm, panel, cr, p->surface->scale };
	int M = popup_shadow_margin(panel);
	double W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "startmenu");
	uint32_t fg = tw_theme_color(t, "startmenu.fg", 0x000000ff);
	uint32_t hl_bg = tw_theme_color(t, "startmenu.hl_bg", 0x0000000f);
	uint32_t hl_fg = tw_theme_color(t, "startmenu.hl_fg", fg);
	double x0 = M + 32, w0 = W - 2 * M - 64;
	const char *bold = bar_bold_font(panel);

	draw_search_box(&c, x0, M + 24, w0, 36, tw_theme_color(t, "startmenu.search_bg", 0xffffffff),
		tw_theme_color(t, "startmenu.search_fg", 0x5f5f5fff), 0x00000020, 18,
		"Type here to search");
	pd_rect(cr, x0 + 18, M + 24 + 35, w0 - 36, 1, sm->show_search ? 0x005fb8ff : 0x00000000);

	double footer_h = 64;
	double content_y = M + 80, content_h = H - M - footer_h - content_y - 8;
	sm->hits->length = 0;

	if (sm->search[0]) {
		list_t *results = search_results(sm);
		pd_text(cr, bold, "Best match", x0 + 8, content_y, w0, 28, fg, PD_LEFT);
		draw_app_list(&c, results, false, x0, content_y + 32, w0, content_h - 32, 44, 32,
			fg, fg, hl_bg, hl_fg, 4);
		list_free(results);
	} else if (sm->all_apps) {
		pd_text(cr, bold, "All apps", x0 + 16, content_y, w0, 32, fg, PD_LEFT);
		double bw = 84, bx = x0 + w0 - bw - 16;
		bool hov = hovered(&c, bx, content_y + 2, bw, 28);
		pd_rounded(cr, bx + 0.5, content_y + 2.5, bw - 1, 27, 4);
		pd_color(cr, hov ? 0xf6f6f6ff : 0xfbfbfbff);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000020);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_glyph_arrow(cr, bx + 8, content_y + 10, 12, 2, fg);
		pd_text(cr, bar_font(panel), "Back", bx + 24, content_y + 2, bw - 30, 28, fg, PD_LEFT);
		psurface_add_hotspot(p->surface, bx, content_y + 2, bw, 28, NULL, HS_BACK, 0, NULL);
		draw_app_list(&c, sm->apps, true, x0 + 8, content_y + 40, w0 - 16, content_h - 40, 40,
			24, fg, fg, hl_bg, hl_fg, 4);
	} else {
		pd_text(cr, bold, "Pinned", x0 + 16, content_y, w0, 32, fg, PD_LEFT);
		double bw = 96, bx = x0 + w0 - bw - 16;
		bool hov = hovered(&c, bx, content_y + 2, bw, 28);
		pd_rounded(cr, bx + 0.5, content_y + 2.5, bw - 1, 27, 4);
		pd_color(cr, hov ? 0xf6f6f6ff : 0xfbfbfbff);
		cairo_fill_preserve(cr);
		pd_color(cr, 0x00000020);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_text(cr, bar_font(panel), "All apps", bx + 10, content_y + 2, bw - 30, 28, fg, PD_LEFT);
		pd_glyph_arrow(cr, bx + bw - 20, content_y + 10, 12, 0, fg);
		psurface_add_hotspot(p->surface, bx, content_y + 2, bw, 28, NULL, HS_ALLAPPS, 0, NULL);

		int cols = 6;
		double cw = w0 / cols, ch = 92;
		int rows = (int)((content_h - 44) / ch);
		for (int i = 0; i < sm->pinned->length && i < rows * cols; i++) {
			struct tw_desktop_entry *e = sm->pinned->items[i];
			double cx = x0 + (i % cols) * cw, cy = content_y + 44 + (i / cols) * ch;
			int hit_index = sm->hits->length;
			add_hit(&c, e);
			bool sel = hovered(&c, cx + 4, cy, cw - 8, ch - 8) || hit_index == sm->selected;
			if (sel) {
				pd_rounded(cr, cx + 4, cy, cw - 8, ch - 8, 4);
				pd_color(cr, hl_bg);
				cairo_fill(cr);
			}
			cairo_surface_t *icon = e->icon ? apps_icon(panel, e->icon, 32 * c.scale) : NULL;
			pd_icon(cr, icon, cx + (cw - 32) / 2, cy + 14, 32);
			pd_text(cr, bar_font(panel), e->name, cx + 6, cy + 52, cw - 12, 24, fg, PD_CENTER);
			psurface_add_hotspot(p->surface, cx + 4, cy, cw - 8, ch - 8, NULL, HS_APP,
				list_find(sm->apps, e), NULL);
		}
	}

	// footer
	double fy = H - M - footer_h;
	pd_rounded4(cr, M, fy, W - 2 * M, footer_h, 0, 0, popup_radius(panel, "startmenu"),
		popup_radius(panel, "startmenu"));
	pd_color(cr, tw_theme_color(t, "startmenu.footer_bg", 0xe7e7e7f5));
	cairo_fill(cr);
	pd_rect(cr, M, fy, W - 2 * M, 1, 0x00000014);
	draw_avatar(cr, x0 + 16, fy + 16, 32, 0xd0d0d0ff, 0x808080ff, true);
	pd_text(cr, bar_font(panel), sm->user, x0 + 58, fy, 300, footer_h, fg, PD_LEFT);
	double pbx = x0 + w0 - 40, pby = fy + 12;
	if (hovered(&c, pbx, pby, 40, 40)) {
		pd_rounded(cr, pbx, pby, 40, 40, 4);
		pd_color(cr, hl_bg);
		cairo_fill(cr);
	}
	pd_glyph_power(cr, pbx + 11, pby + 11, 18, fg);
	psurface_add_hotspot(p->surface, pbx, pby, 40, 40, NULL, HS_POWER, 0, NULL);
}

/* ---------- start screen (8) ---------- */

#define TILE 120
#define TILE_GAP 8
#define APPS_COLUMN 250
#define APPS_ROW 40

static uint32_t tile_color(struct panel *panel, const struct tw_desktop_entry *e) {
	const char *list = tw_theme_str(panel->theme, "startmenu.tile_colors",
		"#2672ec #00a300 #dc572e #8c0095 #00aba9 #ac193d #2e8def #d24726");
	char *copy = strdup(list);
	list_t *colors = create_list();
	char *save = NULL;
	for (char *tok = strtok_r(copy, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
		list_add(colors, tok);
	}
	unsigned long hash = 5381;
	for (const char *s = e->id; s && *s; s++) {
		hash = hash * 33 + (unsigned char)*s;
	}
	uint32_t color = 0x2672ecff;
	if (colors->length > 0) {
		tw_parse_color(colors->items[hash % colors->length], &color);
	}
	list_free(colors);
	free(copy);
	return color;
}

/* Apps in columns that continue to the right; returns the width of all columns. */
static double draw_app_columns(struct sm_ctx *c, list_t *entries, bool headers, double x0,
		double y0, double h, double W, uint32_t fg, uint32_t hl_bg) {
	struct startmenu *sm = c->sm;
	int rows = (int)(h / APPS_ROW);
	rows = rows < 1 ? 1 : rows;
	int slot = 0;
	char last = 0;
	for (int i = 0; i < entries->length; i++) {
		struct tw_desktop_entry *e = entries->items[i];
		if (headers && header_letter(e) != last) {
			last = header_letter(e);
			double hx = x0 + (slot / rows) * APPS_COLUMN - sm->scroll;
			double hy = y0 + (slot % rows) * APPS_ROW;
			char text[2] = { last, 0 };
			pd_text(c->cr, bar_bold_font(c->panel), text, hx + 6, hy, 60, APPS_ROW, fg, PD_LEFT);
			slot++;
		}
		double x = x0 + (slot / rows) * APPS_COLUMN - sm->scroll;
		double y = y0 + (slot % rows) * APPS_ROW;
		if (x + APPS_COLUMN > 0 && x < W) {
			draw_app_row(c, e, x, y, APPS_COLUMN - 16, APPS_ROW, 24, bar_font(c->panel), fg,
				hl_bg, fg, 0, NULL, 0);
		} else {
			add_hit(c, e);
		}
		slot++;
	}
	return ((slot + rows - 1) / rows) * APPS_COLUMN;
}

static void draw_round_button(struct sm_ctx *c, int kind, double x, double y, double size,
		int arrow_direction, uint32_t fg) {
	cairo_t *cr = c->cr;
	cairo_new_path(cr);
	if (hovered(c, x, y, size, size)) {
		cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
		pd_color(cr, 0xffffff30);
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	cairo_arc(cr, x + size / 2, y + size / 2, size / 2 - 1, 0, 2 * M_PI);
	pd_color(cr, fg);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	pd_glyph_arrow(cr, x + size / 2 - 6, y + size / 2 - 6, 12, arrow_direction, fg);
	psurface_add_hotspot(c->p->surface, x, y, size, size, NULL, kind, 0, NULL);
}

static void render_tiles(struct popup *p, cairo_t *cr) {
	struct startmenu *sm = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	struct sm_ctx c = { p, sm, panel, cr, p->surface->scale };
	double W = p->surface->width, H = p->surface->height;
	uint32_t bg = tw_theme_color(t, "startmenu.bg", 0x180052ff);
	uint32_t fg = tw_theme_color(t, "startmenu.fg", 0xffffffff);
	uint32_t hl_bg = tw_theme_color(t, "startmenu.hl_bg", 0xffffff26);
	pd_rect(cr, 0, 0, W, H, bg);
	sm->hits->length = 0;

	double left = W > 900 ? 116 : 40, top = 40;
	const char *title = sm->search[0] ? "Search" : sm->all_apps ? "Apps" : "Start";
	const char *title_font = tw_theme_str(t, "startmenu.title_font",
		"Segoe UI Light, Noto Sans Light 32");
	pd_text(cr, title_font, title, left, top, 260, 56, fg, PD_LEFT);
	if (sm->show_search || sm->search[0]) {
		draw_search_box(&c, left + 220, top + 12, W - left - 220 - 300 > 360 ? 360 :
			W - left - 520, 34, tw_theme_color(t, "startmenu.search_bg", 0xffffffff),
			tw_theme_color(t, "startmenu.search_fg", 0x000000ff), 0x00000000, 0, "Search");
	}

	// account and power at the top right, like the Windows 8.1 start screen
	double avatar = 40, ax = W - 64 - avatar - 8;
	pd_text(cr, bar_font(panel), sm->user, ax - 208, top + 8, 200, avatar, fg, PD_RIGHT);
	draw_avatar(cr, ax, top + 8, avatar, 0xffffff40, fg, false);
	double px = W - 64, py = top + 8;
	if (hovered(&c, px, py, 40, 40)) {
		pd_rect(cr, px, py, 40, 40, hl_bg);
	}
	pd_glyph_power(cr, px + 10, py + 10, 20, fg);
	psurface_add_hotspot(p->surface, px, py, 40, 40, NULL, HS_POWER, 0, NULL);
	double sx = px - avatar - 216 - 48;
	if (!sm->show_search && !sm->search[0]) {
		if (hovered(&c, sx, py, 40, 40)) {
			pd_rect(cr, sx, py, 40, 40, hl_bg);
		}
		pd_glyph_search(cr, sx + 11, py + 11, 18, fg);
		psurface_add_hotspot(p->surface, sx, py, 40, 40, NULL, HS_SEARCH, 0, NULL);
	}

	double content_y = top + 104, content_h = H - content_y - 96;
	double view_w = W - left - 40;
	double content_w = 0;
	cairo_save(cr);
	cairo_rectangle(cr, 0, content_y - 8, W, content_h + 16);
	cairo_clip(cr);
	if (sm->search[0]) {
		list_t *results = search_results(sm);
		if (results->length == 0) {
			pd_text(cr, bar_font(panel), "No apps match your search", left, content_y, 400,
				APPS_ROW, fg, PD_LEFT);
		}
		content_w = draw_app_columns(&c, results, false, left, content_y, content_h, W, fg,
			hl_bg);
		list_free(results);
	} else if (sm->all_apps) {
		content_w = draw_app_columns(&c, sm->apps, true, left, content_y, content_h, W, fg,
			hl_bg);
	} else {
		int rows = (int)((content_h + TILE_GAP) / (TILE + TILE_GAP));
		rows = rows < 1 ? 1 : rows > 4 ? 4 : rows;
		int cell = TILE + TILE_GAP;
		for (int i = 0; i < sm->pinned->length; i++) {
			struct tw_desktop_entry *e = sm->pinned->items[i];
			double x = left + (i / rows) * cell - sm->scroll;
			double y = content_y + (i % rows) * cell;
			int hit_index = sm->hits->length;
			add_hit(&c, e);
			if (x + TILE < 0 || x > W) {
				continue;
			}
			pd_rect(cr, x, y, TILE, TILE, tile_color(panel, e));
			if (hovered(&c, x, y, TILE, TILE) || hit_index == sm->selected) {
				cairo_rectangle(cr, x + 1.5, y + 1.5, TILE - 3, TILE - 3);
				pd_color(cr, 0xffffffb0);
				cairo_set_line_width(cr, 3);
				cairo_stroke(cr);
			}
			cairo_surface_t *icon = e->icon ? apps_icon(panel, e->icon, 48 * c.scale) : NULL;
			pd_icon(cr, icon, x + (TILE - 48) / 2.0, y + 26, 48);
			pd_text(cr, bar_font(panel), e->name, x + 8, y + TILE - 28, TILE - 14, 22,
				0xffffffff, PD_LEFT);
			psurface_add_hotspot(p->surface, x, y, TILE, TILE, NULL, HS_APP,
				list_find(sm->apps, e), NULL);
		}
		content_w = ((sm->pinned->length + rows - 1) / rows) * cell;
		if (sm->pinned->length == 0) {
			pd_text(cr, bar_font(panel), "Pin apps in the Settings app (Menus) to see them "
				"here as tiles", left, content_y, W - left, 30, fg, PD_LEFT);
		}
	}
	cairo_restore(cr);
	sm->max_scroll = content_w > view_w ? (int)(content_w - view_w) : 0;
	if (sm->scroll > sm->max_scroll) {
		sm->scroll = sm->max_scroll;
	}

	// down to all apps, up back to the tiles
	if (!sm->search[0]) {
		draw_round_button(&c, sm->all_apps ? HS_BACK : HS_ALLAPPS, left, H - 72, 36,
			sm->all_apps ? 3 : 1, fg);
	}
}

/* ---------- input ---------- */

static void sm_render(struct popup *p, cairo_t *cr) {
	struct startmenu *sm = p->data;
	switch (sm->layout) {
	case SM_TWOCOLUMN:
		render_twocolumn(p, cr);
		break;
	case SM_LIST:
		render_list(p, cr);
		break;
	case SM_TILES:
		render_tiles(p, cr);
		break;
	default:
		render_centered(p, cr);
		break;
	}
	if (sm->selected >= sm->hits->length) {
		sm->selected = sm->hits->length - 1;
	}
	sm->scroll_to_selected = false;
}

static void sm_motion(struct popup *p, double x, double y) {
	struct startmenu *sm = p->data;
	struct hotspot *before = sm->inside ? psurface_hotspot_at(p->surface, sm->px, sm->py) : NULL;
	sm->px = x;
	sm->py = y;
	sm->inside = true;
	struct hotspot *after = psurface_hotspot_at(p->surface, x, y);
	if (before != after && (!before || !after || before->kind != after->kind ||
			before->id != after->id || before->box.y != after->box.y)) {
		sm->selected = -1;
		popup_set_dirty(p);
	}
}

static void sm_leave(struct popup *p) {
	struct startmenu *sm = p->data;
	sm->inside = false;
	popup_set_dirty(p);
}

static void open_run_later(void *data) {
	struct panel *panel = data;
	struct panel_output *output = panel_focused_output(panel);
	if (output) {
		rundialog_open(panel, output);
	}
}

static void launch(struct popup *p, struct tw_desktop_entry *e) {
	struct panel *panel = p->panel;
	apps_launch(panel, e);
	popup_close_later(panel);
}

static void sm_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	struct startmenu *sm = p->data;
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (!hs) {
		return;
	}
	switch (hs->kind) {
	case HS_APP:
		if (hs->id >= 0 && hs->id < sm->apps->length) {
			launch(p, sm->apps->items[hs->id]);
		}
		break;
	case HS_PLACE:
		if (hs->id >= 0 && hs->id < sm->places->length) {
			struct place *pl = sm->places->items[hs->id];
			bar_run_command(p->panel, pl->command, NULL);
			popup_close_later(p->panel);
		}
		break;
	case HS_POWER:
		if (power_dialog(p->panel)) {
			popup_close_later(p->panel);
			// the XP menu has Turn Off Computer (0) and Log Off (1)
			loop_add_timer(p->panel->loop, 5, hs->id == 1 ? open_logoff_later :
				open_shutdown_later, p->panel);
			break;
		}
		menu_open_at(p->panel, p, p->output, power_menu_items(p->panel), true,
			p->x + (int)x, p->y + (int)y, true, NULL);
		break;
	case HS_THEMES:
		menu_open_at(p->panel, p, p->output, theme_menu_items(p->panel), true,
			p->x + (int)x, p->y + (int)y, true, NULL);
		break;
	case HS_RUN:
		popup_close_later(p->panel);
		loop_add_timer(p->panel->loop, 5, open_run_later, p->panel);
		break;
	case HS_ALLAPPS:
		sm->all_apps = true;
		sm->scroll = 0;
		popup_set_dirty(p);
		break;
	case HS_BACK:
		sm->all_apps = false;
		sm->scroll = 0;
		popup_set_dirty(p);
		break;
	case HS_SEARCH:
		sm->show_search = true;
		popup_set_dirty(p);
		break;
	}
}

static void sm_axis(struct popup *p, double x, double y, int direction) {
	struct startmenu *sm = p->data;
	sm->scroll += direction * 48;
	if (sm->scroll < 0) {
		sm->scroll = 0;
	}
	if (sm->scroll > sm->max_scroll) {
		sm->scroll = sm->max_scroll;
	}
	popup_set_dirty(p);
}

static void sm_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct startmenu *sm = p->data;
	switch (sym) {
	case XKB_KEY_Escape:
		if (sm->search[0]) {
			sm->search[0] = '\0';
			sm->scroll = 0;
		} else if (sm->all_apps) {
			sm->all_apps = false;
		} else {
			popup_close_later(p->panel);
			return;
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (sm->hits->length > 0) {
			int index = sm->selected >= 0 ? sm->selected : 0;
			launch(p, sm->hits->items[index]);
		}
		return;
	case XKB_KEY_Down:
		if (sm->selected < sm->hits->length - 1) {
			sm->selected++;
			sm->scroll_to_selected = true;
		}
		break;
	case XKB_KEY_Up:
		if (sm->selected > 0) {
			sm->selected--;
			sm->scroll_to_selected = true;
		}
		break;
	default:
		switch (text_key(sm->search, sizeof(sm->search), &sm->tc, sym, utf8, mods)) {
		case TEXT_KEY_IGNORED:
			return;
		case TEXT_KEY_CHANGED:
			sm->selected = 0;
			sm->scroll = 0;
			sm->show_search = true;
			break;
		case TEXT_KEY_MOVED:
			break;
		}
		break;
	}
	popup_set_dirty(p);
}

static void sm_destroy(struct popup *p) {
	struct startmenu *sm = p->data;
	list_free(sm->pinned);
	list_free(sm->hits);
	for (int i = 0; i < sm->places->length; i++) {
		struct place *pl = sm->places->items[i];
		free(pl->label);
		free(pl->icon);
		free(pl->command);
		free(pl);
	}
	list_free(sm->places);
	tw_desktop_list_free(sm->apps);
	free(sm);
}

static const struct popup_vtable startmenu_vtable = {
	.render = sm_render,
	.motion = sm_motion,
	.leave = sm_leave,
	.button = sm_button,
	.axis = sm_axis,
	.key = sm_key,
	.destroy = sm_destroy,
};

void startmenu_toggle(struct panel *panel, struct panel_output *output, bool search) {
	if (popup_is_open(panel, POPUP_STARTMENU)) {
		if (search) {
			struct startmenu *sm = panel->popup->data;
			if (panel->popup->vtable == &startmenu_vtable) {
				sm->show_search = true;
				popup_set_dirty(panel->popup);
				return;
			}
		}
		popup_close_all(panel);
		return;
	}
	enum pstyle style = panel_style(panel);
	const char *layout_name = tw_theme_str(panel->theme, "startmenu.layout",
		style == PS_CLASSIC ? "classic" : style == PS_LUNA || style == PS_AERO ? "twocolumn" :
		style == PS_FLAT ? "list" : "centered");
	// taskbar.conf wins over the theme, unless it says "layout theme"
	const char *chosen = panel->config ?
		twconf_value(panel->config->startmenu, "layout") : NULL;
	if (chosen && strcmp(chosen, "theme") != 0) {
		layout_name = chosen;
	}
	enum sm_layout layout = strcmp(layout_name, "classic") == 0 ? SM_CLASSIC :
		strcmp(layout_name, "twocolumn") == 0 ? SM_TWOCOLUMN :
		strcmp(layout_name, "list") == 0 ? SM_LIST :
		strcmp(layout_name, "tiles") == 0 ? SM_TILES : SM_CENTERED;
	if (layout == SM_CLASSIC) {
		open_classic(panel, output);
		return;
	}

	struct startmenu *sm = calloc(1, sizeof(*sm));
	sm->layout = layout;
	sm->apps = load_apps();
	sm->hits = create_list();
	sm->selected = -1;
	sm->show_search = search;
	load_config(panel, sm);
	if (layout == SM_TILES) {
		popup_create(panel, POPUP_STARTMENU, NULL, output, 0, 0, output->width, output->height,
			&startmenu_vtable, sm);
		return;
	}

	int M = layout == SM_LIST ? 0 : popup_shadow_margin(panel);
	int def_w = layout == SM_TWOCOLUMN ? (style == PS_AERO ? 400 : 380) :
		layout == SM_LIST ? 360 : 640;
	int def_h = layout == SM_TWOCOLUMN ? (style == PS_AERO ? 520 : 470) :
		layout == SM_LIST ? 560 : 700;
	int width = tw_theme_int(panel->theme, "startmenu.width", def_w) + 2 * M;
	int height = tw_theme_int(panel->theme, "startmenu.height", def_h) + 2 * M;
	int bar = output->bar ? output->bar->height : 0;
	bool bottom = panel->config->layouts[panel->layout].bottom;
	if (height > output->height - bar - 8) {
		height = output->height - bar - 8;
	}
	int x = layout == SM_CENTERED ? (output->width - width) / 2 : 0;
	int y = bottom ? output->height - bar - height + (layout == SM_CENTERED ? -4 : M) :
		bar - M;
	popup_create(panel, POPUP_STARTMENU, NULL, output, x - (layout == SM_CENTERED ? 0 : M) +
		(layout == SM_CENTERED ? 0 : M ? 0 : 0), y, width, height, &startmenu_vtable, sm);
}
