/*
 * The sched_ext scheduler while a window fills a screen. With
 * `fullscreen_scheduler scx_lavd` in taskbar.conf (the Kernel page of the
 * settings writes it), that scheduler of scx_loader runs while a window on a
 * workspace that is shown is fullscreen, in `fullscreen_scheduler_mode`
 * (Gaming unless it says otherwise: Auto, Gaming, PowerSave, LowLatency or
 * Server). Once no window is, the scheduler that ran before comes back, or
 * none. It all goes through scx_loader on the system bus;
 * org.tilewin.scx.rules lets administrators at the computer do that without
 * a password.
 *
 * For tests: TILEWIN_SCX_BUS=session talks to a scx_loader on the session bus.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <systemd/sd-bus.h>
#include "log.h"
#include "panel.h"

#define SCX_NAME "org.scx.Loader"
#define SCX_PATH "/org/scx/Loader"
#define SCX_IFACE "org.scx.Loader"

static struct {
	struct panel *panel;
	sd_bus *bus;
	int fd;
	bool active;    // ours runs, for a window that fills a screen
	bool busy;      // a call is on its way; update() again once it is back
	bool again;
	char *previous; // what ran before ours, NULL for none
	uint32_t previous_mode;
	char *ours;     // the one we started
} sc = { .fd = -1 };

static void update(void);

static uint32_t mode_number(const char *name) {
	static const char *const modes[] = { "auto", "gaming", "powersave", "lowlatency", "server" };
	for (uint32_t i = 0; name && i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcasecmp(name, modes[i]) == 0) {
			return i;
		}
	}
	return 1; // Gaming
}

static void bus_in(int fd, short mask, void *data) {
	int r;
	while ((r = sd_bus_process(sc.bus, NULL)) > 0) {
		// keep processing
	}
	if (r < 0) {
		sway_log(SWAY_ERROR, "Scheduler: bus error: %s", strerror(-r));
		loop_remove_fd(sc.panel->loop, sc.fd);
		sd_bus_flush_close_unref(sc.bus);
		sc.bus = NULL;
		sc.fd = -1;
	}
}

static bool connect_bus(void) {
	if (sc.bus) {
		return true;
	}
	const char *which = getenv("TILEWIN_SCX_BUS");
	int r = which && strcmp(which, "session") == 0 ? sd_bus_open_user(&sc.bus) :
		sd_bus_open_system(&sc.bus);
	if (r < 0) {
		sway_log(SWAY_INFO, "Scheduler: no bus: %s", strerror(-r));
		sc.bus = NULL;
		return false;
	}
	sc.fd = sd_bus_get_fd(sc.bus);
	loop_add_fd(sc.panel->loop, sc.fd, POLLIN, bus_in, NULL);
	return true;
}

/* A call has come back: whatever changed meanwhile is looked at now. */
static void done(void) {
	sc.busy = false;
	if (sc.again) {
		sc.again = false;
		update();
	}
}

static int on_reply(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const sd_bus_error *e = sd_bus_message_get_error(m);
	if (e) {
		sway_log(SWAY_ERROR, "Scheduler: %s: %s", (const char *)data, e->message);
	}
	done();
	return 0;
}

static void call(const char *method, const char *what, const char *types, const char *name,
		uint32_t mode) {
	sc.busy = true;
	int r = types ? sd_bus_call_method_async(sc.bus, NULL, SCX_NAME, SCX_PATH, SCX_IFACE, method,
			on_reply, (void *)what, types, name, mode) :
		sd_bus_call_method_async(sc.bus, NULL, SCX_NAME, SCX_PATH, SCX_IFACE, method,
			on_reply, (void *)what, NULL);
	if (r < 0) {
		sway_log(SWAY_ERROR, "Scheduler: %s: %s", what, strerror(-r));
		sc.busy = false;
	}
}

/* What runs now is known: ours goes in its place. */
static int on_current(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *current = NULL;
	uint32_t mode = 0;
	if (!sd_bus_message_is_method_error(m, NULL)) {
		// a{sv} of GetAll
		if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
			while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
				const char *key;
				sd_bus_message_read(m, "s", &key);
				if (strcmp(key, "CurrentScheduler") == 0) {
					sd_bus_message_read(m, "v", "s", &current);
				} else if (strcmp(key, "SchedulerMode") == 0) {
					sd_bus_message_read(m, "v", "u", &mode);
				} else {
					sd_bus_message_skip(m, "v");
				}
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_exit_container(m);
		}
	} else {
		sway_log(SWAY_INFO, "Scheduler: scx_loader did not answer: %s",
			sd_bus_message_get_error(m)->message);
		done();
		return 0;
	}
	free(sc.previous);
	sc.previous = current && *current && strcmp(current, "unknown") != 0 ? strdup(current) : NULL;
	sc.previous_mode = mode;
	const char *want = sc.ours;
	uint32_t want_mode = mode_number(sc.panel->config ?
		sc.panel->config->fullscreen_scheduler_mode : NULL);
	sc.busy = false;
	if (sc.previous && strcmp(sc.previous, want) == 0 && sc.previous_mode == want_mode) {
		// it runs already: nothing to do, and nothing to put back
		free(sc.previous);
		sc.previous = strdup(want);
		done();
		return 0;
	}
	sway_log(SWAY_INFO, "Scheduler: %s for a window that fills a screen", want);
	call(sc.previous ? "SwitchScheduler" : "StartScheduler", "starting the scheduler", "su",
		want, want_mode);
	return 0;
}

static bool fullscreen_shown(struct panel *panel) {
	list_t *windows = panel->state.windows, *spaces = panel->state.workspaces;
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (!w->fullscreen || w->minimized) {
			continue;
		}
		for (int j = 0; spaces && j < spaces->length; j++) {
			struct pworkspace *ws = spaces->items[j];
			if (ws->visible && w->workspace && strcmp(ws->name, w->workspace) == 0) {
				return true;
			}
		}
	}
	return false;
}

static void update_now(void);

/* Sends what update_now() asked for; inside a reply, sd_bus_process does. */
static void update(void) {
	update_now();
	if (sc.bus) {
		sd_bus_flush(sc.bus);
	}
}

static void update_now(void) {
	struct panel *panel = sc.panel;
	if (!panel) {
		return;
	}
	if (sc.busy) {
		sc.again = true;
		return;
	}
	const char *configured = panel->config ? panel->config->fullscreen_scheduler : NULL;
	bool want = configured && *configured && fullscreen_shown(panel);
	if (want && !sc.active) {
		if (!connect_bus()) {
			return;
		}
		free(sc.ours);
		sc.ours = strdup(configured);
		sc.active = true;
		sc.busy = true;
		int r = sd_bus_call_method_async(sc.bus, NULL, SCX_NAME, SCX_PATH,
			"org.freedesktop.DBus.Properties", "GetAll", on_current, NULL, "s", SCX_IFACE);
		if (r < 0) {
			sway_log(SWAY_ERROR, "Scheduler: asking scx_loader: %s", strerror(-r));
			sc.busy = false;
			sc.active = false;
		}
	} else if (want && sc.active && sc.ours && strcmp(sc.ours, configured) != 0) {
		// another scheduler was chosen while the window is there
		free(sc.ours);
		sc.ours = strdup(configured);
		call("SwitchScheduler", "starting the scheduler", "su", sc.ours,
			mode_number(panel->config->fullscreen_scheduler_mode));
	} else if (!want && sc.active) {
		sc.active = false;
		if (!sc.bus) {
			return;
		}
		if (sc.previous && sc.ours && strcmp(sc.previous, sc.ours) == 0) {
			// it ran before the window came: it stays
		} else if (sc.previous) {
			sway_log(SWAY_INFO, "Scheduler: back to %s", sc.previous);
			call("SwitchScheduler", "going back to the scheduler of before", "su",
				sc.previous, sc.previous_mode);
		} else {
			sway_log(SWAY_INFO, "Scheduler: back to the kernel's own");
			call("StopScheduler", "stopping the scheduler", NULL, NULL, 0);
		}
	}
}

void scheduler_init(struct panel *panel) {
	sc.panel = panel;
}

void scheduler_update(struct panel *panel) {
	if (sc.panel == panel) {
		update();
	}
}

void scheduler_fini(void) {
	// a game left fullscreen when the panel ends: the scheduler of before
	if (sc.active && sc.bus) {
		sc.active = false;
		if (sc.previous && sc.ours && strcmp(sc.previous, sc.ours) == 0) {
			// it ran before
		} else if (sc.previous) {
			sd_bus_call_method(sc.bus, SCX_NAME, SCX_PATH, SCX_IFACE, "SwitchScheduler", NULL,
				NULL, "su", sc.previous, sc.previous_mode);
		} else {
			sd_bus_call_method(sc.bus, SCX_NAME, SCX_PATH, SCX_IFACE, "StopScheduler", NULL,
				NULL, NULL);
		}
	}
	if (sc.bus) {
		loop_remove_fd(sc.panel->loop, sc.fd);
		sd_bus_flush_close_unref(sc.bus);
		sc.bus = NULL;
	}
	free(sc.previous);
	free(sc.ours);
	sc.previous = sc.ours = NULL;
	sc.panel = NULL;
}
