#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include "log.h"
#include "sway/config.h"
#include "sway/ipc-server.h"
#include "sway/server.h"
#include "sway/tilewin.h"

/*
 * Battery saver, like the one of Windows: on battery and at or below a level
 * (battery_saver <percent>, 20 by default, "off" for never, 100 for always
 * on battery) the things that only look nice and keep the processor and
 * the graphics busy stop: the animations, the trail of the pointer, and in
 * the taskbar the charts that keep reading in the background (it is told with
 * the tilewin event "battery_saver"). Plugging in, or charging past the level,
 * brings them back.
 *
 * The batteries are read from /sys/class/power_supply once a minute, which
 * costs next to nothing; TILEWIN_SYS_POWER points elsewhere and
 * TILEWIN_BATTERY_POLL_MS reads more often, for the tests.
 */

#define POLL_MS 60000

static struct {
	bool active;
	int percent; // of the batteries, -1 for none
	bool on_battery;
	struct wl_event_source *timer;
} bs = { .percent = -1 };

static char *read_line(const char *dir, const char *name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char buf[64] = "";
	char *ok = fgets(buf, sizeof(buf), f);
	fclose(f);
	if (!ok) {
		return NULL;
	}
	buf[strcspn(buf, "\n")] = '\0';
	return strdup(buf);
}

/* The lowest charge of the batteries and whether the computer runs on them. */
static void read_power(int *percent, bool *on_battery) {
	const char *root = getenv("TILEWIN_SYS_POWER");
	root = root && *root ? root : "/sys/class/power_supply";
	*percent = -1;
	*on_battery = false;
	bool mains = false, discharging = false;
	DIR *d = opendir(root);
	if (!d) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d))) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char dir[512];
		snprintf(dir, sizeof(dir), "%s/%s", root, entry->d_name);
		char *type = read_line(dir, "type");
		if (type && strcasecmp(type, "Battery") == 0) {
			char *scope = read_line(dir, "scope");
			// a mouse or a headset reports its battery here too: not the computer's
			if (!scope || strcasecmp(scope, "Device") != 0) {
				char *capacity = read_line(dir, "capacity");
				char *status = read_line(dir, "status");
				if (capacity) {
					int value = atoi(capacity);
					if (*percent < 0 || value < *percent) {
						*percent = value;
					}
				}
				if (status && strcasecmp(status, "Discharging") == 0) {
					discharging = true;
				}
				free(capacity);
				free(status);
			}
			free(scope);
		} else if (type && strcasecmp(type, "Mains") == 0) {
			char *online = read_line(dir, "online");
			mains = mains || (online && atoi(online) == 1);
			free(online);
		}
		free(type);
	}
	closedir(d);
	*on_battery = *percent >= 0 && (discharging || !mains);
}

static void announce(void) {
	json_object *data = json_object_new_object();
	json_object_object_add(data, "active", json_object_new_boolean(bs.active));
	json_object_object_add(data, "percent", json_object_new_int(bs.percent));
	ipc_event_tilewin("battery_saver", data);
}

void tw_battery_saver_check(void) {
	read_power(&bs.percent, &bs.on_battery);
	int level = config ? config->tw_battery_saver : 0;
	bool want = level > 0 && bs.on_battery && bs.percent >= 0 && bs.percent <= level;
	if (want == bs.active) {
		return;
	}
	bs.active = want;
	sway_log(SWAY_INFO, "Battery saver %s (%d%%, %s)", want ? "on" : "off", bs.percent,
		bs.on_battery ? "on battery" : "plugged in");
	if (want) {
		tw_pointer_trail_changed(); // the copies there are go away now
	}
	announce();
}

static int handle_timer(void *data) {
	tw_battery_saver_check();
	const char *poll = getenv("TILEWIN_BATTERY_POLL_MS");
	wl_event_source_timer_update(bs.timer, poll && atoi(poll) > 0 ? atoi(poll) : POLL_MS);
	return 0;
}

void tw_battery_saver_init(void) {
	if (bs.timer) {
		return;
	}
	bs.timer = wl_event_loop_add_timer(server.wl_event_loop, handle_timer, NULL);
	handle_timer(NULL);
}

void tw_battery_saver_fini(void) {
	if (bs.timer) {
		wl_event_source_remove(bs.timer);
		bs.timer = NULL;
	}
}

bool tw_battery_saver_active(void) {
	return bs.active;
}
