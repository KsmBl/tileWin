#include <stdio.h>
#include <string.h>
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
	GtkWidget *format_dd, *format_entry;
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

static void on_format_text(GtkEditable *editable, gpointer data) {
	struct datetime_page *p = data;
	if (p->updating) {
		return;
	}
	char *value = ui_input_value(gtk_editable_get_text(editable));
	write_format(p, *value ? value : NULL);
	g_free(value);
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
	p->zone_row = ui_row(group, "Time zone", "Type to search", p->zone_dd);

	p->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->status), 0);
	gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
	gtk_widget_add_css_class(p->status, "tw-heading");
	gtk_widget_set_margin_top(p->status, 12);
	gtk_widget_set_visible(p->status, FALSE);
	gtk_box_append(GTK_BOX(content), p->status);

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
	ui_row(bar, "Own format", "strftime format, \\n starts a second line", p->format_entry);

	p->updating = true; // filling the list selects its first entry
	load_zones(p);
	p->updating = false;
	show_format(p);
	show_state(p);
	p->timer = g_timeout_add_seconds(1, on_tick, p);
	g_signal_connect(page, "destroy", G_CALLBACK(on_destroy), p);
	return page;
}
