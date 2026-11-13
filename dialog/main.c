/*
 * tilewin-dialog: a message box in the look of the theme that is on, for the
 * GTK 3 module of tileWin (gtk3module.c), which shows it in place of GTK's own
 * message dialogs ("Save changes?" with Save, Don't Save and Cancel, a
 * question, a warning, an error).
 *
 * It reads what to show from its standard input, as a key file:
 *
 *   [dialog]
 *   title=Mousepad                  the title of the window
 *   text=Save changes to "a.txt"?   the main text, and below it
 *   secondary=Unsaved changes ...   the text that explains it (may be empty)
 *   markup=false                    whether text is Pango markup
 *   secondary_markup=false          whether secondary is
 *   type=question                   info, warning, question, error or other: the icon
 *   labels=Do_n't Save;_Cancel;_Save;   the buttons, with _ before the access key
 *   responses=-9;-6;-3;             what each answers (GtkResponseType or the app's own)
 *   default=-3                      the button Return presses (optional)
 *
 * and prints the response of the button pressed, or -4 (GTK_RESPONSE_DELETE_EVENT)
 * when the window was closed or Escape pressed, as GTK's dialog does.
 *
 * --parent wayland:<handle> makes it the dialog of that window (xdg-foreign), so
 * tileWin floats it centred over the app. The window frame is tileWin's, the
 * inside the style sheet the settings use for the theme (themecss.c).
 *
 * The buttons are in the order of Windows: the one Return presses first, Cancel
 * and the like last, on the right from Windows 7 on and centred before it.
 */
#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_theme.h"

#define DIALOG_APP_ID "org.tilewin.Dialog"

struct button {
	char *label;
	int response;
};

static struct {
	char *title, *text, *secondary, *type, *parent;
	bool markup, secondary_markup;
	struct button *buttons;
	size_t count;
	bool has_default;
	int default_response;
	bool answered;
	GtkWidget *window;
	GMainLoop *loop;
} dlg;

/* themecss.c asks for these two of the settings app */
char *settings_current_theme(void) {
	char *name = tw_ipc_state("theme");
	if (name && *name) {
		return name;
	}
	g_free(name);
	char *saved = tw_theme_current_name();
	name = g_strdup(saved ? saved : TW_DEFAULT_THEME);
	free(saved);
	return name;
}

void settings_apply_color_scheme(bool dark) {
	GtkSettings *gtk = gtk_settings_get_default();
	if (!gtk) {
		return;
	}
#if GTK_CHECK_VERSION(4, 20, 0)
	g_object_set(gtk, "gtk-interface-color-scheme",
		dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#else
	g_object_set(gtk, "gtk-application-prefer-dark-theme", dark, NULL);
#endif
}

static bool glossy; // XP to 7: the icons with a shine, flat before and after

/* Newer Windows put the buttons on the right of a band below the text; XP and
 * 7 have glossy icons. */
static bool buttons_on_the_right(void) {
	char *name = settings_current_theme();
	char *error = NULL;
	struct tw_theme *t = name ? tw_theme_load(name, &error) : NULL;
	free(error);
	g_free(name);
	const char *style = t && t->style ? t->style : "win10";
	static const char *const centred[] = { "win1", "win3", "win31", "win95", "classic",
		"winxp", "luna" };
	bool right = true;
	for (size_t i = 0; i < G_N_ELEMENTS(centred); i++) {
		right = right && g_ascii_strcasecmp(style, centred[i]) != 0;
	}
	glossy = g_ascii_strcasecmp(style, "winxp") == 0 || g_ascii_strcasecmp(style, "luna") == 0 ||
		g_ascii_strcasecmp(style, "win7") == 0 || g_ascii_strcasecmp(style, "aero") == 0;
	tw_theme_free(t);
	return right;
}

/* What the dialog asks of tileWin's look beyond the settings' style sheet. */
static const char dialog_css[] =
	".tw-dialog-body { padding: 18px 18px 14px 14px; }\n"
	".tw-dialog-main { font-weight: bold; }\n"
	".tw-dialog-buttons { padding: 10px 12px; }\n"
	".tw-dialog-buttons.tw-band { background-color: alpha(currentColor, 0.06); "
	"border-top: 1px solid alpha(currentColor, 0.12); }\n"
	".tw-dialog-buttons button { min-width: 75px; }\n";

static void answer(int response) {
	if (dlg.answered) {
		return;
	}
	dlg.answered = true;
	printf("%d\n", response);
	fflush(stdout);
	g_main_loop_quit(dlg.loop);
}

static void on_button(GtkButton *button, gpointer data) {
	answer(GPOINTER_TO_INT(data));
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
	answer(GTK_RESPONSE_DELETE_EVENT);
	return FALSE;
}

static gboolean on_key(GtkEventControllerKey *key, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	if (keyval == GDK_KEY_Escape) {
		answer(GTK_RESPONSE_DELETE_EVENT);
		return TRUE;
	}
	return FALSE;
}

enum icon { ICON_NONE, ICON_INFO, ICON_WARNING, ICON_QUESTION, ICON_ERROR };

static enum icon icon_for(const char *type) {
	if (!type) {
		return ICON_NONE;
	}
	if (strcmp(type, "warning") == 0) {
		return ICON_WARNING;
	} else if (strcmp(type, "question") == 0) {
		return ICON_QUESTION;
	} else if (strcmp(type, "error") == 0) {
		return ICON_ERROR;
	} else if (strcmp(type, "info") == 0) {
		return ICON_INFO;
	}
	return ICON_NONE;
}

static void fill_shape(cairo_t *cr, double r, double g, double b, double size) {
	if (glossy) {
		cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, size);
		cairo_pattern_add_color_stop_rgb(p, 0, r + (1 - r) * 0.45, g + (1 - g) * 0.45,
			b + (1 - b) * 0.45);
		cairo_pattern_add_color_stop_rgb(p, 1, r * 0.8, g * 0.8, b * 0.8);
		cairo_set_source(cr, p);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(p);
	} else {
		cairo_set_source_rgb(cr, r, g, b);
		cairo_fill_preserve(cr);
	}
	cairo_set_source_rgba(cr, 0, 0, 0, glossy ? 0.45 : 0.6);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void mark(cairo_t *cr, const char *text, double cx, double cy, double size,
		double r, double g, double b) {
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, size);
	cairo_text_extents_t e;
	cairo_text_extents(cr, text, &e);
	cairo_move_to(cr, cx - e.width / 2 - e.x_bearing, cy - e.height / 2 - e.y_bearing);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_show_text(cr, text);
}

/* The icons of Windows' message boxes: a red circle with a cross for an error,
 * a yellow triangle with ! for a warning, blue circles with i and ?. */
static void draw_icon(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
	enum icon icon = GPOINTER_TO_INT(data);
	double s = w < h ? w : h, c = s / 2;
	switch (icon) {
	case ICON_ERROR:
		cairo_arc(cr, c, c, c - 1, 0, 2 * G_PI);
		fill_shape(cr, 0.86, 0.1, 0.1, s);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, s * 0.11);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_move_to(cr, s * 0.33, s * 0.33);
		cairo_line_to(cr, s * 0.67, s * 0.67);
		cairo_move_to(cr, s * 0.67, s * 0.33);
		cairo_line_to(cr, s * 0.33, s * 0.67);
		cairo_stroke(cr);
		break;
	case ICON_WARNING:
		cairo_move_to(cr, c, 1.5);
		cairo_line_to(cr, s - 1, s - 2.5);
		cairo_line_to(cr, 1, s - 2.5);
		cairo_close_path(cr);
		fill_shape(cr, 0.99, 0.82, 0.09, s);
		mark(cr, "!", c, s * 0.62, s * 0.62, 0, 0, 0);
		break;
	case ICON_INFO:
	case ICON_QUESTION:
		cairo_arc(cr, c, c, c - 1, 0, 2 * G_PI);
		fill_shape(cr, 0.12, 0.42, 0.82, s);
		mark(cr, icon == ICON_INFO ? "i" : "?", c, c, s * 0.66, 1, 1, 1);
		break;
	case ICON_NONE:
		break;
	}
}

static GtkWidget *text_label(const char *text, bool markup, bool main) {
	GtkWidget *label = gtk_label_new(NULL);
	if (markup) {
		gtk_label_set_markup(GTK_LABEL(label), text);
	} else {
		gtk_label_set_text(GTK_LABEL(label), text);
	}
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), FALSE);
	if (main) {
		gtk_widget_add_css_class(label, "tw-dialog-main");
	}
	return label;
}

/* Cancel, Close, No and the like go last, as on Windows. */
static int rank(const struct button *b) {
	if (dlg.has_default && b->response == dlg.default_response) {
		return 0;
	}
	switch (b->response) {
	case GTK_RESPONSE_CANCEL:
	case GTK_RESPONSE_CLOSE:
	case GTK_RESPONSE_DELETE_EVENT:
		return 2;
	default:
		return 1;
	}
}

static void build(void) {
	GtkWidget *window = gtk_window_new();
	dlg.window = window;
	gtk_window_set_title(GTK_WINDOW(window), dlg.title && *dlg.title ? dlg.title : "");
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_window_set_modal(GTK_WINDOW(window), TRUE);
	gtk_widget_add_css_class(window, "tw-dialog");
	g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
	gtk_widget_add_controller(window, keys);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(window), outer);

	bool right = buttons_on_the_right(); // and whether the icons shine
	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	gtk_widget_add_css_class(body, "tw-dialog-body");
	gtk_box_append(GTK_BOX(outer), body);
	enum icon icon = icon_for(dlg.type);
	if (icon != ICON_NONE) {
		GtkWidget *image = gtk_drawing_area_new();
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(image), 32);
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(image), 32);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(image), draw_icon,
			GINT_TO_POINTER(icon), NULL);
		gtk_widget_set_valign(image, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(body), image);
	}
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(texts, TRUE);
	gtk_box_append(GTK_BOX(body), texts);
	bool secondary = dlg.secondary && *dlg.secondary;
	if (dlg.text && *dlg.text) {
		gtk_box_append(GTK_BOX(texts), text_label(dlg.text, dlg.markup, secondary));
	}
	if (secondary) {
		gtk_box_append(GTK_BOX(texts),
			text_label(dlg.secondary, dlg.secondary_markup, false));
	}

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(row, "tw-dialog-buttons");
	if (right) {
		gtk_widget_add_css_class(row, "tw-band");
	}
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_hexpand(inner, TRUE);
	gtk_widget_set_halign(inner, right ? GTK_ALIGN_END : GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row), inner);
	gtk_box_append(GTK_BOX(outer), row);
	GtkWidget *focus = NULL;
	for (int pass = 0; pass < 3; pass++) {
		for (size_t i = 0; i < dlg.count; i++) {
			struct button *b = &dlg.buttons[i];
			if (rank(b) != pass) {
				continue;
			}
			GtkWidget *button = gtk_button_new_with_mnemonic(b->label);
			g_signal_connect(button, "clicked", G_CALLBACK(on_button),
				GINT_TO_POINTER(b->response));
			gtk_box_append(GTK_BOX(inner), button);
			if (dlg.has_default && b->response == dlg.default_response) {
				gtk_window_set_default_widget(GTK_WINDOW(window), button);
				gtk_widget_add_css_class(button, "suggested-action");
				focus = button;
			} else if (!focus) {
				focus = button;
			}
		}
	}
	if (focus) {
		gtk_window_set_focus(GTK_WINDOW(window), focus);
	}
}

static void set_parent(GtkWidget *window) {
	if (!dlg.parent || !g_str_has_prefix(dlg.parent, "wayland:")) {
		return;
	}
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (surface && GDK_IS_WAYLAND_TOPLEVEL(surface)) {
		gdk_wayland_toplevel_set_transient_for_exported(GDK_WAYLAND_TOPLEVEL(surface),
			dlg.parent + strlen("wayland:"));
	}
}

static void on_realize(GtkWidget *window, gpointer data) {
	set_parent(window);
}

static bool read_spec(void) {
	GString *in = g_string_new(NULL);
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
		g_string_append_len(in, chunk, n);
	}
	GKeyFile *kf = g_key_file_new();
	bool ok = g_key_file_load_from_data(kf, in->str, in->len, G_KEY_FILE_NONE, NULL);
	g_string_free(in, TRUE);
	if (!ok) {
		g_key_file_unref(kf);
		return false;
	}
	dlg.title = g_key_file_get_string(kf, "dialog", "title", NULL);
	dlg.text = g_key_file_get_string(kf, "dialog", "text", NULL);
	dlg.secondary = g_key_file_get_string(kf, "dialog", "secondary", NULL);
	dlg.type = g_key_file_get_string(kf, "dialog", "type", NULL);
	dlg.markup = g_key_file_get_boolean(kf, "dialog", "markup", NULL);
	dlg.secondary_markup = g_key_file_get_boolean(kf, "dialog", "secondary_markup", NULL);
	gsize labels = 0, responses = 0;
	char **label = g_key_file_get_string_list(kf, "dialog", "labels", &labels, NULL);
	gint *response = g_key_file_get_integer_list(kf, "dialog", "responses", &responses, NULL);
	GError *error = NULL;
	dlg.default_response = g_key_file_get_integer(kf, "dialog", "default", &error);
	dlg.has_default = error == NULL;
	g_clear_error(&error);
	g_key_file_unref(kf);
	if (!label || !response || labels != responses || labels == 0) {
		g_strfreev(label);
		g_free(response);
		return false;
	}
	dlg.count = labels;
	dlg.buttons = g_new0(struct button, labels);
	for (gsize i = 0; i < labels; i++) {
		dlg.buttons[i].label = g_strdup(label[i]);
		dlg.buttons[i].response = response[i];
	}
	g_strfreev(label);
	g_free(response);
	return true;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--parent") == 0 && i + 1 < argc) {
			dlg.parent = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("usage: tilewin-dialog [--parent wayland:<handle>] < dialog.ini\n"
				"Shows a message box in the look of the theme and prints the response "
				"of the button pressed.\n");
			return 0;
		}
	}
	if (!read_spec()) {
		fprintf(stderr, "tilewin-dialog: no dialog with buttons on standard input\n");
		return 2;
	}
	g_set_prgname(DIALOG_APP_ID);
	if (!gtk_init_check()) {
		fprintf(stderr, "tilewin-dialog: no display\n");
		return 3;
	}
	// the theme's look, as the settings have it, and a little of the dialog's own
	settings_apply_theme();
	GtkCssProvider *own = gtk_css_provider_new();
	gtk_css_provider_load_from_string(own, dialog_css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(own), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

	dlg.loop = g_main_loop_new(NULL, FALSE);
	build();
	g_signal_connect(dlg.window, "realize", G_CALLBACK(on_realize), NULL);
	gtk_window_present(GTK_WINDOW(dlg.window));
	g_main_loop_run(dlg.loop);
	return 0;
}
