#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_widgets.h"

/*
 * The Desktop page: the grid of the desktop icons and the widgets that sit in
 * it. Both live in taskbar.conf, the grid at its top level and the widgets in
 * its "desktop_widgets" block; how a widget looks and where it sits is edited
 * in the widget dialog (widgets.c).
 */

struct desktop_page {
	struct settings *s;
	GPtrArray *keys; // struct ui_taskbar_key *
	GtkWidget *widgets;
	guint rebuild_id;
};

static struct confdoc *doc(struct desktop_page *p) {
	return p->s->taskbar;
}

static GPtrArray *desktop_names(struct desktop_page *p) {
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	struct cstmt *block = confdoc_block(doc(p), "desktop_widgets", NULL, false);
	for (guint i = 0; block && i < block->children->len; i++) {
		struct cstmt *c = block->children->pdata[i];
		g_ptr_array_add(names, g_strdup(c->name));
	}
	return names;
}

static bool array_has(GPtrArray *array, const char *str) {
	for (guint i = 0; array && i < array->len; i++) {
		if (strcmp(array->pdata[i], str) == 0) {
			return true;
		}
	}
	return false;
}

static void rebuild(struct desktop_page *p);

static gboolean rebuild_idle(gpointer data) {
	struct desktop_page *p = data;
	p->rebuild_id = 0;
	rebuild(p);
	return G_SOURCE_REMOVE;
}

static void schedule_rebuild(struct desktop_page *p) {
	if (!p->rebuild_id) {
		p->rebuild_id = g_idle_add(rebuild_idle, p);
	}
}

/* ---------- adding and removing ---------- */

struct name_action {
	struct desktop_page *p;
	char *name;
};

static struct name_action *name_action_new(struct desktop_page *p, const char *name) {
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

static void on_add(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	if (popover) {
		gtk_popover_popdown(GTK_POPOVER(popover));
	}
	struct cstmt *block = confdoc_block(doc(a->p), "desktop_widgets", NULL, true);
	char *entry = g_strdup_printf("%s {\n}", a->name);
	confdoc_append(doc(a->p), block, entry);
	g_free(entry);
	settings_taskbar_changed(a->p->s);
	settings_status(a->p->s, "Added to the desktop. It takes the first free cells at the "
		"right; drag it where you want it.");
	schedule_rebuild(a->p);
}

static void on_settings(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	widget_dialog_open(a->p->s, a->name, true);
}

static void on_remove(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	struct cstmt *entry = widget_desktop_entry(a->p->s, a->name);
	if (entry) {
		confdoc_remove(doc(a->p), entry);
		settings_taskbar_changed(a->p->s);
	}
	schedule_rebuild(a->p);
}

/*
 * Every widget that can go on the desktop, the scripts of taskbar.conf too. A
 * widget that is there already comes again under a name of its own
 * (clock:2), so there can be a digital and an analog clock.
 */
static GtkWidget *add_button(struct desktop_page *p, GPtrArray *used) {
	GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free);
	for (size_t i = 0; i < tw_widget_count; i++) {
		const struct tw_widget_info *info = &tw_widgets[i];
		if ((info->flags & TW_WIDGET_DESKTOP) && !(info->flags & TW_WIDGET_UNLISTED)) {
			g_ptr_array_add(candidates, g_strdup(info->type));
		}
	}
	struct cstmt *root = doc(p)->root;
	for (guint i = 0; i < root->children->len; i++) {
		struct cstmt *c = root->children->pdata[i];
		const char *name = cstmt_arg(c, 0);
		if (strcmp(c->name, "widget") == 0 && name && g_str_has_prefix(name, "custom:") &&
				!array_has(candidates, name)) {
			g_ptr_array_add(candidates, g_strdup(name));
		}
	}

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (guint i = 0; i < candidates->len; i++) {
		const char *name = candidates->pdata[i];
		char *unique = g_strdup(name);
		for (int n = 2; array_has(used, unique); n++) {
			g_free(unique);
			unique = g_strdup_printf("%s:%d", name, n);
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
		g_signal_connect_data(button, "clicked", G_CALLBACK(on_add),
			name_action_new(p, unique), name_action_free, 0);
		gtk_box_append(GTK_BOX(box), button);
		g_free(title);
		g_free(unique);
	}
	g_ptr_array_unref(candidates);

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

/* "Analog clock · column -1, row 0", from its entry in desktop_widgets. */
static char *describe(struct desktop_page *p, const char *name) {
	struct cstmt *entry = widget_desktop_entry(p->s, name);
	char *type = widget_type_of(name);
	const struct tw_widget_info *info = tw_widget_find(type);
	g_free(type);
	const char *style_name = cstmt_arg(confdoc_child(entry, "style", NULL), 0);
	const struct tw_widget_style *style = tw_widget_style_find(info, style_name);
	const char *column = cstmt_arg(confdoc_child(entry, "column", NULL), 0);
	const char *row = cstmt_arg(confdoc_child(entry, "row", NULL), 0);
	const char *size = cstmt_arg(confdoc_child(entry, "size", NULL), 0);
	GString *text = g_string_new(style ? style->title : name);
	if (column || row) {
		g_string_append_printf(text, " · column %s, row %s", column ? column : "-1",
			row ? row : "0");
	} else {
		g_string_append(text, " · placed at the right");
	}
	if (size) {
		g_string_append_printf(text, " · %s cells", size);
	}
	return g_string_free(text, FALSE);
}

static void rebuild(struct desktop_page *p) {
	ui_taskbar_keys_refresh(p->keys);
	GtkWidget *list = p->widgets;
	gtk_list_box_remove_all(GTK_LIST_BOX(list));
	GPtrArray *names = desktop_names(p);
	for (guint i = 0; i < names->len; i++) {
		const char *name = names->pdata[i];
		char *title = widget_title(name, NULL);
		char *subtitle = describe(p, name);
		GtkWidget *row = ui_row(list, title, subtitle, NULL);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
		gtk_widget_set_tooltip_text(row, "Click to change how it looks and where it sits");
		g_object_set_data_full(G_OBJECT(row), "widget", g_strdup(name), g_free);
		GtkWidget *box = ui_row_box(row);
		static const struct {
			const char *icon, *tooltip;
			GCallback callback;
		} buttons[] = {
			{ "emblem-system-symbolic", "Widget settings", G_CALLBACK(on_settings) },
			{ "list-remove-symbolic", "Take off the desktop", G_CALLBACK(on_remove) },
		};
		for (size_t b = 0; b < G_N_ELEMENTS(buttons); b++) {
			GtkWidget *button = gtk_button_new_from_icon_name(buttons[b].icon);
			gtk_widget_set_tooltip_text(button, buttons[b].tooltip);
			gtk_widget_add_css_class(button, "flat");
			gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
			g_signal_connect_data(button, "clicked", buttons[b].callback,
				name_action_new(p, name), name_action_free, 0);
			gtk_box_append(GTK_BOX(box), button);
		}
		g_free(subtitle);
		g_free(title);
	}
	ui_row(list, NULL, names->len ? NULL : "None yet", add_button(p, names));
	g_ptr_array_unref(names);
}

void desktop_page_refresh(struct settings *s) {
	if (s->desktop_page) {
		schedule_rebuild(s->desktop_page);
	}
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
	struct desktop_page *p = data;
	const char *name = g_object_get_data(G_OBJECT(row), "widget");
	if (name) {
		widget_dialog_open(p->s, name, true);
	}
}

static void on_wallpaper(GtkButton *button, gpointer data) {
	struct desktop_page *p = data;
	gtk_stack_set_visible_child_name(p->s->stack, "wallpaper");
}

GtkWidget *desktop_page_new(struct settings *s) {
	struct desktop_page *p = g_new0(struct desktop_page, 1);
	p->s = s;
	p->keys = g_ptr_array_new();
	GtkWidget *content;
	GtkWidget *page = ui_page("Desktop",
		"The icons and the widgets on the desktop. Changes are saved to taskbar.conf and "
		"show right away.", &content);

	GtkWidget *icons = ui_group(content, "Icons and grid",
		"The icons of ~/Desktop and the grid they sit in. The widgets take whole cells of the "
		"same grid, so a bigger cell makes them bigger too.");
	ui_taskbar_key(s, p->keys, icons, "desktop_icons", "Show icons on the desktop", NULL,
		true, 0, 0, 1);
	ui_taskbar_key(s, p->keys, icons, "desktop_icon_size", "Icon size", "Pixels",
		false, 16, 256, 48);
	ui_taskbar_key(s, p->keys, icons, "desktop_icon_width", "Cell width",
		"Pixels of a cell of the grid", false, 48, 400, 100);
	ui_taskbar_key(s, p->keys, icons, "desktop_icon_height", "Cell height",
		"Pixels of a cell of the grid", false, 48, 400, 100);
	ui_taskbar_key(s, p->keys, icons, "desktop_margin", "Margin", "Pixels around the whole grid",
		false, 0, 200, 10);

	p->widgets = ui_group(content, "Widgets",
		"The widgets of the taskbar, on the desktop: as on the taskbar, or as a chart, a "
		"gauge, a ring, a bar, an analog, digital or binary clock, drawn in the colors of "
		"the theme. Drag one on the desktop to move it from cell to cell; the icons make "
		"room. Its own settings are shared with the taskbar.");
	g_signal_connect(p->widgets, "row-activated", G_CALLBACK(on_row_activated), p);

	GtkWidget *more = ui_group(content, "Background", NULL);
	GtkWidget *wallpaper = gtk_button_new_with_label("Wallpaper…");
	gtk_widget_set_valign(wallpaper, GTK_ALIGN_CENTER);
	g_signal_connect(wallpaper, "clicked", G_CALLBACK(on_wallpaper), p);
	ui_row(more, "Wallpaper", "The picture or color behind the icons, on its own page",
		wallpaper);

	s->desktop_page = p;
	rebuild(p);
	return page;
}
