#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_disks.h"
#include "tw_widgets.h"

/*
 * The settings window of a widget, opened from the Taskbar page and from the
 * Desktop page. Its rows come from the widget list (common/tw_widgets.c): the
 * options of the widget go to its "widget <name>" block, shared by the taskbar
 * and the desktop, and the options of its place on the desktop to its entry in
 * "desktop_widgets".
 */

static void list_disks(GPtrArray *values, GPtrArray *labels) {
	g_ptr_array_add(labels, g_strdup("Every disk"));
	struct tw_disk *disks;
	size_t count = tw_disks_list(TW_BLOCK_DIR, &disks);
	for (size_t i = 0; i < count; i++) {
		g_ptr_array_add(values, g_strdup(disks[i].name));
		g_ptr_array_add(labels, tw_disk_label(&disks[i]));
	}
	tw_disks_free(disks, count);
}

/*
 * Options picked from a list found when the dialog opens instead of a fixed
 * one. The list fills in the values and one more label: the first label
 * stands for the default.
 */
static const struct {
	const char *type, *key;
	void (*list)(GPtrArray *values, GPtrArray *labels);
} listed_opts[] = {
	{ "disk", "devices", list_disks },
};

/* The screens a desktop widget can go on: the main one, all of them, or one by name. */
static void list_desktop_screens(GPtrArray *values, GPtrArray *labels) {
	g_ptr_array_add(labels, g_strdup("The main display"));
	g_ptr_array_add(values, g_strdup("all"));
	g_ptr_array_add(labels, g_strdup("Every screen"));
	GPtrArray *screens = tw_ipc_screens();
	for (guint i = 0; i < screens->len; i++) {
		const struct tw_screen *screen = screens->pdata[i];
		g_ptr_array_add(values, g_strdup(screen->name));
		g_ptr_array_add(labels, g_strdup(screen->label));
	}
	g_ptr_array_unref(screens);
}

char *widget_type_of(const char *name) {
	const char *colon = strchr(name, ':');
	return colon ? g_strndup(name, colon - name) : g_strdup(name);
}

char *widget_title(const char *name, const char **description) {
	const char *colon = strchr(name, ':');
	char *type = widget_type_of(name);
	char *title = NULL;
	if (description) {
		*description = NULL;
	}
	const struct tw_widget_info *info = tw_widget_find(type);
	if (info && strcmp(type, "custom") == 0) {
		title = g_strdup_printf("%s: %s", info->title, colon ? colon + 1 : name);
	} else if (info) {
		title = colon ? g_strdup_printf("%s (%s)", info->title, colon + 1) : g_strdup(info->title);
	}
	if (info && description) {
		*description = info->description;
	}
	g_free(type);
	return title ? title : g_strdup(name);
}

struct cstmt *widget_desktop_entry(struct settings *s, const char *widget) {
	struct cstmt *block = confdoc_block(s->taskbar, "desktop_widgets", NULL, false);
	struct cstmt *entry = block ? confdoc_child(block, widget, NULL) : NULL;
	return entry && entry->children ? entry : NULL;
}

/* ---------- one option, one row ---------- */

struct opt_binding {
	struct settings *s;
	char *widget;
	bool desktop; // an option of its entry in desktop_widgets, not of its widget block
	const struct tw_widget_option *opt;
	GPtrArray *values; // of a dropdown, NULL stands for the default
};

static void opt_binding_free(gpointer data, GClosure *closure) {
	struct opt_binding *b = data;
	g_free(b->widget);
	if (b->values) {
		g_ptr_array_unref(b->values);
	}
	g_free(b);
}

static void write_option(struct settings *s, const char *widget, const char *key,
		const char *value) {
	struct confdoc *d = s->taskbar;
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
	settings_taskbar_changed(s);
}

static void write_desktop_option(struct settings *s, const char *widget, const char *key,
		const char *value) {
	struct cstmt *entry = widget_desktop_entry(s, widget);
	if (!entry) {
		return;
	}
	char *quoted = value ? conf_quote_command(value) : NULL;
	confdoc_set(s->taskbar, entry, key, NULL, quoted);
	g_free(quoted);
	settings_taskbar_changed(s);
	desktop_page_refresh(s);
}

static void write_bound(struct opt_binding *b, const char *value) {
	if (b->desktop) {
		write_desktop_option(b->s, b->widget, b->opt->key, value);
	} else {
		write_option(b->s, b->widget, b->opt->key, value);
	}
}

static void on_option_text(GtkEditable *editable, gpointer data) {
	struct opt_binding *b = data;
	char *value = ui_input_value(gtk_editable_get_text(editable));
	write_bound(b, *value ? value : NULL);
	g_free(value);
}

static void on_option_choice(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct opt_binding *b = data;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	write_bound(b, i < b->values->len ? b->values->pdata[i] : NULL);
}

static void (*opt_lister(const char *widget, const struct tw_widget_option *opt,
		bool desktop))(GPtrArray *, GPtrArray *) {
	if (desktop) {
		return strcmp(opt->key, "output") == 0 ? list_desktop_screens : NULL;
	}
	char *type = widget_type_of(widget);
	void (*list)(GPtrArray *, GPtrArray *) = NULL;
	for (size_t i = 0; i < G_N_ELEMENTS(listed_opts) && !list; i++) {
		if (strcmp(listed_opts[i].type, type) == 0 && strcmp(listed_opts[i].key, opt->key) == 0) {
			list = listed_opts[i].list;
		}
	}
	g_free(type);
	return list;
}

/* The looks of the widget on the desktop, the default one first. */
static bool list_styles(const char *widget, GPtrArray *values, GPtrArray *labels) {
	char *type = widget_type_of(widget);
	const struct tw_widget_info *info = tw_widget_find(type);
	g_free(type);
	if (!info || !info->styles) {
		return false;
	}
	g_ptr_array_add(labels, g_strdup(info->styles[0].title));
	for (const struct tw_widget_style *style = info->styles + 1; style->name; style++) {
		g_ptr_array_add(values, g_strdup(style->name));
		g_ptr_array_add(labels, g_strdup(style->title));
	}
	return true;
}

static void add_option_row(struct settings *s, GtkWidget *list, const char *widget,
		const struct tw_widget_option *opt, bool desktop) {
	void (*lister)(GPtrArray *, GPtrArray *) = opt_lister(widget, opt, desktop);
	bool styles = desktop && strcmp(opt->key, "style") == 0;
	struct cstmt *block = desktop ? widget_desktop_entry(s, widget) :
		confdoc_block(s->taskbar, "widget", widget, false);
	char *value = cstmt_join(confdoc_child(block, opt->key, NULL), 0);
	struct opt_binding *b = g_new0(struct opt_binding, 1);
	b->s = s;
	b->widget = g_strdup(widget);
	b->desktop = desktop;
	b->opt = opt;
	GtkWidget *control;
	if (opt->choices || lister || styles) {
		b->values = g_ptr_array_new_with_free_func(g_free);
		GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(b->values, NULL);
		if (styles) {
			list_styles(widget, b->values, labels);
		} else if (lister) {
			lister(b->values, labels);
		} else {
			g_ptr_array_add(labels, g_strdup("Default"));
			for (guint i = 0; opt->choices[i]; i++) {
				g_ptr_array_add(b->values, g_strdup(opt->choices[i]));
				g_ptr_array_add(labels, g_strdup(opt->choices[i]));
			}
		}
		guint sel = 0;
		for (guint i = 1; value && i < b->values->len; i++) {
			if (g_ascii_strcasecmp(value, b->values->pdata[i]) == 0) {
				sel = i;
			}
		}
		if (value && *value && sel == 0 && !(styles && labels->len &&
				g_ascii_strcasecmp(value, "compact") == 0)) {
			// a value written by hand, or a disk that is not plugged in, stays offered
			g_ptr_array_add(b->values, g_strdup(value));
			g_ptr_array_add(labels, g_strdup(value));
			sel = b->values->len - 1;
		}
		GtkStringList *model = gtk_string_list_new(NULL);
		for (guint i = 0; i < labels->len; i++) {
			gtk_string_list_append(model, labels->pdata[i]);
		}
		g_ptr_array_unref(labels);
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
	ui_row(list, opt->title, opt->hint, control);
	g_free(value);
}

/* ---------- the dialog ---------- */

struct delete_request {
	struct settings *s;
	char *name;
	GtkWidget *window;
};

static void delete_request_free(gpointer data, GClosure *closure) {
	struct delete_request *r = data;
	g_free(r->name);
	g_free(r);
}

static void on_dialog_delete(GtkButton *button, gpointer data) {
	struct delete_request *r = data;
	struct settings *s = r->s;
	char *name = g_strdup(r->name);
	GtkWidget *window = r->window;
	taskbar_delete_script(s, name);
	g_free(name);
	gtk_window_destroy(GTK_WINDOW(window)); // frees r
}

static void on_dialog_close(GtkButton *button, gpointer data) {
	gtk_window_destroy(GTK_WINDOW(data));
}

void widget_dialog_open(struct settings *s, const char *name, bool desktop) {
	const char *description;
	char *title = widget_title(name, &description);
	char *type = widget_type_of(name);

	GtkWidget *window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(window), s->window);
	gtk_window_set_modal(GTK_WINDOW(window), TRUE);
	gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
	char *window_title = g_strdup_printf("%s settings", title);
	gtk_window_set_title(GTK_WINDOW(window), window_title);
	g_free(window_title);
	gtk_window_set_default_size(GTK_WINDOW(window), 640, 660);

	char *subtitle = description ? g_strdup_printf("%s (%s)", description, name) : g_strdup(name);
	GtkWidget *content;
	GtkWidget *page = ui_page(title, subtitle, &content);
	g_free(subtitle);
	gtk_widget_set_vexpand(page, TRUE);

	if (desktop) {
		GtkWidget *place = ui_group(content, "On the desktop",
			"How it looks and which cells of the icon grid it takes. Dragging it on the "
			"desktop moves it too.");
		for (const struct tw_widget_option *opt = tw_widget_desktop_options; opt->key; opt++) {
			if (strcmp(opt->key, "seconds") == 0 && strcmp(type, "clock") != 0) {
				continue; // only clocks have seconds
			}
			add_option_row(s, place, name, opt, true);
		}
	}
	GtkWidget *list = ui_group(content, "Settings", desktop ?
		"Shared with the same widget on the taskbar. Leave a field empty to use the default." :
		"Leave a field empty to use the default.");
	int count = 0;
	const struct tw_widget_info *info = tw_widget_find(type);
	for (const struct tw_widget_option *opt = info ? info->options : NULL; opt && opt->key; opt++) {
		add_option_row(s, list, name, opt, false);
		count++;
	}
	if (strcmp(type, "quicklaunch") == 0) {
		ui_row(list, "Apps", "Edit the apps under Quick launch on the Taskbar page.", NULL);
		count++;
	}
	if (count == 0) {
		ui_row(list, NULL, "This widget has no settings of its own.", NULL);
	}

	GtkWidget *events = ui_group(content, "Mouse actions",
		"Commands run when the widget is clicked or scrolled, e.g. exec pavucontrol. "
		"On right click replaces the widget's menu.");
	for (const struct tw_widget_option *opt = tw_widget_events; opt->key; opt++) {
		add_option_row(s, events, name, opt, false);
	}

	if (strcmp(type, "custom") == 0) {
		GtkWidget *danger = ui_group(content, "Script widget", NULL);
		GtkWidget *button = gtk_button_new_with_label("Delete widget");
		gtk_widget_add_css_class(button, "destructive-action");
		struct delete_request *r = g_new0(struct delete_request, 1);
		r->s = s;
		r->name = g_strdup(name);
		r->window = window;
		g_signal_connect_data(button, "clicked", G_CALLBACK(on_dialog_delete), r,
			delete_request_free, 0);
		ui_row(danger, "Delete this script widget",
			"It is also removed from both layouts and from the desktop.", button);
	}

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), page);
	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	GtkWidget *close = gtk_button_new_with_label("Close");
	gtk_widget_add_css_class(close, "suggested-action");
	gtk_widget_set_halign(close, GTK_ALIGN_END);
	gtk_widget_set_margin_top(close, 10);
	gtk_widget_set_margin_bottom(close, 10);
	gtk_widget_set_margin_end(close, 16);
	g_signal_connect(close, "clicked", G_CALLBACK(on_dialog_close), window);
	gtk_box_append(GTK_BOX(box), close);
	gtk_window_set_child(GTK_WINDOW(window), box);
	gtk_window_set_default_widget(GTK_WINDOW(window), close);
	gtk_window_present(GTK_WINDOW(window));

	g_free(type);
	g_free(title);
}
