#define _POSIX_C_SOURCE 200809L
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "panel.h"

/*
 * Which windows are playing sound, for the speaker on their taskbar buttons:
 * the sink inputs of PulseAudio (or PipeWire's pulse server) belong to the
 * window whose process started the process playing (a browser plays from a
 * child of the one owning the window). "pactl subscribe" (pulse.c) says when
 * they change, and only then are they read again.
 */

#define MAX_ANCESTORS 32

struct stream {
	int64_t index;
	bool muted, corked;
	int volume; // percent
	int ancestors[MAX_ANCESTORS]; // the process playing and its parents
	int ancestor_count;
};

static struct {
	struct panel *panel;
	list_t *streams; // struct stream *
	struct proc *query;
	bool query_again;
} as;

static void query(void);

static int parent_of(int pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	char buf[512];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	// pid (comm) state ppid: comm may hold spaces and parentheses
	char *close = strrchr(buf, ')');
	int ppid = 0;
	if (!close || sscanf(close + 1, " %*c %d", &ppid) != 1) {
		return 0;
	}
	return ppid;
}

static int json_percent(json_object *obj) {
	json_object *volume;
	if (!json_object_object_get_ex(obj, "volume", &volume)) {
		return 100;
	}
	int total = 0, count = 0;
	json_object_object_foreach(volume, key, channel) {
		(void)key;
		json_object *percent;
		if (json_object_object_get_ex(channel, "value_percent", &percent)) {
			total += atoi(json_object_get_string(percent));
			count++;
		}
	}
	return count > 0 ? total / count : 100;
}

static bool stream_flag(json_object *obj, const char *key) {
	json_object *value;
	return json_object_object_get_ex(obj, key, &value) && json_object_get_boolean(value);
}

static void clear_streams(void) {
	while (as.streams->length > 0) {
		free(as.streams->items[as.streams->length - 1]);
		list_del(as.streams, as.streams->length - 1);
	}
}

static void query_done(void *data, const char *output) {
	as.query = NULL;
	clear_streams();
	json_object *array = output ? json_tokener_parse(output) : NULL;
	int n = array && json_object_is_type(array, json_type_array) ?
		(int)json_object_array_length(array) : 0;
	for (int i = 0; i < n; i++) {
		json_object *obj = json_object_array_get_idx(array, i);
		json_object *properties = NULL, *index = NULL, *pid = NULL;
		json_object_object_get_ex(obj, "properties", &properties);
		if (!properties || !json_object_object_get_ex(properties, "application.process.id",
				&pid)) {
			continue;
		}
		struct stream *s = calloc(1, sizeof(*s));
		if (!s) {
			break;
		}
		s->index = json_object_object_get_ex(obj, "index", &index) ?
			json_object_get_int64(index) : -1;
		s->muted = stream_flag(obj, "mute");
		s->corked = stream_flag(obj, "corked");
		s->volume = json_percent(obj);
		for (int p = atoi(json_object_get_string(pid)); p > 1 &&
				s->ancestor_count < MAX_ANCESTORS; p = parent_of(p)) {
			s->ancestors[s->ancestor_count++] = p;
		}
		list_add(as.streams, s);
	}
	json_object_put(array);
	panel_set_dirty(as.panel);
	if (as.query_again) {
		as.query_again = false;
		query();
	}
}

static void query(void) {
	if (as.query) {
		as.query_again = true;
		return;
	}
	as.query = proc_run(as.panel, "pactl -f json list sink-inputs 2>/dev/null", false, NULL,
		query_done, NULL);
}

static void watch_line(void *data, const char *line) {
	if (strstr(line, "sink-input")) {
		query();
	}
}

static void watch_started(void *data) {
	query();
}

static void watch_lost(void *data) {
	// the sound server went away (or there is none): no app plays now
	if (as.streams->length > 0) {
		clear_streams();
		panel_set_dirty(as.panel);
	}
}

void appsound_init(struct panel *panel) {
	as.panel = panel;
	if (as.streams) {
		return;
	}
	as.streams = create_list();
	pulse_listen(panel, watch_line, watch_started, watch_lost, &as);
}

static bool belongs(const struct stream *s, int pid) {
	for (int i = 0; pid > 1 && i < s->ancestor_count; i++) {
		if (s->ancestors[i] == pid) {
			return true;
		}
	}
	return false;
}

/* The windows of the button: the one, or all of the app for a group. */
static bool of_button(struct panel *panel, struct pwindow *win, bool group,
		const struct stream *s) {
	if (!group) {
		return belongs(s, win->pid);
	}
	for (int i = 0; i < panel->state.windows->length; i++) {
		struct pwindow *o = panel->state.windows->items[i];
		if (strcmp(o->app_id, win->app_id) == 0 && belongs(s, o->pid)) {
			return true;
		}
	}
	return false;
}

bool appsound_for_window(struct panel *panel, struct pwindow *win, bool group,
		struct app_sound *out) {
	if (!as.streams || !win) {
		return false;
	}
	bool found = false;
	out->muted = true;
	out->volume = 0;
	for (int i = 0; i < as.streams->length; i++) {
		struct stream *s = as.streams->items[i];
		if (s->corked || !of_button(panel, win, group, s)) {
			continue; // paused streams make no sound
		}
		found = true;
		out->muted = out->muted && s->muted;
		out->volume = s->volume > out->volume ? s->volume : out->volume;
	}
	return found;
}

static void run_for_streams(struct panel *panel, struct pwindow *win, bool group,
		const char *format, const char *arg) {
	for (int i = 0; i < as.streams->length; i++) {
		struct stream *s = as.streams->items[i];
		if (s->corked || !of_button(panel, win, group, s)) {
			continue;
		}
		char cmd[128];
		snprintf(cmd, sizeof(cmd), format, (long long)s->index, arg);
		proc_spawn(cmd);
	}
}

void appsound_toggle_mute(struct panel *panel, struct pwindow *win, bool group) {
	struct app_sound sound;
	if (!appsound_for_window(panel, win, group, &sound)) {
		return;
	}
	// all muted: unmute them all; otherwise mute all of them
	run_for_streams(panel, win, group, "pactl set-sink-input-mute %lld %s",
		sound.muted ? "0" : "1");
}

void appsound_change_volume(struct panel *panel, struct pwindow *win, bool group, int step) {
	char arg[16];
	snprintf(arg, sizeof(arg), "%+d%%", step);
	run_for_streams(panel, win, group, "pactl set-sink-input-volume %lld %s", arg);
}
