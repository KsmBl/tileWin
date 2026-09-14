#ifndef _TW_DESKTOP_H
#define _TW_DESKTOP_H
#include <stdbool.h>
#include <cairo.h>
#include "list.h"

struct tw_desktop_entry {
	char *id;   // e.g. "org.xfce.mousepad.desktop"
	char *path;
	char *name;
	char *generic_name;
	char *comment;
	char *exec;
	char *icon;
	char *categories;
	char *keywords;
	char *startup_wm_class;
	bool terminal;
	bool no_display;
	bool hidden;
};

struct tw_desktop_entry *tw_desktop_load(const char *path, const char *id);
void tw_desktop_entry_free(struct tw_desktop_entry *entry);

/* All application entries, deduplicated by id and sorted by name. */
list_t *tw_desktop_scan(void);
void tw_desktop_list_free(list_t *entries);

/* Best matching entry for a Wayland app_id / X11 class, or NULL. */
struct tw_desktop_entry *tw_desktop_find_for_app_id(const char *app_id);

/* Exec line with field codes removed. Newly allocated. */
char *tw_desktop_exec_command(const struct tw_desktop_entry *entry);

/* Loads a PNG/SVG (or anything gdk-pixbuf knows) into a size x size surface. */
cairo_surface_t *tw_icon_load_file(const char *path, int size);
/* Loads an icon by theme name or absolute path. */
cairo_surface_t *tw_icon_load(const char *name, int size, const char *theme);
/* Resolves the icon for an application id via its desktop entry. */
cairo_surface_t *tw_icon_load_for_app(const char *app_id, int size, const char *theme);
/* Renders an image (SVG or raster) scaled to cover width x height, cropping the overflow. */
cairo_surface_t *tw_image_render_cover(const char *path, int width, int height);
/* Loads an image at its natural size. */
cairo_surface_t *tw_image_load(const char *path);

#endif
