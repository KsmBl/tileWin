#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "settings.h"

/*
 * Account page: the account picture and name. The picture is ~/.face, which
 * the tileWin lock screen and most login screens show (~/.face.icon is linked
 * to it for the ones that look there).
 */

#define FACE_SIZE 256

struct account_page {
	struct settings *s;
	GtkWidget *picture, *remove;
};

static char *home_file(const char *name) {
	return g_build_filename(g_get_home_dir(), name, NULL);
}

static void load_picture(struct account_page *p) {
	char *path = home_file(".face");
	GFile *file = g_file_new_for_path(path);
	GdkTexture *texture = g_file_test(path, G_FILE_TEST_IS_REGULAR) ?
		gdk_texture_new_from_file(file, NULL) : NULL;
	if (texture) {
		gtk_image_set_from_paintable(GTK_IMAGE(p->picture), GDK_PAINTABLE(texture));
		g_object_unref(texture);
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(p->picture), "avatar-default");
	}
	gtk_widget_set_sensitive(p->remove, texture != NULL);
	g_object_unref(file);
	g_free(path);
}

static void picture_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct account_page *p = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file) {
		return;
	}
	char *path = g_file_get_path(file);
	g_object_unref(file);
	GError *error = NULL;
	GdkPixbuf *image = path ? gdk_pixbuf_new_from_file(path, &error) : NULL;
	g_free(path);
	if (!image) {
		settings_status(p->s, "Couldn't open the picture: %s", error ? error->message : "?");
		g_clear_error(&error);
		return;
	}
	GdkPixbuf *rotated = gdk_pixbuf_apply_embedded_orientation(image);
	g_object_unref(image);
	int w = gdk_pixbuf_get_width(rotated), h = gdk_pixbuf_get_height(rotated);
	int side = w < h ? w : h;
	GdkPixbuf *square = gdk_pixbuf_new_subpixbuf(rotated, (w - side) / 2, (h - side) / 2, side,
		side);
	GdkPixbuf *scaled = gdk_pixbuf_scale_simple(square, FACE_SIZE, FACE_SIZE, GDK_INTERP_HYPER);
	char *face = home_file(".face");
	if (gdk_pixbuf_save(scaled, face, "png", &error, NULL)) {
		char *icon = home_file(".face.icon");
		if (!g_file_test(icon, G_FILE_TEST_EXISTS) && !g_file_test(icon, G_FILE_TEST_IS_SYMLINK)) {
			if (symlink(".face", icon) != 0) {
				settings_status(p->s, "The picture is saved, but ~/.face.icon couldn't be linked");
			}
		}
		g_free(icon);
		settings_status(p->s, "Your account picture was changed");
	} else {
		settings_status(p->s, "Couldn't save the picture: %s", error ? error->message : "?");
		g_clear_error(&error);
	}
	g_free(face);
	g_object_unref(scaled);
	g_object_unref(square);
	g_object_unref(rotated);
	load_picture(p);
}

static void on_choose(GtkButton *button, gpointer data) {
	struct account_page *p = data;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Choose an account picture");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Pictures");
	gtk_file_filter_add_mime_type(filter, "image/*");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	char *pictures = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
	if (pictures) {
		GFile *folder = g_file_new_for_path(pictures);
		gtk_file_dialog_set_initial_folder(dialog, folder);
		g_object_unref(folder);
	}
	g_free(pictures);
	gtk_file_dialog_open(dialog, p->s->window, NULL, picture_chosen, p);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void on_remove(GtkButton *button, gpointer data) {
	struct account_page *p = data;
	char *face = home_file(".face");
	char *icon = home_file(".face.icon");
	char *target = g_file_read_link(icon, NULL);
	if (target && strcmp(target, ".face") == 0) {
		g_unlink(icon);
	}
	g_unlink(face);
	g_free(target);
	g_free(icon);
	g_free(face);
	settings_status(p->s, "Your account picture was removed");
	load_picture(p);
}

void account_page_refresh(struct settings *s) {
	if (s->account_page) {
		load_picture(s->account_page);
	}
}

GtkWidget *account_page_new(struct settings *s) {
	struct account_page *p = g_new0(struct account_page, 1);
	p->s = s;
	s->account_page = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Account", "Your picture and name.", &content);

	GtkWidget *group = ui_group(content, NULL, NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	p->picture = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(p->picture), 96);
	gtk_widget_set_overflow(p->picture, GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(p->picture, "tw-avatar");
	gtk_box_append(GTK_BOX(box), p->picture);
	GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
	const char *real = g_get_real_name();
	GtkWidget *name = gtk_label_new(real && strcmp(real, "Unknown") != 0 ? real : g_get_user_name());
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_add_css_class(name, "tw-title");
	gtk_box_append(GTK_BOX(names), name);
	GtkWidget *user = gtk_label_new(g_get_user_name());
	gtk_label_set_xalign(GTK_LABEL(user), 0);
	gtk_widget_add_css_class(user, "dim-label");
	gtk_box_append(GTK_BOX(names), user);
	gtk_box_append(GTK_BOX(box), names);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_append(GTK_LIST_BOX(group), row);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *choose = gtk_button_new_with_label("Choose a picture…");
	g_signal_connect(choose, "clicked", G_CALLBACK(on_choose), p);
	gtk_box_append(GTK_BOX(buttons), choose);
	p->remove = gtk_button_new_with_label("Remove");
	g_signal_connect(p->remove, "clicked", G_CALLBACK(on_remove), p);
	gtk_box_append(GTK_BOX(buttons), p->remove);
	ui_row(group, "Account picture", "Shown on the lock screen and the login screen", buttons);

	load_picture(p);
	return page;
}
