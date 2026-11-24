#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_claude_usage.h"
#include "tw_codex_usage.h"
#include "tw_agent_accounts.h"
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

/* The Claude Code folders of this computer, each with the account logged in there. */
static void list_claude_accounts(GPtrArray *values, GPtrArray *labels) {
	g_ptr_array_add(labels, g_strdup("The one used last"));
	list_t *all = tw_claude_usage_all();
	const char *home = g_get_home_dir();
	for (int i = 0; i < all->length; i++) {
		struct tw_claude_usage *u = all->items[i];
		bool in_home = g_str_has_prefix(u->dir, home) && u->dir[strlen(home)] == '/';
		char *shown = in_home ? g_strdup_printf("~%s", u->dir + strlen(home)) : g_strdup(u->dir);
		g_ptr_array_add(values, g_strdup(shown));
		g_ptr_array_add(labels, g_strdup_printf("%s%s%s%s", u->account ? u->account : "No account",
			" (", shown, u->connected ? ")" : ", not connected yet)"));
		g_free(shown);
	}
	tw_claude_usage_list_free(all);
}

/* The Codex folders of this computer, each with the plan it works with. */
static void list_codex_accounts(GPtrArray *values, GPtrArray *labels) {
	g_ptr_array_add(labels, g_strdup("The installed one (~/.codex)"));
	list_t *all = tw_codex_usage_all();
	const char *home = g_get_home_dir();
	for (int i = 0; i < all->length; i++) {
		struct tw_claude_usage *u = all->items[i];
		bool in_home = g_str_has_prefix(u->dir, home) && u->dir[strlen(home)] == '/';
		char *shown = in_home ? g_strdup_printf("~%s", u->dir + strlen(home)) : g_strdup(u->dir);
		g_ptr_array_add(values, g_strdup(shown));
		g_ptr_array_add(labels, g_strdup_printf("%s (%s)", u->account ? u->account :
			"No limits seen yet", shown));
		g_free(shown);
	}
	tw_claude_usage_list_free(all);
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
	{ "claude", "account", list_claude_accounts },
	{ "codex", "account", list_codex_accounts },
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
	{ "bluetooth", "format", { "{device}", "{count}" }, { "Headphones", "1" } },
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

/*
 * One change of the old widgets, if there is any left; false when none is. The
 * document is parsed anew after each, so the search starts again every time.
 */
static bool replace_one_old(struct confdoc *d) {
	struct cstmt *root = d->root;
	static const char *const sections[] = { "left", "center", "right" };
	for (guint i = 0; i < root->children->len; i++) {
		struct cstmt *c = root->children->pdata[i];
		// the taskbar of a mode: each becomes "agents", once in the layout
		if (strcmp(c->name, "layout") == 0 && c->children) {
			bool agents = false;
			for (size_t k = 0; k < G_N_ELEMENTS(sections); k++) {
				struct cstmt *line = confdoc_child(c, sections[k], NULL);
				if (!line) {
					continue;
				}
				GString *words = g_string_new(NULL);
				bool changed = false;
				for (int a = 0; a < cstmt_argc(line); a++) {
					const char *name = cstmt_arg(line, a);
					if (tw_widget_replaced_by_agents(name)) {
						name = "agents";
						changed = true;
					}
					if (strcmp(name, "agents") == 0) {
						if (agents) {
							changed = true;
							continue;
						}
						agents = true;
					}
					char *quoted = conf_quote(name);
					g_string_append_printf(words, "%s%s", words->len ? " " : "", quoted);
					g_free(quoted);
				}
				if (changed) {
					confdoc_set(d, c, sections[k], NULL, words->len ? words->str : NULL);
					g_string_free(words, TRUE);
					return true;
				}
				g_string_free(words, TRUE);
			}
		}
		// their own settings
		if (strcmp(c->name, "widget") == 0 && tw_widget_replaced_by_agents(cstmt_arg(c, 0))) {
			confdoc_remove(d, c);
			return true;
		}
	}
	// the desktop: the entry of "agents" stays, else the first of theirs becomes it
	struct cstmt *block = confdoc_child(root, "desktop_widgets", NULL);
	if (block && block->children) {
		bool agents = confdoc_child(block, "agents", NULL) != NULL;
		for (guint i = 0; i < block->children->len; i++) {
			struct cstmt *e = block->children->pdata[i];
			if (tw_widget_replaced_by_agents(e->name)) {
				if (agents) {
					confdoc_remove(d, e);
				} else {
					confdoc_rename(d, e, "agents");
				}
				return true;
			}
		}
	}
	return false;
}

bool widgets_replace_old(struct confdoc *d) {
	if (!d || d->error) {
		return false;
	}
	bool changed = false;
	// each change takes one away, so this ends; the bound only guards against a bug
	for (int n = 0; n < 1000 && replace_one_old(d); n++) {
		changed = true;
	}
	return changed;
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

/* ---------- the size on the desktop ---------- */

/*
 * The cells a widget takes on the desktop: "Default" leaves it to the style,
 * else columns x rows, picked on a little grid (hovering shows what a click
 * makes) or set in the two fields. Written as "size CxR" of its desktop entry.
 */
#define SIZE_GRID_COLS 8
#define SIZE_GRID_ROWS 6
#define SIZE_CELL 18
#define SIZE_MOST 20

struct size_editor {
	struct settings *s;
	char *widget;
	GtkWidget *deflt, *cols, *rows, *grid, *shown;
	int hover_c, hover_r; // the cell under the pointer, 0 for none
	bool updating;
};

static void size_editor_free(gpointer data) {
	struct size_editor *e = data;
	g_free(e->widget);
	g_free(e);
}

/* What the style it has makes, when the size is left to it; 0 when it measures itself. */
static void size_of_style(struct size_editor *e, int *c, int *r) {
	char *type = widget_type_of(e->widget);
	const struct tw_widget_info *info = tw_widget_find(type);
	g_free(type);
	struct cstmt *entry = widget_desktop_entry(e->s, e->widget);
	const char *name = cstmt_arg(confdoc_child(entry, "style", NULL), 0);
	const struct tw_widget_style *style = info ? tw_widget_style_find(info, name) : NULL;
	*c = style ? style->columns : 0;
	*r = style ? style->rows : 0;
}

static void size_show(struct size_editor *e) {
	bool deflt = gtk_check_button_get_active(GTK_CHECK_BUTTON(e->deflt));
	int c = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->cols));
	int r = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->rows));
	char text[96];
	if (e->hover_c > 0) {
		snprintf(text, sizeof(text), "%d × %d cells", e->hover_c, e->hover_r);
	} else if (deflt && c == 0) {
		snprintf(text, sizeof(text), "As wide as it needs");
	} else {
		snprintf(text, sizeof(text), "%d × %d cells%s", c, r, deflt ? " (the style's)" : "");
	}
	gtk_label_set_text(GTK_LABEL(e->shown), text);
	gtk_widget_set_sensitive(e->cols, !deflt);
	gtk_widget_set_sensitive(e->rows, !deflt);
	gtk_widget_queue_draw(e->grid);
}

static void size_write(struct size_editor *e) {
	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(e->deflt))) {
		write_desktop_option(e->s, e->widget, "size", NULL);
	} else {
		char value[32];
		snprintf(value, sizeof(value), "%dx%d",
			gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->cols)),
			gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->rows)));
		write_desktop_option(e->s, e->widget, "size", value);
	}
	size_show(e);
}

/* A size of its own, from the fields or the grid: no longer the default. */
static void size_set(struct size_editor *e, int c, int r) {
	e->updating = true;
	gtk_check_button_set_active(GTK_CHECK_BUTTON(e->deflt), FALSE);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->cols), c);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->rows), r);
	e->updating = false;
	size_write(e);
}

static void on_size_spin(GtkSpinButton *spin, gpointer data) {
	struct size_editor *e = data;
	if (!e->updating) {
		size_write(e);
	}
}

static void on_size_default(GtkCheckButton *check, gpointer data) {
	struct size_editor *e = data;
	if (e->updating) {
		return;
	}
	if (gtk_check_button_get_active(check)) {
		int c, r;
		size_of_style(e, &c, &r);
		e->updating = true;
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->cols), c);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->rows), r);
		e->updating = false;
	} else if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->cols)) == 0) {
		// a style that measures itself: a size to start from
		e->updating = true;
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->cols), 2);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->rows), 1);
		e->updating = false;
	}
	size_write(e);
}

static void size_grid_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	struct size_editor *e = data;
	int c = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->cols));
	int r = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->rows));
	bool hovering = e->hover_c > 0;
	GdkRGBA fg;
	gtk_widget_get_color(GTK_WIDGET(area), &fg);
	for (int y = 0; y < SIZE_GRID_ROWS; y++) {
		for (int x = 0; x < SIZE_GRID_COLS; x++) {
			bool taken = hovering ? x < e->hover_c && y < e->hover_r : x < c && y < r;
			cairo_rectangle(cr, x * SIZE_CELL + 1.5, y * SIZE_CELL + 1.5, SIZE_CELL - 3,
				SIZE_CELL - 3);
			if (taken) {
				// the accent, lighter while it only shows what a click makes
				cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, hovering ? 0.55 : 0.9);
				cairo_fill_preserve(cr);
			}
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.35);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
	}
}

static void size_cell_at(double x, double y, int *c, int *r) {
	*c = (int)(x / SIZE_CELL) + 1;
	*r = (int)(y / SIZE_CELL) + 1;
	*c = *c < 1 ? 1 : *c > SIZE_GRID_COLS ? SIZE_GRID_COLS : *c;
	*r = *r < 1 ? 1 : *r > SIZE_GRID_ROWS ? SIZE_GRID_ROWS : *r;
}

static void on_size_motion(GtkEventControllerMotion *motion, double x, double y, gpointer data) {
	struct size_editor *e = data;
	size_cell_at(x, y, &e->hover_c, &e->hover_r);
	size_show(e);
}

static void on_size_leave(GtkEventControllerMotion *motion, gpointer data) {
	struct size_editor *e = data;
	e->hover_c = e->hover_r = 0;
	size_show(e);
}

static void on_size_click(GtkGestureClick *click, int n, double x, double y, gpointer data) {
	struct size_editor *e = data;
	int c, r;
	size_cell_at(x, y, &c, &r);
	size_set(e, c, r);
}

static void add_size_row(struct settings *s, GtkWidget *list, const char *widget,
		const struct tw_widget_option *opt) {
	struct size_editor *e = g_new0(struct size_editor, 1);
	e->s = s;
	e->widget = g_strdup(widget);
	int c = 0, r = 0;
	struct cstmt *entry = widget_desktop_entry(s, widget);
	const char *value = cstmt_arg(confdoc_child(entry, "size", NULL), 0);
	bool own = value && sscanf(value, "%dx%d", &c, &r) == 2 && c > 0 && r > 0;
	if (!own) {
		size_of_style(e, &c, &r);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	e->deflt = gtk_check_button_new_with_label("Default");
	gtk_widget_set_tooltip_text(e->deflt, "The style decides how many cells it takes");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(e->deflt), !own);
	gtk_box_append(GTK_BOX(line), e->deflt);
	e->cols = gtk_spin_button_new_with_range(0, SIZE_MOST, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->cols), c);
	gtk_widget_set_tooltip_text(e->cols, "Columns");
	gtk_box_append(GTK_BOX(line), e->cols);
	gtk_box_append(GTK_BOX(line), gtk_label_new("×"));
	e->rows = gtk_spin_button_new_with_range(0, SIZE_MOST, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->rows), r);
	gtk_widget_set_tooltip_text(e->rows, "Rows");
	gtk_box_append(GTK_BOX(line), e->rows);
	gtk_box_append(GTK_BOX(box), line);
	GtkWidget *below = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	e->grid = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(e->grid), SIZE_GRID_COLS * SIZE_CELL);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(e->grid), SIZE_GRID_ROWS * SIZE_CELL);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(e->grid), size_grid_draw, e, NULL);
	gtk_widget_set_tooltip_text(e->grid, "Click the cell of its lower right corner");
	gtk_widget_set_cursor_from_name(e->grid, "pointer");
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_size_motion), e);
	g_signal_connect(motion, "leave", G_CALLBACK(on_size_leave), e);
	gtk_widget_add_controller(e->grid, motion);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(on_size_click), e);
	gtk_widget_add_controller(e->grid, GTK_EVENT_CONTROLLER(click));
	gtk_box_append(GTK_BOX(below), e->grid);
	e->shown = gtk_label_new(NULL);
	gtk_widget_add_css_class(e->shown, "dim-label");
	gtk_widget_set_valign(e->shown, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(below), e->shown);
	gtk_box_append(GTK_BOX(box), below);
	g_signal_connect(e->deflt, "toggled", G_CALLBACK(on_size_default), e);
	g_signal_connect(e->cols, "value-changed", G_CALLBACK(on_size_spin), e);
	g_signal_connect(e->rows, "value-changed", G_CALLBACK(on_size_spin), e);
	g_object_set_data_full(G_OBJECT(box), "editor", e, size_editor_free);
	size_show(e);
	ui_row(list, opt->title, "Cells of the icon grid it takes, columns × rows: click the grid, "
		"or Default for what its style takes", box);
}

static void add_option_row(struct settings *s, GtkWidget *list, const char *widget,
		const struct tw_widget_option *opt, bool desktop) {
	char *row_type = widget_type_of(widget);
	bool own_editor = (strcmp(row_type, "agents") == 0 || strcmp(row_type, "claude") == 0 ||
		strcmp(row_type, "codex") == 0) && (strcmp(opt->key, "show") == 0 ||
		strcmp(opt->key, "accounts") == 0);
	g_free(row_type);
	if (own_editor) {
		return; // ticked and sorted in a group of its own
	}
	if (desktop && strcmp(opt->key, "size") == 0) {
		add_size_row(s, list, widget, opt);
		return;
	}
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

/* ---------- what the Claude widget shows ---------- */

/*
 * The items of the Claude widget, ticked and in order: "show" of its block for
 * the taskbar, of its desktop_widgets entry for the desktop, each with presets
 * of its own. Ticked items come first, in their order; the arrows move them.
 */
struct show_editor {
	struct settings *s;
	char *widget;
	bool desktop;
	GPtrArray *order;   // item ids, all of them: the ticked ones first
	GHashTable *ticked; // id -> id
	GtkWidget *list, *presets;
	bool updating;
	guint rebuild_idle;
};

static const char *show_default(struct show_editor *e) {
	return e->desktop ? TW_CLAUDE_SHOW_DESKTOP : TW_CLAUDE_SHOW_TASKBAR;
}

static const struct tw_claude_preset *show_presets(struct show_editor *e) {
	return e->desktop ? tw_claude_desktop_presets : tw_claude_taskbar_presets;
}

static char *show_current(struct show_editor *e) {
	GString *out = g_string_new(NULL);
	for (guint i = 0; i < e->order->len; i++) {
		if (g_hash_table_contains(e->ticked, e->order->pdata[i])) {
			g_string_append_printf(out, "%s%s", out->len ? " " : "", (char *)e->order->pdata[i]);
		}
	}
	return g_string_free(out, FALSE);
}

/* The order and ticks of a "show" value: its items first, then the others. */
static void show_load(struct show_editor *e, const char *show) {
	g_ptr_array_set_size(e->order, 0);
	g_hash_table_remove_all(e->ticked);
	char **ids = g_strsplit_set(show ? show : "", " ,\t", -1);
	for (int i = 0; ids[i]; i++) {
		const struct tw_claude_item *item = tw_claude_item_find(ids[i]);
		if (item && !g_hash_table_contains(e->ticked, item->id)) {
			g_ptr_array_add(e->order, (gpointer)item->id);
			g_hash_table_add(e->ticked, (gpointer)item->id);
		}
	}
	g_strfreev(ids);
	for (size_t i = 0; i < tw_claude_item_count; i++) {
		if (!g_hash_table_contains(e->ticked, tw_claude_items[i].id)) {
			g_ptr_array_add(e->order, (gpointer)tw_claude_items[i].id);
		}
	}
}

static void show_rebuild(struct show_editor *e);

static gboolean show_rebuild_idle(gpointer data) {
	struct show_editor *e = data;
	e->rebuild_idle = 0;
	show_rebuild(e);
	return G_SOURCE_REMOVE;
}

/* After the click: the row clicked in is among the ones the list is made of again. */
static void show_rebuild_later(struct show_editor *e) {
	if (!e->rebuild_idle) {
		e->rebuild_idle = g_idle_add(show_rebuild_idle, e);
	}
}

static void show_write(struct show_editor *e) {
	char *show = show_current(e);
	// the default is written as nothing, so a later default comes along
	const char *value = strcmp(show, show_default(e)) == 0 ? NULL : show;
	if (e->desktop) {
		write_desktop_option(e->s, e->widget, "show", value);
	} else {
		write_option(e->s, e->widget, "show", value);
	}
	g_free(show);
	show_rebuild_later(e);
}

static void on_show_tick(GtkCheckButton *check, gpointer data) {
	struct show_editor *e = data;
	if (e->updating) {
		return;
	}
	const char *id = g_object_get_data(G_OBJECT(check), "item");
	if (gtk_check_button_get_active(check)) {
		g_hash_table_add(e->ticked, (gpointer)id);
		// a newly ticked item goes after the ticked ones
		guint at = 0, ticked = 0;
		for (guint i = 0; i < e->order->len; i++) {
			if (e->order->pdata[i] == id) {
				at = i;
			} else if (g_hash_table_contains(e->ticked, e->order->pdata[i])) {
				ticked++;
			}
		}
		g_ptr_array_remove_index(e->order, at);
		g_ptr_array_insert(e->order, ticked, (gpointer)id);
	} else if (g_hash_table_size(e->ticked) > 1) {
		g_hash_table_remove(e->ticked, id);
	} else {
		show_rebuild_later(e); // the last one stays: a widget showing nothing helps nobody
		return;
	}
	show_write(e);
}

static void show_move(struct show_editor *e, const char *id, int step) {
	for (guint i = 0; i < e->order->len; i++) {
		if (e->order->pdata[i] == id) {
			guint to = i + step;
			if ((int)i + step < 0 || to >= e->order->len ||
					!g_hash_table_contains(e->ticked, e->order->pdata[to])) {
				return;
			}
			e->order->pdata[i] = e->order->pdata[to];
			e->order->pdata[to] = (gpointer)id;
			show_write(e);
			return;
		}
	}
}

static void on_show_up(GtkButton *button, gpointer data) {
	show_move(data, g_object_get_data(G_OBJECT(button), "item"), -1);
}

static void on_show_down(GtkButton *button, gpointer data) {
	show_move(data, g_object_get_data(G_OBJECT(button), "item"), 1);
}

static void on_show_preset(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct show_editor *e = data;
	if (e->updating) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	const struct tw_claude_preset *presets = show_presets(e);
	for (guint n = 0; presets[n].title; n++) {
		if (n == i) {
			show_load(e, presets[n].show);
			show_write(e);
			return;
		}
	}
}

static void show_rebuild(struct show_editor *e) {
	e->updating = true;
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(e->list))) {
		gtk_list_box_remove(GTK_LIST_BOX(e->list), child);
	}
	char *current = show_current(e);
	const struct tw_claude_preset *presets = show_presets(e);
	guint selected = GTK_INVALID_LIST_POSITION, n = 0;
	for (; presets[n].title; n++) {
		if (strcmp(presets[n].show, current) == 0) {
			selected = n;
		}
	}
	// a list of one's own shows as such in the presets
	GtkStringList *model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(e->presets)));
	guint items = g_list_model_get_n_items(G_LIST_MODEL(model));
	if (items > n) {
		gtk_string_list_remove(model, n);
	}
	if (selected == GTK_INVALID_LIST_POSITION) {
		gtk_string_list_append(model, "Your own");
		selected = n;
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(e->presets), selected);
	g_free(current);

	guint ticked = g_hash_table_size(e->ticked);
	for (guint i = 0; i < e->order->len; i++) {
		const char *id = e->order->pdata[i];
		const struct tw_claude_item *item = tw_claude_item_find(id);
		bool on = g_hash_table_contains(e->ticked, id);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(box, 12);
		gtk_widget_set_margin_end(box, 12);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		GtkWidget *check = gtk_check_button_new_with_label(item->title);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), on);
		gtk_widget_set_hexpand(check, TRUE);
		g_object_set_data(G_OBJECT(check), "item", (gpointer)id);
		g_signal_connect(check, "toggled", G_CALLBACK(on_show_tick), e);
		gtk_box_append(GTK_BOX(box), check);
		// buttons of their own: ui_icon_button frees its data with the button
		GtkWidget *up = gtk_button_new_from_icon_name("go-up-symbolic");
		gtk_widget_set_tooltip_text(up, "Earlier");
		gtk_widget_add_css_class(up, "flat");
		gtk_widget_set_sensitive(up, on && i > 0);
		g_signal_connect(up, "clicked", G_CALLBACK(on_show_up), e);
		g_object_set_data(G_OBJECT(up), "item", (gpointer)id);
		gtk_box_append(GTK_BOX(box), up);
		GtkWidget *down = gtk_button_new_from_icon_name("go-down-symbolic");
		gtk_widget_set_tooltip_text(down, "Later");
		gtk_widget_add_css_class(down, "flat");
		gtk_widget_set_sensitive(down, on && i + 1 < ticked);
		g_signal_connect(down, "clicked", G_CALLBACK(on_show_down), e);
		g_object_set_data(G_OBJECT(down), "item", (gpointer)id);
		gtk_box_append(GTK_BOX(box), down);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(e->list), row);
	}
	e->updating = false;
}

static void show_editor_free(gpointer data) {
	struct show_editor *e = data;
	if (e->rebuild_idle) {
		g_source_remove(e->rebuild_idle);
	}
	g_ptr_array_unref(e->order);
	g_hash_table_unref(e->ticked);
	g_free(e->widget);
	g_free(e);
}

static void show_editor_attach(struct settings *s, GtkWidget *content, const char *widget,
		bool desktop) {
	struct show_editor *e = g_new0(struct show_editor, 1);
	e->s = s;
	e->widget = g_strdup(widget);
	e->desktop = desktop;
	e->order = g_ptr_array_new();
	e->ticked = g_hash_table_new(g_str_hash, g_str_equal);
	struct cstmt *block = desktop ? widget_desktop_entry(s, widget) :
		confdoc_block(s->taskbar, "widget", widget, false);
	char *value = cstmt_join(confdoc_child(block, "show", NULL), 0);
	show_load(e, value && *value ? value : show_default(e));
	g_free(value);

	GtkWidget *group = ui_group(content, desktop ? "What it shows on the desktop" :
		"What it shows on the taskbar", "Tick what to show; the arrows put it earlier or "
		"later. The time left is how long the limit lasts at the pace it was used at since "
		"it started.");
	GtkStringList *names = gtk_string_list_new(NULL);
	const struct tw_claude_preset *presets = show_presets(e);
	for (guint n = 0; presets[n].title; n++) {
		gtk_string_list_append(names, presets[n].title);
	}
	e->presets = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	g_signal_connect(e->presets, "notify::selected", G_CALLBACK(on_show_preset), e);
	ui_row(group, "Preset", NULL, e->presets);
	e->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(e->list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(e->list, "boxed-list");
	gtk_widget_set_margin_bottom(e->list, 8);
	GtkWidget *holder = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(holder), e->list);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(holder), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(group), holder);
	g_object_set_data_full(G_OBJECT(e->list), "editor", e, show_editor_free);
	show_rebuild(e);
}

/* ---------- the accounts of the AI agent usage widget ---------- */

/*
 * A tick for each account found on this computer (tw_agent_accounts.h) and
 * each the config names that is not there now: "accounts" of its block for
 * the taskbar, of its desktop_widgets entry for the desktop (the taskbar's
 * there by default). The arrows put an account earlier or later, the order
 * the widget shows them in. All of them ticked, in the order they are found,
 * is written as nothing, so an account added later comes along.
 */
struct accounts_editor {
	struct settings *s;
	char *widget;
	bool desktop, legacy;
	GPtrArray *ids;     // all, in the order shown
	GPtrArray *titles;  // of each, "Claude Code · ~/.claude"
	GHashTable *ticked; // id -> id
	guint found;        // how many of ids were found here, in the order found at first
	char *found_order;  // their ids as found, space separated
	GtkWidget *list;
	guint rebuild_idle;
	bool updating;
};

static void accounts_editor_free(gpointer data) {
	struct accounts_editor *e = data;
	if (e->rebuild_idle) {
		g_source_remove(e->rebuild_idle);
	}
	g_free(e->found_order);
	g_hash_table_unref(e->ticked); // (its keys are in ids)
	g_ptr_array_unref(e->ids);
	g_ptr_array_unref(e->titles);
	g_free(e->widget);
	g_free(e);
}

static void accounts_write(struct accounts_editor *e) {
	GString *out = g_string_new(NULL);
	for (guint i = 0; i < e->ids->len; i++) {
		if (g_hash_table_contains(e->ticked, e->ids->pdata[i])) {
			g_string_append_printf(out, "%s%s", out->len ? " " : "", (char *)e->ids->pdata[i]);
		}
	}
	// all of them as found: nothing, so the ones found later come along (an older
	// claude or codex widget shows its one account by default, so it is written)
	bool all = g_hash_table_size(e->ticked) == e->ids->len && !e->legacy &&
		g_strcmp0(out->str, e->found_order) == 0;
	if (e->desktop) {
		write_desktop_option(e->s, e->widget, "accounts", all ? NULL : out->str);
	} else {
		write_option(e->s, e->widget, "accounts", all ? NULL : out->str);
	}
	g_string_free(out, TRUE);
}

static void on_account_tick(GtkCheckButton *check, gpointer data) {
	struct accounts_editor *e = data;
	if (e->updating) {
		return;
	}
	const char *id = g_object_get_data(G_OBJECT(check), "account");
	if (gtk_check_button_get_active(check)) {
		g_hash_table_add(e->ticked, (gpointer)id);
	} else if (g_hash_table_size(e->ticked) > 1) {
		g_hash_table_remove(e->ticked, id);
	} else {
		// the last one stays: a widget showing nothing helps nobody
		e->updating = true;
		gtk_check_button_set_active(check, TRUE);
		e->updating = false;
		return;
	}
	accounts_write(e);
}

/* "Claude Code · ~/.claude-work" for the row of an account. */
static char *account_title(const struct tw_agent_account *a) {
	if (!a->dir) {
		return g_strdup(tw_agent_names[a->agent]);
	}
	const char *colon = strchr(a->id, ':');
	return g_strdup_printf("%s · %s", tw_agent_names[a->agent], colon ? colon + 1 : a->dir);
}

static void accounts_rebuild(struct accounts_editor *e);

/* A folder picked: a Claude Code or a Codex account of its own, ticked and kept. */
static void on_account_folder(GObject *source, GAsyncResult *result, gpointer data) {
	struct accounts_editor *e = data;
	GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
	char *path = file ? g_file_get_path(file) : NULL;
	if (path) {
		char *dir = tw_claude_normalize_dir(path);
		bool codex = tw_agent_folder_is_codex(dir);
		char id[300];
		tw_agent_account_id(codex ? TW_AGENT_CODEX : TW_AGENT_CLAUDE, dir, id, sizeof(id));
		tw_agent_accounts_remember(id); // listed from now on, here and on the taskbar
		guint at;
		if (!g_ptr_array_find_with_equal_func(e->ids, id, g_str_equal, &at)) {
			struct tw_agent_account a;
			g_ptr_array_add(e->ids, g_strdup(id));
			g_ptr_array_add(e->titles, tw_agent_account_parse(id, &a) ? account_title(&a) :
				g_strdup(id));
			tw_agent_account_finish(&a);
			at = e->ids->len - 1;
		}
		g_hash_table_add(e->ticked, e->ids->pdata[at]);
		accounts_write(e);
		accounts_rebuild(e);
		settings_status(e->s, codex ? "Codex folder added" : "Claude Code folder added");
		free(dir);
	}
	g_free(path);
	if (file) {
		g_object_unref(file);
	}
}

static void on_account_add(GtkButton *button, gpointer data) {
	struct accounts_editor *e = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "The folder of a Claude Code or Codex");
	GFile *home = g_file_new_for_path(g_get_home_dir());
	gtk_file_dialog_set_initial_folder(dialog, home);
	g_object_unref(home);
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
	gtk_file_dialog_select_folder(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
		on_account_folder, e);
	g_object_unref(dialog);
}

static gboolean accounts_rebuild_idle(gpointer data) {
	struct accounts_editor *e = data;
	e->rebuild_idle = 0;
	accounts_rebuild(e);
	return G_SOURCE_REMOVE;
}

/* An account one place earlier (-1) or later (1): the order the widget shows them in. */
static void accounts_move(GtkButton *button, struct accounts_editor *e, int by) {
	const char *id = g_object_get_data(G_OBJECT(button), "account");
	guint at;
	if (!g_ptr_array_find_with_equal_func(e->ids, id, g_str_equal, &at) ||
			(by < 0 && at == 0) || (by > 0 && at + 1 >= e->ids->len)) {
		return;
	}
	guint to = at + by;
	gpointer swap = e->ids->pdata[at];
	e->ids->pdata[at] = e->ids->pdata[to];
	e->ids->pdata[to] = swap;
	swap = e->titles->pdata[at];
	e->titles->pdata[at] = e->titles->pdata[to];
	e->titles->pdata[to] = swap;
	accounts_write(e);
	// made again once the click is over: the button clicked is one of the rows
	if (!e->rebuild_idle) {
		e->rebuild_idle = g_idle_add(accounts_rebuild_idle, e);
	}
}

static void on_account_up(GtkButton *button, gpointer data) {
	accounts_move(button, data, -1);
}

static void on_account_down(GtkButton *button, gpointer data) {
	accounts_move(button, data, 1);
}

/* A tick for each account with its arrows, then the button that adds a folder. */
static void accounts_rebuild(struct accounts_editor *e) {
	e->updating = true;
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(e->list))) {
		gtk_list_box_remove(GTK_LIST_BOX(e->list), child);
	}
	for (guint i = 0; i < e->ids->len; i++) {
		GtkWidget *check = gtk_check_button_new_with_label(e->titles->pdata[i]);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
			g_hash_table_contains(e->ticked, e->ids->pdata[i]));
		g_object_set_data(G_OBJECT(check), "account", e->ids->pdata[i]);
		g_signal_connect(check, "toggled", G_CALLBACK(on_account_tick), e);
		gtk_widget_set_hexpand(check, TRUE);
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(line, 12);
		gtk_widget_set_margin_end(line, 12);
		gtk_widget_set_margin_top(line, 4);
		gtk_widget_set_margin_bottom(line, 4);
		gtk_box_append(GTK_BOX(line), check);
		GtkWidget *up = gtk_button_new_from_icon_name("go-up-symbolic");
		gtk_widget_set_tooltip_text(up, "Earlier");
		gtk_widget_add_css_class(up, "flat");
		gtk_widget_set_sensitive(up, i > 0);
		g_object_set_data(G_OBJECT(up), "account", e->ids->pdata[i]);
		g_signal_connect(up, "clicked", G_CALLBACK(on_account_up), e);
		gtk_box_append(GTK_BOX(line), up);
		GtkWidget *down = gtk_button_new_from_icon_name("go-down-symbolic");
		gtk_widget_set_tooltip_text(down, "Later");
		gtk_widget_add_css_class(down, "flat");
		gtk_widget_set_sensitive(down, i + 1 < e->ids->len);
		g_object_set_data(G_OBJECT(down), "account", e->ids->pdata[i]);
		g_signal_connect(down, "clicked", G_CALLBACK(on_account_down), e);
		gtk_box_append(GTK_BOX(line), down);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), line);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(e->list), row);
	}
	// a Claude Code or Codex somewhere else (portable, another folder)
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	GtkWidget *hint = gtk_label_new(e->ids->len ? "Installed somewhere else?" :
		"No AI agent found on this computer");
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_widget_set_hexpand(hint, TRUE);
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_box_append(GTK_BOX(box), hint);
	GtkWidget *add = gtk_button_new_with_label("Add a folder…");
	gtk_widget_set_tooltip_text(add, "A Claude Code folder (its .claude, or the one "
		"$CLAUDE_CONFIG_DIR names) or a Codex folder ($CODEX_HOME)");
	g_signal_connect(add, "clicked", G_CALLBACK(on_account_add), e);
	gtk_box_append(GTK_BOX(box), add);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(e->list), row);
	e->updating = false;
}

static void accounts_editor_attach(struct settings *s, GtkWidget *content, const char *widget,
		const char *type, bool desktop) {
	struct accounts_editor *e = g_new0(struct accounts_editor, 1);
	e->s = s;
	e->widget = g_strdup(widget);
	e->desktop = desktop;
	e->legacy = strcmp(type, "agents") != 0;
	e->ids = g_ptr_array_new_with_free_func(g_free);
	e->ticked = g_hash_table_new(g_str_hash, g_str_equal);
	struct cstmt *block = desktop ? widget_desktop_entry(s, widget) : NULL;
	char *value = block ? cstmt_join(confdoc_child(block, "accounts", NULL), 0) : NULL;
	if (!value || !*value) {
		g_free(value);
		block = confdoc_block(s->taskbar, "widget", widget, false);
		value = cstmt_join(confdoc_child(block, "accounts", NULL), 0);
	}
	e->titles = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *titles = e->titles;
	struct tw_agent_account *found;
	int n = tw_agent_accounts_found(&found);
	for (int i = 0; i < n; i++) {
		g_ptr_array_add(e->ids, g_strdup(found[i].id));
		g_ptr_array_add(titles, account_title(&found[i]));
	}
	tw_agent_accounts_free(found, n);
	GString *order = g_string_new(NULL);
	for (guint i = 0; i < e->ids->len; i++) {
		g_string_append_printf(order, "%s%s", order->len ? " " : "", (char *)e->ids->pdata[i]);
	}
	e->found_order = g_string_free(order, FALSE);
	// the ticked ones: of the config, or all of them (the older widgets: their account)
	char *wanted = value && *value ? g_strdup(value) : NULL;
	if (!wanted && e->legacy) {
		char *account = cstmt_join(confdoc_child(block, "account", NULL), 0);
		bool codex = strcmp(type, "codex") == 0;
		char *dir = tw_usage_select_dir(codex ? &tw_codex_tool : &tw_claude_tool,
			account && *account ? account : NULL);
		char id[300];
		tw_agent_account_id(codex ? TW_AGENT_CODEX : TW_AGENT_CLAUDE, dir, id, sizeof(id));
		wanted = g_strdup(id);
		free(dir);
		g_free(account);
	}
	g_free(value);
	char **ids = g_strsplit_set(wanted ? wanted : "", " ,\t", -1);
	g_free(wanted);
	bool any = false;
	for (int i = 0; ids[i]; i++) {
		struct tw_agent_account a;
		if (!tw_agent_account_parse(ids[i], &a)) {
			continue;
		}
		guint at;
		if (!g_ptr_array_find_with_equal_func(e->ids, a.id, g_str_equal, &at)) {
			// named in the config, not found now (a folder of a disk not plugged in)
			g_ptr_array_add(e->ids, g_strdup(a.id));
			g_ptr_array_add(titles, account_title(&a));
			at = e->ids->len - 1;
		}
		if (g_hash_table_contains(e->ticked, e->ids->pdata[at])) {
			tw_agent_account_finish(&a); // named twice: its first place counts
			continue;
		}
		// in the order of the config: the ticked ones first, as named
		guint place = g_hash_table_size(e->ticked);
		if (at != place) {
			gpointer id = g_ptr_array_steal_index(e->ids, at);
			gpointer title = g_ptr_array_steal_index(e->titles, at);
			g_ptr_array_insert(e->ids, place, id);
			g_ptr_array_insert(e->titles, place, title);
			at = place;
		}
		g_hash_table_add(e->ticked, e->ids->pdata[at]);
		any = true;
		tw_agent_account_finish(&a);
	}
	g_strfreev(ids);
	for (guint i = 0; !any && i < e->ids->len; i++) {
		g_hash_table_add(e->ticked, e->ids->pdata[i]); // by default all of them
	}

	GtkWidget *group = ui_group(content, desktop ? "Accounts on the desktop" : "Accounts",
		"Which agents' accounts it shows, one after the other. Claude Code shows its limits "
		"once its folder is connected; Copilot's allowance comes only when the accounts are "
		"asked (below).");
	e->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(e->list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(e->list, "boxed-list");
	gtk_widget_set_margin_bottom(e->list, 8);
	accounts_rebuild(e);
	GtkWidget *holder = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(holder), e->list);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(holder), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(group), holder);
	g_object_set_data_full(G_OBJECT(e->list), "editor", e, accounts_editor_free);
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
	if (strcmp(type, "agents") == 0 || strcmp(type, "claude") == 0 ||
			strcmp(type, "codex") == 0) {
		// which accounts and what of them come first
		accounts_editor_attach(s, content, name, type, desktop);
		show_editor_attach(s, content, name, desktop);
	}
	const struct tw_widget_option *own_look = tw_widget_look(type, desktop);
	if (own_look) {
		// how it looks there, apart for each place
		GtkWidget *look = ui_group(content, desktop ? "Look on the desktop" :
			"Look on the taskbar", desktop ? "Only for the desktop: the taskbar keeps its own." :
			"Only for the taskbar: the desktop keeps its own.");
		for (const struct tw_widget_option *opt = own_look; opt->key; opt++) {
			add_option_row(s, look, name, opt, desktop);
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
