#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/*
 * Mouse & touchpad page: libinput settings in the "input type:pointer" and
 * "input type:touchpad" blocks of common.conf, applied live over IPC, and the
 * cursor theme ("seat * xcursor_theme") which is also given to GTK apps.
 */

enum control_kind {
	CONTROL_SWITCH,
	CONTROL_SCALE,
	CONTROL_CHOICE,
};

struct control {
	const char *block, *key, *title, *hint;
	enum control_kind kind;
	const char *on, *off; // switch values
	bool default_on;
	double min, max, step, default_value; // scale
	const char *const *values, *const *labels; // choice
};

static const char *const tap_map_values[] = { "lrm", "lmr", NULL };
static const char *const tap_map_labels[] = { "Right click", "Middle click", NULL };
static const char *const scroll_values[] = { "two_finger", "edge", "none", NULL };
static const char *const scroll_labels[] = { "Two fingers", "Edge of the touchpad", "Off", NULL };
static const char *const click_values[] = { "button_areas", "clickfinger", NULL };
static const char *const click_labels[] = { "Bottom corners of the touchpad",
	"Number of fingers (two = right)", NULL };
static const char *const events_values[] = { "enabled", "disabled_on_external_mouse", "disabled", NULL };
static const char *const events_labels[] = { "On", "Off while a mouse is connected", "Off", NULL };

#define POINTER "type:pointer"
#define TOUCHPAD "type:touchpad"

static const struct control pointer_controls[] = {
	{ .block = POINTER, .key = "pointer_accel", .title = "Pointer speed", .hint = NULL, .kind = CONTROL_SCALE, .min = -1, .max = 1, .step = 0.05, .default_value = 0 },
	{ .block = POINTER, .key = "accel_profile", .title = "Mouse acceleration", .hint = "Fast movements move the pointer further", .kind = CONTROL_SWITCH, .on = "adaptive", .off = "flat", .default_on = true },
	{ .block = POINTER, .key = "scroll_factor", .title = "Scrolling speed", .hint = NULL, .kind = CONTROL_SCALE, .min = 0.2, .max = 3, .step = 0.1, .default_value = 1 },
	{ .block = POINTER, .key = "natural_scroll", .title = "Natural scrolling", .hint = "Content follows the wheel like on a touch screen", .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ .block = POINTER, .key = "left_handed", .title = "Left-handed", .hint = "Swaps the left and right buttons", .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ .block = POINTER, .key = "middle_emulation", .title = "Middle click with both buttons", .hint = NULL, .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ 0 },
};

static const struct control touchpad_controls[] = {
	{ .block = TOUCHPAD, .key = "events", .title = "Touchpad", .hint = NULL, .kind = CONTROL_CHOICE, .values = events_values, .labels = events_labels },
	{ .block = TOUCHPAD, .key = "tap", .title = "Tap to click", .hint = NULL, .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ .block = TOUCHPAD, .key = "tap_button_map", .title = "Two-finger tap", .hint = "Three fingers do the other one", .kind = CONTROL_CHOICE, .values = tap_map_values, .labels = tap_map_labels },
	{ .block = TOUCHPAD, .key = "drag", .title = "Tap and drag", .hint = "Tap, then touch again and move to drag", .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = true },
	{ .block = TOUCHPAD, .key = "drag_lock", .title = "Drag lock", .hint = "Lifting the finger briefly does not end a drag", .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ .block = TOUCHPAD, .key = "dwt", .title = "Disable while typing", .hint = NULL, .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = true },
	{ .block = TOUCHPAD, .key = "pointer_accel", .title = "Pointer speed", .hint = NULL, .kind = CONTROL_SCALE, .min = -1, .max = 1, .step = 0.05, .default_value = 0 },
	{ .block = TOUCHPAD, .key = "accel_profile", .title = "Acceleration", .hint = "Fast movements move the pointer further", .kind = CONTROL_SWITCH, .on = "adaptive", .off = "flat", .default_on = true },
	{ .block = TOUCHPAD, .key = "natural_scroll", .title = "Natural scrolling", .hint = "Content follows the fingers", .kind = CONTROL_SWITCH, .on = "enabled", .off = "disabled", .default_on = false },
	{ .block = TOUCHPAD, .key = "scroll_method", .title = "Scroll with", .hint = NULL, .kind = CONTROL_CHOICE, .values = scroll_values, .labels = scroll_labels },
	{ .block = TOUCHPAD, .key = "scroll_factor", .title = "Scrolling speed", .hint = NULL, .kind = CONTROL_SCALE, .min = 0.2, .max = 3, .step = 0.1, .default_value = 1 },
	{ .block = TOUCHPAD, .key = "click_method", .title = "Right click by pressing", .hint = NULL, .kind = CONTROL_CHOICE, .values = click_values, .labels = click_labels },
	{ 0 },
};

static const int cursor_sizes[] = { 16, 24, 32, 48, 64, 96 };

struct mouse_page {
	struct settings *s;
	bool updating;
	GPtrArray *bindings; // struct control_binding *
	GPtrArray *cursor_themes; // char *
	GtkWidget *theme_dd, *size_dd, *trail;
	guint trail_timer;
};

struct control_binding {
	struct mouse_page *p;
	const struct control *control;
	GtkWidget *widget;
	guint timer;
};

static struct confdoc *common(struct mouse_page *p) {
	return p->s->common;
}

static char *control_value(struct mouse_page *p, const struct control *c) {
	struct cstmt *block = confdoc_block(common(p), "input", c->block, false);
	const char *value = cstmt_arg(confdoc_child(block, c->key, NULL), 0);
	return g_strdup(value);
}

static void apply(struct mouse_page *p, const struct control *c, const char *value) {
	struct confdoc *d = common(p);
	struct cstmt *block = confdoc_block(d, "input", c->block, true);
	confdoc_set(d, block, c->key, NULL, value);
	settings_common_changed(p->s, false);
	settings_command(p->s, "input %s %s %s", c->block, c->key, value);
}

static gboolean on_switch(GtkSwitch *widget, gboolean active, gpointer data) {
	struct control_binding *b = data;
	if (!b->p->updating) {
		apply(b->p, b->control, active ? b->control->on : b->control->off);
	}
	return FALSE;
}

static gboolean apply_scale(gpointer data) {
	struct control_binding *b = data;
	b->timer = 0;
	char value[32];
	g_ascii_formatd(value, sizeof(value), "%.2f", gtk_range_get_value(GTK_RANGE(b->widget)));
	apply(b->p, b->control, value);
	return G_SOURCE_REMOVE;
}

static void on_scale(GtkRange *range, gpointer data) {
	struct control_binding *b = data;
	if (b->p->updating) {
		return;
	}
	if (b->timer) {
		g_source_remove(b->timer);
	}
	b->timer = g_timeout_add(200, apply_scale, b);
}

static void on_choice(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct control_binding *b = data;
	if (b->p->updating) {
		return;
	}
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	apply(b->p, b->control, b->control->values[sel]);
}

static void add_controls(struct mouse_page *p, GtkWidget *group, const struct control *controls) {
	for (const struct control *c = controls; c->key; c++) {
		struct control_binding *b = g_new0(struct control_binding, 1);
		b->p = p;
		b->control = c;
		switch (c->kind) {
		case CONTROL_SWITCH:
			b->widget = gtk_switch_new();
			g_signal_connect(b->widget, "state-set", G_CALLBACK(on_switch), b);
			break;
		case CONTROL_SCALE:
			b->widget = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, c->min, c->max, c->step);
			gtk_widget_set_size_request(b->widget, 260, -1);
			gtk_scale_add_mark(GTK_SCALE(b->widget), c->default_value, GTK_POS_BOTTOM, NULL);
			g_signal_connect(b->widget, "value-changed", G_CALLBACK(on_scale), b);
			break;
		case CONTROL_CHOICE:
			b->widget = gtk_drop_down_new_from_strings(c->labels);
			g_signal_connect(b->widget, "notify::selected", G_CALLBACK(on_choice), b);
			break;
		}
		ui_row(group, c->title, c->hint, b->widget);
		g_ptr_array_add(p->bindings, b);
	}
}

/* ---------- cursor ---------- */

static void collect_cursor_themes(GPtrArray *themes, const char *dir) {
	GDir *d = dir ? g_dir_open(dir, 0, NULL) : NULL;
	const char *name;
	while (d && (name = g_dir_read_name(d))) {
		char *cursors = g_build_filename(dir, name, "cursors", NULL);
		bool found = false;
		for (guint i = 0; i < themes->len; i++) {
			found |= strcmp(themes->pdata[i], name) == 0;
		}
		if (!found && g_file_test(cursors, G_FILE_TEST_IS_DIR)) {
			g_ptr_array_add(themes, g_strdup(name));
		}
		g_free(cursors);
	}
	if (d) {
		g_dir_close(d);
	}
}

static int str_cmp(gconstpointer a, gconstpointer b) {
	return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

static struct cstmt *cursor_statement(struct confdoc *d) {
	for (guint i = 0; i < d->root->children->len; i++) {
		struct cstmt *c = d->root->children->pdata[i];
		if (strcmp(c->name, "seat") == 0 && g_strcmp0(cstmt_arg(c, 1), "xcursor_theme") == 0) {
			return c;
		}
	}
	return NULL;
}

static GSettings *interface_settings(void) {
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema = source ?
		g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : NULL;
	if (!schema) {
		return NULL;
	}
	g_settings_schema_unref(schema);
	return g_settings_new("org.gnome.desktop.interface");
}

static void on_cursor_changed(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct mouse_page *p = data;
	guint theme = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->theme_dd));
	guint size = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->size_dd));
	if (p->updating || theme >= p->cursor_themes->len || size >= G_N_ELEMENTS(cursor_sizes)) {
		return;
	}
	const char *name = p->cursor_themes->pdata[theme];
	char *args = g_strdup_printf("* xcursor_theme %s %d", name, cursor_sizes[size]);
	struct confdoc *d = common(p);
	struct cstmt *stmt = cursor_statement(d);
	char *line = g_strdup_printf("seat %s", args);
	if (stmt) {
		confdoc_replace(d, stmt, line);
	} else {
		confdoc_append(d, d->root, line);
	}
	settings_common_changed(p->s, false);
	settings_command(p->s, "seat %s", args);
	// GTK and GNOME apps read the cursor from gsettings
	GSettings *gs = interface_settings();
	if (gs) {
		g_settings_set_string(gs, "cursor-theme", name);
		g_settings_set_int(gs, "cursor-size", cursor_sizes[size]);
		g_object_unref(gs);
	}
	g_free(line);
	g_free(args);
}

/* ---------- pointer trail ---------- */

static gboolean apply_trail(gpointer data) {
	struct mouse_page *p = data;
	p->trail_timer = 0;
	char value[16];
	snprintf(value, sizeof(value), "%d",
		(int)round(gtk_range_get_value(GTK_RANGE(p->trail))));
	struct confdoc *d = common(p);
	confdoc_set(d, d->root, "pointer_trail", NULL, value);
	settings_common_changed(p->s, false);
	settings_command(p->s, "pointer_trail %s", value);
	return G_SOURCE_REMOVE;
}

static void on_trail(GtkRange *range, gpointer data) {
	struct mouse_page *p = data;
	if (p->updating) {
		return;
	}
	if (p->trail_timer) {
		g_source_remove(p->trail_timer);
	}
	p->trail_timer = g_timeout_add(300, apply_trail, p);
}

/* ---------- page ---------- */

void mouse_page_refresh(struct settings *s) {
	struct mouse_page *p = s->mouse_page;
	if (!p) {
		return;
	}
	p->updating = true;
	for (guint i = 0; i < p->bindings->len; i++) {
		struct control_binding *b = p->bindings->pdata[i];
		const struct control *c = b->control;
		char *value = control_value(p, c);
		switch (c->kind) {
		case CONTROL_SWITCH:
			gtk_switch_set_active(GTK_SWITCH(b->widget),
				value ? strcmp(value, c->on) == 0 : c->default_on);
			break;
		case CONTROL_SCALE:
			gtk_range_set_value(GTK_RANGE(b->widget),
				value ? g_ascii_strtod(value, NULL) : c->default_value);
			break;
		case CONTROL_CHOICE:;
			guint sel = 0;
			for (guint v = 0; value && c->values[v]; v++) {
				if (strcmp(value, c->values[v]) == 0) {
					sel = v;
				}
			}
			gtk_drop_down_set_selected(GTK_DROP_DOWN(b->widget), sel);
			break;
		}
		g_free(value);
	}

	struct cstmt *cursor = cursor_statement(common(p));
	const char *theme = cstmt_arg(cursor, 2);
	const char *size = cstmt_arg(cursor, 3);
	char *gtk_theme = NULL;
	int gtk_size = 24;
	GSettings *gs = interface_settings();
	if (gs) {
		gtk_theme = g_settings_get_string(gs, "cursor-theme");
		gtk_size = g_settings_get_int(gs, "cursor-size");
		g_object_unref(gs);
	}
	const char *current = theme ? theme : gtk_theme ? gtk_theme : "default";
	int current_size = size ? atoi(size) : gtk_size;
	for (guint i = 0; i < p->cursor_themes->len; i++) {
		if (strcmp(p->cursor_themes->pdata[i], current) == 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->theme_dd), i);
		}
	}
	for (guint i = 0; i < G_N_ELEMENTS(cursor_sizes); i++) {
		if (cursor_sizes[i] == current_size) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(p->size_dd), i);
		}
	}
	const char *trail = cstmt_arg(confdoc_child(common(p)->root, "pointer_trail", NULL), 0);
	gtk_range_set_value(GTK_RANGE(p->trail), trail ? atoi(trail) : 0);

	g_free(gtk_theme);
	p->updating = false;
}

GtkWidget *mouse_page_new(struct settings *s) {
	struct mouse_page *p = g_new0(struct mouse_page, 1);
	p->s = s;
	p->bindings = g_ptr_array_new_with_free_func(g_free);
	GtkWidget *content;
	GtkWidget *page = ui_page("Mouse & touchpad",
		"Changes apply immediately to all connected mice and touchpads.", &content);

	GtkWidget *mouse = ui_group(content, "Mouse", NULL);
	add_controls(p, mouse, pointer_controls);
	GtkWidget *touchpad = ui_group(content, "Touchpad",
		"Only shown by devices that support a setting; other devices ignore it.");
	add_controls(p, touchpad, touchpad_controls);

	p->cursor_themes = g_ptr_array_new_with_free_func(g_free);
	char *user_icons = g_build_filename(g_get_user_data_dir(), "icons", NULL);
	char *home_icons = g_build_filename(g_get_home_dir(), ".icons", NULL);
	collect_cursor_themes(p->cursor_themes, user_icons);
	collect_cursor_themes(p->cursor_themes, home_icons);
	collect_cursor_themes(p->cursor_themes, "/usr/local/share/icons");
	collect_cursor_themes(p->cursor_themes, "/usr/share/icons");
	g_free(user_icons);
	g_free(home_icons);
	g_ptr_array_sort(p->cursor_themes, str_cmp);
	GtkStringList *theme_model = gtk_string_list_new(NULL);
	for (guint i = 0; i < p->cursor_themes->len; i++) {
		gtk_string_list_append(theme_model, p->cursor_themes->pdata[i]);
	}
	GtkWidget *cursor = ui_group(content, "Pointer", NULL);
	p->theme_dd = gtk_drop_down_new(G_LIST_MODEL(theme_model), NULL);
	g_signal_connect(p->theme_dd, "notify::selected", G_CALLBACK(on_cursor_changed), p);
	ui_row(cursor, "Cursor theme", p->cursor_themes->len ? NULL : "No cursor themes installed",
		p->theme_dd);
	GtkStringList *size_model = gtk_string_list_new(NULL);
	for (size_t i = 0; i < G_N_ELEMENTS(cursor_sizes); i++) {
		char label[16];
		snprintf(label, sizeof(label), "%d", cursor_sizes[i]);
		gtk_string_list_append(size_model, label);
	}
	p->size_dd = gtk_drop_down_new(G_LIST_MODEL(size_model), NULL);
	g_signal_connect(p->size_dd, "notify::selected", G_CALLBACK(on_cursor_changed), p);
	ui_row(cursor, "Cursor size", "Apps started later use the new cursor", p->size_dd);

	p->trail = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 20, 1);
	gtk_widget_set_size_request(p->trail, 260, -1);
	gtk_scale_set_draw_value(GTK_SCALE(p->trail), TRUE);
	gtk_scale_set_digits(GTK_SCALE(p->trail), 0);
	gtk_scale_set_value_pos(GTK_SCALE(p->trail), GTK_POS_LEFT);
	gtk_scale_add_mark(GTK_SCALE(p->trail), 0, GTK_POS_BOTTOM, NULL);
	g_signal_connect(p->trail, "value-changed", G_CALLBACK(on_trail), p);
	ui_row(cursor, "Pointer trail",
		"How many copies of the pointer follow it while it moves; 0 turns the trail off",
		p->trail);

	s->mouse_page = p;
	mouse_page_refresh(s);
	return page;
}
