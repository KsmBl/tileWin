#include <gtk/gtk.h>
#include <string.h>
#include "devices.h"
#include "settings.h"

/*
 * Device manager page: every device Linux found, by kind, as the Device
 * Manager of Windows lists them, with a properties window of its driver, its
 * details, resources and the kernel's messages about it. Disable, Enable and
 * Restart go through tilewin-devices-root (pkexec), which refuses what the
 * running system stands on: the disk of a mounted file system, the graphics
 * of a screen in use, the controllers they hang on.
 */

struct devices_page {
	struct settings *s;
	GtkWidget *list, *summary;
	GtkWidget *properties, *disable, *enable, *restart;
	GtkWidget *hidden;
	GPtrArray *devices;      // struct tw_device *
	GHashTable *open;        // category keys shown unfolded
	GHashTable *rows;        // category key -> GPtrArray of its device rows
	char *selected;          // the path of the selected device, kept over a scan
	GFileMonitor *monitor;   // udev's database: a device came or went
	guint rescan_timer;
	gint64 toggled_at;       // a click folded a category: the double-click's activation must not again
	bool busy;
};

static void rescan(struct devices_page *p);
static void properties_open(struct devices_page *p, struct tw_device *d);

static char *helper_path(void) {
	const char *test = g_getenv("TILEWIN_DEVICES_ROOT_HELPER");
	return g_strdup(test && *test ? test : TILEWIN_LIBEXECDIR "/tilewin-devices-root");
}

static struct tw_device *row_device(GtkListBoxRow *row) {
	return row ? g_object_get_data(G_OBJECT(row), "device") : NULL;
}

static struct tw_device *selected_device(struct devices_page *p) {
	return row_device(gtk_list_box_get_selected_row(GTK_LIST_BOX(p->list)));
}

/* ---------- what may be done to a device ---------- */

/* "ok", or why the root helper would refuse: it decides, the page only asks. */
static char *check_device(struct tw_device *d) {
	if (!d->action || !d->action_bus) {
		return g_strdup("This device is not one tileWin can disable: disable the device it "
			"belongs to instead.");
	}
	char *helper = helper_path(), *base = g_path_get_basename(d->action);
	const char *argv[] = { "bash", helper, "check", d->action_bus, base, NULL };
	char *out = NULL;
	g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
		NULL, NULL, &out, NULL, NULL, NULL);
	g_free(helper);
	g_free(base);
	if (!out) {
		return g_strdup("tilewin-devices-root is missing: install tileWin again.");
	}
	g_strstrip(out);
	return out;
}

static void update_buttons(struct devices_page *p) {
	struct tw_device *d = selected_device(p);
	gtk_widget_set_sensitive(p->properties, d != NULL);
	bool can = false;
	char *why = NULL;
	if (d && !p->busy) {
		why = check_device(d);
		can = strcmp(why, "ok") == 0;
	}
	bool off = d && d->problem == TW_DEV_DISABLED;
	gtk_widget_set_sensitive(p->disable, can && !off);
	gtk_widget_set_sensitive(p->enable, d && d->action && off && !p->busy);
	gtk_widget_set_sensitive(p->restart, can && !off);
	const char *tip = can || !why ? NULL : why;
	gtk_widget_set_tooltip_text(p->disable, tip);
	gtk_widget_set_tooltip_text(p->restart, tip);
	g_free(why);
}

struct action {
	struct devices_page *p;
	char *what, *name;
};

static void on_action_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct action *a = data;
	struct devices_page *p = a->p;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		&error) && g_subprocess_get_successful(G_SUBPROCESS(source));
	p->busy = false;
	if (ok) {
		settings_status(p->s, "%s", out && *out ? g_strstrip(out) : "Done");
	} else {
		const char *why = err && *g_strstrip(err) ? err : error ? error->message : "It did not work";
		settings_status(p->s, "%s: %s", a->name, why);
		GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", a->name);
		gtk_alert_dialog_set_detail(dialog, why);
		gtk_alert_dialog_show(dialog, p->s->window);
		g_object_unref(dialog);
	}
	g_clear_error(&error);
	g_free(out);
	g_free(err);
	g_free(a->what);
	g_free(a->name);
	g_free(a);
	rescan(p);
}

static void run_action(struct devices_page *p, struct tw_device *d, const char *what) {
	if (p->busy || (!d->action && strcmp(what, "rescan") != 0)) {
		return;
	}
	char *helper = helper_path();
	char *base = d && d->action ? g_path_get_basename(d->action) : NULL;
	const char *prefix = g_getenv("TILEWIN_DEVICES_ROOT_HELPER") ? "bash" : "pkexec";
	const char *argv[] = { prefix, helper, what, d && d->action_bus ? d->action_bus : NULL, base,
		NULL };
	if (strcmp(what, "rescan") == 0) {
		argv[3] = NULL;
	}
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	if (proc) {
		struct action *a = g_new0(struct action, 1);
		a->p = p;
		a->what = g_strdup(what);
		a->name = g_strdup(strcmp(what, "disable") == 0 ? "Disable device" :
			strcmp(what, "enable") == 0 ? "Enable device" : strcmp(what, "restart") == 0 ?
			"Restart device" : "Scan for hardware changes");
		p->busy = true;
		update_buttons(p);
		g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_action_done, a);
		g_object_unref(proc);
	} else {
		settings_status(p->s, "%s", error->message);
		g_error_free(error);
	}
	g_free(helper);
	g_free(base);
}

struct confirm {
	struct devices_page *p;
	char *path, *what;
};

static void on_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
	struct confirm *c = data;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	for (guint i = 0; choice == 1 && c->p->devices && i < c->p->devices->len; i++) {
		struct tw_device *d = c->p->devices->pdata[i];
		if (g_strcmp0(d->path, c->path) == 0) {
			run_action(c->p, d, c->what);
			break;
		}
	}
	g_free(c->path);
	g_free(c->what);
	g_free(c);
}

/* Disabling asks first, as on Windows: the device stops working. */
static void ask_disable(struct devices_page *p, struct tw_device *d, const char *what) {
	bool disable = strcmp(what, "disable") == 0;
	GtkAlertDialog *dialog = gtk_alert_dialog_new(disable ? "Disable %s?" : "Restart %s?", d->name);
	gtk_alert_dialog_set_detail(dialog, disable ? "Disabling this device will cause it to stop "
		"functioning, also after a restart, until it is enabled again here. What it belongs "
		"to goes with it: a USB device takes all its functions along." : "Its driver lets go "
		"of it and takes it again, as when it is plugged in anew. What uses it loses it for "
		"a moment.");
	gtk_alert_dialog_set_buttons(dialog, (const char *const[]){ "Cancel",
		disable ? "Disable" : "Restart", NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	struct confirm *c = g_new0(struct confirm, 1);
	c->p = p;
	c->path = g_strdup(d->path);
	c->what = g_strdup(what);
	gtk_alert_dialog_choose(dialog, p->s->window, NULL, on_confirmed, c);
	g_object_unref(dialog);
}

static void on_toolbar(GtkButton *button, gpointer data) {
	struct devices_page *p = data;
	const char *what = g_object_get_data(G_OBJECT(button), "what");
	struct tw_device *d = selected_device(p);
	if (strcmp(what, "rescan") == 0) {
		run_action(p, NULL, "rescan");
	} else if (!d) {
		return;
	} else if (strcmp(what, "properties") == 0) {
		properties_open(p, d);
	} else if (strcmp(what, "enable") == 0) {
		run_action(p, d, "enable");
	} else {
		ask_disable(p, d, what);
	}
}

/* ---------- the list ---------- */

static GtkWidget *row_box(const char *icon, const char *text, int indent) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(box, 8 + indent);
	gtk_widget_set_margin_end(box, 8);
	gtk_widget_set_margin_top(box, 3);
	gtk_widget_set_margin_bottom(box, 3);
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_box_append(GTK_BOX(box), image);
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(box), label);
	return box;
}

static void set_open(struct devices_page *p, const char *key, bool open) {
	if (open) {
		g_hash_table_add(p->open, g_strdup(key));
	} else {
		g_hash_table_remove(p->open, key);
	}
	GPtrArray *rows = g_hash_table_lookup(p->rows, key);
	for (guint i = 0; rows && i < rows->len; i++) {
		gtk_widget_set_visible(rows->pdata[i], open);
	}
}

static void fold(struct devices_page *p, GtkListBoxRow *row, bool open) {
	const char *category = g_object_get_data(G_OBJECT(row), "category");
	if (!category) {
		return;
	}
	set_open(p, category, open);
	GtkWidget *arrow = g_object_get_data(G_OBJECT(row), "arrow");
	gtk_image_set_from_icon_name(GTK_IMAGE(arrow), open ? "pan-down-symbolic" :
		"pan-end-symbolic");
}

static bool is_open(struct devices_page *p, GtkListBoxRow *row) {
	const char *category = g_object_get_data(G_OBJECT(row), "category");
	return category && g_hash_table_contains(p->open, category);
}

/* Enter, or a double-click: a device's properties, a category opened or closed. */
static void on_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct devices_page *p = data;
	struct tw_device *d = row_device(row);
	if (d) {
		properties_open(p, d);
	} else if (g_get_monotonic_time() - p->toggled_at > 500000) {
		fold(p, row, !is_open(p, row));
	}
}

/* One click opens or closes a category, as its arrow does on Windows. */
static void on_click(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
	struct devices_page *p = data;
	GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(p->list), (int)y);
	if (n == 1 && row && g_object_get_data(G_OBJECT(row), "category")) {
		fold(p, row, !is_open(p, row));
		p->toggled_at = g_get_monotonic_time();
	}
}

/* The arrow keys of a tree: right opens a category, left closes it, or goes
 * from a device up to its category. */
static gboolean on_key(GtkEventControllerKey *key, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct devices_page *p = data;
	GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->list));
	if (!row || (keyval != GDK_KEY_Right && keyval != GDK_KEY_Left)) {
		return FALSE;
	}
	if (g_object_get_data(G_OBJECT(row), "category")) {
		fold(p, row, keyval == GDK_KEY_Right);
		return TRUE;
	}
	if (keyval == GDK_KEY_Left) {
		for (GtkWidget *w = GTK_WIDGET(row); w; w = gtk_widget_get_prev_sibling(w)) {
			if (g_object_get_data(G_OBJECT(w), "category")) {
				fold(p, GTK_LIST_BOX_ROW(w), false);
				gtk_list_box_select_row(GTK_LIST_BOX(p->list), GTK_LIST_BOX_ROW(w));
				gtk_widget_grab_focus(w);
				return TRUE;
			}
		}
	}
	return FALSE;
}

static void on_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct devices_page *p = data;
	struct tw_device *d = row_device(row);
	g_free(p->selected);
	p->selected = d ? g_strdup(d->path) : NULL;
	update_buttons(p);
}

static void on_right_click(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
	struct devices_page *p = data;
	GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(p->list), (int)y);
	if (!row_device(row)) {
		return;
	}
	gtk_list_box_select_row(GTK_LIST_BOX(p->list), row);
	GtkWidget *menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *buttons[] = { p->properties, p->disable, p->enable, p->restart };
	const char *labels[] = { "Properties", "Disable device", "Enable device", "Restart device" };
	const char *whats[] = { "properties", "disable", "enable", "restart" };
	GtkWidget *popover = gtk_popover_new();
	for (int i = 0; i < 4; i++) {
		GtkWidget *b = gtk_button_new_with_label(labels[i]);
		gtk_widget_add_css_class(b, "flat");
		gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
		gtk_widget_set_sensitive(b, gtk_widget_get_sensitive(buttons[i]));
		g_object_set_data(G_OBJECT(b), "what", (gpointer)whats[i]);
		g_signal_connect(b, "clicked", G_CALLBACK(on_toolbar), p);
		g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
		gtk_box_append(GTK_BOX(menu), b);
	}
	gtk_popover_set_child(GTK_POPOVER(popover), menu);
	gtk_widget_set_parent(popover, p->list);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){ (int)x, (int)y, 1, 1 });
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}

static void show_devices(struct devices_page *p) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->list), child);
	}
	g_hash_table_remove_all(p->rows);
	bool hidden = gtk_check_button_get_active(GTK_CHECK_BUTTON(p->hidden));
	size_t n;
	const struct tw_dev_category *cats = tw_dev_categories(&n);
	int shown = 0, problems = 0;
	GtkListBoxRow *reselect = NULL;
	for (size_t c = 0; c < n; c++) {
		// sorted by category title: the rows of one category follow each other
		GPtrArray *mine = g_ptr_array_new();
		bool warning = false;
		for (guint i = 0; i < p->devices->len; i++) {
			struct tw_device *d = p->devices->pdata[i];
			if (strcmp(d->category, cats[c].key) == 0 && (hidden || !d->hidden)) {
				g_ptr_array_add(mine, d);
				warning = warning || d->problem == TW_DEV_WARNING;
			}
		}
		if (mine->len == 0) {
			g_ptr_array_unref(mine);
			continue;
		}
		// a category with a problem opens by itself, as Windows does
		if (warning && !g_hash_table_contains(p->open, cats[c].key)) {
			g_hash_table_add(p->open, g_strdup(cats[c].key));
		}
		bool open = g_hash_table_contains(p->open, cats[c].key);
		GtkWidget *box = row_box(cats[c].icon, cats[c].title, 0);
		GtkWidget *arrow = gtk_image_new_from_icon_name(open ? "pan-down-symbolic" :
			"pan-end-symbolic");
		gtk_box_prepend(GTK_BOX(box), arrow);
		char *count = g_strdup_printf("%u", mine->len);
		GtkWidget *count_label = gtk_label_new(count);
		gtk_widget_add_css_class(count_label, "dim-label");
		gtk_box_append(GTK_BOX(box), count_label);
		g_free(count);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		g_object_set_data(G_OBJECT(row), "category", (gpointer)cats[c].key);
		g_object_set_data(G_OBJECT(row), "arrow", arrow);
		gtk_list_box_append(GTK_LIST_BOX(p->list), row);
		GPtrArray *rows = g_ptr_array_new();
		for (guint i = 0; i < mine->len; i++) {
			struct tw_device *d = mine->pdata[i];
			GtkWidget *dbox = row_box(d->icon, d->name, 42);
			if (d->problem != TW_DEV_OK) {
				GtkWidget *mark = gtk_image_new_from_icon_name(d->problem == TW_DEV_WARNING ?
					"dialog-warning-symbolic" : "action-unavailable-symbolic");
				gtk_widget_add_css_class(mark, d->problem == TW_DEV_WARNING ? "warning" : "dim-label");
				gtk_widget_set_tooltip_text(mark, d->status);
				gtk_box_insert_child_after(GTK_BOX(dbox), mark, gtk_widget_get_first_child(dbox));
				problems++;
			}
			if (d->hidden) {
				gtk_widget_set_opacity(dbox, 0.6);
			}
			gtk_widget_set_tooltip_text(dbox, d->status);
			GtkWidget *drow = gtk_list_box_row_new();
			gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(drow), dbox);
			g_object_set_data(G_OBJECT(drow), "device", d);
			gtk_widget_set_visible(drow, open);
			gtk_list_box_append(GTK_LIST_BOX(p->list), drow);
			g_ptr_array_add(rows, drow);
			if (p->selected && g_strcmp0(p->selected, d->path) == 0 && !reselect) {
				reselect = GTK_LIST_BOX_ROW(drow);
			}
			shown++;
		}
		g_hash_table_insert(p->rows, g_strdup(cats[c].key), rows);
		g_ptr_array_unref(mine);
	}
	if (reselect) {
		gtk_list_box_select_row(GTK_LIST_BOX(p->list), reselect);
	}
	char *text = problems ? g_strdup_printf("%d devices; %d of them %s a problem or %s disabled",
		shown, problems, problems == 1 ? "has" : "have", problems == 1 ? "is" : "are") :
		g_strdup_printf("%d devices, all working", shown);
	gtk_label_set_text(GTK_LABEL(p->summary), text);
	g_free(text);
	update_buttons(p);
}

static void rescan(struct devices_page *p) {
	if (p->devices) {
		g_ptr_array_unref(p->devices);
	}
	p->devices = tw_devices_scan();
	show_devices(p);
}

static void on_hidden(GtkCheckButton *button, gpointer data) {
	show_devices(data);
}

static gboolean rescan_later(gpointer data) {
	struct devices_page *p = data;
	p->rescan_timer = 0;
	if (gtk_widget_get_mapped(p->list) && !p->busy) {
		rescan(p);
	}
	return G_SOURCE_REMOVE;
}

/* A device plugged in or out: udev writes its database a moment after. */
static void on_udev_changed(GFileMonitor *monitor, GFile *file, GFile *other,
		GFileMonitorEvent event, gpointer data) {
	struct devices_page *p = data;
	if (event != G_FILE_MONITOR_EVENT_CREATED && event != G_FILE_MONITOR_EVENT_DELETED) {
		return;
	}
	if (p->rescan_timer) {
		g_source_remove(p->rescan_timer);
	}
	p->rescan_timer = g_timeout_add(700, rescan_later, p);
}

static void on_map(GtkWidget *widget, gpointer data) {
	struct devices_page *p = data;
	if (!p->devices) {
		rescan(p);
	}
}

/* ---------- the properties window ---------- */

static GtkWidget *text_label(const char *text, bool dim) {
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	if (dim) {
		gtk_widget_add_css_class(l, "dim-label");
	}
	return l;
}

static void grid_row(GtkWidget *grid, int *y, const char *name, const char *value) {
	if (!value || !*value) {
		return;
	}
	GtkWidget *n = text_label(name, true);
	gtk_label_set_selectable(GTK_LABEL(n), FALSE);
	gtk_widget_set_valign(n, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), n, 0, *y, 1, 1);
	GtkWidget *v = text_label(value, false);
	gtk_widget_set_hexpand(v, TRUE);
	gtk_grid_attach(GTK_GRID(grid), v, 1, *y, 1, 1);
	(*y)++;
}

static GtkWidget *tab_box(void) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(box, 14);
	gtk_widget_set_margin_end(box, 14);
	gtk_widget_set_margin_top(box, 14);
	gtk_widget_set_margin_bottom(box, 14);
	return box;
}

static GtkWidget *new_grid(void) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	return grid;
}

static GtkWidget *scrolled(GtkWidget *child) {
	GtkWidget *s = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
	gtk_widget_set_vexpand(s, TRUE);
	return s;
}

/* field: value lines of modinfo, for a module. */
static GHashTable *modinfo(const char *module) {
	GHashTable *info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (!module) {
		return info;
	}
	const char *argv[] = { "modinfo", module, NULL };
	char *out = NULL;
	if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			NULL, NULL, &out, NULL, NULL, NULL) && out) {
		char **lines = g_strsplit(out, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char *colon = strchr(lines[i], ':');
			if (colon) {
				char *key = g_strndup(lines[i], colon - lines[i]);
				// the first of a field: alias and parm come many times
				if (!g_hash_table_contains(info, key)) {
					g_hash_table_insert(info, key, g_strstrip(g_strdup(colon + 1)));
				} else {
					g_free(key);
				}
			}
		}
		g_strfreev(lines);
	}
	g_free(out);
	return info;
}

static char *package_of(const char *file) {
	if (!file || !*file || *file != '/') {
		return NULL;
	}
	const char *argv[] = { "pacman", "-Qo", file, NULL };
	char *out = NULL;
	int status = 1;
	g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
		NULL, NULL, &out, NULL, &status, NULL);
	char *package = NULL;
	// /usr/lib/modules/.../iwlwifi.ko.zst is owned by linux 7.2.11.arch1-1
	const char *by = out && status == 0 ? strstr(out, " is owned by ") : NULL;
	if (by) {
		package = g_strstrip(g_strdup(by + strlen(" is owned by ")));
	}
	g_free(out);
	return package;
}

struct dialog {
	struct devices_page *p;
	char *path;
	GtkWidget *window, *value;
	GPtrArray *props;
};

static void on_dialog_button(GtkButton *button, gpointer data) {
	struct dialog *dl = data;
	const char *what = g_object_get_data(G_OBJECT(button), "what");
	for (guint i = 0; i < dl->p->devices->len; i++) {
		struct tw_device *d = dl->p->devices->pdata[i];
		if (g_strcmp0(d->path, dl->path) == 0) {
			if (strcmp(what, "enable") == 0) {
				run_action(dl->p, d, what);
			} else {
				ask_disable(dl->p, d, what);
			}
			break;
		}
	}
	gtk_window_destroy(GTK_WINDOW(dl->window));
}

static void on_detail(GtkDropDown *dd, GParamSpec *spec, gpointer data) {
	struct dialog *dl = data;
	guint i = gtk_drop_down_get_selected(dd);
	const char *line = i < dl->props->len ? dl->props->pdata[i] : "";
	const char *tab = strchr(line, '\t');
	gtk_label_set_text(GTK_LABEL(dl->value), tab ? tab + 1 : "");
}

static void on_dialog_closed(GtkWidget *window, gpointer data) {
	struct dialog *dl = data;
	g_free(dl->path);
	g_ptr_array_unref(dl->props);
	g_free(dl);
}

static gboolean on_dialog_key(GtkEventControllerKey *key, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	if (keyval == GDK_KEY_Escape) {
		gtk_window_destroy(GTK_WINDOW(data));
		return TRUE;
	}
	return FALSE;
}

static GtkWidget *action_row(struct dialog *dl, const char *label, const char *what,
		const char *text, bool sensitive, const char *why) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *b = gtk_button_new_with_label(label);
	gtk_widget_set_size_request(b, 150, -1);
	gtk_widget_set_valign(b, GTK_ALIGN_START);
	gtk_widget_set_sensitive(b, sensitive);
	g_object_set_data(G_OBJECT(b), "what", (gpointer)what);
	g_signal_connect(b, "clicked", G_CALLBACK(on_dialog_button), dl);
	gtk_box_append(GTK_BOX(row), b);
	GtkWidget *t = text_label(sensitive || !why ? text : why, true);
	gtk_widget_set_hexpand(t, TRUE);
	gtk_box_append(GTK_BOX(row), t);
	return row;
}

static void properties_open(struct devices_page *p, struct tw_device *d) {
	struct dialog *dl = g_new0(struct dialog, 1);
	dl->p = p;
	dl->path = g_strdup(d->path);
	dl->props = g_ptr_array_ref(d->props);
	GtkWidget *window = gtk_window_new();
	dl->window = window;
	char *title = g_strdup_printf("%s Properties", d->name);
	gtk_window_set_title(GTK_WINDOW(window), title);
	g_free(title);
	gtk_window_set_transient_for(GTK_WINDOW(window), p->s->window);
	gtk_window_set_modal(GTK_WINDOW(window), TRUE);
	gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(window), 560, 600);
	g_signal_connect(window, "destroy", G_CALLBACK(on_dialog_closed), dl);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_dialog_key), window);
	gtk_widget_add_controller(window, keys);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(outer, 12);
	gtk_widget_set_margin_end(outer, 12);
	gtk_widget_set_margin_top(outer, 12);
	gtk_widget_set_margin_bottom(outer, 12);
	GtkWidget *notebook = gtk_notebook_new();
	gtk_widget_set_vexpand(notebook, TRUE);
	gtk_box_append(GTK_BOX(outer), notebook);

	const struct tw_dev_category *cat = tw_dev_category(d->category);

	// General
	GtkWidget *general = tab_box();
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *icon = gtk_image_new_from_icon_name(d->icon);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
	gtk_box_append(GTK_BOX(head), icon);
	GtkWidget *name = text_label(d->name, false);
	gtk_widget_add_css_class(name, "tw-heading");
	gtk_box_append(GTK_BOX(head), name);
	gtk_box_append(GTK_BOX(general), head);
	GtkWidget *grid = new_grid();
	int y = 0;
	grid_row(grid, &y, "Device type", cat->title);
	grid_row(grid, &y, "Manufacturer", d->manufacturer ? d->manufacturer : "Unknown");
	grid_row(grid, &y, "Location", d->location ? d->location : "Unknown");
	gtk_box_append(GTK_BOX(general), grid);
	GtkWidget *frame = gtk_frame_new("Device status");
	GtkWidget *status = text_label(d->status, false);
	gtk_widget_set_margin_start(status, 10);
	gtk_widget_set_margin_end(status, 10);
	gtk_widget_set_margin_top(status, 8);
	gtk_widget_set_margin_bottom(status, 8);
	gtk_frame_set_child(GTK_FRAME(frame), status);
	gtk_box_append(GTK_BOX(general), frame);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), general, gtk_label_new("General"));

	// Driver
	GtkWidget *driver = tab_box();
	GHashTable *info = modinfo(d->module ? d->module : d->driver && !strchr(d->driver, ',') ?
		d->driver : NULL);
	const char *file = g_hash_table_lookup(info, "filename");
	char *package = package_of(file);
	grid = new_grid();
	y = 0;
	grid_row(grid, &y, "Driver", d->driver ? d->driver : "None");
	grid_row(grid, &y, "Kernel module", d->module ? d->module : d->driver ?
		"Built into the kernel" : NULL);
	char *version = g_strdup(g_hash_table_lookup(info, "version"));
	if (!version && d->driver) {
		char *osrelease = NULL;
		g_file_get_contents("/proc/sys/kernel/osrelease", &osrelease, NULL, NULL);
		version = osrelease ? g_strdup_printf("That of the kernel, %s", g_strstrip(osrelease)) : NULL;
		g_free(osrelease);
	}
	grid_row(grid, &y, "Driver version", version);
	g_free(version);
	grid_row(grid, &y, "Driver provider", package ? package : g_hash_table_lookup(info, "author"));
	grid_row(grid, &y, "File", file);
	grid_row(grid, &y, "License", g_hash_table_lookup(info, "license"));
	grid_row(grid, &y, "Description", g_hash_table_lookup(info, "description"));
	const char *signer = g_hash_table_lookup(info, "signer");
	grid_row(grid, &y, "Digital signer", signer ? signer : file ? "Not signed" : NULL);
	if (d->firmware->len) {
		GString *fw = g_string_new(NULL);
		for (guint i = 0; i < d->firmware->len; i++) {
			g_string_append_printf(fw, "%s%s", i ? "\n" : "", (char *)d->firmware->pdata[i]);
		}
		grid_row(grid, &y, "Firmware loaded", fw->str);
		g_string_free(fw, TRUE);
	}
	gtk_box_append(GTK_BOX(driver), grid);
	g_free(package);
	g_hash_table_unref(info);
	char *why = check_device(d);
	bool can = strcmp(why, "ok") == 0;
	bool off = d->problem == TW_DEV_DISABLED;
	gtk_box_append(GTK_BOX(driver), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	if (off) {
		gtk_box_append(GTK_BOX(driver), action_row(dl, "Enable Device", "enable",
			"Lets the driver take the device again.", d->action != NULL && !p->busy, NULL));
	} else {
		gtk_box_append(GTK_BOX(driver), action_row(dl, "Disable Device", "disable",
			"Stops the device from working, also after a restart, until it is enabled again.",
			can && !p->busy, why));
		gtk_box_append(GTK_BOX(driver), action_row(dl, "Restart Device", "restart",
			"The driver lets go of the device and takes it again, as when it is plugged in "
			"anew: what helps a device that hangs.", can && !p->busy, why));
	}
	g_free(why);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), driver, gtk_label_new("Driver"));

	// Details
	GtkWidget *details = tab_box();
	GtkStringList *names = gtk_string_list_new(NULL);
	for (guint i = 0; i < d->props->len; i++) {
		char *line = g_strdup(d->props->pdata[i]);
		char *tab = strchr(line, '\t');
		if (tab) {
			*tab = '\0';
		}
		gtk_string_list_append(names, line);
		g_free(line);
	}
	GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), TRUE);
	GtkWidget *pick = gtk_label_new("Property");
	gtk_label_set_xalign(GTK_LABEL(pick), 0);
	gtk_box_append(GTK_BOX(details), pick);
	gtk_box_append(GTK_BOX(details), dd);
	dl->value = text_label("", false);
	gtk_widget_set_valign(dl->value, GTK_ALIGN_START);
	GtkWidget *value_frame = gtk_frame_new("Value");
	gtk_widget_set_margin_start(dl->value, 10);
	gtk_widget_set_margin_top(dl->value, 8);
	gtk_widget_set_margin_bottom(dl->value, 8);
	gtk_frame_set_child(GTK_FRAME(value_frame), dl->value);
	gtk_box_append(GTK_BOX(details), value_frame);
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_detail), dl);
	on_detail(GTK_DROP_DOWN(dd), NULL, dl);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), details, gtk_label_new("Details"));

	// Resources
	GtkWidget *resources = tab_box();
	if (d->resources->len) {
		grid = new_grid();
		y = 0;
		GtkWidget *h1 = text_label("Resource type", true), *h2 = text_label("Setting", true);
		gtk_grid_attach(GTK_GRID(grid), h1, 0, y, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), h2, 1, y++, 1, 1);
		for (guint i = 0; i < d->resources->len; i++) {
			char **f = g_strsplit(d->resources->pdata[i], "\t", 2);
			grid_row(grid, &y, f[0], f[1]);
			g_strfreev(f);
		}
		gtk_box_append(GTK_BOX(resources), scrolled(grid));
	} else {
		gtk_box_append(GTK_BOX(resources), text_label("This device uses no resources of its "
			"own: no interrupt, no memory or I/O range.", true));
	}
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), resources, gtk_label_new("Resources"));

	// Events
	GtkWidget *events = tab_box();
	if (d->events->len) {
		GString *text = g_string_new(NULL);
		for (guint i = 0; i < d->events->len; i++) {
			g_string_append_printf(text, "%s%s", i ? "\n" : "", (char *)d->events->pdata[i]);
		}
		GtkWidget *log = text_label(text->str, false);
		gtk_widget_add_css_class(log, "monospace");
		gtk_label_set_wrap_mode(GTK_LABEL(log), PANGO_WRAP_WORD_CHAR);
		gtk_widget_set_valign(log, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(events), text_label("What the kernel said about this device "
			"since the computer started:", true));
		gtk_box_append(GTK_BOX(events), scrolled(log));
		g_string_free(text, TRUE);
	} else {
		gtk_box_append(GTK_BOX(events), text_label("The kernel said nothing about this device "
			"since the computer started.", true));
	}
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), events, gtk_label_new("Events"));

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	GtkWidget *close = gtk_button_new_with_label("Close");
	g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), window);
	gtk_box_append(GTK_BOX(buttons), close);
	gtk_box_append(GTK_BOX(outer), buttons);
	gtk_window_set_child(GTK_WINDOW(window), outer);
	gtk_window_set_default_widget(GTK_WINDOW(window), close);
	gtk_window_present(GTK_WINDOW(window));
}

/* ---------- the page ---------- */

static GtkWidget *tool_button(struct devices_page *p, const char *icon, const char *label,
		const char *what) {
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
	gtk_box_append(GTK_BOX(box), gtk_label_new(label));
	gtk_button_set_child(GTK_BUTTON(b), box);
	g_object_set_data(G_OBJECT(b), "what", (gpointer)what);
	g_signal_connect(b, "clicked", G_CALLBACK(on_toolbar), p);
	return b;
}

GtkWidget *devices_page_new(struct settings *s) {
	struct devices_page *p = g_new0(struct devices_page, 1);
	p->s = s;
	p->open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	p->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	GtkWidget *content;
	GtkWidget *page = ui_page("Device manager", "Every device Linux found in this computer, "
		"sorted as the Device Manager of Windows sorts them, with its driver and whether it "
		"works. Double-click one for its properties: its driver, its details, the resources it "
		"uses and what the kernel said about it.", &content);

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(bar, 10);
	p->properties = tool_button(p, "document-properties-symbolic", "Properties", "properties");
	p->disable = tool_button(p, "action-unavailable-symbolic", "Disable", "disable");
	p->enable = tool_button(p, "object-select-symbolic", "Enable", "enable");
	p->restart = tool_button(p, "view-refresh-symbolic", "Restart", "restart");
	GtkWidget *scan = tool_button(p, "system-search-symbolic", "Scan for hardware changes",
		"rescan");
	gtk_box_append(GTK_BOX(bar), p->properties);
	gtk_box_append(GTK_BOX(bar), p->disable);
	gtk_box_append(GTK_BOX(bar), p->enable);
	gtk_box_append(GTK_BOX(bar), p->restart);
	GtkWidget *space = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(space, TRUE);
	gtk_box_append(GTK_BOX(bar), space);
	gtk_box_append(GTK_BOX(bar), scan);
	gtk_box_append(GTK_BOX(content), bar);

	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	p->summary = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->summary), 0);
	gtk_widget_set_hexpand(p->summary, TRUE);
	gtk_widget_add_css_class(p->summary, "dim-label");
	gtk_box_append(GTK_BOX(line), p->summary);
	p->hidden = gtk_check_button_new_with_label("Show hidden devices");
	gtk_widget_set_tooltip_text(p->hidden, "The devices of the computer's own software (loop "
		"devices, virtual networks), the jacks of the sound cards, the platform's own parts");
	g_signal_connect(p->hidden, "toggled", G_CALLBACK(on_hidden), p);
	gtk_box_append(GTK_BOX(line), p->hidden);
	gtk_box_append(GTK_BOX(content), line);

	p->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_SINGLE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->list), FALSE);
	gtk_widget_add_css_class(p->list, "tw-group");
	g_signal_connect(p->list, "row-activated", G_CALLBACK(on_activated), p);
	g_signal_connect(p->list, "row-selected", G_CALLBACK(on_selected), p);
	GtkGesture *right = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
	g_signal_connect(right, "pressed", G_CALLBACK(on_right_click), p);
	gtk_widget_add_controller(p->list, GTK_EVENT_CONTROLLER(right));
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "pressed", G_CALLBACK(on_click), p);
	gtk_widget_add_controller(p->list, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), p);
	gtk_widget_add_controller(p->list, keys);
	gtk_box_append(GTK_BOX(content), p->list);
	g_signal_connect(p->list, "map", G_CALLBACK(on_map), p);

	const char *r = g_getenv("TILEWIN_DEVICES_ROOT");
	char *udev_dir = g_strdup_printf("%s/run/udev/data", r ? r : "");
	GFile *dir = g_file_new_for_path(udev_dir);
	p->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, NULL);
	if (p->monitor) {
		g_signal_connect(p->monitor, "changed", G_CALLBACK(on_udev_changed), p);
	}
	g_object_unref(dir);
	g_free(udev_dir);
	return page;
}
