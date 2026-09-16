#include <string.h>
#include "settings.h"

enum entry_kind {
	ENTRY_ITEM,
	ENTRY_SEPARATOR,
	ENTRY_SUBMENU,
};

struct mentry {
	enum entry_kind kind;
	char *label, *icon, *command;
	bool disabled, checked, bold;
	GPtrArray *children; // submenus only
};

static const char *const menu_names[] = { "taskbar", "window", "start" };

/* A list of `place` or `power` statements of the start menu. */
struct record_list {
	struct menus_page *p;
	const char *statement;
	int fields;
	const char *const *placeholders;
	GPtrArray *records; // char ** with `fields` strings
	GPtrArray *trash;
	GtkWidget *list;
	guint rebuild_id;
};

struct menus_page {
	struct settings *s;
	bool updating;
	GtkWidget *menu_dd, *crumb, *back, *list, *layout_dd;
	GPtrArray *items; // struct mentry *, root level of the selected menu
	GPtrArray *path;  // struct mentry *, open submenus (borrowed)
	GPtrArray *trash; // removed entries, kept until the next reload
	guint rebuild_id;
	struct app_list *pinned;
	struct record_list places, power;
};

static struct confdoc *doc(struct menus_page *p) {
	return p->s->taskbar;
}

static void mentry_free(gpointer data) {
	struct mentry *e = data;
	g_free(e->label);
	g_free(e->icon);
	g_free(e->command);
	if (e->children) {
		g_ptr_array_unref(e->children);
	}
	g_free(e);
}

static struct mentry *mentry_new(enum entry_kind kind, const char *label) {
	struct mentry *e = g_new0(struct mentry, 1);
	e->kind = kind;
	e->label = g_strdup(label);
	if (kind == ENTRY_SUBMENU) {
		e->children = g_ptr_array_new_with_free_func(mentry_free);
	}
	return e;
}

static GPtrArray *parse_entries(struct cstmt *block) {
	GPtrArray *items = g_ptr_array_new_with_free_func(mentry_free);
	for (guint i = 0; block && block->children && i < block->children->len; i++) {
		struct cstmt *c = block->children->pdata[i];
		if (strcmp(c->name, "separator") == 0) {
			g_ptr_array_add(items, mentry_new(ENTRY_SEPARATOR, NULL));
			continue;
		}
		bool submenu = strcmp(c->name, "submenu") == 0;
		if ((!submenu && strcmp(c->name, "item") != 0) || cstmt_argc(c) < 1) {
			continue;
		}
		struct mentry *e = mentry_new(submenu ? ENTRY_SUBMENU : ENTRY_ITEM, cstmt_arg(c, 0));
		int arg = 1;
		while (arg < cstmt_argc(c)) {
			const char *a = cstmt_arg(c, arg);
			if (strcmp(a, "icon") == 0 && arg + 1 < cstmt_argc(c)) {
				e->icon = g_strdup(cstmt_arg(c, arg + 1));
				arg += 2;
			} else if (strcmp(a, "disabled") == 0) {
				e->disabled = true;
				arg++;
			} else if (strcmp(a, "checked") == 0) {
				e->checked = true;
				arg++;
			} else if (strcmp(a, "bold") == 0) {
				e->bold = true;
				arg++;
			} else {
				break;
			}
		}
		if (submenu) {
			g_ptr_array_unref(e->children);
			e->children = parse_entries(c);
		} else {
			e->command = cstmt_join(c, arg);
		}
		g_ptr_array_add(items, e);
	}
	return items;
}

static char *quote_label(const char *label) {
	char *quoted = conf_quote(label ? label : "");
	if (quoted[0] == '"') {
		return quoted;
	}
	char *result = g_strdup_printf("\"%s\"", quoted);
	g_free(quoted);
	return result;
}

static void serialize(GString *out, GPtrArray *items, int depth) {
	for (guint i = 0; i < items->len; i++) {
		struct mentry *e = items->pdata[i];
		for (int d = 0; d < depth; d++) {
			g_string_append_c(out, '\t');
		}
		if (e->kind == ENTRY_SEPARATOR) {
			g_string_append(out, "separator\n");
			continue;
		}
		char *label = quote_label(e->label);
		g_string_append_printf(out, "%s %s", e->kind == ENTRY_SUBMENU ? "submenu" : "item",
			label);
		g_free(label);
		if (e->icon) {
			char *icon = conf_quote(e->icon);
			g_string_append_printf(out, " icon %s", icon);
			g_free(icon);
		}
		if (e->disabled) {
			g_string_append(out, " disabled");
		}
		if (e->checked) {
			g_string_append(out, " checked");
		}
		if (e->bold) {
			g_string_append(out, " bold");
		}
		if (e->kind == ENTRY_SUBMENU) {
			g_string_append(out, " {\n");
			serialize(out, e->children, depth + 1);
			for (int d = 0; d < depth; d++) {
				g_string_append_c(out, '\t');
			}
			g_string_append(out, "}\n");
		} else {
			if (e->command && *e->command) {
				char *command = conf_quote_command(e->command);
				g_string_append_printf(out, " %s", command);
				g_free(command);
			}
			g_string_append_c(out, '\n');
		}
	}
}

static const char *menu_name(struct menus_page *p) {
	return menu_names[gtk_drop_down_get_selected(GTK_DROP_DOWN(p->menu_dd))];
}

static void write_menu(struct menus_page *p) {
	GString *text = g_string_new(NULL);
	g_string_append_printf(text, "menu %s {\n", menu_name(p));
	serialize(text, p->items, 1);
	g_string_append(text, "}");
	struct cstmt *block = confdoc_block(doc(p), "menu", menu_name(p), false);
	if (block) {
		confdoc_replace(doc(p), block, text->str);
	} else {
		confdoc_append(doc(p), doc(p)->root, text->str);
	}
	g_string_free(text, TRUE);
	settings_taskbar_changed(p->s);
}

static GPtrArray *current_level(struct menus_page *p) {
	if (p->path->len == 0) {
		return p->items;
	}
	struct mentry *submenu = p->path->pdata[p->path->len - 1];
	return submenu->children;
}

static void rebuild_menu(struct menus_page *p);

static gboolean rebuild_menu_idle(gpointer data) {
	struct menus_page *p = data;
	p->rebuild_id = 0;
	rebuild_menu(p);
	return G_SOURCE_REMOVE;
}

static void schedule_menu_rebuild(struct menus_page *p) {
	if (!p->rebuild_id) {
		p->rebuild_id = g_idle_add(rebuild_menu_idle, p);
	}
}

enum {
	MENU_UP,
	MENU_DOWN,
	MENU_REMOVE,
	MENU_OPEN,
};

struct menu_action {
	struct menus_page *p;
	guint index;
	int op;
};

static void on_menu_action(GtkButton *button, gpointer data) {
	struct menu_action *a = data;
	struct menus_page *p = a->p;
	GPtrArray *level = current_level(p);
	guint i = a->index;
	if (i >= level->len) {
		return;
	}
	gpointer *d = level->pdata;
	gpointer tmp;
	switch (a->op) {
	case MENU_UP:
		if (i == 0) {
			return;
		}
		tmp = d[i];
		d[i] = d[i - 1];
		d[i - 1] = tmp;
		break;
	case MENU_DOWN:
		if (i + 1 >= level->len) {
			return;
		}
		tmp = d[i];
		d[i] = d[i + 1];
		d[i + 1] = tmp;
		break;
	case MENU_REMOVE:
		g_ptr_array_add(p->trash, g_ptr_array_steal_index(level, i));
		break;
	case MENU_OPEN:
		g_ptr_array_add(p->path, d[i]);
		schedule_menu_rebuild(p);
		return;
	}
	write_menu(p);
	schedule_menu_rebuild(p);
}

struct entry_binding {
	struct menus_page *p;
	struct mentry *entry;
	bool command;
};

static void on_entry_text(GtkEditable *editable, gpointer data) {
	struct entry_binding *b = data;
	if (b->p->updating) {
		return;
	}
	char **field = b->command ? &b->entry->command : &b->entry->label;
	g_free(*field);
	*field = g_strdup(gtk_editable_get_text(editable));
	write_menu(b->p);
}

static GtkWidget *bound_entry(struct menus_page *p, struct mentry *e, bool command) {
	GtkWidget *entry = gtk_entry_new();
	const char *value = command ? e->command : e->label;
	gtk_editable_set_text(GTK_EDITABLE(entry), value ? value : "");
	if (command) {
		gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Command, e.g. exec firefox");
		gtk_widget_set_hexpand(entry, TRUE);
	} else {
		gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Label");
		gtk_editable_set_width_chars(GTK_EDITABLE(entry), 22);
	}
	struct entry_binding *b = g_new0(struct entry_binding, 1);
	b->p = p;
	b->entry = e;
	b->command = command;
	g_signal_connect_data(entry, "changed", G_CALLBACK(on_entry_text), b, ui_closure_free, 0);
	return entry;
}

static void add_menu_button(struct menus_page *p, GtkWidget *box, const char *icon,
		const char *tooltip, bool sensitive, guint index, int op) {
	struct menu_action *a = g_new0(struct menu_action, 1);
	a->p = p;
	a->index = index;
	a->op = op;
	gtk_box_append(GTK_BOX(box), ui_icon_button(icon, tooltip, sensitive,
		G_CALLBACK(on_menu_action), a));
}

static void rebuild_menu(struct menus_page *p) {
	p->updating = true;
	gtk_list_box_remove_all(GTK_LIST_BOX(p->list));
	GPtrArray *level = current_level(p);
	GString *crumb = g_string_new(NULL);
	const char *titles[] = { "Taskbar menu", "Taskbar button menu", "Start button menu" };
	g_string_append(crumb, titles[gtk_drop_down_get_selected(GTK_DROP_DOWN(p->menu_dd))]);
	for (guint i = 0; i < p->path->len; i++) {
		struct mentry *e = p->path->pdata[i];
		g_string_append_printf(crumb, " › %s", e->label);
	}
	gtk_label_set_text(GTK_LABEL(p->crumb), crumb->str);
	g_string_free(crumb, TRUE);
	gtk_widget_set_visible(p->back, p->path->len > 0);

	for (guint i = 0; i < level->len; i++) {
		struct mentry *e = level->pdata[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_margin_start(box, 10);
		gtk_widget_set_margin_end(box, 6);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		if (e->kind == ENTRY_SEPARATOR) {
			GtkWidget *label = gtk_label_new("Separator");
			gtk_widget_add_css_class(label, "dim-label");
			gtk_label_set_xalign(GTK_LABEL(label), 0);
			gtk_widget_set_hexpand(label, TRUE);
			gtk_box_append(GTK_BOX(box), label);
		} else {
			gtk_box_append(GTK_BOX(box), ui_app_icon(e->kind == ENTRY_SUBMENU ?
				"folder-symbolic" : (e->icon ? e->icon : "system-run-symbolic"), 16));
			gtk_box_append(GTK_BOX(box), bound_entry(p, e, false));
			if (e->kind == ENTRY_SUBMENU) {
				char *text = g_strdup_printf("%u entries", e->children->len);
				GtkWidget *label = gtk_label_new(text);
				g_free(text);
				gtk_widget_add_css_class(label, "dim-label");
				gtk_label_set_xalign(GTK_LABEL(label), 0);
				gtk_widget_set_hexpand(label, TRUE);
				gtk_box_append(GTK_BOX(box), label);
				add_menu_button(p, box, "go-next-symbolic", "Open submenu", true, i, MENU_OPEN);
			} else {
				gtk_box_append(GTK_BOX(box), bound_entry(p, e, true));
			}
		}
		add_menu_button(p, box, "go-up-symbolic", "Move up", i > 0, i, MENU_UP);
		add_menu_button(p, box, "go-down-symbolic", "Move down", i + 1 < level->len, i,
			MENU_DOWN);
		add_menu_button(p, box, "list-remove-symbolic", "Remove", true, i, MENU_REMOVE);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(p->list), row);
	}
	if (level->len == 0) {
		ui_row(p->list, NULL, "This menu is empty", NULL);
	}
	p->updating = false;
}

static void on_add_entry(GtkButton *button, gpointer data) {
	struct menus_page *p = data;
	int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "kind"));
	const char *label = kind == ENTRY_SUBMENU ? "New submenu" : "New item";
	g_ptr_array_add(current_level(p), mentry_new(kind, kind == ENTRY_SEPARATOR ? NULL : label));
	write_menu(p);
	schedule_menu_rebuild(p);
}

static void on_back(GtkButton *button, gpointer data) {
	struct menus_page *p = data;
	if (p->path->len > 0) {
		g_ptr_array_remove_index(p->path, p->path->len - 1);
		schedule_menu_rebuild(p);
	}
}

static void load_menu(struct menus_page *p) {
	if (p->items) {
		g_ptr_array_unref(p->items);
	}
	p->items = parse_entries(confdoc_block(doc(p), "menu", menu_name(p), false));
	g_ptr_array_set_size(p->path, 0);
	g_ptr_array_set_size(p->trash, 0);
	rebuild_menu(p);
}

static void on_menu_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	load_menu(data);
}

/* ---------- start menu ---------- */

static struct cstmt *startmenu_block(struct menus_page *p, bool create) {
	return confdoc_block(doc(p), "startmenu", NULL, create);
}

/* The style of the start menu, normally the one the theme asks for. */
static const char *const layout_names[] = {
	NULL, "classic", "twocolumn", "list", "tiles", "centered",
};

static void on_layout_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct menus_page *p = data;
	if (p->updating) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	const char *name = i < G_N_ELEMENTS(layout_names) ? layout_names[i] : NULL;
	struct cstmt *block = startmenu_block(p, name != NULL);
	if (!block) {
		return; // "follow the theme" and nothing written yet
	}
	confdoc_set(doc(p), block, "layout", NULL, name);
	settings_taskbar_changed(p->s);
}

static void on_pinned_changed(struct app_list *list, gpointer data) {
	struct menus_page *p = data;
	GString *args = g_string_new(NULL);
	for (guint i = 0; i < list->ids->len; i++) {
		char *quoted = conf_quote(list->ids->pdata[i]);
		g_string_append_printf(args, "%s%s", i ? " " : "", quoted);
		g_free(quoted);
	}
	struct cstmt *block = startmenu_block(p, true);
	confdoc_set(doc(p), block, "pinned", NULL, args->len ? args->str : NULL);
	g_string_free(args, TRUE);
	settings_taskbar_changed(p->s);
}

static void records_write(struct record_list *r) {
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < r->records->len; i++) {
		char **fields = r->records->pdata[i];
		GString *line = g_string_new(NULL);
		for (int f = 0; f < r->fields; f++) {
			char *quoted = f == 0 ? quote_label(fields[f]) : conf_quote(fields[f]);
			g_string_append_printf(line, "%s%s", f ? " " : "", quoted);
			g_free(quoted);
		}
		g_ptr_array_add(lines, g_string_free(line, FALSE));
	}
	struct cstmt *block = startmenu_block(r->p, true);
	confdoc_set_list(doc(r->p), block, r->statement, NULL, lines);
	g_ptr_array_unref(lines);
	settings_taskbar_changed(r->p->s);
}

static void records_rebuild(struct record_list *r);

static gboolean records_rebuild_idle(gpointer data) {
	struct record_list *r = data;
	r->rebuild_id = 0;
	records_rebuild(r);
	return G_SOURCE_REMOVE;
}

struct record_action {
	struct record_list *r;
	guint index;
	int op;
};

static void on_record_action(GtkButton *button, gpointer data) {
	struct record_action *a = data;
	struct record_list *r = a->r;
	guint i = a->index;
	if (a->op == MENU_OPEN) { // add
		char **fields = g_new0(char *, r->fields + 1);
		for (int f = 0; f < r->fields; f++) {
			fields[f] = g_strdup(f == 0 ? "New entry" : "");
		}
		g_ptr_array_add(r->records, fields);
	} else if (i >= r->records->len) {
		return;
	} else if (a->op == MENU_REMOVE) {
		g_ptr_array_add(r->trash, g_ptr_array_steal_index(r->records, i));
	} else {
		guint j = a->op == MENU_UP ? i - 1 : i + 1;
		if ((a->op == MENU_UP && i == 0) || j >= r->records->len) {
			return;
		}
		gpointer tmp = r->records->pdata[i];
		r->records->pdata[i] = r->records->pdata[j];
		r->records->pdata[j] = tmp;
	}
	records_write(r);
	if (!r->rebuild_id) {
		r->rebuild_id = g_idle_add(records_rebuild_idle, r);
	}
}

struct field_binding {
	struct record_list *r;
	char **fields;
	int field;
};

static void on_field_text(GtkEditable *editable, gpointer data) {
	struct field_binding *b = data;
	if (b->r->p->updating) {
		return;
	}
	g_free(b->fields[b->field]);
	b->fields[b->field] = g_strdup(gtk_editable_get_text(editable));
	records_write(b->r);
}

static void add_record_button(struct record_list *r, GtkWidget *box, const char *icon,
		const char *tooltip, bool sensitive, guint index, int op) {
	struct record_action *a = g_new0(struct record_action, 1);
	a->r = r;
	a->index = index;
	a->op = op;
	gtk_box_append(GTK_BOX(box), ui_icon_button(icon, tooltip, sensitive,
		G_CALLBACK(on_record_action), a));
}

static void records_rebuild(struct record_list *r) {
	r->p->updating = true;
	gtk_list_box_remove_all(GTK_LIST_BOX(r->list));
	for (guint i = 0; i < r->records->len; i++) {
		char **fields = r->records->pdata[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_margin_start(box, 10);
		gtk_widget_set_margin_end(box, 6);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		for (int f = 0; f < r->fields; f++) {
			GtkWidget *entry = gtk_entry_new();
			gtk_editable_set_text(GTK_EDITABLE(entry), fields[f] ? fields[f] : "");
			gtk_entry_set_placeholder_text(GTK_ENTRY(entry), r->placeholders[f]);
			if (f == r->fields - 1) {
				gtk_widget_set_hexpand(entry, TRUE);
			} else {
				gtk_editable_set_width_chars(GTK_EDITABLE(entry), f == 0 ? 16 : 18);
			}
			struct field_binding *b = g_new0(struct field_binding, 1);
			b->r = r;
			b->fields = fields;
			b->field = f;
			g_signal_connect_data(entry, "changed", G_CALLBACK(on_field_text), b,
				ui_closure_free, 0);
			gtk_box_append(GTK_BOX(box), entry);
		}
		add_record_button(r, box, "go-up-symbolic", "Move up", i > 0, i, MENU_UP);
		add_record_button(r, box, "go-down-symbolic", "Move down", i + 1 < r->records->len, i,
			MENU_DOWN);
		add_record_button(r, box, "list-remove-symbolic", "Remove", true, i, MENU_REMOVE);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(r->list), row);
	}
	GtkWidget *add = gtk_button_new_with_label("Add entry");
	struct record_action *a = g_new0(struct record_action, 1);
	a->r = r;
	a->op = MENU_OPEN;
	g_signal_connect_data(add, "clicked", G_CALLBACK(on_record_action), a, ui_closure_free, 0);
	ui_row(r->list, NULL, r->records->len ? NULL : "No entries: the built-in defaults are used",
		add);
	r->p->updating = false;
}

static void records_read(struct record_list *r) {
	g_ptr_array_set_size(r->records, 0);
	g_ptr_array_set_size(r->trash, 0);
	struct cstmt *block = startmenu_block(r->p, false);
	for (guint i = 0; block && i < block->children->len; i++) {
		struct cstmt *c = block->children->pdata[i];
		if (strcmp(c->name, r->statement) != 0 || cstmt_argc(c) < r->fields) {
			continue;
		}
		char **fields = g_new0(char *, r->fields + 1);
		for (int f = 0; f < r->fields - 1; f++) {
			fields[f] = g_strdup(cstmt_arg(c, f));
		}
		fields[r->fields - 1] = cstmt_join(c, r->fields - 1);
		g_ptr_array_add(r->records, fields);
	}
	records_rebuild(r);
}

static void records_init(struct record_list *r, struct menus_page *p, GtkWidget *content,
		const char *title, const char *description, const char *statement, int fields,
		const char *const *placeholders) {
	r->p = p;
	r->statement = statement;
	r->fields = fields;
	r->placeholders = placeholders;
	r->records = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	r->trash = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	r->list = ui_group(content, title, description);
}

void menus_page_refresh(struct settings *s) {
	struct menus_page *p = s->menus_page;
	if (!p) {
		return;
	}
	load_menu(p);
	p->updating = true;
	const char *layout = cstmt_arg(confdoc_child(startmenu_block(p, false), "layout", NULL), 0);
	guint selected = 0;
	for (guint i = 1; layout && i < G_N_ELEMENTS(layout_names); i++) {
		if (strcmp(layout, layout_names[i]) == 0) {
			selected = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->layout_dd), selected);
	p->updating = false;
	struct cstmt *pinned = confdoc_child(startmenu_block(p, false), "pinned", NULL);
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; i < cstmt_argc(pinned); i++) {
		g_ptr_array_add(ids, g_strdup(cstmt_arg(pinned, i)));
	}
	ui_app_list_set(p->pinned, ids);
	g_ptr_array_unref(ids);
	records_read(&p->places);
	records_read(&p->power);
}

GtkWidget *menus_page_new(struct settings *s) {
	struct menus_page *p = g_new0(struct menus_page, 1);
	p->s = s;
	p->path = g_ptr_array_new();
	p->trash = g_ptr_array_new_with_free_func(mentry_free);
	GtkWidget *content;
	GtkWidget *page = ui_page("Menus",
		"Right-click menus of the taskbar and the contents of the start menu. Commands are tileWin commands such as \"exec firefox\" or \"arrange cascade\".",
		&content);

	GtkWidget *group = ui_group(content, "Right-click menus", NULL);
	p->menu_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Empty area of the taskbar", "Taskbar buttons (extra entries)", "Start button", NULL });
	ui_row(group, "Menu", NULL, p->menu_dd);

	GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(nav, 8);
	p->back = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_set_tooltip_text(p->back, "Back to the parent menu");
	g_signal_connect(p->back, "clicked", G_CALLBACK(on_back), p);
	gtk_box_append(GTK_BOX(nav), p->back);
	p->crumb = gtk_label_new("");
	gtk_widget_add_css_class(p->crumb, "tw-heading");
	gtk_label_set_xalign(GTK_LABEL(p->crumb), 0);
	gtk_widget_set_hexpand(p->crumb, TRUE);
	gtk_box_append(GTK_BOX(nav), p->crumb);
	static const struct {
		const char *label;
		int kind;
	} adds[] = {
		{ "Add item", ENTRY_ITEM },
		{ "Add separator", ENTRY_SEPARATOR },
		{ "Add submenu", ENTRY_SUBMENU },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(adds); i++) {
		GtkWidget *button = gtk_button_new_with_label(adds[i].label);
		g_object_set_data(G_OBJECT(button), "kind", GINT_TO_POINTER(adds[i].kind));
		g_signal_connect(button, "clicked", G_CALLBACK(on_add_entry), p);
		gtk_box_append(GTK_BOX(nav), button);
	}
	gtk_box_append(GTK_BOX(content), nav);
	p->list = ui_group(content, NULL, NULL);
	g_signal_connect(p->menu_dd, "notify::selected", G_CALLBACK(on_menu_selected), p);

	GtkWidget *start = ui_group(content, "Start menu", NULL);
	p->layout_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"From the theme", "Classic (Windows 95)", "Two columns (Windows XP, 7)",
		"List (Windows 10)", "Tiles (Windows 8)", "Centered (Windows 11)", NULL });
	ui_row(start, "Style", "How the start menu is laid out", p->layout_dd);
	g_signal_connect(p->layout_dd, "notify::selected", G_CALLBACK(on_layout_selected), p);

	p->pinned = ui_app_list_new(content, "Start menu: pinned apps", NULL, on_pinned_changed, p);
	records_init(&p->places, p, content, "Start menu: places",
		"Links shown next to the app list. The icon is an icon name.", "place", 3,
		(const char *const[]){ "Label", "Icon", "Command" });
	records_init(&p->power, p, content, "Start menu: power menu", NULL, "power", 2,
		(const char *const[]){ "Label", "Command" });

	s->menus_page = p;
	menus_page_refresh(s);
	return page;
}
