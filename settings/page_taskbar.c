#include <stdlib.h>
#include <string.h>
#include "reorder.h"
#include "settings.h"
#include "tw_desktop.h"
#include "tw_widgets.h"

enum {
	SECTION_LEFT,
	SECTION_CENTER,
	SECTION_RIGHT,
	SECTION_COUNT,
};

static const char *const section_keys[] = { "left", "center", "right" };
static const char *const section_titles[] = { "Left", "Center", "Right" };

struct taskbar_page {
	struct settings *s;
	bool updating;
	GtkWidget *layout_dd, *position_dd, *height_spin;
	GtkWidget *sections[SECTION_COUNT];
	GtkWidget *scripts;
	GtkWidget *custom_entry, *custom_popover;
	GtkWidget *font_entry, *terminal_entry, *delay_spin;
	struct app_list *quick;
	GPtrArray *root_settings;
	GHashTable *quick_icons;
	guint rebuild_id;
	char *open_dialog; // widget whose settings open after the next rebuild
};

static struct confdoc *doc(struct taskbar_page *p) {
	return p->s->taskbar;
}

static const char *layout_name(struct taskbar_page *p) {
	return gtk_drop_down_get_selected(GTK_DROP_DOWN(p->layout_dd)) == 1 ? "tile" : "window";
}

static const char *other_layout_name(struct taskbar_page *p) {
	return strcmp(layout_name(p), "tile") == 0 ? "window" : "tile";
}

static bool array_has(GPtrArray *array, const char *str) {
	for (guint i = 0; array && i < array->len; i++) {
		if (strcmp(array->pdata[i], str) == 0) {
			return true;
		}
	}
	return false;
}

/* The layout the panel uses: a missing layout mirrors the other one. */
static struct cstmt *layout_block(struct taskbar_page *p) {
	struct cstmt *block = confdoc_block(doc(p), "layout", layout_name(p), false);
	return block ? block : confdoc_block(doc(p), "layout", other_layout_name(p), false);
}

static struct cstmt *ensure_layout(struct taskbar_page *p) {
	struct cstmt *block = confdoc_block(doc(p), "layout", layout_name(p), false);
	if (block) {
		return block;
	}
	struct cstmt *other = confdoc_block(doc(p), "layout", other_layout_name(p), false);
	GString *text = g_string_new(NULL);
	g_string_append_printf(text, "layout %s {\n\tposition %s\n", layout_name(p),
		strcmp(layout_name(p), "tile") == 0 ? "top" : "bottom");
	for (guint i = 0; other && i < other->children->len; i++) {
		struct cstmt *c = other->children->pdata[i];
		if (strcmp(c->name, "left") == 0 || strcmp(c->name, "center") == 0 ||
				strcmp(c->name, "right") == 0 || strcmp(c->name, "height") == 0) {
			char *raw = cstmt_raw_args(doc(p), c);
			g_string_append_printf(text, "\t%s %s\n", c->name, raw);
			g_free(raw);
		}
	}
	g_string_append(text, "}");
	confdoc_append(doc(p), doc(p)->root, text->str);
	g_string_free(text, TRUE);
	return confdoc_block(doc(p), "layout", layout_name(p), false);
}

static GPtrArray *read_names(struct cstmt *statement) {
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; i < cstmt_argc(statement); i++) {
		g_ptr_array_add(names, g_strdup(cstmt_arg(statement, i)));
	}
	return names;
}

static GPtrArray *read_section(struct taskbar_page *p, int section) {
	return read_names(confdoc_child(layout_block(p), section_keys[section], NULL));
}

static void write_names(struct confdoc *d, struct cstmt *parent, const char *key,
		GPtrArray *names) {
	if (names->len == 0) {
		confdoc_set(d, parent, key, NULL, NULL);
		return;
	}
	GString *args = g_string_new(NULL);
	for (guint i = 0; i < names->len; i++) {
		char *quoted = conf_quote(names->pdata[i]);
		if (i > 0) {
			g_string_append_c(args, ' ');
		}
		g_string_append(args, quoted);
		g_free(quoted);
	}
	confdoc_set(d, parent, key, NULL, args->str);
	g_string_free(args, TRUE);
}

static void write_section(struct taskbar_page *p, int section, GPtrArray *names) {
	struct cstmt *layout = ensure_layout(p);
	if (layout) {
		write_names(doc(p), layout, section_keys[section], names);
		if (strcmp(layout_name(p), "window") == 0) {
			// writing read the file again and built new statements, so the
			// block from before has been freed and has to be looked up anew
			layout = confdoc_block(doc(p), "layout", layout_name(p), false);
			// the taskbar adds the notifications button to older configs unless told not to
			bool shown = false;
			for (int s = 0; layout && s < SECTION_COUNT && !shown; s++) {
				GPtrArray *all = read_names(confdoc_child(layout, section_keys[s], NULL));
				shown = array_has(all, "notifications");
				g_ptr_array_unref(all);
			}
			confdoc_set(doc(p), doc(p)->root, "notifications_button", NULL, shown ? NULL : "no");
		}
		settings_taskbar_changed(p->s);
	}
}

static void rebuild_all(struct taskbar_page *p);

static gboolean rebuild_idle(gpointer data) {
	struct taskbar_page *p = data;
	p->rebuild_id = 0;
	rebuild_all(p);
	return G_SOURCE_REMOVE;
}

static void schedule_rebuild(struct taskbar_page *p) {
	if (!p->rebuild_id) {
		p->rebuild_id = g_idle_add(rebuild_idle, p);
	}
}

static void delete_custom(struct taskbar_page *p, const char *widget) {
	char *name = g_strdup(widget);
	struct confdoc *d = doc(p);
	struct cstmt *block = confdoc_block(d, "widget", name, false);
	if (block) {
		confdoc_remove(d, block);
	}
	struct cstmt *entry = widget_desktop_entry(p->s, name);
	if (entry) {
		confdoc_remove(d, entry);
		desktop_page_refresh(p->s);
	}
	static const char *const layouts[] = { "window", "tile" };
	for (size_t l = 0; l < G_N_ELEMENTS(layouts); l++) {
		for (int s = 0; s < SECTION_COUNT; s++) {
			struct cstmt *layout = confdoc_block(d, "layout", layouts[l], false);
			GPtrArray *names = read_names(confdoc_child(layout, section_keys[s], NULL));
			if (array_has(names, name)) {
				for (guint i = names->len; i > 0; i--) {
					if (strcmp(names->pdata[i - 1], name) == 0) {
						g_ptr_array_remove_index(names, i - 1);
					}
				}
				write_names(d, layout, section_keys[s], names);
			}
			g_ptr_array_unref(names);
		}
	}
	settings_taskbar_changed(p->s);
	settings_status(p->s, "Deleted %s", name);
	g_free(name);
	schedule_rebuild(p);
}

struct name_action {
	struct taskbar_page *p;
	char *name;
};

static struct name_action *name_action_new(struct taskbar_page *p, const char *name) {
	struct name_action *a = g_new0(struct name_action, 1);
	a->p = p;
	a->name = g_strdup(name);
	return a;
}

static void name_action_free(gpointer data, GClosure *closure) {
	struct name_action *a = data;
	g_free(a->name);
	g_free(a);
}

void taskbar_delete_script(struct settings *s, const char *name) {
	if (s->taskbar_page) {
		delete_custom(s->taskbar_page, name);
	}
}

static void open_widget_dialog(struct taskbar_page *p, const char *name) {
	widget_dialog_open(p->s, name, false);
}

static void on_open_dialog(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	open_widget_dialog(a->p, a->name);
}

static void on_delete_script(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	delete_custom(a->p, a->name);
}

static GPtrArray *known_widgets(struct taskbar_page *p) {
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	static const char *const layouts[] = { "window", "tile" };
	for (size_t l = 0; l < G_N_ELEMENTS(layouts); l++) {
		struct cstmt *layout = confdoc_block(doc(p), "layout", layouts[l], false);
		for (int s = 0; s < SECTION_COUNT; s++) {
			struct cstmt *st = confdoc_child(layout, section_keys[s], NULL);
			for (int i = 0; i < cstmt_argc(st); i++) {
				if (!array_has(names, cstmt_arg(st, i))) {
					g_ptr_array_add(names, g_strdup(cstmt_arg(st, i)));
				}
			}
		}
	}
	struct cstmt *root = doc(p)->root;
	for (guint i = 0; i < root->children->len; i++) {
		struct cstmt *c = root->children->pdata[i];
		const char *name = cstmt_arg(c, 0);
		if (strcmp(c->name, "widget") == 0 && name && !array_has(names, name)) {
			g_ptr_array_add(names, g_strdup(name));
		}
	}
	for (size_t i = 0; i < tw_widget_count; i++) {
		const struct tw_widget_info *info = &tw_widgets[i];
		if ((info->flags & TW_WIDGET_TASKBAR) && !(info->flags & TW_WIDGET_UNLISTED) &&
				!array_has(names, info->type)) {
			g_ptr_array_add(names, g_strdup(info->type));
		}
	}
	return names;
}

static void on_create_custom(GtkWidget *widget, gpointer data) {
	struct taskbar_page *p = data;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(p->custom_entry));
	GString *clean = g_string_new(NULL);
	for (const char *c = text; *c; c++) {
		bool ok = g_ascii_isalnum(*c) || *c == '_' || *c == '-';
		g_string_append_c(clean, ok ? g_ascii_tolower(*c) : '-');
	}
	if (clean->len == 0) {
		g_string_free(clean, TRUE);
		return;
	}
	char *name = g_strdup_printf("custom:%s", clean->str);
	g_string_free(clean, TRUE);
	if (confdoc_block(doc(p), "widget", name, false)) {
		settings_status(p->s, "A widget called %s already exists", name);
	} else {
		char *block = g_strdup_printf("widget %s {\n\texec \"echo Hello\"\n\tinterval 60\n}", name);
		confdoc_append(doc(p), doc(p)->root, block);
		g_free(block);
		settings_taskbar_changed(p->s);
		settings_status(p->s, "Created %s. Add it to a section of the layout to show it.", name);
	}
	gtk_popover_popdown(GTK_POPOVER(p->custom_popover));
	gtk_editable_set_text(GTK_EDITABLE(p->custom_entry), "");
	g_free(p->open_dialog);
	p->open_dialog = name;
	schedule_rebuild(p);
}

/* ---------- layout sections ---------- */

enum {
	OP_LEFT,
	OP_RIGHT,
	OP_REMOVE,
	OP_CONFIGURE,
};

struct widget_action {
	struct taskbar_page *p;
	int section;
	guint index;
	int op;
};

static void on_widget_action(GtkButton *button, gpointer data) {
	struct widget_action *a = data;
	struct taskbar_page *p = a->p;
	GPtrArray *names = read_section(p, a->section);
	guint i = a->index;
	if (i >= names->len) {
		g_ptr_array_unref(names);
		return;
	}
	char *name = g_strdup(names->pdata[i]);
	switch (a->op) {
	case OP_REMOVE:
		g_ptr_array_remove_index(names, i);
		write_section(p, a->section, names);
		break;
	case OP_LEFT:
	case OP_RIGHT:;
		int target = a->section + (a->op == OP_LEFT ? -1 : 1);
		g_ptr_array_remove_index(names, i);
		write_section(p, a->section, names);
		GPtrArray *other = read_section(p, target);
		if (a->op == OP_LEFT) {
			g_ptr_array_add(other, g_strdup(name));
		} else {
			g_ptr_array_insert(other, 0, g_strdup(name));
		}
		write_section(p, target, other);
		g_ptr_array_unref(other);
		break;
	case OP_CONFIGURE:
		open_widget_dialog(p, name);
		break;
	}
	g_free(name);
	g_ptr_array_unref(names);
	if (a->op != OP_CONFIGURE) {
		schedule_rebuild(p);
	}
}

static void add_action_button(struct taskbar_page *p, GtkWidget *box, const char *icon,
		const char *tooltip, bool sensitive, int section, guint index, int op) {
	struct widget_action *a = g_new0(struct widget_action, 1);
	a->p = p;
	a->section = section;
	a->index = index;
	a->op = op;
	gtk_box_append(GTK_BOX(box), ui_icon_button(icon, tooltip, sensitive,
		G_CALLBACK(on_widget_action), a));
}

struct add_request {
	struct taskbar_page *p;
	int section;
	char *name;
};

static void add_request_free(gpointer data, GClosure *closure) {
	struct add_request *r = data;
	g_free(r->name);
	g_free(r);
}

static void on_add_widget(GtkButton *button, gpointer data) {
	struct add_request *r = data;
	GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	if (popover) {
		gtk_popover_popdown(GTK_POPOVER(popover));
	}
	GPtrArray *names = read_section(r->p, r->section);
	g_ptr_array_add(names, g_strdup(r->name));
	write_section(r->p, r->section, names);
	g_ptr_array_unref(names);
	schedule_rebuild(r->p);
}

static GtkWidget *add_widget_button(struct taskbar_page *p, int section) {
	GPtrArray *used = g_ptr_array_new_with_free_func(g_free);
	for (int s = 0; s < SECTION_COUNT; s++) {
		GPtrArray *names = read_section(p, s);
		for (guint i = 0; i < names->len; i++) {
			g_ptr_array_add(used, g_strdup(names->pdata[i]));
		}
		g_ptr_array_unref(names);
	}
	GPtrArray *candidates = known_widgets(p);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (guint i = 0; i < candidates->len; i++) {
		const char *name = candidates->pdata[i];
		char *unique = NULL;
		if (array_has(used, name)) {
			// separators and spacers can appear several times under distinct names
			if (strcmp(name, "separator") != 0 && strcmp(name, "spacer") != 0) {
				continue;
			}
			for (int n = 2; !unique; n++) {
				char *candidate = g_strdup_printf("%s:%d", name, n);
				if (array_has(used, candidate)) {
					g_free(candidate);
				} else {
					unique = candidate;
				}
			}
		}
		const char *description;
		char *title = widget_title(name, &description);
		GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *label = gtk_label_new(title);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(labels), label);
		if (description) {
			GtkWidget *desc = gtk_label_new(description);
			gtk_label_set_xalign(GTK_LABEL(desc), 0);
			gtk_widget_add_css_class(desc, "dim-label");
			gtk_widget_add_css_class(desc, "tw-caption");
			gtk_box_append(GTK_BOX(labels), desc);
		}
		GtkWidget *button = gtk_button_new();
		gtk_widget_add_css_class(button, "flat");
		gtk_button_set_child(GTK_BUTTON(button), labels);
		struct add_request *r = g_new0(struct add_request, 1);
		r->p = p;
		r->section = section;
		r->name = unique ? unique : g_strdup(name);
		g_signal_connect_data(button, "clicked", G_CALLBACK(on_add_widget), r,
			add_request_free, 0);
		gtk_box_append(GTK_BOX(box), button);
		g_free(title);
	}
	g_ptr_array_unref(candidates);
	g_ptr_array_unref(used);

	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), scroll);
	GtkWidget *menu = gtk_menu_button_new();
	gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), "Add widget…");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
	return menu;
}

/* ---------- moving widgets by their handle ---------- */

/*
 * What a dragged row carries: its section, its place in it and its name, e.g.
 * "2:4:clock". The name makes sure a drop never moves another widget than the
 * one that was picked up.
 */
static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y,
		gpointer data) {
	GtkWidget *row = gtk_widget_get_ancestor(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), GTK_TYPE_LIST_BOX_ROW);
	const char *name = row ? g_object_get_data(G_OBJECT(row), "widget") : NULL;
	if (!name) {
		return NULL;
	}
	char *text = g_strdup_printf("%d:%d:%s",
		GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "section")),
		gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row)), name);
	GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_STRING, text);
	g_free(text);
	return content;
}

static void on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer data) {
	GtkWidget *row = gtk_widget_get_ancestor(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), GTK_TYPE_LIST_BOX_ROW);
	if (row) {
		// the whole row follows the pointer, not only the handle; a still picture
		// of it, so the row fading while it is away does not fade the picture too
		GdkPaintable *live = gtk_widget_paintable_new(row);
		GdkPaintable *picture = gdk_paintable_get_current_image(live);
		gtk_drag_source_set_icon(source, picture, 0, 0);
		g_object_unref(picture);
		g_object_unref(live);
		gtk_widget_add_css_class(row, "tw-dragged");
	}
}

static void on_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data,
		gpointer data) {
	GtkWidget *row = gtk_widget_get_ancestor(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), GTK_TYPE_LIST_BOX_ROW);
	if (row) {
		gtk_widget_remove_css_class(row, "tw-dragged");
	}
}

/*
 * The grip of the handle: two columns of three dots in the text color. It is
 * drawn rather than taken from the icon theme, because list-drag-handle-symbolic
 * only comes with Adwaita and the icon themes of tileWin inherit from breeze.
 */
static void draw_grip(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	GdkRGBA color;
	gtk_widget_get_color(GTK_WIDGET(area), &color);
	gdk_cairo_set_source_rgba(cr, &color);
	double r = 1.5, gap = 5;
	double x = width / 2.0 - gap / 2, y = height / 2.0 - gap;
	for (int column = 0; column < 2; column++) {
		for (int row = 0; row < 3; row++) {
			cairo_new_sub_path(cr);
			cairo_arc(cr, x + column * gap, y + row * gap, r, 0, 2 * G_PI);
		}
	}
	cairo_fill(cr);
}

static GtkWidget *drag_handle(void) {
	GtkWidget *handle = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(handle), 16);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(handle), 16);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(handle), draw_grip, NULL, NULL);
	gtk_widget_set_tooltip_text(handle, "Drag to move the widget");
	gtk_widget_set_cursor_from_name(handle, "grab");
	gtk_widget_set_valign(handle, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(handle, "dim-label");
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), NULL);
	g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
	g_signal_connect(source, "drag-end", G_CALLBACK(on_drag_end), NULL);
	gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(source));
	return handle;
}

/*
 * Where in a section a drop at y lands: in front of the widget row under the
 * pointer, or behind it on its lower half. Rows without a widget, like the
 * Add widget row, stand for the end. Also returns the row that marks the
 * spot, and whether the mark goes below it.
 */
static guint drop_position(GtkListBox *list, double y, guint count, GtkWidget **mark,
		bool *below) {
	GtkListBoxRow *row = gtk_list_box_get_row_at_y(list, (int)y);
	*mark = NULL;
	*below = false;
	if (row && g_object_get_data(G_OBJECT(row), "widget")) {
		graphene_rect_t bounds;
		guint index = gtk_list_box_row_get_index(row);
		*mark = GTK_WIDGET(row);
		if (gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(list), &bounds) &&
				y > bounds.origin.y + bounds.size.height / 2) {
			*below = true;
			return index + 1;
		}
		return index;
	}
	// past the widgets: the mark goes below the last one
	if (count > 0) {
		*mark = GTK_WIDGET(gtk_list_box_get_row_at_index(list, count - 1));
		*below = true;
	}
	return count;
}

static void clear_drop_mark(GtkListBox *list) {
	for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(list)); row;
			row = gtk_widget_get_next_sibling(row)) {
		gtk_widget_remove_css_class(row, "tw-drop-above");
		gtk_widget_remove_css_class(row, "tw-drop-below");
	}
}

static guint section_count(GtkListBox *list) {
	guint count = 0;
	GtkListBoxRow *row;
	while ((row = gtk_list_box_get_row_at_index(list, count)) &&
			g_object_get_data(G_OBJECT(row), "widget")) {
		count++;
	}
	return count;
}

static GdkDragAction on_drop_motion(GtkDropTarget *target, double x, double y,
		gpointer data) {
	GtkListBox *list = GTK_LIST_BOX(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
	GtkWidget *mark;
	bool below;
	drop_position(list, y, section_count(list), &mark, &below);
	clear_drop_mark(list);
	if (mark) {
		gtk_widget_add_css_class(mark, below ? "tw-drop-below" : "tw-drop-above");
	}
	return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *target, gpointer data) {
	clear_drop_mark(GTK_LIST_BOX(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target))));
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y,
		gpointer data) {
	struct taskbar_page *p = data;
	GtkListBox *list = GTK_LIST_BOX(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
	clear_drop_mark(list);
	const char *text = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : NULL;
	int from_section, from_index, consumed = 0;
	if (!text || sscanf(text, "%d:%d:%n", &from_section, &from_index, &consumed) != 2 ||
			consumed == 0 || from_section < 0 || from_section >= SECTION_COUNT ||
			from_index < 0) {
		return FALSE;
	}
	const char *name = text + consumed;
	int to_section = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(list), "section"));

	GPtrArray *from = read_section(p, from_section);
	if ((guint)from_index >= from->len || strcmp(from->pdata[from_index], name) != 0) {
		g_ptr_array_unref(from); // the layout changed under the drag
		return FALSE;
	}
	GPtrArray *to = to_section == from_section ? g_ptr_array_ref(from) :
		read_section(p, to_section);
	GtkWidget *mark;
	bool below;
	guint before = drop_position(list, y, to->len, &mark, &below);
	if (reorder_move(from, from_index, to, before)) {
		write_section(p, from_section, from);
		if (to != from) {
			write_section(p, to_section, to);
		}
		schedule_rebuild(p);
	}
	g_ptr_array_unref(to);
	g_ptr_array_unref(from);
	return TRUE;
}

static void accept_drops(struct taskbar_page *p, GtkWidget *list, int section) {
	g_object_set_data(G_OBJECT(list), "section", GINT_TO_POINTER(section));
	GtkDropTarget *target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect(target, "motion", G_CALLBACK(on_drop_motion), p);
	g_signal_connect(target, "enter", G_CALLBACK(on_drop_motion), p);
	g_signal_connect(target, "leave", G_CALLBACK(on_drop_leave), p);
	g_signal_connect(target, "drop", G_CALLBACK(on_drop), p);
	gtk_widget_add_controller(list, GTK_EVENT_CONTROLLER(target));
}

static void rebuild_sections(struct taskbar_page *p) {
	for (int s = 0; s < SECTION_COUNT; s++) {
		GtkWidget *list = p->sections[s];
		gtk_list_box_remove_all(GTK_LIST_BOX(list));
		GPtrArray *names = read_section(p, s);
		for (guint i = 0; i < names->len; i++) {
			const char *name = names->pdata[i];
			char *title = widget_title(name, NULL);
			GtkWidget *row = ui_row(list, title, name, NULL);
			gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
			gtk_widget_set_tooltip_text(row, "Click to change the settings of this widget");
			g_object_set_data_full(G_OBJECT(row), "widget", g_strdup(name), g_free);
			g_object_set_data(G_OBJECT(row), "section", GINT_TO_POINTER(s));
			GtkWidget *box = ui_row_box(row);
			gtk_box_prepend(GTK_BOX(box), drag_handle());
			add_action_button(p, box, "go-previous-symbolic", "Move to the previous section",
				s > 0, s, i, OP_LEFT);
			add_action_button(p, box, "go-next-symbolic", "Move to the next section",
				s + 1 < SECTION_COUNT, s, i, OP_RIGHT);
			add_action_button(p, box, "emblem-system-symbolic", "Widget settings", true, s, i,
				OP_CONFIGURE);
			add_action_button(p, box, "list-remove-symbolic", "Remove", true, s, i, OP_REMOVE);
			g_free(title);
		}
		ui_row(list, NULL, names->len ? NULL : "Empty", add_widget_button(p, s));
		g_ptr_array_unref(names);
	}
}

static void rebuild_scripts(struct taskbar_page *p) {
	gtk_list_box_remove_all(GTK_LIST_BOX(p->scripts));
	struct cstmt *root = doc(p)->root;
	int count = 0;
	for (guint i = 0; i < root->children->len; i++) {
		struct cstmt *c = root->children->pdata[i];
		const char *name = cstmt_arg(c, 0);
		if (strcmp(c->name, "widget") != 0 || !name || !g_str_has_prefix(name, "custom:")) {
			continue;
		}
		struct cstmt *exec = confdoc_child(c, "exec", NULL);
		if (!exec) {
			exec = confdoc_child(c, "exec_listen", NULL);
		}
		char *command = cstmt_join(exec, 0);
		char *title = widget_title(name, NULL);
		GtkWidget *row = ui_row(p->scripts, title, command ? command : "No command yet", NULL);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
		g_object_set_data_full(G_OBJECT(row), "widget", g_strdup(name), g_free);
		GtkWidget *box = ui_row_box(row);
		GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
		gtk_widget_set_tooltip_text(settings, "Settings");
		gtk_widget_add_css_class(settings, "flat");
		gtk_widget_set_valign(settings, GTK_ALIGN_CENTER);
		g_signal_connect_data(settings, "clicked", G_CALLBACK(on_open_dialog),
			name_action_new(p, name), name_action_free, 0);
		gtk_box_append(GTK_BOX(box), settings);
		GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_set_tooltip_text(remove, "Delete");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		g_signal_connect_data(remove, "clicked", G_CALLBACK(on_delete_script),
			name_action_new(p, name), name_action_free, 0);
		gtk_box_append(GTK_BOX(box), remove);
		g_free(title);
		g_free(command);
		count++;
	}
	if (count == 0) {
		ui_row(p->scripts, NULL, "No script widgets yet.", NULL);
	}
}

static void refresh_layout_controls(struct taskbar_page *p) {
	struct cstmt *layout = layout_block(p);
	const char *position = cstmt_arg(confdoc_child(layout, "position", NULL), 0);
	bool top = position ? g_ascii_strcasecmp(position, "top") == 0 :
		strcmp(layout_name(p), "tile") == 0;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->position_dd), top);
	const char *height = cstmt_arg(confdoc_child(layout, "height", NULL), 0);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->height_spin), height ? atoi(height) : 0);
}

static void set_entry_text(GtkWidget *entry, const char *text) {
	if (strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)), text ? text : "") != 0) {
		gtk_editable_set_text(GTK_EDITABLE(entry), text ? text : "");
	}
}

static void refresh_general(struct taskbar_page *p) {
	struct cstmt *root = doc(p)->root;
	char *font = cstmt_join(confdoc_child(root, "font", NULL), 0);
	char *terminal = cstmt_join(confdoc_child(root, "terminal", NULL), 0);
	const char *delay = cstmt_arg(confdoc_child(root, "tooltip_delay", NULL), 0);
	set_entry_text(p->font_entry, font);
	set_entry_text(p->terminal_entry, terminal);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->delay_spin), delay ? atoi(delay) : 600);
	g_free(font);
	g_free(terminal);

	g_hash_table_remove_all(p->quick_icons);
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	struct cstmt *quick = confdoc_block(doc(p), "widget", "quicklaunch", false);
	for (guint i = 0; quick && i < quick->children->len; i++) {
		struct cstmt *item = quick->children->pdata[i];
		if (strcmp(item->name, "item") != 0 || cstmt_argc(item) < 1) {
			continue;
		}
		g_ptr_array_add(ids, g_strdup(cstmt_arg(item, 0)));
		if (cstmt_argc(item) > 1) {
			g_hash_table_insert(p->quick_icons, g_strdup(cstmt_arg(item, 0)),
				g_strdup(cstmt_arg(item, 1)));
		}
	}
	ui_app_list_set(p->quick, ids);
	g_ptr_array_unref(ids);
}

static void rebuild_all(struct taskbar_page *p) {
	p->updating = true;
	refresh_layout_controls(p);
	ui_taskbar_keys_refresh(p->root_settings);
	refresh_general(p);
	p->updating = false;
	rebuild_sections(p);
	rebuild_scripts(p);
	if (p->open_dialog) {
		char *name = p->open_dialog;
		p->open_dialog = NULL;
		open_widget_dialog(p, name);
		g_free(name);
	}
}

void taskbar_page_refresh(struct settings *s) {
	if (s->taskbar_page) {
		rebuild_all(s->taskbar_page);
	}
}

/* ---------- signal handlers for the static controls ---------- */

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
	const char *name = g_object_get_data(G_OBJECT(row), "widget");
	if (name) {
		open_widget_dialog(data, name);
	}
}

static void on_layout_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	schedule_rebuild(data);
}

static void on_position_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct taskbar_page *p = data;
	if (p->updating) {
		return;
	}
	struct cstmt *layout = ensure_layout(p);
	bool top = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) == 1;
	confdoc_set(doc(p), layout, "position", NULL, top ? "top" : "bottom");
	settings_taskbar_changed(p->s);
}

static void on_height_changed(GtkSpinButton *spin, gpointer data) {
	struct taskbar_page *p = data;
	if (p->updating) {
		return;
	}
	int height = gtk_spin_button_get_value_as_int(spin);
	struct cstmt *layout = ensure_layout(p);
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", height);
	confdoc_set(doc(p), layout, "height", NULL, height > 0 ? buf : NULL);
	settings_taskbar_changed(p->s);
}

static void on_root_text(GtkEditable *editable, gpointer data) {
	struct taskbar_page *p = data;
	if (p->updating) {
		return;
	}
	const char *key = g_object_get_data(G_OBJECT(editable), "key");
	const char *text = gtk_editable_get_text(editable);
	char *quoted = *text ? conf_quote(text) : NULL;
	confdoc_set(doc(p), doc(p)->root, key, NULL, quoted);
	g_free(quoted);
	settings_taskbar_changed(p->s);
}

static void on_delay_changed(GtkSpinButton *spin, gpointer data) {
	struct taskbar_page *p = data;
	if (p->updating) {
		return;
	}
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", gtk_spin_button_get_value_as_int(spin));
	confdoc_set(doc(p), doc(p)->root, "tooltip_delay", NULL, buf);
	settings_taskbar_changed(p->s);
}

static void on_quick_changed(struct app_list *list, gpointer data) {
	struct taskbar_page *p = data;
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < list->ids->len; i++) {
		const char *id = list->ids->pdata[i];
		const char *icon = g_hash_table_lookup(p->quick_icons, id);
		char *qid = conf_quote(id);
		char *qicon = icon ? conf_quote(icon) : NULL;
		g_ptr_array_add(lines, qicon ? g_strdup_printf("%s %s", qid, qicon) : g_strdup(qid));
		g_free(qid);
		g_free(qicon);
	}
	struct cstmt *block = confdoc_block(doc(p), "widget", "quicklaunch", true);
	confdoc_set_list(doc(p), block, "item", NULL, lines);
	g_ptr_array_unref(lines);
	settings_taskbar_changed(p->s);
}

/* ---------- the icon of a quick launch app ---------- */

struct icon_request {
	struct taskbar_page *p;
	char *id;
};

static void icon_request_free(struct icon_request *r) {
	g_free(r->id);
	g_free(r);
}

static void on_icon_chosen(const char *icon, gpointer data) {
	struct icon_request *r = data;
	if (icon) {
		g_hash_table_insert(r->p->quick_icons, g_strdup(r->id), g_strdup(icon));
	} else {
		g_hash_table_remove(r->p->quick_icons, r->id);
	}
	on_quick_changed(r->p->quick, r->p);
	ui_app_list_refresh(r->p->quick);
	settings_status(r->p->s, icon ? "Icon changed" : "Using the app's own icon");
	icon_request_free(r);
}

static const char *quick_row_icon(struct app_list *l, guint index, gpointer data) {
	struct taskbar_page *p = data;
	return index < l->ids->len ? g_hash_table_lookup(p->quick_icons, l->ids->pdata[index]) : NULL;
}

static void on_quick_icon(struct app_list *l, guint index, gpointer data) {
	struct taskbar_page *p = data;
	if (index >= l->ids->len) {
		return;
	}
	struct icon_request *r = g_new0(struct icon_request, 1);
	r->p = p;
	r->id = g_strdup(l->ids->pdata[index]);
	const struct tw_desktop_entry *e = ui_find_app(r->id);
	ui_icon_dialog(p->s->window, e ? e->name : r->id,
		"The icon shown in the taskbar: an icon name of your icon theme, e.g. firefox, "
		"or an image file.", g_hash_table_lookup(p->quick_icons, r->id),
		e ? e->icon : NULL, "Use the app's icon", on_icon_chosen, r);
}

static GtkWidget *root_entry(struct taskbar_page *p, const char *key, const char *placeholder) {
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
	gtk_widget_set_size_request(entry, 300, -1);
	g_object_set_data(G_OBJECT(entry), "key", (gpointer)key);
	g_signal_connect(entry, "changed", G_CALLBACK(on_root_text), p);
	return entry;
}

GtkWidget *taskbar_page_new(struct settings *s) {
	struct taskbar_page *p = g_new0(struct taskbar_page, 1);
	p->s = s;
	p->root_settings = g_ptr_array_new();
	p->quick_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GtkWidget *content;
	GtkWidget *page = ui_page("Taskbar",
		"Changes are saved to taskbar.conf and the taskbar reloads them right away.", &content);

	GtkWidget *general = ui_group(content, "General", NULL);
	p->font_entry = root_entry(p, "font", "Theme font, e.g. Noto Sans 10");
	ui_row(general, "Font", NULL, p->font_entry);
	p->terminal_entry = root_entry(p, "terminal", "xfce4-terminal -x");
	ui_row(general, "Terminal for console apps", "Runs apps whose desktop entry asks for a terminal",
		p->terminal_entry);
	p->delay_spin = gtk_spin_button_new_with_range(0, 5000, 100);
	g_signal_connect(p->delay_spin, "value-changed", G_CALLBACK(on_delay_changed), p);
	ui_row(general, "Tooltip delay", "Milliseconds", p->delay_spin);

	GtkWidget *layout = ui_group(content, "Layout",
		"tileWin keeps a separate taskbar layout for window mode and for tile mode.");
	p->layout_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Window mode", "Tile mode", NULL });
	char *mode = settings_current_mode();
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->layout_dd), strcmp(mode, "tile") == 0);
	g_free(mode);
	g_signal_connect(p->layout_dd, "notify::selected", G_CALLBACK(on_layout_selected), p);
	ui_row(layout, "Edit the layout of", NULL, p->layout_dd);
	p->position_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Bottom", "Top", NULL });
	g_signal_connect(p->position_dd, "notify::selected", G_CALLBACK(on_position_selected), p);
	ui_row(layout, "Position", NULL, p->position_dd);
	p->height_spin = gtk_spin_button_new_with_range(0, 200, 1);
	g_signal_connect(p->height_spin, "value-changed", G_CALLBACK(on_height_changed), p);
	ui_row(layout, "Height", "Pixels; 0 uses the theme's height", p->height_spin);
	ui_taskbar_key(s, p->root_settings, layout, "theme_layout", "Let the theme bring its own layout",
		"Off keeps the sections below whichever theme is picked", true, 0, 0, 1);

	GtkWidget *clipboard = ui_group(content, "Clipboard", NULL);
	ui_taskbar_key(s, p->root_settings, clipboard, "clipboard_history", "Remember what was copied",
		"Win+V shows the history; passwords marked as secret are never kept", true, 0, 0, 1);
	ui_taskbar_key(s, p->root_settings, clipboard, "clipboard_paste", "Paste the entry that is picked",
		"Off only copies it back to the clipboard", true, 0, 0, 1);

	for (int i = 0; i < SECTION_COUNT; i++) {
		char *title = g_strdup_printf("%s section", section_titles[i]);
		p->sections[i] = ui_group(content, title, i == SECTION_LEFT ?
			"The Windows 11 theme centers the left section. Click a widget to change its settings; "
			"drag it by its handle to move it, also into another section." :
			NULL);
		g_signal_connect(p->sections[i], "row-activated", G_CALLBACK(on_row_activated), p);
		accept_drops(p, p->sections[i], i);
		g_free(title);
	}

	GtkWidget *scripts_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *scripts_title = gtk_label_new("Script widgets");
	gtk_widget_add_css_class(scripts_title, "tw-heading");
	gtk_widget_set_hexpand(scripts_title, TRUE);
	gtk_label_set_xalign(GTK_LABEL(scripts_title), 0);
	gtk_box_append(GTK_BOX(scripts_header), scripts_title);

	p->custom_popover = gtk_popover_new();
	GtkWidget *custom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->custom_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->custom_entry), "Name, e.g. weather");
	g_signal_connect(p->custom_entry, "activate", G_CALLBACK(on_create_custom), p);
	gtk_box_append(GTK_BOX(custom_box), p->custom_entry);
	GtkWidget *create = gtk_button_new_with_label("Create");
	gtk_widget_add_css_class(create, "suggested-action");
	g_signal_connect(create, "clicked", G_CALLBACK(on_create_custom), p);
	gtk_box_append(GTK_BOX(custom_box), create);
	gtk_popover_set_child(GTK_POPOVER(p->custom_popover), custom_box);
	GtkWidget *custom_button = gtk_menu_button_new();
	gtk_menu_button_set_label(GTK_MENU_BUTTON(custom_button), "New script widget…");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(custom_button), p->custom_popover);
	gtk_box_append(GTK_BOX(scripts_header), custom_button);
	gtk_widget_set_margin_top(scripts_header, 14);
	gtk_box_append(GTK_BOX(content), scripts_header);
	GtkWidget *scripts_hint = gtk_label_new(
		"Script widgets show the output of a command. Add them to a section above with Add widget.");
	gtk_label_set_xalign(GTK_LABEL(scripts_hint), 0);
	gtk_label_set_wrap(GTK_LABEL(scripts_hint), TRUE);
	gtk_widget_add_css_class(scripts_hint, "dim-label");
	gtk_box_append(GTK_BOX(content), scripts_hint);
	p->scripts = ui_group(content, NULL, NULL);
	g_signal_connect(p->scripts, "row-activated", G_CALLBACK(on_row_activated), p);

	p->quick = ui_app_list_new(content, "Quick launch",
		"Apps shown by the quick launch widget. The button beside an app changes its icon.",
		on_quick_changed, p);
	p->quick->extra = on_quick_icon;
	p->quick->extra_icon = "image-x-generic-symbolic";
	p->quick->extra_tooltip = "Change the icon";
	p->quick->row_icon = quick_row_icon;

	menus_section_attach(s, content);

	s->taskbar_page = p;
	rebuild_all(p);
	return page;
}
