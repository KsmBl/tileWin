#include <stdlib.h>
#include <string.h>
#include "settings.h"

enum {
	SECTION_LEFT,
	SECTION_CENTER,
	SECTION_RIGHT,
	SECTION_COUNT,
};

static const char *const section_keys[] = { "left", "center", "right" };
static const char *const section_titles[] = { "Left", "Center", "Right" };

struct widget_type {
	const char *type, *title, *description;
};

static const struct widget_type widget_types[] = {
	{ "start", "Start button", "Opens the start menu" },
	{ "search", "Search box", "Opens the start menu to search apps" },
	{ "taskbar", "Window buttons", "A button for each open window" },
	{ "quicklaunch", "Quick launch", "Icons of pinned apps" },
	{ "workspaces", "Workspaces", "Buttons for the virtual desktops" },
	{ "title", "Window title", "Title of the focused window" },
	{ "tray", "System tray", "Icons of background apps" },
	{ "keyboard", "Keyboard layout", "Current layout, click to switch" },
	{ "volume", "Volume", "Speaker volume" },
	{ "network", "Network", "Connection status" },
	{ "battery", "Battery", "Charge level" },
	{ "brightness", "Brightness", "Screen brightness" },
	{ "cpu", "CPU usage", "Processor load" },
	{ "memory", "Memory usage", "RAM in use" },
	{ "clock", "Clock", "Time and date with a calendar" },
	{ "modeswitch", "Mode switch", "Switches between tile and window mode" },
	{ "showdesktop", "Show desktop", "Minimizes all windows" },
	{ "separator", "Separator", "A thin line" },
	{ "spacer", "Spacer", "Empty space" },
};

enum opt_flags {
	OPT_TEXT,
	OPT_CHOICE,
};

struct opt {
	const char *key, *title, *hint;
	const char *const *choices; // NULL for free text
};

static const char *const choice_yes_no[] = { "yes", "no", NULL };
static const char *const choice_theme_yes_no[] = { "theme", "yes", "no", NULL };
static const char *const choice_current_all[] = { "current", "all", NULL };
static const char *const choice_close_new[] = { "close", "new", NULL };
static const char *const choice_text_graph[] = { "text", "graph", NULL };

static const struct opt opts_clock[] = {
	{ "format", "Format", "strftime format, \\n starts a second line. The theme decides by default.", NULL },
	{ "tooltip_format", "Tooltip format", "strftime format of the tooltip", NULL },
	{ 0 },
};
static const struct opt opts_cpu[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{usage} is the load in percent", NULL },
	{ "style", "Style", NULL, choice_text_graph },
	{ "width", "Graph width", "Pixels", NULL },
	{ 0 },
};
static const struct opt opts_memory[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "format", "Format", "{used_percent}, {used} and {total} in GiB", NULL },
	{ 0 },
};
static const struct opt opts_battery[] = {
	{ "interval", "Update interval", "Seconds, default 30", NULL },
	{ "format", "Format", "{capacity} is the charge in percent", NULL },
	{ "device", "Device", "Name in /sys/class/power_supply, e.g. BAT0", NULL },
	{ 0 },
};
static const struct opt opts_network[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "interface", "Interface", "e.g. wlan0; detected automatically if empty", NULL },
	{ 0 },
};
static const struct opt opts_volume[] = {
	{ "mixer", "Mixer command", "Runs on click, default exec pavucontrol", NULL },
	{ "step", "Scroll step", "Percent, default 5", NULL },
	{ "format", "Format", NULL, NULL },
	{ 0 },
};
static const struct opt opts_taskbar[] = {
	{ "icons_only", "Icons only", "\"theme\" follows the theme (Windows 7 and 11 show icons only)", choice_theme_yes_no },
	{ "group", "Combine windows of the same app", NULL, choice_yes_no },
	{ "workspaces", "Show windows of", "The current workspace or all workspaces", choice_current_all },
	{ "outputs", "Show windows on", "The current monitor or all monitors", choice_current_all },
	{ "middle_click", "Middle click", "Close the window or start a new one", choice_close_new },
	{ "max_width", "Maximum button width", "Pixels", NULL },
	{ "button_width", "Button width", "Pixels", NULL },
	{ 0 },
};
static const struct opt opts_search[] = {
	{ "label", "Placeholder text", NULL, NULL },
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct opt opts_start[] = {
	{ "label", "Label", "The theme decides by default", NULL },
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct opt opts_title[] = {
	{ "max_width", "Maximum width", "Pixels, default 480", NULL },
	{ 0 },
};
static const struct opt opts_width[] = {
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct opt opts_custom[] = {
	{ "exec", "Command", "Shell command whose output is shown", NULL },
	{ "interval", "Interval", "Seconds between runs; 0 runs it once", NULL },
	{ "exec_listen", "Streaming command", "Instead of Command: keeps running and prints one line (or JSON) per update", NULL },
	{ "format", "Format", "{} is replaced with the output", NULL },
	{ "icon", "Icon", "Icon name or path", NULL },
	{ 0 },
};
static const struct opt opts_events[] = {
	{ "on_click", "On click", "Command, e.g. exec pavucontrol", NULL },
	{ "on_middle_click", "On middle click", NULL, NULL },
	{ "on_right_click", "On right click", "Replaces the right-click menu", NULL },
	{ "on_scroll_up", "On scroll up", NULL, NULL },
	{ "on_scroll_down", "On scroll down", NULL, NULL },
	{ 0 },
};

static const struct {
	const char *type;
	const struct opt *opts;
} type_opts[] = {
	{ "clock", opts_clock },
	{ "cpu", opts_cpu },
	{ "memory", opts_memory },
	{ "battery", opts_battery },
	{ "network", opts_network },
	{ "volume", opts_volume },
	{ "taskbar", opts_taskbar },
	{ "search", opts_search },
	{ "start", opts_start },
	{ "title", opts_title },
	{ "separator", opts_width },
	{ "spacer", opts_width },
	{ "showdesktop", opts_width },
	{ "custom", opts_custom },
};

struct taskbar_page {
	struct settings *s;
	bool updating;
	GtkWidget *layout_dd, *position_dd, *height_spin;
	GtkWidget *sections[SECTION_COUNT];
	GtkWidget *widget_dd;
	GPtrArray *widget_names;
	GtkWidget *options;
	GtkWidget *custom_entry, *custom_popover;
	GtkWidget *font_entry, *terminal_entry, *delay_spin;
	struct app_list *quick;
	GHashTable *quick_icons;
	guint rebuild_id;
	char *select_name;
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

static char *widget_type_of(const char *name) {
	const char *colon = strchr(name, ':');
	return colon ? g_strndup(name, colon - name) : g_strdup(name);
}

static char *widget_title(const char *name, const char **description) {
	const char *colon = strchr(name, ':');
	char *type = widget_type_of(name);
	char *title = NULL;
	if (description) {
		*description = NULL;
	}
	if (strcmp(type, "custom") == 0) {
		title = g_strdup_printf("Script: %s", colon ? colon + 1 : name);
		if (description) {
			*description = "Shows the output of a command";
		}
	}
	for (size_t i = 0; !title && i < G_N_ELEMENTS(widget_types); i++) {
		if (strcmp(widget_types[i].type, type) == 0) {
			title = colon ? g_strdup_printf("%s (%s)", widget_types[i].title, colon + 1) :
				g_strdup(widget_types[i].title);
			if (description) {
				*description = widget_types[i].description;
			}
		}
	}
	g_free(type);
	return title ? title : g_strdup(name);
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

/* ---------- widget options ---------- */

static const char *selected_widget(struct taskbar_page *p) {
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->widget_dd));
	return p->widget_names && i < p->widget_names->len ? p->widget_names->pdata[i] : NULL;
}

struct opt_binding {
	struct taskbar_page *p;
	char *widget;
	const struct opt *opt;
};

static void opt_binding_free(gpointer data, GClosure *closure) {
	struct opt_binding *b = data;
	g_free(b->widget);
	g_free(b);
}

static void write_option(struct taskbar_page *p, const char *widget, const char *key,
		const char *value) {
	struct confdoc *d = doc(p);
	struct cstmt *block = confdoc_block(d, "widget", widget, value != NULL);
	if (!block) {
		return;
	}
	if (value) {
		char *quoted = conf_quote_command(value);
		confdoc_set(d, block, key, NULL, quoted);
		g_free(quoted);
	} else {
		confdoc_set(d, block, key, NULL, NULL);
	}
	settings_taskbar_changed(p->s);
}

static void on_option_text(GtkEditable *editable, gpointer data) {
	struct opt_binding *b = data;
	if (b->p->updating) {
		return;
	}
	char *value = ui_input_value(gtk_editable_get_text(editable));
	write_option(b->p, b->widget, b->opt->key, *value ? value : NULL);
	g_free(value);
}

static void on_option_choice(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct opt_binding *b = data;
	if (b->p->updating) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	write_option(b->p, b->widget, b->opt->key, i == 0 ? NULL : b->opt->choices[i - 1]);
}

static void add_option_row(struct taskbar_page *p, const char *widget, const struct opt *opt) {
	struct cstmt *block = confdoc_block(doc(p), "widget", widget, false);
	char *value = cstmt_join(confdoc_child(block, opt->key, NULL), 0);
	struct opt_binding *b = g_new0(struct opt_binding, 1);
	b->p = p;
	b->widget = g_strdup(widget);
	b->opt = opt;
	GtkWidget *control;
	if (opt->choices) {
		GtkStringList *model = gtk_string_list_new(NULL);
		gtk_string_list_append(model, "Default");
		guint sel = 0;
		for (guint i = 0; opt->choices[i]; i++) {
			gtk_string_list_append(model, opt->choices[i]);
			if (value && g_ascii_strcasecmp(value, opt->choices[i]) == 0) {
				sel = i + 1;
			}
		}
		control = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(control), sel);
		g_signal_connect_data(control, "notify::selected", G_CALLBACK(on_option_choice), b,
			opt_binding_free, 0);
	} else {
		control = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(control), "Default");
		gtk_widget_set_size_request(control, 300, -1);
		char *display = ui_display_value(value);
		gtk_editable_set_text(GTK_EDITABLE(control), display);
		g_free(display);
		g_signal_connect_data(control, "changed", G_CALLBACK(on_option_text), b,
			opt_binding_free, 0);
	}
	ui_row(p->options, opt->title, opt->hint, control);
	g_free(value);
}

static void on_delete_custom(GtkButton *button, gpointer data) {
	struct taskbar_page *p = data;
	const char *selected = selected_widget(p);
	if (!selected) {
		return;
	}
	char *name = g_strdup(selected);
	struct confdoc *d = doc(p);
	struct cstmt *block = confdoc_block(d, "widget", name, false);
	if (block) {
		confdoc_remove(d, block);
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

static void rebuild_options(struct taskbar_page *p) {
	gtk_list_box_remove_all(GTK_LIST_BOX(p->options));
	const char *name = selected_widget(p);
	if (!name) {
		return;
	}
	char *type = widget_type_of(name);
	for (size_t i = 0; i < G_N_ELEMENTS(type_opts); i++) {
		if (strcmp(type_opts[i].type, type) == 0) {
			for (const struct opt *opt = type_opts[i].opts; opt->key; opt++) {
				add_option_row(p, name, opt);
			}
		}
	}
	if (strcmp(type, "quicklaunch") == 0) {
		ui_row(p->options, "Apps", "Edit the apps under Quick launch below.", NULL);
	}
	for (const struct opt *opt = opts_events; opt->key; opt++) {
		add_option_row(p, name, opt);
	}
	if (strcmp(type, "custom") == 0) {
		GtkWidget *button = gtk_button_new_with_label("Delete widget");
		gtk_widget_add_css_class(button, "destructive-action");
		g_signal_connect(button, "clicked", G_CALLBACK(on_delete_custom), p);
		ui_row(p->options, "Delete this script widget", "Also removes it from both layouts.",
			button);
	}
	g_free(type);
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
	for (size_t i = 0; i < G_N_ELEMENTS(widget_types); i++) {
		if (!array_has(names, widget_types[i].type)) {
			g_ptr_array_add(names, g_strdup(widget_types[i].type));
		}
	}
	return names;
}

static void on_widget_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct taskbar_page *p = data;
	if (!p->updating) {
		rebuild_options(p);
	}
}

static void rebuild_widget_dd(struct taskbar_page *p) {
	char *selected = p->select_name ? p->select_name : g_strdup(selected_widget(p));
	p->select_name = NULL;
	if (p->widget_names) {
		g_ptr_array_unref(p->widget_names);
	}
	p->widget_names = known_widgets(p);
	GtkStringList *model = gtk_string_list_new(NULL);
	guint sel = 0;
	for (guint i = 0; i < p->widget_names->len; i++) {
		const char *name = p->widget_names->pdata[i];
		char *title = widget_title(name, NULL);
		gtk_string_list_append(model, title);
		g_free(title);
		if (selected && strcmp(selected, name) == 0) {
			sel = i;
		}
	}
	bool updating = p->updating;
	p->updating = true;
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->widget_dd), G_LIST_MODEL(model));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->widget_dd), sel);
	p->updating = updating;
	g_object_unref(model);
	g_free(selected);
	rebuild_options(p);
}

static void select_widget(struct taskbar_page *p, const char *name) {
	for (guint i = 0; p->widget_names && i < p->widget_names->len; i++) {
		if (strcmp(p->widget_names->pdata[i], name) == 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->widget_dd), i);
			gtk_widget_grab_focus(p->widget_dd);
			return;
		}
	}
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
	g_free(p->select_name);
	p->select_name = name;
	schedule_rebuild(p);
}

/* ---------- layout sections ---------- */

enum {
	OP_UP,
	OP_DOWN,
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
	gpointer *d = names->pdata;
	gpointer tmp;
	switch (a->op) {
	case OP_UP:
		if (i > 0) {
			tmp = d[i];
			d[i] = d[i - 1];
			d[i - 1] = tmp;
			write_section(p, a->section, names);
		}
		break;
	case OP_DOWN:
		if (i + 1 < names->len) {
			tmp = d[i];
			d[i] = d[i + 1];
			d[i + 1] = tmp;
			write_section(p, a->section, names);
		}
		break;
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
		select_widget(p, name);
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

static void rebuild_sections(struct taskbar_page *p) {
	for (int s = 0; s < SECTION_COUNT; s++) {
		GtkWidget *list = p->sections[s];
		gtk_list_box_remove_all(GTK_LIST_BOX(list));
		GPtrArray *names = read_section(p, s);
		for (guint i = 0; i < names->len; i++) {
			const char *name = names->pdata[i];
			char *title = widget_title(name, NULL);
			GtkWidget *row = ui_row(list, title, name, NULL);
			GtkWidget *box = ui_row_box(row);
			add_action_button(p, box, "go-up-symbolic", "Move up", i > 0, s, i, OP_UP);
			add_action_button(p, box, "go-down-symbolic", "Move down", i + 1 < names->len, s, i,
				OP_DOWN);
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
	refresh_general(p);
	p->updating = false;
	rebuild_sections(p);
	rebuild_widget_dd(p);
}

void taskbar_page_refresh(struct settings *s) {
	if (s->taskbar_page) {
		rebuild_all(s->taskbar_page);
	}
}

/* ---------- signal handlers for the static controls ---------- */

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

	for (int i = 0; i < SECTION_COUNT; i++) {
		char *title = g_strdup_printf("%s section", section_titles[i]);
		p->sections[i] = ui_group(content, title, i == SECTION_LEFT ?
			"The Windows 11 theme centers the left section." : NULL);
		g_free(title);
	}

	GtkWidget *options_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *options_title = gtk_label_new("Widget settings");
	gtk_widget_add_css_class(options_title, "tw-heading");
	gtk_widget_set_hexpand(options_title, TRUE);
	gtk_label_set_xalign(GTK_LABEL(options_title), 0);
	gtk_box_append(GTK_BOX(options_header), options_title);
	p->widget_dd = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->widget_dd, "notify::selected", G_CALLBACK(on_widget_selected), p);
	gtk_box_append(GTK_BOX(options_header), p->widget_dd);

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
	gtk_box_append(GTK_BOX(options_header), custom_button);
	gtk_widget_set_margin_top(options_header, 14);
	gtk_box_append(GTK_BOX(content), options_header);
	p->options = ui_group(content, NULL, NULL);

	p->quick = ui_app_list_new(content, "Quick launch",
		"Apps shown by the quick launch widget.", on_quick_changed, p);

	s->taskbar_page = p;
	rebuild_all(p);
	return page;
}
