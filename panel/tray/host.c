#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "panel.h"
#include "tray/host.h"
#include "tray/item.h"
#include "tray/tray.h"
#include "list.h"
#include "log.h"
#include "stringop.h"

static const char *watcher_path = "/StatusNotifierWatcher";

/* The service and object path an item id points at. */
static void split_id(const char *id, char **service, char **path) {
	const char *slash = strchr(id, '/');
	if (slash) {
		*service = strndup(id, slash - id);
		*path = strdup(slash);
	} else {
		*service = strdup(id);
		*path = strdup("/StatusNotifierItem");
	}
}

/* The item of that service and path, -1 if the tray does not have it. */
static int find_sni(struct swaybar_tray *tray, const char *service, const char *path) {
	for (int i = 0; i < tray->items->length; i++) {
		struct swaybar_sni *sni = tray->items->items[i];
		if (strcmp(sni->service, service) == 0 && strcmp(sni->path, path) == 0) {
			return i;
		}
	}
	return -1;
}

static void add_sni(struct swaybar_tray *tray, char *id) {
	char *service, *path;
	split_id(id, &service, &path);
	int idx = find_sni(tray, service, path);
	free(service);
	free(path);
	if (idx != -1) {
		// The same item registered with both watchers, e.g. an item that found
		// a left over KDE watcher besides ours: one of the two ids names the
		// item's object path and speaks its interface, the other answers
		// neither properties nor clicks. Keep the one with the path.
		struct swaybar_sni *sni = tray->items->items[idx];
		if (!strchr(id, '/') || strchr(sni->watcher_id, '/')) {
			return;
		}
		sway_log(SWAY_INFO, "Replacing Status Notifier Item '%s' with '%s'",
			sni->watcher_id, id);
		destroy_sni(sni);
		list_del(tray->items, idx);
	}
	sway_log(SWAY_INFO, "Registering Status Notifier Item '%s'", id);
	struct swaybar_sni *sni = create_sni(id, tray);
	if (sni) {
		list_add(tray->items, sni);
		tray_set_dirty(tray);
	}
}

void tray_remove_service(struct swaybar_tray *tray, const char *service) {
	for (int i = tray->items->length - 1; i >= 0; i--) {
		struct swaybar_sni *sni = tray->items->items[i];
		if (strcmp(sni->service, service) == 0) {
			sway_log(SWAY_INFO, "Status Notifier Item '%s' is gone", sni->watcher_id);
			destroy_sni(sni);
			list_del(tray->items, i);
			tray_set_dirty(tray);
		}
	}
}

static int handle_sni_registered(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	char *id;
	int ret = sd_bus_message_read(msg, "s", &id);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to parse register SNI message: %s", strerror(-ret));
	}

	struct swaybar_tray *tray = data;
	add_sni(tray, id);

	return ret;
}

static int handle_sni_unregistered(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	char *id;
	int ret = sd_bus_message_read(msg, "s", &id);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to parse unregister SNI message: %s", strerror(-ret));
	}

	struct swaybar_tray *tray = data;
	char *service, *path;
	split_id(id, &service, &path);
	int idx = find_sni(tray, service, path);
	free(service);
	free(path);
	if (idx != -1) {
		sway_log(SWAY_INFO, "Unregistering Status Notifier Item '%s'", id);
		destroy_sni(tray->items->items[idx]);
		list_del(tray->items, idx);
		tray_set_dirty(tray);
	}
	return ret;
}

static int get_registered_snis_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	if (sd_bus_message_is_method_error(msg, NULL)) {
		const sd_bus_error *err = sd_bus_message_get_error(msg);
		sway_log(SWAY_ERROR, "Failed to get registered SNIs: %s", err->message);
		return -sd_bus_error_get_errno(err);
	}

	int ret = sd_bus_message_enter_container(msg, 'v', NULL);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to read registered SNIs: %s", strerror(-ret));
		return ret;
	}

	char **ids;
	ret = sd_bus_message_read_strv(msg, &ids);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to read registered SNIs: %s", strerror(-ret));
		return ret;
	}

	if (ids) {
		struct swaybar_tray *tray = data;
		for (char **id = ids; *id; ++id) {
			add_sni(tray, *id);
			free(*id);
		}
	}

	free(ids);
	return ret;
}

static bool register_to_watcher(struct swaybar_host *host) {
	// this is called asynchronously in case the watcher is owned by this process
	int ret = sd_bus_call_method_async(host->tray->bus, NULL,
			host->watcher_interface, watcher_path, host->watcher_interface,
			"RegisterStatusNotifierHost", NULL, NULL, "s", host->service);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to send register call: %s", strerror(-ret));
		return false;
	}

	ret = sd_bus_call_method_async(host->tray->bus, NULL,
			host->watcher_interface, watcher_path,
			"org.freedesktop.DBus.Properties", "Get",
			get_registered_snis_callback, host->tray, "ss",
			host->watcher_interface, "RegisteredStatusNotifierItems");
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to get registered SNIs: %s", strerror(-ret));
	}

	return ret >= 0;
}

static int handle_new_watcher(sd_bus_message *msg,
		void *data, sd_bus_error *error) {
	char *service, *old_owner, *new_owner;
	int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to parse owner change message: %s", strerror(-ret));
		return ret;
	}

	if (!*old_owner) {
		struct swaybar_host *host = data;
		if (strcmp(service, host->watcher_interface) == 0) {
			register_to_watcher(host);
		}
	}

	return 0;
}

bool init_host(struct swaybar_host *host, char *protocol,
		struct swaybar_tray *tray) {
	host->watcher_interface = format_str("org.%s.StatusNotifierWatcher", protocol);
	if (!host->watcher_interface) {
		return false;
	}

	sd_bus_slot *reg_slot = NULL, *unreg_slot = NULL, *watcher_slot = NULL;
	int ret = sd_bus_match_signal(tray->bus, &reg_slot, host->watcher_interface,
			watcher_path, host->watcher_interface,
			"StatusNotifierItemRegistered", handle_sni_registered, tray);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to subscribe to registering events: %s",
				strerror(-ret));
		goto error;
	}
	ret = sd_bus_match_signal(tray->bus, &unreg_slot, host->watcher_interface,
			watcher_path, host->watcher_interface,
			"StatusNotifierItemUnregistered", handle_sni_unregistered, tray);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to subscribe to unregistering events: %s",
				strerror(-ret));
		goto error;
	}

	ret = sd_bus_match_signal(tray->bus, &watcher_slot, "org.freedesktop.DBus",
			"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
			handle_new_watcher, host);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Failed to subscribe to unregistering events: %s",
				strerror(-ret));
		goto error;
	}

	pid_t pid = getpid();
	host->service = format_str("org.%s.StatusNotifierHost-%d", protocol, pid);
	if (!host->service) {
		goto error;
	}
	ret = sd_bus_request_name(tray->bus, host->service, 0);
	if (ret < 0) {
		sway_log(SWAY_DEBUG, "Failed to acquire service name: %s", strerror(-ret));
		goto error;
	}

	host->tray = tray;
	if (!register_to_watcher(host)) {
		goto error;
	}

	sd_bus_slot_set_floating(reg_slot, 0);
	sd_bus_slot_set_floating(unreg_slot, 0);
	sd_bus_slot_set_floating(watcher_slot, 0);

	sway_log(SWAY_DEBUG, "Registered %s", host->service);
	return true;
error:
	sd_bus_slot_unref(reg_slot);
	sd_bus_slot_unref(unreg_slot);
	sd_bus_slot_unref(watcher_slot);
	finish_host(host);
	return false;
}

void finish_host(struct swaybar_host *host) {
	sd_bus_release_name(host->tray->bus, host->service);
	free(host->service);
	free(host->watcher_interface);
}
