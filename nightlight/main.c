/*
 * tilewin-nightlight: warmer screen colors in the evening, like the night
 * light of Windows.
 *
 *   tilewin-nightlight                 sets the gamma tables of all screens
 *                                      (tileWin starts it)
 *   tilewin-nightlight on|off|toggle   turns the night light on or off now
 *   tilewin-nightlight status          prints "on" or "off"
 *
 * ~/.config/tileWin/nightlight.conf:
 *   temperature 3400    color temperature while on, 1900 (warm) to 6500 K
 *   schedule yes|no     turn on at "from" and off at "to"
 *   from 21:00
 *   to 07:00
 *
 * Whether it is on right now is kept in ~/.local/state/tileWin/nightlight.
 * The schedule writes it at the start and end times, so turning the night
 * light off by hand lasts until the next start time. Both files are watched,
 * so the settings app and the quick settings only write them.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "tw_paths.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#define TEMP_NEUTRAL 6500
#define TEMP_MIN 1900
#define TEMP_MAX 6500
#define FADE_MS 1500
#define FADE_TEMP_MS 250

struct settings {
	int temperature;
	bool schedule;
	int from, to; // minutes after midnight
};

struct output {
	struct wl_list link;
	uint32_t name;
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma;
	uint32_t size;
	bool failed;
};

static struct {
	struct wl_display *display;
	struct zwlr_gamma_control_manager_v1 *manager;
	struct wl_list outputs;
	struct settings settings;
	bool on;
	bool inside; // the time is inside the schedule
	double temperature; // shown right now
	double fade_from, fade_to;
	struct timespec fade_start;
	int fade_ms;
	bool fading;
	char *config_path, *state_path;
} nl;

static void parse_time(const char *text, int *minutes) {
	int h, m;
	if (sscanf(text, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
		*minutes = h * 60 + m;
	}
}

static void load_settings(struct settings *s) {
	*s = (struct settings){ .temperature = 3400, .schedule = false,
		.from = 21 * 60, .to = 7 * 60 };
	FILE *f = nl.config_path ? fopen(nl.config_path, "r") : NULL;
	if (!f) {
		return;
	}
	char line[256], key[64], value[128];
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " %63s %127s", key, value) != 2 || key[0] == '#') {
			continue;
		}
		if (strcmp(key, "temperature") == 0) {
			int t = atoi(value);
			s->temperature = t < TEMP_MIN ? TEMP_MIN : t > TEMP_MAX ? TEMP_MAX : t;
		} else if (strcmp(key, "schedule") == 0) {
			s->schedule = strcmp(value, "yes") == 0 || strcmp(value, "on") == 0 ||
				strcmp(value, "true") == 0;
		} else if (strcmp(key, "from") == 0) {
			parse_time(value, &s->from);
		} else if (strcmp(key, "to") == 0) {
			parse_time(value, &s->to);
		}
	}
	fclose(f);
}

static bool read_state(void) {
	char *value = nl.state_path ? tw_read_first_line(nl.state_path) : NULL;
	bool on = value && strcmp(value, "on") == 0;
	free(value);
	return on;
}

static void write_state(bool on) {
	if (nl.state_path && read_state() != on) {
		tw_write_string(nl.state_path, on ? "on\n" : "off\n");
	}
}

static bool inside_schedule(const struct settings *s) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	int m = tm.tm_hour * 60 + tm.tm_min;
	if (s->from == s->to) {
		return false;
	} else if (s->from < s->to) {
		return m >= s->from && m < s->to;
	}
	return m >= s->from || m < s->to;
}

/* ---------- colors ---------- */

/* RGB of a black body at that temperature (Tanner Helland's fit), 0-255. */
static void blackbody(double kelvin, double rgb[3]) {
	double t = kelvin / 100.0;
	double r, g, b;
	if (t <= 66) {
		r = 255;
		g = 99.4708025861 * log(t) - 161.1195681661;
		b = t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
	} else {
		r = 329.698727446 * pow(t - 60, -0.1332047592);
		g = 288.1221695283 * pow(t - 60, -0.0755148492);
		b = 255;
	}
	rgb[0] = r;
	rgb[1] = g;
	rgb[2] = b;
}

/* Channel factors, relative to 6500 K so that the neutral temperature is exact. */
static void temperature_rgb(double kelvin, double rgb[3]) {
	double v[3], n[3];
	blackbody(kelvin, v);
	blackbody(TEMP_NEUTRAL, n);
	for (int i = 0; i < 3; i++) {
		double f = v[i] / n[i];
		rgb[i] = f < 0 ? 0 : f > 1 ? 1 : f;
	}
}

static void set_ramps(struct output *out) {
	if (!out->gamma || out->size == 0) {
		return;
	}
	size_t bytes = out->size * 3 * sizeof(uint16_t);
	int fd = memfd_create("tilewin-gamma", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, bytes) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	uint16_t *table = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (table == MAP_FAILED) {
		close(fd);
		return;
	}
	double rgb[3];
	temperature_rgb(nl.temperature, rgb);
	for (int c = 0; c < 3; c++) {
		for (uint32_t i = 0; i < out->size; i++) {
			double v = out->size > 1 ? (double)i / (out->size - 1) : 1;
			table[c * out->size + i] = (uint16_t)round(v * rgb[c] * 65535.0);
		}
	}
	munmap(table, bytes);
	zwlr_gamma_control_v1_set_gamma(out->gamma, fd);
	close(fd);
}

static void gamma_size(void *data, struct zwlr_gamma_control_v1 *gamma, uint32_t size) {
	struct output *out = data;
	out->size = size;
	set_ramps(out);
}

static void gamma_failed(void *data, struct zwlr_gamma_control_v1 *gamma) {
	struct output *out = data;
	if (!out->failed) {
		fprintf(stderr, "tilewin-nightlight: a screen does not support gamma tables, "
			"or another program controls them\n");
	}
	zwlr_gamma_control_v1_destroy(out->gamma);
	out->gamma = NULL;
	out->size = 0;
	out->failed = true;
}

static const struct zwlr_gamma_control_v1_listener gamma_listener = {
	.gamma_size = gamma_size,
	.failed = gamma_failed,
};

static void apply(void) {
	bool neutral = !nl.fading && nl.temperature >= TEMP_NEUTRAL - 0.5;
	struct output *out;
	wl_list_for_each(out, &nl.outputs, link) {
		if (neutral) {
			// giving the gamma control up restores the normal colors
			if (out->gamma) {
				zwlr_gamma_control_v1_destroy(out->gamma);
				out->gamma = NULL;
				out->size = 0;
			}
			out->failed = false;
		} else if (!out->gamma && !out->failed && nl.manager) {
			out->gamma = zwlr_gamma_control_manager_v1_get_gamma_control(nl.manager,
				out->wl_output);
			zwlr_gamma_control_v1_add_listener(out->gamma, &gamma_listener, out);
		} else {
			set_ramps(out);
		}
	}
}

static long ms_since(const struct timespec *start) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static void set_target(void) {
	double target = nl.on ? nl.settings.temperature : TEMP_NEUTRAL;
	double goal = nl.fading ? nl.fade_to : nl.temperature;
	if (fabs(goal - target) < 0.5) {
		return;
	}
	// turning on or off fades slowly, moving the strength slider follows fast
	bool was_on = goal < TEMP_NEUTRAL - 0.5;
	nl.fade_ms = was_on && nl.on ? FADE_TEMP_MS : FADE_MS;
	nl.fade_from = nl.temperature;
	nl.fade_to = target;
	clock_gettime(CLOCK_MONOTONIC, &nl.fade_start);
	nl.fading = true;
}

static void fade_step(void) {
	if (!nl.fading) {
		return;
	}
	double f = (double)ms_since(&nl.fade_start) / nl.fade_ms;
	if (f >= 1) {
		f = 1;
		nl.fading = false;
	}
	nl.temperature = nl.fade_from + (nl.fade_to - nl.fade_from) * f;
	apply();
}

/* Re-reads both files; the schedule turns the light on or off at its edges. */
static void reload(bool first) {
	struct settings old = nl.settings;
	load_settings(&nl.settings);
	const struct settings *s = &nl.settings;
	bool inside = inside_schedule(s);
	if (s->schedule && (first || !old.schedule || old.from != s->from || old.to != s->to ||
			inside != nl.inside)) {
		write_state(inside);
	}
	nl.inside = inside;
	nl.on = read_state();
	set_target();
}

/* ---------- wayland ---------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *out = calloc(1, sizeof(*out));
		if (!out) {
			return;
		}
		out->name = name;
		out->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		wl_list_insert(&nl.outputs, &out->link);
		apply();
	} else if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
		nl.manager = wl_registry_bind(registry, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct output *out, *tmp;
	wl_list_for_each_safe(out, tmp, &nl.outputs, link) {
		if (out->name == name) {
			if (out->gamma) {
				zwlr_gamma_control_v1_destroy(out->gamma);
			}
			wl_output_destroy(out->wl_output);
			wl_list_remove(&out->link);
			free(out);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static bool single_instance(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return true;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/tilewin-nightlight-%s.lock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	return fd < 0 || flock(fd, LOCK_EX | LOCK_NB) == 0; // the fd stays open
}

static int run(void) {
	if (!single_instance()) {
		return 0;
	}
	nl.display = wl_display_connect(NULL);
	if (!nl.display) {
		fprintf(stderr, "tilewin-nightlight: can't connect to the Wayland display\n");
		return 1;
	}
	wl_list_init(&nl.outputs);
	nl.temperature = TEMP_NEUTRAL;
	struct wl_registry *registry = wl_display_get_registry(nl.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(nl.display);
	if (!nl.manager) {
		fprintf(stderr, "tilewin-nightlight: the compositor has no gamma control\n");
		return 1;
	}

	char *config_dir = tw_config_dir();
	char *state_dir = tw_state_dir();
	int inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE;
	if (config_dir && tw_mkdir_p(config_dir) && inotify >= 0) {
		inotify_add_watch(inotify, config_dir, mask);
	}
	if (state_dir && tw_mkdir_p(state_dir) && inotify >= 0) {
		inotify_add_watch(inotify, state_dir, mask);
	}
	free(config_dir);
	free(state_dir);

	reload(true);
	fade_step();
	struct pollfd fds[2] = {
		{ .fd = wl_display_get_fd(nl.display), .events = POLLIN },
		{ .fd = inotify, .events = POLLIN },
	};
	while (true) {
		wl_display_dispatch_pending(nl.display);
		if (wl_display_flush(nl.display) < 0 && errno != EAGAIN) {
			break;
		}
		int timeout = nl.fading ? 30 : 30000;
		int n = poll(fds, inotify >= 0 ? 2 : 1, timeout);
		if (n < 0 && errno != EINTR) {
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) {
			break;
		}
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(nl.display) < 0) {
			break;
		}
		bool changed = false;
		if (inotify >= 0 && (fds[1].revents & POLLIN)) {
			char buf[4096];
			ssize_t len;
			while ((len = read(inotify, buf, sizeof(buf))) > 0) {
				for (char *p = buf; p < buf + len;) {
					struct inotify_event *ev = (struct inotify_event *)p;
					if (ev->len && (strcmp(ev->name, "nightlight.conf") == 0 ||
							strcmp(ev->name, "nightlight") == 0)) {
						changed = true;
					}
					p += sizeof(*ev) + ev->len;
				}
			}
		}
		if (changed || !nl.fading) {
			reload(false); // also checks the schedule every 30 seconds
		}
		fade_step();
	}
	return 0;
}

int main(int argc, char **argv) {
	char *config_dir = tw_config_dir();
	char *state_dir = tw_state_dir();
	if (config_dir) {
		nl.config_path = malloc(strlen(config_dir) + 32);
		sprintf(nl.config_path, "%s/nightlight.conf", config_dir);
	}
	if (state_dir) {
		nl.state_path = malloc(strlen(state_dir) + 32);
		sprintf(nl.state_path, "%s/nightlight", state_dir);
	}
	free(config_dir);
	free(state_dir);

	if (argc < 2) {
		return run();
	}
	const char *cmd = argv[1];
	if (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0) {
		write_state(strcmp(cmd, "on") == 0);
	} else if (strcmp(cmd, "toggle") == 0) {
		write_state(!read_state());
	} else if (strcmp(cmd, "status") == 0) {
		printf("%s\n", read_state() ? "on" : "off");
	} else {
		fprintf(stderr, "Usage: tilewin-nightlight [on|off|toggle|status]\n"
			"Without an argument it keeps the screen colors (tileWin starts it).\n");
		return strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ? 0 : 1;
	}
	return 0;
}
