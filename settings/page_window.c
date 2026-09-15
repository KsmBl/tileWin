#include <math.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"

/*
 * Window behavior page: snapping, sticking, moving touching windows together
 * and stretching by double-click (tileWin commands in common.conf, applied over
 * IPC), and the sway settings of window mode for moving windows and focus
 * (windowmode.conf, applied by reloading tileWin).
 */

enum doc {
	DOC_COMMON,
	DOC_WINDOWMODE,
};

enum control_kind {
	CONTROL_SWITCH,
	CONTROL_SCALE,
	CONTROL_CHOICE,
};

struct control {
	enum doc doc;
	const char *key, *title, *hint;
	enum control_kind kind;
	bool default_on; // switch: writes enable / disable
	double min, max, step, default_value; // scale
	const char *const *values, *const *labels; // choice, the first one is the default
	const char *suffix; // written after a choice, e.g. floating_modifier's "normal"
};

static const char *const group_values[] = { "Shift", "Ctrl", "Alt", "Super", "none", NULL };
static const char *const group_labels[] = { "Shift", "Ctrl", "Alt", "Super", "Off", NULL };
static const char *const move_values[] = { "Super", "Alt", "Ctrl", "none", NULL };
static const char *const move_labels[] = { "Super", "Alt", "Ctrl", "Off", NULL };
static const char *const follows_values[] = { "no", "yes", "always", NULL };
static const char *const follows_labels[] = { "Clicking it", "Pointing at it",
	"Pointing at it, also after switching desktops", NULL };
static const char *const activation_values[] = { "focus", "smart", "urgent", "none", NULL };
static const char *const activation_labels[] = { "Switch to it",
	"Switch to it if it is on this desktop", "Highlight its taskbar button", "Nothing", NULL };

static const struct control move_controls[] = {
	{ .doc = DOC_COMMON, .key = "window_snap", .title = "Snap to screen edges",
		.hint = "Drag a window to a side for half the screen, to a corner for a quarter, "
		"to the top to maximize it", .kind = CONTROL_SWITCH, .default_on = true },
	{ .doc = DOC_COMMON, .key = "window_stick", .title = "Stick windows together",
		.hint = "Moved and resized windows stick to the edges of other windows and of the "
		"screen", .kind = CONTROL_SWITCH, .default_on = true },
	{ .doc = DOC_COMMON, .key = "window_stick_distance", .title = "Sticking distance",
		.hint = "How close in pixels an edge has to come to stick", .kind = CONTROL_SCALE,
		.min = 2, .max = 40, .step = 1, .default_value = 12 },
	{ .doc = DOC_COMMON, .key = "window_group_modifier",
		.title = "Move stuck windows together with",
		.hint = "Hold this key while dragging a window to take the windows touching it along",
		.kind = CONTROL_CHOICE, .values = group_values, .labels = group_labels },
	{ .doc = DOC_COMMON, .key = "window_stretch", .title = "Stretch by double-clicking a side",
		.hint = "Left or right side: as wide as there is room up to the next window or the "
		"screen edge, top or bottom side: as high. Double-click again for the old size",
		.kind = CONTROL_SWITCH, .default_on = true },
	{ .doc = DOC_WINDOWMODE, .key = "floating_modifier", .title = "Move windows anywhere with",
		.hint = "Hold this key and drag a window with the left mouse button to move it, with "
		"the right one to resize it", .kind = CONTROL_CHOICE, .values = move_values,
		.labels = move_labels, .suffix = "normal" },
	{ 0 },
};

static const struct control focus_controls[] = {
	{ .doc = DOC_WINDOWMODE, .key = "focus_follows_mouse", .title = "Focus a window by",
		.kind = CONTROL_CHOICE, .values = follows_values, .labels = follows_labels },
	{ .doc = DOC_WINDOWMODE, .key = "focus_on_window_activation",
		.title = "When an app asks for attention",
		.hint = "For example a chat app with a new message, or a link opened in the browser",
		.kind = CONTROL_CHOICE, .values = activation_values, .labels = activation_labels },
	{ 0 },
};

struct window_page {
	struct settings *s;
	bool updating;
	GPtrArray *bindings; // struct binding *
};

struct binding {
	struct window_page *p;
	const struct control *control;
	GtkWidget *widget;
	guint timer;
};

static struct confdoc *doc_of(struct window_page *p, const struct control *c) {
	return c->doc == DOC_COMMON ? p->s->common : p->s->windowmode;
}

/* The names sway accepts for one modifier, as the value in the list. */
static const char *normalize(const char *value) {
	if (!value) {
		return NULL;
	}
	if (g_ascii_strcasecmp(value, "$mod") == 0 || g_ascii_strcasecmp(value, "Mod4") == 0 ||
			g_ascii_strcasecmp(value, "Logo") == 0) {
		return "Super";
	}
	if (g_ascii_strcasecmp(value, "Mod1") == 0) {
		return "Alt";
	}
	if (g_ascii_strcasecmp(value, "Control") == 0) {
		return "Ctrl";
	}
	return value;
}

static void apply(struct window_page *p, const struct control *c, const char *value) {
	struct confdoc *d = doc_of(p, c);
	char *args = c->suffix && strcmp(value, "none") != 0 ?
		g_strdup_printf("%s %s", value, c->suffix) : g_strdup(value);
	confdoc_set(d, d->root, c->key, NULL, args);
	if (c->doc == DOC_COMMON) {
		settings_common_changed(p->s, false);
		settings_command(p->s, "%s %s", c->key, args);
	} else {
		// only window mode uses it: tileWin reloads its config once it is saved
		settings_mode_changed(p->s, d);
	}
	g_free(args);
}

static void on_switch(GObject *object, GParamSpec *pspec, gpointer data) {
	struct binding *b = data;
	if (!b->p->updating) {
		apply(b->p, b->control, gtk_switch_get_active(GTK_SWITCH(b->widget)) ?
			"enable" : "disable");
	}
}

static gboolean apply_scale(gpointer data) {
	struct binding *b = data;
	b->timer = 0;
	char value[32];
	snprintf(value, sizeof(value), "%d", (int)round(gtk_range_get_value(GTK_RANGE(b->widget))));
	apply(b->p, b->control, value);
	return G_SOURCE_REMOVE;
}

static void on_scale(GtkRange *range, gpointer data) {
	struct binding *b = data;
	if (b->p->updating) {
		return;
	}
	if (b->timer) {
		g_source_remove(b->timer);
	}
	b->timer = g_timeout_add(300, apply_scale, b);
}

static void on_choice(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct binding *b = data;
	if (!b->p->updating) {
		guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
		apply(b->p, b->control, b->control->values[sel]);
	}
}

static void add_controls(struct window_page *p, GtkWidget *group, const struct control *controls) {
	for (const struct control *c = controls; c->key; c++) {
		struct binding *b = g_new0(struct binding, 1);
		b->p = p;
		b->control = c;
		switch (c->kind) {
		case CONTROL_SWITCH:
			b->widget = gtk_switch_new();
			g_signal_connect(b->widget, "notify::active", G_CALLBACK(on_switch), b);
			break;
		case CONTROL_SCALE:
			b->widget = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, c->min, c->max,
				c->step);
			gtk_widget_set_size_request(b->widget, 260, -1);
			gtk_scale_set_draw_value(GTK_SCALE(b->widget), TRUE);
			gtk_scale_set_digits(GTK_SCALE(b->widget), 0);
			gtk_scale_set_value_pos(GTK_SCALE(b->widget), GTK_POS_LEFT);
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

void window_page_refresh(struct settings *s) {
	struct window_page *p = s->window_page;
	if (!p) {
		return;
	}
	p->updating = true;
	for (guint i = 0; i < p->bindings->len; i++) {
		struct binding *b = p->bindings->pdata[i];
		const struct control *c = b->control;
		struct confdoc *d = doc_of(p, c);
		const char *value = cstmt_arg(confdoc_child(d->root, c->key, NULL), 0);
		switch (c->kind) {
		case CONTROL_SWITCH:
			gtk_switch_set_active(GTK_SWITCH(b->widget), !value ? c->default_on :
				!(g_ascii_strcasecmp(value, "disable") == 0 ||
				g_ascii_strcasecmp(value, "no") == 0 ||
				g_ascii_strcasecmp(value, "false") == 0 ||
				g_ascii_strcasecmp(value, "off") == 0));
			break;
		case CONTROL_SCALE:
			gtk_range_set_value(GTK_RANGE(b->widget),
				value ? g_ascii_strtod(value, NULL) : c->default_value);
			break;
		case CONTROL_CHOICE:;
			guint sel = 0;
			const char *name = normalize(value);
			for (guint v = 0; name && c->values[v]; v++) {
				if (g_ascii_strcasecmp(name, c->values[v]) == 0) {
					sel = v;
				}
			}
			gtk_drop_down_set_selected(GTK_DROP_DOWN(b->widget), sel);
			break;
		}
	}
	p->updating = false;
}

GtkWidget *window_page_new(struct settings *s) {
	struct window_page *p = g_new0(struct window_page, 1);
	p->s = s;
	p->bindings = g_ptr_array_new_with_free_func(g_free);
	GtkWidget *content;
	GtkWidget *page = ui_page("Window behavior",
		"How windows move, snap and get focus in window mode. Tile mode keeps its own sway "
		"settings.", &content);
	GtkWidget *moving = ui_group(content, "Moving and resizing", NULL);
	add_controls(p, moving, move_controls);
	GtkWidget *focus = ui_group(content, "Focus", NULL);
	add_controls(p, focus, focus_controls);
	s->window_page = p;
	window_page_refresh(s);
	return page;
}
