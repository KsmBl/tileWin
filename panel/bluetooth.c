/*
 * Bluetooth through BlueZ (org.bluez on the system bus): the power of the
 * adapter, paired and nearby devices, connecting and pairing.
 *
 * Calls never start bluetoothd: without it, or without an adapter, Bluetooth
 * is just unavailable. While the user pairs a device an agent answers BlueZ:
 * codes to type on a keyboard are shown, confirmations are accepted. Only
 * pairing the user started is accepted.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "log.h"
#include "panel.h"

#if HAVE_TRAY
#include <poll.h>
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif

#define BLUEZ "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define AGENT_PATH "/org/tilewin/bluetooth_agent"
#define LONG_TIMEOUT (60 * 1000 * 1000ULL)

enum bt_op {
	OP_POWER,
	OP_DISCOVERY,
	OP_CONNECT,
	OP_DISCONNECT,
	OP_REGISTER,
	OP_UNREGISTER,
	OP_PAIR,
	OP_TRUST,
	OP_REMOVE,
	OP_OTHER,
};

struct pending {
	enum bt_op op;
	char *path;
};

static struct {
	struct panel *panel;
	sd_bus *bus;
	int fd;
	char *adapter;
	bool powered, discovering;
	list_t *devices; // struct bt_device *
	char status[200];
	char *pairing; // device being paired
} bt = { .fd = -1 };

static void changed(void) {
	if (bt.panel) {
		quicksettings_redraw(bt.panel);
		flyout_bluetooth_changed(bt.panel);
		panel_set_dirty(bt.panel);
	}
}

static void device_free(struct bt_device *d) {
	free(d->path);
	free(d->name);
	free(d->address);
	free(d->icon);
	free(d);
}

static struct bt_device *device_find(const char *path, bool create) {
	for (int i = 0; bt.devices && i < bt.devices->length; i++) {
		struct bt_device *d = bt.devices->items[i];
		if (strcmp(d->path, path) == 0) {
			return d;
		}
	}
	if (!create) {
		return NULL;
	}
	struct bt_device *d = calloc(1, sizeof(*d));
	d->path = strdup(path);
	list_add(bt.devices, d);
	return d;
}

static void device_remove(const char *path) {
	for (int i = 0; i < bt.devices->length; i++) {
		struct bt_device *d = bt.devices->items[i];
		if (strcmp(d->path, path) == 0) {
			list_del(bt.devices, i);
			device_free(d);
			return;
		}
	}
}

/* Devices of other adapters (or of none yet) are not shown. */
static void drop_foreign_devices(void) {
	size_t len = bt.adapter ? strlen(bt.adapter) : 0;
	for (int i = bt.devices->length - 1; i >= 0; i--) {
		struct bt_device *d = bt.devices->items[i];
		if (!bt.adapter || strncmp(d->path, bt.adapter, len) != 0 || d->path[len] != '/') {
			list_del(bt.devices, i);
			device_free(d);
		}
	}
}

static void forget_everything(void) {
	free(bt.adapter);
	bt.adapter = NULL;
	bt.powered = bt.discovering = false;
	while (bt.devices->length) {
		device_free(bt.devices->items[0]);
		list_del(bt.devices, 0);
	}
}

static int read_bool(sd_bus_message *m, bool *out) {
	int value;
	int r = sd_bus_message_read(m, "v", "b", &value);
	if (r >= 0) {
		*out = value;
	}
	return r;
}

static int read_string(sd_bus_message *m, char **out) {
	const char *value;
	int r = sd_bus_message_read(m, "v", "s", &value);
	if (r >= 0) {
		free(*out);
		*out = strdup(value);
	}
	return r;
}

/* Reads an a{sv} of properties of one interface of an object. */
static int parse_properties(sd_bus_message *m, const char *path, const char *iface) {
	bool adapter = strcmp(iface, ADAPTER_IFACE) == 0;
	bool device = strcmp(iface, DEVICE_IFACE) == 0;
	if (!adapter && !device) {
		return sd_bus_message_skip(m, "a{sv}");
	}
	if (adapter && !bt.adapter) {
		bt.adapter = strdup(path);
	}
	bool ours = adapter && strcmp(bt.adapter, path) == 0;
	struct bt_device *d = device ? device_find(path, true) : NULL;
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *contents;
		char type;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0 ||
				(r = sd_bus_message_peek_type(m, &type, &contents)) < 0) {
			return r;
		}
		bool is_bool = strcmp(contents, "b") == 0, is_str = strcmp(contents, "s") == 0;
		if (ours && is_bool && strcmp(key, "Powered") == 0) {
			r = read_bool(m, &bt.powered);
		} else if (ours && is_bool && strcmp(key, "Discovering") == 0) {
			r = read_bool(m, &bt.discovering);
		} else if (d && is_str && strcmp(key, "Alias") == 0) {
			r = read_string(m, &d->name);
		} else if (d && is_str && strcmp(key, "Address") == 0) {
			r = read_string(m, &d->address);
		} else if (d && is_str && strcmp(key, "Icon") == 0) {
			r = read_string(m, &d->icon);
		} else if (d && is_bool && strcmp(key, "Paired") == 0) {
			r = read_bool(m, &d->paired);
		} else if (d && is_bool && strcmp(key, "Connected") == 0) {
			r = read_bool(m, &d->connected);
		} else if (d && is_bool && strcmp(key, "Trusted") == 0) {
			r = read_bool(m, &d->trusted);
		} else if (d && strcmp(contents, "n") == 0 && strcmp(key, "RSSI") == 0) {
			int16_t rssi;
			r = sd_bus_message_read(m, "v", "n", &rssi);
			d->rssi = rssi;
			d->has_rssi = r >= 0;
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) {
			return r;
		}
	}
	if (r < 0) {
		return r;
	}
	return sd_bus_message_exit_container(m);
}

/* Reads an a{sa{sv}} of interfaces of an object. */
static int parse_interfaces(sd_bus_message *m, const char *path) {
	int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface;
		if ((r = sd_bus_message_read(m, "s", &iface)) < 0 ||
				(r = parse_properties(m, path, iface)) < 0 ||
				(r = sd_bus_message_exit_container(m)) < 0) {
			return r;
		}
	}
	if (r < 0) {
		return r;
	}
	return sd_bus_message_exit_container(m);
}

static const char *error_text(const sd_bus_error *error) {
	if (!error) {
		return "";
	}
	if (error->message && *error->message) {
		return error->message;
	}
	const char *dot = error->name ? strrchr(error->name, '.') : NULL;
	return dot ? dot + 1 : error->name ? error->name : "unknown error";
}

static const char *device_label(const char *path) {
	struct bt_device *d = path ? device_find(path, false) : NULL;
	return d && d->name ? d->name : "the device";
}

static int call(const char *path, const char *iface, const char *member, uint64_t timeout,
		sd_bus_message_handler_t callback, void *data, const char *types, ...) {
	if (!bt.bus) {
		return -ENOTCONN;
	}
	sd_bus_message *m = NULL;
	int r = sd_bus_message_new_method_call(bt.bus, &m, BLUEZ, path, iface, member);
	if (r >= 0) {
		// never start bluetoothd just to ask it something
		r = sd_bus_message_set_auto_start(m, false);
	}
	if (r >= 0 && types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(m, types, ap);
		va_end(ap);
	}
	if (r >= 0) {
		r = sd_bus_call_async(bt.bus, NULL, m, callback, data, timeout);
	}
	sd_bus_message_unref(m);
	if (r >= 0) {
		sd_bus_flush(bt.bus);
	}
	return r;
}

static struct pending *pending_new(enum bt_op op, const char *path) {
	struct pending *p = calloc(1, sizeof(*p));
	p->op = op;
	p->path = path ? strdup(path) : NULL;
	return p;
}

static int action_done(sd_bus_message *m, void *data, sd_bus_error *ret_error);

static void start(enum bt_op op, const char *path, const char *iface, const char *member,
		uint64_t timeout, const char *types, ...) {
	struct pending *p = pending_new(op, path);
	if (!bt.bus) {
		free(p->path);
		free(p);
		return;
	}
	sd_bus_message *m = NULL;
	const char *object = op == OP_REMOVE || op == OP_DISCOVERY || op == OP_POWER ?
		bt.adapter : op == OP_REGISTER || op == OP_UNREGISTER ? "/org/bluez" : path;
	int r = object ? sd_bus_message_new_method_call(bt.bus, &m, BLUEZ, object, iface, member) :
		-ENODEV;
	if (r >= 0) {
		r = sd_bus_message_set_auto_start(m, false);
	}
	if (r >= 0 && types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(m, types, ap);
		va_end(ap);
	}
	if (r >= 0) {
		r = sd_bus_call_async(bt.bus, NULL, m, action_done, p, timeout);
	}
	sd_bus_message_unref(m);
	if (r < 0) {
		snprintf(bt.status, sizeof(bt.status), "Bluetooth is not available");
		free(p->path);
		free(p);
		changed();
		return;
	}
	sd_bus_flush(bt.bus);
}

static void stop_pairing(void) {
	if (!bt.pairing) {
		return;
	}
	free(bt.pairing);
	bt.pairing = NULL;
	start(OP_UNREGISTER, NULL, "org.bluez.AgentManager1", "UnregisterAgent", 0, "o", AGENT_PATH);
}

static int action_done(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	struct pending *p = data;
	const sd_bus_error *error = sd_bus_message_get_error(m);
	struct bt_device *d = p->path ? device_find(p->path, false) : NULL;
	const char *name = device_label(p->path);
	switch (p->op) {
	case OP_CONNECT:
	case OP_DISCONNECT:
		if (d) {
			d->busy = false;
		}
		if (error) {
			snprintf(bt.status, sizeof(bt.status), "Couldn't %s %s: %s",
				p->op == OP_CONNECT ? "connect to" : "disconnect", name, error_text(error));
		} else {
			bt.status[0] = '\0';
		}
		break;
	case OP_REGISTER:
		if (error && !sd_bus_error_has_name(error, "org.bluez.Error.AlreadyExists")) {
			snprintf(bt.status, sizeof(bt.status), "Couldn't pair: %s", error_text(error));
			if (d) {
				d->busy = false;
			}
			free(bt.pairing);
			bt.pairing = NULL;
		} else if (p->path) {
			start(OP_PAIR, p->path, DEVICE_IFACE, "Pair", LONG_TIMEOUT, NULL);
		}
		break;
	case OP_PAIR:
		stop_pairing();
		if (error && !sd_bus_error_has_name(error, "org.bluez.Error.AlreadyExists")) {
			if (d) {
				d->busy = false;
			}
			snprintf(bt.status, sizeof(bt.status), "Couldn't pair with %s: %s", name,
				error_text(error));
		} else {
			snprintf(bt.status, sizeof(bt.status), "Paired with %s", name);
			start(OP_TRUST, p->path, "org.freedesktop.DBus.Properties", "Set", 0, "ssv",
				DEVICE_IFACE, "Trusted", "b", 1);
			start(OP_CONNECT, p->path, DEVICE_IFACE, "Connect", LONG_TIMEOUT, NULL);
		}
		break;
	case OP_POWER:
		if (error) {
			snprintf(bt.status, sizeof(bt.status), "Couldn't turn Bluetooth %s: %s",
				bt.powered ? "off" : "on", error_text(error));
		}
		break;
	case OP_REMOVE:
		if (error) {
			snprintf(bt.status, sizeof(bt.status), "Couldn't remove %s: %s", name,
				error_text(error));
		}
		break;
	case OP_DISCOVERY:
	case OP_TRUST:
	case OP_UNREGISTER:
	case OP_OTHER:
		break;
	}
	free(p->path);
	free(p);
	changed();
	return 0;
}

static int managed_objects_done(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	if (sd_bus_message_is_method_error(m, NULL)) {
		return 0; // bluetoothd isn't running
	}
	forget_everything();
	int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
	while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path;
		if ((r = sd_bus_message_read(m, "o", &path)) < 0 ||
				(r = parse_interfaces(m, path)) < 0) {
			break;
		}
		r = sd_bus_message_exit_container(m);
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Bluetooth: bad reply from BlueZ: %s", strerror(-r));
	}
	drop_foreign_devices();
	changed();
	return 0;
}

static void refresh(void) {
	call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", 0,
		managed_objects_done, NULL, NULL);
}

static int on_interfaces_added(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *path;
	if (sd_bus_message_read(m, "o", &path) >= 0 && parse_interfaces(m, path) >= 0) {
		drop_foreign_devices();
		changed();
	}
	return 0;
}

static int on_interfaces_removed(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *path;
	char **ifaces = NULL;
	if (sd_bus_message_read(m, "o", &path) < 0 || sd_bus_message_read_strv(m, &ifaces) < 0) {
		return 0;
	}
	bool adapter_gone = false;
	for (int i = 0; ifaces && ifaces[i]; i++) {
		if (strcmp(ifaces[i], DEVICE_IFACE) == 0) {
			device_remove(path);
		} else if (strcmp(ifaces[i], ADAPTER_IFACE) == 0 && bt.adapter &&
				strcmp(bt.adapter, path) == 0) {
			adapter_gone = true;
		}
		free(ifaces[i]);
	}
	free(ifaces);
	if (adapter_gone) {
		forget_everything();
		refresh(); // maybe there is another adapter
	}
	changed();
	return 0;
}

static int on_properties_changed(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *path = sd_bus_message_get_path(m);
	const char *iface;
	if (!path || sd_bus_message_read(m, "s", &iface) < 0) {
		return 0;
	}
	bool device = strcmp(iface, DEVICE_IFACE) == 0;
	if (device && !device_find(path, false)) {
		refresh(); // a device we haven't seen: read it completely
		return 0;
	}
	if (device || strcmp(iface, ADAPTER_IFACE) == 0) {
		parse_properties(m, path, iface);
		drop_foreign_devices();
		changed();
	}
	return 0;
}

static int on_name_owner_changed(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *name, *old_owner, *new_owner;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 ||
			strcmp(name, BLUEZ) != 0) {
		return 0;
	}
	if (*new_owner) {
		refresh();
	} else {
		forget_everything();
		free(bt.pairing);
		bt.pairing = NULL;
		changed();
	}
	return 0;
}

/* ---------- pairing agent ---------- */

static int agent_rejected(sd_bus_message *m) {
	return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected",
		"tileWin only pairs devices the user chose");
}

static int agent_ok(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	return bt.pairing ? sd_bus_reply_method_return(m, "") : agent_rejected(m);
}

static int agent_release(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	return sd_bus_reply_method_return(m, "");
}

static int agent_request_pin(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	return bt.pairing ? sd_bus_reply_method_return(m, "s", "0000") : agent_rejected(m);
}

static int agent_request_passkey(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	return bt.pairing ? sd_bus_reply_method_return(m, "u", (uint32_t)0) : agent_rejected(m);
}

static int agent_display_pin(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *device, *pin;
	if (bt.pairing && sd_bus_message_read(m, "os", &device, &pin) >= 0) {
		snprintf(bt.status, sizeof(bt.status), "Type %s on %s, then press Enter", pin,
			device_label(device));
		changed();
	}
	return agent_ok(m, data, ret_error);
}

static int agent_display_passkey(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *device;
	uint32_t passkey;
	uint16_t entered;
	if (bt.pairing && sd_bus_message_read(m, "ouq", &device, &passkey, &entered) >= 0) {
		snprintf(bt.status, sizeof(bt.status), "Type %06u on %s, then press Enter", passkey,
			device_label(device));
		changed();
	}
	return agent_ok(m, data, ret_error);
}

static int agent_confirm(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *device;
	uint32_t passkey;
	if (bt.pairing && sd_bus_message_read(m, "ou", &device, &passkey) >= 0) {
		snprintf(bt.status, sizeof(bt.status), "Pairing code %06u", passkey);
		changed();
	}
	return agent_ok(m, data, ret_error);
}

static const sd_bus_vtable agent_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Release", "", "", agent_release, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestPinCode", "o", "s", agent_request_pin, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DisplayPinCode", "os", "", agent_display_pin, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestPasskey", "o", "u", agent_request_passkey,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_display_passkey,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestAuthorization", "o", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("AuthorizeService", "os", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Cancel", "", "", agent_release, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

/* ---------- public ---------- */

static void bus_in(int fd, short mask, void *data) {
	int r;
	while ((r = sd_bus_process(bt.bus, NULL)) > 0) {
		// keep processing
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Bluetooth: system bus error: %s", strerror(-r));
		loop_remove_fd(bt.panel->loop, bt.fd);
		sd_bus_flush_close_unref(bt.bus);
		bt.bus = NULL;
		bt.fd = -1;
		forget_everything();
		changed();
	}
}

void bt_init(struct panel *panel) {
	bt.panel = panel;
	bt.devices = create_list();
	sd_bus *bus = NULL;
	int r = sd_bus_open_system(&bus);
	if (r < 0) {
		sway_log(SWAY_INFO, "Bluetooth: no system bus: %s", strerror(-r));
		return;
	}
	bt.bus = bus;
	sd_bus_add_object_vtable(bus, NULL, AGENT_PATH, "org.bluez.Agent1", agent_vtable, NULL);
	sd_bus_match_signal(bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.ObjectManager",
		"InterfacesAdded", on_interfaces_added, NULL);
	sd_bus_match_signal(bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.ObjectManager",
		"InterfacesRemoved", on_interfaces_removed, NULL);
	sd_bus_match_signal(bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.Properties",
		"PropertiesChanged", on_properties_changed, NULL);
	sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed, NULL);
	bt.fd = sd_bus_get_fd(bus);
	loop_add_fd(panel->loop, bt.fd, POLLIN, bus_in, NULL);
	refresh();
	bus_in(bt.fd, 0, NULL);
}

void bt_fini(void) {
	if (bt.bus) {
		if (bt.discovering) {
			bt_set_discovery(false);
		}
		stop_pairing();
		loop_remove_fd(bt.panel->loop, bt.fd);
		sd_bus_flush_close_unref(bt.bus);
		bt.bus = NULL;
	}
	if (bt.devices) {
		forget_everything();
		list_free(bt.devices);
		bt.devices = NULL;
	}
	bt.panel = NULL;
}

bool bt_available(void) {
	return bt.bus && bt.adapter;
}

bool bt_powered(void) {
	return bt_available() && bt.powered;
}

bool bt_discovering(void) {
	return bt_available() && bt.discovering;
}

list_t *bt_devices(void) {
	return bt.devices;
}

const char *bt_status(void) {
	return bt.status[0] ? bt.status : NULL;
}

void bt_set_powered(bool on) {
	if (!bt_available()) {
		return;
	}
	bt.status[0] = '\0';
	bt.powered = on; // shown right away, BlueZ confirms with a signal
	start(OP_POWER, NULL, "org.freedesktop.DBus.Properties", "Set", 0, "ssv", ADAPTER_IFACE,
		"Powered", "b", on);
	changed();
}

void bt_set_discovery(bool on) {
	if (bt_powered() || (!on && bt_available())) {
		start(OP_DISCOVERY, NULL, ADAPTER_IFACE, on ? "StartDiscovery" : "StopDiscovery", 0,
			NULL);
	}
}

void bt_connect(const char *path, bool connect) {
	struct bt_device *d = device_find(path, false);
	if (!d || d->busy) {
		return;
	}
	d->busy = true;
	bt.status[0] = '\0';
	start(connect ? OP_CONNECT : OP_DISCONNECT, path, DEVICE_IFACE,
		connect ? "Connect" : "Disconnect", LONG_TIMEOUT, NULL);
	changed();
}

void bt_pair(const char *path) {
	struct bt_device *d = device_find(path, false);
	if (!d || d->busy || bt.pairing) {
		return;
	}
	d->busy = true;
	bt.pairing = strdup(path);
	snprintf(bt.status, sizeof(bt.status), "Pairing with %s…", device_label(path));
	start(OP_REGISTER, path, "org.bluez.AgentManager1", "RegisterAgent", 0, "os", AGENT_PATH,
		"KeyboardDisplay");
	changed();
}

void bt_remove(const char *path) {
	if (bt_available() && device_find(path, false)) {
		bt.status[0] = '\0';
		start(OP_REMOVE, path, ADAPTER_IFACE, "RemoveDevice", 0, "o", path);
	}
}

#else

void bt_init(struct panel *panel) {
}

void bt_fini(void) {
}

bool bt_available(void) {
	return false;
}

bool bt_powered(void) {
	return false;
}

bool bt_discovering(void) {
	return false;
}

list_t *bt_devices(void) {
	return NULL;
}

const char *bt_status(void) {
	return NULL;
}

void bt_set_powered(bool on) {
}

void bt_set_discovery(bool on) {
}

void bt_connect(const char *path, bool connect) {
}

void bt_pair(const char *path) {
}

void bt_remove(const char *path) {
}

#endif
