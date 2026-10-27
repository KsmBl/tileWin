/*
 * The devices of the computer for the Device manager page: what sysfs has,
 * named with the database of udev (the names of the PCI and USB ID lists),
 * sorted into the categories of the Device Manager of Windows, each with its
 * driver and whether it works.
 */
#define _GNU_SOURCE
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "devices.h"

static const struct tw_dev_category categories[] = {
	{ "batteries", "Batteries", "battery-symbolic" },
	{ "bluetooth", "Bluetooth", "bluetooth-symbolic" },
	{ "cameras", "Cameras", "camera-web-symbolic" },
	{ "computer", "Computer", "computer-symbolic" },
	{ "disks", "Disk drives", "drive-harddisk-symbolic" },
	{ "display", "Display adapters", "video-display-symbolic" },
	{ "optical", "DVD/CD-ROM drives", "drive-optical-symbolic" },
	{ "firmware", "Firmware", "application-x-firmware-symbolic" },
	{ "hid", "Human Interface Devices", "input-gaming-symbolic" },
	{ "keyboards", "Keyboards", "input-keyboard-symbolic" },
	{ "mtd", "Memory technology devices", "media-flash-symbolic" },
	{ "mice", "Mice and other pointing devices", "input-mouse-symbolic" },
	{ "monitors", "Monitors", "video-display-symbolic" },
	{ "network", "Network adapters", "network-wired-symbolic" },
	{ "other", "Other devices", "dialog-warning-symbolic" },
	{ "ports", "Ports (COM & LPT)", "printer-symbolic" },
	{ "processors", "Processors", "system-run-symbolic" },
	{ "security", "Security devices", "security-high-symbolic" },
	{ "software", "Software devices", "application-x-addon-symbolic" },
	{ "sound", "Sound, video and game controllers", "audio-card-symbolic" },
	{ "storage", "Storage controllers", "drive-harddisk-system-symbolic" },
	{ "system", "System devices", "emblem-system-symbolic" },
	{ "usbctl", "Universal Serial Bus controllers", "media-removable-symbolic" },
	{ "usb", "Universal Serial Bus devices", "drive-removable-media-symbolic" },
};

const struct tw_dev_category *tw_dev_categories(size_t *n) {
	*n = G_N_ELEMENTS(categories);
	return categories;
}

const struct tw_dev_category *tw_dev_category(const char *key) {
	for (size_t i = 0; i < G_N_ELEMENTS(categories); i++) {
		if (strcmp(categories[i].key, key) == 0) {
			return &categories[i];
		}
	}
	return &categories[0];
}

/* ---------- files ---------- */

static const char *root(void) {
	const char *r = g_getenv("TILEWIN_DEVICES_ROOT");
	return r ? r : "";
}

static char *at_root(const char *path) {
	return g_strconcat(root(), path, NULL);
}

/* A file below the root, its first line without the newline; NULL if none. */
static char *read_line(const char *path) {
	char *full = at_root(path), *text = NULL;
	if (g_file_get_contents(full, &text, NULL, NULL)) {
		text[strcspn(text, "\n")] = '\0';
		g_strstrip(text);
		if (!*text) {
			g_clear_pointer(&text, g_free);
		}
	}
	g_free(full);
	return text;
}

static char *sys_attr(const char *dev, const char *attr) {
	char *path = g_strdup_printf("/sys/%s/%s", dev, attr);
	char *v = read_line(path);
	g_free(path);
	return v;
}

static bool sys_has(const char *dev, const char *attr) {
	char *path = g_strdup_printf("%s/sys/%s/%s", root(), dev, attr);
	bool has = g_file_test(path, G_FILE_TEST_EXISTS);
	g_free(path);
	return has;
}

/* The device a class or bus link points to, below /sys: devices/... */
static char *sys_resolve(const char *link) {
	char *full = g_strdup_printf("%s/sys/%s", root(), link);
	char *real = realpath(full, NULL);
	g_free(full);
	if (!real) {
		return NULL;
	}
	char *sys = g_strdup_printf("%s/sys/", root());
	char *rel = NULL;
	if (g_str_has_prefix(real, sys)) {
		rel = g_strdup(real + strlen(sys));
	}
	g_free(sys);
	free(real);
	return rel;
}

/* The last part of where a link of the device points: its driver, module, subsystem. */
static char *sys_link_name(const char *dev, const char *link) {
	char *full = g_strdup_printf("%s/sys/%s/%s", root(), dev, link);
	char *target = g_file_read_link(full, NULL);
	g_free(full);
	if (!target) {
		return NULL;
	}
	char *name = g_path_get_basename(target);
	g_free(target);
	return name;
}

/* The device above: the nearest folder up that is one (net/, sound/ and the
 * like between them are not). */
static char *parent_of(const char *dev) {
	char *p = g_path_get_dirname(dev);
	while (strcmp(p, "devices") != 0 && strcmp(p, ".") != 0 && g_str_has_prefix(p, "devices")) {
		char *uevent = g_strdup_printf("%s/sys/%s/uevent", root(), p);
		bool device = g_file_test(uevent, G_FILE_TEST_EXISTS);
		g_free(uevent);
		if (device) {
			return p;
		}
		char *up = g_path_get_dirname(p);
		g_free(p);
		p = up;
	}
	g_free(p);
	return NULL;
}

/* The names in a folder below the root, sorted. */
static GPtrArray *list_dir(const char *path) {
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	char *full = at_root(path);
	GDir *dir = g_dir_open(full, 0, NULL);
	g_free(full);
	if (dir) {
		const char *n;
		while ((n = g_dir_read_name(dir))) {
			g_ptr_array_add(names, g_strdup(n));
		}
		g_dir_close(dir);
	}
	g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
	return names;
}

/* The names in a device's folder in sysfs. */
static GPtrArray *list_sys(const char *dev) {
	char *path = g_strdup_printf("/sys/%s", dev);
	GPtrArray *names = list_dir(path);
	g_free(path);
	return names;
}

/* ---------- what udev knows ---------- */

/* KEY=value of udev's database for a device, E: lines only. */
static GHashTable *udev_props(const char *key) {
	GHashTable *props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (!key) {
		return props;
	}
	char *path = g_strdup_printf("%s/run/udev/data/%s", root(), key);
	char *text = NULL;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char *eq;
			if (g_str_has_prefix(lines[i], "E:") && (eq = strchr(lines[i], '='))) {
				g_hash_table_insert(props, g_strndup(lines[i] + 2, eq - lines[i] - 2),
					g_strdup(eq + 1));
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);
	return props;
}

/* What udev calls a device: b8:0 and c189:1 for those with a node, n3 for a
 * network interface, +pci:0000:00:14.3 for the others. */
static char *device_key(const char *dev) {
	char *subsystem = sys_link_name(dev, "subsystem");
	char *sysname = g_path_get_basename(dev);
	char *key = NULL, *devnum = sys_attr(dev, "dev");
	if (subsystem && strcmp(subsystem, "net") == 0) {
		char *index = sys_attr(dev, "ifindex");
		key = index ? g_strdup_printf("n%s", index) : NULL;
		g_free(index);
	} else if (devnum) {
		key = g_strdup_printf("%c%s", subsystem && strcmp(subsystem, "block") == 0 ? 'b' : 'c',
			devnum);
	}
	if (!key) {
		key = g_strdup_printf("+%s:%s", subsystem ? subsystem : "", sysname);
	}
	g_free(subsystem);
	g_free(sysname);
	g_free(devnum);
	return key;
}

/* ---------- the kernel's messages ---------- */

struct klog {
	GHashTable *by_key; // char *key -> GPtrArray of char *message
	GHashTable *errors; // char *key -> GPtrArray of char *message (priority 3 or more urgent)
};

static void klog_add(GHashTable *table, const char *key, const char *message) {
	GPtrArray *list = g_hash_table_lookup(table, key);
	if (!list) {
		list = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(table, g_strdup(key), list);
	}
	g_ptr_array_add(list, g_strdup(message));
}

static void klog_read(struct klog *k) {
	k->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	k->errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	char *text = NULL;
	const char *file = g_getenv("TILEWIN_DEVICES_KLOG");
	if (file) {
		g_file_get_contents(file, &text, NULL, NULL);
	} else {
		const char *argv[] = { "journalctl", "-k", "-b", "-o", "json", "--no-pager",
			"--output-fields=_KERNEL_DEVICE,MESSAGE,PRIORITY,__REALTIME_TIMESTAMP", NULL };
		g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			NULL, NULL, &text, NULL, NULL, NULL);
	}
	if (!text) {
		return;
	}
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		json_object *o = *lines[i] ? json_tokener_parse(lines[i]) : NULL;
		json_object *dev, *msg, *prio, *when;
		if (o && json_object_object_get_ex(o, "_KERNEL_DEVICE", &dev) &&
				json_object_object_get_ex(o, "MESSAGE", &msg) &&
				json_object_is_type(msg, json_type_string)) {
			const char *key = json_object_get_string(dev);
			char *line = g_strdup(json_object_get_string(msg));
			if (json_object_object_get_ex(o, "__REALTIME_TIMESTAMP", &when)) {
				GDateTime *t = g_date_time_new_from_unix_local(
					g_ascii_strtoll(json_object_get_string(when), NULL, 10) / 1000000);
				if (t) {
					char *stamp = g_date_time_format(t, "%H:%M:%S");
					char *with = g_strdup_printf("%s  %s", stamp, line);
					g_free(line);
					line = with;
					g_free(stamp);
					g_date_time_unref(t);
				}
			}
			klog_add(k->by_key, key, line);
			if (json_object_object_get_ex(o, "PRIORITY", &prio) &&
					atoi(json_object_get_string(prio)) <= 3) {
				klog_add(k->errors, key, json_object_get_string(msg));
			}
			g_free(line);
		}
		if (o) {
			json_object_put(o);
		}
	}
	g_strfreev(lines);
	g_free(text);
}

static void klog_free(struct klog *k) {
	g_clear_pointer(&k->by_key, g_hash_table_unref);
	g_clear_pointer(&k->errors, g_hash_table_unref);
}

/* ---------- tileWin's udev rules for disabled devices ---------- */

#define DISABLED_RULES "/etc/udev/rules.d/90-tilewin-disabled.rules"

bool tw_device_disabled_by_rule(const char *action) {
	if (!action) {
		return false;
	}
	char *path = at_root(DISABLED_RULES), *text = NULL;
	bool found = false;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char *sysname = g_path_get_basename(action);
		char **lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i] && !found; i++) {
			// # tilewin-disabled BUS SYSNAME DRIVER
			char **f = g_strsplit(lines[i], " ", 5);
			found = g_strv_length(f) >= 4 && strcmp(f[0], "#") == 0 &&
				strcmp(f[1], "tilewin-disabled") == 0 && strcmp(f[3], sysname) == 0;
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(sysname);
		g_free(text);
	}
	g_free(path);
	return found;
}

/* ---------- a device ---------- */

void tw_device_free(struct tw_device *d) {
	g_free(d->path);
	g_free(d->key);
	g_free(d->name);
	g_free(d->icon);
	g_free(d->manufacturer);
	g_free(d->location);
	g_free(d->driver);
	g_free(d->module);
	g_free(d->status);
	g_free(d->action);
	g_free(d->action_bus);
	g_ptr_array_unref(d->props);
	g_ptr_array_unref(d->resources);
	g_ptr_array_unref(d->firmware);
	g_ptr_array_unref(d->events);
	g_free(d);
}

static struct tw_device *device_new(GPtrArray *all, const char *path, const char *category) {
	struct tw_device *d = g_new0(struct tw_device, 1);
	d->path = g_strdup(path);
	d->key = path ? device_key(path) : NULL;
	d->category = tw_dev_category(category)->key;
	d->props = g_ptr_array_new_with_free_func(g_free);
	d->resources = g_ptr_array_new_with_free_func(g_free);
	d->firmware = g_ptr_array_new_with_free_func(g_free);
	d->events = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(all, d);
	return d;
}

static void prop(struct tw_device *d, const char *name, const char *value) {
	if (value && *value) {
		g_ptr_array_add(d->props, g_strdup_printf("%s\t%s", name, value));
	}
}

/* The driver of the device, or of the nearest device above it that has one. */
static char *driver_above(const char *dev, char **owner) {
	char *p = g_strdup(dev);
	while (p) {
		char *drv = sys_link_name(p, "driver");
		if (drv) {
			if (owner) {
				*owner = p;
			} else {
				g_free(p);
			}
			return drv;
		}
		char *up = parent_of(p);
		g_free(p);
		p = up;
	}
	if (owner) {
		*owner = NULL;
	}
	return NULL;
}

/* The kernel module behind a driver: its link, or NULL when it is built in. */
static char *module_of(const char *dev) {
	char *path = g_strdup_printf("%s/driver", dev);
	char *m = sys_link_name(path, "module");
	g_free(path);
	return m;
}

/* What the journal and udev say of a device: messages, firmware, errors. */
static void add_log(struct tw_device *d, struct klog *k, const char *key) {
	GPtrArray *lines = key && k->by_key ? g_hash_table_lookup(k->by_key, key) : NULL;
	for (guint i = 0; lines && i < lines->len; i++) {
		const char *m = lines->pdata[i];
		g_ptr_array_add(d->events, g_strdup(m));
		// i915 0000:00:02.0: [drm] GT0: GuC firmware i915/tgl_guc_70.bin version 70.49.4
		// iwlwifi 0000:00:14.3: loaded firmware version 89.d2579d43.0 so-a0-gf-a0-89.ucode
		if (!strcasestr(m, "firmware") || strstr(m, "Direct firmware load") || strstr(m, "failed")) {
			continue;
		}
		static const char *const kinds[] = { ".bin", ".ucode", ".fw", ".pnvm", ".sbin",
			".img", ".dat", ".zst", ".xz", NULL };
		char **words = g_strsplit_set(m, " \t,()", -1);
		for (int w = 0; words[w]; w++) {
			bool file = false;
			for (int x = 0; kinds[x] && !file; x++) {
				file = g_str_has_suffix(words[w], kinds[x]) && strlen(words[w]) > strlen(kinds[x]);
			}
			bool have = false;
			for (guint j = 0; file && j < d->firmware->len; j++) {
				have = have || strcmp(d->firmware->pdata[j], words[w]) == 0;
			}
			if (file && !have) {
				g_ptr_array_add(d->firmware, g_strdup(words[w]));
			}
		}
		g_strfreev(words);
	}
}

/* A sentence for the General tab, and whether something is wrong. */
static void judge(struct tw_device *d, struct klog *k, bool needs_driver) {
	if (d->status) {
		return;
	}
	GPtrArray *lines = d->key && k->by_key ? g_hash_table_lookup(k->by_key, d->key) : NULL;
	for (guint i = 0; lines && i < lines->len && !d->status; i++) {
		const char *m = lines->pdata[i], *at;
		if ((at = strstr(m, "Direct firmware load for "))) {
			char **words = g_strsplit(at + strlen("Direct firmware load for "), " ", 2);
			d->status = g_strdup_printf("The driver could not load the firmware %s, so the "
				"device may not work. It comes with the package that has it, linux-firmware "
				"or one of its parts most often.", words[0]);
			g_strfreev(words);
			d->problem = TW_DEV_WARNING;
		} else if ((at = strstr(m, "probe with driver ")) && strstr(at, "failed")) {
			d->status = g_strdup_printf("This device cannot start: %s", strstr(m, "  ") ?
				strstr(m, "  ") + 2 : m);
			d->problem = TW_DEV_WARNING;
		}
	}
	if (d->status) {
		return;
	}
	if (!d->driver && needs_driver) {
		d->status = g_strdup("The driver for this device is not installed: Linux has none in "
			"use for it. It may need a driver package (or a newer kernel), or Linux does not "
			"support it.");
		d->problem = TW_DEV_WARNING;
		return;
	}
	if (!d->driver) {
		d->status = g_strdup("This device is working properly. No driver is in use for it; "
			"devices of its kind need none.");
		return;
	}
	GPtrArray *errors = d->key && k->errors ? g_hash_table_lookup(k->errors, d->key) : NULL;
	d->status = g_strdup(errors && errors->len ? "This device is working, but its driver "
		"reported errors (see Events)." : "This device is working properly.");
}

/* The driver bound to the device itself, with its module. */
static void take_driver(struct tw_device *d, const char *dev) {
	d->driver = sys_link_name(dev, "driver");
	if (d->driver) {
		d->module = module_of(dev);
	}
}

/* The driver of the nearest device above with one: a keyboard's is atkbd on serio0. */
static void take_driver_above(struct tw_device *d, const char *dev) {
	char *owner = NULL;
	d->driver = driver_above(dev, &owner);
	if (owner) {
		d->module = module_of(owner);
		g_free(owner);
	}
}

/* Disable, Enable and Restart act on this device, on this bus. */
static void set_action(struct tw_device *d, const char *dev) {
	char *bus = sys_link_name(dev, "subsystem");
	if (bus && (strcmp(bus, "pci") == 0 || strcmp(bus, "usb") == 0 || strcmp(bus, "i2c") == 0 ||
			strcmp(bus, "serio") == 0 || strcmp(bus, "hdaudio") == 0)) {
		d->action = g_strdup(dev);
		d->action_bus = bus;
	} else {
		g_free(bus);
	}
}

/* The nearest device up (or the device) with a driver: Disable acts on it when
 * it is on a bus that can be, never on one further up (a keyboard does not take
 * its controller along). A HID device goes with what carries it (its USB device,
 * its I2C client), a USB function with its USB device. */
static void set_action_above(struct tw_device *d, const char *dev) {
	char *p = NULL;
	char *drv = driver_above(dev, &p);
	g_free(drv);
	if (!p) {
		return;
	}
	char *bus = sys_link_name(p, "subsystem");
	if (bus && strcmp(bus, "hid") == 0) {
		char *up = parent_of(p);
		g_free(p);
		p = NULL;
		drv = up ? driver_above(up, &p) : NULL;
		g_free(drv);
		g_free(up);
		g_free(bus);
		bus = p ? sys_link_name(p, "subsystem") : NULL;
	}
	if (p && bus && strcmp(bus, "usb") == 0 && !sys_has(p, "idVendor")) {
		char *up = parent_of(p);
		g_free(p);
		p = up;
	}
	if (p) {
		set_action(d, p);
	}
	g_free(bus);
	g_free(p);
}

static void disabled_check(struct tw_device *d) {
	if (!d->action) {
		return;
	}
	bool unauthorized = false;
	if (d->action_bus && strcmp(d->action_bus, "usb") == 0) {
		char *auth = sys_attr(d->action, "authorized");
		unauthorized = auth && strcmp(auth, "0") == 0;
		g_free(auth);
	}
	bool rule = tw_device_disabled_by_rule(d->action);
	if (unauthorized || rule) {
		g_free(d->status);
		d->status = g_strdup(rule ? "This device is disabled. It stays disabled after a restart, "
			"until it is enabled again." : "This device is disabled.");
		d->problem = TW_DEV_DISABLED;
	}
}

static void common_props(struct tw_device *d, GHashTable *udev) {
	prop(d, "Device instance path", d->path ? d->path + strlen("devices/") : NULL);
	char *modalias = d->path ? sys_attr(d->path, "modalias") : NULL;
	prop(d, "Hardware ID (modalias)", modalias);
	g_free(modalias);
	prop(d, "udev name", d->key);
	GList *keys = g_hash_table_get_keys(udev);
	keys = g_list_sort(keys, (GCompareFunc)strcmp);
	for (GList *l = keys; l; l = l->next) {
		prop(d, l->data, g_hash_table_lookup(udev, l->data));
	}
	g_list_free(keys);
}

static void irq_resource(struct tw_device *d, const char *dev) {
	char *irq = sys_attr(dev, "irq");
	if (irq && strcmp(irq, "0") != 0) {
		g_ptr_array_add(d->resources, g_strdup_printf("IRQ\t%s", irq));
	}
	g_free(irq);
}

/* ---------- PCI ---------- */

static const char *pci_category(unsigned cls, const char *driver) {
	unsigned base = cls >> 16, sub = (cls >> 8) & 0xff;
	switch (base) {
	case 0x01: return "storage";
	case 0x02: return "network";
	case 0x03: return "display";
	case 0x04: return "sound";
	case 0x07: return "system"; // serial controllers of today are the sensor hub, the ME
	case 0x09: return "hid";
	case 0x0c: return sub == 0x03 ? "usbctl" : "system";
	case 0x0d: return sub == 0x11 ? "bluetooth" : "network";
	case 0x10: return "security";
	case 0xff: return driver && g_str_has_prefix(driver, "rtsx") ? "mtd" : "system";
	default: return "system";
	}
}

/* Kinds that do nothing without a driver: no driver for one of them is a problem. */
static bool pci_needs_driver(unsigned cls) {
	unsigned base = cls >> 16, sub = (cls >> 8) & 0xff;
	return base == 0x01 || base == 0x02 || base == 0x03 || base == 0x04 || base == 0x09 ||
		base == 0x0d || base == 0x10 || base == 0x12 || (base == 0x0c && (sub == 0x03 || sub == 0x00));
}

static void scan_pci(GPtrArray *all, struct klog *k) {
	GPtrArray *names = list_dir("/sys/bus/pci/devices");
	for (guint i = 0; i < names->len; i++) {
		char *link = g_strdup_printf("bus/pci/devices/%s", (char *)names->pdata[i]);
		char *dev = sys_resolve(link);
		g_free(link);
		if (!dev) {
			continue;
		}
		char *cls_text = sys_attr(dev, "class");
		unsigned cls = cls_text ? (unsigned)strtoul(cls_text, NULL, 16) : 0;
		char *driver = sys_link_name(dev, "driver");
		struct tw_device *d = device_new(all, dev, pci_category(cls, driver));
		g_free(driver);
		GHashTable *udev = udev_props(d->key);
		const char *model = g_hash_table_lookup(udev, "ID_MODEL_FROM_DATABASE");
		const char *kind = g_hash_table_lookup(udev, "ID_PCI_SUBCLASS_FROM_DATABASE");
		char *vendor = sys_attr(dev, "vendor"), *device = sys_attr(dev, "device");
		d->name = g_strdup(model ? model : kind ? kind : "PCI device");
		d->manufacturer = g_strdup(g_hash_table_lookup(udev, "ID_VENDOR_FROM_DATABASE"));
		unsigned bus = 0, slot = 0, fn = 0;
		const char *bdf = names->pdata[i];
		if (sscanf(bdf, "%*x:%x:%x.%x", &bus, &slot, &fn) == 3) {
			d->location = g_strdup_printf("PCI bus %u, device %u, function %u", bus, slot, fn);
		}
		take_driver(d, dev);
		set_action(d, dev);
		prop(d, "PCI class", kind);
		char *vd = vendor && device ? g_strdup_printf("%s:%s", vendor + 2, device + 2) : NULL;
		prop(d, "Vendor and device ID", vd);
		g_free(vd);
		// what it is for the rest of the system: its network interface, sound card, ...
		const char *classes[] = { "net", "sound", "drm", NULL };
		for (int c = 0; classes[c]; c++) {
			char *sub = g_strdup_printf("%s/%s", dev, classes[c]);
			GPtrArray *subs = list_sys(sub);
			for (guint j = 0; j < subs->len; j++) {
				prop(d, strcmp(classes[c], "net") == 0 ? "Network interface" :
					strcmp(classes[c], "sound") == 0 ? "Sound device" : "Graphics device",
					subs->pdata[j]);
			}
			g_ptr_array_unref(subs);
			g_free(sub);
		}
		// reading the link wakes a device that sleeps (a GPU that is off): only awake ones
		char *runtime = sys_attr(dev, "power/runtime_status");
		bool awake = !runtime || strcmp(runtime, "active") == 0;
		g_free(runtime);
		char *link_speed = awake ? sys_attr(dev, "current_link_speed") : NULL;
		char *width = awake ? sys_attr(dev, "current_link_width") : NULL;
		if (link_speed && width && strcmp(width, "0") != 0) {
			char *v = g_strdup_printf("%s, x%s", link_speed, width);
			prop(d, "PCI Express link", v);
			g_free(v);
		}
		g_free(link_speed);
		g_free(width);
		common_props(d, udev);
		irq_resource(d, dev);
		char *res = NULL, *res_path = g_strdup_printf("%s/sys/%s/resource", root(), dev);
		if (g_file_get_contents(res_path, &res, NULL, NULL)) {
			char **lines = g_strsplit(res, "\n", -1);
			for (int r = 0; lines[r]; r++) {
				unsigned long long start, end, flags;
				if (sscanf(lines[r], "%llx %llx %llx", &start, &end, &flags) == 3 && end) {
					g_ptr_array_add(d->resources, g_strdup_printf("%s\t0x%llX-0x%llX",
						flags & 0x100 ? "I/O range" : "Memory range", start, end));
				}
			}
			g_strfreev(lines);
			g_free(res);
		}
		g_free(res_path);
		add_log(d, k, d->key);
		judge(d, k, pci_needs_driver(cls));
		// one without the driver it needs is an unknown device, as on Windows
		if (!d->driver && pci_needs_driver(cls) && d->problem == TW_DEV_WARNING) {
			d->category = "other";
		}
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(vendor);
		g_free(device);
		g_free(cls_text);
		g_free(dev);
	}
	g_ptr_array_unref(names);
}

/* ---------- USB ---------- */

static void scan_usb(GPtrArray *all, struct klog *k) {
	GPtrArray *names = list_dir("/sys/bus/usb/devices");
	for (guint i = 0; i < names->len; i++) {
		const char *sysname = names->pdata[i];
		char *link = g_strdup_printf("bus/usb/devices/%s", sysname);
		char *dev = sys_resolve(link);
		g_free(link);
		if (!dev || !sys_has(dev, "idVendor")) {
			g_free(dev);
			continue;
		}
		bool root_hub = g_str_has_prefix(sysname, "usb");
		char *cls = sys_attr(dev, "bDeviceClass");
		bool hub = root_hub || (cls && strcmp(cls, "09") == 0);
		g_free(cls);
		// its functions: which of them have no driver
		GPtrArray *drivers = g_ptr_array_new_with_free_func(g_free);
		int missing = 0, functions = 0;
		GPtrArray *subs = list_sys(dev);
		// the functions: 1-5:1.0 of 1-5, 1-0:1.0 of the root hub usb1
		char *prefix = root_hub ? g_strdup_printf("%s-0:", sysname + 3) :
			g_strdup_printf("%s:", sysname);
		for (guint j = 0; j < subs->len; j++) {
			const char *s = subs->pdata[j];
			if (!g_str_has_prefix(s, prefix)) {
				continue;
			}
			char *iface = g_strdup_printf("%s/%s", dev, s);
			char *drv = sys_link_name(iface, "driver");
			functions++;
			if (drv) {
				bool have = false;
				for (guint m = 0; m < drivers->len; m++) {
					have = have || strcmp(drivers->pdata[m], drv) == 0;
				}
				if (!have) {
					g_ptr_array_add(drivers, drv);
				} else {
					g_free(drv);
				}
			} else {
				missing++;
			}
			g_free(iface);
		}
		g_ptr_array_unref(subs);
		char *auth = sys_attr(dev, "authorized");
		bool disabled = auth && strcmp(auth, "0") == 0;
		g_free(auth);
		struct tw_device *d = device_new(all, dev, hub ? "usbctl" : missing && !disabled ?
			"other" : "usb");
		GHashTable *udev = udev_props(d->key);
		char *product = sys_attr(dev, "product"), *manuf = sys_attr(dev, "manufacturer");
		const char *model = g_hash_table_lookup(udev, "ID_MODEL_FROM_DATABASE");
		char *speed = sys_attr(dev, "speed");
		if (root_hub) {
			const char *usb3 = speed && atoi(speed) >= 5000 ? "USB 3" : "USB 2.0";
			d->name = g_strdup_printf("USB Root Hub (%s)", usb3);
		} else if (hub) {
			d->name = g_strdup(product && *product ? product : "Generic USB Hub");
		} else {
			bool says_little = !product || strlen(product) < 4 ||
				strspn(product, "0123456789 ") == strlen(product);
			d->name = g_strdup(!says_little ? product : model ? model : product ? product :
				"USB device");
		}
		d->manufacturer = g_strdup(root_hub ? "Linux Foundation" :
			g_hash_table_lookup(udev, "ID_VENDOR_FROM_DATABASE") ?
			g_hash_table_lookup(udev, "ID_VENDOR_FROM_DATABASE") : manuf);
		const char *dash = strchr(sysname, '-');
		d->location = root_hub ? g_strdup_printf("USB bus %s", sysname + 3) :
			dash ? g_strdup_printf("Port %s on USB bus %.*s", dash + 1, (int)(dash - sysname),
			sysname) : NULL;
		if (drivers->len) {
			GString *all_drivers = g_string_new(NULL);
			for (guint m = 0; m < drivers->len; m++) {
				g_string_append_printf(all_drivers, "%s%s", m ? ", " : "",
					(char *)drivers->pdata[m]);
			}
			d->driver = g_string_free(all_drivers, FALSE);
			// the module of the first function's driver
			GPtrArray *again = list_sys(dev);
			for (guint j = 0; j < again->len && !d->module; j++) {
				const char *s = again->pdata[j];
				if (g_str_has_prefix(s, prefix)) {
					char *iface = g_strdup_printf("%s/%s", dev, s);
					d->module = module_of(iface);
					g_free(iface);
				}
			}
			g_ptr_array_unref(again);
		}
		if (!root_hub) {
			set_action(d, dev);
		}
		char *vid = sys_attr(dev, "idVendor"), *pid = sys_attr(dev, "idProduct");
		char *vp = g_strdup_printf("%s:%s", vid ? vid : "", pid ? pid : "");
		prop(d, "Vendor and product ID", vp);
		g_free(vp);
		prop(d, "Name the device gives itself", product);
		prop(d, "Manufacturer the device gives", manuf);
		char *serial = sys_attr(dev, "serial");
		prop(d, "Serial number", serial);
		g_free(serial);
		if (speed) {
			char *s = g_strdup_printf("%s Mbit/s", speed);
			prop(d, "Speed", s);
			g_free(s);
		}
		char *power = sys_attr(dev, "bMaxPower");
		prop(d, "Power it may draw", power);
		g_free(power);
		char *count = g_strdup_printf("%d, %d without a driver", functions, missing);
		prop(d, "Functions (interfaces)", count);
		g_free(count);
		common_props(d, udev);
		add_log(d, k, d->key);
		char *usb_key = g_strdup_printf("+usb:%s", sysname);
		add_log(d, k, usb_key);
		g_free(usb_key);
		if (missing && !disabled && !root_hub) {
			d->status = g_strdup_printf("The driver for %s of this device's %d function%s is "
				"not installed: Linux has none in use for %s. It may need a driver package, or "
				"Linux does not support it.", missing == functions ? (functions == 1 ? "the one" :
				"all") : "some", functions, functions == 1 ? "" : "s", missing == 1 ? "it" : "them");
			d->problem = TW_DEV_WARNING;
		}
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_ptr_array_unref(drivers);
		g_free(prefix);
		g_free(product);
		g_free(manuf);
		g_free(speed);
		g_free(vid);
		g_free(pid);
		g_free(dev);
	}
	g_ptr_array_unref(names);
}

/* ---------- the devices of the classes: input, sound, block, net, ... ---------- */

/* The names a device of a class has: its class folder entries, resolved. */
static GPtrArray *class_devices(const char *cls) {
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	char *dir = g_strdup_printf("/sys/class/%s", cls);
	GPtrArray *names = list_dir(dir);
	for (guint i = 0; i < names->len; i++) {
		char *link = g_strdup_printf("class/%s/%s", cls, (char *)names->pdata[i]);
		char *dev = sys_resolve(link);
		if (dev) {
			g_ptr_array_add(out, dev);
		}
		g_free(link);
	}
	g_ptr_array_unref(names);
	g_free(dir);
	return out;
}

/* The product name of the USB device a device belongs to, if it is one. */
static char *usb_product_above(const char *dev, char **manufacturer) {
	char *p = parent_of(dev);
	while (p) {
		if (sys_has(p, "idVendor")) {
			char *name = sys_attr(p, "product");
			if (manufacturer) {
				*manufacturer = sys_attr(p, "manufacturer");
			}
			g_free(p);
			return name;
		}
		char *up = parent_of(p);
		g_free(p);
		p = up;
	}
	return NULL;
}

static void scan_input(GPtrArray *all, struct klog *k) {
	GPtrArray *devs = class_devices("input");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *base = g_path_get_basename(dev);
		if (!g_str_has_prefix(base, "input")) {
			g_free(base);
			continue;
		}
		g_free(base);
		// what udev made of it is on its event node
		GHashTable *udev = NULL;
		GPtrArray *subs = list_sys(dev);
		for (guint j = 0; j < subs->len && !udev; j++) {
			if (g_str_has_prefix(subs->pdata[j], "event")) {
				char *ev = g_strdup_printf("%s/%s", dev, (char *)subs->pdata[j]);
				char *key = device_key(ev);
				udev = udev_props(key);
				g_free(key);
				g_free(ev);
			}
		}
		g_ptr_array_unref(subs);
		if (!udev) {
			udev = udev_props(NULL);
		}
		char *name = sys_attr(dev, "name"), *phys = sys_attr(dev, "phys");
		bool is = false;
#define HAS(p) ((is = g_strcmp0(g_hash_table_lookup(udev, p), "1") == 0))
		const char *category = HAS("ID_INPUT_KEYBOARD") ? "keyboards" :
			HAS("ID_INPUT_MOUSE") || HAS("ID_INPUT_TOUCHPAD") || HAS("ID_INPUT_TABLET") ||
			HAS("ID_INPUT_POINTINGSTICK") ? "mice" : "hid";
#undef HAS
		(void)is;
		// what is not on a bus of input devices is part of the computer: the lid, the
		// power button, the hotkeys of the firmware, the PC speaker
		char *owner = NULL, *owner_drv = driver_above(dev, &owner);
		char *owner_bus = owner ? sys_link_name(owner, "subsystem") : NULL;
		if (strcmp(category, "hid") == 0 && !(owner_bus && (strcmp(owner_bus, "hid") == 0 ||
				strcmp(owner_bus, "usb") == 0 || strcmp(owner_bus, "serio") == 0 ||
				strcmp(owner_bus, "i2c") == 0 || strcmp(owner_bus, "bluetooth") == 0))) {
			category = "system";
		}
		g_free(owner);
		g_free(owner_drv);
		g_free(owner_bus);
		struct tw_device *d = device_new(all, dev, category);
		d->hidden = phys && strcmp(phys, "ALSA") == 0; // a jack of a sound card
		d->name = g_strdup(name ? name : "Input device");
		if (g_strcmp0(g_hash_table_lookup(udev, "ID_INPUT_TOUCHPAD"), "1") == 0) {
			d->icon = g_strdup("input-touchpad-symbolic");
		}
		char *manuf = NULL, *product = usb_product_above(dev, &manuf);
		d->manufacturer = manuf ? manuf : g_strdup(g_hash_table_lookup(udev, "ID_VENDOR"));
		g_free(product);
		d->location = g_strdup(phys && *phys ? phys : g_hash_table_lookup(udev, "ID_PATH"));
		take_driver_above(d, dev);
		set_action_above(d, dev);
		// a keyboard of the laptop itself (on serio) is not disabled by mistake as easily:
		// it stays, but the page asks first
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(name);
		g_free(phys);
	}
	g_ptr_array_unref(devs);
}

static void scan_hid(GPtrArray *all, struct klog *k) {
	GPtrArray *names = list_dir("/sys/bus/hid/devices");
	for (guint i = 0; i < names->len; i++) {
		char *link = g_strdup_printf("bus/hid/devices/%s", (char *)names->pdata[i]);
		char *dev = sys_resolve(link);
		g_free(link);
		if (!dev) {
			continue;
		}
		struct tw_device *d = device_new(all, dev, "hid");
		GHashTable *udev = udev_props(d->key);
		char *uevent = NULL, *path = g_strdup_printf("%s/sys/%s/uevent", root(), dev);
		char *hid_name = NULL;
		if (g_file_get_contents(path, &uevent, NULL, NULL)) {
			char **lines = g_strsplit(uevent, "\n", -1);
			for (int l = 0; lines[l]; l++) {
				if (g_str_has_prefix(lines[l], "HID_NAME=")) {
					hid_name = g_strdup(lines[l] + 9);
				}
			}
			g_strfreev(lines);
			g_free(uevent);
		}
		g_free(path);
		char *bus = g_strndup(names->pdata[i], 4);
		const char *over = strcmp(bus, "0003") == 0 ? "USB" : strcmp(bus, "0018") == 0 ? "I2C" :
			strcmp(bus, "0005") == 0 ? "Bluetooth" : strcmp(bus, "001F") == 0 ? "sensor hub" : NULL;
		d->name = over ? g_strdup_printf("%s HID device%s%s", over, hid_name ? ": " : "",
			hid_name ? hid_name : "") : g_strdup(hid_name ? hid_name : "HID device");
		g_free(bus);
		g_free(hid_name);
		take_driver(d, dev);
		char *carrier = parent_of(dev);
		if (carrier) {
			set_action_above(d, carrier);
			g_free(carrier);
		}
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(dev);
	}
	g_ptr_array_unref(names);
}

/* The codecs of the sound cards: Realtek ALC289, the HDMI outputs of a GPU. */
static void scan_hdaudio(GPtrArray *all, struct klog *k) {
	GPtrArray *names = list_dir("/sys/bus/hdaudio/devices");
	for (guint i = 0; i < names->len; i++) {
		char *link = g_strdup_printf("bus/hdaudio/devices/%s", (char *)names->pdata[i]);
		char *dev = sys_resolve(link);
		g_free(link);
		if (!dev) {
			continue;
		}
		struct tw_device *d = device_new(all, dev, "sound");
		GHashTable *udev = udev_props(d->key);
		char *vendor = sys_attr(dev, "vendor_name"), *chip = sys_attr(dev, "chip_name");
		d->name = g_strdup_printf("%s%s%s High Definition Audio", vendor ? vendor : "",
			vendor ? " " : "", chip ? chip : "");
		d->manufacturer = g_strdup(vendor);
		d->location = g_strdup_printf("On %s", (char *)names->pdata[i]);
		take_driver(d, dev);
		set_action(d, dev);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, true);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(vendor);
		g_free(chip);
		g_free(dev);
	}
	g_ptr_array_unref(names);
}

static void scan_sound(GPtrArray *all, struct klog *k) {
	// sound cards not on PCI (USB headsets, ...): those on PCI are their controller
	GPtrArray *devs = class_devices("sound");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *base = g_path_get_basename(dev);
		bool card = g_str_has_prefix(base, "card");
		g_free(base);
		char *parent = parent_of(dev);
		char *bus = parent ? sys_link_name(parent, "subsystem") : NULL;
		bool on_pci = bus && strcmp(bus, "pci") == 0;
		g_free(bus);
		if (!card || on_pci || !parent) {
			g_free(parent);
			continue;
		}
		struct tw_device *d = device_new(all, dev, "sound");
		GHashTable *udev = udev_props(d->key);
		char *manuf = NULL, *product = usb_product_above(dev, &manuf);
		char *id = sys_attr(dev, "id");
		d->name = g_strdup(product ? product : id ? id : "Sound card");
		d->manufacturer = manuf;
		take_driver_above(d, dev);
		set_action_above(d, dev);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(product);
		g_free(id);
		g_free(parent);
	}
	g_ptr_array_unref(devs);
}

static void scan_block(GPtrArray *all, struct klog *k) {
	GPtrArray *devs = class_devices("block");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		if (sys_has(dev, "partition")) {
			continue;
		}
		char *base = g_path_get_basename(dev);
		bool is_virtual = g_str_has_prefix(dev, "devices/virtual/");
		char *size_text = sys_attr(dev, "size");
		guint64 size = size_text ? g_ascii_strtoull(size_text, NULL, 10) * 512 : 0;
		g_free(size_text);
		if (size == 0 && is_virtual) {
			g_free(base); // an unused loop device
			continue;
		}
		struct tw_device *d = device_new(all, dev, is_virtual ? "software" :
			g_str_has_prefix(base, "sr") ? "optical" : "disks");
		d->hidden = is_virtual;
		GHashTable *udev = udev_props(d->key);
		char *model = sys_attr(dev, "device/model");
		const char *udev_model = g_hash_table_lookup(udev, "ID_MODEL");
		char *dm_name = sys_attr(dev, "dm/name");
		char *sz = g_format_size(size);
		if (dm_name) {
			d->name = g_strdup_printf("%s (%s)", dm_name, sz);
		} else if (model || udev_model) {
			char *m = g_strdup(model ? model : udev_model);
			g_strdelimit(m, "_", ' ');
			d->name = g_strdup_printf("%s (%s)", g_strstrip(m), sz);
			g_free(m);
		} else {
			d->name = g_strdup_printf("%s (%s)", base, sz);
		}
		g_free(sz);
		d->manufacturer = g_strdup(g_hash_table_lookup(udev, "ID_VENDOR"));
		d->location = g_strdup(g_hash_table_lookup(udev, "ID_PATH"));
		if (!is_virtual) {
			take_driver_above(d, dev);
		}
		prop(d, "Block device", base);
		char *removable = sys_attr(dev, "removable");
		prop(d, "Removable", removable && strcmp(removable, "1") == 0 ? "yes" : "no");
		g_free(removable);
		char *rot = sys_attr(dev, "queue/rotational");
		prop(d, "Kind", rot && strcmp(rot, "1") == 0 ? "spinning disk" : "solid state");
		g_free(rot);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		g_hash_table_unref(udev);
		g_free(model);
		g_free(dm_name);
		g_free(base);
	}
	g_ptr_array_unref(devs);
}

static void scan_net(GPtrArray *all, struct klog *k) {
	GPtrArray *devs = class_devices("net");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *parent = parent_of(dev);
		char *pbus = parent ? sys_link_name(parent, "subsystem") : NULL;
		bool on_pci = pbus && strcmp(pbus, "pci") == 0;
		bool is_virtual = g_str_has_prefix(dev, "devices/virtual/");
		g_free(pbus);
		g_free(parent);
		if (on_pci) {
			continue; // its PCI adapter is the one in the list
		}
		char *base = g_path_get_basename(dev);
		struct tw_device *d = device_new(all, dev, "network");
		d->hidden = is_virtual;
		GHashTable *udev = udev_props(d->key);
		char *manuf = NULL, *product = usb_product_above(dev, &manuf);
		d->name = product ? g_strdup_printf("%s (%s)", product, base) :
			g_strdup_printf(is_virtual ? "%s (a network of the computer itself)" : "%s", base);
		d->manufacturer = manuf;
		if (sys_has(dev, "wireless") || sys_has(dev, "phy80211")) {
			d->icon = g_strdup("network-wireless-symbolic");
		}
		if (!is_virtual) {
			take_driver_above(d, dev);
			set_action_above(d, dev);
		}
		char *mac = sys_attr(dev, "address");
		prop(d, "Network interface", base);
		prop(d, "Hardware address (MAC)", mac);
		g_free(mac);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(product);
		g_free(base);
	}
	g_ptr_array_unref(devs);
}

/* The name a monitor gives itself in its EDID, and its maker's three letters. */
static void edid_names(const guint8 *e, gsize n, char **name, char **maker) {
	*name = *maker = NULL;
	if (n < 128) {
		return;
	}
	unsigned id = (e[8] << 8) | e[9];
	*maker = g_strdup_printf("%c%c%c", '@' + ((id >> 10) & 31), '@' + ((id >> 5) & 31),
		'@' + (id & 31));
	for (int b = 54; b <= 108; b += 18) {
		if (e[b] == 0 && e[b + 1] == 0 && e[b + 3] == 0xfc) {
			char text[14] = { 0 };
			memcpy(text, e + b + 5, 13);
			text[strcspn(text, "\n")] = '\0';
			*name = g_strdup(g_strstrip(text));
		}
	}
}

static void scan_monitors(GPtrArray *all, struct klog *k) {
	GPtrArray *devs = class_devices("drm");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *base = g_path_get_basename(dev);
		char *status = strchr(base, '-') ? sys_attr(dev, "status") : NULL;
		if (!status || strcmp(status, "connected") != 0) {
			g_free(status);
			g_free(base);
			continue;
		}
		struct tw_device *d = device_new(all, dev, "monitors");
		char *path = g_strdup_printf("%s/sys/%s/edid", root(), dev), *edid = NULL;
		gsize len = 0;
		char *name = NULL, *maker = NULL;
		if (g_file_get_contents(path, &edid, &len, NULL)) {
			edid_names((guint8 *)edid, len, &name, &maker);
			g_free(edid);
		}
		g_free(path);
		const char *connector = strchr(base, '-') + 1;
		bool builtin = g_str_has_prefix(connector, "eDP") || g_str_has_prefix(connector, "LVDS") ||
			g_str_has_prefix(connector, "DSI");
		d->name = name ? g_strdup(name) : g_strdup(builtin ? "Built-in display" :
			"Generic PnP Monitor");
		d->manufacturer = maker;
		d->location = g_strdup_printf("On %s%s", connector, builtin ? " (built in)" : "");
		take_driver_above(d, dev);
		prop(d, "Connector", base);
		char *modes = sys_attr(dev, "modes");
		prop(d, "Best mode", modes);
		g_free(modes);
		GHashTable *udev = udev_props(NULL);
		common_props(d, udev);
		g_hash_table_unref(udev);
		judge(d, k, false);
		g_free(name);
		g_free(status);
		g_free(base);
	}
	g_ptr_array_unref(devs);
}

static void scan_power(GPtrArray *all, struct klog *k) {
	GPtrArray *devs = class_devices("power_supply");
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *type = sys_attr(dev, "type"), *base = g_path_get_basename(dev);
		bool battery = type && strcmp(type, "Battery") == 0;
		bool mains = type && strcmp(type, "Mains") == 0;
		struct tw_device *d = device_new(all, dev, "batteries");
		d->hidden = !battery && !mains; // the power of the USB-C ports
		char *model = sys_attr(dev, "model_name"), *manuf = sys_attr(dev, "manufacturer");
		d->name = battery ? g_strdup_printf("Battery %s%s%s", base, model ? ": " : "",
			model ? model : "") : mains ? g_strdup("AC Adapter") : g_strdup(base);
		d->manufacturer = manuf;
		if (mains) {
			d->icon = g_strdup("ac-adapter-symbolic");
		}
		take_driver_above(d, dev);
		const char *attrs[] = { "technology", "capacity", "status", "cycle_count",
			"energy_full_design", "energy_full", NULL };
		for (int a = 0; attrs[a]; a++) {
			char *v = sys_attr(dev, attrs[a]);
			prop(d, attrs[a], v);
			g_free(v);
		}
		GHashTable *udev = udev_props(d->key);
		common_props(d, udev);
		g_hash_table_unref(udev);
		judge(d, k, false);
		g_free(type);
		g_free(base);
		g_free(model);
	}
	g_ptr_array_unref(devs);
}

/* Devices of a class that are one each: Bluetooth adapters, cameras, TPMs, serial ports. */
static void scan_simple(GPtrArray *all, struct klog *k, const char *cls, const char *category) {
	GPtrArray *devs = class_devices(cls);
	for (guint i = 0; i < devs->len; i++) {
		const char *dev = devs->pdata[i];
		char *base = g_path_get_basename(dev);
		if ((strcmp(cls, "video4linux") == 0 && !g_str_has_prefix(base, "video")) ||
				(strcmp(cls, "tty") == 0 && !g_str_has_prefix(base, "ttyUSB") &&
				!g_str_has_prefix(base, "ttyACM"))) {
			g_free(base);
			continue;
		}
		if (strcmp(cls, "video4linux") == 0) {
			char *index = sys_attr(dev, "index");
			bool first = !index || strcmp(index, "0") == 0;
			g_free(index);
			if (!first) {
				g_free(base); // a camera's other nodes (its metadata)
				continue;
			}
		}
		struct tw_device *d = device_new(all, dev, category);
		GHashTable *udev = udev_props(d->key);
		char *manuf = NULL, *product = usb_product_above(dev, &manuf);
		char *name = sys_attr(dev, "name");
		if (strcmp(cls, "tpm") == 0) {
			char *major = sys_attr(dev, "tpm_version_major");
			d->name = g_strdup_printf("Trusted Platform Module %s", major && strcmp(major, "2") == 0 ?
				"2.0" : major ? major : "");
			g_free(major);
		} else if (strcmp(cls, "tty") == 0) {
			d->name = g_strdup_printf("%s (%s)", product ? product : "USB serial device", base);
		} else {
			d->name = g_strdup(product ? product : name ? name : base);
		}
		d->manufacturer = manuf;
		take_driver_above(d, dev);
		set_action_above(d, dev);
		prop(d, "Device", base);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		disabled_check(d);
		g_hash_table_unref(udev);
		g_free(product);
		g_free(name);
		g_free(base);
	}
	g_ptr_array_unref(devs);
}

static void scan_processors(GPtrArray *all, struct klog *k) {
	char *path = at_root("/proc/cpuinfo"), *text = NULL;
	GPtrArray *models = g_ptr_array_new_with_free_func(g_free);
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			if (g_str_has_prefix(lines[i], "model name")) {
				const char *colon = strchr(lines[i], ':');
				g_ptr_array_add(models, g_strstrip(g_strdup(colon ? colon + 1 : "")));
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);
	char *scaling = read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");
	GPtrArray *names = list_dir("/sys/devices/system/cpu");
	GPtrArray *cpus = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < names->len; i++) {
		const char *n = names->pdata[i];
		if (g_str_has_prefix(n, "cpu") && g_ascii_isdigit(n[3])) {
			g_ptr_array_add(cpus, g_strdup(n));
		}
	}
	// cpu2 before cpu10
	for (guint i = 0; i < cpus->len; i++) {
		for (guint j = i + 1; j < cpus->len; j++) {
			if (atoi((char *)cpus->pdata[j] + 3) < atoi((char *)cpus->pdata[i] + 3)) {
				gpointer t = cpus->pdata[i];
				cpus->pdata[i] = cpus->pdata[j];
				cpus->pdata[j] = t;
			}
		}
	}
	for (guint i = 0; i < cpus->len; i++) {
		int n = atoi((char *)cpus->pdata[i] + 3);
		char *dev = g_strdup_printf("devices/system/cpu/%s", (char *)cpus->pdata[i]);
		struct tw_device *d = device_new(all, dev, "processors");
		d->name = g_strdup(n < (int)models->len ? models->pdata[n] : models->len ?
			models->pdata[0] : "Processor");
		char *online = sys_attr(dev, "online");
		if (online && strcmp(online, "0") == 0) {
			d->status = g_strdup("This processor is offline: the kernel does not use it now.");
			d->problem = TW_DEV_DISABLED;
		}
		g_free(online);
		d->driver = g_strdup(scaling);
		char *loc = g_strdup_printf("Processor %d", n);
		d->location = loc;
		char *core = sys_attr(dev, "topology/core_id");
		prop(d, "Core", core);
		g_free(core);
		char *max = sys_attr(dev, "cpufreq/cpuinfo_max_freq");
		if (max) {
			char *mhz = g_strdup_printf("%ld MHz", atol(max) / 1000);
			prop(d, "Highest clock", mhz);
			g_free(mhz);
		}
		g_free(max);
		prop(d, "Frequency driver", scaling);
		GHashTable *udev = udev_props(NULL);
		common_props(d, udev);
		g_hash_table_unref(udev);
		judge(d, k, false);
		g_free(dev);
	}
	g_ptr_array_unref(cpus);
	g_ptr_array_unref(names);
	g_ptr_array_unref(models);
	g_free(scaling);
}

static void scan_machine(GPtrArray *all, struct klog *k) {
	char *vendor = read_line("/sys/class/dmi/id/sys_vendor");
	char *product = read_line("/sys/class/dmi/id/product_name");
	char *efi = at_root("/sys/firmware/efi");
	bool uefi = g_file_test(efi, G_FILE_TEST_IS_DIR);
	g_free(efi);
	struct tw_device *d = device_new(all, "devices/virtual/dmi/id", "computer");
	d->name = g_strdup_printf("%s%s%s", vendor ? vendor : "", vendor && product ? " " : "",
		product ? product : "This computer");
	d->manufacturer = g_strdup(vendor);
	d->status = g_strdup("This device is working properly.");
	prop(d, "Starts with", uefi ? "UEFI" : "BIOS (legacy)");
	const char *dmi[] = { "product_version", "board_vendor", "board_name", "chassis_type", NULL };
	for (int i = 0; dmi[i]; i++) {
		char *p = g_strdup_printf("/sys/class/dmi/id/%s", dmi[i]);
		char *v = read_line(p);
		prop(d, dmi[i], v);
		g_free(v);
		g_free(p);
	}
	char *bios_vendor = read_line("/sys/class/dmi/id/bios_vendor");
	char *bios_version = read_line("/sys/class/dmi/id/bios_version");
	char *bios_date = read_line("/sys/class/dmi/id/bios_date");
	if (bios_version) {
		struct tw_device *f = device_new(all, "devices/virtual/dmi/id", "firmware");
		f->name = g_strdup_printf("System Firmware %s", bios_version);
		f->manufacturer = g_strdup(bios_vendor);
		f->status = g_strdup("This device is working properly.");
		prop(f, "Version", bios_version);
		prop(f, "Date", bios_date);
		prop(f, "Kind", uefi ? "UEFI" : "BIOS");
		g_free(f->key);
		f->key = NULL;
	}
	g_free(d->key);
	d->key = NULL;
	(void)k;
	g_free(bios_vendor);
	g_free(bios_version);
	g_free(bios_date);
	g_free(vendor);
	g_free(product);
}

/* Platform devices with a driver: the thermal, the buttons, ... (hidden, as Windows'
 * software devices are). */
static void scan_platform(GPtrArray *all, struct klog *k) {
	GPtrArray *names = list_dir("/sys/bus/platform/devices");
	for (guint i = 0; i < names->len; i++) {
		char *link = g_strdup_printf("bus/platform/devices/%s", (char *)names->pdata[i]);
		char *dev = sys_resolve(link);
		g_free(link);
		if (!dev || !sys_has(dev, "driver")) {
			g_free(dev);
			continue;
		}
		struct tw_device *d = device_new(all, dev, "system");
		d->hidden = true;
		d->name = g_strdup(names->pdata[i]);
		take_driver(d, dev);
		GHashTable *udev = udev_props(d->key);
		common_props(d, udev);
		add_log(d, k, d->key);
		judge(d, k, false);
		g_hash_table_unref(udev);
		g_free(dev);
	}
	g_ptr_array_unref(names);
}

static int compare_devices(gconstpointer a, gconstpointer b) {
	const struct tw_device *x = *(struct tw_device **)a, *y = *(struct tw_device **)b;
	int c = strcmp(tw_dev_category(x->category)->title, tw_dev_category(y->category)->title);
	if (c) {
		return c;
	}
	// processors in their order, everything else by name
	if (strcmp(x->category, "processors") == 0) {
		return g_strcmp0(x->location, y->location) == 0 ? 0 :
			atoi(x->location + 10) - atoi(y->location + 10);
	}
	return g_utf8_collate(x->name, y->name);
}

GPtrArray *tw_devices_scan(void) {
	GPtrArray *all = g_ptr_array_new_with_free_func((GDestroyNotify)tw_device_free);
	struct klog k = { 0 };
	klog_read(&k);
	scan_machine(all, &k);
	scan_pci(all, &k);
	scan_usb(all, &k);
	scan_hid(all, &k);
	scan_input(all, &k);
	scan_hdaudio(all, &k);
	scan_sound(all, &k);
	scan_block(all, &k);
	scan_net(all, &k);
	scan_monitors(all, &k);
	scan_power(all, &k);
	scan_simple(all, &k, "bluetooth", "bluetooth");
	scan_simple(all, &k, "video4linux", "cameras");
	scan_simple(all, &k, "tpm", "security");
	scan_simple(all, &k, "tty", "ports");
	scan_processors(all, &k);
	scan_platform(all, &k);
	klog_free(&k);
	for (guint i = 0; i < all->len; i++) {
		struct tw_device *d = all->pdata[i];
		// an unknown device has the icon of one; the category's is the warning sign
		if (!d->icon) {
			d->icon = g_strdup(strcmp(d->category, "other") == 0 ? "dialog-question-symbolic" :
				tw_dev_category(d->category)->icon);
		}
		if (!d->status) {
			d->status = g_strdup("This device is working properly.");
		}
	}
	g_ptr_array_sort(all, compare_devices);
	return all;
}

int tw_devices_print(bool hidden) {
	GPtrArray *all = tw_devices_scan();
	for (guint i = 0; i < all->len; i++) {
		struct tw_device *d = all->pdata[i];
		if (d->hidden && !hidden) {
			continue;
		}
		GString *fw = g_string_new(NULL);
		for (guint j = 0; j < d->firmware->len; j++) {
			g_string_append_printf(fw, "%s%s", j ? "," : "", (char *)d->firmware->pdata[j]);
		}
		printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", d->category, d->name,
			d->problem == TW_DEV_WARNING ? "warning" : d->problem == TW_DEV_DISABLED ? "disabled" :
			"ok", d->driver ? d->driver : "-", d->action ? d->action : "-", d->status,
			fw->len ? fw->str : "-");
		g_string_free(fw, TRUE);
	}
	g_ptr_array_unref(all);
	return 0;
}
