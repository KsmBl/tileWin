#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/*
 * Network page: each saved network of NetworkManager (Wi-Fi, cable, ...) with
 * its IP address (from DHCP or fixed) and the password of a Wi-Fi network to
 * show and change. The same code, with only the network picker and the DNS
 * servers, is the "Servers of single networks" section of the DNS page, so
 * all DNS is set in one place. All of it
 * goes through nmcli; changing a connection and reading its password take
 * polkit's yes, which NetworkManager gives administrators at the computer.
 * For tests TILEWIN_NMCLI is run instead of nmcli.
 *
 * Its advanced settings are NetworkManager's (connect automatically,
 * metered, the hardware address), and three of tileWin's own that
 * NetworkManager has no setting for: connect again after an amount of data
 * and the speed limits. Those are kept in /etc/tileWin/network.conf by
 * tilewin-network-root (pkexec), which NetworkManager's dispatcher runs as
 * each network comes up. For tests TILEWIN_NETWORK_HELPER is run as it is,
 * without pkexec, and TILEWIN_NETWORK_CONF is read instead.
 */

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

struct net_conn {
	char *name, *uuid, *type, *device;
	bool active;
};

struct network_page {
	struct settings *s;
	bool updating, loading;
	GPtrArray *conns; // struct net_conn *
	char *uuid;       // the network shown
	char *device;     // its device while connected
	char *key_mgmt;   // its Wi-Fi security, "" for an open network
	GtkWidget *conn_dd, *conn_sub, *details, *message;
	GtkWidget *now_list; // the Network page's "Connected now": every network in use
	GtkWidget *method_dd, *address, *prefix_dd, *gateway, *fixed_rows[3], *apply_row, *now_sub;
	GtkWidget *dns_now, *dns_dd, *dns_list, *dns_pick, *dns_entry_row, *dns_entry, *dns_note, *dns_empty;
	GPtrArray *dns, *dns_rows, *dns_picks;
	GtkWidget *wifi_group, *password, *show_button, *save_password, *password_sub;
	GtkWidget *auto_switch, *metered_dd, *mac_dd, *mac_row;
	GtkWidget *limit_switch[3], *limit_spin[3], *limits_apply, *limits_sub;
	char *mac_key; // the setting of the hardware address of its kind of network
	bool dns_syncing, was_fixed, shown;
	char *now_address, *now_gateway; // the lease, to start a fixed address from
};

static const char *nmcli(void) {
	const char *test = g_getenv("TILEWIN_NMCLI");
	return test && *test ? test : "nmcli";
}

static void set_message(struct network_page *p, const char *text, bool error) {
	gtk_label_set_text(GTK_LABEL(p->message), text ? text : "");
	gtk_widget_set_visible(p->message, text && *text);
	if (error) {
		gtk_widget_add_css_class(p->message, "error");
	} else {
		gtk_widget_remove_css_class(p->message, "error");
	}
}

/* ---------- nmcli ---------- */

struct run {
	struct network_page *p;
	void (*done)(struct network_page *p, bool ok, char *out, char *err, gpointer data);
	gpointer data;
};

static void run_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct run *r = data;
	char *out = NULL, *err = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		NULL) && g_subprocess_get_successful(G_SUBPROCESS(source));
	r->done(r->p, ok, out, err, r->data);
	if (out) {
		memset(out, 0, strlen(out)); // it may have held a password
	}
	g_free(out);
	g_free(err);
	g_free(r);
}

/* Runs nmcli with the words of argv (NULL ended), and calls done with what it said. */
static void run(struct network_page *p, const char *const *argv,
		void (*done)(struct network_page *, bool, char *, char *, gpointer), gpointer data) {
	GPtrArray *args = g_ptr_array_new();
	g_ptr_array_add(args, (gpointer)nmcli());
	for (int i = 0; argv[i]; i++) {
		g_ptr_array_add(args, (gpointer)argv[i]);
	}
	g_ptr_array_add(args, NULL);
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv((const char *const *)args->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_ptr_array_free(args, TRUE);
	if (!proc) {
		done(p, false, NULL, g_strdup(error->message), data);
		g_error_free(error);
		return;
	}
	struct run *r = g_new0(struct run, 1);
	r->p = p;
	r->done = done;
	r->data = data;
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, run_finished, r);
	g_object_unref(proc);
}

/* Splits a line of nmcli -t at the ':' that are not escaped, and unescapes. */
static char **split_terse(const char *line) {
	GPtrArray *fields = g_ptr_array_new();
	GString *field = g_string_new(NULL);
	for (const char *c = line; *c; c++) {
		if (*c == '\\' && c[1]) {
			g_string_append_c(field, *++c);
		} else if (*c == ':') {
			g_ptr_array_add(fields, g_string_free(field, FALSE));
			field = g_string_new(NULL);
		} else {
			g_string_append_c(field, *c);
		}
	}
	g_ptr_array_add(fields, g_string_free(field, FALSE));
	g_ptr_array_add(fields, NULL);
	return (char **)g_ptr_array_free(fields, FALSE);
}

static void conn_free(gpointer data) {
	struct net_conn *c = data;
	g_free(c->name);
	g_free(c->uuid);
	g_free(c->type);
	g_free(c->device);
	g_free(c);
}

static const char *type_label(const char *type) {
	if (strcmp(type, "802-11-wireless") == 0) {
		return "Wi-Fi";
	}
	if (strcmp(type, "802-3-ethernet") == 0) {
		return "Cable";
	}
	if (strcmp(type, "vpn") == 0 || strcmp(type, "wireguard") == 0) {
		return "VPN";
	}
	if (strcmp(type, "gsm") == 0 || strcmp(type, "cdma") == 0) {
		return "Mobile";
	}
	return type;
}

/* ---------- the network shown ---------- */

static void show_details(struct network_page *p);

static void dns_sync(struct network_page *p);

static bool is_fixed(struct network_page *p) {
	return p->method_dd ? gtk_drop_down_get_selected(GTK_DROP_DOWN(p->method_dd)) == 1 :
		p->was_fixed;
}

static void sync_rows(struct network_page *p) {
	bool fixed = is_fixed(p);
	if (p->method_dd) {
		for (int i = 0; i < 3; i++) {
			gtk_widget_set_visible(p->fixed_rows[i], fixed);
		}
		// DHCP to DHCP changes nothing
		gtk_widget_set_visible(p->apply_row, fixed || p->was_fixed);
	}
	if (!p->dns_dd) {
		return;
	}
	bool own = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->dns_dd)) == 1;
	gtk_widget_set_visible(gtk_widget_get_parent(p->dns_list), own);
	// without DHCP the network hands out no servers: say where they come from
	char *global = dns_networkmanager_global();
	const char *note = NULL;
	if (!own && dns_service_enabled()) {
		note = "Asked: the servers for all networks above";
	} else if (!own && *global) {
		note = "Asked: NetworkManager's own servers for all networks";
	} else if (!own && fixed) {
		note = "This network has a fixed address (Network page), so it hands out no servers: "
			"choose some here, or for all networks above";
	} else if (own && *global && !dns_service_enabled()) {
		note = "NetworkManager has servers for all networks of its own, which win over these "
			"while the DNS service is off";
	}
	gtk_label_set_text(GTK_LABEL(p->dns_note), note ? note : "");
	gtk_widget_set_visible(p->dns_note, note != NULL);
	g_free(global);
}

static void advanced_show(struct network_page *p, const char *type, const char *autoconnect,
	const char *metered, const char *mac);

static void details_loaded(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	char *uuid = data;
	p->loading = false;
	if (g_strcmp0(uuid, p->uuid) != 0) {
		g_free(uuid);
		return; // another network was picked meanwhile
	}
	g_free(uuid);
	if (!ok) {
		set_message(p, err && *err ? g_strstrip(err) : "NetworkManager did not answer", true);
		return;
	}
	char *method = NULL, *addresses = NULL, *gateway = NULL, *dns4 = NULL, *dns6 = NULL;
	char *ignore = NULL, *type = NULL, *autoconnect = NULL, *metered = NULL, *mac = NULL;
	GString *now_dns = g_string_new(NULL);
	g_clear_pointer(&p->now_address, g_free);
	g_clear_pointer(&p->now_gateway, g_free);
	g_clear_pointer(&p->device, g_free);
	g_clear_pointer(&p->key_mgmt, g_free);
	char **lines = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *colon = strchr(lines[i], ':');
		if (!colon) {
			continue;
		}
		*colon = '\0';
		const char *key = lines[i], *value = colon + 1;
		if (strcmp(key, "ipv4.method") == 0) {
			method = g_strdup(value);
		} else if (strcmp(key, "ipv4.addresses") == 0) {
			addresses = g_strdup(value);
		} else if (strcmp(key, "ipv4.gateway") == 0) {
			gateway = g_strdup(value);
		} else if (strcmp(key, "ipv4.dns") == 0) {
			dns4 = g_strdup(value);
		} else if (strcmp(key, "ipv6.dns") == 0) {
			dns6 = g_strdup(value);
		} else if (strcmp(key, "ipv4.ignore-auto-dns") == 0) {
			ignore = g_strdup(value);
		} else if (strcmp(key, "connection.type") == 0) {
			type = g_strdup(value);
		} else if (strcmp(key, "connection.autoconnect") == 0) {
			autoconnect = g_strdup(value);
		} else if (strcmp(key, "connection.metered") == 0) {
			metered = g_strdup(value);
		} else if ((strcmp(key, "802-11-wireless.cloned-mac-address") == 0 ||
				strcmp(key, "802-3-ethernet.cloned-mac-address") == 0) && !mac) {
			mac = g_strdup(value);
		} else if (strcmp(key, "GENERAL.DEVICES") == 0 && *value) {
			p->device = g_strdup(value);
		} else if (strcmp(key, "802-11-wireless-security.key-mgmt") == 0) {
			p->key_mgmt = g_strdup(value);
		} else if (g_str_has_prefix(key, "IP4.ADDRESS") && !p->now_address) {
			p->now_address = g_strdup(value);
		} else if (strcmp(key, "IP4.GATEWAY") == 0 && *value) {
			p->now_gateway = g_strdup(value);
		} else if (g_str_has_prefix(key, "IP4.DNS") || g_str_has_prefix(key, "IP6.DNS")) {
			g_string_append_printf(now_dns, "%s%s", now_dns->len ? ", " : "", value);
		}
	}
	g_strfreev(lines);

	p->updating = true;
	bool fixed = g_strcmp0(method, "manual") == 0;
	p->was_fixed = fixed;
	if (p->method_dd) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->method_dd), fixed ? 1 : 0);
		// a fixed address as it is set, else the one of the lease to start from
		const char *address = fixed && addresses && *addresses ? addresses : p->now_address;
		char *first = address ? g_strndup(address, strcspn(address, ",")) : g_strdup("");
		char *slash = strchr(first, '/');
		int prefix = slash ? atoi(slash + 1) : 24;
		if (slash) {
			*slash = '\0';
		}
		gtk_editable_set_text(GTK_EDITABLE(p->address), first);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->prefix_dd),
			(guint)(prefix >= 8 && prefix <= 32 ? 32 - prefix : 8));
		g_free(first);
		const char *gw = fixed && gateway && *gateway ? gateway : p->now_gateway;
		gtk_editable_set_text(GTK_EDITABLE(p->gateway), gw ? gw : "");
	}

	g_ptr_array_set_size(p->dns, 0);
	char *both = g_strdup_printf("%s,%s", dns4 ? dns4 : "", dns6 ? dns6 : "");
	char **servers = g_strsplit_set(both, ",; ", -1);
	for (int i = 0; servers[i]; i++) {
		if (*servers[i]) {
			g_ptr_array_add(p->dns, g_strdup(servers[i]));
		}
	}
	g_strfreev(servers);
	g_free(both);
	bool own = p->dns->len > 0 && g_strcmp0(ignore, "yes") == 0;
	if (p->dns_dd) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->dns_dd), own ? 1 : 0);
	}
	if (!own) {
		g_ptr_array_set_size(p->dns, 0);
	}
	p->updating = false;
	if (p->dns_dd) {
		dns_sync(p);
	}

	GString *now = g_string_new(NULL);
	if (p->now_address) {
		g_string_append_printf(now, "Now %s", p->now_address);
		if (p->now_gateway) {
			g_string_append_printf(now, ", gateway %s", p->now_gateway);
		}
	} else {
		g_string_append(now, "Not connected now");
	}
	if (p->now_sub) {
		gtk_label_set_text(GTK_LABEL(p->now_sub), now->str);
	}
	if (p->dns_now) {
		char *text = now_dns->len ? g_strdup_printf("The network hands out %s", now_dns->str) :
			g_strdup(p->now_address ? "The network hands out no servers" : "Not connected now");
		gtk_label_set_text(GTK_LABEL(p->dns_now), text);
		g_free(text);
	}
	g_string_free(now, TRUE);
	g_string_free(now_dns, TRUE);

	if (p->wifi_group) {
		bool wifi = g_strcmp0(type, "802-11-wireless") == 0;
		gtk_widget_set_visible(gtk_widget_get_parent(p->wifi_group), wifi);
		bool open = !p->key_mgmt || !*p->key_mgmt || strcmp(p->key_mgmt, "none") == 0 ||
			strcmp(p->key_mgmt, "owe") == 0;
		gtk_editable_set_text(GTK_EDITABLE(p->password), "");
		p->shown = false;
		gtk_button_set_label(GTK_BUTTON(p->show_button), "Show");
		gtk_widget_set_sensitive(p->password, !open);
		gtk_widget_set_sensitive(p->show_button, !open);
		gtk_widget_set_sensitive(p->save_password, !open);
		gtk_label_set_text(GTK_LABEL(p->password_sub), open ? "This network has no password" :
			strcmp(p->key_mgmt, "wpa-eap") == 0 ?
			"The password of your account on this network" : "Hidden until you show it");
	}
	if (p->auto_switch) {
		advanced_show(p, type, autoconnect, metered, mac);
	}
	sync_rows(p);
	gtk_widget_set_sensitive(p->details, true);
	g_free(autoconnect);
	g_free(metered);
	g_free(mac);
	g_free(method);
	g_free(addresses);
	g_free(gateway);
	g_free(dns4);
	g_free(dns6);
	g_free(ignore);
	g_free(type);
}

static void show_details(struct network_page *p) {
	if (!p->uuid) {
		gtk_widget_set_sensitive(p->details, false);
		return;
	}
	p->loading = true;
	const char *argv[] = { "-t", "-f",
		"connection.type,GENERAL.DEVICES,ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,"
		"ipv4.ignore-auto-dns,ipv6.dns,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.DNS,"
		"802-11-wireless-security.key-mgmt,connection.autoconnect,connection.metered,"
		"802-11-wireless.cloned-mac-address,802-3-ethernet.cloned-mac-address",
		"connection", "show", "uuid", p->uuid, NULL };
	run(p, argv, details_loaded, g_strdup(p->uuid));
}

/* ---------- the networks ---------- */

static void on_conn_picked(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	if (p->updating || sel >= p->conns->len) {
		return;
	}
	struct net_conn *c = p->conns->pdata[sel];
	g_free(p->uuid);
	p->uuid = g_strdup(c->uuid);
	set_message(p, NULL, false);
	char *sub = c->active ? g_strdup_printf("%s, connected (%s)", type_label(c->type),
		c->device) : g_strdup_printf("%s, not connected", type_label(c->type));
	gtk_label_set_text(GTK_LABEL(p->conn_sub), sub);
	g_free(sub);
	show_details(p);
}

static int by_activity(gconstpointer a, gconstpointer b) {
	const struct net_conn *x = *(struct net_conn *const *)a, *y = *(struct net_conn *const *)b;
	if (x->active != y->active) {
		return x->active ? -1 : 1;
	}
	return g_utf8_collate(x->name, y->name);
}

/* ---------- the networks in use ---------- */

/* How a connected network gets its address, under its row: "Automatic (DHCP) ·
 * 192.168.1.129/24 · gateway 192.168.1.1". */
static void now_loaded(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	GtkWidget *label = data;
	if (gtk_widget_get_parent(label)) {
		const char *method = NULL;
		char *addr = NULL, *gw = NULL;
		char **lines = g_strsplit(ok && out ? out : "", "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char *colon = strchr(lines[i], ':');
			if (!colon) {
				continue;
			}
			*colon = '\0';
			const char *key = lines[i], *value = colon + 1;
			if (strcmp(key, "ipv4.method") == 0) {
				method = strcmp(value, "auto") == 0 ? "Automatic (DHCP)" :
					strcmp(value, "manual") == 0 ? "Fixed address" :
					strcmp(value, "link-local") == 0 ? "Link-local only" :
					strcmp(value, "shared") == 0 ? "Shared to other computers" :
					strcmp(value, "disabled") == 0 ? "No IPv4" : NULL;
			} else if (g_str_has_prefix(key, "IP4.ADDRESS") && !addr && *value) {
				addr = g_strdup(value);
			} else if (strcmp(key, "IP4.GATEWAY") == 0 && *value) {
				gw = g_strdup(value);
			}
		}
		GString *text = g_string_new(method ? method : ok ? "Connected" :
			"NetworkManager did not say how it is set up");
		if (addr) {
			g_string_append_printf(text, " · %s", addr);
		}
		if (gw) {
			g_string_append_printf(text, " · gateway %s", gw);
		}
		gtk_label_set_text(GTK_LABEL(label), text->str);
		g_string_free(text, TRUE);
		g_strfreev(lines);
		g_free(addr);
		g_free(gw);
	}
	g_object_unref(label);
}

static void on_now_edit(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	const char *uuid = g_object_get_data(G_OBJECT(button), "uuid");
	for (guint i = 0; i < p->conns->len; i++) {
		struct net_conn *c = p->conns->pdata[i];
		if (strcmp(c->uuid, uuid) == 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->conn_dd), i);
		}
	}
}

/* Every network in use at once, Wi-Fi and cable alike, with how each is set up. */
static void now_show(struct network_page *p) {
	if (!p->now_list) {
		return;
	}
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->now_list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->now_list), child);
	}
	bool any = false;
	for (guint i = 0; i < p->conns->len; i++) {
		struct net_conn *c = p->conns->pdata[i];
		if (!c->active) {
			continue;
		}
		any = true;
		char *title = g_strdup_printf("%s (%s%s%s)", c->name, type_label(c->type),
			*c->device ? ", " : "", c->device);
		GtkWidget *edit = gtk_button_new_with_label("Edit");
		g_object_set_data_full(G_OBJECT(edit), "uuid", g_strdup(c->uuid), g_free);
		g_signal_connect(edit, "clicked", G_CALLBACK(on_now_edit), p);
		GtkWidget *row = ui_row(p->now_list, title, "…", edit);
		g_free(title);
		GtkWidget *sub = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
		const char *argv[] = { "-t", "-f", "ipv4.method,IP4.ADDRESS,IP4.GATEWAY", "connection",
			"show", "uuid", c->uuid, NULL };
		run(p, argv, now_loaded, g_object_ref(sub));
	}
	if (!any) {
		ui_row(p->now_list, "Not connected", NULL, NULL);
	}
}

static void conns_loaded(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	g_ptr_array_set_size(p->conns, 0);
	char **lines = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f = split_terse(lines[i]);
		if (g_strv_length(f) >= 5 && strcmp(f[2], "loopback") != 0 &&
				strcmp(f[2], "bridge") != 0 && strcmp(f[2], "tun") != 0) {
			struct net_conn *c = g_new0(struct net_conn, 1);
			c->name = g_strdup(f[0]);
			c->uuid = g_strdup(f[1]);
			c->type = g_strdup(f[2]);
			c->device = g_strdup(f[3]);
			c->active = strcmp(f[4], "yes") == 0;
			g_ptr_array_add(p->conns, c);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_ptr_array_sort(p->conns, by_activity);
	now_show(p);
	GtkStringList *names = gtk_string_list_new(NULL);
	guint keep = 0;
	for (guint i = 0; i < p->conns->len; i++) {
		struct net_conn *c = p->conns->pdata[i];
		gtk_string_list_append(names, c->name);
		if (p->uuid && strcmp(c->uuid, p->uuid) == 0) {
			keep = i;
		}
	}
	p->updating = true;
	ui_drop_down_set_strings(GTK_DROP_DOWN(p->conn_dd), names);
	g_object_unref(names);
	p->updating = false;
	if (p->conns->len == 0) {
		gtk_label_set_text(GTK_LABEL(p->conn_sub), ok ? "NetworkManager has no saved networks" :
			"NetworkManager is not running, or nmcli is not installed");
		gtk_widget_set_sensitive(p->details, false);
		return;
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->conn_dd), keep);
	on_conn_picked(G_OBJECT(p->conn_dd), NULL, p);
}

static void load_conns(struct network_page *p) {
	const char *argv[] = { "-t", "-f", "NAME,UUID,TYPE,DEVICE,ACTIVE", "connection", "show",
		NULL };
	run(p, argv, conns_loaded, NULL);
}

void network_page_refresh(struct settings *s) {
	if (s->network_page) {
		load_conns(s->network_page);
	}
}

/* ---------- changing a network ---------- */

static void changed_done(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	const char *what = data;
	if (ok) {
		set_message(p, what, false);
		settings_status(p->s, "%s", what);
	} else {
		char *text = g_strdup_printf("NetworkManager refused: %s", err && *err ?
			g_strstrip(err) : "no reason given");
		set_message(p, text, true);
		g_free(text);
	}
	show_details(p);
}

/* The changed settings take effect on the connected device at once. */
static void modified(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	bool reconnect = GPOINTER_TO_INT(data);
	if (!ok || !p->device) {
		changed_done(p, ok, out, err, ok ? "Saved; used the next time it connects" : NULL);
		return;
	}
	if (reconnect) {
		const char *argv[] = { "connection", "up", "uuid", p->uuid, NULL };
		run(p, argv, changed_done, "Saved and connected again with the new address");
	} else {
		const char *argv[] = { "device", "reapply", p->device, NULL };
		run(p, argv, changed_done, "Saved and in use");
	}
}

static bool valid_ipv4(const char *text) {
	GInetAddress *a = g_inet_address_new_from_string(text);
	bool ok = a && g_inet_address_get_family(a) == G_SOCKET_FAMILY_IPV4;
	if (a) {
		g_object_unref(a);
	}
	return ok;
}

static void on_apply_address(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	if (!p->uuid) {
		return;
	}
	if (!is_fixed(p)) {
		const char *argv[] = { "connection", "modify", "uuid", p->uuid, "ipv4.method", "auto",
			"ipv4.addresses", "", "ipv4.gateway", "", NULL };
		run(p, argv, modified, GINT_TO_POINTER(1));
		return;
	}
	const char *address = gtk_editable_get_text(GTK_EDITABLE(p->address));
	const char *gateway = gtk_editable_get_text(GTK_EDITABLE(p->gateway));
	bool ok_address = valid_ipv4(address), ok_gateway = !*gateway || valid_ipv4(gateway);
	gtk_widget_remove_css_class(p->address, "error");
	gtk_widget_remove_css_class(p->gateway, "error");
	if (!ok_address || !ok_gateway) {
		gtk_widget_add_css_class(ok_address ? p->gateway : p->address, "error");
		set_message(p, ok_address ? "The gateway is no address like 192.168.1.1" :
			"The address is no address like 192.168.1.50", true);
		return;
	}
	char *with_prefix = g_strdup_printf("%s/%u", address,
		32 - gtk_drop_down_get_selected(GTK_DROP_DOWN(p->prefix_dd)));
	const char *argv[] = { "connection", "modify", "uuid", p->uuid, "ipv4.method", "manual",
		"ipv4.addresses", with_prefix, "ipv4.gateway", gateway, NULL };
	run(p, argv, modified, GINT_TO_POINTER(1));
	g_free(with_prefix);
}

static void on_method(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	if (!p->updating) {
		sync_rows(p);
	}
}

/* The DNS servers of the network: its own ones, or none of its own. */
static void save_dns(struct network_page *p) {
	bool own = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->dns_dd)) == 1 && p->dns->len > 0;
	GString *v4 = g_string_new(NULL), *v6 = g_string_new(NULL);
	for (guint i = 0; own && i < p->dns->len; i++) {
		const char *s = p->dns->pdata[i];
		GString *to = strchr(s, ':') ? v6 : v4;
		g_string_append_printf(to, "%s%s", to->len ? "," : "", s);
	}
	const char *argv[] = { "connection", "modify", "uuid", p->uuid,
		"ipv4.ignore-auto-dns", own ? "yes" : "no", "ipv4.dns", v4->str,
		"ipv6.ignore-auto-dns", own ? "yes" : "no", "ipv6.dns", v6->str, NULL };
	run(p, argv, modified, GINT_TO_POINTER(0));
	g_string_free(v4, TRUE);
	g_string_free(v6, TRUE);
}

static void on_dns_mode(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	if (p->updating) {
		return;
	}
	sync_rows(p);
	bool own = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->dns_dd)) == 1;
	if (!own || p->dns->len > 0) {
		save_dns(p); // own ones are saved once there is one
	}
}

static void on_dns_remove(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	const char *value = g_object_get_data(G_OBJECT(button), "value");
	for (guint i = 0; i < p->dns->len; i++) {
		if (strcmp(p->dns->pdata[i], value) == 0) {
			g_ptr_array_remove_index(p->dns, i);
			break;
		}
	}
	dns_sync(p);
	save_dns(p);
}

static void dns_add(struct network_page *p, const char *value) {
	for (guint i = 0; i < p->dns->len; i++) {
		if (strcmp(p->dns->pdata[i], value) == 0) {
			return;
		}
	}
	g_ptr_array_add(p->dns, g_strdup(value));
	dns_sync(p);
	save_dns(p);
}

/* As on_pick of the DNS page: the list of the dropdown is made anew by the
 * pick, so that waits until its popup is done with the click. */
static gboolean dns_picked(gpointer data) {
	struct network_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->dns_pick));
	if (sel == 0 || sel == GTK_INVALID_LIST_POSITION || sel > p->dns_picks->len) {
		return G_SOURCE_REMOVE;
	}
	char *value = g_strdup(p->dns_picks->pdata[sel - 1]);
	p->dns_syncing = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->dns_pick), 0);
	p->dns_syncing = false;
	if (!*value) {
		gtk_widget_set_visible(p->dns_entry_row, true);
		gtk_widget_grab_focus(p->dns_entry);
	} else {
		dns_add(p, value);
	}
	g_free(value);
	return G_SOURCE_REMOVE;
}

static void on_dns_pick(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	if (!p->dns_syncing && gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)) != 0) {
		g_idle_add(dns_picked, p);
	}
}

static void on_dns_typed(GtkWidget *widget, gpointer data) {
	struct network_page *p = data;
	char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->dns_entry))));
	GInetAddress *a = g_inet_address_new_from_string(text);
	if (!a) {
		gtk_widget_add_css_class(p->dns_entry, "error");
	} else {
		g_object_unref(a);
		gtk_widget_remove_css_class(p->dns_entry, "error");
		gtk_editable_set_text(GTK_EDITABLE(p->dns_entry), "");
		gtk_widget_set_visible(p->dns_entry_row, false);
		dns_add(p, text);
	}
	g_free(text);
}

static void dns_sync(struct network_page *p) {
	for (guint i = 0; i < p->dns_rows->len; i++) {
		gtk_list_box_remove(GTK_LIST_BOX(p->dns_list), p->dns_rows->pdata[i]);
	}
	g_ptr_array_set_size(p->dns_rows, 0);
	for (guint i = 0; i < p->dns->len; i++) {
		const char *value = p->dns->pdata[i];
		const char *name = dns_preset_name(NULL, value);
		char *title = name ? g_strdup_printf("%s  —  %s", value, name) : g_strdup(value);
		GtkWidget *remove = gtk_button_new_from_icon_name("list-remove-symbolic");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_tooltip_text(remove, "Remove this server");
		g_object_set_data_full(G_OBJECT(remove), "value", g_strdup(value), g_free);
		g_signal_connect(remove, "clicked", G_CALLBACK(on_dns_remove), p);
		GtkWidget *row = ui_row(p->dns_list, title, i == 0 ? "Asked first" : NULL, remove);
		g_object_ref(row);
		gtk_list_box_remove(GTK_LIST_BOX(p->dns_list), row);
		gtk_list_box_insert(GTK_LIST_BOX(p->dns_list), row, (int)i);
		g_object_unref(row);
		g_ptr_array_add(p->dns_rows, row);
		g_free(title);
	}
	gtk_widget_set_visible(p->dns_empty, p->dns->len == 0);
	GtkStringList *items = gtk_string_list_new(NULL);
	g_ptr_array_set_size(p->dns_picks, 0);
	gtk_string_list_append(items, "Add a server…");
	for (const struct dns_preset *pr = dns_server_presets; pr->value; pr++) {
		bool there = false;
		for (guint i = 0; i < p->dns->len; i++) {
			there |= strcmp(p->dns->pdata[i], pr->value) == 0;
		}
		if (!there) {
			char *label = g_strdup_printf("%s  (%s)", pr->value, pr->name);
			gtk_string_list_append(items, label);
			g_free(label);
			g_ptr_array_add(p->dns_picks, g_strdup(pr->value));
		}
	}
	if (p->now_gateway) {
		// the router itself often answers DNS for the network
		char *label = g_strdup_printf("%s  (the router of this network)", p->now_gateway);
		gtk_string_list_append(items, label);
		g_free(label);
		g_ptr_array_add(p->dns_picks, g_strdup(p->now_gateway));
	}
	gtk_string_list_append(items, "Other address…");
	g_ptr_array_add(p->dns_picks, g_strdup(""));
	p->dns_syncing = true;
	if (ui_drop_down_set_strings(GTK_DROP_DOWN(p->dns_pick), items)) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->dns_pick), 0);
	}
	p->dns_syncing = false;
	g_object_unref(items);
}

/* ---------- the Wi-Fi password ---------- */

static const char *secret_key(struct network_page *p) {
	if (p->key_mgmt && strcmp(p->key_mgmt, "wpa-eap") == 0) {
		return "802-1x.password";
	}
	if (p->key_mgmt && strcmp(p->key_mgmt, "ieee8021x") == 0) {
		return "802-11-wireless-security.wep-key0";
	}
	return "802-11-wireless-security.psk";
}

static void secret_loaded(struct network_page *p, bool ok, char *out, char *err, gpointer data) {
	gtk_widget_set_sensitive(p->show_button, true);
	if (!ok || !out) {
		gtk_label_set_text(GTK_LABEL(p->password_sub), "NetworkManager did not give the "
			"password: it takes the password of an administrator");
		return;
	}
	g_strchomp(out);
	gtk_editable_set_text(GTK_EDITABLE(p->password), out);
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(p->password), TRUE);
	// shown at once: that is what the button was pressed for
	GtkWidget *text = gtk_widget_get_first_child(p->password);
	if (GTK_IS_TEXT(text)) {
		gtk_text_set_visibility(GTK_TEXT(text), TRUE);
	}
	gtk_label_set_text(GTK_LABEL(p->password_sub), *out ? "Change it and save, or copy it" :
		"No password is saved for this network");
	p->shown = true;
	gtk_button_set_label(GTK_BUTTON(p->show_button), "Hide");
}

static void on_show_password(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	if (!p->uuid) {
		return;
	}
	if (p->shown) {
		p->shown = false;
		gtk_editable_set_text(GTK_EDITABLE(p->password), "");
		gtk_button_set_label(GTK_BUTTON(p->show_button), "Show");
		gtk_label_set_text(GTK_LABEL(p->password_sub), "Hidden until you show it");
		return;
	}
	gtk_widget_set_sensitive(p->show_button, false);
	const char *argv[] = { "-s", "-g", secret_key(p), "connection", "show", "uuid", p->uuid,
		NULL };
	run(p, argv, secret_loaded, NULL);
}

static void on_save_password(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	const char *password = gtk_editable_get_text(GTK_EDITABLE(p->password));
	size_t len = strlen(password);
	bool psk = strcmp(secret_key(p), "802-11-wireless-security.psk") == 0;
	if (psk && (len < 8 || len > 64)) {
		set_message(p, "A Wi-Fi password has 8 to 63 characters", true);
		return;
	}
	if (len == 0) {
		set_message(p, "The password is empty", true);
		return;
	}
	const char *argv[] = { "connection", "modify", "uuid", p->uuid, secret_key(p), password,
		NULL };
	run(p, argv, modified, GINT_TO_POINTER(1));
}

/* ---------- advanced ---------- */

static const char *const metered_labels[] = { "Decided by NetworkManager", "Yes", "No", NULL };
static const char *const metered_values[] = { "unknown", "yes", "no" };
static const char *const mac_labels[] = { "The device's own",
	"A new random one at every connection", "A random one kept for this network", NULL };
static const char *const mac_values[] = { "permanent", "random", "stable" };

enum { LIMIT_RECONNECT, LIMIT_DOWN, LIMIT_UP };

static char *limits_conf_path(void) {
	const char *test = g_getenv("TILEWIN_NETWORK_CONF");
	return g_strdup(test && *test ? test : "/etc/tileWin/network.conf");
}

static char *limits_helper(void) {
	const char *test = g_getenv("TILEWIN_NETWORK_HELPER");
	return test && *test ? g_strdup(test) :
		g_build_filename(TILEWIN_LIBEXECDIR, "tilewin-network-root", NULL);
}

/* The amounts kept for a network: megabytes, and kbit/s down and up; 0 is off. */
static void limits_read(const char *uuid, guint64 values[3]) {
	values[0] = values[1] = values[2] = 0;
	char *path = limits_conf_path(), *text = NULL;
	if (uuid && g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char **f = g_strsplit_set(g_strstrip(lines[i]), " \t", -1);
			if (g_strv_length(f) >= 4 && strcmp(f[0], uuid) == 0) {
				for (int k = 0; k < 3; k++) {
					values[k] = g_ascii_strtoull(f[k + 1], NULL, 10);
				}
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
	}
	g_free(text);
	g_free(path);
}

static void limits_sync(struct network_page *p) {
	for (int k = 0; k < 3; k++) {
		gtk_widget_set_sensitive(p->limit_spin[k],
			gtk_switch_get_active(GTK_SWITCH(p->limit_switch[k])));
	}
}

static void advanced_show(struct network_page *p, const char *type, const char *autoconnect,
		const char *metered, const char *mac) {
	bool was = p->updating;
	p->updating = true; // shown, not changed
	gtk_switch_set_active(GTK_SWITCH(p->auto_switch), g_strcmp0(autoconnect, "no") != 0);
	guint m = 0;
	for (guint i = 0; i < 3; i++) {
		m = g_strcmp0(metered, metered_values[i]) == 0 ? i : m;
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->metered_dd), m);
	g_clear_pointer(&p->mac_key, g_free);
	if (g_strcmp0(type, "802-11-wireless") == 0) {
		p->mac_key = g_strdup("802-11-wireless.cloned-mac-address");
	} else if (g_strcmp0(type, "802-3-ethernet") == 0) {
		p->mac_key = g_strdup("802-3-ethernet.cloned-mac-address");
	}
	gtk_widget_set_visible(p->mac_row, p->mac_key != NULL);
	guint a = 0; // also "", "preserve" and an address typed in elsewhere
	for (guint i = 1; i < 3; i++) {
		a = g_strcmp0(mac, mac_values[i]) == 0 ? i : a;
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mac_dd), a);

	guint64 v[3];
	limits_read(p->uuid, v);
	// megabytes as gigabytes, kbit/s as Mbit/s
	double shown[3] = { v[0] / 1000.0, v[1] / 1000.0, v[2] / 1000.0 };
	for (int k = 0; k < 3; k++) {
		gtk_switch_set_active(GTK_SWITCH(p->limit_switch[k]), v[k] > 0);
		if (v[k] > 0) {
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->limit_spin[k]), shown[k]);
		}
	}
	limits_sync(p);
	p->updating = was;
}

static void modify_one(struct network_page *p, const char *key, const char *value) {
	if (p->updating || !p->uuid) {
		return;
	}
	const char *argv[] = { "connection", "modify", "uuid", p->uuid, key, value, NULL };
	run(p, argv, changed_done, "Saved");
}

static void on_autoconnect(GObject *sw, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	modify_one(p, "connection.autoconnect",
		gtk_switch_get_active(GTK_SWITCH(sw)) ? "yes" : "no");
}

static void on_metered(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	if (sel < 3) {
		modify_one(p, "connection.metered", metered_values[sel]);
	}
}

static void on_mac(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct network_page *p = data;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	if (sel < 3 && p->mac_key && !p->updating && p->uuid) {
		// the device takes its new address when it connects again
		const char *argv[] = { "connection", "modify", "uuid", p->uuid, p->mac_key,
			mac_values[sel], NULL };
		run(p, argv, changed_done, "Saved; used the next time it connects");
	}
}

static void on_limit_switch(GObject *sw, GParamSpec *pspec, gpointer data) {
	limits_sync(data);
}

static void limits_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct network_page *p = data;
	char *out = NULL, *err = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		NULL) && g_subprocess_get_successful(G_SUBPROCESS(source));
	gtk_widget_set_sensitive(p->limits_apply, true);
	if (ok) {
		set_message(p, "The limits are saved, and in force while it is connected", false);
		settings_status(p->s, "Network limits saved");
	} else {
		int status = g_subprocess_get_if_exited(G_SUBPROCESS(source)) ?
			g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
		// 126/127: pkexec was refused or found no agent to ask
		char *text = g_strdup_printf("Could not save the limits: %s",
			err && *err ? g_strstrip(err) :
			status == 126 || status == 127 ? "not authorized" : "the helper failed");
		set_message(p, text, true);
		g_free(text);
	}
	g_free(out);
	g_free(err);
}

static void on_limits_apply(GtkButton *button, gpointer data) {
	struct network_page *p = data;
	if (!p->uuid) {
		return;
	}
	char values[3][24];
	for (int k = 0; k < 3; k++) {
		double v = gtk_switch_get_active(GTK_SWITCH(p->limit_switch[k])) ?
			gtk_spin_button_get_value(GTK_SPIN_BUTTON(p->limit_spin[k])) : 0;
		// gigabytes to megabytes, Mbit/s to kbit/s
		g_snprintf(values[k], sizeof(values[k]), "%.0f", v * 1000);
	}
	char *helper = limits_helper();
	const char *test = g_getenv("TILEWIN_NETWORK_HELPER");
	const char *argv[8];
	int n = 0;
	if (!test || !*test) {
		argv[n++] = "pkexec";
	}
	argv[n++] = helper;
	argv[n++] = "set";
	argv[n++] = p->uuid;
	for (int k = 0; k < 3; k++) {
		argv[n++] = values[k];
	}
	argv[n] = NULL;
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_free(helper);
	if (!proc) {
		char *text = g_strdup_printf("Could not save the limits: %s", error->message);
		set_message(p, text, true);
		g_free(text);
		g_error_free(error);
		return;
	}
	gtk_widget_set_sensitive(p->limits_apply, false);
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, limits_done, p);
	g_object_unref(proc);
}

/* A row with a switch and an amount beside it. */
static void limit_row(struct network_page *p, GtkWidget *group, int k, const char *title,
		const char *subtitle, double min, double max, double step, double value,
		const char *unit) {
	p->limit_switch[k] = gtk_switch_new();
	g_signal_connect(p->limit_switch[k], "notify::active", G_CALLBACK(on_limit_switch), p);
	p->limit_spin[k] = gtk_spin_button_new_with_range(min, max, step);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(p->limit_spin[k]), 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->limit_spin[k]), value);
	GtkWidget *row = ui_row(group, title, subtitle, p->limit_spin[k]);
	GtkWidget *label = gtk_label_new(unit);
	gtk_widget_set_size_request(label, 52, -1);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(ui_row_box(row)), label);
	gtk_widget_set_valign(p->limit_switch[k], GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(ui_row_box(row)), p->limit_switch[k]);
}

static void add_advanced(struct network_page *p) {
	GtkWidget *adv = ui_group(p->details, "Advanced", NULL);
	p->auto_switch = gtk_switch_new();
	g_signal_connect(p->auto_switch, "notify::active", G_CALLBACK(on_autoconnect), p);
	ui_row(adv, "Connect automatically", "When it is in reach or plugged in", p->auto_switch);
	p->metered_dd = gtk_drop_down_new_from_strings(metered_labels);
	g_signal_connect(p->metered_dd, "notify::selected", G_CALLBACK(on_metered), p);
	ui_row(adv, "Metered connection", "Apps hold back big downloads, like updates, on a "
		"network that counts what goes over it", p->metered_dd);
	p->mac_dd = gtk_drop_down_new_from_strings(mac_labels);
	g_signal_connect(p->mac_dd, "notify::selected", G_CALLBACK(on_mac), p);
	p->mac_row = ui_row(adv, "Hardware address", "What the network sees of this computer: "
		"a new one makes it a new device to the network", p->mac_dd);

	limit_row(p, adv, LIMIT_RECONNECT, "Connect again after",
		"Each time this much went in and out since it connected, for hotspots that give a "
		"set amount per connection", 0.1, 100000, 0.5, 1, "GB");
	limit_row(p, adv, LIMIT_DOWN, "Limit downloads to", "Leaves the rest to other devices "
		"of the network", 0.1, 100000, 1, 10, "Mbit/s");
	limit_row(p, adv, LIMIT_UP, "Limit uploads to", "So a backup or an upload does not fill "
		"the line", 0.1, 100000, 1, 5, "Mbit/s");
	p->limits_apply = gtk_button_new_with_label("Apply");
	gtk_widget_add_css_class(p->limits_apply, "suggested-action");
	g_signal_connect(p->limits_apply, "clicked", G_CALLBACK(on_limits_apply), p);
	char *helper = limits_helper();
	bool installed = g_file_test(helper, G_FILE_TEST_EXISTS);
	g_free(helper);
	GtkWidget *row = ui_row(adv, NULL, installed ? "The amount and the limits take an "
		"administrator's yes, and come into force at once on a connected network" :
		"The amount and the limits need tileWin installed with install.sh",
		p->limits_apply);
	p->limits_sub = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
	gtk_widget_set_sensitive(p->limits_apply, installed);
	limits_sync(p);
}

/* ---------- the page ---------- */

static const char *const method_labels[] = { "Automatically (DHCP)", "Fixed address", NULL };
static const char *const dns_labels[] = { "Those for all networks",
	"Servers of its own", NULL };

static GtkWidget *prefix_dropdown(void) {
	// /32 down to /8, by their masks: what routers show
	GtkStringList *items = gtk_string_list_new(NULL);
	for (int prefix = 32; prefix >= 8; prefix--) {
		uint32_t mask = prefix ? 0xffffffffu << (32 - prefix) : 0;
		char *label = g_strdup_printf("%u.%u.%u.%u  (/%d)%s", mask >> 24, mask >> 16 & 0xff,
			mask >> 8 & 0xff, mask & 0xff, prefix, prefix == 24 ? ", most home networks" : "");
		gtk_string_list_append(items, label);
		g_free(label);
	}
	GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(items), NULL);
	return dd;
}

static gboolean on_mapped_refresh(gpointer data) {
	load_conns(data);
	return G_SOURCE_REMOVE;
}

static void on_map(GtkWidget *widget, gpointer data) {
	g_idle_add(on_mapped_refresh, data); // the networks may have changed meanwhile
}

static struct network_page *section_new(struct settings *s) {
	struct network_page *p = g_new0(struct network_page, 1);
	p->s = s;
	p->conns = g_ptr_array_new_with_free_func(conn_free);
	p->dns = g_ptr_array_new_with_free_func(g_free);
	p->dns_rows = g_ptr_array_new();
	p->dns_picks = g_ptr_array_new_with_free_func(g_free);
	return p;
}

/* The network to show, the message under it and the box of its settings. */
static void add_picker(struct network_page *p, GtkWidget *content, const char *title,
		const char *description) {
	GtkWidget *which = ui_group(content, title, description);
	p->conn_dd = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->conn_dd, "notify::selected", G_CALLBACK(on_conn_picked), p);
	GtkWidget *row = ui_row(which, "Network", " ", p->conn_dd);
	p->conn_sub = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
	p->message = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->message), 0);
	gtk_label_set_wrap(GTK_LABEL(p->message), TRUE);
	gtk_widget_set_margin_start(p->message, 12);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(which)), p->message);
	gtk_widget_set_visible(p->message, false);
	p->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_append(GTK_BOX(content), p->details);
	gtk_widget_set_sensitive(p->details, false);
	g_signal_connect(which, "map", G_CALLBACK(on_map), p);
}

void network_dns_section_attach(struct settings *s, GtkWidget *content) {
	struct network_page *p = section_new(s);
	add_picker(p, content, "Servers of single networks",
		"A network can ask servers of its own instead of those for all networks, e.g. the "
		"router at home for the names of the devices there.");
	GtkWidget *dns = ui_group(p->details, NULL, NULL);
	p->dns_dd = gtk_drop_down_new_from_strings(dns_labels);
	g_signal_connect(p->dns_dd, "notify::selected", G_CALLBACK(on_dns_mode), p);
	GtkWidget *row = ui_row(dns, "This network asks", " ", p->dns_dd);
	p->dns_now = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
	p->dns_note = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->dns_note), 0);
	gtk_label_set_wrap(GTK_LABEL(p->dns_note), TRUE);
	gtk_widget_add_css_class(p->dns_note, "dim-label");
	gtk_widget_add_css_class(p->dns_note, "tw-caption");
	gtk_widget_set_margin_start(p->dns_note, 12);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(dns)), p->dns_note);
	p->dns_list = ui_group(p->details, NULL, NULL);
	p->dns_empty = ui_row(p->dns_list, "No servers yet: add one below", NULL, NULL);
	p->dns_pick = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->dns_pick, "notify::selected", G_CALLBACK(on_dns_pick), p);
	ui_row(p->dns_list, NULL, NULL, p->dns_pick);
	p->dns_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->dns_entry), "An address, e.g. 192.168.1.1");
	gtk_widget_set_hexpand(p->dns_entry, TRUE);
	g_signal_connect(p->dns_entry, "activate", G_CALLBACK(on_dns_typed), p);
	GtkWidget *add = gtk_button_new_with_label("Add");
	g_signal_connect(add, "clicked", G_CALLBACK(on_dns_typed), p);
	p->dns_entry_row = ui_row(p->dns_list, NULL, NULL, NULL);
	gtk_box_append(GTK_BOX(ui_row_box(p->dns_entry_row)), p->dns_entry);
	gtk_box_append(GTK_BOX(ui_row_box(p->dns_entry_row)), add);
	gtk_widget_set_visible(p->dns_entry_row, false);
	sync_rows(p);
	dns_sync(p);
	load_conns(p);
}

GtkWidget *network_page_new(struct settings *s) {
	struct network_page *p = section_new(s);
	GtkWidget *content;
	GtkWidget *page = ui_page("Network",
		"The address and the password of each saved network; its DNS servers are set on the "
		"DNS page. New Wi-Fi networks are joined from the network menu of the taskbar.",
		&content);
	p->now_list = ui_group(content, "Connected now", "Every network in use, over the air and "
		"by cable, and how each gets its address. Edit shows one below.");
	add_picker(p, content, NULL, NULL);

	GtkWidget *ip = ui_group(p->details, "IP address", NULL);
	p->method_dd = gtk_drop_down_new_from_strings(method_labels);
	g_signal_connect(p->method_dd, "notify::selected", G_CALLBACK(on_method), p);
	GtkWidget *row = ui_row(ip, "Get the address", " ", p->method_dd);
	p->now_sub = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
	p->address = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->address), "192.168.1.50");
	p->fixed_rows[0] = ui_row(ip, "Address", "One that no other device of the network has, "
		"outside the range the router hands out", p->address);
	p->prefix_dd = prefix_dropdown();
	p->fixed_rows[1] = ui_row(ip, "Subnet mask", NULL, p->prefix_dd);
	p->gateway = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->gateway), "192.168.1.1");
	p->fixed_rows[2] = ui_row(ip, "Gateway", "The router", p->gateway);
	GtkWidget *apply = gtk_button_new_with_label("Apply");
	gtk_widget_add_css_class(apply, "suggested-action");
	g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_address), p);
	p->apply_row = ui_row(ip, NULL, "A connected network connects again with the new address",
		apply);

	p->wifi_group = ui_group(p->details, "Wi-Fi password", NULL);
	p->password = gtk_password_entry_new();
	gtk_widget_set_size_request(p->password, 220, -1);
	p->show_button = gtk_button_new_with_label("Show");
	g_signal_connect(p->show_button, "clicked", G_CALLBACK(on_show_password), p);
	p->save_password = gtk_button_new_with_label("Save");
	g_signal_connect(p->save_password, "clicked", G_CALLBACK(on_save_password), p);
	g_signal_connect(p->password, "activate", G_CALLBACK(on_save_password), p);
	row = ui_row(p->wifi_group, "Password", " ", p->password);
	p->password_sub = gtk_widget_get_last_child(gtk_widget_get_first_child(ui_row_box(row)));
	gtk_box_append(GTK_BOX(ui_row_box(row)), p->show_button);
	gtk_box_append(GTK_BOX(ui_row_box(row)), p->save_password);

	add_advanced(p);
	sync_rows(p);
	s->network_page = p;
	load_conns(p);
	return page;
}
