#define _GNU_SOURCE
#include <cairo.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tw_kernel_auto.h"

static void path_of(char *out, size_t size, const char *path) {
	const char *base = getenv("TILEWIN_KERNEL_AUTO_ROOT");
	snprintf(out, size, "%s%s", base ? base : "", path);
}

bool tw_kernel_auto_changed(long long *seen) {
	static const char *const files[] = { "/var/lib/tilewin/kernel-auto/status",
		"/var/lib/tilewin/kernel-auto/log", "/etc/tilewin/kernel-auto.conf" };
	long long stamp = 0;
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char path[1024];
		struct stat st;
		path_of(path, sizeof(path), files[i]);
		if (stat(path, &st) == 0) {
			stamp = stamp * 31 + st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec +
				st.st_ino;
		}
	}
	bool changed = stamp != *seen;
	*seen = stamp;
	return changed;
}

/* Calls fn for each KEY=VALUE line of a file. */
static bool read_pairs(const char *path, void (*fn)(void *data, const char *key,
		const char *value), void *data) {
	char full[1024];
	path_of(full, sizeof(full), path);
	FILE *f = fopen(full, "r");
	if (!f) {
		return false;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '#') {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		fn(data, line, eq + 1);
	}
	fclose(f);
	return true;
}

#define COPY(field, value) snprintf(field, sizeof(field), "%s", value)

static void conf_pair(void *data, const char *key, const char *value) {
	struct tw_kernel_auto *ka = data;
	if (strcmp(key, "enabled") == 0) {
		ka->enabled = strcmp(value, "yes") == 0;
	} else if (strcmp(key, "profile") == 0) {
		COPY(ka->profile, value);
	} else if (strcmp(key, "series") == 0) {
		COPY(ka->series, value);
	} else if (strcmp(key, "native") == 0) {
		ka->native = strcmp(value, "on") == 0;
	} else if (strcmp(key, "battery") == 0) {
		ka->battery = strcmp(value, "yes") == 0;
	} else if (strcmp(key, "when") == 0) {
		COPY(ka->when, value);
	} else if (strcmp(key, "time") == 0) {
		COPY(ka->time, value);
	} else if (strcmp(key, "catchup") == 0) {
		ka->catchup = strcmp(value, "no") != 0;
	}
}

static void status_pair(void *data, const char *key, const char *value) {
	struct tw_kernel_auto *ka = data;
	if (strcmp(key, "state") == 0) {
		COPY(ka->state, value);
	} else if (strcmp(key, "message") == 0) {
		COPY(ka->message, value);
	} else if (strcmp(key, "error") == 0) {
		COPY(ka->error, value);
	} else if (strcmp(key, "arm_error") == 0) {
		COPY(ka->arm_error, value);
	} else if (strcmp(key, "running") == 0) {
		COPY(ka->running, value);
	} else if (strcmp(key, "running_auto") == 0) {
		ka->running_auto = strcmp(value, "yes") == 0;
	} else if (strcmp(key, "next") == 0) {
		COPY(ka->next, value);
	} else if (strcmp(key, "fixes") == 0) {
		COPY(ka->fixes, value);
	} else if (strcmp(key, "fixes_left") == 0) {
		ka->fixes_left = atoi(value);
	} else if (strcmp(key, "checked") == 0) {
		ka->checked = strtol(value, NULL, 10);
	} else if (strcmp(key, "fixes_applied") == 0) {
		ka->fixes_applied = strtol(value, NULL, 10);
	} else if (strcmp(key, "power") == 0) {
		ka->on_battery = strcmp(value, "battery") == 0;
	}
}

static void build_pair(void *data, const char *key, const char *value) {
	struct tw_kernel_build *b = data;
	if (strcmp(key, "phase") == 0) {
		COPY(b->phase, value);
	} else if (strcmp(key, "version") == 0) {
		COPY(b->version, value);
	} else if (strcmp(key, "text") == 0) {
		COPY(b->text, value);
	} else if (strcmp(key, "percent") == 0) {
		b->percent = atoi(value);
	} else if (strcmp(key, "updated") == 0) {
		b->updated = strtol(value, NULL, 10);
	}
}

/* Whether tilewin-kernel-root auto-run holds its lock now. */
static bool run_going_on(void) {
	char path[1024];
	path_of(path, sizeof(path), "/run/tilewin-kernel-auto.lock");
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	bool held = flock(fd, LOCK_SH | LOCK_NB) != 0;
	close(fd);
	return held;
}

static void read_log(struct tw_kernel_auto *ka) {
	char path[1024];
	path_of(path, sizeof(path), "/var/lib/tilewin/kernel-auto/log");
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[512];
	// a ring of the last ones, then turned round: newest first
	struct { time_t when; char text[192]; } ring[TW_KERNEL_AUTO_LOG];
	int count = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		char *tab = strchr(line, '\t');
		if (!tab) {
			continue;
		}
		*tab = '\0';
		int slot = count % TW_KERNEL_AUTO_LOG;
		ring[slot].when = strtol(line, NULL, 10);
		snprintf(ring[slot].text, sizeof(ring[slot].text), "%s", tab + 1);
		count++;
	}
	fclose(f);
	int n = count < TW_KERNEL_AUTO_LOG ? count : TW_KERNEL_AUTO_LOG;
	for (int i = 0; i < n; i++) {
		int slot = (count - 1 - i) % TW_KERNEL_AUTO_LOG;
		ka->log[i].when = ring[slot].when;
		snprintf(ka->log[i].text, sizeof(ka->log[i].text), "%s", ring[slot].text);
	}
	ka->log_count = n;
}

bool tw_kernel_auto_read(struct tw_kernel_auto *ka) {
	memset(ka, 0, sizeof(*ka));
	ka->kernel.percent = ka->fixes_build.percent = -1;
	ka->catchup = true;
	ka->configured = read_pairs("/etc/tilewin/kernel-auto.conf", conf_pair, ka);
	if (!ka->configured) {
		return false;
	}
	read_pairs("/var/lib/tilewin/kernel-auto/status", status_pair, ka);
	read_pairs("/var/lib/tilewin/kernel-auto/build/kernel", build_pair, &ka->kernel);
	read_pairs("/var/lib/tilewin/kernel-auto/build/fixes", build_pair, &ka->fixes_build);
	read_log(ka);
	ka->busy = run_going_on();
	char timer[1024];
	path_of(timer, sizeof(timer),
		"/etc/systemd/system/timers.target.wants/tilewin-kernel-auto.timer");
	ka->timer_on = access(timer, F_OK) == 0;
	return true;
}

void tw_kernel_auto_schedule_words(const struct tw_kernel_auto *ka, char *out, size_t size) {
	static const char *const days[][2] = { { "mon", "Monday" }, { "tue", "Tuesday" },
		{ "wed", "Wednesday" }, { "thu", "Thursday" }, { "fri", "Friday" },
		{ "sat", "Saturday" }, { "sun", "Sunday" } };
	if (!ka->timer_on) {
		snprintf(out, size, "only at Update now (the timer is off)");
		return;
	}
	const char *at = ka->time[0] ? ka->time : "03:00";
	for (size_t i = 0; i < sizeof(days) / sizeof(days[0]); i++) {
		if (strcmp(ka->when, days[i][0]) == 0) {
			snprintf(out, size, "every %s at %s", days[i][1], at);
			return;
		}
	}
	snprintf(out, size, "every day at %s", at);
}

void tw_kernel_auto_release(const char *kver, char *out, size_t size) {
	snprintf(out, size, "%s", kver ? kver : "");
	char *dash = strchr(out, '-');
	if (dash) {
		*dash = '\0';
	}
	// 7.3.0 is called 7.3
	size_t len = strlen(out);
	if (len > 2 && strcmp(out + len - 2, ".0") == 0 && strchr(out, '.') != out + len - 2) {
		out[len - 2] = '\0';
	}
}

void tw_kernel_auto_ago(time_t then, time_t now, char *out, size_t size) {
	long s = (long)(now - then);
	if (s < 90) {
		snprintf(out, size, "just now");
	} else if (s < 5400) {
		snprintf(out, size, "%ld min ago", (s + 30) / 60);
	} else if (s < 36 * 3600) {
		snprintf(out, size, "%ld h ago", (s + 1800) / 3600);
	} else {
		long days = (s + 43200) / 86400;
		snprintf(out, size, "%ld day%s ago", days, days == 1 ? "" : "s");
	}
}

static bool phase_is(const struct tw_kernel_build *b, const char *phase) {
	return strcmp(b->phase, phase) == 0;
}

static bool build_going(const struct tw_kernel_build *b) {
	return b->phase[0] && !phase_is(b, "done") && !phase_is(b, "failed") &&
		!phase_is(b, "cancelled") && !phase_is(b, "none");
}

#define SET(field, ...) snprintf(out->field, sizeof(out->field), __VA_ARGS__)

/* What a build that goes on does: its phase as words and how far it is. */
static void summarize_build(const struct tw_kernel_build *b, bool fixes,
		struct tw_kernel_auto_summary *out) {
	const char *v = b->version[0] ? b->version : "";
	out->percent = b->percent;
	if (phase_is(b, "download")) {
		out->kind = TW_KA_DOWNLOADING;
		if (fixes) {
			SET(brief, "Downloading fixes");
			SET(title, "Downloading the fixes of Linux %s", v);
		} else if (b->percent >= 0) {
			SET(brief, "Downloading %s · %d%%", v, b->percent);
			SET(title, "Downloading Linux %s", v);
		} else {
			SET(brief, "Downloading %s", v);
			SET(title, "Downloading Linux %s", v);
		}
		SET(detail, "From kernel.org, checked against its signed checksums before it is used.");
		return;
	}
	if (fixes) {
		out->kind = TW_KA_FIXING;
		if (b->percent >= 0) {
			SET(brief, "Making live fixes · %d%%", b->percent);
		} else {
			SET(brief, "Making live fixes");
		}
		SET(title, "Making the fixes of Linux %s", v);
		// what it does now, as the build says it ("Making the livepatch of Linux 7.3.5:
		// building it with the fixes"), else what it is about
		const char *colon = strstr(b->text, ": ");
		if (colon && colon[2]) {
			SET(detail, "Now %s; it goes in without a restart once it is made.", colon + 2);
		} else if (b->text[0]) {
			SET(detail, "%s. It becomes a livepatch, applied without a restart once it is "
				"made.", b->text);
		} else {
			SET(detail, "What kernel.org fixed since the running kernel's release becomes a "
				"livepatch, applied without a restart once it is made.");
		}
		return;
	}
	out->kind = TW_KA_BUILDING;
	if (phase_is(b, "compile") && b->percent >= 0) {
		SET(brief, "Building %s · %d%%", v, b->percent);
	} else {
		SET(brief, "Building %s", v);
	}
	SET(title, "Building Linux %s", v);
	const char *step = phase_is(b, "verify") ? "Checking the download" :
		phase_is(b, "unpack") ? "Unpacking the source" :
		phase_is(b, "configure") ? "Configuring it like the running kernel" :
		phase_is(b, "compile") ? "Compiling" :
		phase_is(b, "modules") ? "Collecting the modules" :
		phase_is(b, "check") ? "Checking that this computer can start it" : "Getting ready";
	SET(detail, "%s, at the lowest priority: the computer stays as quick as it is. It starts "
		"at the next restart.", step);
	if (!phase_is(b, "compile")) {
		out->percent = -1;
	}
}

void tw_kernel_auto_summarize(const struct tw_kernel_auto *ka, time_t now,
		struct tw_kernel_auto_summary *out) {
	memset(out, 0, sizeof(*out));
	out->percent = -1;
	char next[32], running[32];
	tw_kernel_auto_release(ka->next, next, sizeof(next));
	tw_kernel_auto_release(ka->running, running, sizeof(running));
	if (!ka->configured || !ka->enabled) {
		out->kind = TW_KA_OFF;
		SET(brief, "Updates off");
		SET(title, "Automatic kernel updates are off");
		SET(detail, "Switch them on on the Kernel page: fixes go into the running kernel "
			"without a restart, new releases are built for the next one.");
		return;
	}
	if (ka->busy) {
		if (strcmp(ka->state, "applying") == 0) {
			out->kind = TW_KA_APPLYING;
			SET(brief, "Applying fixes");
			SET(title, "Applying fixes to the running kernel");
			SET(detail, "Every program goes on as it is; the kernel is watched for a moment "
				"after, and the fixes are taken out again if it complains.");
			return;
		}
		if (strcmp(ka->state, "installing") == 0) {
			out->kind = TW_KA_APPLYING;
			SET(brief, "Installing kernel");
			SET(title, "Putting the new kernel on the computer");
			SET(detail, "Its modules, its boot image and its entry for the next start.");
			return;
		}
		// (a build that has said nothing for an hour was stopped without a word)
		if (strcmp(ka->state, "fixes") == 0 && build_going(&ka->fixes_build) &&
				now - ka->fixes_build.updated < 3600) {
			summarize_build(&ka->fixes_build, true, out);
			return;
		}
		if (strcmp(ka->state, "building") == 0 && build_going(&ka->kernel) &&
				now - ka->kernel.updated < 3600) {
			summarize_build(&ka->kernel, false, out);
			return;
		}
		out->kind = TW_KA_CHECKING;
		SET(brief, "Checking");
		SET(title, "Looking for kernel fixes and releases");
		SET(detail, "%s", ka->message[0] ? ka->message : "Asking kernel.org");
		return;
	}
	if (ka->error[0] || ka->arm_error[0]) {
		out->kind = TW_KA_ERROR;
		out->attention = true;
		SET(brief, "Kernel update failed");
		SET(title, "A kernel update did not work");
		SET(detail, "%s", ka->error[0] ? ka->error : ka->arm_error);
		return;
	}
	if (ka->next[0]) {
		out->kind = TW_KA_RESTART;
		out->attention = true;
		SET(brief, "%s at restart", next);
		SET(title, "Linux %s is ready for the next start", next);
		if (ka->fixes[0]) {
			SET(detail, "The running kernel has the fixes of Linux %s already%s. Restart "
				"whenever it suits you.", ka->fixes, ka->fixes_left > 0 ?
				"; the few that cannot be applied live come with the new one" : "");
		} else {
			SET(detail, "Restart whenever it suits you; until then nothing is lost.");
		}
		return;
	}
	if (ka->fixes_applied > 0 && now - ka->fixes_applied < 24 * 3600) {
		out->kind = TW_KA_FIXED;
		SET(brief, "Fixed live: %s", ka->fixes);
		SET(title, "Fixed without a restart");
		SET(detail, "The security fixes of Linux %s went into the running kernel.%s",
			ka->fixes, ka->fixes_left > 0 ? " A few need a restart; they come with the next "
			"kernel." : "");
		return;
	}
	if (ka->on_battery) {
		out->kind = TW_KA_WAITING;
		SET(brief, "Waiting for power");
		SET(title, "New kernels are built on mains power");
		SET(detail, "Live fixes still go in; a new release is built once the charger is in.");
		return;
	}
	if (!ka->running_auto) {
		out->kind = TW_KA_PENDING;
		SET(brief, "No update yet");
		SET(title, "Your first updated kernel is not built yet");
		char when[64];
		tw_kernel_auto_schedule_words(ka, when, sizeof(when));
		SET(detail, "It is built %s, or now with Update now.", when);
		return;
	}
	out->kind = TW_KA_OK;
	SET(brief, "Up to date");
	SET(title, "The kernel is up to date");
	if (ka->fixes[0]) {
		SET(detail, "Linux %s with the fixes of %s, applied live.", running, ka->fixes);
	} else if (running[0]) {
		SET(detail, "Linux %s, the newest there is for it.", running);
	} else {
		SET(detail, "Nothing to do.");
	}
}

/* ---------- the shield ---------- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


static void shield_path(cairo_t *cr, double x, double y, double s) {
	double cx = x + s / 2;
	cairo_new_path(cr);
	cairo_move_to(cr, cx, y + s * 0.04);
	cairo_curve_to(cr, cx + s * 0.18, y + s * 0.14, cx + s * 0.32, y + s * 0.16, x + s * 0.88, y + s * 0.16);
	cairo_curve_to(cr, x + s * 0.88, y + s * 0.58, x + s * 0.74, y + s * 0.82, cx, y + s * 0.97);
	cairo_curve_to(cr, x + s * 0.26, y + s * 0.82, x + s * 0.12, y + s * 0.58, x + s * 0.12, y + s * 0.16);
	cairo_curve_to(cr, cx - s * 0.32, y + s * 0.16, cx - s * 0.18, y + s * 0.14, cx, y + s * 0.04);
	cairo_close_path(cr);
}

/*
 * A shield in the color of the state with its sign in it: a tick, an arrow
 * down, dots while busy, the arrow of a restart, "!" or a bar for off. Flat
 * with a dark edge in Windows 95, with a light top elsewhere.
 */
void tw_kernel_auto_draw_shield(cairo_t *cr, double x, double y, double s, enum tw_kernel_auto_kind kind,
		uint32_t color, bool flat) {
	cairo_save(cr);
	shield_path(cr, x, y, s);
	if (flat) {
		cairo_set_source_rgba(cr, (color >> 24 & 0xff) / 255.0, (color >> 16 & 0xff) / 255.0,
			(color >> 8 & 0xff) / 255.0, 1);
		cairo_fill_preserve(cr);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + s);
		double r = (color >> 24 & 0xff) / 255.0, gr = (color >> 16 & 0xff) / 255.0,
			b = (color >> 8 & 0xff) / 255.0;
		cairo_pattern_add_color_stop_rgb(g, 0, r + (1 - r) * 0.35, gr + (1 - gr) * 0.35, b + (1 - b) * 0.35);
		cairo_pattern_add_color_stop_rgb(g, 1, r * 0.85, gr * 0.85, b * 0.85);
		cairo_set_source(cr, g);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(g);
	}
	cairo_set_source_rgba(cr, 0, 0, 0, flat ? 0.75 : 0.35);
	cairo_set_line_width(cr, fmax(1, s / 18));
	cairo_stroke(cr);

	double cx = x + s / 2, cy = y + s * 0.5, u = s / 10;
	// the sign white, or black on a light shield (the dark scheme of the Atari ST)
	double light = ((color >> 24 & 0xff) * 0.299 + (color >> 16 & 0xff) * 0.587 +
		(color >> 8 & 0xff) * 0.114) / 255;
	double sign = light > 0.7 ? 0 : 1;
	cairo_set_source_rgb(cr, sign, sign, sign);
	cairo_set_line_width(cr, fmax(1.4, s / 9));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_new_path(cr);
	switch (kind) {
	case TW_KA_OK:
	case TW_KA_FIXED:
		cairo_move_to(cr, cx - 2.2 * u, cy);
		cairo_line_to(cr, cx - 0.6 * u, cy + 1.7 * u);
		cairo_line_to(cr, cx + 2.4 * u, cy - 1.7 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_DOWNLOADING:
		cairo_move_to(cr, cx, cy - 2.4 * u);
		cairo_line_to(cr, cx, cy + 1.8 * u);
		cairo_move_to(cr, cx - 1.7 * u, cy + 0.3 * u);
		cairo_line_to(cr, cx, cy + 2 * u);
		cairo_line_to(cr, cx + 1.7 * u, cy + 0.3 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_RESTART:
		cairo_arc(cr, cx, cy + 0.2 * u, 2.2 * u, -M_PI * 0.35, M_PI * 1.45);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 0.9 * u, cy - 2.6 * u);
		cairo_line_to(cr, cx + 1.9 * u, cy - 1.6 * u);
		cairo_line_to(cr, cx + 0.6 * u, cy - 1.0 * u);
		cairo_stroke(cr);
		break;
	case TW_KA_ERROR:
		cairo_move_to(cr, cx, cy - 2.4 * u);
		cairo_line_to(cr, cx, cy + 0.6 * u);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy + 2.2 * u, s / 16, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case TW_KA_OFF:
	case TW_KA_WAITING:
	case TW_KA_PENDING:
		cairo_move_to(cr, cx - 2 * u, cy);
		cairo_line_to(cr, cx + 2 * u, cy);
		cairo_stroke(cr);
		break;
	default: // busy: three dots
		for (int i = -1; i <= 1; i++) {
			cairo_new_path(cr);
			cairo_arc(cr, cx + i * 1.9 * u, cy, s / 14, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		break;
	}
	cairo_restore(cr);
}

