#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#if HAVE_TRAY
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif
#endif
#include "log.h"
#include "panel.h"
#include "tw_desktop.h"

/*
 * Progress and counts of apps on their taskbar buttons, as Windows 7 shows a
 * download filling the button green and Windows 10 a number on the icon.
 * Apps tell them with the LauncherEntry signal Unity's dock introduced, which
 * Firefox, Chromium, file managers, mail and chat apps send:
 *
 *   com.canonical.Unity.LauncherEntry.Update("application://firefox.desktop",
 *       { "progress": 0.4, "progress-visible": true, "count": 3, ... })
 *
 * What an app said is kept until it says otherwise or leaves the bus.
 */

struct badge {
	char *desktop_id; // e.g. "firefox.desktop"
	char *sender;     // unique bus name of the app
	double progress;
	bool progress_visible;
	int64_t count;
	bool count_visible;
	bool urgent;
};

static struct {
	struct panel *panel;
	list_t *badges; // struct badge *
#if HAVE_TRAY
	sd_bus *bus;
	int fd;
#endif
} bs;

static void badge_free(struct badge *b) {
	free(b->desktop_id);
	free(b->sender);
	free(b);
}

static struct badge *find_badge(const char *desktop_id) {
	for (int i = 0; bs.badges && i < bs.badges->length; i++) {
		struct badge *b = bs.badges->items[i];
		if (strcmp(b->desktop_id, desktop_id) == 0) {
			return b;
		}
	}
	return NULL;
}

static bool badge_shows(const struct badge *b) {
	return b->progress_visible || (b->count_visible && b->count > 0) || b->urgent;
}

const struct app_badge *badges_for_app(const char *app_id, struct app_badge *out) {
	if (!bs.badges || bs.badges->length == 0 || !app_id) {
		return NULL;
	}
	struct tw_desktop_entry *e = apps_find(app_id);
	char id[256];
	snprintf(id, sizeof(id), "%s.desktop", app_id);
	struct badge *b = find_badge(e ? e->id : id);
	if (!b || !badge_shows(b)) {
		return NULL;
	}
	out->progress = b->progress_visible ? b->progress : -1;
	out->count = b->count_visible ? b->count : 0;
	out->urgent = b->urgent;
	return out;
}

#if HAVE_TRAY

static void changed(void) {
	panel_set_dirty(bs.panel);
}

static int handle_update(sd_bus_message *m, void *data, sd_bus_error *error) {
	const char *uri = NULL;
	if (sd_bus_message_read(m, "s", &uri) < 0 || !uri) {
		return 0;
	}
	const char *id = strncmp(uri, "application://", 14) == 0 ? uri + 14 : uri;
	if (!*id) {
		return 0;
	}
	struct badge *b = find_badge(id);
	if (!b) {
		b = calloc(1, sizeof(*b));
		if (!b) {
			return 0;
		}
		b->desktop_id = strdup(id);
		list_add(bs.badges, b);
	}
	const char *sender = sd_bus_message_get_sender(m);
	if (sender && (!b->sender || strcmp(b->sender, sender) != 0)) {
		free(b->sender);
		b->sender = strdup(sender);
	}
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) {
		return 0;
	}
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL;
		sd_bus_message_read(m, "s", &key);
		const char *type = NULL;
		sd_bus_message_peek_type(m, NULL, &type);
		if (key && type && strcmp(key, "progress") == 0 && strcmp(type, "d") == 0) {
			sd_bus_message_read(m, "v", "d", &b->progress);
		} else if (key && type && strcmp(key, "count") == 0 &&
				(strcmp(type, "x") == 0 || strcmp(type, "i") == 0)) {
			if (type[0] == 'x') {
				sd_bus_message_read(m, "v", "x", &b->count);
			} else {
				int32_t count = 0;
				sd_bus_message_read(m, "v", "i", &count);
				b->count = count;
			}
		} else if (key && type && strcmp(type, "b") == 0 &&
				(strcmp(key, "progress-visible") == 0 || strcmp(key, "count-visible") == 0 ||
				strcmp(key, "urgent") == 0)) {
			int value = 0;
			sd_bus_message_read(m, "v", "b", &value);
			if (strcmp(key, "progress-visible") == 0) {
				b->progress_visible = value;
			} else if (strcmp(key, "count-visible") == 0) {
				b->count_visible = value;
			} else {
				b->urgent = value;
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	b->progress = b->progress < 0 ? 0 : b->progress > 1 ? 1 : b->progress;
	changed();
	return 0;
}

/* An app that leaves the bus takes its progress and count with it. */
static int handle_owner_changed(sd_bus_message *m, void *data, sd_bus_error *error) {
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 ||
			!name || name[0] != ':' || (new_owner && *new_owner)) {
		return 0;
	}
	bool any = false;
	for (int i = bs.badges->length - 1; i >= 0; i--) {
		struct badge *b = bs.badges->items[i];
		if (b->sender && strcmp(b->sender, name) == 0) {
			badge_free(b);
			list_del(bs.badges, i);
			any = true;
		}
	}
	if (any) {
		changed();
	}
	return 0;
}

static void bus_in(int fd, short mask, void *data) {
	int r;
	while ((r = sd_bus_process(bs.bus, NULL)) > 0) {
		// keep processing
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Taskbar badges: bus error: %s", strerror(-r));
		loop_remove_fd(bs.panel->loop, bs.fd);
		sd_bus_flush_close_unref(bs.bus);
		bs.bus = NULL;
	}
}

void badges_init(struct panel *panel) {
	bs.panel = panel;
	if (!bs.badges) {
		bs.badges = create_list();
	}
	if (bs.bus) {
		return;
	}
	sd_bus *bus = NULL;
	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		sway_log(SWAY_INFO, "Taskbar badges: no user bus: %s", strerror(-r));
		return;
	}
	sd_bus_match_signal(bus, NULL, NULL, NULL, "com.canonical.Unity.LauncherEntry", "Update",
		handle_update, NULL);
	sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", handle_owner_changed, NULL);
	bs.bus = bus;
	bs.fd = sd_bus_get_fd(bus);
	loop_add_fd(panel->loop, bs.fd, POLLIN, bus_in, NULL);
	bus_in(bs.fd, 0, NULL);
}

#else

void badges_init(struct panel *panel) {
	bs.panel = panel;
}

#endif
