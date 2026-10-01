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

static const double widths[] = { 0.25, 0.34, 0.5, 0.66, 0.75 };
static const char *const width_labels[] = { "A quarter", "A third", "Half", "Two thirds",
	"Three quarters", NULL };
static const double heights[] = { 0.34, 0.5, 0.66 };
static const char *const height_labels[] = { "A third", "Half", "Two thirds", NULL };

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
	GtkWidget *list, *kind_dd, *width_dd, *height_dd, *height_row, *add;
	GPtrArray *layouts; // struct layout *
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

static void on_add(GtkButton *button, gpointer data) {
	if (sl.layouts->len >= MAX_LAYOUTS) {
		return;
	}
	guint k = gtk_drop_down_get_selected(GTK_DROP_DOWN(sl.kind_dd));
	guint w = gtk_drop_down_get_selected(GTK_DROP_DOWN(sl.width_dd));
	guint h = gtk_drop_down_get_selected(GTK_DROP_DOWN(sl.height_dd));
	if (k >= G_N_ELEMENTS(kinds) || w >= G_N_ELEMENTS(widths) || h >= G_N_ELEMENTS(heights)) {
		return;
	}
	add_layout(kinds[k].kind, widths[w], kinds[k].rows ? heights[h] : 0.5);
	save();
	rebuild();
}

static void on_kind(GObject *dd, GParamSpec *pspec, gpointer data) {
	guint k = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	gtk_widget_set_sensitive(sl.height_dd, k < G_N_ELEMENTS(kinds) && kinds[k].rows);
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
		gtk_box_append(GTK_BOX(box), icon_button("go-up-symbolic", "Earlier", i,
			G_CALLBACK(on_move), GINT_TO_POINTER(-1), i > 0));
		gtk_box_append(GTK_BOX(box), icon_button("go-down-symbolic", "Later", i,
			G_CALLBACK(on_move), GINT_TO_POINTER(1), i < n - 1));
		gtk_box_append(GTK_BOX(box), icon_button("list-remove-symbolic", "Remove", i,
			G_CALLBACK(on_remove), NULL, n > 1));
	}
	gtk_widget_set_sensitive(sl.add, n < MAX_LAYOUTS);
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
	GtkStringList *kind_model = gtk_string_list_new(NULL);
	for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++) {
		gtk_string_list_append(kind_model, kinds[i].label);
	}
	sl.kind_dd = gtk_drop_down_new(G_LIST_MODEL(kind_model), NULL);
	g_signal_connect(sl.kind_dd, "notify::selected", G_CALLBACK(on_kind), NULL);
	ui_row(add, "New layout", NULL, sl.kind_dd);
	sl.width_dd = gtk_drop_down_new_from_strings(width_labels);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(sl.width_dd), 3);
	ui_row(add, "Width of the left part", NULL, sl.width_dd);
	sl.height_dd = gtk_drop_down_new_from_strings(height_labels);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(sl.height_dd), 1);
	ui_row(add, "Height of the upper parts", NULL, sl.height_dd);
	on_kind(G_OBJECT(sl.kind_dd), NULL, NULL);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	sl.add = gtk_button_new_with_label("Add layout");
	gtk_widget_add_css_class(sl.add, "suggested-action");
	g_signal_connect(sl.add, "clicked", G_CALLBACK(on_add), NULL);
	GtkWidget *defaults_button = gtk_button_new_with_label("Back to the default layouts");
	g_signal_connect(defaults_button, "clicked", G_CALLBACK(on_defaults), NULL);
	gtk_box_append(GTK_BOX(buttons), defaults_button);
	gtk_box_append(GTK_BOX(buttons), sl.add);
	GtkWidget *row = ui_row(add, NULL, NULL, NULL);
	gtk_widget_set_hexpand(buttons, TRUE);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(ui_row_box(row)), buttons);

	load();
	rebuild();
}
