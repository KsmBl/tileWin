#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/*
 * The media players there are, for the previous, play/pause and next buttons
 * under the preview of their windows, as Windows shows them for a player.
 * Players announce themselves on the user bus as org.mpris.MediaPlayer2.<name>
 * (Firefox, Chromium, Spotify, mpv, VLC, Rhythmbox, …); a player belongs to
 * the window of its process, or of the app its desktop entry names.
 *
 * Nothing is asked of a player while it plays: it says what changed itself
 * (PropertiesChanged), and every call to it is sent without waiting.
 */

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"

struct media {
	char *name;   // org.mpris.MediaPlayer2.<name>
	char *unique; // :1.42, the sender of its signals
	struct media_player state;
};

static struct {
	struct panel *panel;
	list_t *players; // struct media *
#if HAVE_TRAY
	sd_bus *bus;
	int fd;
#endif
} ms;

static void media_free(struct media *m) {
	free(m->name);
	free(m->unique);
	free(m->state.desktop_entry);
	free(m->state.identity);
	free(m);
}

/* Whether ancestor is pid or one of the processes pid was started from. */
static bool descends_from(int pid, int ancestor) {
	for (int depth = 0; pid > 1 && depth < 16; depth++) {
		if (pid == ancestor) {
			return true;
		}
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE *f = fopen(path, "r");
		if (!f) {
			return false;
		}
		char buf[512];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = '\0';
		char *close = strrchr(buf, ')'); // the name may hold spaces and parentheses
		int ppid = 0;
		if (!close || sscanf(close + 1, " %*c %d", &ppid) != 1) {
			return false;
		}
		pid = ppid;
	}
	return false;
}

static bool names_app(const char *entry, const char *app_id) {
	if (!entry || !*entry || !app_id || !*app_id) {
		return false;
	}
	size_t len = strlen(entry);
	if (len > 8 && strcmp(entry + len - 8, ".desktop") == 0) {
		len -= 8;
	}
	return strlen(app_id) == len && strncasecmp(entry, app_id, len) == 0;
}

const struct media_player *media_for_window(struct panel *panel, struct pwindow *win) {
	if (!win || !ms.players) {
		return NULL;
	}
	// its own process first: two windows of one browser are two players
	for (int i = 0; i < ms.players->length; i++) {
		struct media *m = ms.players->items[i];
		if (win->pid > 0 && m->state.pid > 0 && descends_from(m->state.pid, win->pid)) {
			return &m->state;
		}
	}
	for (int i = 0; i < ms.players->length; i++) {
		struct media *m = ms.players->items[i];
		if (names_app(m->state.desktop_entry, win->app_id) ||
				names_app(m->state.identity, win->app_id)) {
			return &m->state;
		}
	}
	return NULL;
}

static void changed(void) {
	thumbnails_media_changed();
}

#if HAVE_TRAY

static struct media *find(const char *name, const char *unique) {
	for (int i = 0; ms.players && i < ms.players->length; i++) {
		struct media *m = ms.players->items[i];
		if ((name && strcmp(m->name, name) == 0) ||
				(unique && m->unique && strcmp(m->unique, unique) == 0)) {
			return m;
		}
	}
	return NULL;
}

static void read_bool(sd_bus_message *msg, bool *out) {
	int value = 0;
	if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
		if (sd_bus_message_read(msg, "b", &value) >= 0) {
			*out = value;
		}
		sd_bus_message_exit_container(msg);
	} else {
		sd_bus_message_skip(msg, "v");
	}
}

static void read_string(sd_bus_message *msg, char **out) {
	const char *value = NULL;
	if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
		if (sd_bus_message_read(msg, "s", &value) >= 0 && value) {
			free(*out);
			*out = strdup(value);
		}
		sd_bus_message_exit_container(msg);
	} else {
		sd_bus_message_skip(msg, "v");
	}
}

/* The a{sv} of GetAll or PropertiesChanged, into what the buttons show. */
static void read_properties(struct media *m, sd_bus_message *msg) {
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0) {
		return;
	}
	while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(msg, "s", &key) < 0 || !key) {
			sd_bus_message_exit_container(msg);
			break;
		}
		struct media_player *p = &m->state;
		if (strcmp(key, "PlaybackStatus") == 0) {
			char *status = NULL;
			read_string(msg, &status);
			p->playing = status && strcmp(status, "Playing") == 0;
			free(status);
		} else if (strcmp(key, "CanGoNext") == 0) {
			read_bool(msg, &p->can_next);
		} else if (strcmp(key, "CanGoPrevious") == 0) {
			read_bool(msg, &p->can_previous);
		} else if (strcmp(key, "CanPlay") == 0) {
			read_bool(msg, &p->can_play);
		} else if (strcmp(key, "CanPause") == 0) {
			read_bool(msg, &p->can_pause);
		} else if (strcmp(key, "DesktopEntry") == 0) {
			read_string(msg, &p->desktop_entry);
		} else if (strcmp(key, "Identity") == 0) {
			read_string(msg, &p->identity);
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
}

static int got_properties(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = find(data, NULL);
	if (m && !sd_bus_message_is_method_error(msg, NULL)) {
		read_properties(m, msg);
		changed();
	}
	free(data);
	return 0;
}

static int got_pid(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = find(data, NULL);
	uint32_t pid = 0;
	if (m && !sd_bus_message_is_method_error(msg, NULL) &&
			sd_bus_message_read(msg, "u", &pid) >= 0) {
		m->state.pid = (int)pid;
		changed();
	}
	free(data);
	return 0;
}

static void add_player(const char *name, const char *unique) {
	struct media *m = find(name, NULL);
	if (!m) {
		m = calloc(1, sizeof(*m));
		if (!m) {
			return;
		}
		m->name = strdup(name);
		m->state.name = m->name;
		m->state.can_play = m->state.can_pause = true;
		list_add(ms.players, m);
	}
	free(m->unique);
	m->unique = unique && *unique ? strdup(unique) : NULL;
	sd_bus_call_method_async(ms.bus, NULL, name, MPRIS_PATH, "org.freedesktop.DBus.Properties",
		"GetAll", got_properties, strdup(name), "s", MPRIS_PLAYER);
	sd_bus_call_method_async(ms.bus, NULL, name, MPRIS_PATH, "org.freedesktop.DBus.Properties",
		"GetAll", got_properties, strdup(name), "s", "org.mpris.MediaPlayer2");
	sd_bus_call_method_async(ms.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixProcessID", got_pid, strdup(name), "s", name);
}

static void remove_player(const char *name) {
	for (int i = 0; ms.players && i < ms.players->length; i++) {
		struct media *m = ms.players->items[i];
		if (strcmp(m->name, name) == 0) {
			list_del(ms.players, i);
			media_free(m);
			changed();
			return;
		}
	}
}

static int got_owner(sd_bus_message *msg, void *data, sd_bus_error *error) {
	const char *unique = NULL;
	if (!sd_bus_message_is_method_error(msg, NULL) && sd_bus_message_read(msg, "s", &unique) >= 0) {
		add_player(data, unique);
	}
	free(data);
	return 0;
}

static int got_names(sd_bus_message *msg, void *data, sd_bus_error *error) {
	if (sd_bus_message_is_method_error(msg, NULL) ||
			sd_bus_message_enter_container(msg, 'a', "s") < 0) {
		return 0;
	}
	const char *name = NULL;
	while (sd_bus_message_read(msg, "s", &name) > 0) {
		if (strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) == 0) {
			sd_bus_call_method_async(ms.bus, NULL, "org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", got_owner,
				strdup(name), "s", name);
		}
	}
	sd_bus_message_exit_container(msg);
	return 0;
}

static int handle_owner_changed(sd_bus_message *msg, void *data, sd_bus_error *error) {
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
	if (sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner) < 0 || !name ||
			strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0) {
		return 0;
	}
	if (new_owner && *new_owner) {
		add_player(name, new_owner);
	} else {
		remove_player(name);
	}
	return 0;
}

static int handle_properties(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = find(NULL, sd_bus_message_get_sender(msg));
	const char *iface = NULL;
	if (!m || sd_bus_message_read(msg, "s", &iface) < 0 || !iface ||
			strncmp(iface, "org.mpris.MediaPlayer2", 22) != 0) {
		return 0;
	}
	read_properties(m, msg);
	changed();
	return 0;
}

static void bus_in(int fd, short mask, void *data) {
	int r;
	while ((r = sd_bus_process(ms.bus, NULL)) > 0) {
		// keep processing
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Media players: bus error: %s", strerror(-r));
		loop_remove_fd(ms.panel->loop, ms.fd);
		sd_bus_flush_close_unref(ms.bus);
		ms.bus = NULL;
	}
}

void media_send(const struct media_player *player, const char *method) {
	if (!ms.bus || !player || !player->name) {
		return;
	}
	sd_bus_call_method_async(ms.bus, NULL, player->name, MPRIS_PATH, MPRIS_PLAYER, method,
		NULL, NULL, "");
	sd_bus_flush(ms.bus);
}

void media_init(struct panel *panel) {
	ms.panel = panel;
	if (!ms.players) {
		ms.players = create_list();
	}
	if (ms.bus) {
		return;
	}
	sd_bus *bus = NULL;
	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		sway_log(SWAY_INFO, "Media players: no user bus: %s", strerror(-r));
		return;
	}
	ms.bus = bus;
	sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", handle_owner_changed, NULL);
	sd_bus_match_signal(bus, NULL, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties",
		"PropertiesChanged", handle_properties, NULL);
	sd_bus_call_method_async(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "ListNames", got_names, NULL, "");
	ms.fd = sd_bus_get_fd(bus);
	loop_add_fd(panel->loop, ms.fd, POLLIN, bus_in, NULL);
	bus_in(ms.fd, 0, NULL);
}

#else

void media_send(const struct media_player *player, const char *method) {
}

void media_init(struct panel *panel) {
	ms.panel = panel;
	(void)changed;
}

#endif
