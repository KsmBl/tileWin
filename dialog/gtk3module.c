/*
 * libtilewin-gtk3-dialogs.so: a GTK 3 module that shows GTK's message dialogs
 * ("Save changes?" with Save, Don't Save and Cancel, questions, warnings,
 * errors) as tilewin-dialog, a message box in the look of the theme that is on,
 * and hands the button pressed back to the app as the dialog's response.
 *
 * tileWin puts it in GTK3_MODULES of the apps it starts. GtkMessageDialog's
 * "show" is taken over: a dialog with only its texts and buttons is not shown
 * but sent to tilewin-dialog (with the app's window as its parent, so tileWin
 * floats it over the app), and its answer comes back through
 * gtk_dialog_response(), which also ends a gtk_dialog_run(). A dialog with more
 * in it (a check box, an entry, a list the app put in) is GTK's own, as nothing
 * in it may get lost; so is everything when tilewin-dialog cannot be started.
 *
 * TILEWIN_NO_GTK_DIALOGS=1 keeps GTK's dialogs for an app; TILEWIN_DIALOG names
 * another tilewin-dialog (the tests take the one of the build).
 */
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <gtk/gtk.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void (*parent_show)(GtkWidget *widget);

struct pending {
	GtkWidget *dialog;
	GdkWindow *exported; // the parent whose handle was exported, to unexport
	GPid pid;
	GString *answer;
	gulong destroy_handler;
};

static const char *helper(void) {
	const char *env = getenv("TILEWIN_DIALOG");
	return env && *env ? env : "tilewin-dialog";
}

static const char *type_name(GtkMessageType type) {
	switch (type) {
	case GTK_MESSAGE_INFO:
		return "info";
	case GTK_MESSAGE_WARNING:
		return "warning";
	case GTK_MESSAGE_QUESTION:
		return "question";
	case GTK_MESSAGE_ERROR:
		return "error";
	default:
		return "other";
	}
}

/* Only texts and buttons: anything else stays in GTK's own dialog. */
static gboolean only_texts_and_buttons(GtkWidget *dialog) {
	GtkWidget *area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
	GList *children = gtk_container_get_children(GTK_CONTAINER(area));
	gboolean ok = TRUE;
	for (GList *l = children; l; l = l->next) {
		if (gtk_widget_get_visible(l->data) && !GTK_IS_LABEL(l->data)) {
			ok = FALSE;
		}
	}
	g_list_free(children);
	// the content area holds the box of image and texts, and the buttons
	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	children = gtk_container_get_children(GTK_CONTAINER(content));
	int visible = 0;
	for (GList *l = children; l; l = l->next) {
		visible += gtk_widget_get_visible(l->data) ? 1 : 0;
	}
	g_list_free(children);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	GtkWidget *actions = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
	G_GNUC_END_IGNORE_DEPRECATIONS
	// the box of image and texts, and the box of the buttons when it is in there
	if (visible > (gtk_widget_is_ancestor(actions, content) ? 2 : 1)) {
		ok = FALSE;
	}
	return ok;
}

/* The buttons, their responses and the one Return presses; FALSE when there are none. */
static gboolean collect_buttons(GtkWidget *dialog, GKeyFile *kf) {
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	GtkWidget *actions = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
	G_GNUC_END_IGNORE_DEPRECATIONS
	GtkWidget *header = gtk_dialog_get_header_bar(GTK_DIALOG(dialog));
	GPtrArray *labels = g_ptr_array_new();
	GArray *responses = g_array_new(FALSE, FALSE, sizeof(gint));
	gboolean ok = TRUE, has_default = FALSE;
	int default_response = 0;
	GtkWidget *areas[] = { actions, header };
	for (size_t a = 0; a < G_N_ELEMENTS(areas); a++) {
		if (!areas[a]) {
			continue;
		}
		GList *children = gtk_container_get_children(GTK_CONTAINER(areas[a]));
		for (GList *l = children; l; l = l->next) {
			GtkWidget *w = l->data;
			if (!gtk_widget_get_visible(w)) {
				continue;
			}
			int response = gtk_dialog_get_response_for_widget(GTK_DIALOG(dialog), w);
			if (!GTK_IS_BUTTON(w) || response == GTK_RESPONSE_NONE) {
				if (areas[a] == actions) {
					ok = FALSE; // something of the app's own among the buttons
				}
				continue;
			}
			const char *label = gtk_button_get_label(GTK_BUTTON(w));
			if (!label || !*label) {
				ok = FALSE; // a button with only an icon: GTK's own then
				continue;
			}
			// with _ before the access key, as tilewin-dialog's buttons take it; a
			// label without access key has its own _ doubled
			char *text;
			if (gtk_button_get_use_underline(GTK_BUTTON(w))) {
				text = g_strdup(label);
			} else {
				char **parts = g_strsplit(label, "_", -1);
				text = g_strjoinv("__", parts);
				g_strfreev(parts);
			}
			g_ptr_array_add(labels, text);
			g_array_append_val(responses, response);
			if (gtk_widget_has_default(w) || (gtk_widget_get_can_default(w) &&
					gtk_widget_has_focus(w))) {
				has_default = TRUE;
				default_response = response;
			}
		}
		g_list_free(children);
	}
	if (labels->len == 0) {
		ok = FALSE;
	}
	if (ok) {
		g_ptr_array_add(labels, NULL);
		g_key_file_set_string_list(kf, "dialog", "labels",
			(const char *const *)labels->pdata, labels->len - 1);
		g_key_file_set_integer_list(kf, "dialog", "responses", (gint *)responses->data,
			responses->len);
		if (has_default) {
			g_key_file_set_integer(kf, "dialog", "default", default_response);
		}
	}
	g_ptr_array_set_free_func(labels, g_free);
	g_ptr_array_free(labels, TRUE);
	g_array_free(responses, TRUE);
	return ok;
}

static char *describe(GtkWidget *dialog) {
	GKeyFile *kf = g_key_file_new();
	char *text = NULL, *secondary = NULL;
	gboolean markup = FALSE, secondary_markup = FALSE;
	GtkMessageType type = GTK_MESSAGE_OTHER;
	g_object_get(dialog, "text", &text, "secondary-text", &secondary, "use-markup", &markup,
		"secondary-use-markup", &secondary_markup, "message-type", &type, NULL);
	const char *title = gtk_window_get_title(GTK_WINDOW(dialog));
	if (!title || !*title) {
		GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
		title = parent ? gtk_window_get_title(parent) : NULL;
	}
	if (!title || !*title) {
		title = g_get_application_name();
	}
	g_key_file_set_string(kf, "dialog", "title", title ? title : "");
	g_key_file_set_string(kf, "dialog", "text", text ? text : "");
	g_key_file_set_string(kf, "dialog", "secondary", secondary ? secondary : "");
	g_key_file_set_boolean(kf, "dialog", "markup", markup);
	g_key_file_set_boolean(kf, "dialog", "secondary_markup", secondary_markup);
	g_key_file_set_string(kf, "dialog", "type", type_name(type));
	g_free(text);
	g_free(secondary);
	char *data = NULL;
	if (collect_buttons(dialog, kf)) {
		data = g_key_file_to_data(kf, NULL, NULL);
	}
	g_key_file_unref(kf);
	return data;
}

static void finish(struct pending *p, int response) {
	GtkWidget *dialog = p->dialog;
	if (p->destroy_handler && dialog) {
		g_signal_handler_disconnect(dialog, p->destroy_handler);
	}
#ifdef GDK_WINDOWING_WAYLAND
	if (p->exported && GDK_IS_WAYLAND_WINDOW(p->exported)) {
		gdk_wayland_window_unexport_handle(p->exported);
	}
#endif
	if (p->exported) {
		g_object_unref(p->exported);
	}
	g_string_free(p->answer, TRUE);
	g_free(p);
	if (dialog) {
		g_object_set_data(G_OBJECT(dialog), "tilewin-dialog-shown", NULL);
		gtk_dialog_response(GTK_DIALOG(dialog), response);
		g_object_unref(dialog);
	}
}

/* The app destroyed the dialog while it was asked: the message box goes too. */
static void on_destroy(GtkWidget *dialog, gpointer data) {
	struct pending *p = data;
	p->destroy_handler = 0;
	if (p->pid > 0) {
		kill(p->pid, SIGTERM);
	}
}

static gboolean on_answer(GIOChannel *channel, GIOCondition condition, gpointer data) {
	struct pending *p = data;
	char buf[256];
	gsize n = 0;
	GIOStatus status = g_io_channel_read_chars(channel, buf, sizeof(buf), &n, NULL);
	if (n > 0) {
		g_string_append_len(p->answer, buf, n);
	}
	if (status == G_IO_STATUS_NORMAL || status == G_IO_STATUS_AGAIN) {
		return G_SOURCE_CONTINUE;
	}
	// the end: the answer, or the window closed when there is none
	g_io_channel_shutdown(channel, FALSE, NULL);
	char *end = NULL;
	long response = strtol(p->answer->str, &end, 10);
	if (end == p->answer->str) {
		response = GTK_RESPONSE_DELETE_EVENT;
	}
	g_spawn_close_pid(p->pid);
	p->pid = 0;
	finish(p, (int)response);
	return G_SOURCE_REMOVE;
}

static gboolean start_helper(GtkWidget *dialog, const char *parent_handle, GdkWindow *exported) {
	char *spec = describe(dialog);
	if (!spec) {
		return FALSE;
	}
	char *parent = parent_handle ? g_strdup_printf("wayland:%s", parent_handle) : NULL;
	char *argv[] = { (char *)helper(), parent ? "--parent" : NULL, parent, NULL };
	GPid pid = 0;
	int in = -1, out = -1;
	GError *error = NULL;
	gboolean ok = g_spawn_async_with_pipes(NULL, argv, NULL,
		G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &in, &out, NULL,
		&error);
	g_free(parent);
	if (!ok) {
		g_warning("tileWin dialogs: %s, GTK's own dialog then", error->message);
		g_error_free(error);
		g_free(spec);
		return FALSE;
	}
	// the spec is small: written whole before the helper reads it
	size_t len = strlen(spec), done = 0;
	while (done < len) {
		ssize_t w = write(in, spec + done, len - done);
		if (w <= 0) {
			break;
		}
		done += (size_t)w;
	}
	close(in);
	g_free(spec);

	struct pending *p = g_new0(struct pending, 1);
	p->dialog = g_object_ref(dialog);
	p->exported = exported ? g_object_ref(exported) : NULL;
	p->pid = pid;
	p->answer = g_string_new(NULL);
	p->destroy_handler = g_signal_connect(dialog, "destroy", G_CALLBACK(on_destroy), p);
	g_object_set_data(G_OBJECT(dialog), "tilewin-dialog-shown", GINT_TO_POINTER(1));
	GIOChannel *channel = g_io_channel_unix_new(out);
	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_answer, p);
	g_io_channel_unref(channel);
	return TRUE;
}

struct export {
	GtkWidget *dialog;
};

#ifdef GDK_WINDOWING_WAYLAND
static void exported_cb(GdkWindow *window, const char *handle, gpointer data) {
	struct export *e = data;
	GtkWidget *dialog = e->dialog;
	if (!start_helper(dialog, handle, window)) {
		gdk_wayland_window_unexport_handle(window);
		g_object_set_data(G_OBJECT(dialog), "tilewin-dialog-own", GINT_TO_POINTER(1));
		gtk_widget_show(dialog);
	}
}

static void export_free(gpointer data) {
	struct export *e = data;
	g_object_unref(e->dialog);
	g_free(e);
}
#endif

static void tw_show(GtkWidget *dialog) {
	static int disabled = -1;
	if (disabled < 0) {
		const char *off = getenv("TILEWIN_NO_GTK_DIALOGS");
		disabled = off && *off && strcmp(off, "0") != 0;
	}
	if (disabled || g_object_get_data(G_OBJECT(dialog), "tilewin-dialog-own") ||
			!only_texts_and_buttons(dialog)) {
		parent_show(dialog);
		return;
	}
	if (g_object_get_data(G_OBJECT(dialog), "tilewin-dialog-shown")) {
		return; // asked already, e.g. gtk_dialog_run() after gtk_widget_show()
	}
	GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
	GdkWindow *parent_window = parent ? gtk_widget_get_window(GTK_WIDGET(parent)) : NULL;
#ifdef GDK_WINDOWING_WAYLAND
	if (parent_window && GDK_IS_WAYLAND_WINDOW(parent_window)) {
		struct export *e = g_new0(struct export, 1);
		e->dialog = g_object_ref(dialog);
		g_object_set_data(G_OBJECT(dialog), "tilewin-dialog-shown", GINT_TO_POINTER(1));
		if (gdk_wayland_window_export_handle(parent_window, exported_cb, e, export_free)) {
			return;
		}
		g_object_set_data(G_OBJECT(dialog), "tilewin-dialog-shown", NULL);
		export_free(e);
	}
#endif
	if (!start_helper(dialog, NULL, NULL)) {
		parent_show(dialog);
	}
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv) {
	GtkWidgetClass *klass = g_type_class_ref(GTK_TYPE_MESSAGE_DIALOG);
	if (klass->show != tw_show) {
		parent_show = klass->show;
		klass->show = tw_show;
	}
}

G_MODULE_EXPORT const char *g_module_check_init(GModule *module) {
	// classes keep the pointer to tw_show: the module must never be unloaded
	g_module_make_resident(module);
	return NULL;
}
