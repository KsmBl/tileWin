#ifndef _TILEWIN_PANEL_TRAY_HOST_H
#define _TILEWIN_PANEL_TRAY_HOST_H

#include <stdbool.h>

struct swaybar_tray;

struct swaybar_host {
	struct swaybar_tray *tray;
	char *service;
	char *watcher_interface;
};

bool init_host(struct swaybar_host *host, char *protocol, struct swaybar_tray *tray);
/* Removes the items of a service, e.g. when the program owning them is gone. */
void tray_remove_service(struct swaybar_tray *tray, const char *service);
void finish_host(struct swaybar_host *host);

#endif
