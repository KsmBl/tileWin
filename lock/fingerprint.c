/*
 * The fingerprint reader on the lock screen, as Windows Hello has it: while the
 * session is locked, fprintd listens for a finger of the user's, from the clock
 * on, and a match unlocks the session as the right password does. The password
 * works all the time alongside it.
 *
 * Talks to fprintd (net.reactivated.Fprint) on the system bus with sd-bus, every
 * call asynchronous so the lock screen keeps drawing while the reader is
 * opened: the default device; the user's enrolled fingers (none: nothing to
 * listen for); the bus name that owns fprintd, whose signals alone count;
 * Claim; VerifyStart("any"). Each VerifyStatus that is not a match says why
 * (not recognised, try again, put it in the middle) and listening starts again;
 * after MAX_MISSES fingers that were not recognised in a row (within MISS_WINDOW_S
 * of each other) it stops, and only the password unlocks until a key is pressed
 * after a while. When the lock screen goes, fprintd lets go of the reader by
 * itself.
 *
 * A lock that stays for hours keeps the reader working: when fprintd ends or
 * starts again (a crash, an update, a restart around a suspend) all is asked
 * again of the new one; after the computer wakes, and every REFRESH_S without
 * a word from the reader, listening starts afresh (a reader that went to sleep
 * with the USB port says nothing any more); and what failed on fprintd's side
 * (an error of the reader, a VerifyStart refused, the reader gone) is tried
 * again by itself after RETRY_AFTER_S, without a key.
 *
 * "no" in ~/.config/tileWin/lock-fingerprint (the switch on the Fingerprint
 * page of the settings) leaves the reader alone. TILEWIN_FPRINT_BUS=session
 * looks for fprintd on the session bus instead: the tests run a fake one there.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "fingerprint.h"
#include "tw_paths.h"

#if HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#endif

#define FPRINT "net.reactivated.Fprint"
#define FPRINT_DEVICE "net.reactivated.Fprint.Device"
#define MAX_MISSES 5
#define MISS_WINDOW_S 300 // misses further apart than this start counting again
#define MAX_ERRORS 3
#define RETRY_AFTER_S 30 // a key pressed this long after giving up listens again
#define REFRESH_S 600    // listening started afresh after this long without a word

static struct {
	sd_bus *bus;
	sd_bus_slot *status_slot, *owner_slot, *sleep_slot;
	struct fp_callbacks cb;
	char *user, *device, *owner;
	bool claimed, listening, stopping;
	int misses, errors;
	time_t gave_up, last_miss;
	time_t again_at; // when to try again by itself (0: not), after an error or a new fprintd
	bool again_all;  // ... from the start (a new fprintd), not only VerifyStart
	time_t heard;    // when the reader last said something or listening started
	int retry_s, refresh_s;
} fp;

static void restart_listening(void);

static void set_listening(bool on) {
	if (fp.listening != on) {
		fp.listening = on;
		if (fp.cb.changed) {
			fp.cb.changed();
		}
	}
}

static void say(const char *text) {
	if (fp.cb.message) {
		fp.cb.message(text);
	}
}

static bool turned_off(void) {
	char *dir = tw_config_dir();
	char path[4096];
	snprintf(path, sizeof(path), "%s/lock-fingerprint", dir ? dir : ".");
	free(dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[16] = "";
	bool off = fgets(line, sizeof(line), f) && strncmp(line, "no", 2) == 0;
	fclose(f);
	return off;
}

static int call(const char *path, const char *iface, const char *method,
		sd_bus_message_handler_t done, const char *types, ...);

/* Tries again by itself after a while (an error on fprintd's side). */
static void again_later(bool all) {
	fp.again_at = time(NULL) + fp.retry_s;
	fp.again_all = fp.again_all || all;
}

static int on_verify_started(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	if (fp.stopping) {
		return 0;
	}
	if (sd_bus_message_is_method_error(m, NULL)) {
		set_listening(false);
		const sd_bus_error *e = sd_bus_message_get_error(m);
		// a claim lost (fprintd started again between): claim it again
		again_later(e && e->name && strstr(e->name, "ClaimDevice"));
		return 0;
	}
	fp.heard = time(NULL);
	set_listening(true);
	return 0;
}

static void start_verify(void) {
	if (fp.stopping || !fp.claimed) {
		return;
	}
	call(fp.device, FPRINT_DEVICE, "VerifyStart", on_verify_started, "s", "any");
}

static int on_verify_stopped(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	start_verify(); // the next try
	return 0;
}

/* Stops listening: until a key is pressed (fingers not recognised), or tried
 * again by itself (the reader's fault). */
static void give_up(const char *why, bool by_itself) {
	set_listening(false);
	fp.gave_up = time(NULL);
	call(fp.device, FPRINT_DEVICE, "VerifyStop", NULL, "");
	say(why);
	if (by_itself) {
		again_later(!fp.claimed);
	}
}

static int on_status(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	// only fprintd's own: no other program on the bus may unlock the session
	const char *sender = sd_bus_message_get_sender(m);
	if (!fp.owner || !sender || strcmp(sender, fp.owner) != 0) {
		return 0;
	}
	const char *result = NULL;
	int done = 0;
	if (sd_bus_message_read(m, "sb", &result, &done) < 0 || !result) {
		return 0;
	}
	fp.heard = time(NULL);
	if (strcmp(result, "verify-unknown-error") != 0) {
		fp.errors = 0; // errors count in a row
	}
	if (strcmp(result, "verify-match") == 0) {
		fp.stopping = true;
		set_listening(false);
		if (fp.cb.matched) {
			fp.cb.matched();
		}
		return 0;
	}
	static const struct {
		const char *result, *text;
	} hints[] = {
		{ "verify-no-match", "That fingerprint was not recognized. Try again." },
		{ "verify-retry-scan", "Try that finger again." },
		{ "verify-swipe-too-short", "Swipe the finger again, a little slower." },
		{ "verify-finger-not-centered", "Put the finger in the middle of the reader." },
		{ "verify-remove-and-retry", "Lift the finger and try again." },
	};
	if (strcmp(result, "verify-disconnected") == 0) {
		fp.claimed = false;
		give_up("The fingerprint reader is gone. Sign in with your password.", true);
		return 0;
	}
	if (strcmp(result, "verify-no-match") == 0) {
		time_t now = time(NULL);
		if (fp.last_miss && now - fp.last_miss > MISS_WINDOW_S) {
			fp.misses = 0; // one long ago does not count with this one
		}
		fp.last_miss = now;
		if (++fp.misses >= MAX_MISSES) {
			give_up("Too many fingers that were not recognized. Sign in with your password.",
				false);
			return 0;
		}
	}
	if (strcmp(result, "verify-unknown-error") == 0 && ++fp.errors >= MAX_ERRORS) {
		give_up("The fingerprint reader does not work right now. Sign in with your password.",
			true);
		return 0;
	}
	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		if (strcmp(result, hints[i].result) == 0) {
			say(hints[i].text);
		}
	}
	if (done) {
		// this try is over: stop it and listen again
		set_listening(false);
		call(fp.device, FPRINT_DEVICE, "VerifyStop", on_verify_stopped, "");
	}
	return 0;
}

static int on_claimed(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	if (sd_bus_message_is_method_error(m, NULL)) {
		// in use (the settings enrolling a finger): the password only, for a while
		again_later(true);
		return 0;
	}
	fp.claimed = true;
	start_verify();
	return 0;
}

static int on_owner(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *owner = NULL;
	if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "s", &owner) < 0 ||
			!owner) {
		return 0;
	}
	free(fp.owner);
	fp.owner = strdup(owner);
	sd_bus_slot_unref(fp.status_slot);
	fp.status_slot = NULL;
	sd_bus_match_signal_async(fp.bus, &fp.status_slot, fp.owner, fp.device, FPRINT_DEVICE,
		"VerifyStatus", on_status, NULL, NULL);
	call(fp.device, FPRINT_DEVICE, "Claim", on_claimed, "s", fp.user);
	return 0;
}

static int on_fingers(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	if (sd_bus_message_is_method_error(m, NULL)) {
		return 0; // no fingers enrolled (fprintd answers with an error then)
	}
	int count = 0;
	if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
		const char *finger;
		while (sd_bus_message_read(m, "s", &finger) > 0) {
			count++;
		}
		sd_bus_message_exit_container(m);
	}
	if (count == 0) {
		return 0;
	}
	sd_bus_call_method_async(fp.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetNameOwner", on_owner, NULL, "s", FPRINT);
	return 0;
}

static int on_device(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *path = NULL;
	if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "o", &path) < 0 ||
			!path) {
		return 0; // no reader
	}
	free(fp.device);
	fp.device = strdup(path);
	call(fp.device, FPRINT_DEVICE, "ListEnrolledFingers", on_fingers, "s", fp.user);
	return 0;
}

static int call(const char *path, const char *iface, const char *method,
		sd_bus_message_handler_t done, const char *types, ...) {
	if (!fp.bus || !path) {
		return -1;
	}
	sd_bus_message *m = NULL;
	int r = sd_bus_message_new_method_call(fp.bus, &m, FPRINT, path, iface, method);
	if (r >= 0 && types && *types) {
		va_list args;
		va_start(args, types);
		r = sd_bus_message_appendv(m, types, args);
		va_end(args);
	}
	if (r >= 0) {
		// the reader may take its time to open: a long timeout
		r = sd_bus_call_async(fp.bus, NULL, m, done, NULL, 60 * 1000 * 1000ULL);
	}
	sd_bus_message_unref(m);
	return r;
}

static void begin(void) {
	fp.misses = fp.errors = 0;
	fp.stopping = false;
	fp.claimed = false;
	set_listening(false);
	sd_bus_call_method_async(fp.bus, NULL, FPRINT, "/net/reactivated/Fprint/Manager",
		"net.reactivated.Fprint.Manager", "GetDefaultDevice", on_device, NULL, "");
}

/* fprintd ended or another one took its name: what was asked of the old one is gone. */
static int on_owner_changed(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	const char *name = NULL, *old = NULL, *new = NULL;
	if (fp.stopping || sd_bus_message_read(m, "sss", &name, &old, &new) < 0 || !name ||
			strcmp(name, FPRINT) != 0) {
		return 0;
	}
	if (fp.owner && new && strcmp(new, fp.owner) == 0) {
		return 0;
	}
	fp.claimed = false;
	set_listening(false);
	free(fp.owner);
	fp.owner = NULL;
	sd_bus_slot_unref(fp.status_slot);
	fp.status_slot = NULL;
	if (new && *new) {
		// the new one: everything asked again in a moment, when it is ready
		fp.gave_up = 0;
		fp.again_at = time(NULL) + 1;
		fp.again_all = true;
	} else {
		// gone: the bus starts fprintd again when it is asked, at the next try
		again_later(true);
	}
	return 0;
}

/* The computer woke up: a reader that slept with it may not say anything any more. */
static int on_sleep(sd_bus_message *m, void *data, sd_bus_error *ret_error) {
	int going = 0;
	if (fp.stopping || sd_bus_message_read(m, "b", &going) < 0 || going) {
		return 0;
	}
	fp.errors = 0;
	if (fp.claimed && !fp.gave_up) {
		restart_listening();
	} else if (!fp.claimed && fp.device) {
		fp.again_at = time(NULL) + 1;
		fp.again_all = true;
	}
	return 0;
}

/* Stops listening and starts again (VerifyStop, then VerifyStart). */
static void restart_listening(void) {
	fp.heard = time(NULL);
	set_listening(false);
	call(fp.device, FPRINT_DEVICE, "VerifyStop", on_verify_stopped, "");
}

static int env_seconds(const char *name, int fallback) {
	const char *v = getenv(name);
	int n = v ? atoi(v) : 0;
	return n > 0 ? n : fallback;
}

void fp_start(const char *user, const struct fp_callbacks *callbacks) {
	if (fp.bus || !user || turned_off()) {
		return;
	}
	fp.cb = *callbacks;
	fp.user = strdup(user);
	const char *which = getenv("TILEWIN_FPRINT_BUS");
	int r = which && strcmp(which, "session") == 0 ? sd_bus_open_user(&fp.bus) :
		sd_bus_open_system(&fp.bus);
	if (r < 0) {
		fp.bus = NULL;
		return;
	}
	// (the tests shorten these)
	fp.retry_s = env_seconds("TILEWIN_FPRINT_RETRY", RETRY_AFTER_S);
	fp.refresh_s = env_seconds("TILEWIN_FPRINT_REFRESH", REFRESH_S);
	sd_bus_add_match_async(fp.bus, &fp.owner_slot,
		"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
		"member='NameOwnerChanged',arg0='" FPRINT "'", on_owner_changed, NULL, NULL);
	sd_bus_add_match_async(fp.bus, &fp.sleep_slot,
		"type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',"
		"member='PrepareForSleep'", on_sleep, NULL, NULL);
	begin();
}

/* What is due by the clock: a try again after an error, a fresh start when the
 * reader has said nothing for long. */
static void tick(void) {
	if (!fp.bus || fp.stopping) {
		return;
	}
	time_t now = time(NULL);
	if (fp.again_at && now >= fp.again_at) {
		bool all = fp.again_all || !fp.claimed;
		fp.again_at = 0;
		fp.again_all = false;
		fp.gave_up = 0;
		fp.errors = 0;
		if (all) {
			sd_bus_slot_unref(fp.status_slot);
			fp.status_slot = NULL;
			begin();
		} else {
			start_verify();
		}
		return;
	}
	if (fp.listening && fp.heard && now - fp.heard >= fp.refresh_s) {
		restart_listening();
	}
}

void fp_retry(void) {
	if (!fp.bus || fp.listening || fp.stopping || !fp.gave_up ||
			time(NULL) - fp.gave_up < RETRY_AFTER_S) {
		return;
	}
	fp.gave_up = 0;
	fp.again_at = 0;
	if (fp.claimed) {
		fp.misses = fp.errors = 0;
		start_verify();
	} else {
		sd_bus_slot_unref(fp.status_slot);
		fp.status_slot = NULL;
		begin();
	}
}

bool fp_listening(void) {
	return fp.listening;
}

int fp_fd(void) {
	return fp.bus ? sd_bus_get_fd(fp.bus) : -1;
}

short fp_poll_events(void) {
	return fp.bus ? (short)sd_bus_get_events(fp.bus) : 0;
}

int fp_timeout_ms(void) {
	uint64_t usec;
	if (!fp.bus || sd_bus_get_timeout(fp.bus, &usec) < 0 || usec == UINT64_MAX) {
		return -1;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t now_usec = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
	return usec <= now_usec ? 0 : (int)((usec - now_usec + 999) / 1000);
}

void fp_dispatch(void) {
	if (!fp.bus) {
		return;
	}
	while (sd_bus_process(fp.bus, NULL) > 0) {
		if (!fp.bus) {
			return; // stopped by a callback
		}
	}
	tick();
}

void fp_stop(void) {
	if (!fp.bus) {
		return;
	}
	fp.stopping = true;
	if (fp.claimed && fp.device) {
		// quickly: fprintd lets go of the reader by itself once the lock screen is gone
		sd_bus_call_method(fp.bus, FPRINT, fp.device, FPRINT_DEVICE, "VerifyStop", NULL, NULL, "");
		sd_bus_call_method(fp.bus, FPRINT, fp.device, FPRINT_DEVICE, "Release", NULL, NULL, "");
	}
	sd_bus_slot_unref(fp.status_slot);
	sd_bus_slot_unref(fp.owner_slot);
	sd_bus_slot_unref(fp.sleep_slot);
	fp.status_slot = fp.owner_slot = fp.sleep_slot = NULL;
	fp.again_at = fp.gave_up = fp.heard = fp.last_miss = 0;
	sd_bus_flush_close_unref(fp.bus);
	fp.bus = NULL;
	fp.listening = fp.claimed = false;
	free(fp.user);
	free(fp.device);
	free(fp.owner);
	fp.user = fp.device = fp.owner = NULL;
}
