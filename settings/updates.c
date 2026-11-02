#include <string.h>
#include "settings.h"

/*
 * The Updates group of the About page: the newest release, or the newest source
 * built in a temporary folder. tilewin-update does the work and restarts tileWin
 * after it; this only starts it and shows what it says. GitHub is asked when the
 * page is shown, not whenever the settings open.
 * TILEWIN_UPDATE_COMMAND is run instead of tilewin-update by the tests.
 */
struct updates {
	struct settings *s;
	GtkWidget *release_row, *source_row, *release_button, *source_button, *spinner, *line, *expander, *log;
	GDataInputStream *reader;
	GSubprocess *proc;
	char *last_line;
	char *running; // "release" or "source"
	bool asked;
};

static const char *update_command(void) {
	const char *command = g_getenv("TILEWIN_UPDATE_COMMAND");
	return command && *command ? command : "tilewin-update";
}

static void set_subtitle(GtkWidget *row, const char *text) {
	GtkWidget *labels = gtk_widget_get_first_child(ui_row_box(row));
	gtk_label_set_text(GTK_LABEL(gtk_widget_get_last_child(labels)), text);
}

/* ---------- what the newest release is ---------- */

static void check_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct updates *u = data;
	char *out = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL,
		NULL) && g_subprocess_get_successful(G_SUBPROCESS(source));
	char *release = NULL, *installed = NULL;
	bool managed = false;
	char **lines = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (g_str_has_prefix(lines[i], "release ")) {
			release = g_strdup(lines[i] + 8);
		} else if (strcmp(lines[i], "managed yes") == 0) {
			managed = true;
		} else if (g_str_has_prefix(lines[i], "installed ")) {
			installed = g_strdup(lines[i] + 10);
		}
	}
	g_strfreev(lines);
	char *text;
	if (!ok || !release) {
		text = g_strdup("GitHub did not answer, or has no release yet");
	} else if (g_strcmp0(release, installed) == 0) {
		text = g_strdup_printf("%s: this is what you have", release);
	} else {
		text = g_strdup_printf("%s is out; you have %s", release, installed ? installed : "another");
	}
	// also when GitHub did not answer: the line came before it was asked
	if (managed) {
		char *more = g_strdup_printf("%s. Your package manager updates tileWin here.", text);
		g_free(text);
		text = more;
		gtk_widget_set_visible(u->release_button, FALSE);
		gtk_widget_set_visible(u->source_row, FALSE);
	}
	set_subtitle(u->release_row, text);
	g_free(text);
	g_free(release);
	g_free(installed);
	g_free(out);
	g_object_unref(source);
}

static void on_page_shown(GtkWidget *widget, gpointer data) {
	struct updates *u = data;
	if (u->asked) {
		return;
	}
	u->asked = true;
	GError *error = NULL;
	const char *argv[] = { update_command(), "check", NULL };
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	if (!proc) {
		set_subtitle(u->release_row, error->message);
		g_error_free(error);
		return;
	}
	g_subprocess_communicate_utf8_async(proc, "", NULL, check_done, u);
}

/* ---------- updating ---------- */

static void start_update(struct updates *u, const char *how);

static void retry_update(gpointer data) {
	struct updates *u = data;
	char *how = g_strdup(u->running);
	start_update(u, how);
	g_free(how);
}

static void set_busy(struct updates *u, bool busy) {
	gtk_widget_set_sensitive(u->release_button, !busy);
	gtk_widget_set_sensitive(u->source_button, !busy);
	gtk_widget_set_visible(u->spinner, busy);
	gtk_spinner_set_spinning(GTK_SPINNER(u->spinner), busy);
}

static void update_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct updates *u = data;
	gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL);
	set_busy(u, false);
	if (ok) {
		gtk_label_set_text(GTK_LABEL(u->line), u->last_line ? u->last_line : "Done");
		settings_status(u->s, "%s", u->last_line ? u->last_line : "Updated");
	} else {
		gtk_label_set_text(GTK_LABEL(u->line), "The update did not finish: see the log below");
		gtk_expander_set_expanded(GTK_EXPANDER(u->expander), TRUE);
		char *text = g_strdup_printf("The update failed: %s", u->last_line ? u->last_line :
			"tilewin-update could not run");
		settings_problem(u->s, text, "Try again", retry_update, u);
		g_free(text);
	}
	g_clear_object(&u->proc);
}

static void read_next(struct updates *u);

static void line_read(GObject *source, GAsyncResult *result, gpointer data) {
	struct updates *u = data;
	char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result,
		NULL, NULL);
	if (!line) {
		g_clear_object(&u->reader);
		g_subprocess_wait_check_async(u->proc, NULL, update_finished, u);
		return;
	}
	// a line of its own with what goes on now; the rest is in the log
	const char *shown = g_str_has_prefix(line, "==> ") ? line + 4 : line;
	if (*shown) {
		g_free(u->last_line);
		u->last_line = g_strdup(shown);
		gtk_label_set_text(GTK_LABEL(u->line), shown);
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(u->log));
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buffer, &end);
	gtk_text_buffer_insert(buffer, &end, line, -1);
	gtk_text_buffer_insert(buffer, &end, "\n", 1);
	g_free(line);
	read_next(u);
}

static void read_next(struct updates *u) {
	g_data_input_stream_read_line_async(u->reader, G_PRIORITY_DEFAULT, NULL, line_read, u);
}

static void start_update(struct updates *u, const char *how) {
	if (u->proc) {
		return;
	}
	settings_problem_clear(u->s);
	g_free(u->running);
	u->running = g_strdup(how);
	g_clear_pointer(&u->last_line, g_free);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(u->log)), "", 0);
	// a pipe for its input: no terminal, so pkexec asks for the password
	GError *error = NULL;
	const char *argv[] = { update_command(), how, NULL };
	u->proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &error);
	if (!u->proc) {
		char *text = g_strdup_printf("Could not start the update: %s", error->message);
		settings_problem(u->s, text, NULL, NULL, NULL);
		g_free(text);
		g_error_free(error);
		return;
	}
	set_busy(u, true);
	gtk_label_set_text(GTK_LABEL(u->line), "Starting…");
	gtk_widget_set_visible(u->line, TRUE);
	gtk_widget_set_visible(u->expander, TRUE);
	u->reader = g_data_input_stream_new(g_subprocess_get_stdout_pipe(u->proc));
	read_next(u);
}

static void on_release(GtkButton *button, gpointer data) {
	start_update(data, "release");
}

static void on_source(GtkButton *button, gpointer data) {
	start_update(data, "source");
}

static void on_welcome(GtkButton *button, gpointer data) {
	const char *argv[] = { "tilewin-settings", "--welcome", NULL };
	g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

void updates_group_new(struct settings *s, GtkWidget *content) {
	struct updates *u = g_new0(struct updates, 1);
	u->s = s;
	GtkWidget *group = ui_group(content, "Updates",
		"After an update tileWin restarts by itself. Your windows stay open.");
	g_signal_connect(group, "map", G_CALLBACK(on_page_shown), u);

	u->release_button = gtk_button_new_with_label("Update");
	g_signal_connect(u->release_button, "clicked", G_CALLBACK(on_release), u);
	u->release_row = ui_row(group, "Newest release", "Asking GitHub…", u->release_button);

	u->source_button = gtk_button_new_with_label("Build");
	g_signal_connect(u->source_button, "clicked", G_CALLBACK(on_source), u);
	u->source_row = ui_row(group, "Newest source",
		"Pulls the repository into a temporary folder, builds it there, and deletes the "
		"folder afterwards. Takes a few minutes.", u->source_button);

	GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	u->spinner = gtk_spinner_new();
	gtk_widget_set_visible(u->spinner, FALSE);
	gtk_box_append(GTK_BOX(progress), u->spinner);
	u->line = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(u->line), 0);
	gtk_label_set_ellipsize(GTK_LABEL(u->line), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(u->line, TRUE);
	gtk_widget_set_visible(u->line, FALSE);
	gtk_box_append(GTK_BOX(progress), u->line);
	gtk_box_append(GTK_BOX(content), progress);

	u->log = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(u->log), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(u->log), TRUE);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), u->log);
	gtk_widget_set_size_request(scroll, -1, 180);
	u->expander = gtk_expander_new("What it is doing");
	gtk_expander_set_child(GTK_EXPANDER(u->expander), scroll);
	gtk_widget_set_visible(u->expander, FALSE);
	gtk_box_append(GTK_BOX(content), u->expander);

	GtkWidget *more = ui_group(content, NULL, NULL);
	GtkWidget *welcome = gtk_button_new_with_label("Show");
	g_signal_connect(welcome, "clicked", G_CALLBACK(on_welcome), u);
	ui_row(more, "Welcome window", "The theme, the mode and the keys worth knowing, as at the "
		"first start", welcome);
}
