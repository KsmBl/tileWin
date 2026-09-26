#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "savers.h"
#include "settings.h"

/*
 * The Screen saver page, like the Screen Saver Settings of Windows: which
 * one, after how long, whether coming back asks for the password, the
 * options of the one picked, and a little monitor that shows it running.
 *
 * The saver and its options are the "screensaver" block of taskbar.conf,
 * which tilewin-screensaver reads; when it starts and whether it locks are
 * compositor commands in common.conf (idle_timeout screensaver, and
 * screensaver_lock).
 */

static const int waits[] = { 60, 120, 180, 300, 600, 900, 1200, 1800, 2700, 3600 };
static const char *const wait_labels[] = { "1 minute", "2 minutes", "3 minutes", "5 minutes",
	"10 minutes", "15 minutes", "20 minutes", "30 minutes", "45 minutes", "1 hour", NULL };
static const double speeds[] = { 0.5, 1, 1.75, 2.5 };
static const char *const speed_labels[] = { "Slow", "Normal", "Fast", "Very fast", NULL };

#define PREVIEW_W 320
#define PREVIEW_H 200
#define BEZEL 14

struct screensaver_page {
	struct settings *s;
	bool updating;
	GtkWidget *saver_dd, *saver_row, *wait_dd, *wait_row, *lock_switch, *lock_row;
	GtkWidget *speed_dd, *speed_row, *text_entry, *text_row, *photos_entry, *photos_row;
	GtkWidget *seconds_spin, *seconds_row, *preview_button, *preview, *options;
	GPtrArray *own;       // struct own_row *: the rows of the savers' own settings
	struct saver_run *run;
	const struct saver *shown; // what the preview runs
	gint64 last_frame;
	char *preview_options; // what it was made with, to start over when they change
};

/* A row of one saver's own settings, and the control in it. */
struct own_row {
	const struct saver *saver;
	const struct saver_option *option;
	GtkWidget *row, *control;
	char *key;            // "<saver>_<key>"
};

/* ---------- reading and writing ---------- */

static struct cstmt *saver_block(struct screensaver_page *p, bool create) {
	return confdoc_block(p->s->taskbar, "screensaver", NULL, create);
}

static const char *saver_value(struct screensaver_page *p, const char *key) {
	return cstmt_arg(confdoc_child(saver_block(p, false), key, NULL), 0);
}

static void saver_write(struct screensaver_page *p, const char *key, const char *value) {
	struct cstmt *block = saver_block(p, value != NULL);
	if (!block) {
		return;
	}
	char *quoted = value ? conf_quote(value) : NULL;
	confdoc_set(p->s->taskbar, block, key, NULL, quoted);
	g_free(quoted);
	settings_taskbar_changed(p->s);
}

/* The saver picked in the dropdown: NULL for none, "random" or a name. */
static const char *picked(struct screensaver_page *p) {
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->saver_dd));
	if (i == 0) {
		return NULL;
	}
	if (i == 1) {
		return "random";
	}
	return i - 2 < (guint)saver_count ? savers[i - 2]->name : NULL;
}

static void write_timeout(struct screensaver_page *p) {
	struct confdoc *d = p->s->common;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->wait_dd));
	int seconds = picked(p) && sel < G_N_ELEMENTS(waits) ? waits[sel] : 0;
	char *args = g_strdup_printf("screensaver %d", seconds);
	confdoc_set(d, d->root, "idle_timeout", "screensaver", seconds ? args : NULL);
	settings_common_changed(p->s, false);
	settings_command(p->s, "idle_timeout %s", args);
	g_free(args);
}

/* ---------- the preview ---------- */

/* The savers' own settings in the block, name and value after each other; and as one string. */
static GPtrArray *own_settings(struct screensaver_page *p, GString *state) {
	GPtrArray *pairs = g_ptr_array_new();
	struct cstmt *block = saver_block(p, false);
	for (guint i = 0; block && block->children && i < block->children->len; i++) {
		struct cstmt *c = block->children->pdata[i];
		const char *value = cstmt_arg(c, 0);
		if (value && strchr(c->name, '_')) {
			g_ptr_array_add(pairs, c->name);
			g_ptr_array_add(pairs, (gpointer)value);
			if (state) {
				g_string_append_printf(state, "|%s=%s", c->name, value);
			}
		}
	}
	return pairs;
}

/* The options for the preview; the settings in *own, to be freed after it started. */
static void preview_options(struct screensaver_page *p, struct saver_options *o,
		GPtrArray **own) {
	const char *speed = saver_value(p, "speed");
	const char *seconds = saver_value(p, "photo_seconds");
	*own = own_settings(p, NULL);
	*o = (struct saver_options){
		.speed = speed ? g_ascii_strtod(speed, NULL) : 1,
		.text = saver_value(p, "text"),
		.photos = saver_value(p, "photos"),
		.photo_seconds = seconds ? atoi(seconds) : 8,
		.settings = (const char *const *)(*own)->pdata,
		.setting_count = (int)(*own)->len,
	};
}

/* Starts the preview over, e.g. with another saver or new options. */
static void preview_restart(struct screensaver_page *p) {
	saver_run_free(p->run);
	p->run = NULL;
	const char *name = picked(p);
	if (!name) {
		p->shown = NULL;
	} else if (!p->shown || strcmp(name, "random") != 0) {
		p->shown = saver_find(name); // a random one stays until something changes
	}
	if (p->shown) {
		struct saver_options o;
		GPtrArray *own;
		preview_options(p, &o, &own);
		char *expanded = o.photos && o.photos[0] == '~' ?
			g_build_filename(g_get_home_dir(), o.photos + 1, NULL) : NULL;
		if (expanded) {
			o.photos = expanded;
		}
		p->run = saver_run_new(p->shown, PREVIEW_W, PREVIEW_H, &o);
		g_free(expanded);
		g_ptr_array_free(own, TRUE); // the savers read their settings as they start
	}
	p->last_frame = 0;
	gtk_widget_queue_draw(p->preview);
}

/* A desktop for the savers that are drawn over it. */
static void draw_desktop(cairo_t *cr) {
	cairo_pattern_t *wall = cairo_pattern_create_linear(0, 0, PREVIEW_W, PREVIEW_H);
	cairo_pattern_add_color_stop_rgb(wall, 0, 0.05, 0.25, 0.55);
	cairo_pattern_add_color_stop_rgb(wall, 1, 0.1, 0.55, 0.85);
	cairo_rectangle(cr, 0, 0, PREVIEW_W, PREVIEW_H);
	cairo_set_source(cr, wall);
	cairo_fill(cr);
	cairo_pattern_destroy(wall);
	for (int i = 0; i < 3; i++) {
		cairo_rectangle(cr, 8, 8 + i * 28, 16, 18);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
		cairo_fill(cr);
	}
	cairo_rectangle(cr, 0, PREVIEW_H - 14, PREVIEW_W, 14);
	cairo_set_source_rgba(cr, 0.08, 0.08, 0.1, 0.9);
	cairo_fill(cr);
}

static void draw_preview(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer data) {
	struct screensaver_page *p = data;
	double x = (width - PREVIEW_W - 2 * BEZEL) / 2.0, y = 4;
	// the monitor: a dark frame and a stand, as the Windows dialog had it
	double fw = PREVIEW_W + 2 * BEZEL, fh = PREVIEW_H + 2 * BEZEL;
	cairo_rectangle(cr, x + fw / 2 - 30, y + fh, 60, 16);
	cairo_set_source_rgb(cr, 0.3, 0.31, 0.33);
	cairo_fill(cr);
	cairo_rectangle(cr, x + fw / 2 - 70, y + fh + 16, 140, 6);
	cairo_fill(cr);
	cairo_new_sub_path(cr);
	double r = 8;
	cairo_arc(cr, x + fw - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + fw - r, y + fh - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + fh - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
	cairo_pattern_t *bezel = cairo_pattern_create_linear(0, y, 0, y + fh);
	cairo_pattern_add_color_stop_rgb(bezel, 0, 0.25, 0.26, 0.28);
	cairo_pattern_add_color_stop_rgb(bezel, 1, 0.12, 0.12, 0.13);
	cairo_set_source(cr, bezel);
	cairo_fill(cr);
	cairo_pattern_destroy(bezel);

	cairo_save(cr);
	cairo_translate(cr, x + BEZEL, y + BEZEL);
	cairo_rectangle(cr, 0, 0, PREVIEW_W, PREVIEW_H);
	cairo_clip(cr);
	if (!p->run) {
		draw_desktop(cr); // no saver: the desktop stays
	} else {
		gint64 now = g_get_monotonic_time();
		double dt = p->last_frame ? (now - p->last_frame) / 1e6 : 0;
		p->last_frame = now;
		cairo_surface_t *screen = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			PREVIEW_W, PREVIEW_H);
		cairo_t *scr = cairo_create(screen);
		saver_run_draw(p->run, scr, dt);
		cairo_destroy(scr);
		if (p->shown->transparent) {
			draw_desktop(cr);
		}
		cairo_set_source_surface(cr, screen, 0, 0);
		cairo_paint(cr);
		cairo_surface_destroy(screen);
	}
	cairo_restore(cr);
}

static gboolean preview_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	struct screensaver_page *p = data;
	if (p->run) {
		// about 30 times a second is plenty for a picture this small
		gint64 now = gdk_frame_clock_get_frame_time(clock);
		if (!p->last_frame || now - p->last_frame >= 30000) {
			gtk_widget_queue_draw(widget);
		}
	}
	return G_SOURCE_CONTINUE;
}

/* ---------- showing what fits ---------- */

static void update_rows(struct screensaver_page *p) {
	const char *name = picked(p);
	bool any = name != NULL;
	gtk_widget_set_visible(p->wait_row, any);
	gtk_widget_set_visible(p->lock_row, any);
	gtk_widget_set_visible(p->speed_row, any && strcmp(name, "blank") != 0);
	bool random = any && strcmp(name, "random") == 0;
	gtk_widget_set_visible(p->text_row, any && (random || strcmp(name, "text3d") == 0));
	gtk_widget_set_visible(p->photos_row, any && (random || strcmp(name, "photos") == 0));
	gtk_widget_set_visible(p->seconds_row, any && (random || strcmp(name, "photos") == 0));
	gtk_widget_set_sensitive(p->preview_button, any);
	gtk_widget_set_visible(p->options, any && strcmp(name, "blank") != 0);
	for (guint i = 0; i < p->own->len; i++) {
		struct own_row *r = p->own->pdata[i];
		gtk_widget_set_visible(r->row, any && strcmp(name, r->saver->name) == 0);
	}
}

static void refresh(struct screensaver_page *p) {
	p->updating = true;
	const char *name = saver_value(p, "name");
	struct cstmt *timeout = confdoc_child(p->s->common->root, "idle_timeout", "screensaver");
	const char *value = cstmt_arg(timeout, 1);
	int seconds = value ? atoi(value) : 0;
	guint sel = 0;
	if (seconds > 0) {
		// tilewin-screensaver shows Bubbles when the block names none
		const char *shown = name ? name : "bubbles";
		sel = 1;
		for (int i = 0; i < saver_count; i++) {
			if (g_ascii_strcasecmp(savers[i]->name, shown) == 0) {
				sel = i + 2;
			}
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->saver_dd), sel);
	guint wait = 4; // 10 minutes
	int best = G_MAXINT;
	for (guint i = 0; seconds > 0 && i < G_N_ELEMENTS(waits); i++) {
		if (abs(waits[i] - seconds) < best) {
			best = abs(waits[i] - seconds);
			wait = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->wait_dd), wait);
	const char *lock = cstmt_arg(confdoc_child(p->s->common->root, "screensaver_lock", NULL), 0);
	gtk_switch_set_active(GTK_SWITCH(p->lock_switch), lock &&
		(g_ascii_strcasecmp(lock, "yes") == 0 || g_ascii_strcasecmp(lock, "on") == 0 ||
		g_ascii_strcasecmp(lock, "true") == 0));
	const char *speed = saver_value(p, "speed");
	double sp = speed ? g_ascii_strtod(speed, NULL) : 1;
	guint speed_sel = 1;
	for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
		if (fabs(speeds[i] - sp) < 0.01) {
			speed_sel = i;
		}
	}
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->speed_dd), speed_sel);
	const char *text = saver_value(p, "text"), *photos = saver_value(p, "photos");
	if (strcmp(gtk_editable_get_text(GTK_EDITABLE(p->text_entry)), text ? text : "") != 0) {
		gtk_editable_set_text(GTK_EDITABLE(p->text_entry), text ? text : "");
	}
	if (strcmp(gtk_editable_get_text(GTK_EDITABLE(p->photos_entry)), photos ? photos : "") != 0) {
		gtk_editable_set_text(GTK_EDITABLE(p->photos_entry), photos ? photos : "");
	}
	const char *photo_seconds = saver_value(p, "photo_seconds");
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->seconds_spin),
		photo_seconds ? atoi(photo_seconds) : 8);
	for (guint i = 0; i < p->own->len; i++) {
		struct own_row *r = p->own->pdata[i];
		const char *v = saver_value(p, r->key);
		if (r->option->type == SAVER_TOGGLE) {
			bool on = v ? g_ascii_strcasecmp(v, "yes") == 0 || g_ascii_strcasecmp(v, "on") == 0 ||
				g_ascii_strcasecmp(v, "true") == 0 : r->option->on;
			gtk_switch_set_active(GTK_SWITCH(r->control), on);
		} else {
			guint k = 0;
			for (guint j = 0; v && r->option->values[j]; j++) {
				if (g_ascii_strcasecmp(r->option->values[j], v) == 0) {
					k = j;
				}
			}
			gtk_drop_down_set_selected(GTK_DROP_DOWN(r->control), k);
		}
	}
	p->updating = false;
	update_rows(p);
	// the preview starts over only when what it shows changed
	GString *own_state = g_string_new(NULL);
	g_ptr_array_free(own_settings(p, own_state), TRUE);
	char *state = g_strdup_printf("%u|%s|%s|%s|%s%s", sel, speed ? speed : "", text ? text : "",
		photos ? photos : "", photo_seconds ? photo_seconds : "", own_state->str);
	g_string_free(own_state, TRUE);
	if (!p->preview_options || strcmp(state, p->preview_options) != 0) {
		g_free(p->preview_options);
		p->preview_options = state;
		preview_restart(p);
	} else {
		g_free(state);
	}
}

void screensaver_page_refresh(struct settings *s) {
	if (s->screensaver_page) {
		refresh(s->screensaver_page);
	}
}

/* ---------- controls ---------- */

static void on_saver(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screensaver_page *p = data;
	if (p->updating) {
		return;
	}
	const char *name = picked(p);
	if (name) {
		saver_write(p, "name", name);
	}
	write_timeout(p);
	refresh(p);
}

static void on_wait(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screensaver_page *p = data;
	if (!p->updating) {
		write_timeout(p);
	}
}

static void on_lock(GObject *object, GParamSpec *pspec, gpointer data) {
	struct screensaver_page *p = data;
	if (p->updating) {
		return;
	}
	bool on = gtk_switch_get_active(GTK_SWITCH(p->lock_switch));
	struct confdoc *d = p->s->common;
	confdoc_set(d, d->root, "screensaver_lock", NULL, on ? "yes" : NULL);
	settings_common_changed(p->s, false);
	settings_command(p->s, "screensaver_lock %s", on ? "yes" : "no");
}

static void on_speed(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct screensaver_page *p = data;
	if (p->updating) {
		return;
	}
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->speed_dd));
	char value[16];
	g_ascii_formatd(value, sizeof(value), "%g", sel < G_N_ELEMENTS(speeds) ? speeds[sel] : 1);
	saver_write(p, "speed", sel == 1 ? NULL : value);
	refresh(p);
}

static void on_text(GtkEditable *editable, gpointer data) {
	struct screensaver_page *p = data;
	if (p->updating) {
		return;
	}
	const char *key = g_object_get_data(G_OBJECT(editable), "key");
	const char *text = gtk_editable_get_text(editable);
	saver_write(p, key, *text ? text : NULL);
	refresh(p);
}

static void on_seconds(GtkSpinButton *spin, gpointer data) {
	struct screensaver_page *p = data;
	if (p->updating) {
		return;
	}
	int seconds = gtk_spin_button_get_value_as_int(spin);
	char value[16];
	snprintf(value, sizeof(value), "%d", seconds);
	saver_write(p, "photo_seconds", seconds == 8 ? NULL : value);
	refresh(p);
}

/* One of a saver's own settings changed: written, or taken out when it is the default. */
static void own_changed(struct screensaver_page *p, struct own_row *r) {
	if (p->updating) {
		return;
	}
	const char *value = NULL;
	if (r->option->type == SAVER_TOGGLE) {
		bool on = gtk_switch_get_active(GTK_SWITCH(r->control));
		value = on == r->option->on ? NULL : on ? "yes" : "no";
	} else {
		guint k = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->control));
		value = k == 0 || k == GTK_INVALID_LIST_POSITION ? NULL : r->option->values[k];
	}
	saver_write(p, r->key, value);
	refresh(p);
}

static void on_own_choice(GObject *object, GParamSpec *pspec, gpointer data) {
	own_changed(g_object_get_data(object, "page"), data);
}

static void on_own_toggle(GObject *object, GParamSpec *pspec, gpointer data) {
	own_changed(g_object_get_data(object, "page"), data);
}

static void own_row_free(gpointer data) {
	struct own_row *r = data;
	g_free(r->key);
	g_free(r);
}

/* The rows of every saver's own settings, shown only while that saver is picked. */
static void add_own_rows(struct screensaver_page *p, GtkWidget *group) {
	p->own = g_ptr_array_new_with_free_func(own_row_free);
	for (int i = 0; i < saver_count; i++) {
		const struct saver *sv = savers[i];
		for (const struct saver_option *o = sv->options; o && o->key; o++) {
			struct own_row *r = g_new0(struct own_row, 1);
			r->saver = sv;
			r->option = o;
			r->key = g_strdup_printf("%s_%s", sv->name, o->key);
			if (o->type == SAVER_TOGGLE) {
				r->control = gtk_switch_new();
				g_signal_connect(r->control, "notify::active", G_CALLBACK(on_own_toggle), r);
			} else {
				r->control = gtk_drop_down_new_from_strings(o->labels);
				g_signal_connect(r->control, "notify::selected", G_CALLBACK(on_own_choice), r);
			}
			g_object_set_data(G_OBJECT(r->control), "page", p);
			r->row = ui_row(group, o->label, o->help, r->control);
			g_ptr_array_add(p->own, r);
		}
	}
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct screensaver_page *p = data;
	GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (folder) {
		char *path = g_file_get_path(folder);
		if (path) {
			gtk_editable_set_text(GTK_EDITABLE(p->photos_entry), path); // writes it
		}
		g_free(path);
		g_object_unref(folder);
	}
}

static void on_choose_folder(GtkButton *button, gpointer data) {
	struct screensaver_page *p = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Pictures for the slideshow");
	const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	if (pictures) {
		GFile *folder = g_file_new_for_path(pictures);
		gtk_file_dialog_set_initial_folder(dialog, folder);
		g_object_unref(folder);
	}
	gtk_file_dialog_select_folder(dialog, p->s->window, NULL, on_folder_chosen, p);
	g_object_unref(dialog);
}

static void on_preview(GtkButton *button, gpointer data) {
	struct screensaver_page *p = data;
	const char *name = picked(p);
	if (!name) {
		return;
	}
	const char *argv[] = { "tilewin-screensaver", "--saver", name, NULL };
	GError *error = NULL;
	if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
			&error)) {
		settings_status(p->s, "Could not start the screen saver: %s", error->message);
		g_clear_error(&error);
	}
}

GtkWidget *screensaver_page_new(struct settings *s) {
	struct screensaver_page *p = g_new0(struct screensaver_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Screen saver",
		"What the screens show once the computer has been left alone for a while. Moving "
		"the mouse or pressing a key ends it.", &content);

	p->preview = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->preview), PREVIEW_W + 2 * BEZEL);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->preview),
		PREVIEW_H + 2 * BEZEL + 26);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->preview), draw_preview, p, NULL);
	gtk_widget_add_tick_callback(p->preview, preview_tick, p, NULL);
	gtk_widget_set_margin_top(p->preview, 8);
	gtk_box_append(GTK_BOX(content), p->preview);

	GtkWidget *group = ui_group(content, "Screen saver", NULL);
	GtkStringList *names = gtk_string_list_new((const char *const[]){ "None", "Random", NULL });
	for (int i = 0; i < saver_count; i++) {
		gtk_string_list_append(names, savers[i]->title);
	}
	p->saver_dd = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	g_signal_connect(p->saver_dd, "notify::selected", G_CALLBACK(on_saver), p);
	p->preview_button = gtk_button_new_with_label("Preview");
	g_signal_connect(p->preview_button, "clicked", G_CALLBACK(on_preview), p);
	GtkWidget *pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(pick), p->saver_dd);
	gtk_box_append(GTK_BOX(pick), p->preview_button);
	p->saver_row = ui_row(group, "Screen saver", "Bubbles, Mystify, Ribbons, 3D Text and "
		"Photos from Windows 7, Starfield, 3D Pipes, 3D Maze and Flying Windows from before, "
		"Aurora, Word Clock, Tiling, Diggers, Hellfire and Thunderstorm of tileWin's own, the "
		"Matrix, and "
		"a word from Microslop", pick);

	p->wait_dd = gtk_drop_down_new_from_strings(wait_labels);
	g_signal_connect(p->wait_dd, "notify::selected", G_CALLBACK(on_wait), p);
	p->wait_row = ui_row(group, "Wait", "Without input for this long; an app playing a "
		"video keeps it away", p->wait_dd);
	p->lock_switch = gtk_switch_new();
	g_signal_connect(p->lock_switch, "notify::active", G_CALLBACK(on_lock), p);
	p->lock_row = ui_row(group, "On resume, display the lock screen",
		"Coming back asks for the password", p->lock_switch);

	GtkWidget *options = ui_group(content, "Options", NULL);
	p->options = gtk_widget_get_parent(options); // the heading goes with the rows
	p->speed_dd = gtk_drop_down_new_from_strings(speed_labels);
	g_signal_connect(p->speed_dd, "notify::selected", G_CALLBACK(on_speed), p);
	p->speed_row = ui_row(options, "Speed", NULL, p->speed_dd);
	p->text_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->text_entry), "tileWin");
	gtk_widget_set_size_request(p->text_entry, 260, -1);
	g_object_set_data(G_OBJECT(p->text_entry), "key", (gpointer)"text");
	g_signal_connect(p->text_entry, "changed", G_CALLBACK(on_text), p);
	p->text_row = ui_row(options, "3D Text", "The words it shows; \"time\" shows the clock",
		p->text_entry);
	GtkWidget *folder = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->photos_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->photos_entry), "Pictures");
	gtk_widget_set_size_request(p->photos_entry, 220, -1);
	g_object_set_data(G_OBJECT(p->photos_entry), "key", (gpointer)"photos");
	g_signal_connect(p->photos_entry, "changed", G_CALLBACK(on_text), p);
	gtk_box_append(GTK_BOX(folder), p->photos_entry);
	GtkWidget *browse = gtk_button_new_with_label("Choose…");
	g_signal_connect(browse, "clicked", G_CALLBACK(on_choose_folder), p);
	gtk_box_append(GTK_BOX(folder), browse);
	p->photos_row = ui_row(options, "Photos", "The folder of the slideshow, with its "
		"subfolders; your Pictures folder by default", folder);
	p->seconds_spin = gtk_spin_button_new_with_range(2, 120, 1);
	g_signal_connect(p->seconds_spin, "value-changed", G_CALLBACK(on_seconds), p);
	p->seconds_row = ui_row(options, "Slide show speed", "Seconds each photo stays",
		p->seconds_spin);
	add_own_rows(p, options);

	s->screensaver_page = p;
	refresh(p);
	return page;
}
