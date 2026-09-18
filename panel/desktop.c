#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "log.h"
#include "popup.h"
#include "textfield.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "tw_paths.h"

/*
 * The desktop: icons of the files in ~/Desktop and a right-click menu to
 * create folders, documents and shortcuts. Every output gets a transparent
 * input surface (a 1x1 buffer scaled to the output) below the windows for
 * clicks on the empty desktop, and a narrow surface that only covers the
 * icon columns, so the desktop costs almost no memory.
 */

#define ICON_SIZE 48
#define DRAG_THRESHOLD 6
#define CELL_W 100
#define CELL_H 100
#define GRID_MARGIN 10
#define DOUBLE_CLICK_MS 400

enum {
	DESK_HS_BACKGROUND = 1,
	DESK_HS_ITEM,
};

struct desktop_item {
	char *name;  // file name
	char *path;
	char *label; // shown name
	char **icons; // icon names to try, NULL-terminated
	bool dir;
	bool selected;
	struct tw_desktop_entry *entry; // .desktop files
	int col, row; // cell of the grid it sits in
};

/* Where the user dragged an icon; kept in the state dir between sessions. */
struct saved_pos {
	char *name;
	int col, row;
};

static struct {
	list_t *items; // struct desktop_item *
	list_t *positions; // struct saved_pos *
	int selected, hover; // the icon clicked last, and the one under the pointer
	int64_t last_click_ms;
	struct loop_timer *rescan_timer;
	struct {
		bool armed, active;
		bool group;              // the whole selection moves along
		int index;
		double start_x, start_y; // where the button went down
		double grab_x, grab_y;   // where inside the icon it was grabbed
		double x, y;             // pointer now
		int col, row;            // cell it would land in
	} drag;
	/* Rubber band: dragging on the empty desktop selects the icons it covers. */
	struct {
		bool armed, active;
		double x0, y0, x1, y1;
	} band;
} desktop = { .selected = -1, .hover = -1 };

/* ---------- selection ---------- */

static void select_none(void) {
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		item->selected = false;
	}
	desktop.selected = -1;
}

static int selection_count(void) {
	int count = 0;
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		count += item->selected;
	}
	return count;
}

/* The one selected icon, NULL when none or several are selected. */
static struct desktop_item *only_selected(void) {
	struct desktop_item *only = NULL;
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		if (!item->selected) {
			continue;
		}
		if (only) {
			return NULL;
		}
		only = item;
	}
	return only;
}

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char *desktop_directory(void) {
	const char *home = g_get_home_dir();
	const char *special = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
	if (special) {
		char *normalized = g_canonicalize_filename(special, NULL);
		char *home_normalized = g_canonicalize_filename(home, NULL);
		bool is_home = strcmp(normalized, home_normalized) == 0;
		g_free(home_normalized);
		if (!is_home) {
			char *dir = strdup(normalized);
			g_free(normalized);
			return dir;
		}
		g_free(normalized);
	}
	// without a configured desktop folder (or when it is the home folder)
	return format_str("%s/Desktop", home);
}

/* ---------- items ---------- */

static void item_free(struct desktop_item *item) {
	free(item->name);
	free(item->path);
	free(item->label);
	g_strfreev(item->icons);
	if (item->entry) {
		tw_desktop_entry_free(item->entry);
	}
	free(item);
}

static char **icon_candidates(const char *name, bool dir) {
	GPtrArray *names = g_ptr_array_new();
	if (dir) {
		g_ptr_array_add(names, g_strdup("folder"));
	} else {
		char *type = g_content_type_guess(name, NULL, 0, NULL);
		GIcon *icon = type ? g_content_type_get_icon(type) : NULL;
		if (icon && G_IS_THEMED_ICON(icon)) {
			const char *const *theme_names = g_themed_icon_get_names(G_THEMED_ICON(icon));
			for (int i = 0; theme_names && theme_names[i]; i++) {
				g_ptr_array_add(names, g_strdup(theme_names[i]));
			}
		}
		if (icon) {
			g_object_unref(icon);
		}
		g_free(type);
		g_ptr_array_add(names, g_strdup("text-x-generic"));
	}
	g_ptr_array_add(names, g_strdup("unknown"));
	g_ptr_array_add(names, NULL);
	return (char **)g_ptr_array_free(names, FALSE);
}

static int item_cmp(const void *a, const void *b) {
	const struct desktop_item *ia = *(struct desktop_item **)a;
	const struct desktop_item *ib = *(struct desktop_item **)b;
	if (ia->dir != ib->dir) {
		return ia->dir ? -1 : 1;
	}
	char *ka = g_utf8_casefold(ia->label, -1), *kb = g_utf8_casefold(ib->label, -1);
	int result = g_utf8_collate(ka, kb);
	g_free(ka);
	g_free(kb);
	return result;
}

static void desktop_scan(void) {
	if (desktop.items) {
		for (int i = 0; i < desktop.items->length; i++) {
			item_free(desktop.items->items[i]);
		}
		list_free(desktop.items);
	}
	desktop.items = create_list();
	desktop.selected = desktop.hover = -1;
	char *dir = desktop_directory();
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		struct desktop_item *item = calloc(1, sizeof(*item));
		item->name = strdup(de->d_name);
		item->path = format_str("%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(item->path, &st) != 0) {
			item_free(item);
			continue;
		}
		item->dir = S_ISDIR(st.st_mode);
		size_t len = strlen(item->name);
		if (!item->dir && len > 8 && strcmp(item->name + len - 8, ".desktop") == 0) {
			item->entry = tw_desktop_load(item->path, item->name);
		}
		if (item->entry && item->entry->name) {
			item->label = strdup(item->entry->name);
			const char *names[] = { item->entry->icon ? item->entry->icon :
				"application-x-executable", "application-x-executable", NULL };
			item->icons = g_strdupv((char **)names);
		} else {
			item->label = strdup(item->name);
			item->icons = icon_candidates(item->name, item->dir);
		}
		list_add(desktop.items, item);
	}
	if (d) {
		closedir(d);
	}
	free(dir);
	list_qsort(desktop.items, item_cmp);
}

static struct desktop_item *item_at(int index) {
	return desktop.items && index >= 0 && index < desktop.items->length ?
		desktop.items->items[index] : NULL;
}

/* ---------- where the icons sit ---------- */

static char *positions_path(void) {
	char *dir = tw_state_dir();
	char *path = dir ? format_str("%s/desktop-icons", dir) : NULL;
	free(dir);
	return path;
}

static void positions_load(void) {
	if (desktop.positions) {
		return;
	}
	desktop.positions = create_list();
	char *path = positions_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	char line[1024];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		int col = 0, row = 0, offset = 0;
		if (sscanf(line, "%d %d %n", &col, &row, &offset) >= 2 && offset > 0 && line[offset]) {
			struct saved_pos *pos = calloc(1, sizeof(*pos));
			pos->name = strdup(line + offset);
			pos->col = col;
			pos->row = row;
			list_add(desktop.positions, pos);
		}
	}
	if (f) {
		fclose(f);
	}
	free(path);
}

static struct saved_pos *positions_find(const char *name) {
	for (int i = 0; desktop.positions && i < desktop.positions->length; i++) {
		struct saved_pos *pos = desktop.positions->items[i];
		if (strcmp(pos->name, name) == 0) {
			return pos;
		}
	}
	return NULL;
}

static void positions_save(void) {
	char *path = positions_path();
	if (!path) {
		return;
	}
	char *content = strdup("");
	for (int i = 0; desktop.positions && i < desktop.positions->length; i++) {
		struct saved_pos *pos = desktop.positions->items[i];
		char *next = format_str("%s%d %d %s\n", content, pos->col, pos->row, pos->name);
		free(content);
		content = next;
	}
	tw_write_string(path, content);
	free(content);
	free(path);
}

static void positions_set(const char *name, int col, int row) {
	positions_load();
	struct saved_pos *pos = positions_find(name);
	if (!pos) {
		pos = calloc(1, sizeof(*pos));
		pos->name = strdup(name);
		list_add(desktop.positions, pos);
	}
	pos->col = col;
	pos->row = row;
}

/* Forgets where an icon sat, so it flows into a free cell again. */
static void positions_remove(const char *name) {
	positions_load();
	for (int i = 0; i < desktop.positions->length; i++) {
		struct saved_pos *pos = desktop.positions->items[i];
		if (strcmp(pos->name, name) == 0) {
			free(pos->name);
			free(pos);
			list_del(desktop.positions, i);
			return;
		}
	}
}

static void positions_clear(void) {
	positions_load();
	for (int i = 0; i < desktop.positions->length; i++) {
		struct saved_pos *pos = desktop.positions->items[i];
		free(pos->name);
		free(pos);
	}
	desktop.positions->length = 0;
	positions_save();
}

static struct psurface *primary_icons(struct panel *panel) {
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		if (output->desktop) {
			return output->desktop;
		}
	}
	return NULL;
}

static int grid_rows(struct psurface *s) {
	int rows = (s->height - 2 * GRID_MARGIN) / CELL_H;
	return rows > 0 ? rows : 1;
}

static int grid_columns(struct psurface *s) {
	int width = s->output ? s->output->width : s->width;
	int columns = (width - 2 * GRID_MARGIN) / CELL_W;
	return columns > 0 ? columns : 1;
}

/* Whether an icon moves with the drag: the grabbed one, or the whole selection. */
static bool drag_moves(int index, struct desktop_item *item) {
	return index == desktop.drag.index || (desktop.drag.group && item->selected);
}

static struct desktop_item *item_in_cell(int col, int row, int except) {
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		if (i != except && item->col == col && item->row == row) {
			return item;
		}
	}
	return NULL;
}

/*
 * Gives every icon a cell: the ones the user dragged somewhere keep that place,
 * the rest fill the free cells column by column. Returns the last used column.
 */
static int layout_items(struct psurface *s) {
	if (!desktop.items) {
		return 0;
	}
	positions_load();
	int rows = s->configured ? grid_rows(s) : 6;
	int columns = grid_columns(s);
	for (int i = 0; i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		item->col = item->row = -1;
	}
	for (int i = 0; i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		struct saved_pos *pos = positions_find(item->name);
		if (pos && pos->col >= 0 && pos->col < columns && pos->row >= 0 && pos->row < rows &&
				!item_in_cell(pos->col, pos->row, i)) {
			item->col = pos->col;
			item->row = pos->row;
		}
	}
	int max_col = 0;
	for (int i = 0; i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		for (int c = 0; item->col < 0 && c < columns; c++) {
			for (int r = 0; item->col < 0 && r < rows; r++) {
				if (!item_in_cell(c, r, i)) {
					item->col = c;
					item->row = r;
				}
			}
		}
		if (item->col < 0) {
			item->col = columns - 1;
			item->row = rows - 1;
		}
		max_col = item->col > max_col ? item->col : max_col;
	}
	return max_col;
}

/* The icons surface is as wide as the icon columns it shows. */
static void update_icons_size(struct panel *panel, struct psurface *s) {
	bool primary = s == primary_icons(panel) && desktop.items && desktop.items->length;
	int width = 1;
	if (primary) {
		width = 2 * GRID_MARGIN + (layout_items(s) + 1) * CELL_W;
	}
	if ((desktop.drag.active || desktop.band.active) && primary && s->output) {
		width = s->output->width; // room to drag an icon or a band anywhere
	}
	if (s->req_width != width) {
		psurface_set_size(s, width, 0);
		wl_surface_commit(s->surface);
	}
}

static void desktop_refresh_surfaces(struct panel *panel) {
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		if (output->desktop) {
			update_icons_size(panel, output->desktop);
			psurface_set_dirty(output->desktop);
		}
	}
}

static void rescan_now(struct panel *panel) {
	desktop_scan();
	desktop_refresh_surfaces(panel);
}

static void rescan_timer_fired(void *data) {
	struct panel *panel = data;
	desktop.rescan_timer = NULL;
	rescan_now(panel);
}

void desktop_dir_changed(struct panel *panel) {
	if (!desktop.rescan_timer) {
		desktop.rescan_timer = loop_add_timer(panel->loop, 200, rescan_timer_fired, panel);
	}
}

/* ---------- drawing ---------- */

static void draw_label(cairo_t *cr, const char *font, const char *text, double x, double y,
		double width, bool full) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	if (!full) {
		pango_layout_set_height(layout, -2);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	}
	pango_layout_set_text(layout, text, -1);
	// white text with a shadow stays readable on any wallpaper
	cairo_move_to(cr, x + 1, y + 1);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	pango_cairo_show_layout(cr, layout);
	cairo_move_to(cr, x, y);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void draw_cell(cairo_t *cr, double x, double y, uint32_t fill, uint32_t border) {
	cairo_new_path(cr);
	pd_rounded(cr, x + 2.5, y + 2.5, CELL_W - 5, CELL_H - 5, 3);
	pd_color(cr, fill);
	cairo_fill_preserve(cr);
	pd_color(cr, border);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void draw_item(struct psurface *s, cairo_t *cr, struct desktop_item *item,
		double x, double y, bool selected, bool hover, double alpha) {
	struct panel *panel = s->panel;
	if (selected || hover) {
		draw_cell(cr, x, y, selected ? 0x3399ff55 : 0xffffff26,
			selected ? 0x99ccffb0 : 0xffffff40);
	}
	cairo_surface_t *icon = NULL;
	for (int k = 0; item->icons && item->icons[k] && !icon; k++) {
		if (tw_icon_theme_has(item->icons[k])) {
			icon = apps_icon(panel, item->icons[k], ICON_SIZE * s->scale);
		}
	}
	for (int k = 0; item->icons && item->icons[k] && !icon; k++) {
		icon = apps_icon(panel, item->icons[k], ICON_SIZE * s->scale);
	}
	if (alpha < 1) {
		cairo_push_group(cr);
	}
	pd_icon(cr, icon, x + (CELL_W - ICON_SIZE) / 2.0, y + 8, ICON_SIZE);
	draw_label(cr, bar_font(panel), item->label, x + 4, y + 12 + ICON_SIZE, CELL_W - 8, selected);
	if (alpha < 1) {
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, alpha);
	}
}

static void icons_render(struct psurface *s, cairo_t *cr) {
	struct panel *panel = s->panel;
	psurface_add_hotspot(s, 0, 0, s->width, s->height, NULL, DESK_HS_BACKGROUND, -1, NULL);
	if (s != primary_icons(panel) || !desktop.items) {
		return;
	}
	layout_items(s);
	bool dragging = desktop.drag.active;
	for (int i = 0; i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		double x = GRID_MARGIN + item->col * CELL_W, y = GRID_MARGIN + item->row * CELL_H;
		if (!(dragging && drag_moves(i, item))) {
			draw_item(s, cr, item, x, y, item->selected, i == desktop.hover, 1);
		}
		psurface_add_hotspot(s, x, y, CELL_W, CELL_H, NULL, DESK_HS_ITEM, i, NULL);
	}
	if (desktop.band.active) {
		double x = fmin(desktop.band.x0, desktop.band.x1);
		double y = fmin(desktop.band.y0, desktop.band.y1);
		double w = fabs(desktop.band.x1 - desktop.band.x0);
		double h = fabs(desktop.band.y1 - desktop.band.y0);
		pd_rect(cr, x, y, w, h, 0x3399ff44);
		cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
		pd_color(cr, 0x99ccffc0);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	struct desktop_item *dragged = dragging ? item_at(desktop.drag.index) : NULL;
	if (dragged) {
		// every icon of the group follows the pointer by the same amount, so
		// they keep the places they have next to each other
		int dcol = desktop.drag.col - dragged->col, drow = desktop.drag.row - dragged->row;
		double dx = desktop.drag.x - desktop.drag.grab_x -
			(GRID_MARGIN + dragged->col * CELL_W);
		double dy = desktop.drag.y - desktop.drag.grab_y -
			(GRID_MARGIN + dragged->row * CELL_H);
		for (int i = 0; i < desktop.items->length; i++) {
			struct desktop_item *item = desktop.items->items[i];
			if (drag_moves(i, item)) {
				draw_cell(cr, GRID_MARGIN + (item->col + dcol) * CELL_W,
					GRID_MARGIN + (item->row + drow) * CELL_H, 0xffffff26, 0xffffff90);
			}
		}
		for (int i = 0; i < desktop.items->length; i++) {
			struct desktop_item *item = desktop.items->items[i];
			if (drag_moves(i, item)) {
				draw_item(s, cr, item, GRID_MARGIN + item->col * CELL_W + dx,
					GRID_MARGIN + item->row * CELL_H + dy, false, false, 0.75);
			}
		}
	}
}

/* ---------- menus ---------- */

static struct menu_item *add_item(list_t *items, const char *label, const char *icon,
		const char *command) {
	struct menu_item *item = menu_item_new(label, command);
	if (icon) {
		item->icon = strdup(icon);
	}
	list_add(items, item);
	return item;
}

static list_t *background_menu(struct panel *panel) {
	list_t *items = create_list();
	add_item(items, "Refresh", "view-refresh", "panel desktop refresh");
	add_item(items, "Sort icons", "view-sort-ascending", "panel desktop sort");
	list_add(items, menu_item_separator());
	struct menu_item *new_menu = add_item(items, "New", "document-new", NULL);
	new_menu->children = create_list();
	add_item(new_menu->children, "Folder", "folder-new", "panel desktop new folder");
	add_item(new_menu->children, "Text document", "accessories-text-editor", "panel desktop new text");
	list_add(new_menu->children, menu_item_separator());
	add_item(new_menu->children, "Shortcut...", "emblem-symbolic-link", "panel desktop new shortcut");
	list_add(items, menu_item_separator());
	add_item(items, "Open desktop folder", "folder", "panel desktop folder");
	add_item(items, "Open terminal here", "utilities-terminal", "panel desktop terminal");
	list_t *custom = panel_named_menu(panel, "desktop");
	if (custom) {
		list_add(items, menu_item_separator());
		list_cat(items, custom);
		list_free(custom);
	}
	list_add(items, menu_item_separator());
	add_item(items, "Change wallpaper", "preferences-desktop-wallpaper",
		"exec tilewin-settings --page wallpaper");
	add_item(items, "Personalize", "preferences-desktop-theme", "exec tilewin-settings --page theme");
	return items;
}

static list_t *item_menu(struct desktop_item *item) {
	list_t *items = create_list();
	struct menu_item *open = add_item(items, "Open", NULL, "panel desktop open {id}");
	open->bold = true;
	if (item->dir) {
		add_item(items, "Open terminal here", "utilities-terminal", "panel desktop terminal {id}");
	}
	list_add(items, menu_item_separator());
	int selected = selection_count();
	if (selected < 2) {
		add_item(items, "Rename", "edit-rename", "panel desktop rename {id}");
		add_item(items, "Delete", "edit-delete", "panel desktop delete {id}");
	} else {
		char *label = format_str("Delete %d items", selected);
		add_item(items, label, "edit-delete", "panel desktop delete {id}");
		free(label);
	}
	return items;
}

/* ---------- input ---------- */

/* Selects every icon the rubber band covers. */
static void band_select(void) {
	double x0 = fmin(desktop.band.x0, desktop.band.x1);
	double x1 = fmax(desktop.band.x0, desktop.band.x1);
	double y0 = fmin(desktop.band.y0, desktop.band.y1);
	double y1 = fmax(desktop.band.y0, desktop.band.y1);
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		double cx = GRID_MARGIN + item->col * CELL_W, cy = GRID_MARGIN + item->row * CELL_H;
		item->selected = cx + CELL_W > x0 && cx < x1 && cy + CELL_H > y0 && cy < y1;
	}
}

enum dialog_mode {
	DIALOG_NEW_FOLDER,
	DIALOG_NEW_TEXT,
	DIALOG_NEW_SHORTCUT,
	DIALOG_RENAME,
	DIALOG_DELETE, // there is no trash here: delete for good?
};

static void open_dialog(struct panel *panel, enum dialog_mode mode, struct desktop_item *item);
static void open_delete_dialog(struct panel *panel, list_t *paths);

static int surface_offset_y(struct psurface *s) {
	struct panel *panel = s->panel;
	bool bottom = panel->config ? panel->config->layouts[panel->layout].bottom : true;
	return !bottom && s->output && s->output->bar ? s->output->bar->height : 0;
}

static void open_item(struct panel *panel, struct desktop_item *item) {
	if (item->entry && item->entry->url && *item->entry->url) {
		// a shortcut to a place (KDE writes short urls such as trash:/)
		const char *url = item->entry->url;
		char *full = strncmp(url, "trash:", 6) == 0 ? strdup("trash:///") : strdup(url);
		char *quoted = g_shell_quote(full);
		ipc_panel_commandf(panel, "exec xdg-open %s", quoted);
		g_free(quoted);
		free(full);
		return;
	}
	if (item->entry && item->entry->exec) {
		apps_launch(panel, item->entry);
		return;
	}
	char *quoted = g_shell_quote(item->path);
	ipc_panel_commandf(panel, "exec xdg-open %s", quoted);
	g_free(quoted);
}

/*
 * Files and folders go to the trash. Some places have none (another mount, a
 * file system without one), and there the caller asks whether to delete.
 */
static bool trash_item(struct desktop_item *item) {
	GFile *file = g_file_new_for_path(item->path);
	GError *error = NULL;
	bool trashed = g_file_trash(file, NULL, &error);
	if (!trashed) {
		sway_log(SWAY_INFO, "Could not move %s to the trash: %s", item->path,
			error ? error->message : "unknown error");
		g_clear_error(&error);
	}
	g_object_unref(file);
	return trashed;
}

/*
 * The cell the grabbed icon would land in. A group keeps its shape, so the
 * cell is kept close enough to the grid for every icon of it to fit.
 */
static void drag_update_target(struct psurface *s) {
	double x = desktop.drag.x - desktop.drag.grab_x, y = desktop.drag.y - desktop.drag.grab_y;
	int columns = grid_columns(s), rows = grid_rows(s);
	int col = (int)round((x - GRID_MARGIN) / CELL_W);
	int row = (int)round((y - GRID_MARGIN) / CELL_H);
	int lo_col = 0, hi_col = columns - 1, lo_row = 0, hi_row = rows - 1;
	struct desktop_item *grabbed = item_at(desktop.drag.index);
	if (desktop.drag.group && grabbed) {
		int min_col = grabbed->col, max_col = grabbed->col;
		int min_row = grabbed->row, max_row = grabbed->row;
		for (int i = 0; i < desktop.items->length; i++) {
			struct desktop_item *item = desktop.items->items[i];
			if (!drag_moves(i, item)) {
				continue;
			}
			min_col = item->col < min_col ? item->col : min_col;
			max_col = item->col > max_col ? item->col : max_col;
			min_row = item->row < min_row ? item->row : min_row;
			max_row = item->row > max_row ? item->row : max_row;
		}
		lo_col = grabbed->col - min_col;
		hi_col = columns - 1 - (max_col - grabbed->col);
		lo_row = grabbed->row - min_row;
		hi_row = rows - 1 - (max_row - grabbed->row);
	}
	if (hi_col < lo_col) {
		hi_col = lo_col;
	}
	if (hi_row < lo_row) {
		hi_row = lo_row;
	}
	desktop.drag.col = col < lo_col ? lo_col : col > hi_col ? hi_col : col;
	desktop.drag.row = row < lo_row ? lo_row : row > hi_row ? hi_row : row;
}

static void drag_finish(struct psurface *s) {
	struct desktop_item *item = item_at(desktop.drag.index);
	if (desktop.drag.active && item && desktop.drag.group) {
		int dcol = desktop.drag.col - item->col, drow = desktop.drag.row - item->row;
		// an icon that stays behind and sits where the group lands loses its
		// place and flows into a free cell again
		for (int i = 0; i < desktop.items->length; i++) {
			struct desktop_item *other = desktop.items->items[i];
			if (drag_moves(i, other)) {
				continue;
			}
			for (int j = 0; j < desktop.items->length; j++) {
				struct desktop_item *moved = desktop.items->items[j];
				if (drag_moves(j, moved) && other->col == moved->col + dcol &&
						other->row == moved->row + drow) {
					positions_remove(other->name);
					break;
				}
			}
		}
		for (int i = 0; i < desktop.items->length; i++) {
			struct desktop_item *moved = desktop.items->items[i];
			if (drag_moves(i, moved)) {
				positions_set(moved->name, moved->col + dcol, moved->row + drow);
			}
		}
		positions_save();
	} else if (desktop.drag.active && item) {
		struct desktop_item *other = item_in_cell(desktop.drag.col, desktop.drag.row,
			desktop.drag.index);
		if (other) {
			positions_set(other->name, item->col, item->row); // they swap places
		}
		positions_set(item->name, desktop.drag.col, desktop.drag.row);
		positions_save();
	}
	desktop.drag.armed = desktop.drag.active = desktop.drag.group = false;
	update_icons_size(s->panel, s);
	desktop_refresh_surfaces(s->panel);
}

static void desktop_motion(struct psurface *s, double x, double y) {
	if (desktop.drag.armed || desktop.drag.active) {
		desktop.drag.x = x;
		desktop.drag.y = y;
		if (!desktop.drag.active && (fabs(x - desktop.drag.start_x) > DRAG_THRESHOLD ||
				fabs(y - desktop.drag.start_y) > DRAG_THRESHOLD)) {
			desktop.drag.active = true;
			update_icons_size(s->panel, s);
		}
		if (desktop.drag.active) {
			drag_update_target(s);
			psurface_set_dirty(s);
			return;
		}
	}
	if (desktop.band.armed || desktop.band.active) {
		desktop.band.x1 = x;
		desktop.band.y1 = y;
		if (!desktop.band.active && (fabs(x - desktop.band.x0) > DRAG_THRESHOLD ||
				fabs(y - desktop.band.y0) > DRAG_THRESHOLD)) {
			desktop.band.active = true;
		}
		if (desktop.band.active) {
			band_select();
			struct psurface *icons = primary_icons(s->panel);
			if (icons) {
				update_icons_size(s->panel, icons); // room to drag anywhere
				psurface_set_dirty(icons);
			}
			return;
		}
	}
	struct hotspot *hs = psurface_hotspot_at(s, x, y);
	int hover = hs && hs->kind == DESK_HS_ITEM ? (int)hs->id : -1;
	if (hover != desktop.hover) {
		desktop.hover = hover;
		psurface_set_dirty(s);
	}
}

static void desktop_leave(struct psurface *s) {
	if (desktop.hover != -1) {
		desktop.hover = -1;
		desktop_refresh_surfaces(s->panel);
	}
}

static void desktop_button(struct psurface *s, double x, double y, uint32_t button,
		bool pressed) {
	struct panel *panel = s->panel;
	if (button == BTN_LEFT && !pressed) {
		if (desktop.drag.armed || desktop.drag.active) {
			drag_finish(s);
		}
		if (desktop.band.armed || desktop.band.active) {
			desktop.band.armed = desktop.band.active = false;
			struct psurface *icons = primary_icons(panel);
			if (icons) {
				update_icons_size(panel, icons);
			}
			desktop_refresh_surfaces(panel);
		}
		return;
	}
	if (!pressed || (button != BTN_LEFT && button != BTN_RIGHT)) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(s, x, y);
	int index = hs && hs->kind == DESK_HS_ITEM ? (int)hs->id : -1;
	uint32_t mods = panel_modifiers(panel);
	if (button == BTN_LEFT) {
		int64_t now = now_ms();
		struct desktop_item *item = item_at(index);
		if (item && index == desktop.selected && now - desktop.last_click_ms < DOUBLE_CLICK_MS) {
			open_item(panel, item);
			now = 0;
		}
		if (mods && item) {
			item->selected = mods & 1 ? !item->selected : true; // Ctrl toggles
		} else if (!item || !item->selected) {
			select_none();
			if (item) {
				item->selected = true;
			}
		}
		desktop.selected = index;
		desktop.last_click_ms = now;
		if (!item) {
			// dragging on the empty desktop draws a selection rectangle
			desktop.band.armed = true;
			desktop.band.active = false;
			desktop.band.x0 = desktop.band.x1 = x;
			desktop.band.y0 = desktop.band.y1 = y;
		}
		if (item) {
			// dragging it to another cell starts once the pointer moves; when
			// it is one of several selected icons, they all come along
			desktop.drag.armed = true;
			desktop.drag.active = false;
			desktop.drag.group = item->selected && selection_count() > 1;
			desktop.drag.index = index;
			desktop.drag.start_x = desktop.drag.x = x;
			desktop.drag.start_y = desktop.drag.y = y;
			desktop.drag.grab_x = x - (GRID_MARGIN + item->col * CELL_W);
			desktop.drag.grab_y = y - (GRID_MARGIN + item->row * CELL_H);
		}
		desktop_refresh_surfaces(panel);
		return;
	}
	struct desktop_item *clicked = item_at(index);
	if (!clicked || !clicked->selected) {
		select_none();
		if (clicked) {
			clicked->selected = true;
		}
	}
	desktop.selected = index;
	desktop_refresh_surfaces(panel);
	struct popup_anchor anchor = {
		.output = s->output,
		.x = (int)x,
		.y = (int)y + surface_offset_y(s),
	};
	struct desktop_item *item = clicked;
	char context[16];
	snprintf(context, sizeof(context), "%d", index);
	menu_open(panel, item ? item_menu(item) : background_menu(panel), true, anchor, context);
}

/*
 * Moves every selected icon to the trash, or the clicked one when nothing is
 * selected. What has no trash is collected and the dialog asks about it.
 */
static void delete_selection(struct panel *panel, struct desktop_item *clicked) {
	list_t *left = create_list();
	bool any = false;
	for (int i = 0; desktop.items && i < desktop.items->length; i++) {
		struct desktop_item *item = desktop.items->items[i];
		if (!item->selected && item != clicked) {
			continue;
		}
		any = true;
		if (!trash_item(item)) {
			list_add(left, strdup(item->path));
		}
	}
	if (left->length) {
		open_delete_dialog(panel, left); // it takes the list
	} else {
		list_free(left);
	}
	if (any) {
		rescan_now(panel);
	}
}

/* Delete moves the selected icons to the trash, F2 renames one. */
static void desktop_key(struct psurface *s, xkb_keysym_t sym, const char *utf8,
		uint32_t modifiers) {
	struct panel *panel = s->panel;
	struct desktop_item *item = item_at(desktop.selected);
	if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) {
		delete_selection(panel, item);
	} else if (sym == XKB_KEY_F2) {
		// the icon clicked last, or the only one a rectangle selected
		struct desktop_item *one = item ? item : only_selected();
		if (one) {
			open_dialog(panel, DIALOG_RENAME, one);
		}
	} else if (sym == XKB_KEY_Escape) {
		select_none(); // also what the rubber band selected
		desktop_refresh_surfaces(panel);
	} else if (sym == XKB_KEY_F5) {
		rescan_now(panel);
	}
}

static void desktop_configured(struct psurface *s) {
	struct panel_output *output = s->data;
	if (s == output->desktop) {
		update_icons_size(s->panel, s);
	}
}

static void desktop_closed(struct psurface *s) {
	struct panel_output *output = s->data;
	if (output->desktop == s) {
		output->desktop = NULL;
	}
	if (output->desktop_bg == s) {
		output->desktop_bg = NULL;
	}
	psurface_destroy(s);
}

static const struct psurface_impl icons_impl = {
	.render = icons_render,
	.pointer_motion = desktop_motion,
	.pointer_leave = desktop_leave,
	.pointer_button = desktop_button,
	.key = desktop_key,
	.configured = desktop_configured,
	.closed = desktop_closed,
};

static const struct psurface_impl background_impl = {
	.pointer_motion = desktop_motion,
	.pointer_button = desktop_button,
	.key = desktop_key,
	.closed = desktop_closed,
};

void desktop_create(struct panel_output *output) {
	struct panel *panel = output->panel;
	if (!output->ready || output->desktop) {
		return;
	}
	if (!desktop.items) {
		desktop_scan();
	}
	struct psurface *bg = psurface_create(panel, output, &background_impl, output,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "tilewin-desktop");
	bg->catcher = true; // transparent 1x1 buffer scaled to the whole surface
	// clicking the desktop gives it the keyboard, for Delete, F2 and Escape
	zwlr_layer_surface_v1_set_keyboard_interactivity(bg->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	if (panel->viewporter) {
		bg->viewport = wp_viewporter_get_viewport(panel->viewporter, bg->surface);
	}
	zwlr_layer_surface_v1_set_anchor(bg->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	wl_surface_commit(bg->surface);
	output->desktop_bg = bg;

	struct psurface *icons = psurface_create(panel, output, &icons_impl, output,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "tilewin-desktop-icons");
	zwlr_layer_surface_v1_set_anchor(icons->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(icons->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	output->desktop = icons;
	icons->req_width = -1;
	update_icons_size(panel, icons);
}

void desktop_destroy(struct panel_output *output) {
	if (output->desktop) {
		struct psurface *s = output->desktop;
		output->desktop = NULL;
		psurface_destroy(s);
	}
	if (output->desktop_bg) {
		struct psurface *s = output->desktop_bg;
		output->desktop_bg = NULL;
		psurface_destroy(s);
	}
}

/* ---------- name dialog: new folder, document, shortcut and rename ---------- */

enum {
	DLG_HS_OK = 1,
	DLG_HS_CANCEL,
};

struct name_dialog {
	enum dialog_mode mode;
	char *path;    // item being renamed
	list_t *paths; // char *, items to delete for good
	char text[512];
	struct text_cursor tc;
	char error[200];
	double px, py;
	bool inside;
};

static bool dialog_hover(struct name_dialog *d, double x, double y, double w, double h) {
	return d->inside && d->px >= x && d->px < x + w && d->py >= y && d->py < y + h;
}

static void dialog_render(struct popup *p, cairo_t *cr) {
	struct name_dialog *d = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	int M = popup_shadow_margin(panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "menu");

	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t bg = tw_theme_color(t, "menu.bg", 0xf2f2f2ff);
	int luma = (int)((bg >> 24 & 0xff) * 299 + (bg >> 16 & 0xff) * 587 + (bg >> 8 & 0xff) * 114) / 1000;
	bool dark = luma < 128;
	uint32_t field_bg = tw_theme_color(t, "menu.field_bg", dark ? 0x1f1f1fff : 0xffffffff);
	uint32_t field_fg = tw_theme_color(t, "menu.field_fg", dark ? 0xffffffff : 0x000000ff);
	uint32_t accent = style == PS_CLASSIC ? 0x000080ff :
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff);
	const char *font = tw_theme_str(t, "menu.font", bar_font(panel));

	static const char *const titles[] = { "New folder", "New text document", "New shortcut",
		"Rename", "Delete" };
	static const char *const prompts[] = { "Name of the new folder:",
		"Name of the new document:",
		"Type a program, a file or folder, or a web address:", "New name:",
		"There is no trash for this place. Delete it for good?" };
	int x0 = M + 18, cw = W - 2 * M - 36, y = M + 14;
	pd_text(cr, bar_bold_font(panel), titles[d->mode], x0, y, cw, 24, fg, PD_LEFT);
	y += 34;
	pd_text(cr, font, prompts[d->mode], x0, y, cw, 20, fg, PD_LEFT);
	y += 26;

	int fh = 30;
	if (d->mode == DIALOG_DELETE) {
		pd_text_wrapped(cr, font, d->text, x0, y, cw, 2, fg, true);
	} else if (style == PS_CLASSIC) {
		pd_rect(cr, x0, y, cw, fh, field_bg);
		pd_bevel(cr, x0, y, cw, fh, true);
	} else {
		cairo_new_path(cr);
		pd_rounded(cr, x0 + 0.5, y + 0.5, cw - 1, fh - 1, 4);
		pd_color(cr, field_bg);
		cairo_fill_preserve(cr);
		pd_color(cr, dark ? 0xffffff40 : 0x00000045);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pd_rect(cr, x0 + 1, y + fh - 2, cw - 2, 2, accent);
	}
	if (d->mode != DIALOG_DELETE) {
		struct text_style ts = { .font = font, .fg = field_fg, .caret = true };
		text_style_colors(panel, &ts);
		text_draw(cr, &ts, d->text, &d->tc, x0 + 8, y, cw - 16, fh);
	}
	y += fh + 6;
	if (d->error[0]) {
		pd_text(cr, font, d->error, x0, y, cw, 20, 0xe04040ff, PD_LEFT);
	}

	int bw = 90, bh = 30, by = H - M - bh - 14;
	int ok_x = W - M - 18 - 2 * bw - 8, cancel_x = W - M - 18 - bw;
	const struct {
		int x;
		const char *label;
		bool primary;
		int kind;
	} buttons[] = {
		{ ok_x, d->mode == DIALOG_RENAME ? "Rename" :
			d->mode == DIALOG_DELETE ? "Delete" : "Create", true, DLG_HS_OK },
		{ cancel_x, "Cancel", false, DLG_HS_CANCEL },
	};
	for (size_t i = 0; i < 2; i++) {
		int bx = buttons[i].x;
		bool hover = dialog_hover(d, bx, by, bw, bh);
		if (style == PS_CLASSIC) {
			pd_rect(cr, bx, by, bw, bh, 0xc0c0c0ff);
			pd_bevel(cr, bx, by, bw, bh, false);
			pd_text(cr, buttons[i].primary ? bar_bold_font(panel) : font, buttons[i].label,
				bx, by, bw, bh, 0x000000ff, PD_CENTER);
		} else {
			cairo_new_path(cr);
			pd_rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 4);
			uint32_t overlay = dark ? 0xffffff00 : 0x00000000;
			pd_color(cr, buttons[i].primary ? accent : overlay | (hover ? 0x30 : 0x14));
			cairo_fill_preserve(cr);
			pd_color(cr, overlay | 0x30);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			pd_text(cr, font, buttons[i].label, bx, by, bw, bh,
				buttons[i].primary ? 0xffffffff : fg, PD_CENTER);
		}
		psurface_add_hotspot(p->surface, bx, by, bw, bh, NULL, buttons[i].kind, 0, NULL);
	}
}

/* Deletes a file, or a folder with everything in it (there is no trash here). */
static bool delete_recursive(GFile *file, GError **error) {
	GFileEnumerator *dir = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	if (dir) {
		GFileInfo *info = NULL;
		while ((info = g_file_enumerator_next_file(dir, NULL, NULL))) {
			GFile *child = g_file_enumerator_get_child(dir, info);
			bool gone = delete_recursive(child, error);
			g_object_unref(child);
			g_object_unref(info);
			if (!gone) {
				g_object_unref(dir);
				return false;
			}
		}
		g_object_unref(dir);
	}
	return g_file_delete(file, NULL, error);
}

/* Escapes a value for a double-quoted desktop entry Exec argument. */
static char *exec_quote(const char *value) {
	GString *out = g_string_new("\"");
	for (const char *c = value; *c; c++) {
		if (strchr("\"`$\\", *c)) {
			g_string_append_c(out, '\\');
		}
		g_string_append_c(out, *c);
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

static char *unique_path(const char *dir, const char *name, const char *extension) {
	char *path = format_str("%s/%s%s", dir, name, extension);
	for (int n = 2; access(path, F_OK) == 0; n++) {
		free(path);
		path = format_str("%s/%s (%d)%s", dir, name, n, extension);
	}
	return path;
}

static bool create_shortcut(struct panel *panel, const char *dir, const char *target,
		char *error, size_t size) {
	GString *content = g_string_new("[Desktop Entry]\nVersion=1.0\n");
	char *name = NULL;
	char *expanded = tw_expand_home(target);
	struct stat st;
	if (strstr(target, "://")) {
		const char *host = strstr(target, "://") + 3;
		name = strndup(host, strcspn(host, "/?#"));
		if (!*name) {
			free(name);
			name = strdup("Link");
		}
		g_string_append_printf(content, "Type=Link\nName=%s\nURL=%s\nIcon=text-html\n", name,
			target);
	} else if (stat(expanded, &st) == 0 && !(S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR))) {
		char *base = g_path_get_basename(expanded);
		name = strdup(base);
		g_free(base);
		char *quoted = exec_quote(expanded);
		g_string_append_printf(content, "Type=Application\nName=%s\nExec=xdg-open %s\nIcon=%s\n",
			name, quoted, S_ISDIR(st.st_mode) ? "folder" : "text-x-generic");
		free(quoted);
	} else {
		// a command: use the name and icon of its app if it has a desktop entry
		size_t len = strcspn(target, " \t");
		char *program = strndup(target, len);
		const char *slash = strrchr(program, '/');
		const char *base = slash ? slash + 1 : program;
		if (!*base) {
			free(program);
			free(expanded);
			g_string_free(content, TRUE);
			snprintf(error, size, "Please type a program, file or web address");
			return false;
		}
		struct tw_desktop_entry *app = apps_find(base);
		name = strdup(app && app->name ? app->name : base);
		g_string_append_printf(content, "Type=Application\nName=%s\nExec=%s\nIcon=%s\n", name,
			target, app && app->icon ? app->icon : base);
		free(program);
	}
	free(expanded);
	for (char *c = name; *c; c++) {
		if (*c == '/') {
			*c = '-';
		}
	}
	char *path = unique_path(dir, name, ".desktop");
	bool ok = tw_write_string(path, content->str);
	if (ok) {
		chmod(path, 0755);
	} else {
		snprintf(error, size, "Could not create %s", path);
	}
	free(path);
	free(name);
	g_string_free(content, TRUE);
	return ok;
}

static void dialog_submit(struct popup *p) {
	struct name_dialog *d = p->data;
	struct panel *panel = p->panel;
	if (d->mode == DIALOG_DELETE) {
		for (int i = 0; d->paths && i < d->paths->length; i++) {
			GFile *file = g_file_new_for_path(d->paths->items[i]);
			GError *error = NULL;
			bool gone = delete_recursive(file, &error);
			g_object_unref(file);
			if (!gone) {
				snprintf(d->error, sizeof(d->error), "%s",
					error ? error->message : "Could not delete it");
				g_clear_error(&error);
				popup_set_dirty(p);
				rescan_now(panel);
				return;
			}
		}
		popup_close_later(panel);
		rescan_now(panel);
		return;
	}
	char *text = g_strstrip(g_strdup(d->text));
	d->error[0] = '\0';
	if (!*text) {
		snprintf(d->error, sizeof(d->error), "Please type a name");
	} else if (d->mode != DIALOG_NEW_SHORTCUT &&
			(strchr(text, '/') || strcmp(text, ".") == 0 || strcmp(text, "..") == 0)) {
		snprintf(d->error, sizeof(d->error), "A name cannot contain /");
	}
	char *dir = desktop_directory();
	tw_mkdir_p(dir);
	char *path = d->error[0] ? NULL : format_str("%s/%s", dir, text);
	int result = 0;
	switch (d->error[0] ? -1 : (int)d->mode) {
	case DIALOG_NEW_FOLDER:
		result = mkdir(path, 0755);
		break;
	case DIALOG_NEW_TEXT:;
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		result = fd < 0 ? -1 : close(fd);
		break;
	case DIALOG_RENAME:
		if (strcmp(path, d->path) != 0) {
			if (access(path, F_OK) == 0) {
				errno = EEXIST;
				result = -1;
			} else {
				result = rename(d->path, path);
			}
		}
		break;
	case DIALOG_NEW_SHORTCUT:
		if (!create_shortcut(panel, dir, text, d->error, sizeof(d->error))) {
			result = -2;
		}
		break;
	}
	if (result == -1) {
		snprintf(d->error, sizeof(d->error), "%s", errno == EEXIST ?
			"There already is an item with this name" : strerror(errno));
	}
	free(path);
	free(dir);
	free(text);
	if (d->error[0]) {
		popup_set_dirty(p);
		return;
	}
	popup_close_later(panel);
	rescan_now(panel);
}

static void dialog_motion(struct popup *p, double x, double y) {
	struct name_dialog *d = p->data;
	d->px = x;
	d->py = y;
	d->inside = true;
	popup_set_dirty(p);
}

static void dialog_leave(struct popup *p) {
	struct name_dialog *d = p->data;
	d->inside = false;
	popup_set_dirty(p);
}

static void dialog_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == DLG_HS_OK) {
		dialog_submit(p);
	} else if (hs && hs->kind == DLG_HS_CANCEL) {
		popup_close_later(p->panel);
	}
}

static void dialog_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct name_dialog *d = p->data;
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		dialog_submit(p);
		return;
	default:
		if (d->mode == DIALOG_DELETE) {
			return;
		}
		switch (text_key(d->text, sizeof(d->text), &d->tc, sym, utf8, mods)) {
		case TEXT_KEY_IGNORED:
			return;
		case TEXT_KEY_CHANGED:
			d->error[0] = '\0';
			break;
		case TEXT_KEY_MOVED:
			break;
		}
		break;
	}
	popup_set_dirty(p);
}

static void dialog_destroy(struct popup *p) {
	struct name_dialog *d = p->data;
	if (d->paths) {
		list_free_items_and_destroy(d->paths);
	}
	free(d->path);
	free(d);
}

static const struct popup_vtable dialog_vtable = {
	.render = dialog_render,
	.motion = dialog_motion,
	.leave = dialog_leave,
	.button = dialog_button,
	.key = dialog_key,
	.destroy = dialog_destroy,
};

static void open_dialog(struct panel *panel, enum dialog_mode mode, struct desktop_item *item) {
	struct psurface *s = primary_icons(panel);
	struct panel_output *output = s ? s->output : panel_focused_output(panel);
	if (!output) {
		return;
	}
	struct name_dialog *d = calloc(1, sizeof(*d));
	d->mode = mode;
	char *dir = desktop_directory();
	if (item) {
		d->path = strdup(item->path);
		snprintf(d->text, sizeof(d->text), "%s", item->name);
	} else if (mode == DIALOG_NEW_FOLDER || mode == DIALOG_NEW_TEXT) {
		const char *base = mode == DIALOG_NEW_FOLDER ? "New folder" : "New text document";
		const char *extension = mode == DIALOG_NEW_FOLDER ? "" : ".txt";
		char *path = unique_path(dir, base, extension);
		snprintf(d->text, sizeof(d->text), "%s", strrchr(path, '/') + 1);
		free(path);
	}
	free(dir);
	// like Windows: the name is selected, without the extension of a file
	const char *dot = strrchr(d->text, '.');
	bool keep_extension = dot && dot != d->text && mode != DIALOG_NEW_FOLDER &&
		!(item && item->dir);
	d->tc.anchor = 0;
	d->tc.cursor = keep_extension ? (size_t)(dot - d->text) : strlen(d->text);
	int M = popup_shadow_margin(panel);
	int width = 460 + 2 * M, height = 210 + 2 * M;
	popup_create(panel, POPUP_DIALOG, NULL, output, (output->width - width) / 2,
		(output->height - height) / 3, width, height, &dialog_vtable, d);
}

/* Asks whether to delete for good what could not go to the trash. */
static void open_delete_dialog(struct panel *panel, list_t *paths) {
	open_dialog(panel, DIALOG_DELETE, NULL);
	struct popup *p = panel->popup;
	struct name_dialog *d = p && p->kind == POPUP_DIALOG ? p->data : NULL;
	if (!d) {
		list_free_items_and_destroy(paths);
		return;
	}
	d->paths = paths;
	for (int i = 0; i < paths->length; i++) {
		const char *slash = strrchr(paths->items[i], '/');
		const char *name = slash ? slash + 1 : paths->items[i];
		size_t len = strlen(d->text);
		snprintf(d->text + len, sizeof(d->text) - len, "%s%s", len ? ", " : "", name);
	}
	popup_set_dirty(p);
}

/* ---------- commands (panel desktop ...) ---------- */

static void open_terminal(struct panel *panel, const char *dir) {
	const char *terminal = panel->config && panel->config->terminal ?
		panel->config->terminal : "xfce4-terminal";
	char *program = strndup(terminal, strcspn(terminal, " "));
	char *quoted_dir = g_shell_quote(dir);
	char *quoted_program = g_shell_quote(program);
	char *command = format_str("cd %s && exec %s", quoted_dir, quoted_program);
	proc_spawn(command);
	free(command);
	g_free(quoted_program);
	g_free(quoted_dir);
	free(program);
}

void desktop_handle_command(struct panel *panel, int argc, char **argv) {
	if (argc < 1) {
		return;
	}
	const char *action = argv[0];
	struct desktop_item *item = argc > 1 ? item_at(atoi(argv[1])) : NULL;
	if (strcmp(action, "refresh") == 0) {
		rescan_now(panel);
	} else if (strcmp(action, "sort") == 0) {
		positions_clear(); // back to the order of the names
		rescan_now(panel);
	} else if (strcmp(action, "new") == 0 && argc > 1) {
		if (strcmp(argv[1], "folder") == 0) {
			open_dialog(panel, DIALOG_NEW_FOLDER, NULL);
		} else if (strcmp(argv[1], "text") == 0) {
			open_dialog(panel, DIALOG_NEW_TEXT, NULL);
		} else if (strcmp(argv[1], "shortcut") == 0) {
			open_dialog(panel, DIALOG_NEW_SHORTCUT, NULL);
		}
	} else if (strcmp(action, "folder") == 0) {
		char *dir = desktop_directory();
		tw_mkdir_p(dir);
		char *quoted = g_shell_quote(dir);
		ipc_panel_commandf(panel, "exec xdg-open %s", quoted);
		g_free(quoted);
		free(dir);
	} else if (strcmp(action, "terminal") == 0) {
		char *dir = item && item->dir ? strdup(item->path) : desktop_directory();
		tw_mkdir_p(dir);
		open_terminal(panel, dir);
		free(dir);
	} else if (strcmp(action, "open") == 0 && item) {
		open_item(panel, item);
	} else if (strcmp(action, "rename") == 0 && item) {
		open_dialog(panel, DIALOG_RENAME, item);
	} else if (strcmp(action, "delete") == 0 && item) {
		delete_selection(panel, item);
	}
}
