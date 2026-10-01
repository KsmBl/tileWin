#include <glib/gstdio.h>
#include "settings.h"

/*
 * Backup page: all of tileWin's settings saved to one file, and brought back
 * from one. Restoring keeps the settings it replaces first, listed here to go
 * back to.
 */

#define AUTO_SHOWN 5 // the newest automatic copies listed

struct backup_page {
	struct settings *s;
	GtkWidget *auto_group, *auto_list;
	char *pending; // the backup a restore waits to be confirmed for
};

static void refresh_auto(struct backup_page *p);

static void restore_now(struct backup_page *p, const char *file) {
	settings_flush(p->s); // nothing still waiting to be saved writes over the backup
	char *saved_as = NULL, *error = NULL;
	if (!tw_backup_restore(file, &saved_as, &error)) {
		settings_status(p->s, "%s", error ? error : "The backup could not be restored.");
		g_free(error);
		return;
	}
	if (settings_command(p->s, "reload")) {
		settings_status(p->s, "Restored the settings and reloaded tileWin; the ones before "
			"are kept below");
	} else {
		settings_status(p->s, "Restored the settings; the ones before are kept below");
	}
	g_free(saved_as);
	refresh_auto(p);
}

static void on_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
	struct backup_page *p = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	if (button == 1 && p->pending) {
		restore_now(p, p->pending);
	}
	g_clear_pointer(&p->pending, g_free);
}

/* Asks before the settings are replaced; when is set for an automatic copy. */
static void confirm_restore(struct backup_page *p, const char *file, const char *when) {
	g_free(p->pending);
	p->pending = g_strdup(file);
	char *name = g_path_get_basename(file);
	GtkAlertDialog *dialog = when ?
		gtk_alert_dialog_new("Go back to the settings of %s?", when) :
		gtk_alert_dialog_new("Restore the settings from %s?", name);
	gtk_alert_dialog_set_detail(dialog, "All of tileWin's settings are replaced by the ones "
		"in the backup. The settings you have now are kept first, so you can go back to them "
		"here.");
	gtk_alert_dialog_set_buttons(dialog, (const char *const[]){ "Cancel", "Restore", NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 1);
	gtk_alert_dialog_choose(dialog, p->s->window, NULL, on_confirmed, p);
	g_object_unref(dialog);
	g_free(name);
}

static GListModel *archive_filters(void) {
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "tileWin backups (.tar.gz)");
	gtk_file_filter_add_pattern(filter, "*.tar.gz");
	gtk_file_filter_add_pattern(filter, "*.tgz");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	g_object_unref(filter);
	return G_LIST_MODEL(filters);
}

static void on_save_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct backup_page *p = data;
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file) {
		return;
	}
	char *path = g_file_get_path(file);
	g_object_unref(file);
	settings_flush(p->s);
	char *error = NULL;
	if (path && tw_backup_save(path, &error)) {
		settings_status(p->s, "Saved all settings to %s", path);
	} else {
		settings_status(p->s, "%s", error ? error : "The backup could not be saved.");
	}
	g_free(error);
	g_free(path);
}

static void on_save(GtkButton *button, gpointer data) {
	struct backup_page *p = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Save a backup of the settings");
	GDateTime *now = g_date_time_new_now_local();
	char *name = g_date_time_format(now, "tileWin-settings-%Y-%m-%d.tar.gz");
	g_date_time_unref(now);
	gtk_file_dialog_set_initial_name(dialog, name);
	g_free(name);
	GListModel *filters = archive_filters();
	gtk_file_dialog_set_filters(dialog, filters);
	g_object_unref(filters);
	gtk_file_dialog_save(dialog, p->s->window, NULL, on_save_chosen, p);
	g_object_unref(dialog);
}

static void on_open_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct backup_page *p = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file) {
		return;
	}
	char *path = g_file_get_path(file);
	g_object_unref(file);
	if (path) {
		confirm_restore(p, path, NULL);
	}
	g_free(path);
}

static void on_restore(GtkButton *button, gpointer data) {
	struct backup_page *p = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Restore the settings from a backup");
	GListModel *filters = archive_filters();
	gtk_file_dialog_set_filters(dialog, filters);
	g_object_unref(filters);
	gtk_file_dialog_open(dialog, p->s->window, NULL, on_open_chosen, p);
	g_object_unref(dialog);
}

static void on_restore_auto(GtkButton *button, gpointer data) {
	confirm_restore(data, g_object_get_data(G_OBJECT(button), "path"),
		g_object_get_data(G_OBJECT(button), "when"));
}

static int newest_first(gconstpointer a, gconstpointer b) {
	return -strcmp(*(char *const *)a, *(char *const *)b);
}

/* The automatic copies, newest first, each with a button to go back to it. */
static void refresh_auto(struct backup_page *p) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->auto_list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->auto_list), child);
	}
	char *dir = tw_backup_auto_dir();
	GDir *d = dir ? g_dir_open(dir, 0, NULL) : NULL;
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *name;
	while (d && (name = g_dir_read_name(d))) {
		if (g_str_has_prefix(name, "before-restore-") && g_str_has_suffix(name, ".tar.gz")) {
			g_ptr_array_add(names, g_strdup(name));
		}
	}
	if (d) {
		g_dir_close(d);
	}
	g_ptr_array_sort(names, newest_first);
	for (guint i = 0; i < names->len && i < AUTO_SHOWN; i++) {
		// before-restore-2026-10-01-193000.tar.gz -> 2026-10-01 19:30
		const char *n = names->pdata[i];
		char date[32] = "";
		int y, mo, da, h, mi;
		if (sscanf(n, "before-restore-%4d-%2d-%2d-%2d%2d", &y, &mo, &da, &h, &mi) == 5) {
			snprintf(date, sizeof(date), "%04d-%02d-%02d %02d:%02d", y, mo, da, h, mi);
		}
		char *title = g_strdup_printf("Before the restore of %s", *date ? date : n);
		GtkWidget *button = gtk_button_new_with_label("Go back to these…");
		char *path = g_build_filename(dir, n, NULL);
		g_object_set_data_full(G_OBJECT(button), "path", path, g_free);
		g_object_set_data_full(G_OBJECT(button), "when", g_strdup(*date ? date : n), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(on_restore_auto), p);
		ui_row(p->auto_list, title, NULL, button);
		g_free(title);
	}
	gtk_widget_set_visible(p->auto_group, names->len > 0);
	g_ptr_array_unref(names);
	g_free(dir);
}

GtkWidget *backup_page_new(struct settings *s) {
	struct backup_page *p = g_new0(struct backup_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Backup", "All of tileWin's settings in one file: the theme, "
		"the taskbar, the shortcuts, the desktop and the rest.", &content);

	GtkWidget *group = ui_group(content, "Your settings", NULL);
	GtkWidget *save = gtk_button_new_with_label("Save a backup…");
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), p);
	ui_row(group, "Back up", "Keep them safe, or take them to another computer", save);
	GtkWidget *restore = gtk_button_new_with_label("Restore a backup…");
	g_signal_connect(restore, "clicked", G_CALLBACK(on_restore), p);
	ui_row(group, "Restore", "Replace the settings with the ones of a backup", restore);

	p->auto_list = ui_group(content, "Before a restore",
		"The settings as they were before each restore, to go back to.");
	p->auto_group = gtk_widget_get_parent(p->auto_list);
	refresh_auto(p);
	return page;
}
