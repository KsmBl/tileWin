#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "sway/tilewin.h"
#include "sway/tree/view.h"
#include "tw_desktop.h"

struct icon_cache_entry {
	char *app_id;
	int size;
	int theme_generation;
	cairo_surface_t *surface; // NULL when no icon was found
};

static list_t *cache = NULL;

void tw_icon_cache_clear(void) {
	if (!cache) {
		return;
	}
	for (int i = 0; i < cache->length; i++) {
		struct icon_cache_entry *entry = cache->items[i];
		if (entry->surface) {
			cairo_surface_destroy(entry->surface);
		}
		free(entry->app_id);
		free(entry);
	}
	list_free(cache);
	cache = NULL;
}

cairo_surface_t *tw_icon_for_view(struct sway_view *view, int size) {
	const char *app_id = view_get_app_id(view);
	if (!app_id) {
		app_id = view_get_class(view);
	}
	if (!app_id) {
		return NULL;
	}
	if (!cache) {
		cache = create_list();
	}
	for (int i = 0; i < cache->length; i++) {
		struct icon_cache_entry *entry = cache->items[i];
		if (entry->size == size && strcmp(entry->app_id, app_id) == 0) {
			return entry->surface;
		}
	}
	if (cache->length > 256) {
		// Cached surfaces may still be referenced by decoration caches, which
		// only compare pointers, so clearing is safe.
		tw_icon_cache_clear();
		cache = create_list();
	}
	struct icon_cache_entry *entry = calloc(1, sizeof(*entry));
	entry->app_id = strdup(app_id);
	entry->size = size;
	entry->surface = tw_icon_load_for_app(app_id, size,
		tw_theme_str(tw_theme, "icons.theme", "hicolor"));
	list_add(cache, entry);
	return entry->surface;
}
