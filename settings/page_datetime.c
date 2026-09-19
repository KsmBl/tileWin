#include <math.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "settings.h"

/*
 * Date & time page: the system clock and time zone over systemd-timedated
 * (org.freedesktop.timedate1, the same service timedatectl uses), and the
 * format of the clock in the taskbar. Changing the clock needs authorization,
 * which polkit asks for; the reply is shown when it fails.
 */

#define TIMEDATE "org.freedesktop.timedate1"
#define TIMEDATE_PATH "/org/freedesktop/timedate1"

struct datetime_page {
	struct settings *s;
	bool updating;
	GDBusProxy *proxy;
	GtkWidget *now, *ntp_row, *ntp_switch, *zone_dd, *zone_row;
	GtkWidget *manual, *calendar, *hour, *minute, *second, *apply, *status;
	GtkWidget *format_dd, *format_entry, *code_popover, *code_list;
	GtkWidget *ntp_servers, *ntp_mode_dd, *ntp_fetch, *zone_map;
	GtkStringList *zones;
	guint timer;
	char *zone;
};

static struct confdoc *doc(struct datetime_page *p) {
	return p->s->taskbar;
}

static void set_status(struct datetime_page *p, const char *text) {
	gtk_label_set_text(GTK_LABEL(p->status), text ? text : "");
	gtk_widget_set_visible(p->status, text && *text);
}

/* ---------- the clock of the taskbar ---------- */

static const struct {
	const char *label, *format;
} formats[] = {
	{ "From the theme", NULL },
	{ "24-hour (13:45)", "%H:%M" },
	{ "24-hour with seconds (13:45:30)", "%H:%M:%S" },
	{ "12-hour (1:45 PM)", "%-I:%M %p" },
	{ "12-hour with seconds (1:45:30 PM)", "%-I:%M:%S %p" },
	{ "Time and date (13:45 / 16.09.2026)", "%H:%M\\n%d.%m.%Y" },
	{ "Time and weekday (13:45 / Wed)", "%H:%M\\n%a" },
};

static char *clock_format(struct datetime_page *p) {
	struct cstmt *block = confdoc_block(doc(p), "widget", "clock", false);
	return cstmt_join(confdoc_child(block, "format", NULL), 0);
}

/* format is written the way the taskbar page writes it: "\n" means a newline. */
static void write_format(struct datetime_page *p, const char *format) {
	struct confdoc *d = doc(p);
	struct cstmt *block = confdoc_block(d, "widget", "clock", format != NULL);
	if (!block) {
		return;
	}
	char *value = format ? ui_input_value(format) : NULL;
	char *quoted = value ? conf_quote_command(value) : NULL;
	g_free(value);
	confdoc_set(d, block, "format", NULL, quoted);
	g_free(quoted);
	settings_taskbar_changed(p->s);
}

static void show_format(struct datetime_page *p) {
	char *format = clock_format(p);
	char *display = ui_display_value(format);
	guint selected = G_N_ELEMENTS(formats); // "Custom"
	if (!format || !*format) {
		selected = 0;
	} else {
		for (guint i = 1; i < G_N_ELEMENTS(formats); i++) {
			if (strcmp(display, formats[i].format) == 0) {
				selected = i;
			}
		}
	}
	p->updating = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->format_dd), selected);
	gtk_editable_set_text(GTK_EDITABLE(p->format_entry), display);
	gtk_widget_set_sensitive(p->format_entry, selected == G_N_ELEMENTS(formats));
	g_free(display);
	p->updating = false;
	g_free(format);
}

static void on_format_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct datetime_page *p = data;
	if (p->updating) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (i < G_N_ELEMENTS(formats)) {
		write_format(p, formats[i].format);
	}
	show_format(p);
}

static void format_entry_changed(struct datetime_page *p);

static void on_format_text(GtkEditable *editable, gpointer data) {
	struct datetime_page *p = data;
	if (p->updating) {
		return;
	}
	char *value = ui_input_value(gtk_editable_get_text(editable));
	write_format(p, *value ? value : NULL);
	g_free(value);
	format_entry_changed(p);
}

/* ---------- asking time servers what time it is ---------- */

/*
 * A plain SNTP request to each server at once (RFC 4330): a 48 byte packet
 * goes out, the reply carries the time it was sent. How the answers are used
 * is up to the mode: the first server that is set up, the one that answers
 * quickest, or the middle of all of them.
 */

#define NTP_PORT "123"
#define NTP_TIMEOUT_MS 2500
#define NTP_EPOCH_OFFSET 2208988800ULL // seconds between 1900 and 1970
#define NTP_MAX_SERVERS 8

static const char *const ntp_mode_values[] = { "first", "fastest", "average" };
static const char *const ntp_mode_labels[] = {
	"The first one that answers, in order",
	"The one that answers quickest",
	"The middle of all the answers",
	NULL,
};

struct ntp_answer {
	char server[128];
	double unix_time;   // what the server said, in seconds
	double latency_ms;
	bool answered;
};

static double timespec_seconds(const struct timespec *t) {
	return t->tv_sec + t->tv_nsec / 1e9;
}

/* Sends the request and remembers when, or reports that it could not. */
static int ntp_send(const char *server, struct timespec *sent) {
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *found = NULL;
	if (getaddrinfo(server, NTP_PORT, &hints, &found) != 0 || !found) {
		return -1;
	}
	int fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		freeaddrinfo(found);
		return -1;
	}
	unsigned char packet[48] = { 0 };
	packet[0] = 0x1b; // no leap warning, version 3, client
	clock_gettime(CLOCK_REALTIME, sent);
	ssize_t n = sendto(fd, packet, sizeof(packet), 0, found->ai_addr, found->ai_addrlen);
	freeaddrinfo(found);
	if (n != (ssize_t)sizeof(packet)) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool ntp_receive(int fd, const struct timespec *sent, struct ntp_answer *answer) {
	unsigned char packet[48];
	if (recv(fd, packet, sizeof(packet), 0) != (ssize_t)sizeof(packet)) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	// the transmit timestamp sits at byte 40: seconds then a fraction
	unsigned long long seconds = ((unsigned long long)packet[40] << 24) |
		((unsigned long long)packet[41] << 16) | ((unsigned long long)packet[42] << 8) |
		packet[43];
	unsigned long long fraction = ((unsigned long long)packet[44] << 24) |
		((unsigned long long)packet[45] << 16) | ((unsigned long long)packet[46] << 8) |
		packet[47];
	if (seconds < NTP_EPOCH_OFFSET) {
		return false;
	}
	answer->latency_ms = (timespec_seconds(&now) - timespec_seconds(sent)) * 1000;
	// the reply is that old by the time it arrives, so add half the round trip
	answer->unix_time = (double)(seconds - NTP_EPOCH_OFFSET) + fraction / 4294967296.0 +
		answer->latency_ms / 2000.0;
	answer->answered = true;
	return true;
}

/* Asks every server at once and waits for as many answers as come back. */
static int ntp_ask(char **servers, int count, struct ntp_answer *answers) {
	int fds[NTP_MAX_SERVERS];
	struct timespec sent[NTP_MAX_SERVERS];
	int open_count = 0;
	for (int i = 0; i < count; i++) {
		snprintf(answers[i].server, sizeof(answers[i].server), "%s", servers[i]);
		answers[i].answered = false;
		fds[i] = ntp_send(servers[i], &sent[i]);
		open_count += fds[i] >= 0;
	}
	struct timespec started;
	clock_gettime(CLOCK_MONOTONIC, &started);
	int answered = 0;
	while (answered < open_count) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double waited = (timespec_seconds(&now) - timespec_seconds(&started)) * 1000;
		if (waited >= NTP_TIMEOUT_MS) {
			break;
		}
		fd_set set;
		FD_ZERO(&set);
		int highest = -1;
		for (int i = 0; i < count; i++) {
			if (fds[i] >= 0 && !answers[i].answered) {
				FD_SET(fds[i], &set);
				highest = fds[i] > highest ? fds[i] : highest;
			}
		}
		if (highest < 0) {
			break;
		}
		struct timeval left = {
			.tv_sec = (time_t)((NTP_TIMEOUT_MS - waited) / 1000),
			.tv_usec = (suseconds_t)(((long)(NTP_TIMEOUT_MS - waited) % 1000) * 1000),
		};
		if (select(highest + 1, &set, NULL, NULL, &left) <= 0) {
			break;
		}
		for (int i = 0; i < count; i++) {
			if (fds[i] >= 0 && !answers[i].answered && FD_ISSET(fds[i], &set) &&
					ntp_receive(fds[i], &sent[i], &answers[i])) {
				answered++;
			}
		}
	}
	for (int i = 0; i < count; i++) {
		if (fds[i] >= 0) {
			close(fds[i]);
		}
	}
	return answered;
}

static int compare_times(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

/* Boils the answers down to the one time to set, following the mode. */
static bool ntp_pick(struct ntp_answer *answers, int count, const char *mode,
		double *result, char *chosen, size_t chosen_size) {
	if (strcmp(mode, "fastest") == 0) {
		int best = -1;
		for (int i = 0; i < count; i++) {
			if (answers[i].answered &&
					(best < 0 || answers[i].latency_ms < answers[best].latency_ms)) {
				best = i;
			}
		}
		if (best < 0) {
			return false;
		}
		*result = answers[best].unix_time;
		snprintf(chosen, chosen_size, "%s, %.0f ms", answers[best].server,
			answers[best].latency_ms);
		return true;
	}
	if (strcmp(mode, "average") == 0) {
		double times[NTP_MAX_SERVERS];
		int n = 0;
		for (int i = 0; i < count; i++) {
			if (answers[i].answered) {
				times[n++] = answers[i].unix_time;
			}
		}
		if (n == 0) {
			return false;
		}
		// the middle one, so a single wrong clock cannot drag the result
		qsort(times, n, sizeof(times[0]), compare_times);
		*result = n % 2 ? times[n / 2] : (times[n / 2 - 1] + times[n / 2]) / 2;
		snprintf(chosen, chosen_size, "the middle of %d answers", n);
		return true;
	}
	for (int i = 0; i < count; i++) {
		if (answers[i].answered) {
			*result = answers[i].unix_time;
			snprintf(chosen, chosen_size, "%s, %.0f ms", answers[i].server,
				answers[i].latency_ms);
			return true;
		}
	}
	return false;
}

/* ---------- the system clock ---------- */

static void call_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct datetime_page *p = data;
	GError *error = NULL;
	GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
	if (reply) {
		g_variant_unref(reply);
		set_status(p, NULL);
	} else if (error) {
		g_dbus_error_strip_remote_error(error);
		set_status(p, error->message);
		g_error_free(error);
	}
}

static void call(struct datetime_page *p, const char *method, GVariant *params) {
	if (!p->proxy) {
		set_status(p, "systemd-timedated isn't available on this system.");
		return;
	}
	set_status(p, NULL);
	g_dbus_proxy_call(p->proxy, method, params, G_DBUS_CALL_FLAGS_NONE, 120000, NULL,
		call_done, p);
}



/* ---------- picking the time zone off a map ---------- */

/*
 * tzdata ships the position of every zone in zone.tab, so the map is drawn
 * from those: a dot per zone, colored by the part of the world it belongs to,
 * on an equirectangular grid. Clicking takes the nearest zone. There is no
 * coastline behind it; the dots follow where people live, which is enough to
 * find your corner of the world.
 */

#define TZ_MAP_WIDTH 640
#define TZ_MAP_HEIGHT 320

struct tz_place {
	char *zone;
	double latitude, longitude;
};

/* "+5230+01322" or "+353916+1394441" (ISO 6709). */
static bool parse_coordinates(const char *text, double *latitude, double *longitude) {
	if (!text || (text[0] != '+' && text[0] != '-')) {
		return false;
	}
	const char *split = strpbrk(text + 1, "+-");
	if (!split) {
		return false;
	}
	size_t lat_digits = split - text - 1;
	size_t lon_digits = strlen(split) - 1;
	if ((lat_digits != 4 && lat_digits != 6) || (lon_digits != 5 && lon_digits != 7)) {
		return false;
	}
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%.*s", (int)lat_digits, text + 1);
	double degrees = atof(buffer) / (lat_digits == 4 ? 100 : 10000);
	double minutes = lat_digits == 4 ? fmod(atof(buffer), 100) :
		fmod(atof(buffer) / 100, 100);
	double seconds = lat_digits == 6 ? fmod(atof(buffer), 100) : 0;
	*latitude = ((int)degrees + minutes / 60 + seconds / 3600) * (text[0] == '-' ? -1 : 1);
	snprintf(buffer, sizeof(buffer), "%.*s", (int)lon_digits, split + 1);
	degrees = atof(buffer) / (lon_digits == 5 ? 100 : 10000);
	minutes = lon_digits == 5 ? fmod(atof(buffer), 100) : fmod(atof(buffer) / 100, 100);
	seconds = lon_digits == 7 ? fmod(atof(buffer), 100) : 0;
	*longitude = ((int)degrees + minutes / 60 + seconds / 3600) * (split[0] == '-' ? -1 : 1);
	return true;
}

static GPtrArray *tz_places(void) {
	static GPtrArray *places = NULL;
	if (places) {
		return places;
	}
	places = g_ptr_array_new();
	char *text = NULL;
	if (!g_file_get_contents("/usr/share/zoneinfo/zone.tab", &text, NULL, NULL)) {
		return places;
	}
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (lines[i][0] == '#' || !lines[i][0]) {
			continue;
		}
		char **fields = g_strsplit(lines[i], "\t", -1);
		double latitude, longitude;
		if (fields[0] && fields[1] && fields[2] &&
				parse_coordinates(fields[1], &latitude, &longitude)) {
			struct tz_place *place = g_new0(struct tz_place, 1);
			place->zone = g_strdup(fields[2]);
			place->latitude = latitude;
			place->longitude = longitude;
			g_ptr_array_add(places, place);
		}
		g_strfreev(fields);
	}
	g_strfreev(lines);
	g_free(text);
	return places;
}

/* A color per part of the world, so the dots read as continents. */
static void tz_region_color(const char *zone, double *r, double *g, double *b) {
	static const struct {
		const char *prefix;
		double r, g, b;
	} regions[] = {
		{ "Europe/", 0.36, 0.66, 0.94 },
		{ "America/", 0.42, 0.80, 0.52 },
		{ "Asia/", 0.96, 0.72, 0.34 },
		{ "Africa/", 0.93, 0.52, 0.42 },
		{ "Australia/", 0.72, 0.56, 0.92 },
		{ "Pacific/", 0.40, 0.82, 0.82 },
		{ "Atlantic/", 0.58, 0.70, 0.88 },
		{ "Indian/", 0.88, 0.62, 0.76 },
		{ "Antarctica/", 0.78, 0.82, 0.86 },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(regions); i++) {
		if (g_str_has_prefix(zone, regions[i].prefix)) {
			*r = regions[i].r;
			*g = regions[i].g;
			*b = regions[i].b;
			return;
		}
	}
	*r = *g = *b = 0.7;
}

static void tz_to_pixels(double latitude, double longitude, int width, int height,
		double *x, double *y) {
	*x = (longitude + 180) / 360.0 * width;
	*y = (90 - latitude) / 180.0 * height;
}

static void tz_map_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	struct datetime_page *p = data;
	cairo_set_source_rgb(cr, 0.12, 0.15, 0.20);
	cairo_paint(cr);
	// the grid: every 30 degrees across and 30 down, with the equator picked out
	cairo_set_line_width(cr, 1);
	for (int longitude = -180; longitude <= 180; longitude += 30) {
		double x, y;
		tz_to_pixels(0, longitude, width, height, &x, &y);
		cairo_set_source_rgba(cr, 1, 1, 1, longitude == 0 ? 0.22 : 0.09);
		cairo_move_to(cr, x, 0);
		cairo_line_to(cr, x, height);
		cairo_stroke(cr);
	}
	for (int latitude = -60; latitude <= 60; latitude += 30) {
		double x, y;
		tz_to_pixels(latitude, 0, width, height, &x, &y);
		cairo_set_source_rgba(cr, 1, 1, 1, latitude == 0 ? 0.22 : 0.09);
		cairo_move_to(cr, 0, y);
		cairo_line_to(cr, width, y);
		cairo_stroke(cr);
	}
	GPtrArray *places = tz_places();
	const char *selected = p->zone;
	double selected_x = -1, selected_y = -1;
	for (guint i = 0; i < places->len; i++) {
		struct tz_place *place = places->pdata[i];
		double x, y, r, g, b;
		tz_to_pixels(place->latitude, place->longitude, width, height, &x, &y);
		tz_region_color(place->zone, &r, &g, &b);
		bool is_selected = selected && strcmp(place->zone, selected) == 0;
		if (is_selected) {
			selected_x = x;
			selected_y = y;
			continue; // drawn last, on top
		}
		cairo_set_source_rgba(cr, r, g, b, 0.75);
		cairo_arc(cr, x, y, 2.0, 0, 2 * G_PI);
		cairo_fill(cr);
	}
	if (selected_x >= 0) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_arc(cr, selected_x, selected_y, 5.5, 0, 2 * G_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.10, 0.45, 0.85);
		cairo_arc(cr, selected_x, selected_y, 3.0, 0, 2 * G_PI);
		cairo_fill(cr);
	}
}

/* The zone whose place is nearest to where the map was clicked. */
static const char *tz_nearest(double x, double y, int width, int height) {
	GPtrArray *places = tz_places();
	const char *best = NULL;
	double best_distance = 0;
	for (guint i = 0; i < places->len; i++) {
		struct tz_place *place = places->pdata[i];
		double px, py;
		tz_to_pixels(place->latitude, place->longitude, width, height, &px, &py);
		double distance = (px - x) * (px - x) + (py - y) * (py - y);
		if (!best || distance < best_distance) {
			best = place->zone;
			best_distance = distance;
		}
	}
	return best;
}

static void on_tz_map_pressed(GtkGestureClick *gesture, int presses, double x, double y,
		gpointer data) {
	struct datetime_page *p = data;
	int width = gtk_widget_get_width(p->zone_map);
	int height = gtk_widget_get_height(p->zone_map);
	const char *zone = width > 0 && height > 0 ? tz_nearest(x, y, width, height) : NULL;
	if (!zone) {
		return;
	}
	for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(p->zones)); i++) {
		const char *name = gtk_string_list_get_string(p->zones, i);
		if (name && strcmp(name, zone) == 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->zone_dd), i);
			return;
		}
	}
}

/* ---------- the "get the time now" button ---------- */

#define NTP_DEFAULT_SERVERS "0.pool.ntp.org 1.pool.ntp.org time.cloudflare.com"

struct ntp_job {
	char *servers;
	char *mode;
	double result;
	char chosen[160];
	int answered, asked;
	bool ok;
};

static void ntp_job_free(gpointer data) {
	struct ntp_job *job = data;
	g_free(job->servers);
	g_free(job->mode);
	g_free(job);
}

/* Runs off the main thread, so the window keeps drawing while it waits. */
static void ntp_job_run(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
	struct ntp_job *job = data;
	char **list = g_strsplit_set(job->servers, " \t,", -1);
	char *servers[NTP_MAX_SERVERS];
	int count = 0;
	for (int i = 0; list[i] && count < NTP_MAX_SERVERS; i++) {
		if (*list[i]) {
			servers[count++] = list[i];
		}
	}
	job->asked = count;
	struct ntp_answer answers[NTP_MAX_SERVERS];
	job->answered = count ? ntp_ask(servers, count, answers) : 0;
	job->ok = job->answered > 0 && ntp_pick(answers, count, job->mode, &job->result,
		job->chosen, sizeof(job->chosen));
	g_strfreev(list);
	g_task_return_boolean(task, TRUE);
}

static void ntp_job_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct datetime_page *p = data;
	struct ntp_job *job = g_task_get_task_data(G_TASK(result));
	gtk_widget_set_sensitive(p->ntp_fetch, TRUE);
	if (!job->asked) {
		set_status(p, "Write at least one time server first.");
		return;
	}
	if (!job->ok) {
		set_status(p, "No time server answered.");
		return;
	}
	double difference = job->result - (double)time(NULL);
	call(p, "SetTime", g_variant_new("(xbb)", (gint64)(job->result * 1000000), FALSE, FALSE));
	settings_status(p->s, "Time from %s, %d of %d answered, this clock was %+.1f s off",
		job->chosen, job->answered, job->asked, difference);
}

static void on_ntp_fetch(GtkButton *button, gpointer data) {
	struct datetime_page *p = data;
	struct ntp_job *job = g_new0(struct ntp_job, 1);
	job->servers = g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->ntp_servers)));
	guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->ntp_mode_dd));
	job->mode = g_strdup(mode < G_N_ELEMENTS(ntp_mode_values) ? ntp_mode_values[mode] : "first");
	gtk_widget_set_sensitive(p->ntp_fetch, FALSE);
	set_status(p, "Asking the time servers...");
	GTask *task = g_task_new(NULL, NULL, ntp_job_done, p);
	g_task_set_task_data(task, job, ntp_job_free);
	g_task_run_in_thread(task, ntp_job_run);
	g_object_unref(task);
}

static void ntp_settings_write(struct datetime_page *p) {
	if (p->updating) {
		return;
	}
	struct confdoc *d = p->s->taskbar;
	const char *servers = gtk_editable_get_text(GTK_EDITABLE(p->ntp_servers));
	char *quoted = conf_quote(servers);
	confdoc_set(d, d->root, "time_servers", NULL, *servers ? quoted : NULL);
	g_free(quoted);
	guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->ntp_mode_dd));
	confdoc_set(d, d->root, "time_server_mode", NULL,
		mode < G_N_ELEMENTS(ntp_mode_values) ? ntp_mode_values[mode] : "first");
	settings_taskbar_changed(p->s);
}

static void on_ntp_servers_changed(GtkEditable *editable, gpointer data) {
	ntp_settings_write(data);
}

static void on_ntp_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	ntp_settings_write(data);
}

static bool proxy_bool(struct datetime_page *p, const char *name) {
	GVariant *value = p->proxy ? g_dbus_proxy_get_cached_property(p->proxy, name) : NULL;
	bool result = value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
		g_variant_get_boolean(value);
	if (value) {
		g_variant_unref(value);
	}
	return result;
}

static void show_clock(struct datetime_page *p) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char text[160], zone[64];
	strftime(text, sizeof(text), "%H:%M:%S", &tm);
	strftime(zone, sizeof(zone), "%A, %d %B %Y (%Z, UTC%z)", &tm);
	char *markup = g_markup_printf_escaped("<span size='xx-large'>%s</span>\n%s", text, zone);
	gtk_label_set_markup(GTK_LABEL(p->now), markup);
	g_free(markup);
}

static gboolean on_tick(gpointer data) {
	show_clock(data);
	return G_SOURCE_CONTINUE;
}

/* The time zone of /etc/localtime, for systems without systemd-timedated. */
static char *system_zone(void) {
	char *link = g_file_read_link("/etc/localtime", NULL);
	const char *zone = link ? strstr(link, "/zoneinfo/") : NULL;
	char *result = zone ? g_strdup(zone + strlen("/zoneinfo/")) : NULL;
	g_free(link);
	if (!result && g_file_get_contents("/etc/timezone", &result, NULL, NULL)) {
		result = g_strstrip(result);
	}
	return result;
}

static void show_state(struct datetime_page *p) {
	p->updating = true;
	if (p->ntp_servers) {
		struct confdoc *d = p->s->taskbar;
		const char *list = cstmt_arg(confdoc_child(d->root, "time_servers", NULL), 0);
		gtk_editable_set_text(GTK_EDITABLE(p->ntp_servers), list ? list : NTP_DEFAULT_SERVERS);
		const char *mode = cstmt_arg(confdoc_child(d->root, "time_server_mode", NULL), 0);
		guint selected = 0;
		for (guint i = 0; mode && i < G_N_ELEMENTS(ntp_mode_values); i++) {
			if (strcmp(mode, ntp_mode_values[i]) == 0) {
				selected = i;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->ntp_mode_dd), selected);
	}
	bool ntp = proxy_bool(p, "NTP");
	bool can_ntp = proxy_bool(p, "CanNTP");
	gtk_switch_set_active(GTK_SWITCH(p->ntp_switch), ntp);
	gtk_switch_set_state(GTK_SWITCH(p->ntp_switch), ntp);
	gtk_widget_set_sensitive(p->ntp_switch, p->proxy && can_ntp);
	gtk_widget_set_sensitive(p->manual, !ntp);
	gtk_widget_set_sensitive(p->apply, !ntp);

	GVariant *value = p->proxy ? g_dbus_proxy_get_cached_property(p->proxy, "Timezone") : NULL;
	g_free(p->zone);
	p->zone = value ? g_variant_dup_string(value, NULL) : system_zone();
	if (value) {
		g_variant_unref(value);
	}
	for (guint i = 0; p->zone && i < g_list_model_get_n_items(G_LIST_MODEL(p->zones)); i++) {
		const char *name = gtk_string_list_get_string(p->zones, i);
		if (g_strcmp0(name, p->zone) == 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->zone_dd), i);
			break;
		}
	}
	if (p->zone_map) {
		gtk_widget_queue_draw(p->zone_map);
	}
	p->updating = false;
	show_clock(p);
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
		gpointer data) {
	show_state(data);
}

static gboolean on_ntp(GtkSwitch *widget, gboolean state, gpointer data) {
	struct datetime_page *p = data;
	if (!p->updating) {
		call(p, "SetNTP", g_variant_new("(bb)", state, TRUE));
	}
	return TRUE; // the state follows the property, not the click
}

static void on_zone_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct datetime_page *p = data;
	if (p->updating) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	const char *zone = i == GTK_INVALID_LIST_POSITION ? NULL :
		gtk_string_list_get_string(p->zones, i);
	// p->zone is the zone of the system: without it nothing was read yet
	if (zone && p->zone && g_strcmp0(zone, p->zone) != 0) {
		call(p, "SetTimezone", g_variant_new("(sb)", zone, TRUE));
	}
}

static void on_apply(GtkButton *button, gpointer data) {
	struct datetime_page *p = data;
	GDateTime *day = gtk_calendar_get_date(GTK_CALENDAR(p->calendar));
	struct tm tm = {
		.tm_year = g_date_time_get_year(day) - 1900,
		.tm_mon = g_date_time_get_month(day) - 1,
		.tm_mday = g_date_time_get_day_of_month(day),
		.tm_hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->hour)),
		.tm_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->minute)),
		.tm_sec = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->second)),
		.tm_isdst = -1,
	};
	g_date_time_unref(day);
	time_t when = mktime(&tm);
	if (when == (time_t)-1) {
		set_status(p, "That is not a date this computer can be set to.");
		return;
	}
	call(p, "SetTime", g_variant_new("(xbb)", (gint64)when * 1000000, FALSE, TRUE));
}

static void on_now(GtkButton *button, gpointer data) {
	struct datetime_page *p = data;
	GDateTime *now = g_date_time_new_now_local();
	gtk_calendar_set_day(GTK_CALENDAR(p->calendar), g_date_time_get_day_of_month(now));
	gtk_calendar_set_month(GTK_CALENDAR(p->calendar), g_date_time_get_month(now) - 1);
	gtk_calendar_set_year(GTK_CALENDAR(p->calendar), g_date_time_get_year(now));
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->hour), g_date_time_get_hour(now));
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->minute), g_date_time_get_minute(now));
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->second), g_date_time_get_second(now));
	g_date_time_unref(now);
}

static int compare_names(gconstpointer a, gconstpointer b) {
	return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

/* The time zones of this system, from systemd or from the zoneinfo database. */
static void load_zones(struct datetime_page *p) {
	GVariant *reply = p->proxy ? g_dbus_proxy_call_sync(p->proxy, "ListTimezones", NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL) : NULL;
	if (reply) {
		GVariantIter *iter = NULL;
		const char *zone = NULL;
		g_variant_get(reply, "(as)", &iter);
		while (iter && g_variant_iter_next(iter, "&s", &zone)) {
			gtk_string_list_append(p->zones, zone);
		}
		if (iter) {
			g_variant_iter_free(iter);
		}
		g_variant_unref(reply);
	}
	if (g_list_model_get_n_items(G_LIST_MODEL(p->zones)) > 0) {
		return;
	}
	char *text = NULL;
	if (!g_file_get_contents("/usr/share/zoneinfo/zone1970.tab", &text, NULL, NULL) &&
			!g_file_get_contents("/usr/share/zoneinfo/zone.tab", &text, NULL, NULL)) {
		return;
	}
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (lines[i][0] == '#') {
			continue;
		}
		char **fields = g_strsplit(lines[i], "\t", -1);
		if (fields[0] && fields[1] && fields[2] && *fields[2]) {
			g_ptr_array_add(names, g_strdup(fields[2]));
		}
		g_strfreev(fields);
	}
	g_strfreev(lines);
	g_free(text);
	g_ptr_array_sort(names, compare_names);
	gtk_string_list_append(p->zones, "UTC");
	for (guint i = 0; i < names->len; i++) {
		gtk_string_list_append(p->zones, names->pdata[i]);
	}
	g_ptr_array_unref(names);
}

static void on_destroy(GtkWidget *widget, gpointer data) {
	struct datetime_page *p = data;
	if (p->timer) {
		g_source_remove(p->timer);
		p->timer = 0;
	}
}

void datetime_page_refresh(struct settings *s) {
	if (s->datetime_page) {
		show_format(s->datetime_page);
		show_state(s->datetime_page);
	}
}

/* ---------- the codes a clock format can hold ---------- */

static const struct {
	char code;
	const char *what;
} format_codes[] = {
	{ 'H', "Hour, 00 to 23" },
	{ 'I', "Hour, 01 to 12" },
	{ 'M', "Minute, 00 to 59" },
	{ 'S', "Second, 00 to 59" },
	{ 'p', "AM or PM" },
	{ 'P', "am or pm" },
	{ 'd', "Day of the month, 01 to 31" },
	{ 'e', "Day of the month, 1 to 31" },
	{ 'm', "Month, 01 to 12" },
	{ 'y', "Year, two digits" },
	{ 'Y', "Year, four digits" },
	{ 'a', "Weekday, short" },
	{ 'A', "Weekday, full" },
	{ 'b', "Month name, short" },
	{ 'B', "Month name, full" },
	{ 'j', "Day of the year, 001 to 366" },
	{ 'V', "Week of the year" },
	{ 'Z', "Time zone, short" },
	{ 'z', "Time zone, +0100" },
	{ 'x', "Date, as your language writes it" },
	{ 'X', "Time, as your language writes it" },
	{ 'c', "Date and time together" },
	{ 's', "Seconds since 1970" },
	{ '%', "A percent sign" },
};

/* What every code stands for, and what it looks like right now. */
static char *format_codes_tooltip(void) {
	GString *text = g_string_new("Codes you can use, with what they show right now:\n");
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	for (size_t i = 0; i < G_N_ELEMENTS(format_codes); i++) {
		char pattern[4] = { '%', format_codes[i].code, '\0' };
		char rendered[128] = "";
		strftime(rendered, sizeof(rendered), pattern, &tm);
		g_string_append_printf(text, "\n%%%c  %-30s %s", format_codes[i].code,
			format_codes[i].what, rendered);
	}
	g_string_append(text, "\n\n\\n starts a second line.");
	return g_string_free(text, FALSE);
}

/* Puts the code the row stands for where the cursor is. */
static void on_code_chosen(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
	struct datetime_page *p = data;
	const char *code = g_object_get_data(G_OBJECT(row), "code");
	gtk_popover_popdown(GTK_POPOVER(p->code_popover));
	if (!code) {
		return;
	}
	int position = gtk_editable_get_position(GTK_EDITABLE(p->format_entry));
	// the "%" that opened the list is already there, so only the letter follows
	gtk_editable_insert_text(GTK_EDITABLE(p->format_entry), code, -1, &position);
	gtk_editable_set_position(GTK_EDITABLE(p->format_entry), position);
	gtk_widget_grab_focus(p->format_entry);
}

static void code_popover_build(struct datetime_page *p) {
	p->code_list = gtk_list_box_new();
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	for (size_t i = 0; i < G_N_ELEMENTS(format_codes); i++) {
		char pattern[4] = { '%', format_codes[i].code, '\0' };
		char rendered[128] = "";
		strftime(rendered, sizeof(rendered), pattern, &tm);
		char *label = g_strdup_printf("%%%c   %s", format_codes[i].code, format_codes[i].what);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_widget_set_margin_start(box, 8);
		gtk_widget_set_margin_end(box, 8);
		gtk_widget_set_margin_top(box, 3);
		gtk_widget_set_margin_bottom(box, 3);
		GtkWidget *left = gtk_label_new(label);
		gtk_label_set_xalign(GTK_LABEL(left), 0);
		gtk_widget_set_hexpand(left, TRUE);
		gtk_box_append(GTK_BOX(box), left);
		GtkWidget *right = gtk_label_new(rendered);
		gtk_widget_add_css_class(right, "dim-label");
		gtk_box_append(GTK_BOX(box), right);
		g_free(label);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		char code[2] = { format_codes[i].code, '\0' };
		g_object_set_data_full(G_OBJECT(row), "code", g_strdup(code), g_free);
		gtk_list_box_append(GTK_LIST_BOX(p->code_list), row);
	}
	g_signal_connect(p->code_list, "row-activated", G_CALLBACK(on_code_chosen), p);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, 420, 320);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->code_list);
	p->code_popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(p->code_popover), scroll);
	gtk_popover_set_autohide(GTK_POPOVER(p->code_popover), FALSE);
	gtk_widget_set_parent(p->code_popover, p->format_entry);
	gtk_popover_set_position(GTK_POPOVER(p->code_popover), GTK_POS_BOTTOM);
}

/* Typing "%" offers the codes right there. */
static void format_entry_changed(struct datetime_page *p) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(p->format_entry));
	int position = gtk_editable_get_position(GTK_EDITABLE(p->format_entry));
	bool after_percent = position > 0 && text[position - 1] == '%' &&
		(position < 2 || text[position - 2] != '%');
	if (after_percent) {
		gtk_popover_popup(GTK_POPOVER(p->code_popover));
	} else {
		gtk_popover_popdown(GTK_POPOVER(p->code_popover));
	}
}

/* Hours, minutes and seconds are shown with two digits. */
static gboolean on_spin_leading_zero(GtkSpinButton *spin, gpointer data) {
	char text[8];
	snprintf(text, sizeof(text), "%02d", gtk_spin_button_get_value_as_int(spin));
	if (strcmp(gtk_editable_get_text(GTK_EDITABLE(spin)), text) != 0) {
		gtk_editable_set_text(GTK_EDITABLE(spin), text);
	}
	return TRUE;
}

GtkWidget *datetime_page_new(struct settings *s) {
	struct datetime_page *p = g_new0(struct datetime_page, 1);
	p->s = s;
	s->datetime_page = p;
	p->zones = gtk_string_list_new(NULL);
	p->proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
		TIMEDATE, TIMEDATE_PATH, TIMEDATE, NULL, NULL);
	if (p->proxy) {
		g_signal_connect(p->proxy, "g-properties-changed",
			G_CALLBACK(on_properties_changed), p);
	}

	GtkWidget *content;
	GtkWidget *page = ui_page("Date & time",
		"The clock of this computer, its time zone and how the taskbar shows them.", &content);

	GtkWidget *group = ui_group(content, "Date and time", NULL);
	p->now = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->now), 0);
	gtk_box_prepend(GTK_BOX(ui_row_box(ui_row(group, NULL, NULL, NULL))), p->now);

	p->ntp_switch = gtk_switch_new();
	g_signal_connect(p->ntp_switch, "state-set", G_CALLBACK(on_ntp), p);
	p->ntp_row = ui_row(group, "Set the time automatically",
		"Keeps the clock in step with a time server on the internet",
		p->ntp_switch);

	p->zone_dd = gtk_drop_down_new(G_LIST_MODEL(p->zones), NULL);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(p->zone_dd), TRUE);
	gtk_drop_down_set_expression(GTK_DROP_DOWN(p->zone_dd),
		gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
	g_signal_connect(p->zone_dd, "notify::selected", G_CALLBACK(on_zone_selected), p);
	p->zone_row = ui_row(group, "Time zone", "Type to search, or click the map", p->zone_dd);
	p->zone_map = gtk_drawing_area_new();
	gtk_widget_set_size_request(p->zone_map, TZ_MAP_WIDTH, TZ_MAP_HEIGHT);
	gtk_widget_set_halign(p->zone_map, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(p->zone_map, 4);
	gtk_widget_set_margin_bottom(p->zone_map, 4);
	gtk_widget_set_tooltip_text(p->zone_map,
		"Every zone tzdata knows, where it is on the world; click the nearest one");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->zone_map), tz_map_draw, p, NULL);
	GtkGesture *map_click = gtk_gesture_click_new();
	g_signal_connect(map_click, "pressed", G_CALLBACK(on_tz_map_pressed), p);
	gtk_widget_add_controller(p->zone_map, GTK_EVENT_CONTROLLER(map_click));
	gtk_box_prepend(GTK_BOX(ui_row_box(ui_row(group, NULL, NULL, NULL))), p->zone_map);

	p->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->status), 0);
	gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
	gtk_widget_add_css_class(p->status, "tw-heading");
	gtk_widget_set_margin_top(p->status, 12);
	gtk_widget_set_visible(p->status, FALSE);
	gtk_box_append(GTK_BOX(content), p->status);

	GtkWidget *servers = ui_group(content, "Time from the internet",
		"Ask these servers what time it is and set the clock to their answer.");
	p->ntp_servers = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->ntp_servers), NTP_DEFAULT_SERVERS);
	gtk_widget_set_size_request(p->ntp_servers, 300, -1);
	g_signal_connect(p->ntp_servers, "changed", G_CALLBACK(on_ntp_servers_changed), p);
	ui_row(servers, "Time servers", "Separated by spaces; all of them are asked at once",
		p->ntp_servers);
	p->ntp_mode_dd = gtk_drop_down_new_from_strings(ntp_mode_labels);
	g_signal_connect(p->ntp_mode_dd, "notify::selected", G_CALLBACK(on_ntp_mode_changed), p);
	ui_row(servers, "Which answer to use", NULL, p->ntp_mode_dd);
	p->ntp_fetch = gtk_button_new_with_label("Get the time now");
	g_signal_connect(p->ntp_fetch, "clicked", G_CALLBACK(on_ntp_fetch), p);
	ui_row(servers, NULL, "Sets the clock once; the switch above keeps it in step all the time",
		p->ntp_fetch);

	GtkWidget *set = ui_group(content, "Set the date and time yourself",
		"Only when the time is not set automatically.");
	p->manual = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_halign(p->manual, GTK_ALIGN_START);
	p->calendar = gtk_calendar_new();
	gtk_box_append(GTK_BOX(p->manual), p->calendar);
	GtkWidget *clock = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_valign(clock, GTK_ALIGN_CENTER);
	GtkWidget **spins[] = { &p->hour, &p->minute, &p->second };
	int limits[] = { 23, 59, 59 };
	for (int i = 0; i < 3; i++) {
		if (i) {
			gtk_box_append(GTK_BOX(clock), gtk_label_new(":"));
		}
		*spins[i] = gtk_spin_button_new_with_range(0, limits[i], 1);
		gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(*spins[i]), TRUE);
		gtk_editable_set_width_chars(GTK_EDITABLE(*spins[i]), 2);
		// 09:05:03 rather than 9:5:3
		g_signal_connect(*spins[i], "output", G_CALLBACK(on_spin_leading_zero), NULL);
		gtk_box_append(GTK_BOX(clock), *spins[i]);
	}
	gtk_box_append(GTK_BOX(p->manual), clock);
	GtkWidget *row = ui_row(set, NULL, NULL, NULL);
	gtk_box_prepend(GTK_BOX(ui_row_box(row)), p->manual);
	p->apply = gtk_button_new_with_label("Change the clock");
	g_signal_connect(p->apply, "clicked", G_CALLBACK(on_apply), p);
	GtkWidget *now_button = gtk_button_new_with_label("Fill in the current time");
	g_signal_connect(now_button, "clicked", G_CALLBACK(on_now), p);
	GtkWidget *buttons = ui_row(set, NULL, NULL, p->apply);
	gtk_box_prepend(GTK_BOX(ui_row_box(buttons)), now_button);
	on_now(NULL, p);

	GtkWidget *bar = ui_group(content, "Clock in the taskbar", NULL);
	GtkStringList *choices = gtk_string_list_new(NULL);
	for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
		gtk_string_list_append(choices, formats[i].label);
	}
	gtk_string_list_append(choices, "Custom");
	p->format_dd = gtk_drop_down_new(G_LIST_MODEL(choices), NULL);
	g_signal_connect(p->format_dd, "notify::selected", G_CALLBACK(on_format_selected), p);
	ui_row(bar, "Clock format", "Also decides whether the flyout shows a 12- or 24-hour time",
		p->format_dd);
	p->format_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->format_entry), "%H:%M");
	gtk_widget_set_size_request(p->format_entry, 300, -1);
	g_signal_connect(p->format_entry, "changed", G_CALLBACK(on_format_text), p);
	char *codes = format_codes_tooltip();
	gtk_widget_set_tooltip_text(p->format_entry, codes);
	g_free(codes);
	code_popover_build(p);
	ui_row(bar, "Own format",
		"Type % and pick a code from the list; hover the field for all of them",
		p->format_entry);

	p->updating = true; // filling the list selects its first entry
	load_zones(p);
	p->updating = false;
	show_format(p);
	show_state(p);
	p->timer = g_timeout_add_seconds(1, on_tick, p);
	g_signal_connect(page, "destroy", G_CALLBACK(on_destroy), p);
	return page;
}
