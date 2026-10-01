#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include "settings.h"
#include "tw_desktop.h"

/*
 * Choosing instead of typing. Every control here keeps the text entry the
 * page had (and saves from) but hides it: a dropdown of presets fills it in,
 * a color, font or folder button does, and only "Custom…" shows the entry for
 * a value of one's own. A value the file already has that is none of the
 * presets shows as Custom with the entry, so nothing is lost.
 */

#define CUSTOM_LABEL "Custom…"
#define SEARCH_FROM 12 // longer lists can be searched

struct presets {
	GtkWidget *dropdown, *entry;
	GPtrArray *values; // char *, one per preset; the last row of the dropdown is Custom
	bool syncing;
};

static void presets_free(gpointer data) {
	struct presets *pr = data;
	g_ptr_array_unref(pr->values);
	g_free(pr);
}

/* Shows the preset the entry holds, or Custom and the entry. */
static void presets_sync(struct presets *pr) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(pr->entry));
	guint found = pr->values->len;
	for (guint i = 0; i < pr->values->len; i++) {
		if (strcmp(text, pr->values->pdata[i]) == 0) {
			found = i;
			break;
		}
	}
	pr->syncing = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(pr->dropdown), found);
	pr->syncing = false;
	gtk_widget_set_visible(pr->entry, found == pr->values->len);
}

static void on_presets_entry(GtkEditable *editable, gpointer data) {
	struct presets *pr = data;
	if (!pr->syncing) {
		presets_sync(pr);
	}
}

static void on_presets_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct presets *pr = data;
	if (pr->syncing) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (i == GTK_INVALID_LIST_POSITION) {
		return;
	}
	if (i < pr->values->len) {
		pr->syncing = true;
		gtk_editable_set_text(GTK_EDITABLE(pr->entry), pr->values->pdata[i]);
		pr->syncing = false;
		gtk_widget_set_visible(pr->entry, false);
	} else {
		gtk_widget_set_visible(pr->entry, true);
		gtk_widget_grab_focus(pr->entry);
	}
}

GtkWidget *ui_presets(GtkWidget *entry, GPtrArray *values, GPtrArray *labels) {
	struct presets *pr = g_new0(struct presets, 1);
	pr->entry = entry;
	pr->values = values;
	GtkStringList *model = gtk_string_list_new(NULL);
	for (guint i = 0; i < labels->len; i++) {
		gtk_string_list_append(model, labels->pdata[i]);
	}
	gtk_string_list_append(model, CUSTOM_LABEL);
	g_ptr_array_unref(labels);
	pr->dropdown = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
	if (values->len >= SEARCH_FROM) {
		gtk_drop_down_set_expression(GTK_DROP_DOWN(pr->dropdown),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
		gtk_drop_down_set_enable_search(GTK_DROP_DOWN(pr->dropdown), TRUE);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign(box, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(box), pr->dropdown);
	if (gtk_widget_get_parent(entry)) {
		g_object_ref(entry);
		gtk_widget_unparent(entry);
		gtk_box_append(GTK_BOX(box), entry);
		g_object_unref(entry);
	} else {
		gtk_box_append(GTK_BOX(box), entry);
	}
	g_object_set_data_full(G_OBJECT(box), "presets", pr, presets_free);
	g_signal_connect(entry, "changed", G_CALLBACK(on_presets_entry), pr);
	g_signal_connect(pr->dropdown, "notify::selected", G_CALLBACK(on_presets_selected), pr);
	presets_sync(pr);
	return box;
}

GtkWidget *ui_presets_static(GtkWidget *entry, const char *const *values,
		const char *const *labels) {
	GPtrArray *v = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *l = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; values[i]; i++) {
		g_ptr_array_add(v, g_strdup(values[i]));
		g_ptr_array_add(l, g_strdup(labels[i]));
	}
	return ui_presets(entry, v, l);
}

/* ---------- numbers ---------- */

struct spin_choice {
	GtkWidget *dropdown, *spin;
	GArray *numbers; // int, one per row; the last row is Custom when custom
	bool custom, syncing;
};

static void spin_choice_free(gpointer data) {
	struct spin_choice *sc = data;
	g_array_unref(sc->numbers);
	g_free(sc);
}

/* Shows the number the spin button holds, or Custom and the spin button. */
static void spin_choice_sync(struct spin_choice *sc) {
	int value = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sc->spin));
	guint found = sc->numbers->len;
	for (guint i = 0; i < sc->numbers->len; i++) {
		if (g_array_index(sc->numbers, int, i) == value) {
			found = i;
			break;
		}
	}
	if (found == sc->numbers->len && !sc->custom) {
		found = 0; // cannot happen with every number of the range listed
	}
	sc->syncing = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(sc->dropdown), found);
	sc->syncing = false;
	gtk_widget_set_visible(sc->spin, found == sc->numbers->len);
}

static void on_spin_choice_value(GtkSpinButton *spin, gpointer data) {
	struct spin_choice *sc = data;
	if (!sc->syncing) {
		spin_choice_sync(sc);
	}
}

static void on_spin_choice_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct spin_choice *sc = data;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (sc->syncing || i == GTK_INVALID_LIST_POSITION) {
		return;
	}
	if (i < sc->numbers->len) {
		sc->syncing = true;
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(sc->spin), g_array_index(sc->numbers, int, i));
		sc->syncing = false;
		gtk_widget_set_visible(sc->spin, false);
	} else {
		gtk_widget_set_visible(sc->spin, true);
		gtk_widget_grab_focus(sc->spin);
	}
}

/* Round numbers for a range too wide to list all of. */
static const int round_numbers[] = {
	1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 25, 30, 32, 40, 48, 50, 60, 64, 75, 80, 96,
	100, 120, 128, 150, 200, 250, 256, 300, 400, 500, 600, 750, 800, 1000, 1500, 2000, 2500,
	3000, 4000, 5000, 7500, 10000, 20000, 50000, 100000,
};

GtkWidget *ui_spin_choice(GtkWidget *spin, const int *numbers, const char *format,
		const char *zero_label) {
	struct spin_choice *sc = g_new0(struct spin_choice, 1);
	sc->spin = spin;
	sc->numbers = g_array_new(FALSE, FALSE, sizeof(int));
	double low, high, step, page;
	gtk_spin_button_get_range(GTK_SPIN_BUTTON(spin), &low, &high);
	gtk_spin_button_get_increments(GTK_SPIN_BUTTON(spin), &step, &page);
	int istep = step >= 1 ? (int)step : 1;
	if (numbers) {
		for (int i = 0; numbers[i] >= 0; i++) {
			g_array_append_val(sc->numbers, numbers[i]);
		}
		sc->custom = true;
	} else if ((high - low) / istep <= 60) {
		for (int n = (int)low; n <= (int)high; n += istep) {
			g_array_append_val(sc->numbers, n);
		}
	} else {
		if (low <= 0) {
			int zero = 0;
			g_array_append_val(sc->numbers, zero);
		}
		for (size_t i = 0; i < G_N_ELEMENTS(round_numbers); i++) {
			int n = round_numbers[i];
			if (n >= low && n <= high && (n - (int)low) % istep == 0) {
				g_array_append_val(sc->numbers, n);
			}
		}
		sc->custom = true;
	}
	GtkStringList *model = gtk_string_list_new(NULL);
	for (guint i = 0; i < sc->numbers->len; i++) {
		int n = g_array_index(sc->numbers, int, i);
		if (n == 0 && zero_label) {
			gtk_string_list_append(model, zero_label);
		} else {
			char *label = g_strdup_printf(format ? format : "%d", n);
			gtk_string_list_append(model, label);
			g_free(label);
		}
	}
	if (sc->custom) {
		gtk_string_list_append(model, CUSTOM_LABEL);
	}
	sc->dropdown = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
	if (sc->numbers->len >= SEARCH_FROM * 2) {
		gtk_drop_down_set_expression(GTK_DROP_DOWN(sc->dropdown),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
		gtk_drop_down_set_enable_search(GTK_DROP_DOWN(sc->dropdown), TRUE);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign(box, GTK_ALIGN_END);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), sc->dropdown);
	gtk_box_append(GTK_BOX(box), spin);
	g_object_set_data_full(G_OBJECT(box), "spin-choice", sc, spin_choice_free);
	// after the page's own handler, so the value the page set is shown
	g_signal_connect_after(spin, "value-changed", G_CALLBACK(on_spin_choice_value), sc);
	g_signal_connect(sc->dropdown, "notify::selected", G_CALLBACK(on_spin_choice_selected), sc);
	spin_choice_sync(sc);
	return box;
}

/* ---------- colors ---------- */

struct color_field {
	GtkWidget *button, *entry;
	bool syncing;
};

static void color_sync(struct color_field *cf) {
	GdkRGBA rgba;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(cf->entry));
	cf->syncing = true;
	if (*text && gdk_rgba_parse(&rgba, text)) {
		gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(cf->button), &rgba);
		gtk_widget_set_opacity(cf->button, 1);
	} else {
		gtk_widget_set_opacity(cf->button, 0.5); // the theme's: dimmed
	}
	cf->syncing = false;
}

static void on_color_entry(GtkEditable *editable, gpointer data) {
	struct color_field *cf = data;
	if (!cf->syncing) {
		color_sync(cf);
	}
}

static void on_color_chosen(GObject *button, GParamSpec *pspec, gpointer data) {
	struct color_field *cf = data;
	if (cf->syncing) {
		return;
	}
	const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
	char text[16];
	int r = (int)(rgba->red * 255 + 0.5), g = (int)(rgba->green * 255 + 0.5),
		b = (int)(rgba->blue * 255 + 0.5), a = (int)(rgba->alpha * 255 + 0.5);
	if (a >= 255) {
		snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
	} else {
		snprintf(text, sizeof(text), "#%02x%02x%02x%02x", r, g, b, a);
	}
	cf->syncing = true;
	gtk_editable_set_text(GTK_EDITABLE(cf->entry), text);
	cf->syncing = false;
	gtk_widget_set_opacity(cf->button, 1);
}

static void on_color_default(GtkButton *button, gpointer data) {
	struct color_field *cf = data;
	gtk_editable_set_text(GTK_EDITABLE(cf->entry), "");
}

GtkWidget *ui_color_field(GtkWidget *entry) {
	struct color_field *cf = g_new0(struct color_field, 1);
	cf->entry = entry;
	GtkColorDialog *dialog = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(dialog, TRUE);
	cf->button = gtk_color_dialog_button_new(dialog);
	GtkWidget *reset = gtk_button_new_with_label("Default");
	gtk_widget_set_tooltip_text(reset, "The color the theme gives");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign(box, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(box), cf->button);
	gtk_box_append(GTK_BOX(box), reset);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_widget_set_visible(entry, false);
	g_object_set_data_full(G_OBJECT(box), "color", cf, g_free);
	g_signal_connect(entry, "changed", G_CALLBACK(on_color_entry), cf);
	g_signal_connect(cf->button, "notify::rgba", G_CALLBACK(on_color_chosen), cf);
	g_signal_connect(reset, "clicked", G_CALLBACK(on_color_default), cf);
	color_sync(cf);
	return box;
}

/* ---------- fonts ---------- */

struct font_field {
	GtkWidget *button, *entry;
	bool syncing;
};

static void font_sync(struct font_field *ff) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(ff->entry));
	PangoFontDescription *desc = pango_font_description_from_string(*text ? text :
		"Sans 10");
	ff->syncing = true;
	gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(ff->button), desc);
	ff->syncing = false;
	gtk_widget_set_opacity(ff->button, *text ? 1 : 0.5);
	pango_font_description_free(desc);
}

static void on_font_entry(GtkEditable *editable, gpointer data) {
	struct font_field *ff = data;
	if (!ff->syncing) {
		font_sync(ff);
	}
}

static void on_font_chosen(GObject *button, GParamSpec *pspec, gpointer data) {
	struct font_field *ff = data;
	if (ff->syncing) {
		return;
	}
	PangoFontDescription *desc =
		gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(button));
	char *text = desc ? pango_font_description_to_string(desc) : g_strdup("");
	ff->syncing = true;
	gtk_editable_set_text(GTK_EDITABLE(ff->entry), text);
	ff->syncing = false;
	gtk_widget_set_opacity(ff->button, 1);
	g_free(text);
}

static void on_font_default(GtkButton *button, gpointer data) {
	struct font_field *ff = data;
	gtk_editable_set_text(GTK_EDITABLE(ff->entry), "");
}

GtkWidget *ui_font_field(GtkWidget *entry) {
	struct font_field *ff = g_new0(struct font_field, 1);
	ff->entry = entry;
	ff->button = gtk_font_dialog_button_new(gtk_font_dialog_new());
	GtkWidget *reset = gtk_button_new_with_label("Default");
	gtk_widget_set_tooltip_text(reset, "The font the theme gives");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign(box, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(box), ff->button);
	gtk_box_append(GTK_BOX(box), reset);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_widget_set_visible(entry, false);
	g_object_set_data_full(G_OBJECT(box), "font", ff, g_free);
	g_signal_connect(entry, "changed", G_CALLBACK(on_font_entry), ff);
	g_signal_connect(ff->button, "notify::font-desc", G_CALLBACK(on_font_chosen), ff);
	g_signal_connect(reset, "clicked", G_CALLBACK(on_font_default), ff);
	font_sync(ff);
	return box;
}

/* ---------- folders ---------- */

struct folder_field {
	GtkWidget *button, *label, *entry;
	char *title;
};

static void folder_field_free(gpointer data) {
	struct folder_field *ff = data;
	g_free(ff->title);
	g_free(ff);
}

static void folder_sync(struct folder_field *ff) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(ff->entry));
	char *name = *text ? g_path_get_basename(text) : g_strdup("Choose a folder…");
	gtk_label_set_text(GTK_LABEL(ff->label), name);
	gtk_widget_set_tooltip_text(ff->button, *text ? text : NULL);
	g_free(name);
}

static void on_folder_entry(GtkEditable *editable, gpointer data) {
	folder_sync(data);
}

static void folder_picked(GObject *source, GAsyncResult *result, gpointer data) {
	struct folder_field *ff = data;
	GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (file) {
		char *path = g_file_get_path(file);
		if (path) {
			gtk_editable_set_text(GTK_EDITABLE(ff->entry), path);
		}
		g_free(path);
		g_object_unref(file);
	}
}

static void on_folder_clicked(GtkButton *button, gpointer data) {
	struct folder_field *ff = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, ff->title);
	const char *text = gtk_editable_get_text(GTK_EDITABLE(ff->entry));
	if (*text) {
		GFile *current = g_file_new_for_path(text);
		gtk_file_dialog_set_initial_folder(dialog, current);
		g_object_unref(current);
	}
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
	gtk_file_dialog_select_folder(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
		NULL, folder_picked, ff);
	g_object_unref(dialog);
}

GtkWidget *ui_folder_field(GtkWidget *entry, const char *title) {
	struct folder_field *ff = g_new0(struct folder_field, 1);
	ff->entry = entry;
	ff->title = g_strdup(title);
	ff->label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(ff->label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(ff->label), 28);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(inner), gtk_image_new_from_icon_name("folder-symbolic"));
	gtk_box_append(GTK_BOX(inner), ff->label);
	ff->button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(ff->button), inner);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign(box, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(box), ff->button);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_widget_set_visible(entry, false);
	g_object_set_data_full(G_OBJECT(box), "folder", ff, folder_field_free);
	g_signal_connect(entry, "changed", G_CALLBACK(on_folder_entry), ff);
	g_signal_connect(ff->button, "clicked", G_CALLBACK(on_folder_clicked), ff);
	folder_sync(ff);
	return box;
}

/* ---------- lists of what there is ---------- */

static int entry_name_cmp(gconstpointer a, gconstpointer b) {
	const struct tw_desktop_entry *x = *(struct tw_desktop_entry *const *)a;
	const struct tw_desktop_entry *y = *(struct tw_desktop_entry *const *)b;
	return g_utf8_collate(x->name, y->name);
}

static bool has_category(const struct tw_desktop_entry *e, const char *category) {
	if (!category) {
		return true;
	}
	if (!e->categories) {
		return false;
	}
	char **cats = g_strsplit(e->categories, ";", -1);
	bool found = false;
	char **wanted = g_strsplit(category, ";", -1);
	for (int i = 0; cats[i] && !found; i++) {
		for (int j = 0; wanted[j] && !found; j++) {
			found = *wanted[j] && strcmp(cats[i], wanted[j]) == 0;
		}
	}
	g_strfreev(cats);
	g_strfreev(wanted);
	return found;
}

void ui_list_apps(const char *categories, bool exec_prefix, GPtrArray *values,
		GPtrArray *labels) {
	list_t *all = tw_desktop_scan();
	GPtrArray *found = g_ptr_array_new();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (!e->no_display && !e->hidden && e->exec && !e->link && has_category(e, categories)) {
			g_ptr_array_add(found, e);
		}
	}
	g_ptr_array_sort(found, entry_name_cmp);
	for (guint i = 0; i < found->len; i++) {
		struct tw_desktop_entry *e = found->pdata[i];
		char *command = tw_desktop_exec_command(e);
		if (!command) {
			continue;
		}
		// a terminal app (btop) runs in the terminal of the config
		char *value = e->terminal ? g_strdup_printf("%s$term -e %s", exec_prefix ? "exec " : "",
			command) : g_strdup_printf("%s%s", exec_prefix ? "exec " : "", command);
		g_ptr_array_add(values, value);
		g_ptr_array_add(labels, g_strdup(e->name));
		free(command);
	}
	g_ptr_array_unref(found);
	tw_desktop_list_free(all);
}

void ui_list_app_ids(GPtrArray *ids, GPtrArray *names) {
	list_t *all = tw_desktop_scan();
	GPtrArray *found = g_ptr_array_new();
	for (int i = 0; i < all->length; i++) {
		struct tw_desktop_entry *e = all->items[i];
		if (!e->no_display && !e->hidden && e->exec && !e->link) {
			g_ptr_array_add(found, e);
		}
	}
	g_ptr_array_sort(found, entry_name_cmp);
	for (guint i = 0; i < found->len; i++) {
		struct tw_desktop_entry *e = found->pdata[i];
		// the app id is mostly the id of the entry without .desktop
		size_t len = strlen(e->id);
		char *id = len > 8 && strcmp(e->id + len - 8, ".desktop") == 0 ?
			g_strndup(e->id, len - 8) : g_strdup(e->id);
		g_ptr_array_add(ids, e->startup_wm_class ? g_strdup(e->startup_wm_class) : id);
		if (e->startup_wm_class) {
			g_free(id);
		}
		g_ptr_array_add(names, g_strdup(e->name));
	}
	g_ptr_array_unref(found);
	tw_desktop_list_free(all);
}

/* The screen lockers there are: tileWin's own, and the installed ones of the others. */
static const struct {
	const char *command, *program, *label;
} lockers[] = {
	{ "tilewin-lock -f", NULL, "tileWin's lock screen" },
	{ "swaylock -f", "swaylock", "swaylock" },
	{ "gtklock", "gtklock", "gtklock" },
	{ "hyprlock", "hyprlock", "hyprlock" },
	{ "waylock", "waylock", "waylock" },
};

/* The screenshot tools: tileWin's own, and the installed ones of the others. */
static const struct {
	const char *command, *program, *label;
} shooters[] = {
	{ "tilewin-snip screen", NULL, "All screens (tileWin)" },
	{ "tilewin-snip area", NULL, "A rectangle (tileWin)" },
	{ "flameshot gui", "flameshot", "Flameshot" },
	{ "grimshot copy area", "grimshot", "grimshot" },
	{ "spectacle", "spectacle", "Spectacle" },
};

static void add_installed(GPtrArray *values, GPtrArray *labels, const char *command,
		const char *program, const char *label) {
	char *path = program ? g_find_program_in_path(program) : NULL;
	if (!program || path) {
		g_ptr_array_add(values, g_strdup(command));
		g_ptr_array_add(labels, g_strdup(label));
	}
	g_free(path);
}

GtkWidget *ui_presets_lockers(GtkWidget *entry) {
	GPtrArray *values = ui_strings(), *labels = ui_strings();
	for (size_t i = 0; i < G_N_ELEMENTS(lockers); i++) {
		add_installed(values, labels, lockers[i].command, lockers[i].program, lockers[i].label);
	}
	return ui_presets(entry, values, labels);
}

GtkWidget *ui_presets_screenshots(GtkWidget *entry) {
	GPtrArray *values = ui_strings(), *labels = ui_strings();
	for (size_t i = 0; i < G_N_ELEMENTS(shooters); i++) {
		add_installed(values, labels, shooters[i].command, shooters[i].program,
			shooters[i].label);
	}
	return ui_presets(entry, values, labels);
}

/* Terminals with the flag that makes them run a command, for console apps. */
static const struct {
	const char *command, *program, *label;
} terminals[] = {
	{ "foot", "foot", "foot" },
	{ "kitty", "kitty", "kitty" },
	{ "alacritty -e", "alacritty", "Alacritty" },
	{ "wezterm start --", "wezterm", "WezTerm" },
	{ "xfce4-terminal -x", "xfce4-terminal", "Xfce Terminal" },
	{ "gnome-terminal --", "gnome-terminal", "GNOME Terminal" },
	{ "kgx --", "kgx", "Console (GNOME)" },
	{ "konsole -e", "konsole", "Konsole" },
	{ "xterm -e", "xterm", "XTerm" },
};

GtkWidget *ui_presets_terminals(GtkWidget *entry) {
	GPtrArray *values = ui_strings(), *labels = ui_strings();
	g_ptr_array_add(values, g_strdup(""));
	g_ptr_array_add(labels, g_strdup("Default (xfce4-terminal -x)"));
	for (size_t i = 0; i < G_N_ELEMENTS(terminals); i++) {
		add_installed(values, labels, terminals[i].command, terminals[i].program,
			terminals[i].label);
	}
	return ui_presets(entry, values, labels);
}

GtkWidget *ui_presets_apps(GtkWidget *entry, const char *categories) {
	GPtrArray *values = ui_strings(), *labels = ui_strings();
	ui_list_apps(categories, false, values, labels);
	return ui_presets(entry, values, labels);
}

/* Names in a /sys class directory that pass the filter. */
static void list_sys(const char *dir, bool (*keep)(const char *dir, const char *name),
		GPtrArray *values, GPtrArray *labels) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *entry;
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	while ((entry = readdir(d))) {
		if (entry->d_name[0] != '.' && (!keep || keep(dir, entry->d_name))) {
			g_ptr_array_add(names, g_strdup(entry->d_name));
		}
	}
	closedir(d);
	g_ptr_array_sort(names, (GCompareFunc)g_ascii_strcasecmp);
	for (guint i = 0; i < names->len; i++) {
		g_ptr_array_add(values, g_strdup(names->pdata[i]));
		g_ptr_array_add(labels, g_strdup(names->pdata[i]));
	}
	g_ptr_array_unref(names);
}

static char *sys_read(const char *dir, const char *name, const char *file) {
	char *path = g_build_filename(dir, name, file, NULL);
	char *text = NULL;
	g_file_get_contents(path, &text, NULL, NULL);
	g_free(path);
	if (text) {
		g_strstrip(text);
	}
	return text;
}

static bool keep_net(const char *dir, const char *name) {
	return strcmp(name, "lo") != 0;
}

static bool keep_battery(const char *dir, const char *name) {
	char *type = sys_read(dir, name, "type");
	bool battery = type && strcmp(type, "Battery") == 0;
	g_free(type);
	return battery;
}

static bool keep_card(const char *dir, const char *name) {
	// card0, not card0-HDMI-A-1
	return strncmp(name, "card", 4) == 0 && strchr(name, '-') == NULL;
}

void ui_list_net_devices(GPtrArray *values, GPtrArray *labels) {
	list_sys("/sys/class/net", keep_net, values, labels);
}

void ui_list_batteries(GPtrArray *values, GPtrArray *labels) {
	list_sys("/sys/class/power_supply", keep_battery, values, labels);
}

void ui_list_gpus(GPtrArray *values, GPtrArray *labels) {
	list_sys("/sys/class/drm", keep_card, values, labels);
}

/* "Default" first, then the given values, each shown as format % value. */
void ui_list_numbers(GPtrArray *values, GPtrArray *labels, const char *label_format,
		const int *numbers) {
	for (int i = 0; numbers[i] >= 0; i++) {
		g_ptr_array_add(values, g_strdup_printf("%d", numbers[i]));
		g_ptr_array_add(labels, g_strdup_printf(label_format, numbers[i]));
	}
}

GPtrArray *ui_strings(void) {
	return g_ptr_array_new_with_free_func(g_free);
}
