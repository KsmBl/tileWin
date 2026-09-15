#include <cairo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "list.h"
#include "settings.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "tw_theme.h"

/* ---------- search index ---------- */

static GPtrArray *search_index; // struct ui_search_entry *
static char *index_page, *index_page_title, *index_keywords, *index_group;

static void index_add(const char *title, const char *subtitle, GtkWidget *widget) {
	if (!index_page || !title || !*title) {
		return;
	}
	if (!search_index) {
		search_index = g_ptr_array_new();
	}
	struct ui_search_entry *e = g_new0(struct ui_search_entry, 1);
	e->page = g_strdup(index_page);
	e->page_title = g_strdup(index_page_title);
	e->group = g_strdup(widget ? index_group : NULL);
	e->title = g_strdup(title);
	e->subtitle = g_strdup(subtitle);
	e->keywords = g_strdup(widget ? NULL : index_keywords);
	e->widget = widget;
	if (widget) {
		// rows that are rebuilt later disappear from the results
		g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&e->widget);
	}
	g_ptr_array_add(search_index, e);
}

void ui_index_page(const char *name, const char *title, const char *keywords) {
	g_clear_pointer(&index_page, g_free);
	g_clear_pointer(&index_page_title, g_free);
	g_clear_pointer(&index_keywords, g_free);
	g_clear_pointer(&index_group, g_free);
	if (!name) {
		return;
	}
	index_page = g_strdup(name);
	index_page_title = g_strdup(title);
	index_keywords = g_strdup(keywords);
	index_add(title, NULL, NULL);
}

static bool contains(const char *text, const char *word) {
	if (!text || !*word) {
		return false;
	}
	char *folded = g_utf8_casefold(text, -1);
	bool found = strstr(folded, word) != NULL;
	g_free(folded);
	return found;
}

static int score(const struct ui_search_entry *e, char **words) {
	int total = 0;
	for (int i = 0; words[i]; i++) {
		const char *w = words[i];
		int best = 0;
		char *title = g_utf8_casefold(e->title, -1);
		if (g_str_has_prefix(title, w)) {
			best = 100;
		} else if (strstr(title, w)) {
			best = 60;
		} else if (contains(e->group, w)) {
			best = 30;
		} else if (contains(e->subtitle, w)) {
			best = 20;
		} else if (contains(e->page_title, w) || contains(e->keywords, w)) {
			best = 10;
		}
		g_free(title);
		if (best == 0) {
			return 0; // every word has to match
		}
		total += best;
	}
	return total + (e->widget ? 0 : 5);
}

static int hit_cmp(gconstpointer a, gconstpointer b) {
	const struct ui_search_entry *x = *(struct ui_search_entry **)a;
	const struct ui_search_entry *y = *(struct ui_search_entry **)b;
	if (x->score != y->score) {
		return y->score - x->score;
	}
	return g_utf8_collate(x->title, y->title);
}

GPtrArray *ui_search(const char *query) {
	GPtrArray *hits = g_ptr_array_new();
	char *folded = g_utf8_casefold(query, -1);
	char **words = g_strsplit_set(g_strstrip(folded), " \t", -1);
	int count = 0;
	for (int i = 0; words[i]; i++) {
		if (*words[i]) {
			words[count++] = words[i];
		} else {
			g_free(words[i]);
		}
	}
	words[count] = NULL;
	for (guint i = 0; count && search_index && i < search_index->len; i++) {
		struct ui_search_entry *e = search_index->pdata[i];
		bool gone = e->group && !e->widget; // a row that no longer exists
		e->score = gone ? 0 : score(e, words);
		if (e->score > 0) {
			g_ptr_array_add(hits, e);
		}
	}
	g_ptr_array_sort(hits, hit_cmp);
	// a group and its first row often have the same name: show it once
	for (guint i = 0; i < hits->len; i++) {
		struct ui_search_entry *a = hits->pdata[i];
		for (guint j = i + 1; j < hits->len; j++) {
			struct ui_search_entry *b = hits->pdata[j];
			if (strcmp(a->page, b->page) == 0 && g_utf8_collate(a->title, b->title) == 0) {
				g_ptr_array_remove_index(hits, j--);
			}
		}
	}
	g_strfreev(words);
	g_free(folded);
	return hits;
}

GtkWidget *ui_page(const char *title, const char *description, GtkWidget **content) {
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(box, 32);
	gtk_widget_set_margin_end(box, 32);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 32);
	GtkWidget *heading = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_widget_add_css_class(heading, "tw-title");
	gtk_box_append(GTK_BOX(box), heading);
	if (description) {
		GtkWidget *label = gtk_label_new(description);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_box_append(GTK_BOX(box), label);
	}
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	*content = box;
	return scroll;
}

GtkWidget *ui_group(GtkWidget *content, const char *title, const char *description) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top(box, 14);
	if (title) {
		GtkWidget *label = gtk_label_new(title);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_widget_add_css_class(label, "tw-heading");
		gtk_box_append(GTK_BOX(box), label);
	}
	if (description) {
		GtkWidget *label = gtk_label_new(description);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_box_append(GTK_BOX(box), label);
	}
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_list_box_set_show_separators(GTK_LIST_BOX(list), TRUE);
	gtk_widget_add_css_class(list, "tw-group");
	gtk_box_append(GTK_BOX(box), list);
	gtk_box_append(GTK_BOX(content), box);
	if (title) {
		g_free(index_group);
		index_group = g_strdup(title);
		index_add(title, description, list);
	}
	return list;
}

GtkWidget *ui_row(GtkWidget *group, const char *title, const char *subtitle,
		GtkWidget *control) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(labels, TRUE);
	gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
	if (title) {
		GtkWidget *label = gtk_label_new(title);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_box_append(GTK_BOX(labels), label);
	}
	if (subtitle) {
		GtkWidget *label = gtk_label_new(subtitle);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "tw-caption");
		gtk_box_append(GTK_BOX(labels), label);
	}
	gtk_box_append(GTK_BOX(box), labels);
	if (control) {
		gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), control);
	}
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(group), row);
	index_add(title, subtitle, row);
	return row;
}

GtkWidget *ui_row_box(GtkWidget *row) {
	return gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
}

void ui_closure_free(gpointer data, GClosure *closure) {
	g_free(data);
}

GtkWidget *ui_icon_button(const char *icon, const char *tooltip, bool sensitive,
		GCallback callback, gpointer data) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive(button, sensitive);
	g_signal_connect_data(button, "clicked", callback, data, ui_closure_free, 0);
	return button;
}

GtkWidget *ui_app_icon(const char *icon, int size) {
	GtkWidget *image;
	if (icon && icon[0] == '/') {
		image = gtk_image_new_from_file(icon);
	} else {
		image = gtk_image_new_from_icon_name(icon && *icon ? icon : "application-x-executable");
	}
	gtk_image_set_pixel_size(GTK_IMAGE(image), size);
	return image;
}

char *ui_display_value(const char *value) {
	GString *out = g_string_new(NULL);
	for (const char *p = value; p && *p; p++) {
		if (*p == '\n') {
			g_string_append(out, "\\n");
		} else if (*p == '\t') {
			g_string_append(out, "\\t");
		} else {
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

char *ui_input_value(const char *text) {
	GString *out = g_string_new(NULL);
	for (const char *p = text; p && *p; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			g_string_append_c(out, '\n');
			p++;
		} else if (p[0] == '\\' && p[1] == 't') {
			g_string_append_c(out, '\t');
			p++;
		} else {
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

/* ---------- applications ---------- */

static list_t *apps = NULL;

static list_t *get_apps(void) {
	if (!apps) {
		list_t *all = tw_desktop_scan();
		apps = create_list();
		for (int i = 0; i < all->length; i++) {
			struct tw_desktop_entry *e = all->items[i];
			if (e->no_display || e->hidden) {
				tw_desktop_entry_free(e);
			} else {
				list_add(apps, e);
			}
		}
		list_free(all);
	}
	return apps;
}

const struct tw_desktop_entry *ui_find_app(const char *id) {
	list_t *all = get_apps();
	char *with_suffix = g_strconcat(id, ".desktop", NULL);
	const struct tw_desktop_entry *found = NULL;
	for (int i = 0; i < all->length && !found; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (strcmp(e->id, id) == 0 || strcmp(e->id, with_suffix) == 0) {
			found = e;
		}
	}
	g_free(with_suffix);
	return found;
}

struct picker {
	void (*callback)(const char *id, gpointer data);
	gpointer data;
	GtkWidget *popover, *search, *list;
	bool populated;
};

static gboolean picker_filter(GtkListBoxRow *row, gpointer data) {
	struct picker *p = data;
	const char *query = gtk_editable_get_text(GTK_EDITABLE(p->search));
	if (!*query) {
		return TRUE;
	}
	const char *haystack = g_object_get_data(G_OBJECT(row), "haystack");
	char *needle = g_utf8_casefold(query, -1);
	gboolean match = haystack && strstr(haystack, needle);
	g_free(needle);
	return match;
}

static void picker_show(GtkWidget *popover, gpointer data) {
	struct picker *p = data;
	gtk_editable_set_text(GTK_EDITABLE(p->search), "");
	gtk_widget_grab_focus(p->search);
	if (p->populated) {
		return;
	}
	p->populated = true;
	list_t *all = get_apps();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(box, 6);
		gtk_widget_set_margin_end(box, 6);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		gtk_box_append(GTK_BOX(box), ui_app_icon(e->icon, 24));
		GtkWidget *label = gtk_label_new(e->name);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(box), label);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		char *haystack = g_strdup_printf("%s %s", e->name, e->id);
		g_object_set_data_full(G_OBJECT(row), "haystack", g_utf8_casefold(haystack, -1), g_free);
		g_free(haystack);
		g_object_set_data_full(G_OBJECT(row), "id", g_strdup(e->id), g_free);
		gtk_list_box_append(GTK_LIST_BOX(p->list), row);
	}
}

static void picker_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct picker *p = data;
	char *id = g_strdup(g_object_get_data(G_OBJECT(row), "id"));
	gtk_popover_popdown(GTK_POPOVER(p->popover));
	if (id) {
		p->callback(id, p->data);
	}
	g_free(id);
}

static void picker_search_changed(GtkSearchEntry *entry, gpointer data) {
	struct picker *p = data;
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(p->list));
}

GtkWidget *ui_app_picker(const char *label, void (*callback)(const char *id, gpointer data),
		gpointer data) {
	struct picker *p = g_new0(struct picker, 1);
	p->callback = callback;
	p->data = data;
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_label(GTK_MENU_BUTTON(button), label);
	p->popover = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	p->search = gtk_search_entry_new();
	gtk_box_append(GTK_BOX(box), p->search);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, 340, 380);
	p->list = gtk_list_box_new();
	gtk_list_box_set_filter_func(GTK_LIST_BOX(p->list), picker_filter, p, NULL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->list);
	gtk_box_append(GTK_BOX(box), scroll);
	gtk_popover_set_child(GTK_POPOVER(p->popover), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), p->popover);
	g_signal_connect(p->popover, "show", G_CALLBACK(picker_show), p);
	g_signal_connect(p->list, "row-activated", G_CALLBACK(picker_activated), p);
	g_signal_connect(p->search, "search-changed", G_CALLBACK(picker_search_changed), p);
	g_object_set_data_full(G_OBJECT(button), "picker", p, g_free);
	return button;
}

/* ---------- ordered app lists (quick launch, pinned apps) ---------- */

enum {
	APP_UP,
	APP_DOWN,
	APP_REMOVE,
};

struct app_action {
	struct app_list *list;
	guint index;
	int op;
};

static void app_list_rebuild(struct app_list *l);

static gboolean app_list_rebuild_idle(gpointer data) {
	struct app_list *l = data;
	l->rebuild_id = 0;
	app_list_rebuild(l);
	return G_SOURCE_REMOVE;
}

static void app_list_schedule(struct app_list *l) {
	if (!l->rebuild_id) {
		l->rebuild_id = g_idle_add(app_list_rebuild_idle, l);
	}
}

static void app_action(GtkButton *button, gpointer data) {
	struct app_action *a = data;
	struct app_list *l = a->list;
	guint i = a->index;
	if (i >= l->ids->len) {
		return;
	}
	gpointer *d = l->ids->pdata;
	gpointer tmp;
	switch (a->op) {
	case APP_UP:
		if (i == 0) {
			return;
		}
		tmp = d[i];
		d[i] = d[i - 1];
		d[i - 1] = tmp;
		break;
	case APP_DOWN:
		if (i + 1 >= l->ids->len) {
			return;
		}
		tmp = d[i];
		d[i] = d[i + 1];
		d[i + 1] = tmp;
		break;
	case APP_REMOVE:
		g_ptr_array_remove_index(l->ids, i);
		break;
	}
	l->changed(l, l->data);
	app_list_schedule(l);
}

static void app_added(const char *id, gpointer data) {
	struct app_list *l = data;
	for (guint i = 0; i < l->ids->len; i++) {
		if (strcmp(l->ids->pdata[i], id) == 0) {
			return;
		}
	}
	g_ptr_array_add(l->ids, g_strdup(id));
	l->changed(l, l->data);
	app_list_schedule(l);
}

static void add_app_button(struct app_list *l, GtkWidget *box, const char *icon,
		const char *tooltip, bool sensitive, guint index, int op) {
	struct app_action *a = g_new0(struct app_action, 1);
	a->list = l;
	a->index = index;
	a->op = op;
	gtk_box_append(GTK_BOX(box), ui_icon_button(icon, tooltip, sensitive,
		G_CALLBACK(app_action), a));
}

static void app_list_rebuild(struct app_list *l) {
	gtk_list_box_remove_all(GTK_LIST_BOX(l->list));
	for (guint i = 0; i < l->ids->len; i++) {
		const char *id = l->ids->pdata[i];
		const struct tw_desktop_entry *e = ui_find_app(id);
		GtkWidget *row = ui_row(l->list, e ? e->name : id,
			e ? id : "Not installed, skipped", NULL);
		GtkWidget *box = ui_row_box(row);
		gtk_box_prepend(GTK_BOX(box), ui_app_icon(e ? e->icon : NULL, 32));
		add_app_button(l, box, "go-up-symbolic", "Move up", i > 0, i, APP_UP);
		add_app_button(l, box, "go-down-symbolic", "Move down", i + 1 < l->ids->len, i,
			APP_DOWN);
		add_app_button(l, box, "list-remove-symbolic", "Remove", true, i, APP_REMOVE);
	}
	ui_row(l->list, NULL, l->ids->len ? NULL : "No apps", ui_app_picker("Add app…", app_added, l));
}

struct app_list *ui_app_list_new(GtkWidget *content, const char *title,
		const char *description, void (*changed)(struct app_list *, gpointer), gpointer data) {
	struct app_list *l = g_new0(struct app_list, 1);
	l->list = ui_group(content, title, description);
	l->ids = g_ptr_array_new_with_free_func(g_free);
	l->changed = changed;
	l->data = data;
	return l;
}

void ui_app_list_set(struct app_list *l, GPtrArray *ids) {
	g_ptr_array_set_size(l->ids, 0);
	for (guint i = 0; ids && i < ids->len; i++) {
		g_ptr_array_add(l->ids, g_strdup(ids->pdata[i]));
	}
	app_list_rebuild(l);
}

/* ---------- wallpapers ---------- */

static const char *const wallpaper_exts[] = { "jpg", "jpeg", "png", "webp", "svg" };

char *ui_wallpaper_dropin(const char *basename) {
	char *dir = tw_config_dir();
	if (!dir) {
		return NULL;
	}
	char *found = NULL;
	for (size_t i = 0; i < G_N_ELEMENTS(wallpaper_exts) && !found; i++) {
		char *path = g_strdup_printf("%s/wallpapers/%s.%s", dir, basename, wallpaper_exts[i]);
		if (g_file_test(path, G_FILE_TEST_EXISTS)) {
			found = path;
		} else {
			g_free(path);
		}
	}
	free(dir);
	return found;
}

void ui_wallpaper_remove_dropins(const char *basename) {
	char *dir = tw_config_dir();
	if (!dir) {
		return;
	}
	for (size_t i = 0; i < G_N_ELEMENTS(wallpaper_exts); i++) {
		char *path = g_strdup_printf("%s/wallpapers/%s.%s", dir, basename, wallpaper_exts[i]);
		g_remove(path);
		g_free(path);
	}
	free(dir);
}

static void source_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static GdkTexture *render_wallpaper(const char *type, uint32_t color1, uint32_t color2,
		bool vertical, const char *image, int width, int height) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(surface);
	if (g_ascii_strcasecmp(type, "gradient") == 0) {
		cairo_pattern_t *pattern = cairo_pattern_create_linear(0, 0,
			vertical ? 0 : width, vertical ? height : 0);
		for (int i = 0; i < 2; i++) {
			uint32_t c = i ? color2 : color1;
			cairo_pattern_add_color_stop_rgba(pattern, i, (c >> 24 & 0xff) / 255.0,
				(c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0, 1);
		}
		cairo_set_source(cr, pattern);
		cairo_paint(cr);
		cairo_pattern_destroy(pattern);
	} else if (g_ascii_strcasecmp(type, "none") == 0) {
		for (int y = 0; y < height; y += 8) {
			for (int x = 0; x < width; x += 8) {
				source_color(cr, ((x + y) / 8) % 2 ? 0x909090ff : 0xb0b0b0ff);
				cairo_rectangle(cr, x, y, 8, 8);
				cairo_fill(cr);
			}
		}
	} else {
		source_color(cr, color1 | 0xff);
		cairo_paint(cr);
		if (image && g_ascii_strcasecmp(type, "image") == 0) {
			cairo_surface_t *picture = tw_image_render_cover(image, width, height);
			if (picture) {
				cairo_set_source_surface(cr, picture, 0, 0);
				cairo_paint(cr);
				cairo_surface_destroy(picture);
			}
		}
	}
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	int stride = cairo_image_surface_get_stride(surface);
	GBytes *bytes = g_bytes_new(cairo_image_surface_get_data(surface), (gsize)stride * height);
	GdkTexture *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_DEFAULT, bytes, stride);
	g_bytes_unref(bytes);
	cairo_surface_destroy(surface);
	return texture;
}

GdkTexture *ui_wallpaper_texture(const char *wallpaper, const char *theme, int width,
		int height) {
	int argc = 0;
	char **argv = NULL;
	if (wallpaper && *wallpaper && g_shell_parse_argv(wallpaper, &argc, &argv, NULL) &&
			argc > 0 && g_ascii_strcasecmp(argv[0], "theme") != 0) {
		uint32_t color1 = 0x000000ff, color2;
		char *image = NULL;
		if (g_ascii_strcasecmp(argv[0], "image") == 0) {
			image = argc > 1 ? tw_expand_home(argv[1]) : NULL;
			if (argc > 3) {
				tw_parse_color(argv[3], &color1);
			}
		} else if (argc > 1) {
			tw_parse_color(argv[1], &color1);
		}
		color2 = color1;
		if (argc > 2 && g_ascii_strcasecmp(argv[0], "gradient") == 0) {
			tw_parse_color(argv[2], &color2);
		}
		bool vertical = !(argc > 3 && g_ascii_strcasecmp(argv[3], "horizontal") == 0);
		GdkTexture *texture = render_wallpaper(argv[0], color1, color2, vertical, image,
			width, height);
		free(image);
		g_strfreev(argv);
		return texture;
	}
	g_strfreev(argv);

	char *error = NULL;
	struct tw_theme *t = theme ? tw_theme_load(theme, &error) : NULL;
	free(error);
	uint32_t color1 = t ? tw_theme_color(t, "wallpaper.color", 0x3a6ea5ff) : 0x3a6ea5ff;
	char *dropin = theme ? ui_wallpaper_dropin(theme) : NULL;
	GdkTexture *texture;
	if (dropin || !t) {
		texture = render_wallpaper(dropin ? "image" : "solid", color1, color1, true, dropin,
			width, height);
	} else {
		const char *type = tw_theme_str(t, "wallpaper.type", "solid");
		uint32_t color2 = tw_theme_color(t, "wallpaper.color2", color1);
		bool vertical = g_ascii_strcasecmp(tw_theme_str(t, "wallpaper.direction", "vertical"),
			"horizontal") != 0;
		const char *image = tw_theme_str(t, "wallpaper.image", NULL);
		char *path = NULL;
		if (image) {
			path = image[0] == '/' || image[0] == '~' || !t->dir ?
				tw_expand_home(image) : tw_theme_file(t, image);
		}
		texture = render_wallpaper(type, color1, color2, vertical, path, width, height);
		free(path);
	}
	g_free(dropin);
	if (t) {
		tw_theme_free(t);
	}
	return texture;
}
