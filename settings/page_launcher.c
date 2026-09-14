#include <string.h>
#include "settings.h"

static const struct {
	const char *title, *program, *command;
} launchers[] = {
	{ "Built-in launcher", NULL, "builtin" },
	{ "rofi", "rofi", "rofi -show drun" },
	{ "wofi", "wofi", "wofi --show drun" },
	{ "fuzzel", "fuzzel", "fuzzel" },
	{ "tofi", "tofi-drun", "tofi-drun --drun-launch=true" },
	{ "bemenu", "bemenu-run", "bemenu-run" },
};

#define CUSTOM_LAUNCHER G_N_ELEMENTS(launchers)

static const struct {
	const char *variable, *title, *hint;
} programs[] = {
	{ "$term", "Terminal", "Super+Return" },
	{ "$filemanager", "File manager", "Super+E in window mode" },
	{ "$taskmanager", "Task manager", "Ctrl+Shift+Esc in window mode" },
	{ "$locker", "Screen locker", "Super+L in window mode" },
	{ "$screenshot", "Screenshot", "Print" },
};

struct launcher_page {
	struct settings *s;
	bool updating;
	GtkWidget *radios[G_N_ELEMENTS(launchers) + 1];
	GtkWidget *custom_entry;
	GtkWidget *program_entries[G_N_ELEMENTS(programs)];
	guint custom_timer, programs_timer;
};

static void apply_launcher(struct launcher_page *p, const char *command) {
	struct confdoc *common = p->s->common;
	confdoc_set(common, common->root, "launcher_command", NULL, command);
	settings_common_changed(p->s, false);
	settings_command(p->s, "launcher_command %s", command);
}

static void on_radio_toggled(GtkCheckButton *button, gpointer data) {
	struct launcher_page *p = data;
	if (p->updating || !gtk_check_button_get_active(button)) {
		return;
	}
	guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "index"));
	if (i < CUSTOM_LAUNCHER) {
		apply_launcher(p, launchers[i].command);
		return;
	}
	const char *text = gtk_editable_get_text(GTK_EDITABLE(p->custom_entry));
	if (*text) {
		apply_launcher(p, text);
	} else {
		gtk_widget_grab_focus(p->custom_entry);
	}
}

static gboolean apply_custom(gpointer data) {
	struct launcher_page *p = data;
	p->custom_timer = 0;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(p->custom_entry));
	if (*text && gtk_check_button_get_active(GTK_CHECK_BUTTON(p->radios[CUSTOM_LAUNCHER]))) {
		apply_launcher(p, text);
	}
	return G_SOURCE_REMOVE;
}

static void on_custom_changed(GtkEditable *editable, gpointer data) {
	struct launcher_page *p = data;
	if (p->updating) {
		return;
	}
	GtkCheckButton *radio = GTK_CHECK_BUTTON(p->radios[CUSTOM_LAUNCHER]);
	if (!gtk_check_button_get_active(radio)) {
		p->updating = true;
		gtk_check_button_set_active(radio, TRUE);
		p->updating = false;
	}
	if (p->custom_timer) {
		g_source_remove(p->custom_timer);
	}
	p->custom_timer = g_timeout_add(700, apply_custom, p);
}

static void on_try(GtkButton *button, gpointer data) {
	struct launcher_page *p = data;
	if (!tw_ipc_available()) {
		settings_status(p->s, "tileWin is not running");
		return;
	}
	settings_command(p->s, "launcher");
}

static char *program_value(struct settings *s, const char *variable) {
	struct cstmt *st = confdoc_child(s->common->root, "set", variable);
	char *raw = cstmt_raw_args(s->common, st);
	if (!raw) {
		return NULL;
	}
	const char *value = raw;
	if (g_str_has_prefix(value, variable)) {
		value += strlen(variable);
	}
	while (*value == ' ' || *value == '\t') {
		value++;
	}
	char *result = g_strdup(value);
	g_free(raw);
	return result;
}

static gboolean apply_programs(gpointer data) {
	struct launcher_page *p = data;
	p->programs_timer = 0;
	struct confdoc *common = p->s->common;
	bool changed = false;
	for (size_t i = 0; i < G_N_ELEMENTS(programs); i++) {
		const char *text = gtk_editable_get_text(GTK_EDITABLE(p->program_entries[i]));
		char *old = program_value(p->s, programs[i].variable);
		if (*text && g_strcmp0(old, text) != 0) {
			char *args = g_strdup_printf("%s %s", programs[i].variable, text);
			confdoc_set(common, common->root, "set", programs[i].variable, args);
			g_free(args);
			changed = true;
		}
		g_free(old);
	}
	if (changed) {
		settings_common_changed(p->s, true);
	}
	return G_SOURCE_REMOVE;
}

static void on_program_changed(GtkEditable *editable, gpointer data) {
	struct launcher_page *p = data;
	if (p->updating) {
		return;
	}
	if (p->programs_timer) {
		g_source_remove(p->programs_timer);
	}
	p->programs_timer = g_timeout_add(1000, apply_programs, p);
}

void launcher_page_refresh(struct settings *s) {
	struct launcher_page *p = s->launcher_page;
	if (!p) {
		return;
	}
	p->updating = true;
	char *current = cstmt_raw_args(s->common, confdoc_child(s->common->root, "launcher_command", NULL));
	if (!current || !*current) {
		g_free(current);
		current = g_strdup("builtin");
	}
	guint active = CUSTOM_LAUNCHER;
	for (guint i = 0; i < CUSTOM_LAUNCHER; i++) {
		if (strcmp(current, launchers[i].command) == 0) {
			active = i;
		}
	}
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->radios[active]), TRUE);
	if (active == CUSTOM_LAUNCHER &&
			strcmp(gtk_editable_get_text(GTK_EDITABLE(p->custom_entry)), current) != 0) {
		gtk_editable_set_text(GTK_EDITABLE(p->custom_entry), current);
	}
	g_free(current);

	for (size_t i = 0; i < G_N_ELEMENTS(programs); i++) {
		char *value = program_value(s, programs[i].variable);
		if (value && strcmp(gtk_editable_get_text(GTK_EDITABLE(p->program_entries[i])), value) != 0) {
			gtk_editable_set_text(GTK_EDITABLE(p->program_entries[i]), value);
		}
		g_free(value);
	}
	p->updating = false;
}

GtkWidget *launcher_page_new(struct settings *s) {
	struct launcher_page *p = g_new0(struct launcher_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Launcher & apps",
		"Choose the application launcher and the programs started by the keyboard shortcuts.",
		&content);

	GtkWidget *group = ui_group(content, "Application launcher",
		"Opened with Super+D in tile mode and Super+S in window mode (the launcher command).");
	for (guint i = 0; i <= CUSTOM_LAUNCHER; i++) {
		bool custom = i == CUSTOM_LAUNCHER;
		GtkWidget *radio = gtk_check_button_new();
		if (i > 0) {
			gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(p->radios[0]));
		}
		p->radios[i] = radio;
		g_object_set_data(G_OBJECT(radio), "index", GUINT_TO_POINTER(i));
		g_signal_connect(radio, "toggled", G_CALLBACK(on_radio_toggled), p);

		const char *title = custom ? "Custom command" : launchers[i].title;
		char *subtitle = NULL;
		bool installed = true;
		if (!custom && launchers[i].program) {
			char *path = g_find_program_in_path(launchers[i].program);
			installed = path != NULL;
			g_free(path);
			subtitle = installed ? g_strdup(launchers[i].command) :
				g_strdup_printf("%s (not installed)", launchers[i].command);
		} else if (!custom) {
			subtitle = g_strdup("tileWin's own app search");
		}
		GtkWidget *control = NULL;
		if (custom) {
			p->custom_entry = gtk_entry_new();
			gtk_entry_set_placeholder_text(GTK_ENTRY(p->custom_entry), "e.g. ~/bin/my-launcher");
			gtk_widget_set_size_request(p->custom_entry, 300, -1);
			g_signal_connect(p->custom_entry, "changed", G_CALLBACK(on_custom_changed), p);
			control = p->custom_entry;
		}
		GtkWidget *row = ui_row(group, title, subtitle, control);
		gtk_box_prepend(GTK_BOX(ui_row_box(row)), radio);
		gtk_widget_set_sensitive(row, installed);
		g_free(subtitle);
	}
	GtkWidget *try_button = gtk_button_new_with_label("Open launcher");
	g_signal_connect(try_button, "clicked", G_CALLBACK(on_try), p);
	ui_row(group, "Try it", NULL, try_button);

	GtkWidget *apps = ui_group(content, "Default programs",
		"Used by the shortcuts in both modes. tileWin reloads its config after a change.");
	for (size_t i = 0; i < G_N_ELEMENTS(programs); i++) {
		GtkWidget *entry = gtk_entry_new();
		gtk_widget_set_size_request(entry, 360, -1);
		g_signal_connect(entry, "changed", G_CALLBACK(on_program_changed), p);
		p->program_entries[i] = entry;
		ui_row(apps, programs[i].title, programs[i].hint, entry);
	}

	s->launcher_page = p;
	launcher_page_refresh(s);
	return page;
}
