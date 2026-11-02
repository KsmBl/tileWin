#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dm/config.h"
#include "settings.h"

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

/*
 * The Login screen page: every setting of the display manager, in rows made from
 * the table in dm/config.c, so a new setting there has its row here. Nothing
 * counts until Save, which goes through tilewin-dm-apply as root; that helper
 * checks the file before it replaces the old one. Preview shows the login screen
 * with what is on the page, checks nothing, and Esc ends it.
 *
 * For tests: TILEWIN_DM_APPLY is a helper run as it is, without pkexec, and
 * TILEWIN_DM_CONF is the file to read.
 */
struct login_page {
	struct settings *s;
	GtkWidget **controls; // as dm_options
	char **env_lines;
	GtkWidget *service_switch, *service_row;
	char *preview_file;
	bool updating, enabled;
};

static const char *conf_path(void) {
	const char *path = g_getenv("TILEWIN_DM_CONF");
	return path && *path ? path : DM_CONFIG_PATH;
}

static char *helper_path(void) {
	const char *test = g_getenv("TILEWIN_DM_APPLY");
	return g_strdup(test && *test ? test : TILEWIN_LIBEXECDIR "/tilewin-dm-apply");
}

/* ---------- controls and values ---------- */

static GtkWidget *control_new(const struct dm_option *o) {
	switch (o->type) {
	case DM_BOOL:
		return gtk_switch_new();
	case DM_INT:
		return gtk_spin_button_new_with_range(o->min, o->max, 1);
	case DM_CHOICE:
		return gtk_drop_down_new_from_strings(o->choices);
	case DM_TEXT: {
		GtkWidget *entry = gtk_entry_new();
		gtk_widget_set_size_request(entry, 280, -1);
		return entry;
	}
	}
	return NULL;
}

static void control_set(const struct dm_option *o, GtkWidget *control, const char *value) {
	switch (o->type) {
	case DM_BOOL:
		gtk_switch_set_active(GTK_SWITCH(control), strcmp(value, "yes") == 0 ||
			strcmp(value, "true") == 0 || strcmp(value, "on") == 0 || strcmp(value, "1") == 0);
		break;
	case DM_INT:
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(control), atoi(value));
		break;
	case DM_CHOICE:
		for (guint i = 0; o->choices[i]; i++) {
			if (strcmp(o->choices[i], value) == 0) {
				gtk_drop_down_set_selected(GTK_DROP_DOWN(control), i);
			}
		}
		break;
	case DM_TEXT:
		gtk_editable_set_text(GTK_EDITABLE(control), value);
		break;
	}
}

static char *control_value(const struct dm_option *o, GtkWidget *control) {
	switch (o->type) {
	case DM_BOOL:
		return g_strdup(gtk_switch_get_active(GTK_SWITCH(control)) ? "yes" : "no");
	case DM_INT:
		return g_strdup_printf("%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)));
	case DM_CHOICE:
		return g_strdup(o->choices[gtk_drop_down_get_selected(GTK_DROP_DOWN(control))]);
	case DM_TEXT:
		return g_strdup(gtk_editable_get_text(GTK_EDITABLE(control)));
	}
	return g_strdup("");
}

/* The file as it would be: only what differs from the defaults, and the env lines. */
static char *config_text(struct login_page *p) {
	GString *out = g_string_new("# The display manager of tileWin, written by its settings page.\n"
		"# See docs/display-manager.md for what each line means.\n\n");
	for (size_t i = 0; i < dm_option_count; i++) {
		const struct dm_option *o = &dm_options[i];
		char *value = control_value(o, p->controls[i]);
		// a line ends a value: a value with one in it cannot be meant
		value[strcspn(value, "\r\n")] = '\0';
		if (strcmp(value, o->fallback) != 0) {
			if (o->type == DM_TEXT) {
				g_string_append_printf(out, "%s \"%s\"\n", o->key, value);
			} else {
				g_string_append_printf(out, "%s %s\n", o->key, value);
			}
		}
		g_free(value);
	}
	for (char **env = p->env_lines; env && *env; env++) {
		g_string_append_printf(out, "env %s\n", *env);
	}
	return g_string_free(out, FALSE);
}

static void load_values(struct login_page *p) {
	p->updating = true;
	struct dm_config *config = dm_config_load(conf_path());
	for (size_t i = 0; i < dm_option_count; i++) {
		control_set(&dm_options[i], p->controls[i], config->values[i]);
	}
	g_strfreev(p->env_lines);
	p->env_lines = g_new0(char *, config->env_count + 1);
	for (size_t i = 0; i < config->env_count; i++) {
		p->env_lines[i] = g_strdup(config->env[i]);
	}
	dm_config_free(config);
	p->updating = false;
}

/* ---------- the helper ---------- */

struct call {
	struct login_page *p;
	char *done_text;
	void (*done)(struct login_page *p, bool ok, const char *output);
};

static void helper_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct call *call = data;
	char *out = NULL, *err = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, NULL) &&
		g_subprocess_get_successful(G_SUBPROCESS(source));
	if (!ok) {
		int status = g_subprocess_get_if_exited(G_SUBPROCESS(source)) ?
			g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
		char *text = g_strdup_printf("It did not work: %s", err && *err ? g_strstrip(err) :
			status == 126 || status == 127 ? "not authorized" : "the helper failed");
		settings_problem(call->p->s, text, NULL, NULL, NULL);
		g_free(text);
	} else if (call->done_text) {
		settings_status(call->p->s, "%s", call->done_text);
	}
	if (call->done) {
		call->done(call->p, ok, out);
	}
	g_free(out);
	g_free(err);
	g_free(call->done_text);
	g_free(call);
	g_object_unref(source);
}

static void run_helper(struct login_page *p, const char *what, const char *input,
		const char *done_text, void (*done)(struct login_page *, bool, const char *), bool as_root) {
	char *helper = helper_path();
	const char *argv[4];
	int n = 0;
	const char *test = g_getenv("TILEWIN_DM_APPLY");
	if (as_root && (!test || !*test)) {
		argv[n++] = "pkexec";
	}
	argv[n++] = helper;
	argv[n++] = what;
	argv[n] = NULL;
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_free(helper);
	if (!proc) {
		char *text = g_strdup_printf("It did not work: %s", error->message);
		settings_problem(p->s, text, NULL, NULL, NULL);
		g_free(text);
		g_error_free(error);
		return;
	}
	struct call *call = g_new0(struct call, 1);
	call->p = p;
	call->done_text = g_strdup(done_text);
	call->done = done;
	g_subprocess_communicate_utf8_async(proc, input ? input : "", NULL, helper_finished, call);
}

/* ---------- buttons ---------- */

static void on_save(GtkButton *button, gpointer data) {
	struct login_page *p = data;
	char *text = config_text(p);
	settings_problem_clear(p->s);
	run_helper(p, "write", text, "Saved. The login screen takes it at its next start.", NULL, true);
	g_free(text);
}

static void on_preview(GtkButton *button, gpointer data) {
	struct login_page *p = data;
	char *text = config_text(p);
	GError *error = NULL;
	if (!p->preview_file) {
		p->preview_file = g_build_filename(g_get_user_runtime_dir(), "tilewin-dm-preview.conf", NULL);
	}
	if (g_file_set_contents(p->preview_file, text, -1, &error)) {
		const char *argv[] = { "tilewin-dm-greeter", "--preview", "--config", p->preview_file, NULL };
		g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
	}
	if (error) {
		char *problem = g_strdup_printf("No preview: %s", error->message);
		settings_problem(p->s, problem, NULL, NULL, NULL);
		g_free(problem);
		g_error_free(error);
	}
	g_free(text);
}

static void show_service(struct login_page *p) {
	p->updating = true;
	gtk_switch_set_active(GTK_SWITCH(p->service_switch), p->enabled);
	gtk_switch_set_state(GTK_SWITCH(p->service_switch), p->enabled);
	p->updating = false;
}

static void service_read(struct login_page *p, bool ok, const char *output) {
	p->enabled = ok && output && g_str_has_prefix(output, "enabled");
	show_service(p);
}

static void service_changed(struct login_page *p, bool ok, const char *output) {
	if (ok) {
		p->enabled = !p->enabled;
	}
	show_service(p);
}

static void on_service_answer(GObject *source, GAsyncResult *result, gpointer data) {
	struct login_page *p = data;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	if (choice == 1) {
		run_helper(p, p->enabled ? "disable" : "enable", NULL,
			p->enabled ? "Your other display manager is back at the next start of the computer." :
			"This login screen comes up at the next start of the computer.",
			service_changed, true);
	} else {
		show_service(p);
	}
}

static gboolean on_service_switch(GtkSwitch *widget, gboolean active, gpointer data) {
	struct login_page *p = data;
	if (p->updating || active == p->enabled) {
		return TRUE;
	}
	// this decides whether anyone can log in tomorrow: not on a click alone
	GtkAlertDialog *dialog = gtk_alert_dialog_new(active ?
		"Use this as the display manager of the computer?" : "Stop using this display manager?");
	gtk_alert_dialog_set_detail(dialog, active ?
		"It takes over from your current one at the next start of the computer. If it should "
		"not come up, Ctrl+Alt+F2 gets you to a text console, where "
		"\"sudo tilewin-dm-apply disable\" puts the old one back." :
		"The display manager used before comes back at the next start of the computer.");
	gtk_alert_dialog_set_buttons(dialog, (const char *const[]){ "Cancel",
		active ? "Use it" : "Stop using it", NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, p->s->window, NULL, on_service_answer, p);
	g_object_unref(dialog);
	return TRUE;
}

GtkWidget *login_page_new(struct settings *s) {
	struct login_page *p = g_new0(struct login_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Login screen",
		"The display manager of tileWin: the screen where you log in, in the look of your theme. "
		"Nothing counts until Save. Preview shows it right away; Esc ends the preview.", &content);

	GtkWidget *service = ui_group(content, "Display manager", NULL);
	p->service_switch = gtk_switch_new();
	g_signal_connect(p->service_switch, "state-set", G_CALLBACK(on_service_switch), p);
	p->service_row = ui_row(service, "Use this login screen",
		"Takes over from the display manager in use at the next start of the computer",
		p->service_switch);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *preview = gtk_button_new_with_label("Preview");
	g_signal_connect(preview, "clicked", G_CALLBACK(on_preview), p);
	gtk_box_append(GTK_BOX(buttons), preview);
	GtkWidget *save = gtk_button_new_with_label("Save");
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), p);
	gtk_box_append(GTK_BOX(buttons), save);
	ui_row(service, "These settings", NULL, buttons);

	p->controls = g_new0(GtkWidget *, dm_option_count);
	const char *current = NULL;
	GtkWidget *group = NULL;
	for (size_t i = 0; i < dm_option_count; i++) {
		const struct dm_option *o = &dm_options[i];
		if (!current || strcmp(current, o->group) != 0) {
			current = o->group;
			group = ui_group(content, current, NULL);
		}
		p->controls[i] = control_new(o);
		ui_row(group, o->label, o->help, p->controls[i]);
	}
	load_values(p);
	run_helper(p, "status", NULL, NULL, service_read, false);
	return page;
}
