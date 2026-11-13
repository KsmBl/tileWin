#ifndef _TW_LOCK_FINGERPRINT_H
#define _TW_LOCK_FINGERPRINT_H

#include <stdbool.h>

/*
 * The fingerprint reader on the lock screen, through fprintd: while the session
 * is locked a finger of the user's that fprintd knows unlocks it, alongside the
 * password. See fingerprint.c.
 */
struct fp_callbacks {
	void (*matched)(void);            // a finger of the user's: unlock
	void (*message)(const char *text); // what to tell the user (not recognised, ...)
	void (*changed)(void);            // listening started or stopped: draw again
};

/* Starts listening when there is a reader and the user has fingers on it. */
void fp_start(const char *user, const struct fp_callbacks *callbacks);
/* Listens again after it gave up (a key pressed), when it may. */
void fp_retry(void);
/* Whether a finger is being listened for right now. */
bool fp_listening(void);
/* For poll(): the file descriptor, its events and how long to wait (ms, -1). */
int fp_fd(void);
short fp_poll_events(void);
int fp_timeout_ms(void);
/* After poll(): handles what came. */
void fp_dispatch(void);
/* Stops listening and lets go of the reader. */
void fp_stop(void);

#endif
