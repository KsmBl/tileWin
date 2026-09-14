#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "tw_desktop.h"

/* ---------- desktop entry cache ---------- */

static list_t *entries = NULL;
static time_t scanned_at = 0;
static time_t newest_dir_mtime = 0;

static time_t dirs_mtime(void) {
	const char *home = getenv("HOME");
	char *paths[4] = { 0 };
	paths[0] = strdup("/usr/share/applications");
	paths[1] = strdup("/usr/local/share/applications");
	paths[2] = home ? format_str("%s/.local/share/applications", home) : NULL;
	paths[3] = strdup("/var/lib/flatpak/exports/share/applications");
	time_t newest = 0;
	for (int i = 0; i < 4; i++) {
		struct stat st;
		if (paths[i] && stat(paths[i], &st) == 0 && st.st_mtime > newest) {
			newest = st.st_mtime;
		}
		free(paths[i]);
	}
	return newest;
}

list_t *apps_get(void) {
	time_t now = time(NULL);
	if (entries && now - scanned_at < 10) {
		return entries;
	}
	time_t mtime = dirs_mtime();
	if (entries && mtime == newest_dir_mtime) {
		scanned_at = now;
		return entries;
	}
	tw_desktop_list_free(entries);
	entries = tw_desktop_scan();
	scanned_at = now;
	newest_dir_mtime = mtime;
	return entries;
}

struct tw_desktop_entry *apps_find(const char *id) {
	if (!id || !*id) {
		return NULL;
	}
	list_t *all = apps_get();
	size_t len = strlen(id);
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (strcmp(e->id, id) == 0) {
			return e;
		}
	}
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		size_t idlen = strlen(e->id);
		if (idlen == len + 8 && strncasecmp(e->id, id, len) == 0) {
			return e;
		}
		if (e->startup_wm_class && strcasecmp(e->startup_wm_class, id) == 0) {
			return e;
		}
	}
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		size_t idlen = strlen(e->id) - 8;
		if (idlen > len && e->id[idlen - len - 1] == '.' &&
				strncasecmp(e->id + idlen - len, id, len) == 0) {
			return e;
		}
	}
	return NULL;
}

const char *apps_display_name(const char *app_id) {
	struct tw_desktop_entry *e = apps_find(app_id);
	return e ? e->name : app_id;
}

void apps_launch(struct panel *panel, const struct tw_desktop_entry *entry) {
	char *cmd = tw_desktop_exec_command(entry);
	if (!cmd) {
		return;
	}
	if (entry->terminal && panel->config && panel->config->terminal) {
		char *wrapped = format_str("%s %s", panel->config->terminal, cmd);
		free(cmd);
		cmd = wrapped;
	}
	ipc_panel_commandf(panel, "exec %s", cmd);
	free(cmd);
}

/* ---------- icon cache ---------- */

struct icon_entry {
	char *key;
	int size;
	cairo_surface_t *surface;
};

static list_t *icons = NULL;

static cairo_surface_t *cache_lookup(const char *key, int size, bool *found) {
	*found = false;
	for (int i = 0; icons && i < icons->length; i++) {
		struct icon_entry *e = icons->items[i];
		if (e->size == size && strcmp(e->key, key) == 0) {
			*found = true;
			return e->surface;
		}
	}
	return NULL;
}

static void cache_store(const char *key, int size, cairo_surface_t *surface) {
	if (!icons) {
		icons = create_list();
	}
	if (icons->length > 300) {
		// drop the oldest half; surfaces may still be referenced during this
		// frame only, never stored elsewhere
		for (int i = 0; i < 150; i++) {
			struct icon_entry *e = icons->items[i];
			if (e->surface) {
				cairo_surface_destroy(e->surface);
			}
			free(e->key);
			free(e);
		}
		memmove(icons->items, icons->items + 150, (icons->length - 150) * sizeof(void *));
		icons->length -= 150;
	}
	struct icon_entry *e = calloc(1, sizeof(*e));
	e->key = strdup(key);
	e->size = size;
	e->surface = surface;
	list_add(icons, e);
}

static const char *icon_theme(struct panel *panel) {
	return tw_theme_str(panel->theme, "icons.theme", "hicolor");
}

cairo_surface_t *apps_icon(struct panel *panel, const char *name, int size) {
	if (!name || !*name) {
		return NULL;
	}
	char key[512];
	snprintf(key, sizeof(key), "n:%s", name);
	bool found;
	cairo_surface_t *s = cache_lookup(key, size, &found);
	if (found) {
		return s;
	}
	s = tw_icon_load(name, size, icon_theme(panel));
	cache_store(key, size, s);
	return s;
}

cairo_surface_t *apps_icon_for_window(struct panel *panel, struct pwindow *win, int size) {
	if (!win->app_id || !*win->app_id) {
		return NULL;
	}
	char key[512];
	snprintf(key, sizeof(key), "a:%s", win->app_id);
	bool found;
	cairo_surface_t *s = cache_lookup(key, size, &found);
	if (found) {
		return s;
	}
	struct tw_desktop_entry *entry = apps_find(win->app_id);
	if (entry && entry->icon) {
		s = tw_icon_load(entry->icon, size, icon_theme(panel));
	}
	if (!s) {
		s = tw_icon_load_for_app(win->app_id, size, icon_theme(panel));
	}
	cache_store(key, size, s);
	return s;
}
