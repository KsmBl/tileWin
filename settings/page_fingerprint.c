/*
 * The Fingerprint page: the fingers the user signs in with on the lock screen,
 * as the Sign-in options of Windows have them.
 *
 *   Reader         the reader fprintd uses, and the switch "Unlock the lock
 *                  screen with a fingerprint" (~/.config/tileWin/lock-fingerprint)
 *   Fingerprints   the fingers fprintd knows of this user, each with Remove
 *   Add            a finger that is not there yet: a dialog says to touch the
 *                  reader again and again and shows how far it is
 *
 * Everything goes to fprintd (net.reactivated.Fprint) on the system bus: Claim,
 * EnrollStart and its EnrollStatus, DeleteEnrolledFinger, Release. Enrolling
 * and removing the user's own fingers needs no password (fprintd's polkit
 * rules). TILEWIN_FPRINT_BUS=session talks to a fake fprintd of the tests on
 * the session bus.
 */
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "tw_paths.h"

#define FPRINT "net.reactivated.Fprint"
#define FPRINT_DEVICE "net.reactivated.Fprint.Device"
#define FPRINT_MANAGER "net.reactivated.Fprint.Manager"

static const struct {
	const char *id, *name;
} fingers[] = {
	{ "right-index-finger", "Right index finger" },
	{ "left-index-finger", "Left index finger" },
	{ "right-thumb", "Right thumb" },
	{ "left-thumb", "Left thumb" },
	{ "right-middle-finger", "Right middle finger" },
	{ "left-middle-finger", "Left middle finger" },
	{ "right-ring-finger", "Right ring finger" },
	{ "left-ring-finger", "Left ring finger" },
	{ "right-little-finger", "Right little finger" },
	{ "left-little-finger", "Left little finger" },
};

struct fp_page {
	struct settings *s;
	GDBusConnection *bus;
	char *device, *reader_name;
	int stages;
	GPtrArray *enrolled; // char *: finger ids
	GtkWidget *content, *box; // box: what is rebuilt when the fingers change
	GtkWidget *finger_dd, *add;
	bool busy;
	// the dialog of adding one
	GtkWidget *dialog, *progress, *count, *status, *cancel, *again;
	char *enrolling;
	char *finger; // the finger of the dialog, for Try again
	int passed;
	guint enroll_signal;
	bool enroll_done;
};

static const char *finger_name(const char *id) {
	for (size_t i = 0; i < G_N_ELEMENTS(fingers); i++) {
		if (strcmp(fingers[i].id, id) == 0) {
			return fingers[i].name;
		}
	}
	return id;
}

static char *switch_path(void) {
	char *dir = tw_config_dir();
	char *path = g_build_filename(dir, "lock-fingerprint", NULL);
	free(dir);
	return path;
}

static GVariant *call_sync(struct fp_page *p, const char *path, const char *iface,
		const char *method, GVariant *args, const char *reply, GError **error) {
	return g_dbus_connection_call_sync(p->bus, FPRINT, path, iface, method, args,
		reply ? G_VARIANT_TYPE(reply) : NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, error);
}

/* The reader, its name and stages, and the user's fingers on it. */
static void load(struct fp_page *p) {
	g_clear_pointer(&p->device, g_free);
	g_clear_pointer(&p->reader_name, g_free);
	g_ptr_array_set_size(p->enrolled, 0);
	if (!p->bus) {
		return;
	}
	GVariant *r = call_sync(p, "/net/reactivated/Fprint/Manager", FPRINT_MANAGER,
		"GetDefaultDevice", NULL, "(o)", NULL);
	if (!r) {
		return;
	}
	g_variant_get(r, "(o)", &p->device);
	g_variant_unref(r);
	GError *error = NULL;
	r = g_dbus_connection_call_sync(p->bus, FPRINT, p->device, "org.freedesktop.DBus.Properties",
		"GetAll", g_variant_new("(s)", FPRINT_DEVICE), G_VARIANT_TYPE("(a{sv})"),
		G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
	if (r) {
		GVariant *props = g_variant_get_child_value(r, 0);
		g_variant_lookup(props, "name", "s", &p->reader_name);
		g_variant_lookup(props, "num-enroll-stages", "i", &p->stages);
		g_variant_unref(props);
		g_variant_unref(r);
	}
	r = call_sync(p, p->device, FPRINT_DEVICE, "ListEnrolledFingers",
		g_variant_new("(s)", g_get_user_name()), "(as)", &error);
	if (r) {
		GVariantIter *iter;
		const char *finger;
		g_variant_get(r, "(as)", &iter);
		while (g_variant_iter_next(iter, "&s", &finger)) {
			g_ptr_array_add(p->enrolled, g_strdup(finger));
		}
		g_variant_iter_free(iter);
		g_variant_unref(r);
	}
	g_clear_error(&error); // none enrolled is an error of fprintd's
}

static bool is_enrolled(struct fp_page *p, const char *id) {
	for (guint i = 0; i < p->enrolled->len; i++) {
		if (strcmp(p->enrolled->pdata[i], id) == 0) {
			return true;
		}
	}
	return false;
}

static void rebuild(struct fp_page *p);

/* ---------- removing ---------- */

static void on_remove(GtkButton *button, gpointer data) {
	struct fp_page *p = data;
	const char *id = g_object_get_data(G_OBJECT(button), "finger");
	if (p->busy || !id || !p->device) {
		return;
	}
	GError *error = NULL;
	GVariant *r = call_sync(p, p->device, FPRINT_DEVICE, "Claim",
		g_variant_new("(s)", g_get_user_name()), NULL, &error);
	if (r) {
		g_variant_unref(r);
		r = call_sync(p, p->device, FPRINT_DEVICE, "DeleteEnrolledFinger",
			g_variant_new("(s)", id), NULL, &error);
		if (r) {
			g_variant_unref(r);
		}
		GVariant *released = call_sync(p, p->device, FPRINT_DEVICE, "Release", NULL, NULL,
			NULL);
		if (released) {
			g_variant_unref(released);
		}
	}
	if (error) {
		settings_status(p->s, "The fingerprint was not removed: %s", error->message);
		g_error_free(error);
	} else {
		settings_status(p->s, "%s removed", finger_name(id));
	}
	load(p);
	rebuild(p);
}

/* ---------- adding ---------- */

static void enroll_end(struct fp_page *p) {
	if (p->enroll_signal) {
		g_dbus_connection_signal_unsubscribe(p->bus, p->enroll_signal);
		p->enroll_signal = 0;
	}
	if (p->enrolling) {
		GVariant *r = call_sync(p, p->device, FPRINT_DEVICE, "EnrollStop", NULL, NULL, NULL);
		if (r) {
			g_variant_unref(r);
		}
		r = call_sync(p, p->device, FPRINT_DEVICE, "Release", NULL, NULL, NULL);
		if (r) {
			g_variant_unref(r);
		}
		g_clear_pointer(&p->enrolling, g_free);
	}
	p->busy = false;
}

static void enroll_status_text(struct fp_page *p, const char *text) {
	if (p->status) {
		gtk_label_set_text(GTK_LABEL(p->status), text);
	}
}

static void show_count(struct fp_page *p) {
	if (p->count) {
		char *text = p->stages > 0 ? g_strdup_printf("%d of %d touches", p->passed, p->stages) :
			g_strdup_printf("%d touches", p->passed);
		gtk_label_set_text(GTK_LABEL(p->count), text);
		g_free(text);
	}
}

static void finish_dialog(struct fp_page *p, const char *text, bool success) {
	enroll_status_text(p, text);
	if (p->cancel) {
		gtk_button_set_label(GTK_BUTTON(p->cancel), success ? "Done" : "Close");
	}
	if (p->again) {
		gtk_widget_set_visible(p->again, !success); // from the start, without closing
	}
	p->enroll_done = true;
	char *name = g_strdup(p->enrolling ? finger_name(p->enrolling) : "");
	enroll_end(p);
	if (success) {
		settings_status(p->s, "%s added", name);
	}
	g_free(name);
	load(p);
	rebuild(p);
}

static void on_enroll_status(GDBusConnection *bus, const char *sender, const char *path,
		const char *iface, const char *signal, GVariant *params, gpointer data) {
	struct fp_page *p = data;
	const char *result = NULL;
	gboolean done = FALSE;
	g_variant_get(params, "(&sb)", &result, &done);
	if (!result || !p->enrolling) {
		return;
	}
	if (strcmp(result, "enroll-completed") == 0) {
		if (p->progress) {
			gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress), 1);
		}
		p->passed = MAX(p->passed, p->stages);
		show_count(p);
		finish_dialog(p, "The fingerprint is added. It unlocks the lock screen now.", true);
		return;
	}
	if (strcmp(result, "enroll-stage-passed") == 0) {
		p->passed++;
		if (p->progress) {
			double whole = p->stages > 0 ? p->stages : 10;
			gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress),
				MIN(p->passed / whole, 0.99));
		}
		show_count(p);
		enroll_status_text(p, "Good. Lift the finger and touch the reader again, with another "
			"part of it.");
		return;
	}
	static const struct {
		const char *result, *text;
	} hints[] = {
		{ "enroll-retry-scan", "That did not work. Touch the reader again." },
		{ "enroll-swipe-too-short", "Swipe the finger again, a little slower." },
		{ "enroll-finger-not-centered", "Put the finger in the middle of the reader." },
		{ "enroll-remove-and-retry", "Lift the finger and touch the reader again." },
		{ "enroll-failed", "The reader stopped waiting for the next touch. Try again: lift the "
			"finger all the way between touches, keep it on the reader for a second, and "
			"use another part of it each time." },
		{ "enroll-data-full", "The reader has no room for another fingerprint." },
		{ "enroll-duplicate", "This fingerprint is already added, as another finger." },
		{ "enroll-disconnected", "The fingerprint reader was disconnected." },
		{ "enroll-unknown-error", "The fingerprint reader had a problem. Try again." },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(hints); i++) {
		if (strcmp(result, hints[i].result) == 0) {
			if (done) {
				finish_dialog(p, hints[i].text, false);
			} else {
				enroll_status_text(p, hints[i].text);
			}
			return;
		}
	}
	if (done) {
		finish_dialog(p, "The fingerprint could not be added. Try again.", false);
	}
}

static void on_dialog_close(GtkButton *button, gpointer data) {
	struct fp_page *p = data;
	GtkWidget *dialog = p->dialog;
	if (!p->enroll_done) {
		enroll_end(p); // cancelled
		settings_status(p->s, "No fingerprint added");
	}
	p->dialog = p->progress = p->count = p->status = p->cancel = p->again = NULL;
	if (dialog) {
		gtk_window_destroy(GTK_WINDOW(dialog));
	}
}

static gboolean on_dialog_close_request(GtkWindow *window, gpointer data) {
	on_dialog_close(NULL, data);
	return TRUE;
}

static void enroll_begin(struct fp_page *p, const char *id);

static void on_again(GtkButton *button, gpointer data) {
	struct fp_page *p = data;
	if (p->finger) {
		char *id = g_strdup(p->finger);
		enroll_begin(p, id);
		g_free(id);
	}
}

static void open_dialog(struct fp_page *p, const char *id) {
	GtkWidget *dialog = gtk_window_new();
	p->dialog = dialog;
	gtk_window_set_title(GTK_WINDOW(dialog), "Add a fingerprint");
	gtk_window_set_transient_for(GTK_WINDOW(dialog), p->s->window);
	gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	g_signal_connect(dialog, "close-request", G_CALLBACK(on_dialog_close_request), p);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_widget_set_margin_top(box, 20);
	gtk_widget_set_margin_bottom(box, 18);
	gtk_window_set_child(GTK_WINDOW(dialog), box);
	char *title = g_strdup_printf("Add your %s", finger_name(id));
	char *lower = g_utf8_strdown(title, -1);
	lower[0] = title[0];
	GtkWidget *heading = gtk_label_new(lower);
	gtk_widget_add_css_class(heading, "title-4");
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_box_append(GTK_BOX(box), heading);
	g_free(title);
	g_free(lower);
	GtkWidget *how = gtk_label_new("Lay this finger on the fingerprint reader for a second, lift it "
		"all the way, and lay it on again, each time with another part of it: a little "
		"higher, lower, more to the left or the right, its tip, its sides. A touch too much like "
		"one before does not count. Go on until the bar is full.");
	gtk_label_set_wrap(GTK_LABEL(how), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(how), 44);
	gtk_label_set_xalign(GTK_LABEL(how), 0);
	gtk_box_append(GTK_BOX(box), how);
	p->progress = gtk_progress_bar_new();
	gtk_box_append(GTK_BOX(box), p->progress);
	p->count = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(p->count), 0);
	gtk_widget_add_css_class(p->count, "dim-label");
	gtk_box_append(GTK_BOX(box), p->count);
	show_count(p);
	p->status = gtk_label_new("Touch the reader.");
	gtk_label_set_xalign(GTK_LABEL(p->status), 0);
	gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
	// as wide as the text above: a long message wraps instead of widening the window
	gtk_label_set_max_width_chars(GTK_LABEL(p->status), 44);
	gtk_label_set_width_chars(GTK_LABEL(p->status), 44);
	gtk_label_set_lines(GTK_LABEL(p->status), 4);
	gtk_box_append(GTK_BOX(box), p->status);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	p->again = gtk_button_new_with_label("Try again");
	gtk_widget_set_visible(p->again, FALSE);
	g_signal_connect(p->again, "clicked", G_CALLBACK(on_again), p);
	gtk_box_append(GTK_BOX(buttons), p->again);
	p->cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(p->cancel, "clicked", G_CALLBACK(on_dialog_close), p);
	gtk_box_append(GTK_BOX(buttons), p->cancel);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_window_present(GTK_WINDOW(dialog));
}

static void on_add(GtkButton *button, gpointer data) {
	struct fp_page *p = data;
	if (p->busy || !p->device || !p->finger_dd) {
		return;
	}
	GtkStringObject *item = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(p->finger_dd));
	const char *name = item ? gtk_string_object_get_string(item) : NULL;
	const char *id = NULL;
	for (size_t i = 0; name && i < G_N_ELEMENTS(fingers); i++) {
		if (strcmp(fingers[i].name, name) == 0) {
			id = fingers[i].id;
		}
	}
	if (id) {
		enroll_begin(p, id);
	}
}

/* Claims the reader and starts adding the finger, in the dialog, opened or again. */
static void enroll_begin(struct fp_page *p, const char *id) {
	if (p->busy) {
		return;
	}
	GError *error = NULL;
	GVariant *r = call_sync(p, p->device, FPRINT_DEVICE, "Claim",
		g_variant_new("(s)", g_get_user_name()), NULL, &error);
	if (!r) {
		settings_status(p->s, "The fingerprint reader is in use: %s", error->message);
		g_error_free(error);
		return;
	}
	g_variant_unref(r);
	p->busy = true;
	p->enrolling = g_strdup(id);
	p->passed = 0;
	p->enroll_done = false;
	g_free(p->finger);
	p->finger = g_strdup(id);
	p->enroll_signal = g_dbus_connection_signal_subscribe(p->bus, FPRINT, FPRINT_DEVICE,
		"EnrollStatus", p->device, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_enroll_status, p, NULL);
	if (!p->dialog) {
		open_dialog(p, id);
	} else {
		// Try again: the same dialog from the start
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress), 0);
		show_count(p);
		enroll_status_text(p, "Touch the reader.");
		gtk_button_set_label(GTK_BUTTON(p->cancel), "Cancel");
		gtk_widget_set_visible(p->again, FALSE);
	}
	r = call_sync(p, p->device, FPRINT_DEVICE, "EnrollStart", g_variant_new("(s)", id), NULL,
		&error);
	if (!r) {
		finish_dialog(p, error->message, false);
		g_error_free(error);
		return;
	}
	g_variant_unref(r);
}

/* ---------- the page ---------- */

static void on_switch(GObject *sw, GParamSpec *pspec, gpointer data) {
	char *path = switch_path();
	if (gtk_switch_get_active(GTK_SWITCH(sw))) {
		g_unlink(path);
	} else {
		char *dir = g_path_get_dirname(path);
		g_mkdir_with_parents(dir, 0755);
		g_free(dir);
		g_file_set_contents(path, "no\n", -1, NULL);
	}
	g_free(path);
}

static void rebuild(struct fp_page *p) {
	if (p->box) {
		gtk_box_remove(GTK_BOX(p->content), p->box);
	}
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_append(GTK_BOX(p->content), p->box);
	p->finger_dd = p->add = NULL;

	if (!p->device) {
		GtkWidget *group = ui_group(p->box, "Reader", NULL);
		char *list = g_find_program_in_path("fprintd-list");
		bool fprintd = list || g_file_test("/usr/libexec/fprintd", G_FILE_TEST_EXISTS) ||
			g_file_test("/usr/lib/fprintd", G_FILE_TEST_EXISTS);
		g_free(list);
		ui_empty_row(group, "auth-fingerprint-symbolic", "No fingerprint reader",
			fprintd ? "fprintd finds no fingerprint reader it can use on this computer." :
			"Fingerprints need fprintd: install it (fprintd in most distributions) and "
			"open this page again.");
		return;
	}

	GtkWidget *reader = ui_group(p->box, "Reader", NULL);
	GtkWidget *name = gtk_label_new(p->reader_name ? p->reader_name : "Fingerprint reader");
	gtk_widget_add_css_class(name, "dim-label");
	ui_row(reader, "Fingerprint reader", NULL, name);
	GtkWidget *sw = gtk_switch_new();
	char *path = switch_path();
	char *text = NULL;
	bool off = g_file_get_contents(path, &text, NULL, NULL) && g_str_has_prefix(text, "no");
	g_free(text);
	g_free(path);
	gtk_switch_set_active(GTK_SWITCH(sw), !off);
	g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch), NULL);
	ui_row(reader, "Unlock the lock screen with a fingerprint",
		"The password works as well. The login screen asks for the password.", sw);

	GtkWidget *list = ui_group(p->box, "Fingerprints", NULL);
	if (p->enrolled->len == 0) {
		ui_empty_row(list, "auth-fingerprint-symbolic", "No fingerprints yet",
			"Add one below to unlock with a finger.");
	}
	for (guint i = 0; i < p->enrolled->len; i++) {
		const char *id = p->enrolled->pdata[i];
		GtkWidget *remove = gtk_button_new_with_label("Remove");
		g_object_set_data_full(G_OBJECT(remove), "finger", g_strdup(id), g_free);
		g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), p);
		ui_row(list, finger_name(id), NULL, remove);
	}

	GtkStringList *free_fingers = gtk_string_list_new(NULL);
	for (size_t i = 0; i < G_N_ELEMENTS(fingers); i++) {
		if (!is_enrolled(p, fingers[i].id)) {
			gtk_string_list_append(free_fingers, fingers[i].name);
		}
	}
	if (g_list_model_get_n_items(G_LIST_MODEL(free_fingers)) > 0) {
		GtkWidget *add_group = ui_group(p->box, "Add a fingerprint", NULL);
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		p->finger_dd = gtk_drop_down_new(G_LIST_MODEL(free_fingers), NULL);
		gtk_box_append(GTK_BOX(row), p->finger_dd);
		p->add = gtk_button_new_with_label("Add...");
		g_signal_connect(p->add, "clicked", G_CALLBACK(on_add), p);
		gtk_box_append(GTK_BOX(row), p->add);
		ui_row(add_group, "Finger", "Which finger to add", row);
	} else {
		g_object_unref(free_fingers);
	}
}

static void on_destroy(GtkWidget *widget, gpointer data) {
	struct fp_page *p = data;
	enroll_end(p);
	if (p->dialog) {
		gtk_window_destroy(GTK_WINDOW(p->dialog));
	}
	g_clear_object(&p->bus);
	g_free(p->device);
	g_free(p->reader_name);
	g_free(p->finger);
	g_ptr_array_free(p->enrolled, TRUE);
	g_free(p);
}

static void on_map(GtkWidget *widget, gpointer data) {
	struct fp_page *p = data;
	if (!p->busy) {
		load(p); // fingers added or removed elsewhere (fprintd-enroll)
		rebuild(p);
	}
}

GtkWidget *fingerprint_page_new(struct settings *s) {
	struct fp_page *p = g_new0(struct fp_page, 1);
	p->s = s;
	p->enrolled = g_ptr_array_new_with_free_func(g_free);
	const char *which = g_getenv("TILEWIN_FPRINT_BUS");
	p->bus = g_bus_get_sync(which && strcmp(which, "session") == 0 ? G_BUS_TYPE_SESSION :
		G_BUS_TYPE_SYSTEM, NULL, NULL);
	GtkWidget *page = ui_page("Fingerprint",
		"Unlock the lock screen with a finger on the fingerprint reader instead of the password.",
		&p->content);
	g_signal_connect(page, "destroy", G_CALLBACK(on_destroy), p);
	g_signal_connect(page, "map", G_CALLBACK(on_map), p);
	load(p);
	rebuild(p);
	return page;
}
