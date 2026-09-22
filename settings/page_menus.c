#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_desktop.h"

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
	int icon_field;  // field holding an icon name, -1 when there is none
	bool choosable;  // the last field is a command that can be picked
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

/*
 * The right-click menus sit on the Taskbar page and the start menu has a page
 * of its own, but both are the same config file and share the picker that fills
 * a label, an icon and a command in. They therefore share one structure, made
 * by whichever page is built first.
 */
static struct menus_page *page_get(struct settings *s) {
	if (!s->menus_page) {
		struct menus_page *p = g_new0(struct menus_page, 1);
		p->s = s;
		p->path = g_ptr_array_new();
		p->trash = g_ptr_array_new_with_free_func(mentry_free);
		s->menus_page = p;
	}
	return s->menus_page;
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

/* ---------- choosing a command instead of typing one ---------- */

struct choice {
	const char *label, *icon, *command;
};

/* What tileWin itself can do. */
static const struct choice action_choices[] = {
	{ "Cascade windows", "window-cascade", "arrange cascade" },
	{ "Show windows stacked", "window-stack", "arrange vertical" },
	{ "Show windows side by side", "window-side-by-side", "arrange horizontal" },
	{ "Arrange windows optimally", "window-arrange", "arrange optimal" },
	{ "Show the desktop", "user-desktop", "showdesktop" },
	{ "Task view", "window-stack", "taskview" },
	{ "Switch tile/window mode", "tilewin-mode", "wm_mode toggle" },
	{ "Start menu", "system-search", "panel startmenu toggle" },
	{ "Search apps", "system-search", "panel startmenu search" },
	{ "Run...", "system-run", "panel run" },
	{ "Notifications", "preferences-system", "panel notifications" },
	{ "Clipboard history", "edit-paste", "panel clipboard" },
	{ "Screenshot", "applets-screenshooter", "panel snip" },
	{ "Refresh the desktop", "view-refresh", "panel desktop refresh" },
	{ "Reload the taskbar", "view-refresh", "panel reload" },
	{ "Task manager", "utilities-system-monitor", "exec $taskmanager" },
	{ "Terminal", "utilities-terminal", "exec $term" },
	{ "File Explorer", "system-file-manager", "exec $filemanager" },
	{ "Settings", "preferences-system", "exec tilewin-settings" },
	{ "Taskbar settings", "preferences-system", "exec tilewin-settings --page taskbar" },
	{ "Change wallpaper", "preferences-desktop-wallpaper",
		"exec tilewin-settings --page wallpaper" },
	{ "Personalize", "preferences-desktop-theme", "exec tilewin-settings --page theme" },
	{ "Lock the screen", "system-lock-screen", "exec $locker" },
	{ "Log off", "system-log-out", "panel shutdown logoff" },
	{ "Shut down or sign out", "system-shutdown", "panel shutdown" },
	{ "Restart tileWin", "system-reboot", "restart" },
	{ "Theme: Windows 95", "preferences-desktop-theme", "exec tilewin-theme set win95" },
	{ "Theme: Windows XP", "preferences-desktop-theme", "exec tilewin-theme set winxp" },
	{ "Theme: Windows 7", "preferences-desktop-theme", "exec tilewin-theme set win7" },
	{ "Theme: Windows 8", "preferences-desktop-theme", "exec tilewin-theme set win8" },
	{ "Theme: Windows 10", "preferences-desktop-theme", "exec tilewin-theme set win10" },
	{ "Theme: Windows 11", "preferences-desktop-theme", "exec tilewin-theme set win11" },
};

/* Folders, opened with the file manager of the Default apps page. */
static const struct choice place_choices[] = {
	{ "Home", "user-home", "exec xdg-open ~" },
	{ "Desktop", "user-desktop", "exec xdg-open ~/Desktop" },
	{ "Documents", "folder-documents", "exec xdg-open ~/Documents" },
	{ "Downloads", "folder-download", "exec xdg-open ~/Downloads" },
	{ "Pictures", "folder-pictures", "exec xdg-open ~/Pictures" },
	{ "Music", "folder-music", "exec xdg-open ~/Music" },
	{ "Videos", "folder-videos", "exec xdg-open ~/Videos" },
	{ "Trash", "user-trash", "exec xdg-open trash:///" },
};

/* Takes over the label, the icon and the command of what was picked. */
typedef void (*choice_apply)(gpointer target, const char *label, const char *icon,
	const char *command);

struct choice_picker {
	struct menus_page *p;
	choice_apply apply;
	gpointer target;
	GtkWidget *popover, *search, *list;
	bool populated;
};

/* The command that starts an app, in a terminal when it asks for one. */
static char *app_command(struct menus_page *p, const struct tw_desktop_entry *e) {
	char *exec = tw_desktop_exec_command(e);
	if (!exec) {
		return NULL;
	}
	char *command;
	if (e->terminal) {
		const char *term = cstmt_arg(confdoc_child(doc(p)->root, "terminal", NULL), 0);
		command = g_strdup_printf("exec %s %s", term && *term ? term : "xfce4-terminal -x", exec);
	} else {
		command = g_strdup_printf("exec %s", exec);
	}
	free(exec);
	return command;
}

static gboolean choice_filter(GtkListBoxRow *row, gpointer data) {
	struct choice_picker *cp = data;
	const char *query = gtk_editable_get_text(GTK_EDITABLE(cp->search));
	if (!*query) {
		return TRUE;
	}
	const char *haystack = g_object_get_data(G_OBJECT(row), "haystack");
	char *needle = g_utf8_casefold(query, -1);
	gboolean match = haystack && strstr(haystack, needle);
	g_free(needle);
	return match;
}

/* A heading above the first row of every group, following the search. */
static void choice_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
	const char *group = g_object_get_data(G_OBJECT(row), "group");
	const char *previous = before ? g_object_get_data(G_OBJECT(before), "group") : NULL;
	if (!group || (previous && strcmp(group, previous) == 0)) {
		gtk_list_box_row_set_header(row, NULL);
		return;
	}
	GtkWidget *label = gtk_label_new(group);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_widget_add_css_class(label, "tw-caption");
	gtk_widget_set_margin_start(label, 8);
	gtk_widget_set_margin_top(label, 8);
	gtk_widget_set_margin_bottom(label, 2);
	gtk_list_box_row_set_header(row, label);
}

static void choice_add_row(struct choice_picker *cp, const char *group, const char *label,
		const char *icon, const char *command) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(box, 6);
	gtk_widget_set_margin_end(box, 6);
	gtk_widget_set_margin_top(box, 4);
	gtk_widget_set_margin_bottom(box, 4);
	gtk_box_append(GTK_BOX(box), ui_app_icon(icon, 24));
	GtkWidget *text = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), text);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	char *haystack = g_strdup_printf("%s %s %s", label, group, command);
	g_object_set_data_full(G_OBJECT(row), "haystack", g_utf8_casefold(haystack, -1), g_free);
	g_free(haystack);
	g_object_set_data(G_OBJECT(row), "group", (gpointer)group);
	g_object_set_data_full(G_OBJECT(row), "label", g_strdup(label), g_free);
	g_object_set_data_full(G_OBJECT(row), "icon", g_strdup(icon), g_free);
	g_object_set_data_full(G_OBJECT(row), "command", g_strdup(command), g_free);
	gtk_list_box_append(GTK_LIST_BOX(cp->list), row);
}

static void choice_show(GtkWidget *popover, gpointer data) {
	struct choice_picker *cp = data;
	gtk_editable_set_text(GTK_EDITABLE(cp->search), "");
	gtk_widget_grab_focus(cp->search);
	if (cp->populated) {
		return;
	}
	cp->populated = true;
	for (size_t i = 0; i < G_N_ELEMENTS(action_choices); i++) {
		choice_add_row(cp, "Actions", action_choices[i].label, action_choices[i].icon,
			action_choices[i].command);
	}
	for (size_t i = 0; i < G_N_ELEMENTS(place_choices); i++) {
		choice_add_row(cp, "Folders", place_choices[i].label, place_choices[i].icon,
			place_choices[i].command);
	}
	list_t *apps = ui_all_apps();
	for (int i = 0; i < apps->length; i++) {
		struct tw_desktop_entry *e = apps->items[i];
		char *command = app_command(cp->p, e);
		if (command) {
			choice_add_row(cp, "Apps", e->name, e->icon, command);
			g_free(command);
		}
	}
}

static void choice_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct choice_picker *cp = data;
	gtk_popover_popdown(GTK_POPOVER(cp->popover));
	cp->apply(cp->target, g_object_get_data(G_OBJECT(row), "label"),
		g_object_get_data(G_OBJECT(row), "icon"),
		g_object_get_data(G_OBJECT(row), "command"));
}

static void choice_search_changed(GtkSearchEntry *entry, gpointer data) {
	struct choice_picker *cp = data;
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(cp->list));
}

/* A button whose popover offers apps, tileWin actions and folders. */
static GtkWidget *choice_button(struct menus_page *p, const char *label, choice_apply apply,
		gpointer target) {
	struct choice_picker *cp = g_new0(struct choice_picker, 1);
	cp->p = p;
	cp->apply = apply;
	cp->target = target;
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_label(GTK_MENU_BUTTON(button), label);
	gtk_widget_set_tooltip_text(button, "Pick an app, an action of tileWin or a folder");
	cp->popover = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	cp->search = gtk_search_entry_new();
	gtk_box_append(GTK_BOX(box), cp->search);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, 360, 400);
	cp->list = gtk_list_box_new();
	gtk_list_box_set_filter_func(GTK_LIST_BOX(cp->list), choice_filter, cp, NULL);
	gtk_list_box_set_header_func(GTK_LIST_BOX(cp->list), choice_header, NULL, NULL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), cp->list);
	gtk_box_append(GTK_BOX(box), scroll);
	gtk_popover_set_child(GTK_POPOVER(cp->popover), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), cp->popover);
	g_signal_connect(cp->popover, "show", G_CALLBACK(choice_show), cp);
	g_signal_connect(cp->list, "row-activated", G_CALLBACK(choice_activated), cp);
	g_signal_connect(cp->search, "search-changed", G_CALLBACK(choice_search_changed), cp);
	g_object_set_data_full(G_OBJECT(button), "choice-picker", cp, g_free);
	return button;
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
		gtk_editable_set_width_chars(GTK_EDITABLE(entry), 16);
	}
	struct entry_binding *b = g_new0(struct entry_binding, 1);
	b->p = p;
	b->entry = e;
	b->command = command;
	g_signal_connect_data(entry, "changed", G_CALLBACK(on_entry_text), b, ui_closure_free, 0);
	return entry;
}

/* A picked app, action or folder fills in the whole entry at once. */
static void entry_apply_choice(gpointer target, const char *label, const char *icon,
		const char *command) {
	struct entry_binding *b = target;
	struct mentry *e = b->entry;
	g_free(e->label);
	e->label = g_strdup(label);
	g_free(e->icon);
	e->icon = g_strdup(icon);
	g_free(e->command);
	e->command = g_strdup(command);
	write_menu(b->p);
	schedule_menu_rebuild(b->p);
}

static void entry_icon_chosen(const char *icon, gpointer data) {
	struct entry_binding *b = data;
	g_free(b->entry->icon);
	b->entry->icon = g_strdup(icon);
	write_menu(b->p);
	schedule_menu_rebuild(b->p);
}

static void on_entry_icon(GtkButton *button, gpointer data) {
	struct entry_binding *b = data;
	ui_icon_dialog(b->p->s->window, b->entry->label && *b->entry->label ? b->entry->label : "Entry",
		"The icon shown next to the entry: an icon name of your icon theme, e.g. firefox, "
		"or an image file.", b->entry->icon, NULL, "No icon", entry_icon_chosen, b);
}

/* A button showing the entry's icon that opens the icon chooser. */
static GtkWidget *entry_icon_button(struct menus_page *p, struct mentry *e) {
	GtkWidget *button = gtk_button_new();
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_tooltip_text(button, "Choose an icon");
	gtk_button_set_child(GTK_BUTTON(button), ui_app_icon(e->icon ? e->icon :
		e->kind == ENTRY_SUBMENU ? "folder-symbolic" : "system-run-symbolic", 16));
	struct entry_binding *b = g_new0(struct entry_binding, 1);
	b->p = p;
	b->entry = e;
	g_signal_connect_data(button, "clicked", G_CALLBACK(on_entry_icon), b, ui_closure_free, 0);
	return button;
}

struct flag_binding {
	struct menus_page *p;
	struct mentry *entry;
	int which; // 0 bold, 1 checked, 2 disabled
};

static void on_flag_toggled(GtkCheckButton *check, gpointer data) {
	struct flag_binding *f = data;
	if (f->p->updating) {
		return;
	}
	bool on = gtk_check_button_get_active(check);
	switch (f->which) {
	case 0: f->entry->bold = on; break;
	case 1: f->entry->checked = on; break;
	default: f->entry->disabled = on; break;
	}
	write_menu(f->p);
}

/* Bold, checked and greyed out, without having to know the keywords. */
static GtkWidget *entry_flags_button(struct menus_page *p, struct mentry *e) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-symbolic");
	gtk_widget_set_tooltip_text(button, "How the entry looks");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(box, 6);
	gtk_widget_set_margin_end(box, 6);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	static const char *const labels[] = { "Bold", "Checked", "Greyed out" };
	const bool values[] = { e->bold, e->checked, e->disabled };
	for (int i = 0; i < 3; i++) {
		GtkWidget *check = gtk_check_button_new_with_label(labels[i]);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), values[i]);
		struct flag_binding *f = g_new0(struct flag_binding, 1);
		f->p = p;
		f->entry = e;
		f->which = i;
		g_signal_connect_data(check, "toggled", G_CALLBACK(on_flag_toggled), f,
			ui_closure_free, 0);
		gtk_box_append(GTK_BOX(box), check);
	}
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	return button;
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
			gtk_box_append(GTK_BOX(box), entry_icon_button(p, e));
			gtk_box_append(GTK_BOX(box), bound_entry(p, e, false));
			if (e->kind == ENTRY_SUBMENU) {
				char *text = g_strdup_printf("%u entries", e->children->len);
				GtkWidget *label = gtk_label_new(text);
				g_free(text);
				gtk_widget_add_css_class(label, "dim-label");
				gtk_label_set_xalign(GTK_LABEL(label), 0);
				gtk_widget_set_hexpand(label, TRUE);
				gtk_box_append(GTK_BOX(box), label);
				gtk_box_append(GTK_BOX(box), entry_flags_button(p, e));
				add_menu_button(p, box, "go-next-symbolic", "Open submenu", true, i, MENU_OPEN);
			} else {
				gtk_box_append(GTK_BOX(box), bound_entry(p, e, true));
				struct entry_binding *b = g_new0(struct entry_binding, 1);
				b->p = p;
				b->entry = e;
				GtkWidget *choose = choice_button(p, "Choose...", entry_apply_choice, b);
				g_object_set_data_full(G_OBJECT(choose), "binding", b, g_free);
				gtk_box_append(GTK_BOX(box), choose);
				gtk_box_append(GTK_BOX(box), entry_flags_button(p, e));
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

/* "Add item" picks the app, action or folder first, so nothing starts empty. */
static void add_apply_choice(gpointer target, const char *label, const char *icon,
		const char *command) {
	struct menus_page *p = target;
	struct mentry *e = mentry_new(ENTRY_ITEM, label);
	e->icon = g_strdup(icon);
	e->command = g_strdup(command);
	g_ptr_array_add(current_level(p), e);
	write_menu(p);
	schedule_menu_rebuild(p);
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
static void records_write(struct record_list *r);

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
	if (i >= r->records->len) {
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

/* A picked app, action or folder fills the whole record at once. */
static void record_apply_choice(gpointer target, const char *label, const char *icon,
		const char *command) {
	struct field_binding *b = target;
	struct record_list *r = b->r;
	g_free(b->fields[0]);
	b->fields[0] = g_strdup(label);
	if (r->icon_field >= 0) {
		g_free(b->fields[r->icon_field]);
		b->fields[r->icon_field] = g_strdup(icon);
	}
	g_free(b->fields[r->fields - 1]);
	b->fields[r->fields - 1] = g_strdup(command);
	records_write(r);
	if (!r->rebuild_id) {
		r->rebuild_id = g_idle_add(records_rebuild_idle, r);
	}
}

static void record_icon_chosen(const char *icon, gpointer data) {
	struct field_binding *b = data;
	g_free(b->fields[b->field]);
	b->fields[b->field] = g_strdup(icon ? icon : "");
	records_write(b->r);
	if (!b->r->rebuild_id) {
		b->r->rebuild_id = g_idle_add(records_rebuild_idle, b->r);
	}
}

static void on_record_icon(GtkButton *button, gpointer data) {
	struct field_binding *b = data;
	ui_icon_dialog(b->r->p->s->window, b->fields[0] && *b->fields[0] ? b->fields[0] : "Entry",
		"The icon shown next to the entry: an icon name of your icon theme, e.g. folder-music, "
		"or an image file.", b->fields[b->field], NULL, "No icon", record_icon_chosen, b);
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

/* "Add entry" picks first as well, so a new row is complete right away. */
static void record_add_choice(gpointer target, const char *label, const char *icon,
		const char *command) {
	struct record_list *r = target;
	char **fields = g_new0(char *, r->fields + 1);
	for (int f = 0; f < r->fields; f++) {
		fields[f] = g_strdup("");
	}
	g_free(fields[0]);
	fields[0] = g_strdup(label);
	if (r->icon_field >= 0) {
		g_free(fields[r->icon_field]);
		fields[r->icon_field] = g_strdup(icon ? icon : "");
	}
	g_free(fields[r->fields - 1]);
	fields[r->fields - 1] = g_strdup(command);
	g_ptr_array_add(r->records, fields);
	records_write(r);
	if (!r->rebuild_id) {
		r->rebuild_id = g_idle_add(records_rebuild_idle, r);
	}
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
			struct field_binding *b = g_new0(struct field_binding, 1);
			b->r = r;
			b->fields = fields;
			b->field = f;
			if (f == r->icon_field) {
				GtkWidget *button = gtk_button_new();
				gtk_widget_add_css_class(button, "flat");
				gtk_widget_set_tooltip_text(button, "Choose an icon");
				gtk_button_set_child(GTK_BUTTON(button), ui_app_icon(
					fields[f] && *fields[f] ? fields[f] : "system-run-symbolic", 16));
				g_signal_connect_data(button, "clicked", G_CALLBACK(on_record_icon), b,
					ui_closure_free, 0);
				gtk_box_prepend(GTK_BOX(box), button); // the icon leads the row
				continue;
			}
			GtkWidget *entry = gtk_entry_new();
			gtk_editable_set_text(GTK_EDITABLE(entry), fields[f] ? fields[f] : "");
			gtk_entry_set_placeholder_text(GTK_ENTRY(entry), r->placeholders[f]);
			if (f == r->fields - 1) {
				gtk_widget_set_hexpand(entry, TRUE);
			} else {
				gtk_editable_set_width_chars(GTK_EDITABLE(entry), 16);
			}
			g_signal_connect_data(entry, "changed", G_CALLBACK(on_field_text), b,
				ui_closure_free, 0);
			gtk_box_append(GTK_BOX(box), entry);
			if (f == r->fields - 1 && r->choosable) {
				struct field_binding *c = g_new0(struct field_binding, 1);
				c->r = r;
				c->fields = fields;
				c->field = f;
				GtkWidget *choose = choice_button(r->p, "Choose...", record_apply_choice, c);
				g_object_set_data_full(G_OBJECT(choose), "binding", c, g_free);
				gtk_box_append(GTK_BOX(box), choose);
			}
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
	GtkWidget *add = choice_button(r->p, "Add entry...", record_add_choice, r);
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
		const char *const *placeholders, int icon_field, bool choosable) {
	r->p = p;
	r->statement = statement;
	r->fields = fields;
	r->placeholders = placeholders;
	r->icon_field = icon_field;
	r->choosable = choosable;
	r->records = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	r->trash = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	r->list = ui_group(content, title, description);
}

void menus_page_refresh(struct settings *s) {
	struct menus_page *p = s->menus_page;
	if (!p) {
		return;
	}
	if (p->list) {
		load_menu(p);
	}
	if (!p->layout_dd) {
		return; // the start menu page has not been built yet
	}
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

/*
 * The right-click menus of the taskbar, put at the end of the Taskbar page.
 */
void menus_section_attach(struct settings *s, GtkWidget *content) {
	struct menus_page *p = page_get(s);

	GtkWidget *group = ui_group(content, "Right-click menus",
		"What the menus of the taskbar hold. Nothing here has to be typed: Add item... and "
		"Choose... pick an app, an action of tileWin or a folder and fill in the label, the "
		"icon and the command, the icon button opens a grid of icons to click, and the button "
		"beside it makes an entry bold, checked or greyed out. Every field can still be edited "
		"by hand.");
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
	gtk_box_append(GTK_BOX(nav), choice_button(p, "Add item...", add_apply_choice, p));
	static const struct {
		const char *label;
		int kind;
	} adds[] = {
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
	load_menu(p);
}

GtkWidget *startmenu_page_new(struct settings *s) {
	struct menus_page *p = page_get(s);
	GtkWidget *content;
	GtkWidget *page = ui_page("Start menu",
		"How the start menu is laid out and what it holds. Choose... picks an app, an action "
		"of tileWin or a folder and fills the label, the icon and the command in; every field "
		"can still be edited by hand.",
		&content);

	GtkWidget *start = ui_group(content, "Style", NULL);
	p->layout_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"From the theme", "Classic (Windows 95)", "Two columns (Windows XP, 7)",
		"List (Windows 10)", "Tiles (Windows 8)", "Centered (Windows 11)", NULL });
	ui_row(start, "Style", "How the start menu is laid out", p->layout_dd);
	g_signal_connect(p->layout_dd, "notify::selected", G_CALLBACK(on_layout_selected), p);

	p->pinned = ui_app_list_new(content, "Pinned apps", NULL, on_pinned_changed, p);
	records_init(&p->places, p, content, "Places",
		"Links shown next to the app list. Click the icon to change it, or pick a whole "
		"entry with Choose.", "place", 3,
		(const char *const[]){ "Label", "Icon", "Command" }, 1, true);
	records_init(&p->power, p, content, "Power menu", NULL, "power", 2,
		(const char *const[]){ "Label", "Command" }, -1, true);

	menus_page_refresh(s);
	return page;
}
