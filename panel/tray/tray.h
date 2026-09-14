#ifndef _TILEWIN_PANEL_TRAY_TRAY_H
#define _TILEWIN_PANEL_TRAY_TRAY_H
#include "config.h"
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif
#include <cairo.h>
#include <stdint.h>
#include "list.h"
#include "tray/host.h"

struct panel;
struct swaybar_watcher;

struct swaybar_tray {
	struct panel *panel;
	int fd;
	sd_bus *bus;

	struct swaybar_host host_xdg;
	struct swaybar_host host_kde;
	list_t *items; // struct swaybar_sni *
	struct swaybar_watcher *watcher_xdg;
	struct swaybar_watcher *watcher_kde;

	list_t *basedirs; // char *
	list_t *themes;   // struct icon_theme *
};

struct swaybar_tray *create_tray(struct panel *panel);
void destroy_tray(struct swaybar_tray *tray);
void tray_process(struct swaybar_tray *tray);
void tray_set_dirty(struct swaybar_tray *tray);

#endif
