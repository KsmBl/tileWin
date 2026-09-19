#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>
#include "settings.h"
#include "tw_desktop.h"

/*
 * Apps page, like "Default apps" and "Startup apps" of Windows:
 *  - default apps for web links, email, folders, text, pictures, music,
 *    videos and PDFs (mimeapps.list), and the terminal, file manager and task
 *    manager of the tileWin shortcuts and menus ($term, $filemanager and
 *    $taskmanager in common.conf). Everything else points here instead of
 *    naming a program of its own.
 *  - startup apps: the XDG autostart entries tileWin starts
 *    (~/.config/autostart and /etc/xdg/autostart). Turning a system entry off
 *    writes a copy with Hidden=true to ~/.config/autostart.
 */

struct category {
	const char *title, *subtitle;
	const char *types[5];
	const char *variable;     // common.conf variable that follows the choice
	const char *app_category; // desktop category listing the apps (no MIME type)
};

static const struct category categories[] = {
	{ "Web browser", NULL,
		{ "x-scheme-handler/http", "x-scheme-handler/https", "text/html", NULL }, NULL, NULL },
	{ "Email", NULL, { "x-scheme-handler/mailto", NULL }, NULL, NULL },
	{ "File manager", "Win+E, \"File Explorer\" in the start button menu and every folder",
		{ "inode/directory", NULL }, "$filemanager", NULL },
	{ "Terminal", "Win+Return, \"Terminal\" in the menus and \"Open terminal here\" on the desktop",
		{ NULL }, "$term", "TerminalEmulator" },
	{ "Task manager", "Ctrl+Shift+Esc, \"Task Manager\" in the menus and the link in the "
		"CPU and memory flyouts", { NULL }, "$taskmanager", "Monitor" },
	{ "Text editor", NULL, { "text/plain", NULL }, NULL, NULL },
	{ "Pictures", NULL, { "image/png", "image/jpeg", "image/gif", "image/webp", NULL }, NULL, NULL },
	{ "Music", NULL, { "audio/mpeg", "audio/flac", "audio/ogg", "audio/x-wav", NULL }, NULL, NULL },
	{ "Videos", NULL, { "video/mp4", "video/x-matroska", "video/webm", NULL }, NULL, NULL },
	{ "PDF documents", NULL, { "application/pdf", NULL }, NULL, NULL },
};

#define CATEGORY_COUNT G_N_ELEMENTS(categories)
#define DESKTOP "tileWin"

struct apps_page {
	struct settings *s;
	bool updating;
	GtkWidget *dropdowns[CATEGORY_COUNT];
	GPtrArray *ids[CATEGORY_COUNT]; // char *, "" for "not set"
	GtkWidget *startup_group, *startup_empty;
	GPtrArray *startup_rows; // GtkWidget *
};

/* ---------- default apps ---------- */

static char *first_word(const char *command) {
	if (!command) {
		return NULL;
	}
	while (*command == ' ' || *command == '\t') {
		command++;
	}
	const char *end = command;
	while (*end && *end != ' ' && *end != '\t') {
		end++;
	}
	char *word = g_strndup(command, end - command);
	char *base = g_path_get_basename(word);
	g_free(word);
	return base;
}

static char *variable_value(struct settings *s, const char *variable) {
	struct cstmt *stmt = confdoc_child(s->common->root, "set", variable);
	return stmt ? cstmt_join(stmt, 1) : NULL;
}

static void add_app(GPtrArray *ids, GtkStringList *names, GAppInfo *info) {
	const char *id = g_app_info_get_id(info);
	if (!id) {
		return;
	}
	for (guint i = 0; i < ids->len; i++) {
		if (strcmp(ids->pdata[i], id) == 0) {
			return;
		}
	}
	g_ptr_array_add(ids, g_strdup(id));
	gtk_string_list_append(names, g_app_info_get_name(info));
}

static void fill_category(struct apps_page *p, guint index) {
	const struct category *c = &categories[index];
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	GtkStringList *names = gtk_string_list_new(NULL);
	char *current = NULL;
	if (c->app_category) {
		GList *all = g_app_info_get_all();
		char *command = variable_value(p->s, c->variable);
		char *program = first_word(command);
		for (GList *l = all; l; l = l->next) {
			GAppInfo *info = l->data;
			const char *cats = G_IS_DESKTOP_APP_INFO(info) ?
				g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(info)) : NULL;
			if (!cats || !strstr(cats, c->app_category)) {
				continue;
			}
			add_app(ids, names, info);
			char *exe = first_word(g_app_info_get_executable(info));
			if (!current && exe && program && strcmp(exe, program) == 0) {
				current = g_strdup(g_app_info_get_id(info));
			}
			g_free(exe);
		}
		g_free(program);
		g_free(command);
		g_list_free_full(all, g_object_unref);
	} else {
		GList *all = g_app_info_get_all_for_type(c->types[0]);
		for (GList *l = all; l; l = l->next) {
			add_app(ids, names, l->data);
		}
		g_list_free_full(all, g_object_unref);
		GAppInfo *def = g_app_info_get_default_for_type(c->types[0], FALSE);
		if (def) {
			current = g_strdup(g_app_info_get_id(def));
			add_app(ids, names, def);
			g_object_unref(def);
		}
	}
	guint selected = GTK_INVALID_LIST_POSITION;
	for (guint i = 0; current && i < ids->len; i++) {
		if (strcmp(ids->pdata[i], current) == 0) {
			selected = i;
		}
	}
	if (selected == GTK_INVALID_LIST_POSITION) {
		g_ptr_array_insert(ids, 0, g_strdup(""));
		gtk_string_list_splice(names, 0, 0, (const char *const[]){ ids->len > 1 ?
			"Not set" : "No app installed", NULL });
		selected = 0;
	}
	g_free(current);
	p->updating = true;
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->dropdowns[index]), G_LIST_MODEL(names));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->dropdowns[index]), selected);
	gtk_widget_set_sensitive(p->dropdowns[index], ids->len > 1 || *(char *)ids->pdata[0]);
	p->updating = false;
	g_object_unref(names);
	if (p->ids[index]) {
		g_ptr_array_unref(p->ids[index]);
	}
	p->ids[index] = ids;
}

static void on_default(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct apps_page *p = data;
	if (p->updating) {
		return;
	}
	guint index = GPOINTER_TO_UINT(g_object_get_data(dropdown, "category"));
	const struct category *c = &categories[index];
	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (!p->ids[index] || selected >= p->ids[index]->len) {
		return;
	}
	const char *id = p->ids[index]->pdata[selected];
	GDesktopAppInfo *info = *id ? g_desktop_app_info_new(id) : NULL;
	if (!info) {
		return;
	}
	GError *error = NULL;
	for (int i = 0; c->types[i] && !error; i++) {
		g_app_info_set_as_default_for_type(G_APP_INFO(info), c->types[i], &error);
	}
	if (error) {
		settings_status(p->s, "Couldn't change the %s: %s", c->title, error->message);
		g_error_free(error);
	} else {
		if (c->variable) {
			char *program = first_word(g_app_info_get_executable(G_APP_INFO(info)));
			char *wrapped = NULL;
			if (program && g_desktop_app_info_get_boolean(info, "Terminal")) {
				// a program that runs inside a terminal, such as btop, needs one
				const char *term = cstmt_arg(confdoc_child(p->s->taskbar->root,
					"terminal", NULL), 0);
				wrapped = g_strdup_printf("%s %s", term && *term ? term : "xfce4-terminal -x",
					program);
			}
			if (program) {
				char *args = g_strdup_printf("%s %s", c->variable,
					wrapped ? wrapped : program);
				confdoc_set(p->s->common, p->s->common->root, "set", c->variable, args);
				settings_common_changed(p->s, true);
				g_free(args);
			}
			g_free(wrapped);
			g_free(program);
		}
		settings_status(p->s, "%s: %s", c->title, g_app_info_get_name(G_APP_INFO(info)));
	}
	g_object_unref(info);
}

/* ---------- startup apps ---------- */

static char *user_autostart_dir(void) {
	return g_build_filename(g_get_user_config_dir(), "autostart", NULL);
}

static bool desktop_in_list(char **list) {
	for (int i = 0; list && list[i]; i++) {
		if (g_ascii_strcasecmp(list[i], DESKTOP) == 0) {
			return true;
		}
	}
	return false;
}

/* Paths of every autostart entry by file name; user entries replace system ones. */
static GHashTable *collect_entries(GHashTable **system_paths) {
	GHashTable *paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	*system_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *const *system = g_get_system_config_dirs();
	for (int i = -1; i < 0 || system[i]; i++) {
		char *dir = i < 0 ? user_autostart_dir() : g_build_filename(system[i], "autostart", NULL);
		GDir *d = g_dir_open(dir, 0, NULL);
		const char *name;
		while (d && (name = g_dir_read_name(d))) {
			if (!g_str_has_suffix(name, ".desktop")) {
				continue;
			}
			char *path = g_build_filename(dir, name, NULL);
			if (i >= 0 && !g_hash_table_contains(*system_paths, name)) {
				g_hash_table_insert(*system_paths, g_strdup(name), g_strdup(path));
			}
			if (!g_hash_table_contains(paths, name)) {
				g_hash_table_insert(paths, g_strdup(name), path);
			} else {
				g_free(path);
			}
		}
		if (d) {
			g_dir_close(d);
		}
		g_free(dir);
	}
	return paths;
}

static void rebuild_startup(struct apps_page *p);

static bool save_enabled(const char *name, const char *source, bool enabled, GError **error) {
	GKeyFile *kf = g_key_file_new();
	bool ok = g_key_file_load_from_file(kf, source, G_KEY_FILE_KEEP_COMMENTS |
		G_KEY_FILE_KEEP_TRANSLATIONS, error);
	if (ok) {
		g_key_file_set_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, "Hidden", !enabled);
		if (enabled) {
			g_key_file_remove_key(kf, G_KEY_FILE_DESKTOP_GROUP, "X-GNOME-Autostart-enabled",
				NULL);
		}
		char *dir = user_autostart_dir();
		g_mkdir_with_parents(dir, 0755);
		char *dest = g_build_filename(dir, name, NULL);
		ok = g_key_file_save_to_file(kf, dest, error);
		g_free(dest);
		g_free(dir);
	}
	g_key_file_unref(kf);
	return ok;
}

static gboolean on_startup_switch(GtkSwitch *widget, gboolean state, gpointer data) {
	struct apps_page *p = data;
	if (p->updating) {
		return FALSE;
	}
	const char *name = g_object_get_data(G_OBJECT(widget), "name");
	const char *path = g_object_get_data(G_OBJECT(widget), "path");
	GError *error = NULL;
	if (!save_enabled(name, path, state, &error)) {
		settings_status(p->s, "Couldn't change %s: %s", name, error ? error->message : "?");
		g_clear_error(&error);
		return TRUE;
	}
	settings_status(p->s, state ? "%s starts with tileWin" : "%s no longer starts with tileWin",
		(const char *)g_object_get_data(G_OBJECT(widget), "title"));
	return FALSE;
}

static void on_startup_remove(GtkButton *button, gpointer data) {
	struct apps_page *p = data;
	const char *path = g_object_get_data(G_OBJECT(button), "path");
	g_unlink(path);
	rebuild_startup(p);
}

static void on_startup_add(const char *id, gpointer data) {
	struct apps_page *p = data;
	const struct tw_desktop_entry *entry = ui_find_app(id);
	if (!entry || !entry->path) {
		return;
	}
	char *dir = user_autostart_dir();
	g_mkdir_with_parents(dir, 0755);
	char *name = g_str_has_suffix(id, ".desktop") ? g_strdup(id) : g_strconcat(id, ".desktop", NULL);
	char *dest = g_build_filename(dir, name, NULL);
	char *contents = NULL;
	gsize length = 0;
	if (g_file_get_contents(entry->path, &contents, &length, NULL) &&
			g_file_set_contents(dest, contents, length, NULL)) {
		settings_status(p->s, "%s starts with tileWin now", entry->name);
	}
	g_free(contents);
	g_free(dest);
	g_free(name);
	g_free(dir);
	rebuild_startup(p);
}

static int name_cmp(gconstpointer a, gconstpointer b) {
	return g_utf8_collate(*(const char **)a, *(const char **)b);
}

static void rebuild_startup(struct apps_page *p) {
	for (guint i = 0; i < p->startup_rows->len; i++) {
		gtk_list_box_remove(GTK_LIST_BOX(p->startup_group), p->startup_rows->pdata[i]);
	}
	g_ptr_array_set_size(p->startup_rows, 0);
	GHashTable *system = NULL;
	GHashTable *paths = collect_entries(&system);
	GPtrArray *names = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, paths);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		g_ptr_array_add(names, key);
	}
	g_ptr_array_sort(names, name_cmp);
	int shown = 0;
	p->updating = true;
	for (guint i = 0; i < names->len; i++) {
		const char *name = names->pdata[i];
		const char *path = g_hash_table_lookup(paths, name);
		GKeyFile *kf = g_key_file_new();
		if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
			g_key_file_unref(kf);
			continue;
		}
		const char *group = G_KEY_FILE_DESKTOP_GROUP;
		char **only = g_key_file_get_string_list(kf, group, "OnlyShowIn", NULL, NULL);
		char **not = g_key_file_get_string_list(kf, group, "NotShowIn", NULL, NULL);
		bool other_desktop = (only && !desktop_in_list(only)) || desktop_in_list(not);
		g_strfreev(only);
		g_strfreev(not);
		char *title = g_key_file_get_locale_string(kf, group, "Name", NULL, NULL);
		if (other_desktop || !title) {
			g_free(title);
			g_key_file_unref(kf);
			continue;
		}
		char *comment = g_key_file_get_locale_string(kf, group, "Comment", NULL, NULL);
		char *icon = g_key_file_get_string(kf, group, "Icon", NULL);
		GError *error = NULL;
		bool enabled = !g_key_file_get_boolean(kf, group, "Hidden", NULL);
		bool gnome = g_key_file_get_boolean(kf, group, "X-GNOME-Autostart-enabled", &error);
		if (!error && !gnome) {
			enabled = false;
		}
		g_clear_error(&error);

		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		bool user_only = !g_hash_table_contains(system, name);
		if (user_only) {
			GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
			gtk_widget_set_tooltip_text(remove, "Remove from the startup apps");
			gtk_widget_add_css_class(remove, "flat");
			g_object_set_data_full(G_OBJECT(remove), "path", g_strdup(path), g_free);
			g_signal_connect(remove, "clicked", G_CALLBACK(on_startup_remove), p);
			gtk_box_append(GTK_BOX(box), remove);
		}
		GtkWidget *toggle = gtk_switch_new();
		gtk_switch_set_active(GTK_SWITCH(toggle), enabled);
		gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
		g_object_set_data_full(G_OBJECT(toggle), "name", g_strdup(name), g_free);
		g_object_set_data_full(G_OBJECT(toggle), "path", g_strdup(path), g_free);
		g_object_set_data_full(G_OBJECT(toggle), "title", g_strdup(title), g_free);
		g_signal_connect(toggle, "state-set", G_CALLBACK(on_startup_switch), p);
		gtk_box_append(GTK_BOX(box), toggle);
		GtkWidget *row = ui_row(p->startup_group, title, comment, box);
		gtk_box_prepend(GTK_BOX(ui_row_box(row)), ui_app_icon(icon, 28));
		g_ptr_array_add(p->startup_rows, row);
		shown++;
		g_free(title);
		g_free(comment);
		g_free(icon);
		g_key_file_unref(kf);
	}
	p->updating = false;
	gtk_widget_set_visible(p->startup_empty, shown == 0);
	g_ptr_array_free(names, TRUE);
	g_hash_table_unref(paths);
	g_hash_table_unref(system);
	(void)value;
}

void apps_page_refresh(struct settings *s) {
	struct apps_page *p = s->apps_page;
	if (!p) {
		return;
	}
	for (guint i = 0; i < CATEGORY_COUNT; i++) {
		fill_category(p, i);
	}
	rebuild_startup(p);
}

GtkWidget *apps_page_new(struct settings *s) {
	struct apps_page *p = g_new0(struct apps_page, 1);
	p->s = s;
	p->startup_rows = g_ptr_array_new();
	s->apps_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Apps",
		"Which apps open links and files, and which apps start with tileWin.", &content);

	GtkWidget *defaults = ui_group(content, "Default apps", NULL);
	for (guint i = 0; i < CATEGORY_COUNT; i++) {
		p->dropdowns[i] = gtk_drop_down_new(NULL, NULL);
		gtk_widget_set_size_request(p->dropdowns[i], 280, -1);
		g_object_set_data(G_OBJECT(p->dropdowns[i]), "category", GUINT_TO_POINTER(i));
		g_signal_connect(p->dropdowns[i], "notify::selected", G_CALLBACK(on_default), p);
		ui_row(defaults, categories[i].title, categories[i].subtitle, p->dropdowns[i]);
	}

	p->startup_group = ui_group(content, "Startup apps",
		"Apps that start when you log in (XDG autostart).");
	p->startup_empty = ui_row(p->startup_group, "No startup apps", NULL, NULL);
	GtkWidget *add_group = ui_group(content, NULL, NULL);
	ui_row(add_group, "Add a startup app", NULL,
		ui_app_picker("Add…", on_startup_add, p));

	apps_page_refresh(s);
	return page;
}
