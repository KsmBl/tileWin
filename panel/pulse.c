#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
#include "panel.h"

/*
 * One "pactl subscribe" for everything in the taskbar that wants to know when
 * the sound changes (the volume widgets, the speakers on the taskbar buttons):
 * one process instead of one each. While nobody listens none runs. When it
 * ends (no sound server, or the server restarted or was swapped for another)
 * the listeners hear of it, and it is started again: soon at first, then less
 * and less often while there is still no server, up to once a minute.
 */

#define RETRY_FIRST_MS 2000
#define RETRY_MAX_MS 60000
#define SETTLED_S 10 // ran this long: the server was there, so retry soon again

struct listener {
	void (*event)(void *data, const char *line);
	void (*started)(void *data);
	void (*lost)(void *data);
	void *data;
};

static struct {
	struct panel *panel;
	list_t *listeners; // struct listener *
	struct proc *proc;
	struct loop_timer *retry;
	int retry_ms;
	time_t started;
} pulse;

static void start(void *data);

static void line(void *data, const char *text) {
	for (int i = 0; pulse.listeners && i < pulse.listeners->length; i++) {
		struct listener *l = pulse.listeners->items[i];
		l->event(l->data, text);
	}
}

static void done(void *data, const char *output) {
	pulse.proc = NULL;
	if (!pulse.listeners || pulse.listeners->length == 0) {
		return;
	}
	if (time(NULL) - pulse.started >= SETTLED_S) {
		pulse.retry_ms = RETRY_FIRST_MS;
	}
	for (int i = 0; i < pulse.listeners->length; i++) {
		struct listener *l = pulse.listeners->items[i];
		if (l->lost) {
			l->lost(l->data);
		}
	}
	if (!pulse.retry) {
		pulse.retry = loop_add_timer(pulse.panel->loop, pulse.retry_ms, start, NULL);
		pulse.retry_ms = pulse.retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : pulse.retry_ms * 2;
	}
}

static void start(void *data) {
	pulse.retry = NULL;
	if (pulse.proc || !pulse.listeners || pulse.listeners->length == 0) {
		return;
	}
	pulse.started = time(NULL);
	pulse.proc = proc_run(pulse.panel, "exec pactl subscribe 2>/dev/null", true, line, done,
		NULL);
	for (int i = 0; i < pulse.listeners->length; i++) {
		struct listener *l = pulse.listeners->items[i];
		if (l->started) {
			l->started(l->data); // read what is there now: changes come from here on
		}
	}
}

void pulse_listen(struct panel *panel, void (*event)(void *data, const char *line),
		void (*started)(void *data), void (*lost)(void *data), void *data) {
	pulse.panel = panel;
	if (!pulse.listeners) {
		pulse.listeners = create_list();
		pulse.retry_ms = RETRY_FIRST_MS;
	}
	struct listener *l = calloc(1, sizeof(*l));
	*l = (struct listener){ event, started, lost, data };
	list_add(pulse.listeners, l);
	if (pulse.proc) {
		if (started) {
			started(data);
		}
	} else if (!pulse.retry) {
		start(NULL);
	}
}

void pulse_unlisten(void *data) {
	for (int i = 0; pulse.listeners && i < pulse.listeners->length; i++) {
		struct listener *l = pulse.listeners->items[i];
		if (l->data == data) {
			free(l);
			list_del(pulse.listeners, i);
			break;
		}
	}
	if (pulse.listeners && pulse.listeners->length == 0) {
		proc_cancel(pulse.proc);
		pulse.proc = NULL;
		if (pulse.retry) {
			loop_remove_timer(pulse.panel->loop, pulse.retry);
			pulse.retry = NULL;
		}
	}
}
