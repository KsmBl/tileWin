#ifndef _TILEWIN_PANEL_TRAY_MENU_H
#define _TILEWIN_PANEL_TRAY_MENU_H
#include <stdbool.h>
#include "panel.h"
#include "tray/item.h"

/* True if the item publishes a menu on the bus (com.canonical.dbusmenu). */
bool sni_has_menu(struct swaybar_sni *sni);
/* Reads that menu and shows it at the anchor. */
void sni_menu_open(struct swaybar_sni *sni, struct popup_anchor anchor);
/* Tells the program that one of its menu entries was clicked. */
void sni_menu_event(struct swaybar_tray *tray, const char *service, const char *path, int id);

#endif
