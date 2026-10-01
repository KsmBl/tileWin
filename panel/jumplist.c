#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "panel.h"
#include "stringop.h"
#include "tw_desktop.h"

/*
 * Jump lists, as on Windows 7: the right-click menu of a taskbar button starts
 * with the files the app opened last (from recently-used.xbel, where GTK and
 * KDE apps note them) and the tasks its desktop entry offers ("New private
 * window", "Compose message"), each opened with that app.
 */

#define RECENT_MAX 6

/* sh -c safe single quotes */
static char *quote(const char *s) {
	GString *out = g_string_new("'");
	for (const char *p = s; *p; p++) {
		if (*p == '\'') {
			g_string_append(out, "'\\''");
		} else {
			g_string_append_c(out, *p);
		}
	}
	g_string_append_c(out, '\'');
	char *result = strdup(out->str);
	g_string_free(out, TRUE);
	return result;
}

/*
 * The Exec line of an entry with its field codes filled in: the file (or URI)
 * for %f %F %u %U, the rest dropped. Without such a code the file goes at the
 * end, which is what most apps take.
 */
static char *exec_with(const char *exec, const char *path, const char *uri) {
	GString *out = g_string_new(NULL);
	bool used = false;
	for (const char *p = exec; *p; p++) {
		if (*p != '%' || !p[1]) {
			g_string_append_c(out, *p);
			continue;
		}
		char code = *++p;
		if (code == '%') {
			g_string_append_c(out, '%');
		} else if ((code == 'f' || code == 'F' || code == 'u' || code == 'U') && !used) {
			char *q = quote(code == 'u' || code == 'U' ? uri : path);
			g_string_append(out, q);
			free(q);
			used = true;
		}
	}
	if (!used && path) {
		char *q = quote(path);
		g_string_append_c(out, ' ');
		g_string_append(out, q);
		free(q);
	}
	char *result = strdup(out->str);
	g_string_free(out, TRUE);
	return result;
}

static char *strip_codes(const char *exec) {
	struct tw_desktop_entry e = { .exec = (char *)exec };
	return tw_desktop_exec_command(&e);
}

/* The program an Exec line starts: "/usr/bin/mousepad %U" is "mousepad". */
static char *program_of(const char *exec) {
	if (!exec) {
		return NULL;
	}
	while (*exec == ' ' || *exec == '\'' || *exec == '"') {
		exec++;
	}
	if (strncmp(exec, "env ", 4) == 0) {
		// "env VAR=value program"
		exec += 4;
		while (*exec) {
			const char *space = strchr(exec, ' ');
			if (!space) {
				break;
			}
			if (!memchr(exec, '=', space - exec)) {
				break;
			}
			exec = space + 1;
		}
	}
	size_t len = strcspn(exec, " '\"");
	char *word = strndup(exec, len);
	char *slash = strrchr(word, '/');
	char *name = strdup(slash ? slash + 1 : word);
	free(word);
	return name;
}

/* Whether an application named in the bookmark file is this desktop entry. */
static bool same_app(const struct tw_desktop_entry *e, const char *app, const char *exec) {
	if (e->name && strcasecmp(app, e->name) == 0) {
		return true;
	}
	size_t idlen = strlen(e->id);
	if (idlen > 8 && strncasecmp(e->id, app, idlen - 8) == 0 && !app[idlen - 8]) {
		return true;
	}
	char *mine = program_of(e->exec), *theirs = program_of(exec);
	bool same = mine && theirs && *mine && strcmp(mine, theirs) == 0;
	free(mine);
	free(theirs);
	return same;
}

struct recent {
	char *uri;
	time_t when;
};

static int recent_cmp(const void *a, const void *b) {
	const struct recent *x = *(const struct recent **)a, *y = *(const struct recent **)b;
	return x->when < y->when ? 1 : x->when > y->when ? -1 : 0;
}

static void add_recent(const struct tw_desktop_entry *e, list_t *items) {
	char *path = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", NULL);
	GBookmarkFile *file = g_bookmark_file_new();
	if (!g_bookmark_file_load_from_file(file, path, NULL)) {
		g_bookmark_file_free(file);
		g_free(path);
		return;
	}
	g_free(path);
	list_t *found = create_list();
	gsize count = 0;
	gchar **uris = g_bookmark_file_get_uris(file, &count);
	for (gsize i = 0; i < count; i++) {
		gsize napps = 0;
		gchar **apps = g_bookmark_file_get_applications(file, uris[i], &napps, NULL);
		for (gsize a = 0; a < napps; a++) {
			gchar *exec = NULL;
			GDateTime *stamp = NULL;
			if (!g_bookmark_file_get_application_info(file, uris[i], apps[a], &exec, NULL,
					&stamp, NULL) || !same_app(e, apps[a], exec)) {
				g_free(exec);
				continue;
			}
			g_free(exec);
			// only files that are still there
			char *local = g_filename_from_uri(uris[i], NULL, NULL);
			bool there = !local || g_file_test(local, G_FILE_TEST_EXISTS);
			g_free(local);
			if (there) {
				struct recent *r = calloc(1, sizeof(*r));
				r->uri = strdup(uris[i]);
				r->when = stamp ? g_date_time_to_unix(stamp) : 0;
				list_add(found, r);
			}
			break;
		}
		g_strfreev(apps);
	}
	g_strfreev(uris);
	g_bookmark_file_free(file);

	list_qsort(found, recent_cmp);
	if (found->length > 0) {
		struct menu_item *header = menu_item_new("Recent", NULL);
		header->disabled = true;
		header->bold = true;
		list_add(items, header);
	}
	for (int i = 0; i < found->length; i++) {
		struct recent *r = found->items[i];
		if (i < RECENT_MAX) {
			char *local = g_filename_from_uri(r->uri, NULL, NULL);
			char *name = g_path_get_basename(local ? local : r->uri);
			char *cmd_line = exec_with(e->exec, local ? local : r->uri, r->uri);
			char *cmd = format_str("exec %s", cmd_line);
			struct menu_item *item = menu_item_new(name, cmd);
			// the icon of the file's type
			gchar *type = g_content_type_guess(local ? local : r->uri, NULL, 0, NULL);
			gchar *icon = type ? g_content_type_get_generic_icon_name(type) : NULL;
			free(item->icon);
			item->icon = strdup(icon ? icon : "text-x-generic");
			g_free(icon);
			g_free(type);
			list_add(items, item);
			free(cmd);
			free(cmd_line);
			g_free(name);
			g_free(local);
		}
		free(r->uri);
		free(r);
	}
	list_free(found);
}

/* The tasks of the entry: its [Desktop Action ...] groups. */
static void add_actions(const struct tw_desktop_entry *e, list_t *items) {
	GKeyFile *kf = g_key_file_new();
	if (!e->path || !g_key_file_load_from_file(kf, e->path, G_KEY_FILE_NONE, NULL)) {
		g_key_file_free(kf);
		return;
	}
	gsize n = 0;
	gchar **actions = g_key_file_get_string_list(kf, "Desktop Entry", "Actions", &n, NULL);
	bool header = false;
	for (gsize i = 0; i < n; i++) {
		char group[256];
		snprintf(group, sizeof(group), "Desktop Action %s", actions[i]);
		gchar *name = g_key_file_get_locale_string(kf, group, "Name", NULL, NULL);
		gchar *exec = g_key_file_get_string(kf, group, "Exec", NULL);
		if (name && exec && *name && *exec) {
			if (!header) {
				struct menu_item *title = menu_item_new("Tasks", NULL);
				title->disabled = true;
				title->bold = true;
				list_add(items, title);
				header = true;
			}
			char *command = strip_codes(exec);
			char *cmd = format_str("exec %s", command);
			struct menu_item *item = menu_item_new(name, cmd);
			gchar *icon = g_key_file_get_string(kf, group, "Icon", NULL);
			free(item->icon);
			item->icon = strdup(icon ? icon : e->icon ? e->icon : "system-run");
			g_free(icon);
			list_add(items, item);
			free(cmd);
			free(command);
		}
		g_free(name);
		g_free(exec);
	}
	g_strfreev(actions);
	g_key_file_free(kf);
}

void jumplist_add(const struct tw_desktop_entry *entry, list_t *items) {
	if (!entry || !entry->exec) {
		return;
	}
	int start = items->length;
	add_recent(entry, items);
	if (items->length > start) {
		list_add(items, menu_item_separator());
	}
	start = items->length;
	add_actions(entry, items);
	if (items->length > start) {
		list_add(items, menu_item_separator());
	}
}
