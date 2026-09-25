#include <stdlib.h>
#include <string.h>
#include "reorder.h"
#include "settings.h"
#include "tw_desktop.h"
#include "tw_disks.h"

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
	{ "memory", "Memory usage", "RAM in use, click for a flyout" },
	{ "disk", "Disk activity", "A lamp that lights up while the disks are busy" },
	{ "gpu", "GPU usage", "Load of a graphics card" },
	{ "net", "Network usage", "What goes through an interface" },
	{ "storage", "Disk space", "How full a file system is" },
	{ "power", "Power draw", "Watts the computer is drawing" },
	{ "git", "Git", "Branch and changed lines of the repository the focused window works in" },
	{ "clock", "Clock", "Time and date with a calendar" },
	{ "notifications", "Notifications", "Opens the Action Center with the notification history" },
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
static const char *const choice_meter[] = { "text", "graph", "bar", NULL };

static const struct opt opts_clock[] = {
	{ "format", "Format", "strftime format, \\n starts a second line. The theme decides by default.", NULL },
	{ "tooltip_format", "Tooltip format", "strftime format of the tooltip", NULL },
	{ "settings", "Settings link", "Opened by the link in the calendar, default exec tilewin-settings --page datetime", NULL },
	{ 0 },
};
static const struct opt opts_git[] = {
	{ "show_tag", "Show the newest tag", "The tag git describe reaches from HEAD, e.g. v1.0.7",
		choice_yes_no },
	{ "show_untracked", "Count untracked files", "Adds \u201c?3\u201d for files git does not follow yet",
		choice_yes_no },
	{ "interval", "Update interval", "Seconds between checks; 5 by default, and it also "
		"looks whenever another window is focused", NULL },
	{ "icon", "Icon", "Drawn before the branch, e.g. a Nerd Font glyph. Empty by default, "
		"because not every font has one", NULL },
	{ "max_width", "Maximum width", "Pixels; empty or 0 lets it take the room it needs", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default, and the added and removed "
		"counts keep their own colors", NULL },
	{ 0 },
};

static const struct opt opts_cpu[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{usage} is the load in percent", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ "task_manager", "Task manager", "Opened by the link in the flyout, e.g. exec btop", NULL },
	{ 0 },
};
static const struct opt opts_memory[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "format", "Format", "{used_percent}, {used} and {total} in GiB", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ "task_manager", "Task manager", "Opened by the link in the flyout, e.g. exec btop", NULL },
	{ 0 },
};
static const struct opt opts_gpu[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{usage} is the load in percent", NULL },
	{ "device", "Card", "e.g. card0; the first one that reports anything by default", NULL },
	{ "command", "Command", "For cards that report nothing in /sys, e.g. nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct opt opts_net[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{down}, {up}, {total} and {device}", NULL },
	{ "device", "Interface", "e.g. wlan0; the busiest one by default", NULL },
	{ "max_rate", "Full scale", "KiB per second the chart and the bar are drawn against, default 12500", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct opt opts_storage[] = {
	{ "interval", "Update interval", "Seconds, default 30", NULL },
	{ "path", "Folder", "Any folder of the file system to watch, default /", NULL },
	{ "format", "Format", "{used_percent}, {used}, {free}, {total} in GiB and {path}", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct opt opts_power[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "format", "Format", "{watts} is what is being drawn", NULL },
	{ "device", "Battery", "Name in /sys/class/power_supply, e.g. BAT0", NULL },
	{ "max_watts", "Full scale", "Watts the chart and the bar are drawn against, default 60", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
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

static const struct opt opts_disk[] = {
	{ "devices", "Disk", "The disk whose lamp this is; every whole disk by default", NULL },
	{ "threshold", "Threshold", "KiB per second before the lamp lights up, default 50", NULL },
	{ "interval", "Update interval", "Seconds, default 1", NULL },
	{ "format", "Format", "{rate} is e.g. 1.2 MB/s, {kbps} the plain number; empty shows only the lamp", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ 0 },
};
static const struct opt opts_battery[] = {
	{ "interval", "Update interval", "Seconds, default 30", NULL },
	{ "format", "Format", "{capacity} is the charge in percent", NULL },
	{ "format_charging", "Format while charging", "Used instead of Format while the battery charges", NULL },
	{ "format_full", "Format when full", "Used instead of Format once the battery is full", NULL },
	{ "format_plugged", "Format on mains", "Used instead of Format while the charger is plugged in", NULL },
	{ "device", "Device", "Name in /sys/class/power_supply, e.g. BAT0", NULL },
	{ "icons", "Icons", "Characters for {icon}, lowest charge first, separated by spaces", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the battery flyout", choice_yes_no },
	{ "settings", "Settings link", "Opened by the link in the flyout, e.g. exec tilewin-settings --page screen", NULL },
	{ 0 },
};
static const struct opt opts_network[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "interface", "Interface", "e.g. wlan0; detected automatically if empty", NULL },
	{ "format", "Format", "{essid}, {quality} and {ifname}", NULL },
	{ "format_ethernet", "Format on a cable", "Used instead of Format for a wired connection", NULL },
	{ "format_disconnected", "Format when offline", "Used instead of Format while nothing is connected", NULL },
	{ "icons", "Icons", "Characters for {icon}, weakest signal first", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the network flyout", choice_yes_no },
	{ "settings", "Settings link", "Opened by the link in the flyout, default exec nm-connection-editor", NULL },
	{ 0 },
};
static const struct opt opts_volume[] = {
	{ "mixer", "Mixer command", "Runs on click, default exec pavucontrol", NULL },
	{ "step", "Scroll step", "Percent, default 5", NULL },
	{ "format", "Format", "{volume} is the level in percent", NULL },
	{ "format_muted", "Format when muted", "Used instead of Format while the sound is off", NULL },
	{ "icons", "Icons", "Characters for {icon}, quietest first", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the volume flyout", choice_yes_no },
	{ 0 },
};
static const struct opt opts_brightness[] = {
	{ "format", "Format", "{percent} is the brightness", NULL },
	{ "icons", "Icons", "Characters for {icon}, darkest first", NULL },
	{ 0 },
};
static const struct opt opts_notifications[] = {
	{ "always", "Always show the button", "Otherwise it appears only when something is waiting", choice_yes_no },
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
	{ "thumbnails", "Preview on hover", "A live picture of the window above the button", choice_yes_no },
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
	{ "git", opts_git },
	{ "cpu", opts_cpu },
	{ "memory", opts_memory },
	{ "disk", opts_disk },
	{ "gpu", opts_gpu },
	{ "net", opts_net },
	{ "storage", opts_storage },
	{ "power", opts_power },
	{ "battery", opts_battery },
	{ "network", opts_network },
	{ "volume", opts_volume },
	{ "brightness", opts_brightness },
	{ "notifications", opts_notifications },
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

/* ---------- widget options ---------- */

struct opt_binding {
	struct taskbar_page *p;
	char *widget;
	const struct opt *opt;
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
	write_option(b->p, b->widget, b->opt->key, i < b->values->len ? b->values->pdata[i] : NULL);
}

static void (*opt_lister(const char *widget, const struct opt *opt))(GPtrArray *, GPtrArray *) {
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

static void add_option_row(struct taskbar_page *p, GtkWidget *list, const char *widget,
		const struct opt *opt) {
	void (*lister)(GPtrArray *, GPtrArray *) = opt_lister(widget, opt);
	struct cstmt *block = confdoc_block(doc(p), "widget", widget, false);
	char *value = cstmt_join(confdoc_child(block, opt->key, NULL), 0);
	struct opt_binding *b = g_new0(struct opt_binding, 1);
	b->p = p;
	b->widget = g_strdup(widget);
	b->opt = opt;
	GtkWidget *control;
	if (opt->choices || lister) {
		b->values = g_ptr_array_new_with_free_func(g_free);
		GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(b->values, NULL);
		if (lister) {
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
		if (value && *value && sel == 0) {
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

static void delete_custom(struct taskbar_page *p, const char *widget) {
	char *name = g_strdup(widget);
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

/* ---------- widget settings dialog ---------- */

struct name_action {
	struct taskbar_page *p;
	char *name;
	GtkWidget *window; // dialog to close afterwards, may be NULL
};

static struct name_action *name_action_new(struct taskbar_page *p, const char *name,
		GtkWidget *window) {
	struct name_action *a = g_new0(struct name_action, 1);
	a->p = p;
	a->name = g_strdup(name);
	a->window = window;
	return a;
}

static void name_action_free(gpointer data, GClosure *closure) {
	struct name_action *a = data;
	g_free(a->name);
	g_free(a);
}

static void on_dialog_delete(GtkButton *button, gpointer data) {
	struct name_action *a = data;
	struct taskbar_page *p = a->p;
	char *name = g_strdup(a->name);
	GtkWidget *window = a->window;
	delete_custom(p, name);
	g_free(name);
	if (window) {
		gtk_window_destroy(GTK_WINDOW(window)); // frees a
	}
}

static void on_dialog_close(GtkButton *button, gpointer data) {
	gtk_window_destroy(GTK_WINDOW(data));
}

static void open_widget_dialog(struct taskbar_page *p, const char *name) {
	const char *description;
	char *title = widget_title(name, &description);
	char *type = widget_type_of(name);

	GtkWidget *window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(window), p->s->window);
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

	GtkWidget *list = ui_group(content, "Settings", "Leave a field empty to use the default.");
	int count = 0;
	for (size_t i = 0; i < G_N_ELEMENTS(type_opts); i++) {
		if (strcmp(type_opts[i].type, type) == 0) {
			for (const struct opt *opt = type_opts[i].opts; opt->key; opt++) {
				add_option_row(p, list, name, opt);
				count++;
			}
		}
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
	for (const struct opt *opt = opts_events; opt->key; opt++) {
		add_option_row(p, events, name, opt);
	}

	if (strcmp(type, "custom") == 0) {
		GtkWidget *danger = ui_group(content, "Script widget", NULL);
		GtkWidget *button = gtk_button_new_with_label("Delete widget");
		gtk_widget_add_css_class(button, "destructive-action");
		g_signal_connect_data(button, "clicked", G_CALLBACK(on_dialog_delete),
			name_action_new(p, name, window), name_action_free, 0);
		ui_row(danger, "Delete this script widget", "It is also removed from both layouts.",
			button);
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
	for (size_t i = 0; i < G_N_ELEMENTS(widget_types); i++) {
		if (!array_has(names, widget_types[i].type)) {
			g_ptr_array_add(names, g_strdup(widget_types[i].type));
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

static GtkWidget *drag_handle(void) {
	GtkWidget *handle = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
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
			name_action_new(p, name, NULL), name_action_free, 0);
		gtk_box_append(GTK_BOX(box), settings);
		GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_set_tooltip_text(remove, "Delete");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		g_signal_connect_data(remove, "clicked", G_CALLBACK(on_delete_script),
			name_action_new(p, name, NULL), name_action_free, 0);
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

static void root_settings_refresh(struct taskbar_page *p);

static void rebuild_all(struct taskbar_page *p) {
	p->updating = true;
	refresh_layout_controls(p);
	root_settings_refresh(p);
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

/* ---------- switches and numbers at the top level of taskbar.conf ---------- */

struct root_setting {
	struct taskbar_page *p;
	const char *key;
	GtkWidget *widget;
	bool is_switch;
	int fallback;
	guint timer;
};

static bool setting_is_on(const char *value) {
	return !(g_ascii_strcasecmp(value, "no") == 0 || g_ascii_strcasecmp(value, "off") == 0 ||
		g_ascii_strcasecmp(value, "false") == 0 || g_ascii_strcasecmp(value, "disable") == 0);
}

/* The default is written as nothing at all, so the file stays as short as it can. */
static void root_setting_write(struct root_setting *r) {
	char number[16];
	const char *value;
	if (r->is_switch) {
		bool on = gtk_switch_get_active(GTK_SWITCH(r->widget));
		value = on == (r->fallback != 0) ? NULL : on ? "yes" : "no";
	} else {
		int size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->widget));
		snprintf(number, sizeof(number), "%d", size);
		value = size == r->fallback ? NULL : number;
	}
	confdoc_set(doc(r->p), doc(r->p)->root, r->key, NULL, value);
	settings_taskbar_changed(r->p->s);
}

static gboolean root_setting_apply(gpointer data) {
	struct root_setting *r = data;
	r->timer = 0;
	root_setting_write(r);
	return G_SOURCE_REMOVE;
}

static void on_root_setting(GObject *object, gpointer data) {
	struct root_setting *r = data;
	if (r->p->updating) {
		return;
	}
	if (r->is_switch) {
		root_setting_write(r);
		return;
	}
	// spinning through the numbers must not rewrite the file on every step
	if (r->timer) {
		g_source_remove(r->timer);
	}
	r->timer = g_timeout_add(300, root_setting_apply, r);
}

/* "notify::..." hands the handler the property before the data. */
static void on_root_switch(GObject *object, GParamSpec *pspec, gpointer data) {
	on_root_setting(object, data);
}

static void root_setting_new(struct taskbar_page *p, GtkWidget *group, const char *key,
		const char *title, const char *hint, bool is_switch, int low, int high,
		int fallback) {
	struct root_setting *r = g_new0(struct root_setting, 1);
	r->p = p;
	r->key = key;
	r->is_switch = is_switch;
	r->fallback = fallback;
	if (is_switch) {
		r->widget = gtk_switch_new();
		g_signal_connect(r->widget, "notify::active", G_CALLBACK(on_root_switch), r);
	} else {
		r->widget = gtk_spin_button_new_with_range(low, high, 1);
		g_signal_connect(r->widget, "value-changed", G_CALLBACK(on_root_setting), r);
	}
	ui_row(group, title, hint, r->widget);
	g_ptr_array_add(p->root_settings, r);
}

static void root_settings_refresh(struct taskbar_page *p) {
	for (guint i = 0; i < p->root_settings->len; i++) {
		struct root_setting *r = p->root_settings->pdata[i];
		const char *value = cstmt_arg(confdoc_child(doc(p)->root, r->key, NULL), 0);
		if (r->is_switch) {
			gtk_switch_set_active(GTK_SWITCH(r->widget),
				value ? setting_is_on(value) : r->fallback != 0);
		} else {
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->widget),
				value ? atoi(value) : r->fallback);
		}
	}
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
	p->root_settings = g_ptr_array_new_with_free_func(g_free);
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
	root_setting_new(p, layout, "theme_layout", "Let the theme bring its own layout",
		"Off keeps the sections below whichever theme is picked", true, 0, 0, 1);

	GtkWidget *desktop = ui_group(content, "Desktop",
		"The icons of ~/Desktop and the grid they sit in.");
	root_setting_new(p, desktop, "desktop_icons", "Show icons on the desktop", NULL,
		true, 0, 0, 1);
	root_setting_new(p, desktop, "desktop_icon_size", "Icon size", "Pixels",
		false, 16, 256, 48);
	root_setting_new(p, desktop, "desktop_icon_width", "Cell width",
		"Pixels of the cell an icon sits in", false, 48, 400, 100);
	root_setting_new(p, desktop, "desktop_icon_height", "Cell height",
		"Pixels of the cell an icon sits in", false, 48, 400, 100);
	root_setting_new(p, desktop, "desktop_margin", "Margin", "Pixels around the whole grid",
		false, 0, 200, 10);

	GtkWidget *clipboard = ui_group(content, "Clipboard", NULL);
	root_setting_new(p, clipboard, "clipboard_history", "Remember what was copied",
		"Win+V shows the history; passwords marked as secret are never kept", true, 0, 0, 1);
	root_setting_new(p, clipboard, "clipboard_paste", "Paste the entry that is picked",
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
