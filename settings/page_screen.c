#include <json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "settings.h"
#include "tw_paths.h"

/*
 * Screen page:
 *  - displays: resolution, refresh rate, scale, orientation, on/off, the
 *    arrangement of several screens and which of them is the main display
 *    (main_output in common.conf: the desktop icons, the main taskbar). Changes apply right away and ask to be
 *    kept (they revert after 15 seconds otherwise); kept settings are written
 *    to displays.conf, which common.conf includes.
 *  - brightness of a laptop screen (brightnessctl)
 *  - night light: nightlight.conf and the on/off state, which
 *    tilewin-nightlight watches
 *  - power: dimming, turning the screen off, locking and sleeping after a time
 *    without input, what the power button and closing the lid do and locking
 *    before sleep (idle_timeout, power_key_action, lid_action, lock_on_sleep
 *    and lock_command in common.conf)
 */

#define KEEP_SECONDS 15

struct mode {
	int width, height, refresh; // refresh in mHz
};

struct display {
	char *name, *make, *model, *serial;
	bool active;
	int x, y, width, height; // logical layout rectangle
	double scale;
	char *transform;
	struct mode current;
	GArray *modes; // struct mode
};

struct screen_page {
	struct settings *s;
	bool updating;
	GPtrArray *displays; // struct display *
	int selected;

	GtkWidget *displays_group, *not_running;
	GtkWidget *arrangement, *display_row, *display_dd;
	GtkWidget *enable_row, *enable_switch;
	GtkWidget *main_row, *main_switch;
	char *main_name; // the main display, as tileWin reports it
	GtkWidget *resolution_dd, *refresh_dd, *scale_dd, *transform_dd;
	GArray *resolutions; // struct mode (refresh unused) shown in resolution_dd
	GArray *refreshes;   // int mHz shown in refresh_dd

	// dragging in the arrangement
	int drag_display;
	double drag_start_x, drag_start_y;
	int drag_orig_x, drag_orig_y;

	GtkWidget *brightness_scale;
	char *backlight; // /sys/class/backlight/<name>
	guint brightness_timer;

	GtkWidget *night_switch, *night_scale, *night_schedule, *night_from, *night_to;
	guint night_timer;

	GtkWidget *idle_dd[4];
	GtkWidget *lid_dd[2];
	GtkWidget *power_key_dd;
	GtkWidget *lock_entry;
	GtkWidget *lock_on_sleep_switch;
	guint lock_timer;

	// the "keep these settings?" question
	GPtrArray *previous; // struct display * before the change
	GtkWidget *keep_window, *keep_label;
	guint keep_timer;
	int keep_left;
};

static void display_free(gpointer data) {
	struct display *d = data;
	if (!d) {
		return;
	}
	g_free(d->name);
	g_free(d->make);
	g_free(d->model);
	g_free(d->serial);
	g_free(d->transform);
	if (d->modes) {
		g_array_unref(d->modes);
	}
	g_free(d);
}

static struct display *display_copy(const struct display *d) {
	struct display *c = g_new0(struct display, 1);
	*c = *d;
	c->name = g_strdup(d->name);
	c->make = g_strdup(d->make);
	c->model = g_strdup(d->model);
	c->serial = g_strdup(d->serial);
	c->transform = g_strdup(d->transform);
	c->modes = d->modes ? g_array_ref(d->modes) : NULL;
	return c;
}

static const char *json_str(json_object *obj, const char *key) {
	json_object *field;
	return json_object_object_get_ex(obj, key, &field) && field ?
		json_object_get_string(field) : NULL;
}

static int json_int(json_object *obj, const char *key) {
	json_object *field;
	return json_object_object_get_ex(obj, key, &field) && field ? json_object_get_int(field) : 0;
}

/* The identifier tileWin matches outputs by: "make model serial". */
static char *display_id(const struct display *d) {
	return g_strdup_printf("%s %s %s", d->make && *d->make ? d->make : "Unknown",
		d->model && *d->model ? d->model : "Unknown",
		d->serial && *d->serial ? d->serial : "Unknown");
}

static char *display_title(const struct display *d, int number) {
	bool internal = d->name && (g_str_has_prefix(d->name, "eDP") ||
		g_str_has_prefix(d->name, "LVDS") || g_str_has_prefix(d->name, "DSI"));
	const char *what = internal ? "Built-in screen" : d->model && *d->model &&
		strcmp(d->model, "Unknown") != 0 ? d->model : d->name;
	return g_strdup_printf("%d: %s (%s)", number, what, d->name);
}

static GPtrArray *load_displays(void) {
	GPtrArray *list = g_ptr_array_new_with_free_func(display_free);
	char *reply = tw_ipc_request(IPC_GET_OUTPUTS, "");
	json_object *obj = reply ? json_tokener_parse(reply) : NULL;
	g_free(reply);
	int n = obj && json_object_is_type(obj, json_type_array) ?
		(int)json_object_array_length(obj) : 0;
	for (int i = 0; i < n; i++) {
		json_object *o = json_object_array_get_idx(obj, i);
		json_object *field;
		struct display *d = g_new0(struct display, 1);
		d->name = g_strdup(json_str(o, "name"));
		d->make = g_strdup(json_str(o, "make"));
		d->model = g_strdup(json_str(o, "model"));
		d->serial = g_strdup(json_str(o, "serial"));
		d->active = json_object_object_get_ex(o, "active", &field) && json_object_get_boolean(field);
		d->transform = g_strdup(json_str(o, "transform") ? json_str(o, "transform") : "normal");
		d->scale = json_object_object_get_ex(o, "scale", &field) ? json_object_get_double(field) : 1;
		if (d->scale <= 0) {
			d->scale = 1;
		}
		if (json_object_object_get_ex(o, "rect", &field)) {
			d->x = json_int(field, "x");
			d->y = json_int(field, "y");
			d->width = json_int(field, "width");
			d->height = json_int(field, "height");
		}
		if (json_object_object_get_ex(o, "current_mode", &field)) {
			d->current = (struct mode){ json_int(field, "width"), json_int(field, "height"),
				json_int(field, "refresh") };
		}
		d->modes = g_array_new(FALSE, FALSE, sizeof(struct mode));
		json_object *modes;
		if (json_object_object_get_ex(o, "modes", &modes) &&
				json_object_is_type(modes, json_type_array)) {
			for (size_t m = 0; m < json_object_array_length(modes); m++) {
				json_object *mo = json_object_array_get_idx(modes, m);
				struct mode mode = { json_int(mo, "width"), json_int(mo, "height"),
					json_int(mo, "refresh") };
				g_array_append_val(d->modes, mode);
			}
		}
		if (d->modes->len == 0 && d->current.width > 0) {
			g_array_append_val(d->modes, d->current);
		}
		g_ptr_array_add(list, d);
	}
	json_object_put(obj);
	return list;
}

/* ---------- applying and saving ---------- */

static char *display_command(const struct display *d) {
	char *id = display_id(d);
	char *cmd;
	if (!d->active) {
		cmd = g_strdup_printf("output \"%s\" disable", id);
	} else {
		char scale[32];
		g_ascii_formatd(scale, sizeof(scale), "%.4g", d->scale);
		cmd = g_strdup_printf("output \"%s\" enable mode %dx%d@%d.%03dHz position %d %d "
			"scale %s transform %s", id, d->current.width, d->current.height,
			d->current.refresh / 1000, d->current.refresh % 1000, d->x, d->y, scale,
			d->transform);
	}
	g_free(id);
	return cmd;
}

static void apply_display(struct screen_page *p, const struct display *d) {
	char *cmd = display_command(d);
	settings_command(p->s, "%s", cmd);
	g_free(cmd);
}

/* Writes displays.conf and makes sure common.conf includes it. */
static void save_displays(struct screen_page *p) {
	GString *text = g_string_new("# Written by tileWin Settings (Screen). Changes made here are "
		"replaced\n# when the display settings are changed there.\n");
	for (guint i = 0; i < p->displays->len; i++) {
		char *cmd = display_command(p->displays->pdata[i]);
		g_string_append_printf(text, "%s\n", cmd);
		g_free(cmd);
	}
	char *dir = tw_config_dir();
	char *path = dir ? g_build_filename(dir, "displays.conf", NULL) : NULL;
	free(dir);
	GError *error = NULL;
	if (!path || !g_file_set_contents(path, text->str, -1, &error)) {
		settings_status(p->s, "Could not save the display settings: %s",
			error ? error->message : "no config directory");
		g_clear_error(&error);
	} else {
		struct confdoc *common = p->s->common;
		if (!confdoc_child(common->root, "include", "displays.conf")) {
			confdoc_append(common, common->root,
				"# display settings of tileWin Settings\ninclude displays.conf");
			settings_common_changed(p->s, false);
		}
		settings_status(p->s, "Saved the display settings");
	}
	g_free(path);
	g_string_free(text, TRUE);
}

static void rebuild_display_rows(struct screen_page *p);

static void keep_close(struct screen_page *p) {
	if (p->keep_timer) {
		g_source_remove(p->keep_timer);
		p->keep_timer = 0;
	}
	if (p->keep_window) {
		GtkWidget *window = p->keep_window;
		p->keep_window = NULL;
		gtk_window_destroy(GTK_WINDOW(window));
	}
	if (p->previous) {
		g_ptr_array_unref(p->previous);
		p->previous = NULL;
	}
}

static void revert_displays(struct screen_page *p) {
	if (!p->previous) {
		return;
	}
	for (guint i = 0; i < p->previous->len; i++) {
		apply_display(p, p->previous->pdata[i]);
	}
	g_ptr_array_unref(p->displays);
	p->displays = g_ptr_array_ref(p->previous);
	keep_close(p);
	settings_status(p->s, "Went back to the previous display settings");
	rebuild_display_rows(p);
}

static gboolean keep_tick(gpointer data) {
	struct screen_page *p = data;
	if (--p->keep_left <= 0) {
		p->keep_timer = 0;
		revert_displays(p);
		return G_SOURCE_REMOVE;
	}
	char *text = g_strdup_printf("Going back to the previous settings in %d seconds.",
		p->keep_left);
	gtk_label_set_text(GTK_LABEL(p->keep_label), text);
	g_free(text);
	return G_SOURCE_CONTINUE;
}

static void on_keep(GtkButton *button, gpointer data) {
	struct screen_page *p = data;
	keep_close(p);
	save_displays(p);
}

static void on_revert(GtkButton *button, gpointer data) {
	revert_displays(data);
}

static gboolean on_keep_close_request(GtkWindow *window, gpointer data) {
	revert_displays(data);
	return TRUE;
}

/* Applies the changed display and asks whether to keep it, like Windows. */
static void change_display(struct screen_page *p, struct display *changed, GPtrArray *before,
		bool ask) {
	apply_display(p, changed);
	if (!ask) {
		if (before) {
			g_ptr_array_unref(before);
		}
		save_displays(p);
		return;
	}
	if (!p->previous) {
		p->previous = before; // keep the oldest settings when changing again
	} else if (before) {
		g_ptr_array_unref(before);
	}
	p->keep_left = KEEP_SECONDS;
	if (!p->keep_window) {
		p->keep_window = gtk_window_new();
		gtk_window_set_transient_for(GTK_WINDOW(p->keep_window), p->s->window);
		gtk_window_set_modal(GTK_WINDOW(p->keep_window), TRUE);
		gtk_window_set_title(GTK_WINDOW(p->keep_window), "Display settings");
		gtk_window_set_default_size(GTK_WINDOW(p->keep_window), 420, -1);
		g_signal_connect(p->keep_window, "close-request", G_CALLBACK(on_keep_close_request), p);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
		gtk_widget_set_margin_start(box, 22);
		gtk_widget_set_margin_end(box, 22);
		gtk_widget_set_margin_top(box, 22);
		gtk_widget_set_margin_bottom(box, 22);
		GtkWidget *title = gtk_label_new("Keep these display settings?");
		gtk_widget_add_css_class(title, "tw-heading");
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_box_append(GTK_BOX(box), title);
		p->keep_label = gtk_label_new("");
		gtk_label_set_xalign(GTK_LABEL(p->keep_label), 0);
		gtk_box_append(GTK_BOX(box), p->keep_label);
		GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_halign(buttons, GTK_ALIGN_END);
		GtkWidget *keep = gtk_button_new_with_label("Keep changes");
		gtk_widget_add_css_class(keep, "suggested-action");
		g_signal_connect(keep, "clicked", G_CALLBACK(on_keep), p);
		GtkWidget *revert = gtk_button_new_with_label("Revert");
		g_signal_connect(revert, "clicked", G_CALLBACK(on_revert), p);
		gtk_box_append(GTK_BOX(buttons), revert);
		gtk_box_append(GTK_BOX(buttons), keep);
		gtk_box_append(GTK_BOX(box), buttons);
		gtk_window_set_child(GTK_WINDOW(p->keep_window), box);
		gtk_window_present(GTK_WINDOW(p->keep_window));
	}
	if (p->keep_timer) {
		g_source_remove(p->keep_timer);
	}
	char *text = g_strdup_printf("Going back to the previous settings in %d seconds.",
		p->keep_left);
	gtk_label_set_text(GTK_LABEL(p->keep_label), text);
	g_free(text);
	p->keep_timer = g_timeout_add_seconds(1, keep_tick, p);
}

static GPtrArray *snapshot(struct screen_page *p) {
	GPtrArray *copy = g_ptr_array_new_with_free_func(display_free);
	for (guint i = 0; i < p->displays->len; i++) {
		g_ptr_array_add(copy, display_copy(p->displays->pdata[i]));
	}
	return copy;
}

static struct display *selected_display(struct screen_page *p) {
	return p->selected >= 0 && (guint)p->selected < p->displays->len ?
		p->displays->pdata[p->selected] : NULL;
}

/* ---------- display controls ---------- */

static const double scales[] = { 1, 1.25, 1.5, 1.75, 2, 2.5, 3 };
static const char *const scale_labels[] = { "100%", "125%", "150%", "175%", "200%", "250%",
	"300%", NULL };
static const char *const transforms[] = { "normal", "90", "180", "270", NULL };
static const char *const transform_labels[] = { "Landscape", "Portrait (turned right)",
	"Landscape (upside down)", "Portrait (turned left)", NULL };

static int mode_cmp(gconstpointer a, gconstpointer b) {
	const struct mode *ma = a, *mb = b;
	long area = (long)mb->width * mb->height - (long)ma->width * ma->height;
	return area != 0 ? (area > 0 ? 1 : -1) : mb->width - ma->width;
}

static void fill_refreshes(struct screen_page *p, struct display *d) {
	GtkStringList *model = gtk_string_list_new(NULL);
	g_array_set_size(p->refreshes, 0);
	guint selected = 0;
	for (guint i = 0; i < d->modes->len; i++) {
		struct mode *m = &g_array_index(d->modes, struct mode, i);
		if (m->width != d->current.width || m->height != d->current.height) {
			continue;
		}
		bool seen = false;
		for (guint k = 0; k < p->refreshes->len; k++) {
			seen |= g_array_index(p->refreshes, int, k) == m->refresh;
		}
		if (seen) {
			continue;
		}
		if (m->refresh == d->current.refresh) {
			selected = p->refreshes->len;
		}
		g_array_append_val(p->refreshes, m->refresh);
		char label[32];
		if (m->refresh > 0) {
			snprintf(label, sizeof(label), "%.2f Hz", m->refresh / 1000.0);
		} else {
			snprintf(label, sizeof(label), "Default");
		}
		gtk_string_list_append(model, label);
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->refresh_dd), G_LIST_MODEL(model));
	g_object_unref(model);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->refresh_dd), selected);
	gtk_widget_set_sensitive(p->refresh_dd, p->refreshes->len > 1 && d->active);
}

static void rebuild_display_rows(struct screen_page *p) {
	p->updating = true;
	bool have = p->displays->len > 0;
	gtk_widget_set_visible(p->not_running, !have);
	gtk_widget_set_visible(p->displays_group, have);
	if (!have) {
		p->updating = false;
		return;
	}
	if (p->selected < 0 || (guint)p->selected >= p->displays->len) {
		p->selected = 0;
	}
	GtkStringList *names = gtk_string_list_new(NULL);
	int active = 0;
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *d = p->displays->pdata[i];
		char *title = display_title(d, i + 1);
		gtk_string_list_append(names, title);
		g_free(title);
		active += d->active;
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->display_dd), G_LIST_MODEL(names));
	g_object_unref(names);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->display_dd), p->selected);
	bool several = p->displays->len > 1;
	gtk_widget_set_visible(p->display_row, several);
	// hide the list row around the arrangement, not only the drawing
	gtk_widget_set_visible(gtk_widget_get_parent(p->arrangement), several);
	gtk_widget_set_visible(p->enable_row, several);

	struct display *d = selected_display(p);
	bool is_main = p->main_name && d->name && strcmp(p->main_name, d->name) == 0;
	gtk_widget_set_visible(p->main_row, several && d->active);
	gtk_switch_set_active(GTK_SWITCH(p->main_switch), is_main);
	// another screen is made the main display instead of turning this one off
	gtk_widget_set_sensitive(p->main_switch, !is_main);
	gtk_switch_set_active(GTK_SWITCH(p->enable_switch), d->active);
	// the last screen that is on cannot be turned off
	gtk_widget_set_sensitive(p->enable_switch, !(d->active && active <= 1));

	GtkStringList *res_model = gtk_string_list_new(NULL);
	g_array_set_size(p->resolutions, 0);
	GArray *sorted = g_array_copy(d->modes);
	g_array_sort(sorted, mode_cmp);
	guint res_selected = 0;
	for (guint i = 0; i < sorted->len; i++) {
		struct mode *m = &g_array_index(sorted, struct mode, i);
		bool seen = false;
		for (guint k = 0; k < p->resolutions->len; k++) {
			struct mode *r = &g_array_index(p->resolutions, struct mode, k);
			seen |= r->width == m->width && r->height == m->height;
		}
		if (seen) {
			continue;
		}
		if (m->width == d->current.width && m->height == d->current.height) {
			res_selected = p->resolutions->len;
		}
		g_array_append_val(p->resolutions, *m);
		char label[48];
		snprintf(label, sizeof(label), "%d \xc3\x97 %d%s", m->width, m->height,
			p->resolutions->len == 1 ? " (recommended)" : "");
		gtk_string_list_append(res_model, label);
	}
	g_array_unref(sorted);
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->resolution_dd), G_LIST_MODEL(res_model));
	g_object_unref(res_model);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->resolution_dd), res_selected);
	gtk_widget_set_sensitive(p->resolution_dd, d->active && p->resolutions->len > 1);
	fill_refreshes(p, d);

	guint scale_sel = 0;
	double best = 1e9;
	for (guint i = 0; i < G_N_ELEMENTS(scales); i++) {
		if (fabs(scales[i] - d->scale) < best) {
			best = fabs(scales[i] - d->scale);
			scale_sel = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->scale_dd), scale_sel);
	gtk_widget_set_sensitive(p->scale_dd, d->active);
	guint transform_sel = 0;
	for (guint i = 0; transforms[i]; i++) {
		if (strcmp(d->transform, transforms[i]) == 0) {
			transform_sel = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->transform_dd), transform_sel);
	gtk_widget_set_sensitive(p->transform_dd, d->active);
	gtk_widget_queue_draw(p->arrangement);
	p->updating = false;
}

static void on_display_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	p->selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	rebuild_display_rows(p);
}

static void load_main_display(struct screen_page *p) {
	g_free(p->main_name);
	p->main_name = tw_ipc_state("main_output");
}

static gboolean on_main(GtkSwitch *widget, gboolean state, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	if (p->updating || !d || !state) {
		return FALSE;
	}
	// known by make, model and serial so it stays the main display on another
	// connector; screens that do not say what they are go by their name
	char *id = display_id(d);
	char *quoted = conf_quote(strcmp(id, "Unknown Unknown Unknown") == 0 ? d->name : id);
	confdoc_set(p->s->common, p->s->common->root, "main_output", NULL, quoted);
	settings_common_changed(p->s, false);
	settings_command(p->s, "main_output %s", quoted);
	free(quoted);
	g_free(id);
	load_main_display(p);
	rebuild_display_rows(p);
	return FALSE;
}

static gboolean on_enable(GtkSwitch *widget, gboolean state, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	if (p->updating || !d || d->active == (bool)state) {
		return FALSE;
	}
	GPtrArray *before = snapshot(p);
	d->active = state;
	if (state && d->current.width == 0 && d->modes->len > 0) {
		d->current = g_array_index(d->modes, struct mode, 0);
	}
	change_display(p, d, before, true);
	rebuild_display_rows(p);
	return FALSE;
}

static void on_resolution(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (p->updating || !d || sel >= p->resolutions->len) {
		return;
	}
	struct mode *m = &g_array_index(p->resolutions, struct mode, sel);
	if (m->width == d->current.width && m->height == d->current.height) {
		return;
	}
	GPtrArray *before = snapshot(p);
	// the highest refresh rate of the new resolution
	struct mode best = *m;
	for (guint i = 0; i < d->modes->len; i++) {
		struct mode *c = &g_array_index(d->modes, struct mode, i);
		if (c->width == m->width && c->height == m->height && c->refresh > best.refresh) {
			best = *c;
		}
	}
	d->current = best;
	change_display(p, d, before, true);
	rebuild_display_rows(p);
}

static void on_refresh(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (p->updating || !d || sel >= p->refreshes->len ||
			g_array_index(p->refreshes, int, sel) == d->current.refresh) {
		return;
	}
	GPtrArray *before = snapshot(p);
	d->current.refresh = g_array_index(p->refreshes, int, sel);
	change_display(p, d, before, true);
}

static void on_scale(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (p->updating || !d || sel >= G_N_ELEMENTS(scales) || fabs(scales[sel] - d->scale) < 0.01) {
		return;
	}
	GPtrArray *before = snapshot(p);
	d->scale = scales[sel];
	change_display(p, d, before, true);
}

static void on_transform(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	struct display *d = selected_display(p);
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (p->updating || !d || sel >= G_N_ELEMENTS(transforms) - 1 ||
			strcmp(transforms[sel], d->transform) == 0) {
		return;
	}
	GPtrArray *before = snapshot(p);
	g_free(d->transform);
	d->transform = g_strdup(transforms[sel]);
	change_display(p, d, before, true);
}

/* ---------- arrangement ---------- */

struct layout_box {
	double scale, off_x, off_y;
};

static void layout_box(struct screen_page *p, int width, int height, struct layout_box *box) {
	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *d = p->displays->pdata[i];
		if (!d->active) {
			continue;
		}
		min_x = MIN(min_x, d->x);
		min_y = MIN(min_y, d->y);
		max_x = MAX(max_x, d->x + d->width);
		max_y = MAX(max_y, d->y + d->height);
	}
	if (min_x == INT_MAX) {
		min_x = min_y = 0;
		max_x = max_y = 1;
	}
	double bw = max_x - min_x, bh = max_y - min_y;
	box->scale = MIN((width - 40) / bw, (height - 40) / bh);
	box->off_x = (width - bw * box->scale) / 2 - min_x * box->scale;
	box->off_y = (height - bh * box->scale) / 2 - min_y * box->scale;
}

static void draw_arrangement(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	struct screen_page *p = data;
	struct layout_box box;
	layout_box(p, width, height, &box);
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *d = p->displays->pdata[i];
		if (!d->active) {
			continue;
		}
		double x = d->x * box.scale + box.off_x, y = d->y * box.scale + box.off_y;
		double w = d->width * box.scale, h = d->height * box.scale;
		bool sel = (int)i == p->selected;
		cairo_rectangle(cr, x + 2, y + 2, w - 4, h - 4);
		if (sel) {
			cairo_set_source_rgb(cr, 0.0, 0.47, 0.83);
		} else {
			cairo_set_source_rgb(cr, 0.45, 0.47, 0.5);
		}
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, sel ? 0.9 : 0.4);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
		char number[16];
		snprintf(number, sizeof(number), "%u", i + 1);
		cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, MIN(h * 0.45, 42));
		cairo_text_extents_t ext;
		cairo_text_extents(cr, number, &ext);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_move_to(cr, x + (w - ext.width) / 2 - ext.x_bearing,
			y + (h - ext.height) / 2 - ext.y_bearing);
		cairo_show_text(cr, number);
	}
}

static int display_at(struct screen_page *p, double px, double py) {
	struct layout_box box;
	layout_box(p, gtk_widget_get_width(p->arrangement), gtk_widget_get_height(p->arrangement),
		&box);
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *d = p->displays->pdata[i];
		double x = d->x * box.scale + box.off_x, y = d->y * box.scale + box.off_y;
		if (d->active && px >= x && py >= y && px < x + d->width * box.scale &&
				py < y + d->height * box.scale) {
			return i;
		}
	}
	return -1;
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
	struct screen_page *p = data;
	p->drag_display = display_at(p, x, y);
	if (p->drag_display < 0) {
		return;
	}
	p->selected = p->drag_display;
	struct display *d = selected_display(p);
	p->drag_orig_x = d->x;
	p->drag_orig_y = d->y;
	rebuild_display_rows(p);
}

static void on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
	struct screen_page *p = data;
	if (p->drag_display < 0) {
		return;
	}
	struct layout_box box;
	layout_box(p, gtk_widget_get_width(p->arrangement), gtk_widget_get_height(p->arrangement),
		&box);
	struct display *d = p->displays->pdata[p->drag_display];
	d->x = p->drag_orig_x + dx / box.scale;
	d->y = p->drag_orig_y + dy / box.scale;
	gtk_widget_queue_draw(p->arrangement);
}

struct point {
	int x, y;
};

/* Puts the dragged display next to the nearest edge of another one. */
static void snap_display(struct screen_page *p, struct display *d) {
	double best = 1e18;
	int bx = d->x, by = d->y;
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *o = p->displays->pdata[i];
		if (o == d || !o->active) {
			continue;
		}
		int ys[] = { o->y, o->y + o->height - d->height, CLAMP(d->y, o->y - d->height + 1,
			o->y + o->height - 1) };
		int xs[] = { o->x, o->x + o->width - d->width, CLAMP(d->x, o->x - d->width + 1,
			o->x + o->width - 1) };
		struct point candidates[12];
		int n = 0;
		for (int k = 0; k < 3; k++) {
			candidates[n++] = (struct point){ o->x + o->width, ys[k] }; // right of
			candidates[n++] = (struct point){ o->x - d->width, ys[k] }; // left of
			candidates[n++] = (struct point){ xs[k], o->y + o->height }; // below
			candidates[n++] = (struct point){ xs[k], o->y - d->height }; // above
		}
		for (int k = 0; k < n; k++) {
			double dist = pow(candidates[k].x - d->x, 2) + pow(candidates[k].y - d->y, 2);
			if (dist < best) {
				best = dist;
				bx = candidates[k].x;
				by = candidates[k].y;
			}
		}
	}
	d->x = bx;
	d->y = by;
	// the layout starts at 0,0
	int min_x = INT_MAX, min_y = INT_MAX;
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *o = p->displays->pdata[i];
		if (o->active) {
			min_x = MIN(min_x, o->x);
			min_y = MIN(min_y, o->y);
		}
	}
	for (guint i = 0; min_x != INT_MAX && i < p->displays->len; i++) {
		struct display *o = p->displays->pdata[i];
		o->x -= min_x;
		o->y -= min_y;
	}
}

static void on_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
	struct screen_page *p = data;
	if (p->drag_display < 0) {
		return;
	}
	struct display *d = p->displays->pdata[p->drag_display];
	p->drag_display = -1;
	if (fabs(dx) < 3 && fabs(dy) < 3) {
		d->x = p->drag_orig_x;
		d->y = p->drag_orig_y;
		gtk_widget_queue_draw(p->arrangement);
		return;
	}
	snap_display(p, d);
	for (guint i = 0; i < p->displays->len; i++) {
		struct display *o = p->displays->pdata[i];
		if (o->active) {
			apply_display(p, o);
		}
	}
	save_displays(p);
	gtk_widget_queue_draw(p->arrangement);
}

/* ---------- brightness ---------- */

static char *find_backlight(void) {
	GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
	const char *name = dir ? g_dir_read_name(dir) : NULL;
	char *path = name ? g_build_filename("/sys/class/backlight", name, NULL) : NULL;
	if (dir) {
		g_dir_close(dir);
	}
	return path;
}

static long read_long_file(const char *dir, const char *file) {
	char *path = g_build_filename(dir, file, NULL);
	char *text = NULL;
	long value = -1;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		value = strtol(text, NULL, 10);
	}
	g_free(text);
	g_free(path);
	return value;
}

static gboolean apply_brightness(gpointer data) {
	struct screen_page *p = data;
	p->brightness_timer = 0;
	char value[16];
	snprintf(value, sizeof(value), "%d%%",
		(int)lround(gtk_range_get_value(GTK_RANGE(p->brightness_scale))));
	const char *argv[] = { "brightnessctl", "-q", "set", value, NULL };
	GError *error = NULL;
	if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
		settings_status(p->s, "Could not change the brightness: %s", error->message);
		g_clear_error(&error);
	}
	return G_SOURCE_REMOVE;
}

static void on_brightness(GtkRange *range, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	if (p->brightness_timer) {
		g_source_remove(p->brightness_timer);
	}
	p->brightness_timer = g_timeout_add(80, apply_brightness, p);
}

/* ---------- power ---------- */

static const char *const stage_names[4] = { "dim", "screen_off", "lock", "sleep" };
static const char *const stage_titles[4] = { "Dim the screen after",
	"Turn off the screen after", "Lock after", "Put the computer to sleep after" };
static const int durations[] = { 0, 60, 120, 180, 300, 600, 900, 1200, 1500, 1800, 2700,
	3600, 7200, 10800, 14400, 18000 };
static const char *const duration_labels[] = { "Never", "1 minute", "2 minutes", "3 minutes",
	"5 minutes", "10 minutes", "15 minutes", "20 minutes", "25 minutes", "30 minutes",
	"45 minutes", "1 hour", "2 hours", "3 hours", "4 hours", "5 hours", NULL };

static const char *const lid_values[] = { "default", "nothing", "sleep", "hibernate",
	"hybrid_sleep", "lock", "screen_off", "shutdown", NULL };
static const char *const lid_labels[] = { "System default", "Do nothing", "Sleep",
	"Hibernate", "Hybrid sleep", "Lock the screen", "Turn off the screen", "Shut down", NULL };
static const char *const lid_names[2] = { "closed", "docked" };

/* Whether logind can do a sleep verb; true when there is nobody to ask. */
static bool logind_can(const char *method) {
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (!bus) {
		return true;
	}
	GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.login1",
		"/org/freedesktop/login1", "org.freedesktop.login1.Manager", method, NULL,
		G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	g_object_unref(bus);
	if (!reply) {
		return true;
	}
	const char *answer = NULL;
	g_variant_get(reply, "(&s)", &answer);
	bool can = strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0;
	g_variant_unref(reply);
	return can;
}

/* ---------- hibernation setup ---------- */

/* The first line of a file, or NULL. */
static char *read_line(const char *path) {
	char *text = NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		return NULL;
	}
	text[strcspn(text, "\n")] = '\0';
	return text;
}

/* The HOOKS of mkinitcpio include what resumes from the disk (resume, or systemd). */
static bool mkinitcpio_resumes(const char *conf) {
	char *text = NULL;
	if (!g_file_get_contents(conf, &text, NULL, NULL)) {
		return true;
	}
	bool resumes = false;
	char **lines = g_strsplit(text, "\n", -1);
	for (char **l = lines; *l; l++) {
		const char *line = *l + strspn(*l, " \t");
		if (g_str_has_prefix(line, "HOOKS=")) {
			// the last HOOKS= counts; a word of it, not a part of one (sd-resume is none)
			char **words = g_strsplit_set(line + 6, "() \t\"'", -1);
			resumes = false;
			for (char **w = words; *w; w++) {
				resumes |= strcmp(*w, "resume") == 0 || strcmp(*w, "systemd") == 0;
			}
			g_strfreev(words);
		}
	}
	g_strfreev(lines);
	g_free(text);
	return resumes;
}

static void step(GString *out, int *n, const char *text, const char *commands) {
	g_string_append_printf(out, "%s%d. %s", *n > 1 ? "\n\n" : "", *n, text);
	if (commands) {
		char *escaped = g_markup_escape_text(commands, -1);
		g_string_append_printf(out, "\n<tt>%s</tt>", escaped);
		g_free(escaped);
	}
	(*n)++;
}

/*
 * Why this computer cannot hibernate, and the steps that set it up, found by
 * looking at the kernel, the swap and the initramfs. NULL when logind says it can.
 * *cause gets a short reason.
 */
static char *hibernate_steps(char **cause) {
	if (logind_can("CanHibernate")) {
		return NULL;
	}
	GString *out = g_string_new(NULL);
	int n = 1;
	// Secure Boot locks the kernel down, and a locked down kernel does not hibernate
	char *lockdown = read_line("/sys/kernel/security/lockdown");
	char *states = read_line("/sys/power/state");
	bool locked = lockdown && !strstr(lockdown, "[none]");
	bool kernel = states && strstr(states, "disk");
	g_free(lockdown);
	g_free(states);
	// the memory, and the swap on a disk: zram is in the memory, of no use for it
	long mem_kib = 0, disk_kib = 0;
	bool zram = false;
	char *text = NULL;
	if (g_file_get_contents("/proc/meminfo", &text, NULL, NULL)) {
		sscanf(text, "MemTotal: %ld", &mem_kib);
		g_free(text);
	}
	if (g_file_get_contents("/proc/swaps", &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l && l[1]; l++) {
			char name[256];
			long size;
			if (l != lines && sscanf(*l, "%255s %*s %ld", name, &size) == 2) {
				if (g_str_has_prefix(name, "/dev/zram")) {
					zram = true;
				} else {
					disk_kib += size;
				}
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	int gib = (int)((mem_kib + 1048575) / 1048576);
	bool swap_ok = disk_kib >= mem_kib;
	char *root_fs = NULL;
	if (g_file_get_contents("/proc/mounts", &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l; l++) {
			char dev[256], dir[256], type[64];
			if (sscanf(*l, "%255s %255s %63s", dev, dir, type) == 3 && strcmp(dir, "/") == 0) {
				g_free(root_fs);
				root_fs = g_strdup(type);
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	bool btrfs = root_fs && strcmp(root_fs, "btrfs") == 0;
	g_free(root_fs);
	bool efi = g_file_test("/sys/firmware/efi", G_FILE_TEST_IS_DIR);
	bool mkinitcpio = g_file_test("/etc/mkinitcpio.conf", G_FILE_TEST_EXISTS);
	bool initramfs_ok = !mkinitcpio || mkinitcpio_resumes("/etc/mkinitcpio.conf");

	if (locked) {
		*cause = g_strdup("Secure Boot locks the kernel down, which does not allow it");
		step(out, &n, "Turn off Secure Boot in the firmware settings (the setup of the "
			"computer, opened with a key like F2 or Del while it starts). A kernel locked "
			"down by Secure Boot does not hibernate.", NULL);
	} else if (!kernel) {
		*cause = g_strdup("The kernel cannot hibernate");
		step(out, &n, "Start a kernel built with hibernation (CONFIG_HIBERNATION), like the "
			"standard kernel of the distribution.", NULL);
	}
	if (!swap_ok) {
		if (!*cause) {
			*cause = disk_kib ? g_strdup_printf("The swap on the disk (%.1f GB) is smaller "
				"than the memory (%d GB)", disk_kib / 1048576.0, gib) :
				g_strdup(zram ? "There is no swap on the disk, only zram, which is in "
				"the memory" : "There is no swap on the disk");
		}
		char *text2 = g_strdup_printf("Make a swap file of %d GB, as big as the memory, "
			"where the memory is saved to%s:", gib, zram ? " (zram stays, it is used first)" : "");
		char *commands = btrfs ?
			g_strdup_printf("sudo btrfs subvolume create /swap\n"
				"sudo btrfs filesystem mkswapfile --size %dg --uuid clear /swap/swapfile\n"
				"sudo swapon /swap/swapfile", gib) :
			g_strdup_printf("sudo mkswap --file /swapfile --size %dG\n"
				"sudo swapon /swapfile", gib);
		step(out, &n, text2, commands);
		g_free(text2);
		g_free(commands);
		step(out, &n, "Turn it on at every start as well:", btrfs ?
			"echo '/swap/swapfile none swap defaults 0 0' | sudo tee -a /etc/fstab" :
			"echo '/swapfile none swap defaults 0 0' | sudo tee -a /etc/fstab");
	}
	if (!initramfs_ok) {
		if (!*cause) {
			*cause = g_strdup("The initramfs does not resume from the disk");
		}
		step(out, &n, "Let the initramfs resume: in /etc/mkinitcpio.conf add resume to "
			"HOOKS, after filesystems, and build it again:",
			"sudo sed -i '/^HOOKS=/s/filesystems/filesystems resume/' /etc/mkinitcpio.conf\n"
			"sudo mkinitcpio -P");
	} else if (!mkinitcpio && !swap_ok) {
		step(out, &n, "Build the initramfs again, so it finds the swap file:",
			g_file_test("/usr/bin/dracut", G_FILE_TEST_EXISTS) ? "sudo dracut -f --regenerate-all" :
			"sudo update-initramfs -u -k all");
	}
	if (!efi && !swap_ok) {
		step(out, &n, "Without UEFI the kernel has to be told where the swap file is: add "
			"resume=UUID=<the UUID> resume_offset=<the offset> to the kernel parameters of the "
			"boot loader, with what these print:", btrfs ?
			"findmnt -no UUID -T /swap/swapfile\n"
			"sudo btrfs inspect-internal map-swapfile -r /swap/swapfile" :
			"findmnt -no UUID -T /swapfile\n"
			"sudo filefrag -v /swapfile | awk 'NR==4 {print $4+0}'");
	}
	if (n == 1) {
		*cause = g_strdup("logind does not allow it");
		step(out, &n, "Everything needed looks right, but logind still says no. Its log "
			"says why:", "journalctl -b -u systemd-logind");
	} else {
		step(out, &n, "Restart the computer. Then Hibernate and Hybrid sleep work, and "
			"this note is gone.", NULL);
	}
	return g_string_free(out, FALSE);
}

/* A row under the power button and the lid: why it cannot hibernate, and how to set it up. */
static void add_hibernate_help(GtkWidget *group) {
	char *cause = NULL;
	char *steps = hibernate_steps(&cause);
	if (!steps) {
		return;
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	GtkWidget *title = gtk_label_new("This computer cannot hibernate yet, so Hibernate and "
		"Hybrid sleep put it to sleep");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_box_append(GTK_BOX(box), title);
	GtkWidget *why = gtk_label_new(cause);
	gtk_label_set_xalign(GTK_LABEL(why), 0);
	gtk_label_set_wrap(GTK_LABEL(why), TRUE);
	gtk_widget_add_css_class(why, "dim-label");
	gtk_widget_add_css_class(why, "tw-caption");
	gtk_box_append(GTK_BOX(box), why);
	GtkWidget *expander = gtk_expander_new("How to set up hibernation");
	GtkWidget *list = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(list), steps);
	gtk_label_set_xalign(GTK_LABEL(list), 0);
	gtk_label_set_wrap(GTK_LABEL(list), TRUE);
	gtk_label_set_selectable(GTK_LABEL(list), TRUE); // the commands, to copy
	gtk_widget_set_margin_top(list, 6);
	gtk_expander_set_child(GTK_EXPANDER(expander), list);
	gtk_box_append(GTK_BOX(box), expander);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_append(GTK_LIST_BOX(group), row);
	g_free(cause);
	g_free(steps);
}

/* The lid and power key choices; what the computer cannot do says that it sleeps instead,
 * which is what tileWin does then (e.g. hibernate without a swap partition or file). */
static GtkWidget *power_action_dropdown(void) {
	static const char *labels[G_N_ELEMENTS(lid_labels)];
	if (!labels[0]) { // logind is asked once
		memcpy(labels, lid_labels, sizeof(labels));
		if (!logind_can("CanHibernate")) {
			labels[3] = "Hibernate (not set up here, sleeps)";
		}
		if (!logind_can("CanHybridSleep")) {
			labels[4] = "Hybrid sleep (not set up here, sleeps)";
		}
	}
	return gtk_drop_down_new_from_strings(labels);
}

static void on_idle(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	int stage = GPOINTER_TO_INT(g_object_get_data(dropdown, "stage"));
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (sel >= G_N_ELEMENTS(durations)) {
		return;
	}
	struct confdoc *d = p->s->common;
	char *args = g_strdup_printf("%s %d", stage_names[stage], durations[sel]);
	confdoc_set(d, d->root, "idle_timeout", stage_names[stage], durations[sel] ? args : NULL);
	settings_common_changed(p->s, false);
	settings_command(p->s, "idle_timeout %s", args);
	g_free(args);
}

static void on_lid(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	int which = GPOINTER_TO_INT(g_object_get_data(dropdown, "which"));
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (sel >= G_N_ELEMENTS(lid_values) - 1) {
		return;
	}
	struct confdoc *d = p->s->common;
	char *args = g_strdup_printf("%s %s", lid_names[which], lid_values[sel]);
	confdoc_set(d, d->root, "lid_action", lid_names[which], sel == 0 ? NULL : args);
	settings_common_changed(p->s, false);
	settings_command(p->s, "lid_action %s", args);
	g_free(args);
}

static void on_power_key(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (sel >= G_N_ELEMENTS(lid_values) - 1) {
		return;
	}
	struct confdoc *d = p->s->common;
	confdoc_set(d, d->root, "power_key_action", NULL, sel == 0 ? NULL : lid_values[sel]);
	settings_common_changed(p->s, false);
	settings_command(p->s, "power_key_action %s", lid_values[sel]);
}

static gboolean on_lock_on_sleep(GtkSwitch *widget, gboolean active, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return FALSE;
	}
	struct confdoc *d = p->s->common;
	confdoc_set(d, d->root, "lock_on_sleep", NULL, active ? NULL : "no");
	settings_common_changed(p->s, false);
	settings_command(p->s, "lock_on_sleep %s", active ? "yes" : "no");
	return FALSE;
}

static gboolean save_lock_command(gpointer data) {
	struct screen_page *p = data;
	p->lock_timer = 0;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(p->lock_entry));
	struct confdoc *d = p->s->common;
	if (!*text || strcmp(text, "tilewin-lock -f") == 0) {
		confdoc_set(d, d->root, "lock_command", NULL, NULL);
		settings_command(p->s, "lock_command tilewin-lock -f");
	} else {
		confdoc_set(d, d->root, "lock_command", NULL, text);
		settings_command(p->s, "lock_command %s", text);
	}
	settings_common_changed(p->s, false);
	return G_SOURCE_REMOVE;
}

static void on_lock_command(GtkEditable *editable, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return;
	}
	if (p->lock_timer) {
		g_source_remove(p->lock_timer);
	}
	p->lock_timer = g_timeout_add(600, save_lock_command, p);
}

/* ---------- night light ---------- */

#define NIGHT_NEUTRAL 6500
#define NIGHT_WARMEST 1900

static char *night_path(bool state) {
	char *dir = state ? tw_state_dir() : tw_config_dir();
	char *path = dir ? g_build_filename(dir, state ? "nightlight" : "nightlight.conf", NULL) :
		NULL;
	free(dir);
	return path;
}

static const char *night_time_labels[49];

static void night_times_init(void) {
	for (int i = 0; i < 48; i++) {
		if (!night_time_labels[i]) {
			night_time_labels[i] = g_strdup_printf("%02d:%02d", i / 2, i % 2 ? 30 : 0);
		}
	}
}

static guint night_time_index(const char *text, guint fallback) {
	int h, m;
	if (text && sscanf(text, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
		return h * 2 + (m >= 30);
	}
	return fallback;
}

static gboolean night_save(gpointer data) {
	struct screen_page *p = data;
	p->night_timer = 0;
	int strength = (int)round(gtk_range_get_value(GTK_RANGE(p->night_scale)));
	char *text = g_strdup_printf("# Night light, see tilewin-nightlight\n"
		"temperature %d\nschedule %s\nfrom %s\nto %s\n",
		NIGHT_NEUTRAL - strength * (NIGHT_NEUTRAL - NIGHT_WARMEST) / 100,
		gtk_switch_get_active(GTK_SWITCH(p->night_schedule)) ? "yes" : "no",
		night_time_labels[gtk_drop_down_get_selected(GTK_DROP_DOWN(p->night_from)) % 48],
		night_time_labels[gtk_drop_down_get_selected(GTK_DROP_DOWN(p->night_to)) % 48]);
	char *path = night_path(false);
	if (!path || !tw_write_string(path, text)) {
		settings_status(p->s, "Could not save the night light settings");
	}
	g_free(path);
	g_free(text);
	return G_SOURCE_REMOVE;
}

static void night_changed(struct screen_page *p, guint delay) {
	gtk_widget_set_sensitive(p->night_from, gtk_switch_get_active(GTK_SWITCH(p->night_schedule)));
	gtk_widget_set_sensitive(p->night_to, gtk_switch_get_active(GTK_SWITCH(p->night_schedule)));
	if (p->updating) {
		return;
	}
	if (p->night_timer) {
		g_source_remove(p->night_timer);
	}
	p->night_timer = g_timeout_add(delay, night_save, p);
}

static gboolean on_night_switch(GtkSwitch *widget, gboolean state, gpointer data) {
	struct screen_page *p = data;
	if (p->updating) {
		return FALSE;
	}
	char *path = night_path(true);
	if (path) {
		tw_write_string(path, state ? "on\n" : "off\n");
	}
	g_free(path);
	if (state && !tw_in_path("tilewin-nightlight")) {
		settings_status(p->s, "tilewin-nightlight is not installed");
	}
	return FALSE;
}

static void on_night_scale(GtkRange *range, gpointer data) {
	night_changed(data, 150);
}

static gboolean on_night_schedule(GtkSwitch *widget, gboolean state, gpointer data) {
	struct screen_page *p = data;
	gtk_switch_set_state(widget, state);
	night_changed(p, 1);
	return TRUE;
}

static void on_night_time(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	night_changed(data, 1);
}

static void night_refresh(struct screen_page *p) {
	int temperature = 3400, from = 42, to = 14;
	bool schedule = false;
	char *path = night_path(false);
	char *text = NULL;
	if (path && g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l; l++) {
			char key[32], value[64];
			if (sscanf(*l, " %31s %63s", key, value) != 2) {
				continue;
			}
			if (strcmp(key, "temperature") == 0) {
				temperature = CLAMP(atoi(value), NIGHT_WARMEST, NIGHT_NEUTRAL);
			} else if (strcmp(key, "schedule") == 0) {
				schedule = strcmp(value, "yes") == 0;
			} else if (strcmp(key, "from") == 0) {
				from = night_time_index(value, from);
			} else if (strcmp(key, "to") == 0) {
				to = night_time_index(value, to);
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);
	path = night_path(true);
	char *state = path ? tw_read_first_line(path) : NULL;
	g_free(path);
	if (p->night_timer) {
		return; // a change is about to be saved
	}
	gtk_switch_set_active(GTK_SWITCH(p->night_switch), state && strcmp(state, "on") == 0);
	free(state);
	gtk_range_set_value(GTK_RANGE(p->night_scale),
		(NIGHT_NEUTRAL - temperature) * 100.0 / (NIGHT_NEUTRAL - NIGHT_WARMEST));
	gtk_switch_set_active(GTK_SWITCH(p->night_schedule), schedule);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->night_from), from);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->night_to), to);
	night_changed(p, 0);
}

static bool has_lid(void) {
	GDir *dir = g_dir_open("/proc/acpi/button/lid", 0, NULL);
	bool found = dir && g_dir_read_name(dir) != NULL;
	if (dir) {
		g_dir_close(dir);
	}
	return found;
}

/* ---------- page ---------- */

void screen_page_refresh(struct settings *s) {
	struct screen_page *p = s->screen_page;
	if (!p || p->keep_window) {
		return; // don't replace settings that wait for "keep"
	}
	g_ptr_array_unref(p->displays);
	p->displays = load_displays();
	load_main_display(p);
	rebuild_display_rows(p);

	p->updating = true;
	if (p->brightness_scale && p->backlight) {
		long max = read_long_file(p->backlight, "max_brightness");
		long cur = read_long_file(p->backlight, "brightness");
		if (max > 0 && cur >= 0) {
			gtk_range_set_value(GTK_RANGE(p->brightness_scale), cur * 100.0 / max);
		}
	}
	night_refresh(p);
	struct confdoc *d = s->common;
	for (int i = 0; i < 4; i++) {
		struct cstmt *stmt = confdoc_child(d->root, "idle_timeout", stage_names[i]);
		const char *value = cstmt_arg(stmt, 1);
		int seconds = value ? atoi(value) : 0;
		guint sel = 0;
		int best = INT_MAX;
		for (guint k = 0; k < G_N_ELEMENTS(durations); k++) {
			int diff = abs(durations[k] - seconds);
			if (diff < best) {
				best = diff;
				sel = k;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->idle_dd[i]), sel);
	}
	for (int i = 0; i < 2 && p->lid_dd[0]; i++) {
		struct cstmt *stmt = confdoc_child(d->root, "lid_action", lid_names[i]);
		const char *value = cstmt_arg(stmt, 1);
		guint sel = 0;
		for (guint k = 0; value && lid_values[k]; k++) {
			if (strcmp(value, lid_values[k]) == 0) {
				sel = k;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->lid_dd[i]), sel);
	}
	struct cstmt *key = confdoc_child(d->root, "power_key_action", NULL);
	const char *key_value = cstmt_arg(key, 0);
	guint key_sel = 0;
	for (guint k = 0; key_value && lid_values[k]; k++) {
		if (strcmp(key_value, lid_values[k]) == 0) {
			key_sel = k;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->power_key_dd), key_sel);
	struct cstmt *on_sleep = confdoc_child(d->root, "lock_on_sleep", NULL);
	const char *on_sleep_value = cstmt_arg(on_sleep, 0);
	gtk_switch_set_active(GTK_SWITCH(p->lock_on_sleep_switch), !on_sleep_value ||
		!(strcmp(on_sleep_value, "no") == 0 || strcmp(on_sleep_value, "false") == 0 ||
		strcmp(on_sleep_value, "off") == 0 || strcmp(on_sleep_value, "disable") == 0));
	struct cstmt *lock = confdoc_child(d->root, "lock_command", NULL);
	char *lock_text = lock ? cstmt_join(lock, 0) : g_strdup("tilewin-lock -f");
	if (strcmp(gtk_editable_get_text(GTK_EDITABLE(p->lock_entry)), lock_text) != 0) {
		gtk_editable_set_text(GTK_EDITABLE(p->lock_entry), lock_text);
	}
	g_free(lock_text);
	p->updating = false;
}

GtkWidget *screen_page_new(struct settings *s) {
	struct screen_page *p = g_new0(struct screen_page, 1);
	p->s = s;
	p->displays = g_ptr_array_new_with_free_func(display_free);
	p->resolutions = g_array_new(FALSE, FALSE, sizeof(struct mode));
	p->refreshes = g_array_new(FALSE, FALSE, sizeof(int));
	p->drag_display = -1;
	s->screen_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Screen",
		"Resolution, scale and arrangement of your screens, brightness, and when the screen "
		"turns off or the computer sleeps.", &content);

	p->not_running = gtk_label_new("Display settings need tileWin to be running.");
	gtk_label_set_xalign(GTK_LABEL(p->not_running), 0);
	gtk_widget_add_css_class(p->not_running, "dim-label");
	gtk_box_append(GTK_BOX(content), p->not_running);

	p->displays_group = ui_group(content, "Displays",
		"Changes apply right away and go back after 15 seconds unless you keep them.");
	p->arrangement = gtk_drawing_area_new();
	gtk_widget_set_size_request(p->arrangement, -1, 200);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->arrangement), draw_arrangement, p, NULL);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), p);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), p);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), p);
	gtk_widget_add_controller(p->arrangement, GTK_EVENT_CONTROLLER(drag));
	GtkWidget *arrangement_row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(arrangement_row), FALSE);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(arrangement_row), p->arrangement);
	gtk_list_box_append(GTK_LIST_BOX(p->displays_group), arrangement_row);
	gtk_widget_set_tooltip_text(p->arrangement, "Drag the screens to arrange them");

	p->display_dd = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->display_dd, "notify::selected", G_CALLBACK(on_display_selected), p);
	p->display_row = ui_row(p->displays_group, "Display", "The screen the settings below change",
		p->display_dd);
	p->enable_switch = gtk_switch_new();
	g_signal_connect(p->enable_switch, "state-set", G_CALLBACK(on_enable), p);
	p->enable_row = ui_row(p->displays_group, "Use this screen", NULL, p->enable_switch);
	p->main_switch = gtk_switch_new();
	g_signal_connect(p->main_switch, "state-set", G_CALLBACK(on_main), p);
	p->main_row = ui_row(p->displays_group, "Make this my main display",
		"It gets the desktop icons, and the taskbar that can show the windows of every screen",
		p->main_switch);
	p->resolution_dd = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->resolution_dd, "notify::selected", G_CALLBACK(on_resolution), p);
	ui_row(p->displays_group, "Resolution", NULL, p->resolution_dd);
	p->refresh_dd = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(p->refresh_dd, "notify::selected", G_CALLBACK(on_refresh), p);
	ui_row(p->displays_group, "Refresh rate", NULL, p->refresh_dd);
	p->scale_dd = gtk_drop_down_new_from_strings(scale_labels);
	g_signal_connect(p->scale_dd, "notify::selected", G_CALLBACK(on_scale), p);
	ui_row(p->displays_group, "Scale", "Size of text, apps and the taskbar", p->scale_dd);
	p->transform_dd = gtk_drop_down_new_from_strings(transform_labels);
	g_signal_connect(p->transform_dd, "notify::selected", G_CALLBACK(on_transform), p);
	ui_row(p->displays_group, "Orientation", NULL, p->transform_dd);

	p->backlight = find_backlight();
	if (p->backlight) {
		GtkWidget *group = ui_group(content, "Brightness", NULL);
		p->brightness_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
		gtk_widget_set_size_request(p->brightness_scale, 280, -1);
		g_signal_connect(p->brightness_scale, "value-changed", G_CALLBACK(on_brightness), p);
		ui_row(group, "Brightness of the built-in screen", NULL, p->brightness_scale);
	}

	night_times_init();
	GtkWidget *night = ui_group(content, "Night light",
		"Warmer colors in the evening are easier on the eyes and help you fall asleep.");
	p->night_switch = gtk_switch_new();
	g_signal_connect(p->night_switch, "state-set", G_CALLBACK(on_night_switch), p);
	ui_row(night, "Night light", "On right now", p->night_switch);
	p->night_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_widget_set_size_request(p->night_scale, 280, -1);
	g_signal_connect(p->night_scale, "value-changed", G_CALLBACK(on_night_scale), p);
	ui_row(night, "Strength", "How warm the colors get", p->night_scale);
	p->night_schedule = gtk_switch_new();
	g_signal_connect(p->night_schedule, "state-set", G_CALLBACK(on_night_schedule), p);
	ui_row(night, "Schedule", "Turn on and off by itself", p->night_schedule);
	p->night_from = gtk_drop_down_new_from_strings(night_time_labels);
	g_signal_connect(p->night_from, "notify::selected", G_CALLBACK(on_night_time), p);
	ui_row(night, "Turn on at", NULL, p->night_from);
	p->night_to = gtk_drop_down_new_from_strings(night_time_labels);
	g_signal_connect(p->night_to, "notify::selected", G_CALLBACK(on_night_time), p);
	ui_row(night, "Turn off at", NULL, p->night_to);

	GtkWidget *power = ui_group(content, "Power & sleep",
		"Times without using the mouse or keyboard. Videos and other apps that keep the "
		"screen on pause them.");
	for (int i = 0; i < 4; i++) {
		p->idle_dd[i] = gtk_drop_down_new_from_strings(duration_labels);
		g_object_set_data(G_OBJECT(p->idle_dd[i]), "stage", GINT_TO_POINTER(i));
		g_signal_connect(p->idle_dd[i], "notify::selected", G_CALLBACK(on_idle), p);
		ui_row(power, stage_titles[i], NULL, p->idle_dd[i]);
	}

	GtkWidget *lid = ui_group(content, has_lid() ? "Power button and lid" : "Power button",
		"Sleep keeps the work in memory and wakes up at once. Hibernate saves it to the disk "
		"and turns the computer off. Hybrid sleep does both, so the work survives when the "
		"battery runs out.");
	p->power_key_dd = power_action_dropdown();
	g_signal_connect(p->power_key_dd, "notify::selected", G_CALLBACK(on_power_key), p);
	ui_row(lid, "When I press the power button", "System default usually is shut down",
		p->power_key_dd);
	if (has_lid()) {
		const char *titles[2] = { "When I close the lid",
			"When I close the lid with a screen connected" };
		for (int i = 0; i < 2; i++) {
			p->lid_dd[i] = power_action_dropdown();
			g_object_set_data(G_OBJECT(p->lid_dd[i]), "which", GINT_TO_POINTER(i));
			g_signal_connect(p->lid_dd[i], "notify::selected", G_CALLBACK(on_lid), p);
			ui_row(lid, titles[i], i == 0 ? "System default usually is sleep" :
				"\"Turn off the screen\" turns off only the built-in screen", p->lid_dd[i]);
		}
	}
	add_hibernate_help(lid);

	GtkWidget *lock = ui_group(content, "Lock screen", NULL);
	p->lock_entry = gtk_entry_new();
	gtk_widget_set_size_request(p->lock_entry, 280, -1);
	g_signal_connect(p->lock_entry, "changed", G_CALLBACK(on_lock_command), p);
	ui_row(lock, "Lock with", "The program that locks the screen (Win+L uses $locker from "
		"common.conf)", p->lock_entry);
	p->lock_on_sleep_switch = gtk_switch_new();
	g_signal_connect(p->lock_on_sleep_switch, "state-set", G_CALLBACK(on_lock_on_sleep), p);
	ui_row(lock, "Lock before sleeping", "Locks the screen before the computer sleeps or "
		"hibernates, so it wakes up locked", p->lock_on_sleep_switch);

	screen_page_refresh(s);
	return page;
}
