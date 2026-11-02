#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_theme.h"

/*
 * A card of a theme: its wallpaper, with a small window and taskbar over it in
 * the colors of the theme, so a card shows how the theme looks rather than only
 * what is behind the windows.
 */
struct card_look {
	uint32_t title, title_fg, frame, body, bar, start;
	char *gradient; // the stops of the title bar, as the theme has them
	bool sideways;  // from left to right, as Windows 95 to 2000 have it
	bool rounded;
	bool has_start;
};

static void look_free(gpointer data) {
	struct card_look *look = data;
	g_free(look->gradient);
	g_free(look);
}

static void source(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, ((c >> 24) & 0xff) / 255.0, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static uint32_t parse_color(const char *text, uint32_t fallback) {
	if (!text || text[0] != '#') {
		return fallback;
	}
	size_t len = strspn(text + 1, "0123456789abcdefABCDEF");
	uint32_t value = (uint32_t)strtoul(text + 1, NULL, 16);
	if (len == 6) {
		return (value << 8) | 0xff;
	}
	return len == 8 ? value : fallback;
}

/* "0:#000080 1:#1084d0" as a pattern between the two points. */
static cairo_pattern_t *gradient_pattern(const char *text, double x0, double y0, double x1,
		double y1) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(x0, y0, x1, y1);
	char *end;
	for (const char *p = text; *p;) {
		double at = g_ascii_strtod(p, &end);
		if (end == p || *end != ':') {
			break;
		}
		uint32_t c = parse_color(end + 1, 0x000000ff);
		cairo_pattern_add_color_stop_rgb(pattern, at, ((c >> 24) & 0xff) / 255.0,
			((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0);
		p = end + 1 + strcspn(end + 1, " ");
		p += strspn(p, " ");
	}
	return pattern;
}

static void read_look(struct card_look *look, const struct tw_theme *t) {
	uint32_t accent = tw_theme_color(t, "accent.color", 0x0078d7ff);
	look->title = tw_theme_color(t, "decoration.active.title_bg", accent);
	look->gradient = g_strdup(tw_theme_str(t, "decoration.active.title_gradient", NULL));
	// two colors run sideways, more of them are the shine of a bar from top to bottom
	look->sideways = look->gradient && strchr(look->gradient, ' ') &&
		!strchr(strchr(look->gradient, ' ') + 1, ' ');
	look->title_fg = tw_theme_color(t, "decoration.active.title_fg", 0xffffffff);
	look->frame = tw_theme_color(t, "decoration.active.frame", look->title);
	look->body = tw_theme_color(t, "menu.bg", 0xf0f0f0ff);
	look->bar = tw_theme_color(t, "panel.bg", 0x202020ff);
	look->start = tw_theme_color(t, "taskbar.indicator", accent);
	const char *style = tw_theme_str(t, "style", "");
	look->rounded = strcmp(style, "win11") == 0 || strcmp(style, "sway") == 0;
	look->has_start = strcmp(style, "win1") != 0 && strcmp(style, "win3") != 0;
}

static void rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_path(cr);
	if (r <= 0) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

static void draw_look(GtkDrawingArea *area, cairo_t *cr, int W, int H, gpointer data) {
	const struct card_look *look = data;
	double radius = look->rounded ? 4 : 0;
	double bar_h = H * 0.1;
	double wx = W * 0.14, wy = H * 0.14, ww = W * 0.62, wh = H * 0.56, tit = H * 0.1;
	// the window: its frame, title bar, and buttons
	rect(cr, wx - 1, wy - 1, ww + 2, wh + 2, radius);
	source(cr, look->frame);
	cairo_fill(cr);
	rect(cr, wx, wy, ww, wh, radius);
	source(cr, look->body);
	cairo_fill(cr);
	cairo_save(cr);
	rect(cr, wx, wy, ww, wh, radius);
	cairo_clip(cr);
	if (look->gradient) {
		cairo_pattern_t *pattern = look->sideways ?
			gradient_pattern(look->gradient, wx, 0, wx + ww, 0) :
			gradient_pattern(look->gradient, 0, wy, 0, wy + tit);
		cairo_set_source(cr, pattern);
		cairo_rectangle(cr, wx, wy, ww, tit);
		cairo_fill(cr);
		cairo_pattern_destroy(pattern);
	} else {
		source(cr, look->title);
		cairo_rectangle(cr, wx, wy, ww, tit);
		cairo_fill(cr);
	}
	cairo_restore(cr);
	source(cr, look->title_fg);
	cairo_rectangle(cr, wx + 4, wy + tit * 0.4, ww * 0.34, tit * 0.22);
	cairo_fill(cr);
	for (int i = 0; i < 3; i++) {
		cairo_rectangle(cr, wx + ww - (i + 1) * tit * 0.82, wy + tit * 0.28, tit * 0.48, tit * 0.44);
		cairo_fill(cr);
	}
	// the taskbar and its start button
	source(cr, look->bar);
	cairo_rectangle(cr, 0, H - bar_h, W, bar_h);
	cairo_fill(cr);
	if (look->has_start) {
		source(cr, look->start);
		cairo_rectangle(cr, 3, H - bar_h + 2, bar_h * 1.4, bar_h - 4);
		cairo_fill(cr);
	}
}

GtkWidget *theme_card_new(const char *name, const char *title) {
	char *error = NULL;
	struct tw_theme *theme = tw_theme_load(name, &error);
	free(error);

	GtkWidget *picture = gtk_picture_new();
	ui_wallpaper_picture_fill(picture, NULL, name, 240, 150);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
	gtk_widget_set_size_request(picture, 240, 150);
	gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(picture, "tw-thumb");

	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
	if (theme) {
		struct card_look *look = g_new0(struct card_look, 1);
		read_look(look, theme);
		GtkWidget *area = gtk_drawing_area_new();
		gtk_widget_set_can_target(area, FALSE);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_look, look, look_free);
		gtk_overlay_add_overlay(GTK_OVERLAY(overlay), area);
	}

	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(card), overlay);
	GtkWidget *label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "tw-heading");
	gtk_box_append(GTK_BOX(card), label);

	GtkWidget *child = gtk_flow_box_child_new();
	gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), card);
	gtk_widget_add_css_class(child, "tw-card");
	g_object_set_data_full(G_OBJECT(child), "theme", g_strdup(name), g_free);
	g_object_set_data_full(G_OBJECT(child), "title", g_strdup(title), g_free);
	if (theme) {
		tw_theme_free(theme);
	}
	return child;
}
