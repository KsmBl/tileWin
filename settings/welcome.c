#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "settings.h"
#include "tw_paths.h"
#include "tw_theme.h"

/*
 * The first window a new user sees: a theme, light or dark, how windows work,
 * the keys worth knowing, and a way to bring settings over from another
 * computer. Everything is applied at once and all of it can be skipped.
 */
#define STEPS 4

struct welcome {
	struct settings *s;
	GtkWidget *window, *stack, *back, *next, *where;
	int step;
};

static void show_step(struct welcome *w) {
	char name[16], text[32];
	snprintf(name, sizeof(name), "step%d", w->step);
	gtk_stack_set_visible_child_name(GTK_STACK(w->stack), name);
	snprintf(text, sizeof(text), "Step %d of %d", w->step + 1, STEPS);
	gtk_label_set_text(GTK_LABEL(w->where), text);
	gtk_widget_set_sensitive(w->back, w->step > 0);
	gtk_button_set_label(GTK_BUTTON(w->next), w->step == STEPS - 1 ? "Finish" : "Next");
}

static void on_next(GtkButton *button, gpointer data) {
	struct welcome *w = data;
	if (w->step == STEPS - 1) {
		gtk_window_close(GTK_WINDOW(w->window));
		return;
	}
	w->step++;
	show_step(w);
}

static void on_back(GtkButton *button, gpointer data) {
	struct welcome *w = data;
	if (w->step > 0) {
		w->step--;
		show_step(w);
	}
}

static void on_skip(GtkButton *button, gpointer data) {
	struct welcome *w = data;
	gtk_window_close(GTK_WINDOW(w->window));
}

/* With the settings still hidden nothing is left to keep the app running. */
static void on_closed(GtkWindow *window, gpointer data) {
	struct welcome *w = data;
	if (!gtk_widget_get_visible(GTK_WIDGET(w->s->window))) {
		g_application_quit(G_APPLICATION(w->s->app));
	}
	g_free(w);
}

static void on_theme(GtkFlowBox *flow, GtkFlowBoxChild *child, gpointer data) {
	struct welcome *w = data;
	const char *name = g_object_get_data(G_OBJECT(child), "theme");
	if (settings_command(w->s, "theme %s window", name)) {
		settings_apply_theme();
	}
	gtk_widget_grab_focus(w->next); // with a look picked, Enter goes on
}

static gboolean on_dark(GtkSwitch *widget, gboolean active, gpointer data) {
	struct welcome *w = data;
	settings_command(w->s, "color_scheme %s", active ? "dark" : "light");
	settings_apply_color_scheme(active);
	settings_apply_theme();
	return FALSE;
}

static void on_mode(GtkCheckButton *button, gpointer data) {
	struct welcome *w = data;
	if (gtk_check_button_get_active(button)) {
		settings_command(w->s, "wm_mode %s",
			(const char *)g_object_get_data(G_OBJECT(button), "mode"));
	}
}

static void open_settings(struct welcome *w, const char *page) {
	gtk_stack_set_visible_child_name(w->s->stack, page);
	gtk_window_present(w->s->window);
	gtk_window_close(GTK_WINDOW(w->window));
}

static void on_backup(GtkButton *button, gpointer data) {
	open_settings(data, "backup");
}

static void on_settings(GtkButton *button, gpointer data) {
	open_settings(data, "theme");
}

static GtkWidget *label_new(const char *text, const char *css) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	if (css) {
		gtk_widget_add_css_class(label, css);
	}
	return label;
}

static GtkWidget *step_new(struct welcome *w, int step, const char *title, const char *about) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_widget_set_margin_top(box, 20);
	gtk_box_append(GTK_BOX(box), label_new(title, "tw-title"));
	gtk_box_append(GTK_BOX(box), label_new(about, "dim-label"));
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	char name[16];
	snprintf(name, sizeof(name), "step%d", step);
	gtk_stack_add_named(GTK_STACK(w->stack), scroll, name);
	return box;
}

static void build_look(struct welcome *w) {
	GtkWidget *box = step_new(w, 0, "Welcome to tileWin",
		"Pick the look you like. It changes at once, and the Theme page of the settings can "
		"change it again at any time.");
	GtkWidget *flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_SINGLE);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(flow), TRUE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
	list_t *names = tw_theme_list();
	char *current = tw_ipc_state("theme_window");
	for (int i = 0; names && i < names->length; i++) {
		const char *name = names->items[i];
		char *error = NULL;
		struct tw_theme *theme = tw_theme_load(name, &error);
		free(error);
		GtkWidget *child = theme_card_new(name, theme && theme->title ? theme->title : name);
		gtk_flow_box_append(GTK_FLOW_BOX(flow), child);
		if (current && strcmp(current, name) == 0) {
			gtk_flow_box_select_child(GTK_FLOW_BOX(flow), GTK_FLOW_BOX_CHILD(child));
		}
		if (theme) {
			tw_theme_free(theme);
		}
	}
	g_free(current);
	if (names) {
		list_free_items_and_destroy(names);
	}
	g_signal_connect(flow, "child-activated", G_CALLBACK(on_theme), w);
	gtk_box_append(GTK_BOX(box), flow);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(row), label_new("Dark mode", NULL));
	GtkWidget *dark = gtk_switch_new();
	gtk_switch_set_active(GTK_SWITCH(dark), tw_color_scheme_is_dark());
	g_signal_connect(dark, "state-set", G_CALLBACK(on_dark), w);
	gtk_box_append(GTK_BOX(row), dark);
	gtk_box_append(GTK_BOX(box), row);
}

static void build_mode(struct welcome *w) {
	GtkWidget *box = step_new(w, 1, "How should windows work?",
		"You can switch between the two at any time with Super+Shift+W. Your windows stay open.");
	char *mode = settings_current_mode();
	GtkWidget *window_mode = gtk_check_button_new_with_label(
		"Window mode: windows float and have title bars, as on Windows");
	GtkWidget *tile_mode = gtk_check_button_new_with_label(
		"Tile mode: windows fill the screen side by side, driven by the keyboard, as in sway");
	gtk_check_button_set_group(GTK_CHECK_BUTTON(tile_mode), GTK_CHECK_BUTTON(window_mode));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(strcmp(mode, "tile") == 0 ?
		tile_mode : window_mode), TRUE);
	g_free(mode);
	g_object_set_data(G_OBJECT(window_mode), "mode", (gpointer)"window");
	g_object_set_data(G_OBJECT(tile_mode), "mode", (gpointer)"tile");
	g_signal_connect(window_mode, "toggled", G_CALLBACK(on_mode), w);
	g_signal_connect(tile_mode, "toggled", G_CALLBACK(on_mode), w);
	gtk_box_append(GTK_BOX(box), window_mode);
	gtk_box_append(GTK_BOX(box), tile_mode);
}

static void build_keys(struct welcome *w) {
	GtkWidget *box = step_new(w, 2, "The keys worth knowing",
		"Super is the key with the Windows logo. These work in window mode.");
	static const char *const keys[][2] = {
		{ "Super", "The start menu; just start typing to search apps, settings and files" },
		{ "Super+Tab", "Task view: all windows and desktops" },
		{ "Alt+Tab", "Switch windows" },
		{ "Super+← / →", "Snap a window to a half of the screen" },
		{ "Super+I", "The settings" },
		{ "Super+/", "Every keyboard shortcut of the mode you are in" },
		{ "Super+Shift+W", "Switch between window and tile mode" },
	};
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
	for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
		gtk_grid_attach(GTK_GRID(grid), label_new(keys[i][0], "tw-heading"), 0, (int)i, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), label_new(keys[i][1], NULL), 1, (int)i, 1, 1);
	}
	gtk_box_append(GTK_BOX(box), grid);
}

static void build_done(struct welcome *w) {
	GtkWidget *box = step_new(w, 3, "That's it",
		"Coming from another computer? The Backup page brings all your tileWin settings over "
		"from a file. Everything else is in the settings, and all of it has a search.");
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *backup = gtk_button_new_with_label("Restore from a backup");
	g_signal_connect(backup, "clicked", G_CALLBACK(on_backup), w);
	gtk_box_append(GTK_BOX(row), backup);
	GtkWidget *settings = gtk_button_new_with_label("Open the settings");
	g_signal_connect(settings, "clicked", G_CALLBACK(on_settings), w);
	gtk_box_append(GTK_BOX(row), settings);
	gtk_box_append(GTK_BOX(box), row);
}

void welcome_show(struct settings *s) {
	struct welcome *w = g_new0(struct welcome, 1);
	w->s = s;
	w->window = gtk_application_window_new(s->app);
	gtk_window_set_title(GTK_WINDOW(w->window), "Welcome to tileWin");
	gtk_window_set_default_size(GTK_WINDOW(w->window), 860, 680);
	w->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(w->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_widget_set_vexpand(w->stack, TRUE);
	build_look(w);
	build_mode(w);
	build_keys(w);
	build_done(w);

	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(foot, 24);
	gtk_widget_set_margin_end(foot, 24);
	gtk_widget_set_margin_top(foot, 12);
	gtk_widget_set_margin_bottom(foot, 16);
	GtkWidget *skip = gtk_button_new_with_label("Skip");
	g_signal_connect(skip, "clicked", G_CALLBACK(on_skip), w);
	gtk_box_append(GTK_BOX(foot), skip);
	w->where = gtk_label_new("");
	gtk_widget_set_hexpand(w->where, TRUE);
	gtk_widget_add_css_class(w->where, "dim-label");
	gtk_box_append(GTK_BOX(foot), w->where);
	w->back = gtk_button_new_with_label("Back");
	g_signal_connect(w->back, "clicked", G_CALLBACK(on_back), w);
	gtk_box_append(GTK_BOX(foot), w->back);
	w->next = gtk_button_new_with_label("Next");
	gtk_widget_add_css_class(w->next, "suggested-action");
	g_signal_connect(w->next, "clicked", G_CALLBACK(on_next), w);
	gtk_box_append(GTK_BOX(foot), w->next);

	GtkWidget *all = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(all), w->stack);
	gtk_box_append(GTK_BOX(all), foot);
	gtk_window_set_child(GTK_WINDOW(w->window), all);
	gtk_window_set_default_widget(GTK_WINDOW(w->window), w->next);
	g_signal_connect(w->window, "destroy", G_CALLBACK(on_closed), w);
	show_step(w);
	ui_wallpaper_fills_start();
	gtk_window_present(GTK_WINDOW(w->window));
	gtk_widget_grab_focus(w->next);
}
