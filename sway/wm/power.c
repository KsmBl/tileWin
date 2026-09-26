/*
 * Screen power, like the power options of Windows:
 *
 *   idle_timeout dim|screen_off|lock|sleep|screensaver <seconds>
 *       after that long without input the screen turns darker, turns off,
 *       gets locked (lock_command), the computer goes to sleep or the screen
 *       saver starts (screensaver_command, tilewin-screensaver by default,
 *       which runs until input comes and is then ended; with
 *       "screensaver_lock yes" the screen is locked then); 0 is never.
 *       Apps that keep the screen on (idle inhibitors, e.g. a video) pause
 *       the timers.
 *   lid_action closed|docked <action>
 *       what closing the laptop lid does, without and with an external
 *       monitor: default (leave it to logind), nothing, sleep (stand by in
 *       RAM), hibernate (to disk), hybrid_sleep (both), lock, screen_off or
 *       shutdown. As soon as one is not "default", tileWin takes logind's
 *       handle-lid-switch inhibitor lock and does all lid handling itself.
 *   power_key_action <action>
 *       the same for the power key, with logind's handle-power-key lock.
 *   lock_on_sleep yes|no
 *       lock the session before the computer sleeps or hibernates, however
 *       that was asked for (the power key, the lid, a menu, systemctl): a
 *       delay inhibitor holds logind back until the lock screen is up.
 *
 * Input only records a timestamp; a single timer wakes up at the next
 * deadline and checks the time since the last input.
 */
#define _DEFAULT_SOURCE // syscall()
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/syscall.h>
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

static const char *const stage_names[TW_IDLE_STAGES] = { "dim", "screen_off", "lock", "sleep",
	"screensaver" };
static const char *const action_names[] = { "default", "nothing", "sleep", "hibernate",
	"hybrid_sleep", "lock", "screen_off", "shutdown", NULL };

// how long logind may wait for the lock screen before the computer sleeps anyway
#define SLEEP_LOCK_TIMEOUT_MS 3000
// after the lock is up, so the lock screen is drawn before the picture freezes
#define SLEEP_LOCK_SETTLE_MS 300

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
	int saver_fd;      // pidfd of the running screen saver, -1 without one
	pid_t saver_pid;   // and its process group
	struct timespec saver_started;
	bool saver_by_hand; // "screensaver start": the keys of that do not end it
	int key_inhibit_fd;
	int sleep_inhibit_fd; // delay lock, held while awake with lock_on_sleep
	struct wl_event_source *sleep_timer; // waits for the lock screen
	int sleep_waited_ms;
	bool sleep_locked; // the lock came up, settling
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	sd_bus *bus;
	struct wl_event_source *bus_source;
	sd_bus_slot *sleep_slot;
#endif
} power = { .lid_inhibit_fd = -1, .saver_fd = -1, .key_inhibit_fd = -1, .sleep_inhibit_fd = -1 };

bool tw_idle_stage_parse(const char *name, enum tw_idle_stage *stage) {
	for (int i = 0; i < TW_IDLE_STAGES; i++) {
		if (strcasecmp(name, stage_names[i]) == 0) {
			*stage = i;
			return true;
		}
	}
	return false;
}

bool tw_power_action_parse(const char *name, enum tw_power_action *action) {
	for (int i = 0; action_names[i]; i++) {
		if (strcasecmp(name, action_names[i]) == 0) {
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

/* ---------- screen saver ---------- */

#define SAVER_GRACE_MS 1000 // after "screensaver start", for the keys that ran it

static void saver_launch(void) {
	if (power.saver_fd >= 0) {
		return;
	}
	const char *command = config && config->tw_screensaver_command ?
		config->tw_screensaver_command : "tilewin-screensaver";
	pid_t pid = fork();
	if (pid < 0) {
		return;
	}
	if (pid == 0) {
		setsid(); // a group of its own, which is ended as a whole
		execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	// a pidfd, so a finished saver's number taken over by another process is never hit
	power.saver_fd = (int)syscall(SYS_pidfd_open, pid, 0);
	power.saver_pid = pid;
	clock_gettime(CLOCK_MONOTONIC, &power.saver_started);
	sway_log(SWAY_DEBUG, "Screen saver started: %s", command);
}

/* Ends the screen saver; true when one was showing. */
static bool saver_end(void) {
	if (power.saver_fd < 0) {
		return false;
	}
	if (syscall(SYS_pidfd_send_signal, power.saver_fd, 0, NULL, 0) == 0) {
		kill(-power.saver_pid, SIGTERM);
	}
	close(power.saver_fd);
	power.saver_fd = -1;
	power.saver_by_hand = false;
	return true;
}

static void run_stage(enum tw_idle_stage stage) {
	switch (stage) {
	case TW_IDLE_DIM:
		dim();
		break;
	case TW_IDLE_SCREEN_OFF:
		saver_end(); // nobody sees it any more
		run_command("output * power off");
		power.idle_screen_off = true;
		break;
	case TW_IDLE_LOCK:
		saver_end();
		spawn(config->tw_lock_command);
		break;
	case TW_IDLE_SLEEP:
		saver_end();
		spawn("systemctl suspend");
		break;
	case TW_IDLE_SCREENSAVER:
		if (!power.idle_screen_off) {
			saver_launch();
		}
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

/* Undoes dimming, the screen saver and turning the screen off after input. */
static void wake(void) {
	if (saver_end() && config && config->tw_screensaver_lock) {
		spawn(config->tw_lock_command); // "on resume, display the logon screen"
	}
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
	if (power.saver_by_hand && power.saver_fd >= 0) {
		long since = (power.last_input.tv_sec - power.saver_started.tv_sec) * 1000 +
			(power.last_input.tv_nsec - power.saver_started.tv_nsec) / 1000000;
		if (since < SAVER_GRACE_MS) {
			return; // letting go of the keys that started it
		}
	}
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

/* ---------- logind ---------- */

#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
static int handle_bus(int fd, uint32_t mask, void *data) {
	while (power.bus && sd_bus_process(power.bus, NULL) > 0) {
		// one message per call
	}
	return 0;
}

/* The system bus, read from the event loop so logind's signals arrive. */
static sd_bus *system_bus(void) {
	if (power.bus) {
		return power.bus;
	}
	if (sd_bus_open_system(&power.bus) < 0) {
		sway_log(SWAY_ERROR, "Cannot connect to the system bus");
		power.bus = NULL;
		return NULL;
	}
	power.bus_source = wl_event_loop_add_fd(server.wl_event_loop, sd_bus_get_fd(power.bus),
		WL_EVENT_READABLE, handle_bus, NULL);
	return power.bus;
}

/* Takes a logind inhibitor lock; -1 when logind refuses. */
static int inhibit(const char *what, const char *why, const char *mode) {
	sd_bus *bus = system_bus();
	if (!bus) {
		return -1;
	}
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
		what, "tileWin", why, mode);
	int fd = -1, result = -1;
	if (ret < 0) {
		sway_log(SWAY_ERROR, "logind refused the %s inhibitor: %s", what,
			error.message ? error.message : strerror(-ret));
	} else if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
		// the message owns fd
		result = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	// signals that came in during the call wait in the queue
	handle_bus(-1, 0, NULL);
	return result;
}
#else
static int inhibit(const char *what, const char *why, const char *mode) {
	sway_log(SWAY_ERROR, "%s needs tileWin built with sd-bus; logind keeps handling it", what);
	return -1;
}
#endif

/* Takes or lets go of an inhibitor lock, so *fd >= 0 when want. */
static void set_inhibitor(int *fd, bool want, const char *what, const char *why,
		const char *mode) {
	if (want == (*fd >= 0)) {
		return;
	}
	if (!want) {
		close(*fd);
		*fd = -1;
		sway_log(SWAY_INFO, "logind handles %s again", what);
		return;
	}
	*fd = inhibit(what, why, mode);
	if (*fd >= 0) {
		sway_log(SWAY_INFO, "%s", why);
	}
}

/* ---------- actions ---------- */

/* Whether logind can do a sleep verb (CanHibernate etc.); true when there is nobody to ask. */
static bool logind_can(const char *method) {
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	sd_bus *bus = system_bus();
	if (!bus) {
		return true;
	}
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	const char *answer = NULL;
	bool can = true;
	if (sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", method, &error, &reply, "") >= 0 &&
			sd_bus_message_read(reply, "s", &answer) >= 0 && answer) {
		// "challenge" asks for a password, which systemctl does
		can = strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0;
		if (!can) {
			sway_log(SWAY_ERROR, "logind says %s: %s", method, answer);
		}
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	handle_bus(-1, 0, NULL);
	return can;
#else
	return true;
#endif
}

/* Hibernate or hybrid sleep, or sleep when the computer cannot save to disk (no swap
 * partition or file big enough, no resume device, secure boot lockdown...), so the key
 * or the lid still does something and the work stays in memory. The "||" catches a
 * refusal logind did not foresee. */
static void sleep_to_disk(const char *method, const char *verb) {
	if (!logind_can(method)) {
		sway_log(SWAY_ERROR, "This computer cannot %s, sleeping instead", verb);
		spawn("systemctl suspend");
		return;
	}
	char *cmd = format_str("systemctl %s || systemctl suspend", verb);
	spawn(cmd);
	free(cmd);
}

/* The actions the lid and the power key share; false for screen_off, which differs. */
static bool run_action(enum tw_power_action action) {
	switch (action) {
	case TW_POWER_DEFAULT:
	case TW_POWER_NOTHING:
		return true;
	case TW_POWER_SLEEP:
		spawn("systemctl suspend");
		return true;
	case TW_POWER_HIBERNATE:
		sleep_to_disk("CanHibernate", "hibernate");
		return true;
	case TW_POWER_HYBRID_SLEEP:
		sleep_to_disk("CanHybridSleep", "hybrid-sleep");
		return true;
	case TW_POWER_LOCK:
		spawn(config->tw_lock_command);
		return true;
	case TW_POWER_SHUTDOWN:
		spawn("systemctl poweroff");
		return true;
	case TW_POWER_SCREEN_OFF:
		break;
	}
	return false;
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
	enum tw_power_action action = config->tw_lid_action[docked ? 1 : 0];
	if (action == TW_POWER_DEFAULT) {
		// what logind does when it is not inhibited
		action = docked ? TW_POWER_NOTHING : TW_POWER_SLEEP;
	}
	sway_log(SWAY_DEBUG, "Lid closed%s: %s", docked ? " (docked)" : "", action_names[action]);
	if (run_action(action)) {
		return;
	}
	// screen_off
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
}

/* ---------- power key ---------- */

bool tw_power_key(bool pressed) {
	if (!config || power.key_inhibit_fd < 0) {
		return false; // logind does it
	}
	if (!pressed) {
		return true;
	}
	enum tw_power_action action = config->tw_power_key_action;
	sway_log(SWAY_DEBUG, "Power key: %s", action_names[action]);
	if (!run_action(action)) {
		// screen_off: like idling, the next input turns the screens on again
		power.idle_screen_off = true;
		run_command("output * power off");
	}
	return true;
}

/* ---------- lock before sleep ---------- */

static void release_sleep_inhibitor(void) {
	if (power.sleep_timer) {
		wl_event_source_remove(power.sleep_timer);
		power.sleep_timer = NULL;
	}
	if (power.sleep_inhibit_fd >= 0) {
		close(power.sleep_inhibit_fd);
		power.sleep_inhibit_fd = -1;
	}
}

static int handle_sleep_timer(void *data) {
	if (power.sleep_locked) {
		sway_log(SWAY_DEBUG, "Locked, the computer may sleep");
		release_sleep_inhibitor();
		return 0;
	}
	if (server.session_lock.lock) {
		power.sleep_locked = true;
		wl_event_source_timer_update(power.sleep_timer, SLEEP_LOCK_SETTLE_MS);
		return 0;
	}
	power.sleep_waited_ms += 50;
	if (power.sleep_waited_ms >= SLEEP_LOCK_TIMEOUT_MS) {
		sway_log(SWAY_ERROR, "The lock screen did not come up, sleeping unlocked");
		release_sleep_inhibitor();
		return 0;
	}
	wl_event_source_timer_update(power.sleep_timer, 50);
	return 0;
}

static void update_sleep_inhibitor(void);

#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
static int handle_prepare_for_sleep(sd_bus_message *msg, void *data, sd_bus_error *error) {
	int start = 0;
	if (sd_bus_message_read(msg, "b", &start) < 0) {
		return 0;
	}
	if (!start) {
		// woke up, or the sleep was called off: hold logind back again for the next time
		release_sleep_inhibitor();
		update_sleep_inhibitor();
		return 0;
	}
	if (power.sleep_inhibit_fd < 0 || power.sleep_timer) {
		return 0;
	}
	if (server.session_lock.lock) {
		release_sleep_inhibitor();
		return 0;
	}
	sway_log(SWAY_DEBUG, "Going to sleep: locking first");
	spawn(config->tw_lock_command);
	power.sleep_waited_ms = 0;
	power.sleep_locked = false;
	power.sleep_timer = wl_event_loop_add_timer(server.wl_event_loop, handle_sleep_timer, NULL);
	if (!power.sleep_timer) {
		release_sleep_inhibitor();
		return 0;
	}
	wl_event_source_timer_update(power.sleep_timer, 50);
	return 0;
}
#endif

static void update_sleep_inhibitor(void) {
	bool want = config && config->tw_lock_on_sleep;
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	if (want && !power.sleep_slot && system_bus()) {
		if (sd_bus_match_signal(power.bus, &power.sleep_slot, "org.freedesktop.login1",
				"/org/freedesktop/login1", "org.freedesktop.login1.Manager",
				"PrepareForSleep", handle_prepare_for_sleep, NULL) < 0) {
			sway_log(SWAY_ERROR, "Cannot watch logind for sleep; not locking before it");
			return;
		}
	}
#endif
	if (!want) {
		release_sleep_inhibitor();
		return;
	}
	if (power.sleep_timer) {
		return; // on the way to sleep
	}
	set_inhibitor(&power.sleep_inhibit_fd, true, "sleep",
		"tileWin locks the screen before sleeping", "delay");
}

void tw_power_config_changed(void) {
	set_inhibitor(&power.lid_inhibit_fd, config && (config->tw_lid_action[0] != TW_POWER_DEFAULT ||
		config->tw_lid_action[1] != TW_POWER_DEFAULT), "handle-lid-switch",
		"tileWin handles closing the lid", "block");
	set_inhibitor(&power.key_inhibit_fd, config && config->tw_power_key_action != TW_POWER_DEFAULT,
		"handle-power-key", "tileWin handles the power key", "block");
	update_sleep_inhibitor();
	if (power.last_input.tv_sec == 0 && power.last_input.tv_nsec == 0) {
		clock_gettime(CLOCK_MONOTONIC, &power.last_input);
	}
	schedule();
}

void tw_screensaver_start(void) {
	if (power.saver_fd >= 0) {
		return;
	}
	saver_launch();
	if (power.saver_fd >= 0) {
		power.saver_by_hand = true;
		power.done[TW_IDLE_SCREENSAVER] = true; // so the next input ends it
	}
}

void tw_screensaver_stop(void) {
	saver_end();
	power.done[TW_IDLE_SCREENSAVER] = false;
}

void tw_power_fini(void) {
	saver_end();
	undim();
	if (power.timer) {
		wl_event_source_remove(power.timer);
		power.timer = NULL;
	}
	int *fds[] = { &power.lid_inhibit_fd, &power.key_inhibit_fd };
	for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
		if (*fds[i] >= 0) {
			close(*fds[i]);
			*fds[i] = -1;
		}
	}
	release_sleep_inhibitor();
#if HAVE_LIBSYSTEMD || HAVE_LIBELOGIND || HAVE_BASU
	power.sleep_slot = sd_bus_slot_unref(power.sleep_slot);
	if (power.bus_source) {
		wl_event_source_remove(power.bus_source);
		power.bus_source = NULL;
	}
	power.bus = sd_bus_flush_close_unref(power.bus);
#endif
	free(power.lid_disabled_output);
	power.lid_disabled_output = NULL;
}
