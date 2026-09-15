/*
 * Screen power, like the power options of Windows:
 *
 *   idle_timeout dim|screen_off|lock|sleep <seconds>
 *       after that long without input the screen turns darker, turns off,
 *       gets locked (lock_command) or the computer goes to sleep; 0 is never.
 *       Apps that keep the screen on (idle inhibitors, e.g. a video) pause
 *       the timers.
 *   lid_action closed|docked <action>
 *       what closing the laptop lid does, without and with an external
 *       monitor: default (leave it to logind), nothing, sleep, hibernate,
 *       lock, screen_off or shutdown. As soon as one is not "default",
 *       tileWin takes logind's handle-lid-switch inhibitor lock and does all
 *       lid handling itself.
 *
 * Input only records a timestamp; a single timer wakes up at the next
 * deadline and checks the time since the last input.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "config.h"
#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/root.h"
#include "list.h"
#include "log.h"

static const char *const stage_names[TW_IDLE_STAGES] = { "dim", "screen_off", "lock", "sleep" };
static const char *const lid_names[] = { "default", "nothing", "sleep", "hibernate", "lock",
	"screen_off", "shutdown", NULL };

static struct {
	struct wl_event_source *timer;
	struct timespec last_input;
	bool inhibited;
	bool done[TW_IDLE_STAGES]; // the stage ran since the last input
	struct wlr_scene_tree *dim;
	bool idle_screen_off;
	bool lid_closed;
	char *lid_disabled_output; // internal screen turned off while docked
	bool lid_screen_off;
	int lid_inhibit_fd;
} power = { .lid_inhibit_fd = -1 };

bool tw_idle_stage_parse(const char *name, enum tw_idle_stage *stage) {
	for (int i = 0; i < TW_IDLE_STAGES; i++) {
		if (strcasecmp(name, stage_names[i]) == 0) {
			*stage = i;
			return true;
		}
	}
	return false;
}

bool tw_lid_action_parse(const char *name, enum tw_lid_action *action) {
	for (int i = 0; lid_names[i]; i++) {
		if (strcasecmp(name, lid_names[i]) == 0) {
			*action = i;
			return true;
		}
	}
	return false;
}

static void spawn(const char *command) {
	if (!command || !*command) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

static void run_command(const char *command) {
	char *copy = strdup(command);
	list_t *results = execute_command(copy, NULL, NULL);
	while (results && results->length > 0) {
		struct cmd_results *res = results->items[0];
		if (res->status != CMD_SUCCESS) {
			sway_log(SWAY_ERROR, "'%s' failed: %s", command, res->error ? res->error : "?");
		}
		free_cmd_results(res);
		list_del(results, 0);
	}
	list_free(results);
	free(copy);
}

/* ---------- idle ---------- */

static long ms_since_input(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - power.last_input.tv_sec) * 1000 +
		(now.tv_nsec - power.last_input.tv_nsec) / 1000000;
}

static void undim(void) {
	if (power.dim) {
		wlr_scene_node_destroy(&power.dim->node);
		power.dim = NULL;
	}
}

static void dim(void) {
	undim();
	power.dim = wlr_scene_tree_create(root->layer_tree);
	if (!power.dim) {
		return;
	}
	const float color[4] = { 0, 0, 0, 0.55f };
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		struct wlr_scene_rect *rect = wlr_scene_rect_create(power.dim,
			output->width, output->height, color);
		if (rect) {
			wlr_scene_node_set_position(&rect->node, output->lx, output->ly);
		}
	}
}

static void run_stage(enum tw_idle_stage stage) {
	switch (stage) {
	case TW_IDLE_DIM:
		dim();
		break;
	case TW_IDLE_SCREEN_OFF:
		run_command("output * power off");
		power.idle_screen_off = true;
		break;
	case TW_IDLE_LOCK:
		spawn(config->tw_lock_command);
		break;
	case TW_IDLE_SLEEP:
		spawn("systemctl suspend");
		break;
	case TW_IDLE_STAGES:
		break;
	}
}

static int handle_timer(void *data);

static void schedule(void) {
	if (!power.timer) {
		power.timer = wl_event_loop_add_timer(server.wl_event_loop, handle_timer, NULL);
		if (!power.timer) {
			return;
		}
	}
	long next = 0;
	if (config && !power.inhibited) {
		long elapsed = ms_since_input();
		for (int i = 0; i < TW_IDLE_STAGES; i++) {
			int seconds = config->tw_idle_timeout[i];
			if (seconds <= 0 || power.done[i]) {
				continue;
			}
			long due = seconds * 1000L - elapsed;
			due = due < 1 ? 1 : due;
			next = next == 0 || due < next ? due : next;
		}
	}
	wl_event_source_timer_update(power.timer, next);
}

static int handle_timer(void *data) {
	long elapsed = ms_since_input();
	for (int i = 0; config && !power.inhibited && i < TW_IDLE_STAGES; i++) {
		int seconds = config->tw_idle_timeout[i];
		if (seconds > 0 && !power.done[i] && elapsed >= seconds * 1000L) {
			power.done[i] = true;
			sway_log(SWAY_DEBUG, "Idle for %lds: %s", elapsed / 1000, stage_names[i]);
			run_stage(i);
		}
	}
	schedule();
	return 0;
}

/* Undoes dimming and turning the screen off after input. */
static void wake(void) {
	undim();
	if (power.idle_screen_off) {
		power.idle_screen_off = false;
		if (!power.lid_screen_off) {
			run_command("output * power on");
		}
	}
	memset(power.done, 0, sizeof(power.done));
}

void tw_power_activity(void) {
	clock_gettime(CLOCK_MONOTONIC, &power.last_input);
	for (int i = 0; i < TW_IDLE_STAGES; i++) {
		if (power.done[i]) {
			wake();
			schedule();
			return;
		}
	}
	// otherwise the timer notices the new input time when it fires
}

void tw_power_set_inhibited(bool inhibited) {
	if (inhibited == power.inhibited) {
		return;
	}
	power.inhibited = inhibited;
	if (inhibited) {
		wake();
	}
	clock_gettime(CLOCK_MONOTONIC, &power.last_input);
	schedule();
}

/* ---------- lid ---------- */

static bool internal_output_name(const char *name) {
	return strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0 ||
		strncmp(name, "DSI", 3) == 0;
}

static struct sway_output *internal_output(bool *docked) {
	struct sway_output *internal = NULL;
	*docked = false;
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		if (internal_output_name(output->wlr_output->name)) {
			internal = output;
		} else if (output->enabled) {
			*docked = true;
		}
	}
	return internal;
}

static void update_lid_inhibitor(void) {
	bool want = config && (config->tw_lid_action[0] != TW_LID_DEFAULT ||
		config->tw_lid_action[1] != TW_LID_DEFAULT);
	if (want == (power.lid_inhibit_fd >= 0)) {
		return;
	}
	if (!want) {
		close(power.lid_inhibit_fd);
		power.lid_inhibit_fd = -1;
		sway_log(SWAY_INFO, "Closing the lid is handled by logind again");
		return;
	}
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	sd_bus *bus = NULL;
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	if (sd_bus_open_system(&bus) < 0) {
		sway_log(SWAY_ERROR, "Cannot connect to the system bus for the lid inhibitor");
		return;
	}
	int ret = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
		"handle-lid-switch", "tileWin", "tileWin handles closing the lid", "block");
	int fd = -1;
	if (ret < 0) {
		sway_log(SWAY_ERROR, "logind refused the lid inhibitor: %s",
			error.message ? error.message : strerror(-ret));
	} else if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
		// the message owns fd
		power.lid_inhibit_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		sway_log(SWAY_INFO, "tileWin handles closing the lid");
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
#else
	sway_log(SWAY_ERROR, "lid_action needs tileWin built with sd-bus; logind keeps handling the lid");
#endif
}

void tw_power_lid(bool closed) {
	if (closed == power.lid_closed) {
		return;
	}
	power.lid_closed = closed;
	if (!config || power.lid_inhibit_fd < 0) {
		return; // logind does it
	}
	if (!closed) {
		if (power.lid_disabled_output) {
			char *cmd = format_str("output \"%s\" enable", power.lid_disabled_output);
			run_command(cmd);
			free(cmd);
			free(power.lid_disabled_output);
			power.lid_disabled_output = NULL;
		}
		if (power.lid_screen_off) {
			power.lid_screen_off = false;
			run_command("output * power on");
		}
		return;
	}
	bool docked;
	struct sway_output *internal = internal_output(&docked);
	enum tw_lid_action action = config->tw_lid_action[docked ? 1 : 0];
	if (action == TW_LID_DEFAULT) {
		// what logind does when it is not inhibited
		action = docked ? TW_LID_NOTHING : TW_LID_SLEEP;
	}
	sway_log(SWAY_DEBUG, "Lid closed%s: %s", docked ? " (docked)" : "", lid_names[action]);
	switch (action) {
	case TW_LID_DEFAULT:
	case TW_LID_NOTHING:
		break;
	case TW_LID_SLEEP:
		spawn("systemctl suspend");
		break;
	case TW_LID_HIBERNATE:
		spawn("systemctl hibernate");
		break;
	case TW_LID_LOCK:
		spawn(config->tw_lock_command);
		break;
	case TW_LID_SCREEN_OFF:
		if (docked && internal && internal->enabled) {
			free(power.lid_disabled_output);
			power.lid_disabled_output = strdup(internal->wlr_output->name);
			char *cmd = format_str("output \"%s\" disable", internal->wlr_output->name);
			run_command(cmd);
			free(cmd);
		} else {
			power.lid_screen_off = true;
			run_command("output * power off");
		}
		break;
	case TW_LID_SHUTDOWN:
		spawn("systemctl poweroff");
		break;
	}
}

void tw_power_config_changed(void) {
	update_lid_inhibitor();
	if (power.last_input.tv_sec == 0 && power.last_input.tv_nsec == 0) {
		clock_gettime(CLOCK_MONOTONIC, &power.last_input);
	}
	schedule();
}

void tw_power_fini(void) {
	undim();
	if (power.timer) {
		wl_event_source_remove(power.timer);
		power.timer = NULL;
	}
	if (power.lid_inhibit_fd >= 0) {
		close(power.lid_inhibit_fd);
		power.lid_inhibit_fd = -1;
	}
	free(power.lid_disabled_output);
	power.lid_disabled_output = NULL;
}
