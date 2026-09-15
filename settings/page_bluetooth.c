#include <stdio.h>
#include <string.h>
#include "settings.h"

/*
 * Bluetooth page: Bluetooth on and off, the paired devices with connect,
 * disconnect and remove, and a search for nearby devices to pair. It talks to
 * BlueZ over D-Bus and never starts bluetoothd. While a device pairs, an
 * agent shows codes to type on it and accepts the confirmation of that pairing.
 */

#define BLUEZ "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define AGENT_PATH "/org/tilewin/settings/bluetooth_agent"
#define LONG_TIMEOUT 60000

static const char agent_xml[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

struct bluetooth_page {
	struct settings *s;
	bool updating;
	GDBusObjectManager *manager;
	GDBusConnection *bus;
	char *adapter;
	GtkWidget *missing, *groups, *power_switch;
	GtkWidget *paired_group, *paired_empty;
	GtkWidget *nearby_group, *nearby_empty, *search_button, *status;
	GPtrArray *rows; // GtkWidget * rows that are rebuilt
	guint agent_id, rebuild_id;
	char *pairing;
	bool searching;
};

static GVariant *property(GDBusObject *object, const char *iface, const char *name) {
	GDBusInterface *interface = g_dbus_object_get_interface(object, iface);
	if (!interface) {
		return NULL;
	}
	GVariant *value = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(interface), name);
	g_object_unref(interface);
	return value;
}

static bool property_bool(GDBusObject *object, const char *iface, const char *name) {
	GVariant *value = property(object, iface, name);
	bool result = value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
		g_variant_get_boolean(value);
	if (value) {
		g_variant_unref(value);
	}
	return result;
}

static char *property_string(GDBusObject *object, const char *iface, const char *name) {
	GVariant *value = property(object, iface, name);
	char *result = value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ?
		g_variant_dup_string(value, NULL) : NULL;
	if (value) {
		g_variant_unref(value);
	}
	return result;
}

static void set_status(struct bluetooth_page *p, const char *text) {
	gtk_label_set_text(GTK_LABEL(p->status), text ? text : "");
	gtk_widget_set_visible(p->status, text && *text);
}

static const char *device_name(struct bluetooth_page *p, const char *path) {
	static char name[128];
	GDBusObject *object = p->manager && path ?
		g_dbus_object_manager_get_object(p->manager, path) : NULL;
	char *alias = object ? property_string(object, DEVICE_IFACE, "Alias") : NULL;
	snprintf(name, sizeof(name), "%s", alias ? alias : "the device");
	g_free(alias);
	if (object) {
		g_object_unref(object);
	}
	return name;
}

/* Unnamed nearby devices only have their address as name, e.g. "4C-87-5D-12-AB-01". */
static bool looks_like_address(const char *name) {
	if (!name || strlen(name) != 17) {
		return !name || !*name;
	}
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2 ? (name[i] != '-' && name[i] != ':') : !g_ascii_isxdigit(name[i])) {
			return false;
		}
	}
	return true;
}

/* ---------- calls ---------- */

struct call {
	struct bluetooth_page *p;
	char *path;
	enum { CALL_POWER, CALL_CONNECT, CALL_DISCONNECT, CALL_REMOVE, CALL_DISCOVERY,
		CALL_REGISTER, CALL_PAIR, CALL_OTHER } kind;
};

static void call_done(GObject *source, GAsyncResult *result, gpointer data);

static void call(struct bluetooth_page *p, int kind, const char *path, const char *object,
		const char *iface, const char *method, GVariant *params, int timeout) {
	if (!p->bus || !object) {
		if (params) {
			g_variant_unref(g_variant_ref_sink(params));
		}
		return;
	}
	struct call *c = g_new0(struct call, 1);
	c->p = p;
	c->path = g_strdup(path);
	c->kind = kind;
	g_dbus_connection_call(p->bus, BLUEZ, object, iface, method, params, NULL,
		G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout, NULL, call_done, c);
}

static void stop_agent(struct bluetooth_page *p) {
	if (p->agent_id) {
		call(p, CALL_OTHER, NULL, "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent",
			g_variant_new("(o)", AGENT_PATH), -1);
		g_dbus_connection_unregister_object(p->bus, p->agent_id);
		p->agent_id = 0;
	}
	g_clear_pointer(&p->pairing, g_free);
}

static void call_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct call *c = data;
	struct bluetooth_page *p = c->p;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
	if (reply) {
		g_variant_unref(reply);
	}
	char *message = error ? g_dbus_error_get_remote_error(error) : NULL;
	if (error) {
		g_dbus_error_strip_remote_error(error);
	}
	bool exists = message && strcmp(message, "org.bluez.Error.AlreadyExists") == 0;
	char text[300] = "";
	switch (c->kind) {
	case CALL_CONNECT:
	case CALL_DISCONNECT:
		if (error) {
			snprintf(text, sizeof(text), "Couldn't %s %s: %s", c->kind == CALL_CONNECT ?
				"connect to" : "disconnect", device_name(p, c->path), error->message);
			set_status(p, text);
		}
		break;
	case CALL_REGISTER:
		if (error && !exists) {
			snprintf(text, sizeof(text), "Couldn't pair: %s", error->message);
			set_status(p, text);
			stop_agent(p);
		} else {
			call(p, CALL_PAIR, c->path, c->path, DEVICE_IFACE, "Pair", NULL, LONG_TIMEOUT);
		}
		break;
	case CALL_PAIR:
		stop_agent(p);
		if (error && !exists) {
			snprintf(text, sizeof(text), "Couldn't pair with %s: %s", device_name(p, c->path),
				error->message);
		} else {
			snprintf(text, sizeof(text), "Paired with %s", device_name(p, c->path));
			call(p, CALL_OTHER, c->path, c->path, "org.freedesktop.DBus.Properties", "Set",
				g_variant_new("(ssv)", DEVICE_IFACE, "Trusted", g_variant_new_boolean(TRUE)), -1);
			call(p, CALL_CONNECT, c->path, c->path, DEVICE_IFACE, "Connect", NULL, LONG_TIMEOUT);
		}
		set_status(p, text);
		break;
	case CALL_POWER:
	case CALL_REMOVE:
		if (error) {
			snprintf(text, sizeof(text), "Bluetooth didn't accept that: %s", error->message);
			set_status(p, text);
		}
		break;
	case CALL_DISCOVERY:
	case CALL_OTHER:
		break;
	}
	g_free(message);
	g_clear_error(&error);
	g_free(c->path);
	g_free(c);
}

/* ---------- pairing agent ---------- */

static void agent_method(GDBusConnection *connection, const char *sender, const char *path,
		const char *iface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, gpointer data) {
	struct bluetooth_page *p = data;
	if (strcmp(method, "Release") == 0 || strcmp(method, "Cancel") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}
	if (!p->pairing) {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
			"Only the device chosen in tileWin Settings is paired");
		return;
	}
	char text[200];
	if (strcmp(method, "RequestPinCode") == 0) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "0000"));
	} else if (strcmp(method, "RequestPasskey") == 0) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0));
	} else if (strcmp(method, "DisplayPinCode") == 0) {
		const char *device, *pin;
		g_variant_get(params, "(&o&s)", &device, &pin);
		snprintf(text, sizeof(text), "Type %s on %s, then press Enter", pin,
			device_name(p, device));
		set_status(p, text);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "DisplayPasskey") == 0) {
		const char *device;
		guint32 passkey;
		guint16 entered;
		g_variant_get(params, "(&ouq)", &device, &passkey, &entered);
		snprintf(text, sizeof(text), "Type %06u on %s, then press Enter", passkey,
			device_name(p, device));
		set_status(p, text);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "RequestConfirmation") == 0) {
		const char *device;
		guint32 passkey;
		g_variant_get(params, "(&ou)", &device, &passkey);
		snprintf(text, sizeof(text), "Pairing code %06u", passkey);
		set_status(p, text);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else {
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = agent_method };

static void pair(struct bluetooth_page *p, const char *path) {
	if (p->pairing || !p->bus) {
		return;
	}
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(agent_xml, NULL);
	p->agent_id = g_dbus_connection_register_object(p->bus, AGENT_PATH, node->interfaces[0],
		&agent_vtable, p, NULL, NULL);
	g_dbus_node_info_unref(node);
	p->pairing = g_strdup(path);
	char text[200];
	snprintf(text, sizeof(text), "Pairing with %s…", device_name(p, path));
	set_status(p, text);
	call(p, CALL_REGISTER, path, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
		g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"), -1);
}

/* ---------- the page ---------- */

static void on_device_action(GtkButton *button, gpointer data) {
	struct bluetooth_page *p = data;
	const char *path = g_object_get_data(G_OBJECT(button), "path");
	const char *action = g_object_get_data(G_OBJECT(button), "action");
	set_status(p, NULL);
	if (strcmp(action, "connect") == 0) {
		call(p, CALL_CONNECT, path, path, DEVICE_IFACE, "Connect", NULL, LONG_TIMEOUT);
	} else if (strcmp(action, "disconnect") == 0) {
		call(p, CALL_DISCONNECT, path, path, DEVICE_IFACE, "Disconnect", NULL, LONG_TIMEOUT);
	} else if (strcmp(action, "remove") == 0) {
		call(p, CALL_REMOVE, path, p->adapter, ADAPTER_IFACE, "RemoveDevice",
			g_variant_new("(o)", path), -1);
	} else if (strcmp(action, "pair") == 0) {
		pair(p, path);
	}
	gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
}

static GtkWidget *action_button(struct bluetooth_page *p, const char *label, const char *icon,
		const char *action, const char *path) {
	GtkWidget *button = icon ? gtk_button_new_from_icon_name(icon) :
		gtk_button_new_with_label(label);
	if (icon) {
		gtk_widget_set_tooltip_text(button, label);
		gtk_widget_add_css_class(button, "flat");
	}
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
	g_object_set_data(G_OBJECT(button), "action", (gpointer)action);
	g_signal_connect(button, "clicked", G_CALLBACK(on_device_action), p);
	return button;
}

static int object_cmp(gconstpointer a, gconstpointer b) {
	return g_strcmp0(g_dbus_object_get_object_path(*(GDBusObject **)a),
		g_dbus_object_get_object_path(*(GDBusObject **)b));
}

static void rebuild(struct bluetooth_page *p) {
	for (guint i = 0; i < p->rows->len; i++) {
		GtkWidget *row = p->rows->pdata[i];
		gtk_list_box_remove(GTK_LIST_BOX(gtk_widget_get_parent(row)), row);
	}
	g_ptr_array_set_size(p->rows, 0);
	g_clear_pointer(&p->adapter, g_free);

	GList *objects = p->manager ? g_dbus_object_manager_get_objects(p->manager) : NULL;
	GPtrArray *sorted = g_ptr_array_new();
	for (GList *l = objects; l; l = l->next) {
		g_ptr_array_add(sorted, l->data);
		GDBusInterface *adapter = g_dbus_object_get_interface(l->data, ADAPTER_IFACE);
		if (adapter) {
			if (!p->adapter) {
				p->adapter = g_strdup(g_dbus_object_get_object_path(l->data));
			}
			g_object_unref(adapter);
		}
	}
	g_ptr_array_sort(sorted, object_cmp);
	gtk_widget_set_visible(p->missing, p->adapter == NULL);
	gtk_widget_set_visible(p->groups, p->adapter != NULL);

	bool powered = false;
	if (p->adapter) {
		GDBusObject *adapter = g_dbus_object_manager_get_object(p->manager, p->adapter);
		powered = property_bool(adapter, ADAPTER_IFACE, "Powered");
		p->searching = property_bool(adapter, ADAPTER_IFACE, "Discovering");
		g_object_unref(adapter);
	}
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->power_switch), powered);
	p->updating = false;

	int paired_count = 0, nearby_count = 0;
	size_t prefix = p->adapter ? strlen(p->adapter) : 0;
	for (guint i = 0; p->adapter && i < sorted->len; i++) {
		GDBusObject *object = sorted->pdata[i];
		const char *path = g_dbus_object_get_object_path(object);
		GDBusInterface *device = g_dbus_object_get_interface(object, DEVICE_IFACE);
		if (!device) {
			continue;
		}
		g_object_unref(device);
		if (strncmp(path, p->adapter, prefix) != 0 || path[prefix] != '/') {
			continue;
		}
		char *name = property_string(object, DEVICE_IFACE, "Alias");
		char *icon = property_string(object, DEVICE_IFACE, "Icon");
		bool paired = property_bool(object, DEVICE_IFACE, "Paired");
		bool connected = property_bool(object, DEVICE_IFACE, "Connected");
		GtkWidget *row = NULL;
		if (paired) {
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			bool busy = p->pairing && strcmp(p->pairing, path) == 0;
			GtkWidget *button = action_button(p, connected ? "Disconnect" : "Connect", NULL,
				connected ? "disconnect" : "connect", path);
			gtk_widget_set_sensitive(button, powered && !busy);
			gtk_box_append(GTK_BOX(box), button);
			gtk_box_append(GTK_BOX(box), action_button(p, "Remove device", "user-trash-symbolic",
				"remove", path));
			row = ui_row(p->paired_group, name, connected ? "Connected" : "Not connected", box);
			paired_count++;
		} else if (p->searching && !looks_like_address(name)) {
			GtkWidget *button = action_button(p, "Pair", NULL, "pair", path);
			gtk_widget_add_css_class(button, "suggested-action");
			gtk_widget_set_sensitive(button, !p->pairing);
			row = ui_row(p->nearby_group, name, "Not paired", button);
			nearby_count++;
		}
		if (row) {
			GtkWidget *image = gtk_image_new_from_icon_name(icon ? icon : "bluetooth");
			gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
			gtk_box_prepend(GTK_BOX(ui_row_box(row)), image);
			g_ptr_array_add(p->rows, row);
		}
		g_free(name);
		g_free(icon);
	}
	g_ptr_array_free(sorted, TRUE);
	g_list_free_full(objects, g_object_unref);

	gtk_widget_set_visible(p->paired_empty, paired_count == 0);
	gtk_label_set_text(GTK_LABEL(p->nearby_empty), !powered ?
		"Turn on Bluetooth to look for devices" : p->searching && nearby_count > 0 ?
		"Searching for devices…" : p->searching ?
		"Searching… make the device visible (pairing mode)" :
		"Search to find headphones, mice, keyboards and phones nearby");
	gtk_button_set_label(GTK_BUTTON(p->search_button), p->searching ? "Stop" : "Search");
	gtk_widget_set_sensitive(p->search_button, powered);
}

static gboolean rebuild_idle(gpointer data) {
	struct bluetooth_page *p = data;
	p->rebuild_id = 0;
	rebuild(p);
	return G_SOURCE_REMOVE;
}

static void schedule_rebuild(struct bluetooth_page *p) {
	if (!p->rebuild_id) {
		p->rebuild_id = g_timeout_add(50, rebuild_idle, p);
	}
}

static void on_manager_changed(GDBusObjectManager *manager, gpointer data) {
	schedule_rebuild(data);
}

static void on_properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
		GDBusProxy *proxy, GVariant *changed, const char *const *invalidated, gpointer data) {
	schedule_rebuild(data);
}

static void manager_ready(GObject *source, GAsyncResult *result, gpointer data) {
	struct bluetooth_page *p = data;
	GError *error = NULL;
	p->manager = g_dbus_object_manager_client_new_for_bus_finish(result, &error);
	if (!p->manager) {
		gtk_label_set_text(GTK_LABEL(p->missing), "Bluetooth isn't available: the system bus "
			"can't be reached.");
		g_clear_error(&error);
		return;
	}
	p->bus = g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(p->manager));
	g_signal_connect_swapped(p->manager, "object-added", G_CALLBACK(on_manager_changed), p);
	g_signal_connect_swapped(p->manager, "object-removed", G_CALLBACK(on_manager_changed), p);
	g_signal_connect_swapped(p->manager, "interface-added", G_CALLBACK(on_manager_changed), p);
	g_signal_connect_swapped(p->manager, "interface-removed", G_CALLBACK(on_manager_changed), p);
	g_signal_connect_swapped(p->manager, "notify::name-owner", G_CALLBACK(on_manager_changed), p);
	g_signal_connect(p->manager, "interface-proxy-properties-changed",
		G_CALLBACK(on_properties_changed), p);
	rebuild(p);
}

static gboolean on_power(GtkSwitch *widget, gboolean state, gpointer data) {
	struct bluetooth_page *p = data;
	if (!p->updating) {
		set_status(p, NULL);
		call(p, CALL_POWER, NULL, p->adapter, "org.freedesktop.DBus.Properties", "Set",
			g_variant_new("(ssv)", ADAPTER_IFACE, "Powered", g_variant_new_boolean(state)), -1);
	}
	return FALSE;
}

static void on_search(GtkButton *button, gpointer data) {
	struct bluetooth_page *p = data;
	call(p, CALL_DISCOVERY, NULL, p->adapter, ADAPTER_IFACE,
		p->searching ? "StopDiscovery" : "StartDiscovery", NULL, -1);
}

static void on_unmap(GtkWidget *widget, gpointer data) {
	struct bluetooth_page *p = data;
	if (p->searching && p->adapter) {
		call(p, CALL_DISCOVERY, NULL, p->adapter, ADAPTER_IFACE, "StopDiscovery", NULL, -1);
	}
}

void bluetooth_page_refresh(struct settings *s) {
	if (s->bluetooth_page) {
		schedule_rebuild(s->bluetooth_page);
	}
}

GtkWidget *bluetooth_page_new(struct settings *s) {
	struct bluetooth_page *p = g_new0(struct bluetooth_page, 1);
	p->s = s;
	p->rows = g_ptr_array_new();
	s->bluetooth_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Bluetooth",
		"Connect headphones, speakers, mice, keyboards and phones.", &content);

	p->missing = gtk_label_new("Bluetooth isn't available: this computer has no Bluetooth "
		"adapter, or the bluetooth service isn't running (sudo systemctl enable --now bluetooth).");
	gtk_label_set_xalign(GTK_LABEL(p->missing), 0);
	gtk_label_set_wrap(GTK_LABEL(p->missing), TRUE);
	gtk_widget_add_css_class(p->missing, "dim-label");
	gtk_box_append(GTK_BOX(content), p->missing);

	p->groups = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_visible(p->groups, FALSE);
	gtk_box_append(GTK_BOX(content), p->groups);
	GtkWidget *power = ui_group(p->groups, NULL, NULL);
	p->power_switch = gtk_switch_new();
	g_signal_connect(p->power_switch, "state-set", G_CALLBACK(on_power), p);
	ui_row(power, "Bluetooth", NULL, p->power_switch);

	p->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->status), 0);
	gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
	gtk_widget_add_css_class(p->status, "tw-heading");
	gtk_widget_set_margin_top(p->status, 12);
	gtk_widget_set_visible(p->status, FALSE);
	gtk_box_append(GTK_BOX(p->groups), p->status);

	p->paired_group = ui_group(p->groups, "Your devices", NULL);
	p->paired_empty = ui_row(p->paired_group, "No paired devices yet", NULL, NULL);

	p->nearby_group = ui_group(p->groups, "Add a device", NULL);
	p->search_button = gtk_button_new_with_label("Search");
	g_signal_connect(p->search_button, "clicked", G_CALLBACK(on_search), p);
	GtkWidget *search_row = ui_row(p->nearby_group, NULL, NULL, p->search_button);
	p->nearby_empty = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->nearby_empty), 0);
	gtk_label_set_wrap(GTK_LABEL(p->nearby_empty), TRUE);
	gtk_widget_set_hexpand(p->nearby_empty, TRUE);
	gtk_box_prepend(GTK_BOX(ui_row_box(search_row)), p->nearby_empty);

	g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), p);
	g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM,
		G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, BLUEZ, "/", NULL, NULL, NULL, NULL,
		manager_ready, p);
	return page;
}
