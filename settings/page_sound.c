#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/*
 * Sound page: the output and input device with their volume, and the volume of
 * every app that plays sound, like the sound settings and the volume mixer of
 * Windows. Everything goes through pactl (PulseAudio or PipeWire); the page
 * reads the state every two seconds while it is shown.
 */

#define POLL_MS 2000
#define HOLD_US (1500 * 1000) // reads don't move a slider the user just moved

struct device {
	char *name, *description;
	int volume;
	bool muted;
};

struct stream {
	int index;
	char *name, *icon;
	int volume;
	bool muted;
};

struct app_row {
	int index;
	GtkWidget *row, *scale, *mute;
	gint64 hold_until;
};

struct sound_page {
	struct settings *s;
	bool updating;
	GtkWidget *missing, *groups;
	GtkWidget *out_dd, *out_scale, *out_mute;
	GtkWidget *in_dd, *in_scale, *in_mute;
	GtkWidget *apps_group, *apps_empty;
	GPtrArray *sinks, *sources; // struct device *
	GPtrArray *app_rows;        // struct app_row *
	char *default_sink, *default_source;
	gint64 out_hold, in_hold;
	guint poll_timer, requery_timer;
	bool querying;
};

static void device_free(gpointer data) {
	struct device *d = data;
	g_free(d->name);
	g_free(d->description);
	g_free(d);
}

static void stream_free(gpointer data) {
	struct stream *st = data;
	g_free(st->name);
	g_free(st->icon);
	g_free(st);
}

static const char *jstr(json_object *obj, const char *key) {
	json_object *value;
	return obj && json_object_object_get_ex(obj, key, &value) &&
		json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

static const char *prop(json_object *obj, const char *key) {
	json_object *props;
	return json_object_object_get_ex(obj, "properties", &props) ? jstr(props, key) : NULL;
}

static int jvolume(json_object *obj) {
	json_object *volume;
	int sum = 0, count = 0;
	if (json_object_object_get_ex(obj, "volume", &volume) &&
			json_object_is_type(volume, json_type_object)) {
		json_object_object_foreach(volume, channel, value) {
			const char *percent = channel ? jstr(value, "value_percent") : NULL;
			if (percent) {
				sum += atoi(percent);
				count++;
			}
		}
	}
	return count ? sum / count : 0;
}

static bool jmute(json_object *obj) {
	json_object *mute;
	return json_object_object_get_ex(obj, "mute", &mute) && json_object_get_boolean(mute);
}

static void pactl(const char *a, const char *b, const char *c) {
	const char *argv[] = { "pactl", a, b, c, NULL };
	g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
		G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static void pactl_percent(const char *what, const char *target, int percent) {
	char value[16];
	snprintf(value, sizeof(value), "%d%%", percent);
	pactl(what, target, value);
}

static GPtrArray *parse_devices(const char *text, bool skip_monitors) {
	GPtrArray *devices = g_ptr_array_new_with_free_func(device_free);
	json_object *array = json_tokener_parse(text);
	for (size_t i = 0; array && json_object_is_type(array, json_type_array) &&
			i < json_object_array_length(array); i++) {
		json_object *obj = json_object_array_get_idx(array, i);
		const char *name = jstr(obj, "name");
		if (!name || (skip_monitors && g_str_has_suffix(name, ".monitor"))) {
			continue;
		}
		struct device *d = g_new0(struct device, 1);
		d->name = g_strdup(name);
		const char *description = jstr(obj, "description");
		d->description = g_strdup(description ? description : name);
		d->volume = jvolume(obj);
		d->muted = jmute(obj);
		g_ptr_array_add(devices, d);
	}
	json_object_put(array);
	return devices;
}

static GPtrArray *parse_streams(const char *text) {
	GPtrArray *streams = g_ptr_array_new_with_free_func(stream_free);
	json_object *array = json_tokener_parse(text);
	for (size_t i = 0; array && json_object_is_type(array, json_type_array) &&
			i < json_object_array_length(array); i++) {
		json_object *obj = json_object_array_get_idx(array, i), *index;
		if (!json_object_object_get_ex(obj, "index", &index)) {
			continue;
		}
		struct stream *st = g_new0(struct stream, 1);
		st->index = json_object_get_int(index);
		const char *name = prop(obj, "application.name");
		if (!name) {
			name = prop(obj, "media.name");
		}
		st->name = g_strdup(name ? name : "Unknown app");
		const char *icon = prop(obj, "application.icon_name");
		if (!icon) {
			icon = prop(obj, "application.process.binary");
		}
		st->icon = g_strdup(icon);
		st->volume = jvolume(obj);
		st->muted = jmute(obj);
		g_ptr_array_add(streams, st);
	}
	json_object_put(array);
	return streams;
}

/* ---------- showing the state ---------- */

static bool same_devices(GPtrArray *a, GPtrArray *b) {
	if (!a || !b || a->len != b->len) {
		return false;
	}
	for (guint i = 0; i < a->len; i++) {
		struct device *x = a->pdata[i], *y = b->pdata[i];
		if (strcmp(x->name, y->name) != 0 || strcmp(x->description, y->description) != 0) {
			return false;
		}
	}
	return true;
}

static struct device *find_device(GPtrArray *devices, const char *name, guint *index) {
	for (guint i = 0; devices && name && i < devices->len; i++) {
		struct device *d = devices->pdata[i];
		if (strcmp(d->name, name) == 0) {
			if (index) {
				*index = i;
			}
			return d;
		}
	}
	return NULL;
}

static void show_devices(struct sound_page *p, GtkWidget *dd, GtkWidget *scale, GtkWidget *mute,
		GPtrArray **current, GPtrArray *devices, const char *def, gint64 hold) {
	if (!same_devices(*current, devices)) {
		GtkStringList *model = gtk_string_list_new(NULL);
		for (guint i = 0; i < devices->len; i++) {
			gtk_string_list_append(model, ((struct device *)devices->pdata[i])->description);
		}
		gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(model));
		g_object_unref(model);
	}
	if (*current) {
		g_ptr_array_unref(*current);
	}
	*current = devices;
	guint index = 0;
	struct device *d = find_device(devices, def, &index);
	gtk_widget_set_sensitive(dd, devices->len > 0);
	gtk_widget_set_sensitive(scale, d != NULL);
	gtk_widget_set_sensitive(mute, d != NULL);
	if (!d) {
		return;
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), index);
	if (g_get_monotonic_time() > hold) {
		gtk_range_set_value(GTK_RANGE(scale), d->volume);
	}
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute), d->muted);
	gtk_button_set_icon_name(GTK_BUTTON(mute), d->muted ? "audio-volume-muted-symbolic" :
		"audio-volume-high-symbolic");
}

static void on_app_volume(GtkRange *range, gpointer data);
static void on_app_mute(GtkToggleButton *button, gpointer data);

static GtkWidget *volume_box(GtkWidget **scale, GtkWidget **mute) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	*mute = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(*mute), "audio-volume-high-symbolic");
	gtk_widget_set_tooltip_text(*mute, "Mute");
	gtk_widget_add_css_class(*mute, "flat");
	gtk_box_append(GTK_BOX(box), *mute);
	*scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(*scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(*scale), GTK_POS_RIGHT);
	gtk_widget_set_size_request(*scale, 260, -1);
	gtk_box_append(GTK_BOX(box), *scale);
	return box;
}

static void show_streams(struct sound_page *p, GPtrArray *streams) {
	bool same = p->app_rows->len == streams->len;
	for (guint i = 0; same && i < streams->len; i++) {
		same = ((struct app_row *)p->app_rows->pdata[i])->index ==
			((struct stream *)streams->pdata[i])->index;
	}
	if (!same) {
		for (guint i = 0; i < p->app_rows->len; i++) {
			struct app_row *r = p->app_rows->pdata[i];
			gtk_list_box_remove(GTK_LIST_BOX(p->apps_group), r->row);
		}
		g_ptr_array_set_size(p->app_rows, 0);
		for (guint i = 0; i < streams->len; i++) {
			struct stream *st = streams->pdata[i];
			struct app_row *r = g_new0(struct app_row, 1);
			r->index = st->index;
			GtkWidget *box = volume_box(&r->scale, &r->mute);
			r->row = ui_row(p->apps_group, st->name, NULL, box);
			gtk_box_prepend(GTK_BOX(ui_row_box(r->row)), ui_app_icon(st->icon, 24));
			g_object_set_data(G_OBJECT(r->scale), "app-row", r);
			g_object_set_data(G_OBJECT(r->mute), "app-row", r);
			g_signal_connect(r->scale, "value-changed", G_CALLBACK(on_app_volume), p);
			g_signal_connect(r->mute, "toggled", G_CALLBACK(on_app_mute), p);
			g_ptr_array_add(p->app_rows, r);
		}
	}
	for (guint i = 0; i < streams->len; i++) {
		struct stream *st = streams->pdata[i];
		struct app_row *r = p->app_rows->pdata[i];
		if (g_get_monotonic_time() > r->hold_until) {
			gtk_range_set_value(GTK_RANGE(r->scale), st->volume);
		}
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->mute), st->muted);
		gtk_button_set_icon_name(GTK_BUTTON(r->mute), st->muted ?
			"audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
	}
	gtk_widget_set_visible(p->apps_empty, streams->len == 0);
	g_ptr_array_unref(streams);
}

static void query_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct sound_page *p = data;
	char *out = NULL;
	GError *error = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL,
		&error) && g_subprocess_get_successful(G_SUBPROCESS(source));
	p->querying = false;
	g_clear_error(&error);
	char **parts = ok && out ? g_strsplit(out, "@@\n", 5) : NULL;
	ok = parts && g_strv_length(parts) == 5;
	gtk_widget_set_visible(p->missing, !ok);
	gtk_widget_set_visible(p->groups, ok);
	if (ok) {
		p->updating = true;
		g_free(p->default_sink);
		g_free(p->default_source);
		p->default_sink = g_strstrip(g_strdup(parts[3]));
		p->default_source = g_strstrip(g_strdup(parts[4]));
		show_devices(p, p->out_dd, p->out_scale, p->out_mute, &p->sinks,
			parse_devices(parts[0], false), p->default_sink, p->out_hold);
		show_devices(p, p->in_dd, p->in_scale, p->in_mute, &p->sources,
			parse_devices(parts[1], true), p->default_source, p->in_hold);
		show_streams(p, parse_streams(parts[2]));
		p->updating = false;
	}
	g_strfreev(parts);
	g_free(out);
	g_object_unref(source);
}

static void query(struct sound_page *p) {
	if (p->querying) {
		return;
	}
	GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "sh", "-c",
		"pactl -f json list sinks && echo @@ && pactl -f json list sources && echo @@ && "
		"pactl -f json list sink-inputs && echo @@ && pactl get-default-sink && echo @@ && "
		"pactl get-default-source", NULL);
	if (!proc) {
		gtk_widget_set_visible(p->missing, TRUE);
		gtk_widget_set_visible(p->groups, FALSE);
		return;
	}
	p->querying = true;
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, query_done, p);
}

static gboolean poll_fired(gpointer data) {
	query(data);
	return G_SOURCE_CONTINUE;
}

static gboolean requery_fired(gpointer data) {
	struct sound_page *p = data;
	p->requery_timer = 0;
	query(p);
	return G_SOURCE_REMOVE;
}

static void query_soon(struct sound_page *p) {
	if (!p->requery_timer) {
		p->requery_timer = g_timeout_add(300, requery_fired, p);
	}
}

/* ---------- changes ---------- */

static void on_device(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct sound_page *p = data;
	if (p->updating) {
		return;
	}
	bool output = GTK_WIDGET(dd) == p->out_dd;
	GPtrArray *devices = output ? p->sinks : p->sources;
	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	if (!devices || selected >= devices->len) {
		return;
	}
	struct device *d = devices->pdata[selected];
	pactl(output ? "set-default-sink" : "set-default-source", d->name, NULL);
	settings_status(p->s, "%s: %s", output ? "Sound goes to" : "Recording from", d->description);
	query_soon(p);
}

static void on_volume(GtkRange *range, gpointer data) {
	struct sound_page *p = data;
	if (p->updating) {
		return;
	}
	bool output = GTK_WIDGET(range) == p->out_scale;
	*(output ? &p->out_hold : &p->in_hold) = g_get_monotonic_time() + HOLD_US;
	pactl_percent(output ? "set-sink-volume" : "set-source-volume",
		output ? "@DEFAULT_SINK@" : "@DEFAULT_SOURCE@", (int)gtk_range_get_value(range));
}

static void on_mute(GtkToggleButton *button, gpointer data) {
	struct sound_page *p = data;
	if (p->updating) {
		return;
	}
	bool output = GTK_WIDGET(button) == p->out_mute;
	pactl(output ? "set-sink-mute" : "set-source-mute",
		output ? "@DEFAULT_SINK@" : "@DEFAULT_SOURCE@",
		gtk_toggle_button_get_active(button) ? "1" : "0");
	query_soon(p);
}

static void on_app_volume(GtkRange *range, gpointer data) {
	struct sound_page *p = data;
	struct app_row *r = g_object_get_data(G_OBJECT(range), "app-row");
	if (p->updating || !r) {
		return;
	}
	r->hold_until = g_get_monotonic_time() + HOLD_US;
	char index[16];
	snprintf(index, sizeof(index), "%d", r->index);
	pactl_percent("set-sink-input-volume", index, (int)gtk_range_get_value(range));
}

static void on_app_mute(GtkToggleButton *button, gpointer data) {
	struct sound_page *p = data;
	struct app_row *r = g_object_get_data(G_OBJECT(button), "app-row");
	if (p->updating || !r) {
		return;
	}
	char index[16];
	snprintf(index, sizeof(index), "%d", r->index);
	pactl("set-sink-input-mute", index, gtk_toggle_button_get_active(button) ? "1" : "0");
	query_soon(p);
}

static void on_mixer(GtkButton *button, gpointer data) {
	const char *argv[] = { "pavucontrol", NULL };
	g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void on_map(GtkWidget *widget, gpointer data) {
	struct sound_page *p = data;
	query(p);
	if (!p->poll_timer) {
		p->poll_timer = g_timeout_add(POLL_MS, poll_fired, p);
	}
}

static void on_unmap(GtkWidget *widget, gpointer data) {
	struct sound_page *p = data;
	if (p->poll_timer) {
		g_source_remove(p->poll_timer);
		p->poll_timer = 0;
	}
}

/* ---------- page ---------- */

void sound_page_refresh(struct settings *s) {
	if (s->sound_page) {
		query(s->sound_page);
	}
}

GtkWidget *sound_page_new(struct settings *s) {
	struct sound_page *p = g_new0(struct sound_page, 1);
	p->s = s;
	p->app_rows = g_ptr_array_new_with_free_func(g_free);
	s->sound_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Sound",
		"Where sound plays and records, and how loud every app is.", &content);

	p->missing = gtk_label_new("No sound server found: pactl can't reach PulseAudio or "
		"PipeWire (pipewire-pulse).");
	gtk_label_set_xalign(GTK_LABEL(p->missing), 0);
	gtk_label_set_wrap(GTK_LABEL(p->missing), TRUE);
	gtk_widget_add_css_class(p->missing, "dim-label");
	gtk_widget_set_visible(p->missing, FALSE);
	gtk_box_append(GTK_BOX(content), p->missing);
	p->groups = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(content), p->groups);

	GtkWidget *out = ui_group(p->groups, "Output", NULL);
	p->out_dd = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_size_request(p->out_dd, 300, -1);
	g_signal_connect(p->out_dd, "notify::selected", G_CALLBACK(on_device), p);
	ui_row(out, "Play sound through", "Speakers, headphones or a screen", p->out_dd);
	ui_row(out, "Volume", NULL, volume_box(&p->out_scale, &p->out_mute));
	g_signal_connect(p->out_scale, "value-changed", G_CALLBACK(on_volume), p);
	g_signal_connect(p->out_mute, "toggled", G_CALLBACK(on_mute), p);

	GtkWidget *in = ui_group(p->groups, "Input", NULL);
	p->in_dd = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_size_request(p->in_dd, 300, -1);
	g_signal_connect(p->in_dd, "notify::selected", G_CALLBACK(on_device), p);
	ui_row(in, "Record through", "The microphone apps use", p->in_dd);
	ui_row(in, "Microphone volume", NULL, volume_box(&p->in_scale, &p->in_mute));
	g_signal_connect(p->in_scale, "value-changed", G_CALLBACK(on_volume), p);
	g_signal_connect(p->in_mute, "toggled", G_CALLBACK(on_mute), p);

	p->apps_group = ui_group(p->groups, "Apps", "Volume of each app that is playing sound.");
	p->apps_empty = ui_row(p->apps_group, "No app is playing sound right now", NULL, NULL);

	if (g_find_program_in_path("pavucontrol")) {
		GtkWidget *more = ui_group(p->groups, NULL, NULL);
		GtkWidget *button = gtk_button_new_with_label("Open");
		g_signal_connect(button, "clicked", G_CALLBACK(on_mixer), p);
		ui_row(more, "Volume mixer", "All devices, ports and profiles (pavucontrol)", button);
	}

	g_signal_connect(page, "map", G_CALLBACK(on_map), p);
	g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), p);
	return page;
}
