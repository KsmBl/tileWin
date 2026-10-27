#ifndef TW_SETTINGS_DEVICES_H
#define TW_SETTINGS_DEVICES_H

#include <glib.h>
#include <stdbool.h>

/*
 * The devices of the computer, sorted the way the Device Manager of Windows
 * sorts them, read from sysfs, the database of udev (/run/udev/data) and the
 * kernel's messages. TILEWIN_DEVICES_ROOT puts all of those below a folder of
 * its own (for the tests), TILEWIN_DEVICES_KLOG names a file of journalctl's
 * JSON lines to read instead of the journal.
 */

enum tw_dev_problem {
	TW_DEV_OK,
	TW_DEV_WARNING,  // no driver where one is needed, firmware missing, did not start
	TW_DEV_DISABLED,
};

struct tw_dev_category {
	const char *key, *title, *icon;
};

struct tw_device {
	char *path;          // below /sys: devices/pci0000:00/0000:00:14.3
	char *key;           // what udev and the journal call it: +pci:0000:00:14.3, c189:1, n3
	const char *category;
	char *name, *icon, *manufacturer, *location;
	char *driver;        // the driver bound to it (or to the device it belongs to)
	char *module;        // its kernel module, NULL when it is built into the kernel
	char *status;        // a sentence, as the General tab says it
	enum tw_dev_problem problem;
	bool hidden;         // shown with "Show hidden devices" only
	char *action;        // below /sys: the device Disable, Enable and Restart act on, or NULL
	char *action_bus;    // its bus: pci, usb, i2c, serio, hdaudio
	GPtrArray *props;    // char *, "NAME\tvalue" for the Details tab
	GPtrArray *resources; // char *, "KIND\tvalue" for the Resources tab
	GPtrArray *firmware; // char *, the firmware files its driver loaded
	GPtrArray *events;   // char *, its messages of this boot, oldest first
};

const struct tw_dev_category *tw_dev_categories(size_t *n);
const struct tw_dev_category *tw_dev_category(const char *key);

/* Every device found, in the order of the categories, then by name. */
GPtrArray *tw_devices_scan(void);
void tw_device_free(struct tw_device *d);

/* Whether tileWin's udev rules keep a device disabled. */
bool tw_device_disabled_by_rule(const char *action);

/* --list-devices: one device a line, for the tests and for a terminal:
 * CATEGORY NAME ok|warning|disabled DRIVER ACTION STATUS FIRMWARE, tab-separated. */
int tw_devices_print(bool hidden);

#endif
