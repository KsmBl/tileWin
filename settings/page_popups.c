/*
 * The Popups page: apps that come and go with a shortcut, at a place and a
 * size of their own, over the rest of the screen darkened (sway/wm/appopup.c).
 * Each is a block of ~/.config/tileWin/popups.conf,
 *
 *   popup thunderbird {
 *       app org.mozilla.Thunderbird.desktop
 *       position right
 *       width 70%
 *       height 100%
 *       dim 50
 *   }
 *
 * and its shortcut a line at the end of both mode files, so it wins over
 * theirs: "bindsym $mod+m popup toggle thunderbird".
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "confdoc.h"
#include "settings.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "twconf.h"

struct popup_entry {
	char *name, *app, *position;
	int width, height, dim; // percent
	bool slide;             // slides in and out (animation slide|none)
	char *keys;             // its shortcut, NULL for none
};

struct popups_page {
	struct settings *s;
	GPtrArray *entries; // struct popup_entry *
	GtkWidget *list;
};

static struct popups_page *current;

static const char *const positions[] = { "right", "left", "top", "bottom", "center", NULL };
static const char *const position_titles[] = { "Right", "Left", "Top", "Bottom", "Middle",
	NULL };
static const int dims[] = { 0, 30, 50, 70 };
static const char *const dim_titles[] = { "Not at all", "A little", "Half", "Much", NULL };

static void entry_free(gpointer data) {
	struct popup_entry *e = data;
	g_free(e->name);
	g_free(e->app);
	g_free(e->position);
	g_free(e->keys);
	g_free(e);
}

static char *conf_path(void) {
	char *dir = tw_config_dir();
	char *path = g_build_filename(dir, "popups.conf", NULL);
	free(dir);
	return path;
}

/* ---------- shortcuts, in common.conf ---------- */

static bool binding_of(struct confdoc *d, struct cstmt *c, const char *name, char **keys) {
	if (strcmp(c->name, "bindsym") != 0) {
		return false;
	}
	char *raw = cstmt_raw_args(d, c);
	char *pos = raw;
	while (g_str_has_prefix(pos, "--")) {
		pos += strcspn(pos, " \t");
		pos += strspn(pos, " \t");
	}
	size_t klen = strcspn(pos, " \t");
	char *command = pos + klen;
	command += strspn(command, " \t");
	char *wanted = g_strdup_printf("popup toggle %s", name);
	bool match = strcmp(g_strstrip(command), wanted) == 0;
	if (match && keys) {
		*keys = g_strndup(pos, klen);
	}
	g_free(wanted);
	g_free(raw);
	return match;
}

/* The shortcut of a popup: the same in both modes, so the first one found. */
static struct cstmt *find_binding_in(struct confdoc *d, const char *name, char **keys) {
	for (guint i = 0; d && i < d->root->children->len; i++) {
		struct cstmt *c = d->root->children->pdata[i];
		if (binding_of(d, c, name, keys)) {
			return c;
		}
	}
	return NULL;
}

static void find_binding(struct settings *s, const char *name, char **keys) {
	struct confdoc *docs[] = { s->windowmode, s->tilemode, s->common };
	for (size_t k = 0; k < G_N_ELEMENTS(docs); k++) {
		if (find_binding_in(docs[k], name, keys)) {
			return;
		}
	}
}

/*
 * Writes the shortcut into both mode files, after their own: the mode files
 * include common.conf first, and of two bindings of the same keys the later
 * one wins, so the popup takes the keys over.
 */
static void set_binding(struct settings *s, const char *name, const char *keys) {
	struct confdoc *old_common;
	while ((old_common = s->common) && find_binding_in(old_common, name, NULL)) {
		confdoc_remove(old_common, find_binding_in(old_common, name, NULL));
		settings_common_changed(s, false);
	}
	struct confdoc *docs[] = { s->windowmode, s->tilemode };
	for (size_t k = 0; k < G_N_ELEMENTS(docs); k++) {
		struct confdoc *d = docs[k];
		if (!d) {
			continue;
		}
		struct cstmt *old = find_binding_in(d, name, NULL);
		if (!keys || !*keys) {
			if (!old) {
				continue;
			}
			confdoc_remove(d, old);
		} else {
			char *text = g_strdup_printf("bindsym %s popup toggle %s", keys, name);
			if (old) {
				confdoc_replace(d, old, text);
			} else {
				confdoc_append(d, d->root, text);
			}
			g_free(text);
		}
		settings_mode_changed(s, d);
	}
}

/* What else these keys do, in either mode or common.conf; NULL for nothing. */
static char *keys_taken(struct settings *s, const char *keys, const char *except) {
	char *wanted = keyboard_normalize_keys(keys);
	struct confdoc *docs[] = { s->common, s->windowmode, s->tilemode };
	char *found = NULL;
	for (size_t k = 0; k < G_N_ELEMENTS(docs) && !found; k++) {
		struct confdoc *d = docs[k];
		for (guint i = 0; d && i < d->root->children->len && !found; i++) {
			struct cstmt *c = d->root->children->pdata[i];
			if (strcmp(c->name, "bindsym") != 0 || (except && binding_of(d, c, except, NULL))) {
				continue;
			}
			char *raw = cstmt_raw_args(d, c);
			char *pos = raw;
			while (g_str_has_prefix(pos, "--")) {
				pos += strcspn(pos, " \t");
				pos += strspn(pos, " \t");
			}
			size_t klen = strcspn(pos, " \t");
			char *other = g_strndup(pos, klen);
			if (g_str_has_prefix(other, "Mod4+")) {
				char *rest = g_strconcat("$mod+", other + 5, NULL);
				g_free(other);
				other = rest;
			}
			char *norm = keyboard_normalize_keys(other);
			if (strcmp(norm, wanted) == 0) {
				char *command = pos + klen;
				found = g_strdup(g_strstrip(command));
			}
			g_free(norm);
			g_free(other);
			g_free(raw);
		}
	}
	g_free(wanted);
	return found;
}

/* ---------- popups.conf ---------- */

static void load(struct popups_page *p) {
	g_ptr_array_set_size(p->entries, 0);
	char *path = conf_path();
	char *error = NULL;
	struct twconf_node *root = twconf_parse_file(path, &error);
	free(error);
	g_free(path);
	for (int i = 0; root && i < twconf_count(root); i++) {
		struct twconf_node *n = twconf_at(root, i);
		if (strcmp(n->name, "popup") != 0 || n->argc < 1) {
			continue;
		}
		struct popup_entry *e = g_new0(struct popup_entry, 1);
		e->name = g_strdup(n->argv[0]);
		const char *v = twconf_value(n, "app");
		e->app = g_strdup(v ? v : "");
		v = twconf_value(n, "position");
		e->position = g_strdup(v ? v : "center");
		v = twconf_value(n, "width");
		e->width = v ? atoi(v) : 60;
		v = twconf_value(n, "height");
		e->height = v ? atoi(v) : 70;
		v = twconf_value(n, "dim");
		e->dim = !v ? 50 : g_ascii_strcasecmp(v, "no") == 0 ? 0 : atoi(v);
		v = twconf_value(n, "animation");
		e->slide = !v || g_ascii_strcasecmp(v, "none") != 0;
		find_binding(p->s, e->name, &e->keys);
		g_ptr_array_add(p->entries, e);
	}
	twconf_free(root);
}

static bool save(struct popups_page *p) {
	GString *out = g_string_new("# App popups: apps that come and go with a shortcut. Made by the\n"
		"# Popups page of the settings; their shortcuts are in windowmode.conf and tilemode.conf.\n");
	for (guint i = 0; i < p->entries->len; i++) {
		struct popup_entry *e = p->entries->pdata[i];
		char *app = conf_quote(e->app);
		g_string_append_printf(out, "\npopup %s {\n\tapp %s\n\tposition %s\n\twidth %d%%\n"
			"\theight %d%%\n\tdim %d\n\tanimation %s\n", e->name, app, e->position, e->width,
			e->height, e->dim, e->slide ? "slide" : "none");
		g_string_append(out, "}\n");
		g_free(app);
	}
	char *path = conf_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	GError *error = NULL;
	bool ok = g_file_set_contents(path, out->str, -1, &error);
	if (!ok) {
		problem_text(p->s, "Could not save %s: %s", path, error->message);
		g_error_free(error);
	}
	g_free(path);
	g_string_free(out, TRUE);
	return ok;
}

/* A name for popups.conf and the command: the app's, in small letters and dashes. */
static char *name_for(struct popups_page *p, const char *app) {
	char *base = g_strdup(app);
	if (g_str_has_suffix(base, ".desktop")) {
		base[strlen(base) - 8] = '\0';
	}
	const char *dot = strrchr(base, '.');
	GString *name = g_string_new(NULL);
	for (const char *c = dot && dot[1] ? dot + 1 : base; *c; c++) {
		g_string_append_c(name, g_ascii_isalnum(*c) ? g_ascii_tolower(*c) : '-');
	}
	if (!name->len) {
		g_string_append(name, "popup");
	}
	g_free(base);
	char *unique = g_strdup(name->str);
	for (int n = 2;; n++) {
		bool used = false;
		for (guint i = 0; i < p->entries->len && !used; i++) {
			used = strcmp(((struct popup_entry *)p->entries->pdata[i])->name, unique) == 0;
		}
		if (!used) {
			break;
		}
		g_free(unique);
		unique = g_strdup_printf("%s-%d", name->str, n);
	}
	g_string_free(name, TRUE);
	return unique;
}

static int index_of(const char *const *list, const char *value) {
	for (int i = 0; list[i]; i++) {
		if (value && strcmp(list[i], value) == 0) {
			return i;
		}
	}
	return 0;
}

/* ---------- the dialog ---------- */

struct popup_dialog {
	struct popups_page *p;
	struct popup_entry *editing; // NULL for a new one
	char *app, *keys;
	GtkWidget *window, *app_button, *position, *width, *height, *dim, *motion, *keys_button,
		*warning, *preview, *save;
};

static void dialog_free(gpointer data) {
	struct popup_dialog *d = data;
	g_free(d->app);
	g_free(d->keys);
	g_free(d);
}

/* A small screen with the popup on it, where it goes and how big, the rest darkened. */
static void draw_preview(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	struct popup_dialog *d = data;
	double sw = width - 8, sh = sw * 9 / 16;
	if (sh > height - 8) {
		sh = height - 8;
		sw = sh * 16 / 9;
	}
	double x0 = (width - sw) / 2, y0 = (height - sh) / 2, bar = sh * 0.07;
	// the screen: a wallpaper of two blues, a window on it, the taskbar
	cairo_pattern_t *bg = cairo_pattern_create_linear(x0, y0, x0 + sw, y0 + sh);
	cairo_pattern_add_color_stop_rgb(bg, 0, 0.16, 0.36, 0.62);
	cairo_pattern_add_color_stop_rgb(bg, 1, 0.09, 0.20, 0.40);
	cairo_rectangle(cr, x0, y0, sw, sh);
	cairo_set_source(cr, bg);
	cairo_fill(cr);
	cairo_pattern_destroy(bg);
	cairo_rectangle(cr, x0 + sw * 0.08, y0 + sh * 0.12, sw * 0.4, sh * 0.45);
	cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
	cairo_fill(cr);
	cairo_rectangle(cr, x0, y0 + sh - bar, sw, bar);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
	cairo_fill(cr);
	int dim = dims[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->dim))];
	if (dim > 0) {
		cairo_rectangle(cr, x0, y0, sw, sh);
		cairo_set_source_rgba(cr, 0, 0, 0, dim / 100.0);
		cairo_fill(cr);
	}
	// the popup, in the work area above the taskbar
	double aw = sw, ah = sh - bar;
	double w = aw * gtk_range_get_value(GTK_RANGE(d->width)) / 100;
	double h = ah * gtk_range_get_value(GTK_RANGE(d->height)) / 100;
	const char *pos = positions[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->position))];
	double x = x0 + (aw - w) / 2, y = y0 + (ah - h) / 2;
	if (strcmp(pos, "left") == 0) {
		x = x0;
	} else if (strcmp(pos, "right") == 0) {
		x = x0 + aw - w;
	} else if (strcmp(pos, "top") == 0) {
		y = y0;
	} else if (strcmp(pos, "bottom") == 0) {
		y = y0 + ah - h;
	}
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.25, 0.55, 0.95);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);
	cairo_rectangle(cr, x, y, w, fmin(h, sh * 0.06));
	cairo_set_source_rgb(cr, 0.25, 0.55, 0.95);
	cairo_fill(cr);
	const struct tw_desktop_entry *app = d->app ? ui_find_app(d->app) : NULL;
	if (app && app->name && w > 40 && h > 30) {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *font = pango_font_description_from_string("Sans 9");
		pango_layout_set_font_description(layout, font);
		pango_layout_set_text(layout, app->name, -1);
		pango_layout_set_width(layout, (int)((w - 8) * PANGO_SCALE));
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		int lh;
		pango_layout_get_pixel_size(layout, NULL, &lh);
		cairo_move_to(cr, x + 4, y + (h - lh) / 2);
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(font);
		g_object_unref(layout);
	}
}

static void dialog_update(struct popup_dialog *d) {
	const struct tw_desktop_entry *app = d->app ? ui_find_app(d->app) : NULL;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	if (app) {
		gtk_box_append(GTK_BOX(box), ui_app_icon(app->icon, 20));
	}
	gtk_box_append(GTK_BOX(box), gtk_label_new(app ? app->name : d->app ? d->app :
		"Choose an app"));
	gtk_menu_button_set_child(GTK_MENU_BUTTON(d->app_button), box);
	char *shown = d->keys && *d->keys ? keyboard_display_keys(d->keys) :
		g_strdup("Click to record");
	gtk_button_set_label(GTK_BUTTON(d->keys_button), shown);
	g_free(shown);
	char *other = d->keys && *d->keys ? keys_taken(d->p->s, d->keys,
		d->editing ? d->editing->name : NULL) : NULL;
	if (other) {
		char *text = g_strdup_printf("These keys did \"%s\" until now; the popup takes them "
			"over.", other);
		gtk_label_set_text(GTK_LABEL(d->warning), text);
		g_free(text);
	}
	gtk_widget_set_visible(d->warning, other != NULL);
	g_free(other);
	gtk_widget_set_sensitive(d->save, d->app != NULL);
	gtk_widget_queue_draw(d->preview);
}

static void on_app_picked(const char *id, gpointer data) {
	struct popup_dialog *d = data;
	g_free(d->app);
	d->app = g_strdup(id);
	dialog_update(d);
}

static void on_keys_recorded(const char *keys, gpointer data) {
	struct popup_dialog *d = data;
	g_free(d->keys);
	d->keys = g_strdup(keys);
	dialog_update(d);
}

static void on_record(GtkButton *button, gpointer data) {
	struct popup_dialog *d = data;
	keyboard_record_keys(GTK_WINDOW(d->window), on_keys_recorded, d);
}

static void on_clear_keys(GtkButton *button, gpointer data) {
	struct popup_dialog *d = data;
	g_free(d->keys);
	d->keys = NULL;
	dialog_update(d);
}

static void on_changed(GObject *object, GParamSpec *pspec, gpointer data) {
	dialog_update(data);
}

static void on_range_changed(GtkRange *range, gpointer data) {
	dialog_update(data);
}

static void rebuild(struct popups_page *p);

static void on_save(GtkButton *button, gpointer data) {
	struct popup_dialog *d = data;
	struct popups_page *p = d->p;
	if (!d->app) {
		return;
	}
	struct popup_entry *e = d->editing;
	if (!e) {
		e = g_new0(struct popup_entry, 1);
		e->name = name_for(p, d->app);
		g_ptr_array_add(p->entries, e);
	}
	g_free(e->app);
	e->app = g_strdup(d->app);
	g_free(e->position);
	e->position = g_strdup(positions[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->position))]);
	e->width = (int)gtk_range_get_value(GTK_RANGE(d->width));
	e->height = (int)gtk_range_get_value(GTK_RANGE(d->height));
	e->dim = dims[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->dim))];
	e->slide = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->motion)) == 0;
	g_free(e->keys);
	e->keys = d->keys && *d->keys ? g_strdup(d->keys) : NULL;
	if (save(p)) {
		set_binding(p->s, e->name, e->keys);
		settings_status(p->s, d->editing ? "Changed the popup" : "Added the popup");
	}
	rebuild(p);
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static void on_cancel(GtkButton *button, gpointer data) {
	struct popup_dialog *d = data;
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static GtkWidget *labelled(GtkWidget *grid, int row, const char *title, GtkWidget *control) {
	GtkWidget *label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
	gtk_widget_set_hexpand(control, TRUE);
	gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
	return control;
}

static GtkWidget *percent_scale(int value) {
	GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 100, 5);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
	gtk_range_set_value(GTK_RANGE(scale), value);
	for (int v = 25; v <= 100; v += 25) {
		gtk_scale_add_mark(GTK_SCALE(scale), v, GTK_POS_BOTTOM, NULL);
	}
	return scale;
}

static char *format_percent(GtkScale *scale, double value, gpointer data) {
	return g_strdup_printf("%.0f%%", value);
}

static void open_dialog(struct popups_page *p, struct popup_entry *editing) {
	struct popup_dialog *d = g_new0(struct popup_dialog, 1);
	d->p = p;
	d->editing = editing;
	d->app = editing ? g_strdup(editing->app) : NULL;
	d->keys = editing && editing->keys ? g_strdup(editing->keys) : NULL;

	d->window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(d->window), p->s->window);
	gtk_window_set_modal(GTK_WINDOW(d->window), TRUE);
	gtk_window_set_title(GTK_WINDOW(d->window), editing ? "Change popup" : "Add popup");
	gtk_window_set_default_size(GTK_WINDOW(d->window), 560, -1);
	g_object_set_data_full(G_OBJECT(d->window), "dialog", d, dialog_free);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(box, 20);
	gtk_widget_set_margin_end(box, 20);
	gtk_widget_set_margin_top(box, 20);
	gtk_widget_set_margin_bottom(box, 20);

	d->preview = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(d->preview), 150);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->preview), draw_preview, d, NULL);
	gtk_box_append(GTK_BOX(box), d->preview);

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	d->app_button = labelled(grid, 0, "App", ui_app_picker("Choose an app", on_app_picked, d));
	d->position = labelled(grid, 1, "Position",
		gtk_drop_down_new_from_strings(position_titles));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d->position),
		index_of(positions, editing ? editing->position : "right"));
	d->width = labelled(grid, 2, "Width", percent_scale(editing ? editing->width : 70));
	d->height = labelled(grid, 3, "Height", percent_scale(editing ? editing->height : 100));
	gtk_scale_set_format_value_func(GTK_SCALE(d->width), format_percent, NULL, NULL);
	gtk_scale_set_format_value_func(GTK_SCALE(d->height), format_percent, NULL, NULL);
	d->dim = labelled(grid, 4, "Darken the rest", gtk_drop_down_new_from_strings(dim_titles));
	int dim = editing ? editing->dim : 50;
	guint dim_index = 0;
	for (guint i = 0; i < G_N_ELEMENTS(dims); i++) {
		if (abs(dims[i] - dim) < abs(dims[dim_index] - dim)) {
			dim_index = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d->dim), dim_index);

	GtkWidget *keys_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	d->keys_button = gtk_button_new_with_label("Click to record");
	gtk_widget_set_hexpand(d->keys_button, TRUE);
	g_signal_connect(d->keys_button, "clicked", G_CALLBACK(on_record), d);
	gtk_box_append(GTK_BOX(keys_box), d->keys_button);
	GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
	gtk_widget_set_tooltip_text(clear, "No shortcut");
	g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_keys), d);
	gtk_box_append(GTK_BOX(keys_box), clear);
	static const char *const motions[] = { "Slides in from its side", "Just appears", NULL };
	d->motion = labelled(grid, 5, "Comes in", gtk_drop_down_new_from_strings(motions));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d->motion), editing && !editing->slide ? 1 : 0);
	labelled(grid, 6, "Shortcut", keys_box);
	gtk_box_append(GTK_BOX(box), grid);

	d->warning = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(d->warning), TRUE);
	gtk_label_set_xalign(GTK_LABEL(d->warning), 0);
	gtk_widget_add_css_class(d->warning, "warning");
	gtk_widget_set_visible(d->warning, FALSE);
	gtk_box_append(GTK_BOX(box), d->warning);

	GtkWidget *hint = gtk_label_new("The shortcut brings the app (and starts it if needed); "
		"the shortcut again or a click beside it hides it, still running.");
	gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(box), hint);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	GtkWidget *cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), d);
	d->save = gtk_button_new_with_label("Save");
	gtk_widget_add_css_class(d->save, "suggested-action");
	g_signal_connect(d->save, "clicked", G_CALLBACK(on_save), d);
	gtk_box_append(GTK_BOX(buttons), cancel);
	gtk_box_append(GTK_BOX(buttons), d->save);
	gtk_box_append(GTK_BOX(box), buttons);

	g_signal_connect(d->position, "notify::selected", G_CALLBACK(on_changed), d);
	g_signal_connect(d->dim, "notify::selected", G_CALLBACK(on_changed), d);
	g_signal_connect(d->width, "value-changed", G_CALLBACK(on_range_changed), d);
	g_signal_connect(d->height, "value-changed", G_CALLBACK(on_range_changed), d);
	gtk_window_set_child(GTK_WINDOW(d->window), box);
	dialog_update(d);
	gtk_window_present(GTK_WINDOW(d->window));
}

/* ---------- the list ---------- */

static struct popup_entry *entry_named(struct popups_page *p, const char *name) {
	for (guint i = 0; i < p->entries->len; i++) {
		struct popup_entry *e = p->entries->pdata[i];
		if (strcmp(e->name, name) == 0) {
			return e;
		}
	}
	return NULL;
}

static void on_try(GtkButton *button, gpointer data) {
	if (current) {
		settings_command(current->s, "popup toggle %s", (const char *)data);
	}
}

static void on_edit(GtkButton *button, gpointer data) {
	struct popup_entry *e = current ? entry_named(current, data) : NULL;
	if (e) {
		open_dialog(current, e);
	}
}

static void on_remove(GtkButton *button, gpointer data) {
	struct popups_page *p = current;
	struct popup_entry *e = p ? entry_named(p, data) : NULL;
	if (!e) {
		return;
	}
	set_binding(p->s, e->name, NULL);
	g_ptr_array_remove(p->entries, e);
	save(p);
	settings_status(p->s, "Removed the popup");
	rebuild(p);
}

static void on_add(GtkButton *button, gpointer data) {
	open_dialog(data, NULL);
}

static void rebuild(struct popups_page *p) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->list), child);
	}
	if (p->entries->len == 0) {
		ui_empty_row(p->list, "window-new-symbolic", "No popups yet",
			"Add one: an app, where it shows and how big, and the keys that bring it.");
		return;
	}
	for (guint i = 0; i < p->entries->len; i++) {
		struct popup_entry *e = p->entries->pdata[i];
		const struct tw_desktop_entry *app = ui_find_app(e->app);
		char *keys = e->keys ? keyboard_display_keys(e->keys) : g_strdup("no shortcut");
		const char *where = position_titles[index_of(positions, e->position)];
		char *subtitle = g_strdup_printf("%s · %d%% wide, %d%% high · %s", where, e->width,
			e->height, keys);
		GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_box_append(GTK_BOX(controls), ui_icon_button("media-playback-start-symbolic",
			"Show it now", true, G_CALLBACK(on_try), g_strdup(e->name)));
		gtk_box_append(GTK_BOX(controls), ui_icon_button("document-edit-symbolic",
			"Change", true, G_CALLBACK(on_edit), g_strdup(e->name)));
		gtk_box_append(GTK_BOX(controls), ui_icon_button("user-trash-symbolic",
			"Remove the popup", true, G_CALLBACK(on_remove), g_strdup(e->name)));
		GtkWidget *row = ui_row(p->list, app ? app->name : e->app, subtitle, controls);
		GtkWidget *row_box = ui_row_box(row);
		if (row_box && app) {
			gtk_box_prepend(GTK_BOX(row_box), ui_app_icon(app->icon, 32));
		}
		g_free(subtitle);
		g_free(keys);
	}
}

GtkWidget *popups_page_new(struct settings *s) {
	struct popups_page *p = g_new0(struct popups_page, 1);
	p->s = s;
	p->entries = g_ptr_array_new_with_free_func(entry_free);
	current = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("App popups",
		"Apps that come and go with a shortcut, at a place and size of their own, over the "
		"rest of the screen darkened: mail on the right, a terminal from the top.", &content);
	p->list = ui_group(content, "Popups", NULL);
	GtkWidget *add = gtk_button_new_with_label("Add popup");
	gtk_widget_set_halign(add, GTK_ALIGN_START);
	g_signal_connect(add, "clicked", G_CALLBACK(on_add), p);
	gtk_box_append(GTK_BOX(content), add);
	load(p);
	rebuild(p);
	return page;
}
