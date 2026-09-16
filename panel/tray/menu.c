/*
 * Menus of tray icons (com.canonical.dbusmenu). Most tray icons do nothing on
 * their own when they are clicked: network applets, chat programs and KDE apps
 * publish their menu on the bus and expect the panel to show it. The layout is
 * fetched when the icon is clicked and shown as a normal panel menu; clicking
 * an entry sends its id back to the program ("panel tray_event ...").
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "panel.h"
#include "popup.h"
#include "tray/item.h"
#include "tray/menu.h"
#include "tray/tray.h"
#include "list.h"
#include "log.h"
#include "stringop.h"

#define MENU_INTERFACE "com.canonical.dbusmenu"
#define MAX_DEPTH 6

struct menu_request {
	struct swaybar_tray *tray;
	char *service, *path;
	struct popup_anchor anchor;
};

static void request_free(struct menu_request *req) {
	if (req) {
		free(req->service);
		free(req->path);
		free(req);
	}
}

bool sni_has_menu(struct swaybar_sni *sni) {
	return sni && sni->menu && sni->menu[0] == '/' && strcmp(sni->menu, "/NO_DBUSMENU") != 0;
}

/* "_Open files" is shown as "Open files", "__" as one underscore. */
static char *strip_mnemonics(const char *label) {
	char *out = malloc(strlen(label) + 1);
	if (!out) {
		return NULL;
	}
	char *o = out;
	for (const char *p = label; *p; p++) {
		if (*p == '_' && p[1] == '_') {
			*o++ = '_';
			p++;
		} else if (*p != '_') {
			*o++ = *p;
		}
	}
	*o = '\0';
	return out;
}

static struct menu_item *read_item(sd_bus_message *msg, struct menu_request *req, int depth);

static list_t *read_children(sd_bus_message *msg, struct menu_request *req, int depth) {
	list_t *children = create_list();
	if (sd_bus_message_enter_container(msg, 'a', "v") < 0) {
		return children;
	}
	while (sd_bus_message_at_end(msg, 0) == 0) {
		if (sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)") < 0) {
			break;
		}
		struct menu_item *item = read_item(msg, req, depth + 1);
		sd_bus_message_exit_container(msg);
		if (item) {
			list_add(children, item);
		}
	}
	sd_bus_message_exit_container(msg);
	return children;
}

/* One entry of the layout: (id, properties, children). */
static struct menu_item *read_item(sd_bus_message *msg, struct menu_request *req, int depth) {
	if (sd_bus_message_enter_container(msg, 'r', "ia{sv}av") < 0) {
		return NULL;
	}
	int id = 0;
	sd_bus_message_read(msg, "i", &id);
	char *label = NULL, *icon = NULL;
	bool enabled = true, visible = true, separator = false, submenu = false, checked = false;
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") >= 0) {
		while (sd_bus_message_at_end(msg, 0) == 0) {
			if (sd_bus_message_enter_container(msg, 'e', "sv") < 0) {
				break;
			}
			const char *key = NULL, *contents = NULL;
			char type = 0;
			sd_bus_message_read(msg, "s", &key);
			sd_bus_message_peek_type(msg, &type, &contents);
			if (!key || !contents) {
				sd_bus_message_skip(msg, "v");
			} else if (strcmp(contents, "s") == 0) {
				const char *value = NULL;
				sd_bus_message_read(msg, "v", "s", &value);
				if (!value) {
					// nothing to read
				} else if (strcmp(key, "label") == 0) {
					free(label);
					label = strip_mnemonics(value);
				} else if (strcmp(key, "type") == 0) {
					separator = strcmp(value, "separator") == 0;
				} else if (strcmp(key, "children-display") == 0) {
					submenu = strcmp(value, "submenu") == 0;
				} else if (strcmp(key, "icon-name") == 0 && *value) {
					free(icon);
					icon = strdup(value);
				}
			} else if (strcmp(contents, "b") == 0) {
				int value = 0;
				sd_bus_message_read(msg, "v", "b", &value);
				if (strcmp(key, "enabled") == 0) {
					enabled = value;
				} else if (strcmp(key, "visible") == 0) {
					visible = value;
				}
			} else if (strcmp(contents, "i") == 0) {
				int value = 0;
				sd_bus_message_read(msg, "v", "i", &value);
				if (strcmp(key, "toggle-state") == 0) {
					checked = value > 0;
				}
			} else {
				sd_bus_message_skip(msg, "v");
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
	}
	list_t *children = depth < MAX_DEPTH ? read_children(msg, req, depth) : create_list();
	sd_bus_message_exit_container(msg);

	struct menu_item *item = NULL;
	if (!visible) {
		// nothing
	} else if (separator) {
		item = menu_item_separator();
	} else {
		// entries with a submenu open it instead of sending a click
		char *command = submenu && children->length ? NULL :
			format_str("panel tray_event %s %s %d", req->service, req->path, id);
		item = menu_item_new(label && *label ? label : "", command);
		free(command);
		if (item) {
			item->icon = icon;
			icon = NULL;
			item->disabled = !enabled;
			item->checked = checked;
			if (children->length) {
				item->children = children;
				children = NULL;
			}
		}
	}
	free(label);
	free(icon);
	if (children) {
		menu_items_free(children);
	}
	return item;
}

static int handle_layout(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct menu_request *req = data;
	if (sd_bus_message_is_method_error(msg, NULL)) {
		const sd_bus_error *err = sd_bus_message_get_error(msg);
		sway_log(SWAY_INFO, "Tray menu of %s: %s", req->service, err->message);
		request_free(req);
		return 0;
	}
	uint32_t revision = 0;
	sd_bus_message_read(msg, "u", &revision);
	struct menu_item *root = read_item(msg, req, 0);
	list_t *items = create_list();
	if (root) {
		if (root->children) {
			list_cat(items, root->children);
			list_free(root->children);
			root->children = NULL;
		}
		list_t *one = create_list();
		list_add(one, root);
		menu_items_free(one);
	}

	struct panel *panel = req->tray->panel;
	bool output_alive = false;
	struct panel_output *output;
	wl_list_for_each(output, &panel->outputs, link) {
		output_alive = output_alive || output == req->anchor.output;
	}
	if (items->length && output_alive) {
		menu_open(panel, items, true, req->anchor, NULL);
	} else {
		menu_items_free(items);
	}
	request_free(req);
	return 0;
}

static int handle_about_to_show(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct menu_request *req = data;
	// the program filled its menu in (or said it has nothing to do): read it
	int ret = sd_bus_call_method_async(req->tray->bus, NULL, req->service, req->path,
		MENU_INTERFACE, "GetLayout", handle_layout, req, "iias", 0, -1, 0);
	if (ret < 0) {
		sway_log(SWAY_INFO, "Tray menu of %s: %s", req->service, strerror(-ret));
		request_free(req);
	}
	return 0;
}

void sni_menu_open(struct swaybar_sni *sni, struct popup_anchor anchor) {
	if (!sni_has_menu(sni)) {
		return;
	}
	struct menu_request *req = calloc(1, sizeof(*req));
	if (!req) {
		return;
	}
	req->tray = sni->tray;
	req->service = strdup(sni->service);
	req->path = strdup(sni->menu);
	req->anchor = anchor;
	int ret = sd_bus_call_method_async(sni->tray->bus, NULL, req->service, req->path,
		MENU_INTERFACE, "AboutToShow", handle_about_to_show, req, "i", 0);
	if (ret < 0) {
		handle_about_to_show(NULL, req, NULL); // ask for the menu anyway
	}
}

void sni_menu_event(struct swaybar_tray *tray, const char *service, const char *path, int id) {
	sd_bus_message *msg = NULL;
	if (sd_bus_message_new_method_call(tray->bus, &msg, service, path,
			MENU_INTERFACE, "Event") < 0) {
		return;
	}
	if (sd_bus_message_append(msg, "is", id, "clicked") >= 0 &&
			sd_bus_message_open_container(msg, 'v', "s") >= 0 &&
			sd_bus_message_append(msg, "s", "") >= 0 &&
			sd_bus_message_close_container(msg) >= 0 &&
			sd_bus_message_append(msg, "u", (uint32_t)time(NULL)) >= 0) {
		sd_bus_call_async(tray->bus, NULL, msg, NULL, NULL, 0);
	}
	sd_bus_message_unref(msg);
}
