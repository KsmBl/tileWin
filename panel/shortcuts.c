#include <json.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "ipc.h"
#include "popup.h"
#include "shortcuts_text.h"
#include "tw_paths.h"

/*
 * The keyboard shortcuts of the current mode (Super+/ or Super+F1), read from
 * the compositor when the list opens, so what it says is what the keys do now,
 * also for keys changed on the Keyboard page or in the config.
 */

#define COLUMN_W 390
#define ROW_H 22
#define GROUP_H 34
#define TITLE_H 52
#define PAD 20
#define KEYS_W 170

struct shortcut_row {
	char label[160];
	char first[64], last[64];
	int keys;
	enum shortcut_group group;
};

struct shortcuts {
	struct shortcut_row *rows;
	int count;
	int scroll, content;
	int columns;
};

static struct shortcut_row *row_for(struct shortcuts *s, const char *label,
		enum shortcut_group group) {
	for (int i = 0; i < s->count; i++) {
		if (strcmp(s->rows[i].label, label) == 0) {
			return &s->rows[i];
		}
	}
	s->rows = realloc(s->rows, (s->count + 1) * sizeof(*s->rows));
	struct shortcut_row *row = &s->rows[s->count++];
	memset(row, 0, sizeof(*row));
	snprintf(row->label, sizeof(row->label), "%s", label);
	row->group = group;
	return row;
}

static void read_bindings(struct panel *panel, struct shortcuts *s) {
	json_object *all = ipc_request(panel, IPC_GET_BINDINGS, NULL);
	size_t count = all && json_object_is_type(all, json_type_array) ?
		json_object_array_length(all) : 0;
	for (size_t i = 0; i < count; i++) {
		json_object *item = json_object_array_get_idx(all, i), *keys, *command;
		if (!json_object_object_get_ex(item, "keys", &keys) ||
				!json_object_object_get_ex(item, "command", &command)) {
			continue;
		}
		char label[160], text[64];
		enum shortcut_group group;
		const char *cmd = json_object_get_string(command);
		if (!shortcut_describe(cmd, label, sizeof(label), &group)) {
			continue;
		}
		shortcut_keys_text(json_object_get_string(keys), text, sizeof(text));
		struct shortcut_row *row = row_for(s, label, group);
		if (row->keys == 0) {
			snprintf(row->first, sizeof(row->first), "%s", text);
		}
		if (strcmp(row->first, text) != 0) { // left and right Super are one key here
			snprintf(row->last, sizeof(row->last), "%s", text);
			row->keys++;
		} else if (row->keys == 0) {
			row->keys = 1;
		}
	}
	if (all) {
		json_object_put(all);
	}
}

static bool numbered(const char *label) {
	return strcmp(label, "Taskbar button") == 0 || strcmp(label, "Go to a desktop") == 0 ||
		strcmp(label, "Window to a desktop") == 0;
}

static void row_keys(const struct shortcut_row *row, char *out, size_t size) {
	if (row->keys <= 1) {
		snprintf(out, size, "%s", row->first);
	} else {
		snprintf(out, size, "%s %s %s", row->first, numbered(row->label) ? "…" : "or", row->last);
	}
}

static int group_rows(const struct shortcuts *s, int group) {
	int n = 0;
	for (int i = 0; i < s->count; i++) {
		n += (int)s->rows[i].group == group;
	}
	return n;
}

/* Lays the groups into columns; returns the height of the tallest. */
static int lay_out(struct shortcuts *s, cairo_t *cr, const char *font, uint32_t fg,
		uint32_t dim, uint32_t accent, const char *bold, double x0, double y0) {
	int total = 0;
	for (int g = 0; g < SHORTCUT_GROUPS; g++) {
		int n = group_rows(s, g);
		total += n ? GROUP_H + n * ROW_H : 0;
	}
	int target = total / (s->columns > 0 ? s->columns : 1) + GROUP_H;
	int col = 0, y = 0, tallest = 0;
	for (int g = 0; g < SHORTCUT_GROUPS; g++) {
		int n = group_rows(s, g);
		if (n == 0) {
			continue;
		}
		if (y > 0 && y + (GROUP_H + n * ROW_H) / 2 > target && col < s->columns - 1) {
			col++;
			y = 0;
		}
		double x = x0 + col * COLUMN_W;
		if (cr) {
			pd_text(cr, bold, shortcut_group_title(g), x, y0 + y + 8, COLUMN_W - 20, GROUP_H - 8,
				accent, PD_LEFT);
		}
		y += GROUP_H;
		for (int i = 0; i < s->count; i++) {
			if ((int)s->rows[i].group != g) {
				continue;
			}
			if (cr) {
				char keys[140];
				row_keys(&s->rows[i], keys, sizeof(keys));
				pd_text(cr, font, keys, x, y0 + y, KEYS_W, ROW_H, fg, PD_LEFT);
				pd_text(cr, font, s->rows[i].label, x + KEYS_W + 8, y0 + y,
					COLUMN_W - KEYS_W - 28, ROW_H, dim, PD_LEFT);
			}
			y += ROW_H;
		}
		if (y > tallest) {
			tallest = y;
		}
	}
	return tallest;
}

static void shortcuts_render(struct popup *p, cairo_t *cr) {
	struct shortcuts *s = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	int M = popup_shadow_margin(panel);
	double W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "menu");
	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t dim = (fg & 0xffffff00) | 0xb8; // the disabled color is too faint to read a list in
	uint32_t accent = fg;
	const char *font = tw_theme_str(t, "menu.font", bar_font(panel));
	const char *bold = bar_bold_font(panel);
	const char *mode = panel->state.mode && strcmp(panel->state.mode, "tile") == 0 ?
		"Keyboard shortcuts of tile mode" : "Keyboard shortcuts of window mode";
	pd_text(cr, bold, mode, M + PAD, M + 10, W - 2 * M - 2 * PAD, 28, fg, PD_LEFT);
	pd_text(cr, font, "Esc closes this list. The Keyboard page of the settings changes the keys.",
		M + PAD, M + 32, W - 2 * M - 2 * PAD, 18, dim, PD_LEFT);
	cairo_save(cr);
	cairo_rectangle(cr, M, M + TITLE_H, W - 2 * M, H - 2 * M - TITLE_H - 8);
	cairo_clip(cr);
	lay_out(s, cr, font, fg, dim, accent, bold, M + PAD, M + TITLE_H - s->scroll);
	cairo_restore(cr);
}

static void shortcuts_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed && button == BTN_LEFT) {
		popup_close_later(p->panel);
	}
}

static void shortcuts_axis(struct popup *p, double x, double y, int direction) {
	struct shortcuts *s = p->data;
	int view = p->surface->height - 2 * popup_shadow_margin(p->panel) - TITLE_H - 8;
	int most = s->content > view ? s->content - view : 0;
	s->scroll += direction * 3 * ROW_H;
	s->scroll = s->scroll < 0 ? 0 : s->scroll > most ? most : s->scroll;
	popup_set_dirty(p);
}

static void shortcuts_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	switch (sym) {
	case XKB_KEY_Escape:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		popup_close_later(p->panel);
		break;
	case XKB_KEY_Down:
	case XKB_KEY_Page_Down:
		shortcuts_axis(p, 0, 0, sym == XKB_KEY_Down ? 1 : 5);
		break;
	case XKB_KEY_Up:
	case XKB_KEY_Page_Up:
		shortcuts_axis(p, 0, 0, sym == XKB_KEY_Up ? -1 : -5);
		break;
	default:
		break;
	}
}

static void shortcuts_destroy(struct popup *p) {
	struct shortcuts *s = p->data;
	free(s->rows);
	free(s);
}

static const struct popup_vtable shortcuts_vtable = {
	.render = shortcuts_render,
	.button = shortcuts_button,
	.axis = shortcuts_axis,
	.key = shortcuts_key,
	.destroy = shortcuts_destroy,
};

void shortcuts_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_SHORTCUTS)) {
		popup_close_all(panel);
		return;
	}
	if (!output) {
		return;
	}
	struct shortcuts *s = calloc(1, sizeof(*s));
	read_bindings(panel, s);
	int M = popup_shadow_margin(panel);
	int fit = (output->width - 2 * M - 2 * PAD - 40) / COLUMN_W;
	s->columns = fit < 1 ? 1 : fit > 3 ? 3 : fit;
	s->content = lay_out(s, NULL, NULL, 0, 0, 0, NULL, 0, 0);
	int width = s->columns * COLUMN_W + 2 * PAD + 2 * M;
	int height = TITLE_H + s->content + 16 + 2 * M;
	if (height > output->height - 40) {
		height = output->height - 40;
	}
	popup_create(panel, POPUP_SHORTCUTS, NULL, output, (output->width - width) / 2,
		(output->height - height) / 2, width, height, &shortcuts_vtable, s);
}

/* ---------- the word after a switch of the mode ---------- */

#define MODE_HINTS 3

/* The keys of the first binding of a command, as words. */
static bool keys_of(struct panel *panel, const char *command, char *out, size_t size) {
	json_object *all = ipc_request(panel, IPC_GET_BINDINGS, NULL);
	size_t count = all && json_object_is_type(all, json_type_array) ?
		json_object_array_length(all) : 0;
	bool found = false;
	for (size_t i = 0; i < count && !found; i++) {
		json_object *item = json_object_array_get_idx(all, i), *keys, *cmd;
		if (json_object_object_get_ex(item, "keys", &keys) &&
				json_object_object_get_ex(item, "command", &cmd) &&
				strcmp(json_object_get_string(cmd), command) == 0) {
			shortcut_keys_text(json_object_get_string(keys), out, size);
			found = true;
		}
	}
	if (all) {
		json_object_put(all);
	}
	return found;
}

/*
 * A switch of the mode by accident leaves someone with other keys and no way of
 * knowing how to get back, so the first few times it says.
 */
void shortcuts_mode_hint(struct panel *panel, const char *mode) {
	char *dir = tw_state_dir();
	if (!dir) {
		return;
	}
	char *path = malloc(strlen(dir) + 16);
	sprintf(path, "%s/mode-hints", dir);
	free(dir);
	int shown = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%d", &shown) != 1) {
			shown = 0;
		}
		fclose(f);
	}
	if (shown < MODE_HINTS) {
		char number[16];
		snprintf(number, sizeof(number), "%d\n", shown + 1);
		tw_write_string(path, number);
		char keys[64], list[64], body[256];
		if (!keys_of(panel, "wm_mode toggle", keys, sizeof(keys))) {
			snprintf(keys, sizeof(keys), "Super+Shift+W");
		}
		if (!keys_of(panel, "panel shortcuts", list, sizeof(list))) {
			snprintf(list, sizeof(list), "Super+/");
		}
		bool tile = strcmp(mode, "tile") == 0;
		snprintf(body, sizeof(body), "%s goes back to %s mode. %s shows the keys of this mode.",
			keys, tile ? "window" : "tile", list);
		notify_local(panel, "tileWin", "preferences-desktop-keyboard-shortcuts",
			tile ? "This is tile mode" : "This is window mode", body);
	}
	free(path);
}
