#ifndef _TILEWIN_PANEL_TRAY_ITEM_H
#define _TILEWIN_PANEL_TRAY_ITEM_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>
#include "tray/tray.h"
#include "list.h"

struct swaybar_pixmap {
	int size;
	unsigned char pixels[];
};

struct swaybar_sni_slot {
	struct wl_list link; // swaybar_sni::slots
	struct swaybar_sni *sni;
	const char *prop;
	const char *type;
	void *dest;
	sd_bus_slot *slot;
};

struct swaybar_sni {
	struct swaybar_tray *tray;
	cairo_surface_t *icon;
	int icon_size;

	char *watcher_id;
	char *service;
	char *path;
	char *interface;

	char *status;
	char *icon_name;
	list_t *icon_pixmap; // struct swaybar_pixmap *
	char *attention_icon_name;
	list_t *attention_icon_pixmap; // struct swaybar_pixmap *
	bool item_is_menu;
	char *menu;
	char *icon_theme_path; // non-standard KDE property
	char *title;

	struct wl_list slots; // swaybar_sni_slot::link
};

struct swaybar_sni *create_sni(char *id, struct swaybar_tray *tray);
void destroy_sni(struct swaybar_sni *sni);
bool sni_visible(struct swaybar_sni *sni);
/* Icon for the requested pixel size, owned by the item. */
cairo_surface_t *sni_icon(struct swaybar_sni *sni, int size, const char *icon_theme);
void sni_click(struct swaybar_sni *sni, int x, int y, uint32_t button);
void sni_scroll(struct swaybar_sni *sni, int direction);

#endif
