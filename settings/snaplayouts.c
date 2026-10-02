#include <math.h>
#include <string.h>
#include "settings.h"

/*
 * The Snap layouts a window's maximize button offers, as a list to arrange:
 * each with a picture of its parts, moved up and down or removed, and new
 * ones put together from dropdowns. Written to taskbar.conf as
 *
 *   snap_layouts {
 *       layout columns 0.66
 *       layout left_quarters 0.5 0.5
 *   }
 *
 * which the taskbar reads when it shows them. Without the block the taskbar
 * shows the layouts of Windows 11, and so does this list.
 */

struct layout {
	char *kind;
	double fx, fy;
};

static const struct {
	const char *kind, *label;
	bool rows; // has a line across too
} kinds[] = {
	{ "columns", "Two side by side", false },
	{ "quarters", "Four quarters", true },
	{ "left_quarters", "One on the left, two on the right", true },
	{ "quarters_right", "Two on the left, one on the right", true },
};

// where a dragged line likes to stop: quarters, thirds and the middle
static const double stops[] = { 0.25, 0.34, 0.5, 0.66, 0.75 };

// the layouts the taskbar shows without a list of its own
static const struct {
	const char *kind;
	double fx, fy;
} defaults[] = {
	{ "columns", 0.5, 0.5 }, { "columns", 0.66, 0.5 }, { "columns", 0.34, 0.5 },
	{ "quarters", 0.5, 0.5 }, { "left_quarters", 0.5, 0.5 },
};

#define MAX_LAYOUTS 8

static struct {
	struct settings *s;
	GtkWidget *list, *editor, *kind_buttons[4], *add, *save, *editing_label;
	GPtrArray *layouts; // struct layout *
	struct layout edit; // the layout in the editor
	int editing;        // the index of the layout it came from, -1 for a new one
	int dragging;       // 0 none, 1 the line down, 2 the line across
	double drag_x, drag_y;
} sl;

static void layout_free(gpointer data) {
	struct layout *l = data;
	g_free(l->kind);
	g_free(l);
}

static int kind_index(const char *kind) {
	for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++) {
		if (strcmp(kinds[i].kind, kind) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static void add_layout(const char *kind, double fx, double fy) {
	struct layout *l = g_new0(struct layout, 1);
	l->kind = g_strdup(kind);
	l->fx = fx;
	l->fy = fy;
	g_ptr_array_add(sl.layouts, l);
}

/* The layouts of taskbar.conf, or the default ones. */
static void load(void) {
	g_ptr_array_set_size(sl.layouts, 0);
	struct cstmt *block = confdoc_block(sl.s->taskbar, "snap_layouts", NULL, false);
	for (guint i = 0; block && block->children && i < block->children->len; i++) {
		struct cstmt *c = block->children->pdata[i];
		if (strcmp(c->name, "layout") != 0 || cstmt_argc(c) < 2 ||
				kind_index(cstmt_arg(c, 0)) < 0) {
			continue;
		}
		double fx = g_ascii_strtod(cstmt_arg(c, 1), NULL);
		double fy = cstmt_argc(c) > 2 ? g_ascii_strtod(cstmt_arg(c, 2), NULL) : 0.5;
		if (fx >= 0.1 && fx <= 0.9 && fy >= 0.1 && fy <= 0.9) {
			add_layout(cstmt_arg(c, 0), fx, fy);
		}
	}
	if (sl.layouts->len == 0) {
		for (size_t i = 0; i < G_N_ELEMENTS(defaults); i++) {
			add_layout(defaults[i].kind, defaults[i].fx, defaults[i].fy);
		}
	}
}

/* Writes the list; the default layouts are written as no list at all. */
static void save(void) {
	bool is_default = sl.layouts->len == G_N_ELEMENTS(defaults);
	for (guint i = 0; is_default && i < sl.layouts->len; i++) {
		struct layout *l = sl.layouts->pdata[i];
		is_default = strcmp(l->kind, defaults[i].kind) == 0 &&
			fabs(l->fx - defaults[i].fx) < 0.005 && fabs(l->fy - defaults[i].fy) < 0.005;
	}
	struct confdoc *doc = sl.s->taskbar;
	struct cstmt *block = confdoc_block(doc, "snap_layouts", NULL, !is_default);
	if (is_default) {
		if (block) {
			confdoc_remove(doc, block);
		}
	} else {
		GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
		for (guint i = 0; i < sl.layouts->len; i++) {
			struct layout *l = sl.layouts->pdata[i];
			char fx[G_ASCII_DTOSTR_BUF_SIZE], fy[G_ASCII_DTOSTR_BUF_SIZE];
			g_ascii_formatd(fx, sizeof(fx), "%.2f", l->fx);
			g_ascii_formatd(fy, sizeof(fy), "%.2f", l->fy);
			g_ptr_array_add(lines, kinds[kind_index(l->kind)].rows ?
				g_strdup_printf("%s %s %s", l->kind, fx, fy) :
				g_strdup_printf("%s %s", l->kind, fx));
		}
		confdoc_set_list(doc, block, "layout", NULL, lines);
		g_ptr_array_unref(lines);
	}
	settings_taskbar_changed(sl.s);
}

/* ---------- the pictures ---------- */

static void part(cairo_t *cr, double x, double y, double w, double h, GdkRGBA *fg) {
	cairo_rectangle(cr, x + 1.5, y + 1.5, w - 3, h - 3);
	cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.18);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.7);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void draw_layout(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
	const struct layout *l = data;
	GdkRGBA fg;
	gtk_widget_get_color(GTK_WIDGET(area), &fg);
	double lx = w * l->fx, ly = h * l->fy;
	if (strcmp(l->kind, "columns") == 0) {
		part(cr, 0, 0, lx, h, &fg);
		part(cr, lx, 0, w - lx, h, &fg);
	} else if (strcmp(l->kind, "quarters") == 0) {
		part(cr, 0, 0, lx, ly, &fg);
		part(cr, lx, 0, w - lx, ly, &fg);
		part(cr, 0, ly, lx, h - ly, &fg);
		part(cr, lx, ly, w - lx, h - ly, &fg);
	} else if (strcmp(l->kind, "left_quarters") == 0) {
		part(cr, 0, 0, lx, h, &fg);
		part(cr, lx, 0, w - lx, ly, &fg);
		part(cr, lx, ly, w - lx, h - ly, &fg);
	} else {
		part(cr, 0, 0, lx, ly, &fg);
		part(cr, 0, ly, lx, h - ly, &fg);
		part(cr, lx, 0, w - lx, h, &fg);
	}
}

static GtkWidget *picture(struct layout *l) {
	GtkWidget *area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 64);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 40);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_layout, l, NULL);
	gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
	return area;
}

/* "Two side by side, two thirds and a third" */
static char *describe(const struct layout *l) {
	int k = kind_index(l->kind);
	char *size = NULL;
	int pct = (int)lround(l->fx * 100);
	if (!kinds[k].rows) {
		size = pct == 50 ? g_strdup("half and half") :
			g_strdup_printf("%d%% and %d%% wide", pct, 100 - pct);
	} else {
		int pcy = (int)lround(l->fy * 100);
		size = g_strdup_printf("%d%% / %d%% wide, %d%% / %d%% high", pct, 100 - pct, pcy,
			100 - pcy);
	}
	char *text = g_strdup_printf("%s, %s", kinds[k].label, size);
	g_free(size);
	return text;
}

/* ---------- the list ---------- */

static void rebuild(void);

static void on_move(GtkButton *button, gpointer data) {
	int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"));
	int to = index + GPOINTER_TO_INT(data);
	if (to < 0 || to >= (int)sl.layouts->len) {
		return;
	}
	gpointer tmp = sl.layouts->pdata[index];
	sl.layouts->pdata[index] = sl.layouts->pdata[to];
	sl.layouts->pdata[to] = tmp;
	save();
	rebuild();
}

static void on_remove(GtkButton *button, gpointer data) {
	int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"));
	if (sl.layouts->len <= 1 || index >= (int)sl.layouts->len) {
		return; // one layout at least: none at all would mean the default ones
	}
	g_ptr_array_remove_index(sl.layouts, index);
	save();
	rebuild();
}

static void on_defaults(GtkButton *button, gpointer data) {
	g_ptr_array_set_size(sl.layouts, 0);
	for (size_t i = 0; i < G_N_ELEMENTS(defaults); i++) {
		add_layout(defaults[i].kind, defaults[i].fx, defaults[i].fy);
	}
	save();
	rebuild();
}

static void editor_sync(void);

/* ---------- the editor ---------- */

#define EDITOR_W 384
#define EDITOR_H 216

/* The line across, if the kind has one, spans these x (in 0..1). */
static bool across_span(const struct layout *l, double *from, double *to) {
	if (strcmp(l->kind, "quarters") == 0) {
		*from = 0;
		*to = 1;
	} else if (strcmp(l->kind, "left_quarters") == 0) {
		*from = l->fx;
		*to = 1;
	} else if (strcmp(l->kind, "quarters_right") == 0) {
		*from = 0;
		*to = l->fx;
	} else {
		return false;
	}
	return true;
}

/* The size of a part, written in its middle: "66%", or "50% × 34%". */
static void label_part(cairo_t *cr, double x, double y, double w, double h, int pct_w,
		int pct_h, GdkRGBA *fg) {
	char text[32];
	if (pct_h > 0) {
		snprintf(text, sizeof(text), "%d%% × %d%%", pct_w, pct_h);
	} else {
		snprintf(text, sizeof(text), "%d%%", pct_w);
	}
	cairo_text_extents_t ext;
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 13);
	cairo_text_extents(cr, text, &ext);
	cairo_move_to(cr, x + (w - ext.width) / 2 - ext.x_bearing,
		y + (h - ext.height) / 2 - ext.y_bearing);
	cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.75);
	cairo_show_text(cr, text);
}

static void draw_editor(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
	GdkRGBA fg;
	gtk_widget_get_color(GTK_WIDGET(area), &fg);
	const struct layout *l = &sl.edit;
	// the screen, its parts with their sizes, and the lines to drag
	cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.06);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.5);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	draw_layout(area, cr, w, h, (gpointer)l);
	double lx = w * l->fx, ly = h * l->fy;
	int px = (int)lround(l->fx * 100), py = (int)lround(l->fy * 100);
	if (strcmp(l->kind, "columns") == 0) {
		label_part(cr, 0, 0, lx, h, px, 0, &fg);
		label_part(cr, lx, 0, w - lx, h, 100 - px, 0, &fg);
	} else if (strcmp(l->kind, "quarters") == 0) {
		label_part(cr, 0, 0, lx, ly, px, py, &fg);
		label_part(cr, lx, 0, w - lx, ly, 100 - px, py, &fg);
		label_part(cr, 0, ly, lx, h - ly, px, 100 - py, &fg);
		label_part(cr, lx, ly, w - lx, h - ly, 100 - px, 100 - py, &fg);
	} else if (strcmp(l->kind, "left_quarters") == 0) {
		label_part(cr, 0, 0, lx, h, px, 0, &fg);
		label_part(cr, lx, 0, w - lx, ly, 100 - px, py, &fg);
		label_part(cr, lx, ly, w - lx, h - ly, 100 - px, 100 - py, &fg);
	} else {
		label_part(cr, 0, 0, lx, ly, px, py, &fg);
		label_part(cr, 0, ly, lx, h - ly, px, 100 - py, &fg);
		label_part(cr, lx, 0, w - lx, h, 100 - px, 0, &fg);
	}
	// the handles, thicker while one is dragged
	cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 1);
	cairo_set_line_width(cr, sl.dragging == 1 ? 5 : 3);
	cairo_move_to(cr, lx, 0);
	cairo_line_to(cr, lx, h);
	cairo_stroke(cr);
	double from, to;
	if (across_span(l, &from, &to)) {
		cairo_set_line_width(cr, sl.dragging == 2 ? 5 : 3);
		cairo_move_to(cr, w * from, ly);
		cairo_line_to(cr, w * to, ly);
		cairo_stroke(cr);
	}
}

/* Which line is under the pointer: 1 the line down, 2 the line across, 0 none. */
static int line_at(double x, double y) {
	int w = gtk_widget_get_width(sl.editor), h = gtk_widget_get_height(sl.editor);
	double from, to;
	if (across_span(&sl.edit, &from, &to) && fabs(y - h * sl.edit.fy) < 10 &&
			x >= w * from - 4 && x <= w * to + 4) {
		return 2;
	}
	return fabs(x - w * sl.edit.fx) < 10 ? 1 : 0;
}

/* Where a dropped line stays: a stop close by, or else a step of 5%. */
static double snap_fraction(double f) {
	for (size_t i = 0; i < G_N_ELEMENTS(stops); i++) {
		if (fabs(f - stops[i]) < 0.03) {
			return stops[i];
		}
	}
	f = round(f * 20) / 20;
	return f < 0.15 ? 0.15 : f > 0.85 ? 0.85 : f;
}

static void on_editor_motion(GtkEventControllerMotion *m, double x, double y, gpointer data) {
	if (sl.dragging) {
		return;
	}
	int line = line_at(x, y);
	gtk_widget_set_cursor_from_name(sl.editor, line == 1 ? "col-resize" :
		line == 2 ? "row-resize" : NULL);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	sl.dragging = line_at(x, y);
	sl.drag_x = x;
	sl.drag_y = y;
	if (!sl.dragging) {
		gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
	}
	gtk_widget_queue_draw(sl.editor);
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	int w = gtk_widget_get_width(sl.editor), h = gtk_widget_get_height(sl.editor);
	if (sl.dragging == 1) {
		sl.edit.fx = snap_fraction((sl.drag_x + dx) / w);
	} else if (sl.dragging == 2) {
		sl.edit.fy = snap_fraction((sl.drag_y + dy) / h);
	}
	gtk_widget_queue_draw(sl.editor);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	sl.dragging = 0;
	gtk_widget_queue_draw(sl.editor);
}

static void on_kind_button(GtkToggleButton *button, gpointer data) {
	if (!gtk_toggle_button_get_active(button)) {
		return;
	}
	int k = GPOINTER_TO_INT(data);
	if (strcmp(sl.edit.kind, kinds[k].kind) != 0) {
		g_free(sl.edit.kind);
		sl.edit.kind = g_strdup(kinds[k].kind);
	}
	gtk_widget_queue_draw(sl.editor);
}

static void on_add(GtkButton *button, gpointer data) {
	if (sl.layouts->len >= MAX_LAYOUTS) {
		return;
	}
	add_layout(sl.edit.kind, sl.edit.fx, kinds[kind_index(sl.edit.kind)].rows ? sl.edit.fy : 0.5);
	sl.editing = -1;
	save();
	rebuild();
}

/* Puts the layout being changed back in its place in the list. */
static void on_save(GtkButton *button, gpointer data) {
	if (sl.editing < 0 || sl.editing >= (int)sl.layouts->len) {
		return;
	}
	struct layout *l = sl.layouts->pdata[sl.editing];
	g_free(l->kind);
	l->kind = g_strdup(sl.edit.kind);
	l->fx = sl.edit.fx;
	l->fy = kinds[kind_index(sl.edit.kind)].rows ? sl.edit.fy : 0.5;
	sl.editing = -1;
	save();
	rebuild();
}

static void on_edit(GtkButton *button, gpointer data) {
	int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"));
	if (index >= (int)sl.layouts->len) {
		return;
	}
	struct layout *l = sl.layouts->pdata[index];
	g_free(sl.edit.kind);
	sl.edit.kind = g_strdup(l->kind);
	sl.edit.fx = l->fx;
	sl.edit.fy = l->fy;
	sl.editing = index;
	editor_sync();
}

/* The kind buttons, the buttons and the hint after the editor's layout changed. */
static void editor_sync(void) {
	int k = kind_index(sl.edit.kind);
	for (int i = 0; i < (int)G_N_ELEMENTS(kinds); i++) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sl.kind_buttons[i]), i == k);
	}
	gtk_widget_set_visible(sl.save, sl.editing >= 0);
	gtk_widget_set_sensitive(sl.add, sl.layouts->len < MAX_LAYOUTS);
	char *text = sl.editing >= 0 ?
		g_strdup_printf("Changing layout %d of the list: drag its lines, then save", sl.editing + 1) :
		g_strdup("Drag the lines into place; they stop at quarters, thirds and the middle");
	gtk_label_set_text(GTK_LABEL(sl.editing_label), text);
	g_free(text);
	gtk_widget_queue_draw(sl.editor);
}

static GtkWidget *icon_button(const char *icon, const char *tip, int index, GCallback cb,
		gpointer data, bool enabled) {
	GtkWidget *b = gtk_button_new_from_icon_name(icon);
	gtk_widget_add_css_class(b, "flat");
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive(b, enabled);
	g_object_set_data(G_OBJECT(b), "index", GINT_TO_POINTER(index));
	g_signal_connect(b, "clicked", cb, data);
	return b;
}

static void rebuild(void) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(sl.list))) {
		gtk_list_box_remove(GTK_LIST_BOX(sl.list), child);
	}
	int n = (int)sl.layouts->len;
	for (int i = 0; i < n; i++) {
		struct layout *l = sl.layouts->pdata[i];
		char *text = describe(l);
		GtkWidget *row = ui_row(sl.list, text, NULL, NULL);
		g_free(text);
		GtkWidget *box = ui_row_box(row);
		gtk_box_prepend(GTK_BOX(box), picture(l));
		gtk_box_append(GTK_BOX(box), icon_button("document-edit-symbolic", "Change it", i,
			G_CALLBACK(on_edit), NULL, true));
		gtk_box_append(GTK_BOX(box), icon_button("go-up-symbolic", "Earlier", i,
			G_CALLBACK(on_move), GINT_TO_POINTER(-1), i > 0));
		gtk_box_append(GTK_BOX(box), icon_button("go-down-symbolic", "Later", i,
			G_CALLBACK(on_move), GINT_TO_POINTER(1), i < n - 1));
		gtk_box_append(GTK_BOX(box), icon_button("list-remove-symbolic", "Remove", i,
			G_CALLBACK(on_remove), NULL, n > 1));
	}
	if (sl.editing >= n) {
		sl.editing = -1;
	}
	editor_sync();
}

void snaplayouts_section_refresh(struct settings *s) {
	if (!sl.list || sl.s != s) {
		return;
	}
	load();
	rebuild();
}

void snaplayouts_section_attach(struct settings *s, GtkWidget *content) {
	sl.s = s;
	sl.layouts = g_ptr_array_new_with_free_func(layout_free);
	sl.list = ui_group(content, "Snap layouts",
		"The layouts on a window's maximize button, in this order, up to eight.");

	GtkWidget *add = ui_group(content, NULL, NULL);
	sl.edit.kind = g_strdup("columns");
	sl.edit.fx = 0.66;
	sl.edit.fy = 0.5;
	sl.editing = -1;

	// the kinds, as pictures to pick from
	GtkWidget *kinds_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	static struct layout samples[G_N_ELEMENTS(kinds)];
	for (int i = 0; i < (int)G_N_ELEMENTS(kinds); i++) {
		samples[i] = (struct layout){ (char *)kinds[i].kind, 0.5, 0.5 };
		GtkWidget *b = gtk_toggle_button_new();
		gtk_button_set_child(GTK_BUTTON(b), picture(&samples[i]));
		gtk_widget_set_tooltip_text(b, kinds[i].label);
		if (i > 0) {
			gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b),
				GTK_TOGGLE_BUTTON(sl.kind_buttons[0]));
		}
		g_signal_connect(b, "toggled", G_CALLBACK(on_kind_button), GINT_TO_POINTER(i));
		sl.kind_buttons[i] = b;
		gtk_box_append(GTK_BOX(kinds_box), b);
	}
	ui_row(add, "Kind of layout", NULL, kinds_box);

	// the layout itself, its lines dragged into place
	sl.editor = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(sl.editor), EDITOR_W);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(sl.editor), EDITOR_H);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sl.editor), draw_editor, NULL, NULL);
	gtk_widget_set_halign(sl.editor, GTK_ALIGN_CENTER);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
	gtk_widget_add_controller(sl.editor, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_editor_motion), NULL);
	gtk_widget_add_controller(sl.editor, motion);
	// a row of its own, so the picture stays in the middle whatever the hint says
	GtkWidget *editor_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top(editor_box, 10);
	gtk_widget_set_margin_bottom(editor_box, 8);
	gtk_box_append(GTK_BOX(editor_box), sl.editor);
	sl.editing_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(sl.editing_label, "dim-label");
	gtk_box_append(GTK_BOX(editor_box), sl.editing_label);
	GtkWidget *editor_row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(editor_row), editor_box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(editor_row), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(add), editor_row);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *defaults_button = gtk_button_new_with_label("Back to the default layouts");
	g_signal_connect(defaults_button, "clicked", G_CALLBACK(on_defaults), NULL);
	sl.save = gtk_button_new_with_label("Save changes");
	g_signal_connect(sl.save, "clicked", G_CALLBACK(on_save), NULL);
	sl.add = gtk_button_new_with_label("Add layout");
	gtk_widget_add_css_class(sl.add, "suggested-action");
	g_signal_connect(sl.add, "clicked", G_CALLBACK(on_add), NULL);
	gtk_box_append(GTK_BOX(buttons), defaults_button);
	gtk_box_append(GTK_BOX(buttons), sl.save);
	gtk_box_append(GTK_BOX(buttons), sl.add);
	GtkWidget *row = ui_row(add, NULL, NULL, NULL);
	gtk_widget_set_hexpand(buttons, TRUE);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(ui_row_box(row)), buttons);

	load();
	rebuild();
}
