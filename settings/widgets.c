#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_desktop.h"
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
static void list_seconds(GPtrArray *values, GPtrArray *labels) {
	static const int n[] = { 1, 2, 3, 5, 10, 15, 30, 60, 120, 300, -1 };
	ui_list_numbers(values, labels, "Every %d seconds", n);
}

static void list_pixels(GPtrArray *values, GPtrArray *labels) {
	static const int n[] = { 8, 16, 24, 32, 48, 64, 80, 100, 120, 160, 200, 240, 300, -1 };
	ui_list_numbers(values, labels, "%d pixels", n);
}

static void list_percent(GPtrArray *values, GPtrArray *labels) {
	static const int n[] = { 50, 60, 70, 75, 80, 85, 90, 95, -1 };
	ui_list_numbers(values, labels, "%d%%", n);
}

static void list_step(GPtrArray *values, GPtrArray *labels) {
	static const int n[] = { 1, 2, 5, 10, 20, -1 };
	ui_list_numbers(values, labels, "%d%% a notch", n);
}

static void list_kib(GPtrArray *values, GPtrArray *labels) {
	static const int n[] = { 10, 50, 100, 500, 1000, 5000, -1 };
	ui_list_numbers(values, labels, "%d KiB per second", n);
}

static void list_task_managers(GPtrArray *values, GPtrArray *labels) {
	ui_list_apps("Monitor", true, values, labels);
}

static void list_mixers(GPtrArray *values, GPtrArray *labels) {
	ui_list_apps("Mixer", true, values, labels);
}

static void list_calendars(GPtrArray *values, GPtrArray *labels) {
	g_ptr_array_add(values, g_strdup("exec tilewin-settings --page datetime"));
	g_ptr_array_add(labels, g_strdup("Date & time settings"));
	ui_list_apps("Calendar", true, values, labels);
}

/* The options to pick from: by the key alone, or by the widget and key. */
static const struct {
	const char *type, *key; // a NULL type: every widget
	void (*list)(GPtrArray *values, GPtrArray *labels);
} listed_opts[] = {
	{ "disk", "devices", list_disks },
	{ "gpu", "device", ui_list_gpus },
	{ "netspeed", "device", ui_list_net_devices },
	{ "network", "interface", ui_list_net_devices },
	{ "battery", "device", ui_list_batteries },
	{ "power", "device", ui_list_batteries },
	{ "clock", "settings", list_calendars },
	{ NULL, "interval", list_seconds },
	{ NULL, "width", list_pixels },
	{ NULL, "max_width", list_pixels },
	{ NULL, "button_width", list_pixels },
	{ NULL, "warning", list_percent },
	{ NULL, "critical", list_percent },
	{ NULL, "step", list_step },
	{ NULL, "threshold", list_kib },
	{ NULL, "task_manager", list_task_managers },
	{ NULL, "mixer", list_mixers },
};

/*
 * Formats to pick from, by widget and key; the label shows what comes out.
 * "\\n" is how a second line looks in the field.
 */
static const struct {
	const char *type, *key;
	const char *const values[8];
	const char *const labels[8];
} format_presets[] = {
	{ "cpu", "format", { "{usage}%", "CPU {usage}%", "{usage}" },
		{ "12%", "CPU 12%", "12" } },
	{ "memory", "format", { "{used_percent}%", "RAM {used_percent}%", "{used}/{total} GiB",
		"{used} GiB" }, { "45%", "RAM 45%", "7.1/15.4 GiB", "7.1 GiB" } },
	{ "gpu", "format", { "{usage}%", "GPU {usage}%" }, { "30%", "GPU 30%" } },
	{ "gpu", "command", { "nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits" },
		{ "NVIDIA (nvidia-smi)" } },
	{ "netspeed", "format", { "\u2193{down} \u2191{up}", "{total}", "{device} \u2193{down} \u2191{up}" },
		{ "\u2193 1.2 MB/s \u2191 40 kB/s", "1.2 MB/s", "wlan0 \u2193 1.2 MB/s \u2191 40 kB/s" } },
	{ "diskspace", "format", { "{used_percent}%", "{free} GiB free", "{used}/{total} GiB",
		"{path} {used_percent}%" }, { "62%", "180 GiB free", "300/480 GiB", "/ 62%" } },
	{ "power", "format", { "{watts} W", "{watts}W" }, { "9.5 W", "9.5W" } },
	{ "battery", "format", { "{capacity}%", "{icon} {capacity}%", "BAT {capacity}%" },
		{ "80%", "(icon) 80%", "BAT 80%" } },
	{ "network", "format", { "{essid}", "{essid} {quality}%", "{ifname}" },
		{ "HomeWifi", "HomeWifi 70%", "wlan0" } },
	{ "volume", "format", { "{volume}%", "{icon} {volume}%", "VOL {volume}%" },
		{ "60%", "(icon) 60%", "VOL 60%" } },
	{ "volume", "format_muted", { "muted", "{icon} muted" }, { "muted", "(icon) muted" } },
	{ "brightness", "format", { "{percent}%", "{icon} {percent}%" }, { "70%", "(icon) 70%" } },
	{ "clock", "format", { "%H:%M", "%H:%M:%S", "%I:%M %p", "%H:%M\\n%d.%m.%Y",
		"%H:%M\\n%Y-%m-%d", "%a %d %b  %H:%M", "%d.%m.%Y  %H:%M" }, { NULL } },
	{ "clock", "tooltip_format", { "%A, %d %B %Y", "%d.%m.%Y", "%Y-%m-%d", "%A %H:%M" },
		{ NULL } },
};

static bool color_key(const char *key) {
	size_t len = strlen(key);
	return strcmp(key, "fg") == 0 || (len > 3 && strcmp(key + len - 3, "_fg") == 0) ||
		strstr(key, "color") != NULL;
}

/* A text option as presets: what fits the key, at least Default and Custom. */
static GtkWidget *text_option_control(const char *widget, const char *key, GtkWidget *entry) {
	if (color_key(key)) {
		return ui_color_field(entry);
	}
	if (strcmp(key, "path") == 0) {
		return ui_folder_field(entry, "Folder to watch");
	}
	char *type = widget_type_of(widget);
	GPtrArray *values = ui_strings(), *labels = ui_strings();
	g_ptr_array_add(values, g_strdup(""));
	g_ptr_array_add(labels, g_strdup("Default"));
	for (size_t i = 0; i < G_N_ELEMENTS(format_presets); i++) {
		if (strcmp(format_presets[i].type, type) != 0 || strcmp(format_presets[i].key, key) != 0) {
			continue;
		}
		for (int v = 0; v < 8 && format_presets[i].values[v]; v++) {
			const char *value = format_presets[i].values[v];
			g_ptr_array_add(values, g_strdup(value));
			if (format_presets[i].labels[v]) {
				g_ptr_array_add(labels, g_strdup(format_presets[i].labels[v]));
				continue;
			}
			// a strftime format: what it shows now, its lines joined
			char *format = g_strdup(value);
			for (char *n; (n = strstr(format, "\\n"));) {
				n[0] = ' ';
				n[1] = '/';
			}
			GDateTime *now = g_date_time_new_now_local();
			char *shown = g_date_time_format(now, format);
			g_ptr_array_add(labels, shown ? shown : g_strdup(value));
			g_date_time_unref(now);
			g_free(format);
		}
	}
	g_free(type);
	return ui_presets(entry, values, labels);
}

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
	// the ones of this widget first, then those of every widget
	for (int pass = 0; pass < 2 && !list; pass++) {
		for (size_t i = 0; i < G_N_ELEMENTS(listed_opts) && !list; i++) {
			bool mine = listed_opts[i].type && strcmp(listed_opts[i].type, type) == 0;
			if ((pass == 0 ? mine : !listed_opts[i].type) &&
					strcmp(listed_opts[i].key, opt->key) == 0) {
				list = listed_opts[i].list;
			}
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
		control = text_option_control(widget, opt->key, control);
	}
	ui_row(list, opt->title, opt->hint, control);
	g_free(value);
}

/* ---------- the apps of the quick launch ---------- */

/* The list of the quick launch widget, in its settings: what widget
 * quicklaunch { item ID [ICON] } says in taskbar.conf, edited as the Start
 * menu's pinned apps are. Its own for every dialog, gone with it. */
struct quick_editor {
	struct settings *s;
	struct app_list *list;
	GHashTable *icons; // id -> icon of its own
};

static GtkWindow *quick_window(struct quick_editor *q) {
	GtkRoot *root = gtk_widget_get_root(q->list->list);
	return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : q->s->window;
}

static void on_quick_changed(struct app_list *list, gpointer data) {
	struct quick_editor *q = data;
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < list->ids->len; i++) {
		const char *id = list->ids->pdata[i];
		const char *icon = g_hash_table_lookup(q->icons, id);
		char *qid = conf_quote(id);
		char *qicon = icon ? conf_quote(icon) : NULL;
		g_ptr_array_add(lines, qicon ? g_strdup_printf("%s %s", qid, qicon) : g_strdup(qid));
		g_free(qid);
		g_free(qicon);
	}
	struct cstmt *block = confdoc_block(q->s->taskbar, "widget", "quicklaunch", true);
	confdoc_set_list(q->s->taskbar, block, "item", NULL, lines);
	g_ptr_array_unref(lines);
	settings_taskbar_changed(q->s);
}

struct icon_request {
	struct quick_editor *q;
	char *id;
};

static void on_icon_chosen(const char *icon, gpointer data) {
	struct icon_request *r = data;
	if (r->q->list) {
		if (icon) {
			g_hash_table_insert(r->q->icons, g_strdup(r->id), g_strdup(icon));
		} else {
			g_hash_table_remove(r->q->icons, r->id);
		}
		on_quick_changed(r->q->list, r->q);
		ui_app_list_refresh(r->q->list);
		settings_status(r->q->s, icon ? "Icon changed" : "Using the app's own icon");
	}
	g_free(r->id);
	g_free(r);
}

static const char *quick_row_icon(struct app_list *l, guint index, gpointer data) {
	struct quick_editor *q = data;
	return index < l->ids->len ? g_hash_table_lookup(q->icons, l->ids->pdata[index]) : NULL;
}

static void on_quick_icon(struct app_list *l, guint index, gpointer data) {
	struct quick_editor *q = data;
	if (index >= l->ids->len) {
		return;
	}
	struct icon_request *r = g_new0(struct icon_request, 1);
	r->q = q;
	r->id = g_strdup(l->ids->pdata[index]);
	const struct tw_desktop_entry *e = ui_find_app(r->id);
	ui_icon_dialog(quick_window(q), e ? e->name : r->id,
		"The icon shown in the taskbar: an icon name of your icon theme, e.g. firefox, "
		"or an image file.", g_hash_table_lookup(q->icons, r->id),
		e ? e->icon : NULL, "Use the app's icon", on_icon_chosen, r);
}

static void on_command_chosen(const char *command, gpointer data) {
	struct quick_editor *q = data;
	char *text = command ? g_strstrip(g_strdup(command)) : NULL;
	if (!text || !*text || !q->list) {
		g_free(text);
		return;
	}
	ui_app_list_add(q->list, text); // there already, and its icon is asked for next
	struct icon_request *r = g_new0(struct icon_request, 1);
	r->q = q;
	r->id = text;
	ui_icon_dialog(quick_window(q), "Icon of the command",
		"The icon its button shows: an icon name of your icon theme, e.g. utilities-terminal, "
		"or an image file.", NULL, "utilities-terminal", "No icon of its own",
		on_icon_chosen, r);
}

static void on_quick_command(struct app_list *l, gpointer data) {
	struct quick_editor *q = data;
	ui_command_dialog(quick_window(q), "Quick launch command",
		"A command the button runs, e.g. firefox --private-window or a script of yours.",
		NULL, on_command_chosen, q);
}

/* The dialog is gone: its rows are, so a pending rebuild of them may not run.
 * The editor itself stays for a request still open (an icon being picked). */
static void on_quick_gone(GtkWidget *widget, gpointer data) {
	struct quick_editor *q = data;
	if (q->list->rebuild_id) {
		g_source_remove(q->list->rebuild_id);
	}
	g_ptr_array_unref(q->list->ids);
	g_free(q->list);
	q->list = NULL;
}

static void quick_editor_attach(struct settings *s, GtkWidget *content) {
	struct quick_editor *q = g_new0(struct quick_editor, 1);
	q->s = s;
	q->icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	q->list = ui_app_list_new(content, "Apps", "The apps and commands it shows, in this "
		"order. The button beside one changes its icon.", on_quick_changed, q);
	q->list->commands = true;
	q->list->add_other_label = "Add command…";
	q->list->add_other = on_quick_command;
	q->list->extra = on_quick_icon;
	q->list->extra_icon = "image-x-generic-symbolic";
	q->list->extra_tooltip = "Change the icon";
	q->list->row_icon = quick_row_icon;
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	struct cstmt *block = confdoc_block(s->taskbar, "widget", "quicklaunch", false);
	for (guint i = 0; block && i < block->children->len; i++) {
		struct cstmt *item = block->children->pdata[i];
		if (strcmp(item->name, "item") != 0 || cstmt_argc(item) < 1) {
			continue;
		}
		g_ptr_array_add(ids, g_strdup(cstmt_arg(item, 0)));
		if (cstmt_argc(item) > 1) {
			g_hash_table_insert(q->icons, g_strdup(cstmt_arg(item, 0)),
				g_strdup(cstmt_arg(item, 1)));
		}
	}
	ui_app_list_set(q->list, ids);
	g_ptr_array_unref(ids);
	g_signal_connect(q->list->list, "destroy", G_CALLBACK(on_quick_gone), q);
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
	const struct tw_widget_info *info = tw_widget_find(type);
	bool quick = strcmp(type, "quicklaunch") == 0;
	bool any = info && info->options && info->options->key;
	// the quick launch's settings are its apps, in a group of their own below
	if (any || !quick) {
		GtkWidget *list = ui_group(content, "Settings", desktop ?
			"Shared with the same widget on the taskbar. Leave a field empty to use the default." :
			"Leave a field empty to use the default.");
		for (const struct tw_widget_option *opt = any ? info->options : NULL; opt && opt->key;
				opt++) {
			add_option_row(s, list, name, opt, false);
		}
		if (!any) {
			ui_row(list, NULL, "This widget has no settings of its own.", NULL);
		}
	}
	if (quick) {
		quick_editor_attach(s, content);
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
