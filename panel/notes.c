#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "popup.h"
#include "textfield.h"
#include "tw_paths.h"

/*
 * Sticky Notes, as Windows 7 had them: notes of colored paper on the desktop,
 * over the wallpaper and under the windows, that are typed into right there.
 * The strip along the top moves a note, "+" on it makes another one and "x"
 * throws it away; the corner at the bottom right makes it bigger or smaller
 * and the right button picks its color. Each note is a file of its own in
 * ~/.local/share/tileWin/notes, written a moment after the typing stops:
 *
 *   #note <x> <y> <width> <height> <color> <screen>
 *   the text of the note
 *
 * "panel note new" (and New > Sticky note on the desktop) makes one.
 */

#define NOTE_W 220
#define NOTE_H 200
#define STRIP_H 22
#define CORNER 14
#define TEXT_PAD 8
#define MAX_TEXT 8192
#define SAVE_MS 600

static const struct {
	const char *name, *label;
	uint32_t paper, strip, dark_paper, dark_strip;
} colors[] = {
	{ "yellow", "Yellow", 0xfdf7a6ff, 0xf6e97aff, 0x8f8a3cff, 0x7a7530ff },
	{ "blue", "Blue", 0xcdeaf9ff, 0xaedcf4ff, 0x3e6a80ff, 0x325a6eff },
	{ "green", "Green", 0xcdf5c4ff, 0xb2ecaaff, 0x467a40ff, 0x3a6835ff },
	{ "pink", "Pink", 0xf9d2e8ff, 0xf3b9daff, 0x82506aff, 0x70435aff },
	{ "purple", "Purple", 0xdcd0f9ff, 0xc9b9f3ff, 0x5c4c86ff, 0x4e4074ff },
	{ "white", "White", 0xfafafaff, 0xebebebff, 0x4a4a4aff, 0x3c3c3cff },
};
#define COLOR_COUNT (int)(sizeof(colors) / sizeof(colors[0]))

struct note {
	char *path;
	char *screen; // output name
	int x, y, width, height;
	int color;
	char text[MAX_TEXT];
	struct text_cursor tc;
	struct psurface *surface;
	struct loop_timer *save_timer;
	bool focused;
	// moving and resizing: what the pointer holds and where it took it
	int holding; // 0 nothing, 1 the strip, 2 the corner
	double press_x, press_y;
	int start_x, start_y, start_w, start_h;
	double px, py;
	bool inside;
};

static struct {
	struct panel *panel;
	list_t *notes; // struct note *
	bool loaded;
} ns;

static void note_show(struct note *n);
void notes_init(struct panel *panel);

/* ---------- files ---------- */

static char *notes_dir(void) {
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char path[1024];
	if (data && *data) {
		snprintf(path, sizeof(path), "%s/tileWin/notes", data);
	} else if (home) {
		snprintf(path, sizeof(path), "%s/.local/share/tileWin/notes", home);
	} else {
		return NULL;
	}
	return strdup(path);
}

static void note_write(struct note *n) {
	FILE *f = fopen(n->path, "w");
	if (!f) {
		sway_log(SWAY_ERROR, "Cannot save the note %s", n->path);
		return;
	}
	fprintf(f, "#note %d %d %d %d %s %s\n%s", n->x, n->y, n->width, n->height,
		colors[n->color].name, n->screen ? n->screen : "-", n->text);
	fclose(f);
}

static void save_fired(void *data) {
	struct note *n = data;
	n->save_timer = NULL;
	note_write(n);
}

static void save_soon(struct note *n) {
	if (n->save_timer) {
		loop_remove_timer(ns.panel->loop, n->save_timer);
	}
	n->save_timer = loop_add_timer(ns.panel->loop, SAVE_MS, save_fired, n);
}

static void save_now(struct note *n) {
	if (n->save_timer) {
		loop_remove_timer(ns.panel->loop, n->save_timer);
		n->save_timer = NULL;
	}
	note_write(n);
}

static struct note *note_read(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	struct note *n = calloc(1, sizeof(*n));
	char head[512] = "", color[32] = "yellow", screen[128] = "-";
	if (!fgets(head, sizeof(head), f) || sscanf(head, "#note %d %d %d %d %31s %127s", &n->x,
			&n->y, &n->width, &n->height, color, screen) < 4) {
		fclose(f);
		free(n);
		return NULL;
	}
	size_t len = fread(n->text, 1, MAX_TEXT - 1, f);
	n->text[len] = '\0';
	fclose(f);
	n->path = strdup(path);
	n->screen = strcmp(screen, "-") == 0 ? NULL : strdup(screen);
	for (int i = 0; i < COLOR_COUNT; i++) {
		if (strcmp(colors[i].name, color) == 0) {
			n->color = i;
		}
	}
	n->width = n->width < 120 ? 120 : n->width;
	n->height = n->height < 80 ? 80 : n->height;
	text_cursor_end(n->text, &n->tc, false);
	return n;
}

static void note_free(struct note *n) {
	if (n->save_timer) {
		loop_remove_timer(ns.panel->loop, n->save_timer);
	}
	if (n->surface) {
		psurface_destroy(n->surface);
	}
	free(n->path);
	free(n->screen);
	free(n);
}

static void load_all(void) {
	if (ns.loaded) {
		return;
	}
	ns.loaded = true;
	char *dir = notes_dir();
	DIR *d = dir ? opendir(dir) : NULL;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len < 5 || strcmp(e->d_name + len - 4, ".txt") != 0) {
			continue;
		}
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct note *n = note_read(path);
		if (n) {
			list_add(ns.notes, n);
		}
	}
	if (d) {
		closedir(d);
	}
	free(dir);
}

/* ---------- drawing ---------- */

static bool dark(void) {
	return ns.panel->theme && ns.panel->theme->dark;
}

static void draw_glyph_plus(cairo_t *cr, double cx, double cy, uint32_t c) {
	pd_rect(cr, cx - 5, cy - 1, 10, 2, c);
	pd_rect(cr, cx - 1, cy - 5, 2, 10, c);
}

static void draw_glyph_close(cairo_t *cr, double cx, double cy, uint32_t c) {
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 4, cy - 4);
	cairo_line_to(cr, cx + 4, cy + 4);
	cairo_move_to(cr, cx + 4, cy - 4);
	cairo_line_to(cr, cx - 4, cy + 4);
	pd_color(cr, c);
	cairo_set_line_width(cr, 1.6);
	cairo_stroke(cr);
}

static void render(struct psurface *s, cairo_t *cr) {
	struct note *n = s->data;
	double W = s->width, H = s->height;
	bool d = dark();
	uint32_t paper = d ? colors[n->color].dark_paper : colors[n->color].paper;
	uint32_t strip = d ? colors[n->color].dark_strip : colors[n->color].strip;
	uint32_t ink = d ? 0xf2f2f2ff : 0x2a2a2aff;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	// a soft shadow, then the paper with a little light at its top
	for (int i = 3; i >= 1; i--) {
		cairo_rectangle(cr, i, i + 1, W - 4, H - 4);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.07 * (4 - i));
		cairo_fill(cr);
	}
	double w = W - 4, h = H - 4;
	pd_rect(cr, 0, 0, w, h, paper);
	pd_rect(cr, 0, 0, w, STRIP_H, strip);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, STRIP_H, 0, h);
	cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.25);
	cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
	cairo_rectangle(cr, 0, STRIP_H, w, h - STRIP_H);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	// "+" and "x" on the strip show while the pointer is on the note
	if (n->inside || n->focused) {
		uint32_t glyph = (ink & 0xffffff00) | 0x99;
		draw_glyph_plus(cr, 12, STRIP_H / 2.0, glyph);
		draw_glyph_close(cr, w - 12, STRIP_H / 2.0, glyph);
		// the corner to pull
		for (int i = 0; i < 3; i++) {
			cairo_new_path(cr);
			cairo_move_to(cr, w - 3 - i * 4, h - 3);
			cairo_line_to(cr, w - 3, h - 3 - i * 4);
			pd_color(cr, (ink & 0xffffff00) | 0x50);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
	}
	psurface_add_hotspot(s, 0, 0, 24, STRIP_H, NULL, 1, 0, NULL);
	psurface_add_hotspot(s, (int)w - 24, 0, 24, STRIP_H, NULL, 2, 0, NULL);

	// the text, wrapped, with the cursor while the note has the keyboard
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(
		"Segoe Print, Comic Neue, Noto Sans 11");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_width(layout, (int)((w - 2 * TEXT_PAD) * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(layout, n->text, -1);
	size_t lo = n->tc.cursor < n->tc.anchor ? n->tc.cursor : n->tc.anchor;
	size_t hi = n->tc.cursor < n->tc.anchor ? n->tc.anchor : n->tc.cursor;
	if (n->focused && hi > lo) {
		PangoAttrList *attrs = pango_attr_list_new();
		PangoAttribute *bg = pango_attr_background_new(0x3300, 0x9900, 0xff00);
		bg->start_index = lo;
		bg->end_index = hi;
		pango_attr_list_insert(attrs, bg);
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	}
	cairo_save(cr);
	cairo_rectangle(cr, 0, STRIP_H, w, h - STRIP_H);
	cairo_clip(cr);
	cairo_move_to(cr, TEXT_PAD, STRIP_H + 6);
	pd_color(cr, ink);
	pango_cairo_show_layout(cr, layout);
	if (n->focused) {
		PangoRectangle pos;
		pango_layout_index_to_pos(layout, (int)n->tc.cursor, &pos);
		pd_rect(cr, TEXT_PAD + pos.x / (double)PANGO_SCALE, STRIP_H + 6 + pos.y /
			(double)PANGO_SCALE, 1.5, pos.height / (double)PANGO_SCALE, ink);
	}
	cairo_restore(cr);
	g_object_unref(layout);
}

/* ---------- input ---------- */

static struct note *new_note(struct panel *panel, struct panel_output *output, int x, int y);

static void delete_note(struct note *n) {
	if (n->path) {
		unlink(n->path);
	}
	list_del(ns.notes, list_find(ns.notes, n));
	note_free(n);
}

/* The surface can't be destroyed inside its own input handler. */
static void delete_later_fired(void *data) {
	delete_note(data);
}

static void pick_color(struct note *n, int color) {
	n->color = color;
	save_now(n);
	if (n->surface) {
		psurface_set_dirty(n->surface);
	}
}

void notes_color_command(struct panel *panel, const char *path, const char *color) {
	for (int i = 0; ns.notes && i < ns.notes->length; i++) {
		struct note *n = ns.notes->items[i];
		for (int c = 0; c < COLOR_COUNT && strcmp(n->path, path) == 0; c++) {
			if (strcmp(colors[c].name, color) == 0) {
				pick_color(n, c);
			}
		}
	}
}

static void pointer_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	struct note *n = s->data;
	double w = s->width - 4, h = s->height - 4;
	if (!pressed) {
		if (n->holding) {
			n->holding = 0;
			save_now(n);
		}
		return;
	}
	if (button == BTN_RIGHT) {
		list_t *items = create_list();
		for (int c = 0; c < COLOR_COUNT; c++) {
			char cmd[1200];
			snprintf(cmd, sizeof(cmd), "panel note color \"%s\" %s", n->path, colors[c].name);
			struct menu_item *item = menu_item_new(colors[c].label, cmd);
			item->checked = c == n->color;
			list_add(items, item);
		}
		list_add(items, menu_item_separator());
		list_add(items, menu_item_new("New note", "panel note new"));
		struct popup_anchor anchor = { .output = s->output, .x = n->x + (int)x,
			.y = n->y + (int)y };
		menu_open(s->panel, items, true, anchor, NULL);
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	if (y < STRIP_H && x < 24) {
		new_note(s->panel, s->output, n->x + 30, n->y + 30);
		return;
	}
	if (y < STRIP_H && x >= w - 24) {
		loop_add_timer(s->panel->loop, 0, delete_later_fired, n);
		return;
	}
	n->press_x = x;
	n->press_y = y;
	n->start_x = n->x;
	n->start_y = n->y;
	n->start_w = n->width;
	n->start_h = n->height;
	if (y < STRIP_H) {
		n->holding = 1;
	} else if (x >= w - CORNER && y >= h - CORNER) {
		n->holding = 2;
	} else {
		text_cursor_end(n->text, &n->tc, false); // typing goes on at the end
		psurface_set_dirty(s);
	}
}

static void pointer_motion(struct psurface *s, double x, double y) {
	struct note *n = s->data;
	n->px = x;
	n->py = y;
	if (!n->inside) {
		n->inside = true;
		psurface_set_dirty(s);
	}
	if (!n->holding) {
		return;
	}
	// while the button is down the compositor goes on measuring the pointer from
	// where the note was when it was pressed, however far it has moved since
	double dx = x - n->press_x, dy = y - n->press_y;
	if (n->holding == 1) {
		n->x = n->start_x + (int)dx;
		n->y = n->start_y + (int)dy;
		n->x = n->x < 0 ? 0 : n->x;
		n->y = n->y < 0 ? 0 : n->y;
		zwlr_layer_surface_v1_set_margin(s->layer_surface, n->y, 0, 0, n->x);
		wl_surface_commit(s->surface);
	} else {
		int nw = n->start_w + (int)dx, nh = n->start_h + (int)dy;
		n->width = nw < 120 ? 120 : nw > 900 ? 900 : nw;
		n->height = nh < 80 ? 80 : nh > 900 ? 900 : nh;
		psurface_set_size(s, n->width + 4, n->height + 4);
		wl_surface_commit(s->surface);
	}
}

static void pointer_leave(struct psurface *s) {
	struct note *n = s->data;
	n->inside = false;
	psurface_set_dirty(s);
}

static void key(struct psurface *s, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct note *n = s->data;
	n->focused = true;
	enum text_key_result r;
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		// a new line, in place of what is selected
		size_t lo = n->tc.cursor < n->tc.anchor ? n->tc.cursor : n->tc.anchor;
		size_t hi = n->tc.cursor < n->tc.anchor ? n->tc.anchor : n->tc.cursor;
		size_t len = strlen(n->text);
		hi = hi > len ? len : hi;
		lo = lo > hi ? hi : lo;
		if (len - (hi - lo) + 1 >= MAX_TEXT) {
			return;
		}
		memmove(n->text + lo + 1, n->text + hi, len - hi + 1);
		n->text[lo] = '\n';
		n->tc.cursor = n->tc.anchor = lo + 1;
		r = TEXT_KEY_CHANGED;
	} else if (sym == XKB_KEY_Escape) {
		n->focused = false;
		psurface_set_dirty(s);
		return;
	} else {
		r = text_key(n->text, MAX_TEXT, &n->tc, sym, utf8, mods);
	}
	if (r == TEXT_KEY_CHANGED) {
		save_soon(n);
	}
	if (r != TEXT_KEY_IGNORED) {
		psurface_set_dirty(s);
	}
}

static void keyboard_leave(struct psurface *s) {
	struct note *n = s->data;
	n->focused = false;
	if (n->save_timer) {
		save_now(n);
	}
	psurface_set_dirty(s);
}

static void closed(struct psurface *s) {
	struct note *n = s->data;
	n->surface = NULL;
	psurface_destroy(s);
}

static const struct psurface_impl impl = {
	.render = render,
	.pointer_motion = pointer_motion,
	.pointer_leave = pointer_leave,
	.pointer_button = pointer_button,
	.key = key,
	.keyboard_leave = keyboard_leave,
	.closed = closed,
};

/* ---------- notes on the screens ---------- */

static struct panel_output *output_of(struct note *n) {
	struct panel_output *output;
	wl_list_for_each(output, &ns.panel->outputs, link) {
		if (n->screen && output->name && strcmp(output->name, n->screen) == 0 &&
				output->ready) {
			return output;
		}
	}
	return panel_main_output(ns.panel);
}

static void note_show(struct note *n) {
	struct panel_output *output = output_of(n);
	if (!output || !output->ready) {
		return;
	}
	if (n->surface && n->surface->output == output) {
		return;
	}
	if (n->surface) {
		psurface_destroy(n->surface);
	}
	struct psurface *s = psurface_create(ns.panel, output, &impl, n,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "tilewin-sticky-note");
	zwlr_layer_surface_v1_set_anchor(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	zwlr_layer_surface_v1_set_margin(s->layer_surface, n->y, 0, 0, n->x);
	psurface_set_size(s, n->width + 4, n->height + 4);
	wl_surface_commit(s->surface);
	n->surface = s;
}

static struct note *new_note(struct panel *panel, struct panel_output *output, int x, int y) {
	char *dir = notes_dir();
	if (!dir) {
		return NULL;
	}
	tw_mkdir_p(dir);
	char path[1024];
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	snprintf(path, sizeof(path), "%s/note-%lld%03ld.txt", dir, (long long)ts.tv_sec,
		ts.tv_nsec / 1000000);
	free(dir);
	struct note *n = calloc(1, sizeof(*n));
	n->path = strdup(path);
	n->screen = output && output->name ? strdup(output->name) : NULL;
	n->width = NOTE_W;
	n->height = NOTE_H;
	// near the top right of the screen, as Windows put them
	n->x = x >= 0 ? x : (output ? output->width - NOTE_W - 60 : 100);
	n->y = y >= 0 ? y : 60;
	list_add(ns.notes, n);
	note_write(n);
	note_show(n);
	return n;
}

void notes_command(struct panel *panel, int argc, char **argv) {
	notes_init(panel);
	if (argc >= 1 && strcmp(argv[0], "new") == 0) {
		new_note(panel, panel_main_output(panel), -1, -1);
	} else if (argc >= 3 && strcmp(argv[0], "color") == 0) {
		notes_color_command(panel, argv[1], argv[2]);
	}
}

void notes_init(struct panel *panel) {
	ns.panel = panel;
	if (!ns.notes) {
		ns.notes = create_list();
	}
	load_all();
}

void notes_outputs_changed(struct panel *panel) {
	notes_init(panel);
	for (int i = 0; i < ns.notes->length; i++) {
		note_show(ns.notes->items[i]);
	}
}

void notes_output_removed(struct panel_output *output) {
	for (int i = 0; ns.notes && i < ns.notes->length; i++) {
		struct note *n = ns.notes->items[i];
		if (n->surface && n->surface->output == output) {
			psurface_destroy(n->surface);
			n->surface = NULL;
		}
	}
}

void notes_theme_changed(void) {
	for (int i = 0; ns.notes && i < ns.notes->length; i++) {
		struct note *n = ns.notes->items[i];
		if (n->surface) {
			psurface_set_dirty(n->surface);
		}
	}
}

void notes_fini(void) {
	for (int i = 0; ns.notes && i < ns.notes->length; i++) {
		struct note *n = ns.notes->items[i];
		if (n->save_timer) {
			save_now(n);
		}
		note_free(n);
	}
	list_free(ns.notes);
	ns.notes = NULL;
	ns.loaded = false;
}
