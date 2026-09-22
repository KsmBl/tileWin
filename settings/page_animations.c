#include <math.h>
#include <string.h>
#include "settings.h"

/*
 * Animations page: all animations on or off, their speed, and for each kind
 * (opening, closing, minimizing, maximizing and switching desktops) on or off
 * and its style. Written to common.conf as "animations", "animation_speed" and
 * "animation <kind> <style> [disable]", and applied live over IPC. The preview
 * buttons play an animation on the settings window itself.
 */

#define SETTINGS_WINDOW "[app_id=\"org.tilewin.Settings\" title=\"^tileWin Settings$\"]"

struct kind {
	const char *key, *title, *hint;
	const char *const *styles, *const *labels;
};

static const char *const open_styles[] = { "rise", "fade", "zoom", "pop", "drop", NULL };
static const char *const open_labels[] = { "Fade in and rise", "Fade in", "Zoom in", "Pop up",
	"Drop in", NULL };
static const char *const close_styles[] = { "shrink", "fade", "grow", "drop", "explode", NULL };
static const char *const close_labels[] = { "Shrink and fade out", "Fade out",
	"Grow and fade out", "Fall and fade out", "Explode", NULL };
static const char *const minimize_styles[] = { "taskbar", "fade", "shrink", "drop", NULL };
static const char *const minimize_labels[] = { "Fly to the taskbar", "Fade out",
	"Shrink in place", "Slide down", NULL };
static const char *const maximize_styles[] = { "morph", "bounce", "fade", NULL };
static const char *const maximize_labels[] = { "Stretch", "Stretch with a bounce", "Cross-fade",
	NULL };
static const char *const desktop_styles[] = { "slide", "vertical", "fade", "zoom", NULL };
static const char *const desktop_labels[] = { "Slide sideways", "Slide up and down", "Fade",
	"Zoom out", NULL };

enum { KIND_OPEN, KIND_CLOSE, KIND_MINIMIZE, KIND_MAXIMIZE, KIND_DESKTOP, KIND_COUNT };

static const struct kind kinds[KIND_COUNT] = {
	{ "open", "Opening windows", NULL, open_styles, open_labels },
	{ "close", "Closing windows", NULL, close_styles, close_labels },
	{ "minimize", "Minimizing and restoring", NULL, minimize_styles, minimize_labels },
	{ "maximize", "Maximizing and snapping",
		"Also restoring, and stretching a window by double-clicking its side",
		maximize_styles, maximize_labels },
	{ "desktop", "Switching desktops", "The preview needs a second desktop",
		desktop_styles, desktop_labels },
};

struct animations_page;

struct kind_row {
	struct animations_page *p;
	int kind;
	GtkWidget *style_dd, *on_switch, *preview;
};

struct animations_page {
	struct settings *s;
	bool updating;
	GtkWidget *master_switch, *speed_scale, *expensive_switch;
	struct kind_row rows[KIND_COUNT];
	guint speed_timer;
	guint preview_timer;
	GtkWidget *preview_window;
};

static struct confdoc *common(struct animations_page *p) {
	return p->s->common;
}

static bool word_is(const char *value, const char *const *words) {
	for (int i = 0; value && words[i]; i++) {
		if (g_ascii_strcasecmp(value, words[i]) == 0) {
			return true;
		}
	}
	return false;
}

static const char *const off_words[] = { "disable", "no", "false", "off", NULL };
static const char *const on_words[] = { "enable", "yes", "true", "on", NULL };

/* ---------- reading common.conf ---------- */

static bool master_on(struct animations_page *p) {
	struct cstmt *stmt = confdoc_child(common(p)->root, "animations", NULL);
	return !word_is(cstmt_arg(stmt, 0), off_words);
}

static double speed(struct animations_page *p) {
	struct cstmt *stmt = confdoc_child(common(p)->root, "animation_speed", NULL);
	const char *value = cstmt_arg(stmt, 0);
	double v = value ? g_ascii_strtod(value, NULL) : 1;
	return v > 0 ? v : 1;
}

static bool expensive_on(struct animations_page *p) {
	struct cstmt *stmt = confdoc_child(common(p)->root, "expensive_calculations", NULL);
	return stmt && !word_is(cstmt_arg(stmt, 0), off_words);
}

static void kind_state(struct animations_page *p, int kind, guint *style, bool *on) {
	const struct kind *k = &kinds[kind];
	struct cstmt *stmt = confdoc_child(common(p)->root, "animation", k->key);
	*style = 0;
	*on = true;
	for (int i = 1; stmt && cstmt_arg(stmt, i); i++) {
		const char *arg = cstmt_arg(stmt, i);
		if (word_is(arg, off_words)) {
			*on = false;
		} else if (word_is(arg, on_words)) {
			*on = true;
		}
		for (guint j = 0; k->styles[j]; j++) {
			if (g_ascii_strcasecmp(arg, k->styles[j]) == 0) {
				*style = j;
			}
		}
	}
}

/* ---------- changes ---------- */

static void update_sensitivity(struct animations_page *p) {
	bool all = gtk_switch_get_active(GTK_SWITCH(p->master_switch));
	gtk_widget_set_sensitive(p->speed_scale, all);
	gtk_widget_set_sensitive(p->expensive_switch, all);
	for (int i = 0; i < KIND_COUNT; i++) {
		struct kind_row *r = &p->rows[i];
		bool on = all && gtk_switch_get_active(GTK_SWITCH(r->on_switch));
		gtk_widget_set_sensitive(r->on_switch, all);
		gtk_widget_set_sensitive(r->style_dd, on);
		gtk_widget_set_sensitive(r->preview, on);
	}
}

static void on_master(GObject *object, GParamSpec *pspec, gpointer data) {
	struct animations_page *p = data;
	if (p->updating) {
		return;
	}
	bool on = gtk_switch_get_active(GTK_SWITCH(p->master_switch));
	confdoc_set(common(p), common(p)->root, "animations", NULL, on ? NULL : "disable");
	settings_common_changed(p->s, false);
	settings_command(p->s, "animations %s", on ? "enable" : "disable");
	settings_status(p->s, on ? "Windows and desktops are animated" : "Animations are off");
	update_sensitivity(p);
}

static void on_expensive(GObject *object, GParamSpec *pspec, gpointer data) {
	struct animations_page *p = data;
	if (p->updating) {
		return;
	}
	bool on = gtk_switch_get_active(GTK_SWITCH(p->expensive_switch));
	confdoc_set(common(p), common(p)->root, "expensive_calculations", NULL, on ? "on" : NULL);
	settings_common_changed(p->s, false);
	settings_command(p->s, "expensive_calculations %s", on ? "on" : "off");
	settings_status(p->s, on ?
		"Windows are laid out again for every frame while they change size" :
		"A picture of the window is stretched while it changes size");
}

static gboolean apply_speed(gpointer data) {
	struct animations_page *p = data;
	p->speed_timer = 0;
	double v = round(gtk_range_get_value(GTK_RANGE(p->speed_scale)) * 100) / 100;
	char value[32];
	g_ascii_formatd(value, sizeof(value), "%g", v);
	confdoc_set(common(p), common(p)->root, "animation_speed", NULL,
		fabs(v - 1) < 0.005 ? NULL : value);
	settings_common_changed(p->s, false);
	settings_command(p->s, "animation_speed %s", value);
	return G_SOURCE_REMOVE;
}

static void on_speed(GtkRange *range, gpointer data) {
	struct animations_page *p = data;
	if (p->updating) {
		return;
	}
	if (p->speed_timer) {
		g_source_remove(p->speed_timer);
	}
	p->speed_timer = g_timeout_add(200, apply_speed, p);
}

static char *format_speed(GtkScale *scale, double value, gpointer data) {
	char number[32];
	g_ascii_formatd(number, sizeof(number), "%g", round(value * 100) / 100);
	return g_strdup_printf("%s×", number);
}

static void apply_kind(struct kind_row *r) {
	struct animations_page *p = r->p;
	const struct kind *k = &kinds[r->kind];
	guint style = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->style_dd));
	bool on = gtk_switch_get_active(GTK_SWITCH(r->on_switch));
	const char *name = k->styles[style];
	// the default (first style, on) needs no line
	char *args = style == 0 && on ? NULL :
		g_strdup_printf("%s %s%s", k->key, name, on ? "" : " disable");
	confdoc_set(common(p), common(p)->root, "animation", k->key, args);
	g_free(args);
	settings_common_changed(p->s, false);
	settings_command(p->s, "animation %s %s %s", k->key, name, on ? "enable" : "disable");
	update_sensitivity(p);
}

static void on_kind_switch(GObject *object, GParamSpec *pspec, gpointer data) {
	struct kind_row *r = data;
	if (!r->p->updating) {
		apply_kind(r);
	}
}

static void on_kind_style(GObject *object, GParamSpec *pspec, gpointer data) {
	struct kind_row *r = data;
	if (!r->p->updating) {
		apply_kind(r);
	}
}

/* ---------- previews ---------- */

static gboolean end_preview(gpointer data) {
	struct kind_row *r = data;
	struct animations_page *p = r->p;
	p->preview_timer = 0;
	switch (r->kind) {
	case KIND_OPEN:
	case KIND_CLOSE:
		if (p->preview_window) {
			gtk_window_destroy(GTK_WINDOW(p->preview_window));
			p->preview_window = NULL;
		}
		break;
	case KIND_MINIMIZE:
		settings_command(p->s, SETTINGS_WINDOW " minimize disable");
		break;
	case KIND_MAXIMIZE:
		settings_command(p->s, SETTINGS_WINDOW " maximize toggle");
		break;
	case KIND_DESKTOP:
		settings_command(p->s, "workspace prev_on_output");
		break;
	}
	return G_SOURCE_REMOVE;
}

static void on_preview(GtkButton *button, gpointer data) {
	struct kind_row *r = data;
	struct animations_page *p = r->p;
	if (p->preview_timer) {
		return; // one preview at a time
	}
	char *mode = settings_current_mode();
	bool window_mode = strcmp(mode, "window") == 0;
	g_free(mode);
	if (!window_mode || !tw_ipc_available()) {
		settings_status(p->s, "Previews work while tileWin runs in window mode");
		return;
	}
	int delay = 900;
	switch (r->kind) {
	case KIND_OPEN:
	case KIND_CLOSE: {
		GtkWidget *window = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(window), "Animation preview");
		gtk_window_set_default_size(GTK_WINDOW(window), 360, 200);
		gtk_window_set_transient_for(GTK_WINDOW(window), p->s->window);
		GtkWidget *label = gtk_label_new(kinds[r->kind].title);
		gtk_widget_add_css_class(label, "title-2");
		gtk_window_set_child(GTK_WINDOW(window), label);
		gtk_window_present(GTK_WINDOW(window));
		p->preview_window = window;
		delay = 1100;
		break;
	}
	case KIND_MINIMIZE:
		settings_command(p->s, SETTINGS_WINDOW " minimize enable");
		break;
	case KIND_MAXIMIZE:
		settings_command(p->s, SETTINGS_WINDOW " maximize toggle");
		break;
	case KIND_DESKTOP:
		settings_command(p->s, "workspace next_on_output");
		break;
	}
	p->preview_timer = g_timeout_add(delay, end_preview, r);
}

/* ---------- page ---------- */

void animations_page_refresh(struct settings *s) {
	struct animations_page *p = s->animations_page;
	if (!p) {
		return;
	}
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->master_switch), master_on(p));
	gtk_range_set_value(GTK_RANGE(p->speed_scale), speed(p));
	gtk_switch_set_active(GTK_SWITCH(p->expensive_switch), expensive_on(p));
	for (int i = 0; i < KIND_COUNT; i++) {
		guint style;
		bool on;
		kind_state(p, i, &style, &on);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->rows[i].style_dd), style);
		gtk_switch_set_active(GTK_SWITCH(p->rows[i].on_switch), on);
	}
	p->updating = false;
	update_sensitivity(p);
}

static void add_kind(struct animations_page *p, GtkWidget *group, int kind) {
	struct kind_row *r = &p->rows[kind];
	r->p = p;
	r->kind = kind;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	r->style_dd = gtk_drop_down_new_from_strings(kinds[kind].labels);
	gtk_widget_set_size_request(r->style_dd, 200, -1);
	g_signal_connect(r->style_dd, "notify::selected", G_CALLBACK(on_kind_style), r);
	gtk_box_append(GTK_BOX(box), r->style_dd);
	r->preview = gtk_button_new_with_label("Preview");
	gtk_widget_set_valign(r->preview, GTK_ALIGN_CENTER);
	g_signal_connect(r->preview, "clicked", G_CALLBACK(on_preview), r);
	gtk_box_append(GTK_BOX(box), r->preview);
	r->on_switch = gtk_switch_new();
	gtk_widget_set_valign(r->on_switch, GTK_ALIGN_CENTER);
	g_signal_connect(r->on_switch, "notify::active", G_CALLBACK(on_kind_switch), r);
	gtk_box_append(GTK_BOX(box), r->on_switch);
	ui_row(group, kinds[kind].title, kinds[kind].hint, box);
}

GtkWidget *animations_page_new(struct settings *s) {
	struct animations_page *p = g_new0(struct animations_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Animations",
		"How windows and desktops move. Changes apply right away; Preview plays an "
		"animation on this window.", &content);

	GtkWidget *general = ui_group(content, "All animations", NULL);
	p->master_switch = gtk_switch_new();
	g_signal_connect(p->master_switch, "notify::active", G_CALLBACK(on_master), p);
	ui_row(general, "Animations", "Turn every animation on or off", p->master_switch);
	p->speed_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.25, 3, 0.05);
	gtk_widget_set_size_request(p->speed_scale, 360, -1);
	gtk_scale_set_draw_value(GTK_SCALE(p->speed_scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(p->speed_scale), GTK_POS_LEFT);
	gtk_scale_set_format_value_func(GTK_SCALE(p->speed_scale), format_speed, NULL, NULL);
	gtk_scale_add_mark(GTK_SCALE(p->speed_scale), 0.5, GTK_POS_BOTTOM, NULL);
	gtk_scale_add_mark(GTK_SCALE(p->speed_scale), 1, GTK_POS_BOTTOM, "Normal");
	gtk_scale_add_mark(GTK_SCALE(p->speed_scale), 2, GTK_POS_BOTTOM, NULL);
	g_signal_connect(p->speed_scale, "value-changed", G_CALLBACK(on_speed), p);
	ui_row(general, "Speed", "Slower to the left, faster to the right; 2× plays them twice as fast", p->speed_scale);
	p->expensive_switch = gtk_switch_new();
	g_signal_connect(p->expensive_switch, "notify::active", G_CALLBACK(on_expensive), p);
	ui_row(general, "Expensive calculations",
		"While a window is snapped to an edge, maximized or resized, lay it out again for "
		"every frame instead of stretching a picture of it. The window is then never the "
		"wrong shape, at the cost of a redraw per frame.", p->expensive_switch);

	GtkWidget *windows = ui_group(content, "Windows", NULL);
	for (int i = KIND_OPEN; i <= KIND_MAXIMIZE; i++) {
		add_kind(p, windows, i);
	}
	GtkWidget *desktops = ui_group(content, "Desktops", NULL);
	add_kind(p, desktops, KIND_DESKTOP);

	s->animations_page = p;
	animations_page_refresh(s);
	return page;
}
