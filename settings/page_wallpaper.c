#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "settings.h"
#include "tw_paths.h"
#include "tw_theme.h"

enum {
	SCOPE_EACH,
	SCOPE_ALL,
};

enum {
	TYPE_SOLID,
	TYPE_GRADIENT,
	TYPE_IMAGE,
	TYPE_NONE,
};

static const char *const type_names[] = { "solid", "gradient", "image", "none", NULL };
static const char *const fit_names[] = { "fill", "fit", "stretch", "center", NULL };
static const char *const image_exts[] = { "jpg", "jpeg", "png", "webp", "svg", NULL };

/* Basename of the drop-in picture used for all themes. */
#define ALL_THEMES "all-themes"

struct wallpaper_page {
	struct settings *s;
	bool updating;
	GtkWidget *preview, *preview_caption;
	GtkWidget *scope_dd;
	GtkWidget *all_group, *themes_group, *themes_list;
	GtkWidget *type_dd, *color1, *color2, *dir_dd, *image_button, *fit_dd, *bg;
	GtkWidget *row_color1, *row_color2, *row_dir, *row_image, *row_fit, *row_bg;
	char *image_path;
};

static int index_of(const char *const *names, const char *value, int fallback) {
	for (int i = 0; value && names[i]; i++) {
		if (g_ascii_strcasecmp(names[i], value) == 0) {
			return i;
		}
	}
	return fallback;
}

static void set_color(GtkWidget *button, const char *color, const char *fallback) {
	GdkRGBA rgba;
	if (!color || !gdk_rgba_parse(&rgba, color)) {
		gdk_rgba_parse(&rgba, fallback);
	}
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
}

static char *get_color(GtkWidget *button) {
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
	return g_strdup_printf("#%02x%02x%02x", (int)(c->red * 255 + 0.5),
		(int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
}

static guint selected(GtkWidget *dropdown) {
	return gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
}

static void update_preview(struct wallpaper_page *p) {
	char *theme = settings_current_theme();
	struct cstmt *st = confdoc_child(p->s->common->root, "wallpaper", NULL);
	char *args = cstmt_raw_args(p->s->common, st);
	ui_wallpaper_picture_fill(p->preview, args, theme, 384, 240);
	char *caption = g_strdup_printf("Preview with the current theme (%s)", theme);
	gtk_label_set_text(GTK_LABEL(p->preview_caption), caption);
	g_free(caption);
	g_free(args);
	g_free(theme);
}

static void update_visibility(struct wallpaper_page *p) {
	bool all = selected(p->scope_dd) == SCOPE_ALL;
	guint type = selected(p->type_dd);
	gtk_widget_set_visible(p->all_group, all);
	gtk_widget_set_visible(p->themes_group, !all);
	gtk_widget_set_visible(p->row_color1, type == TYPE_SOLID || type == TYPE_GRADIENT);
	gtk_widget_set_visible(p->row_color2, type == TYPE_GRADIENT);
	gtk_widget_set_visible(p->row_dir, type == TYPE_GRADIENT);
	gtk_widget_set_visible(p->row_image, type == TYPE_IMAGE);
	gtk_widget_set_visible(p->row_fit, type == TYPE_IMAGE);
	gtk_widget_set_visible(p->row_bg, type == TYPE_IMAGE);
}

static void apply(struct wallpaper_page *p) {
	if (p->updating) {
		return;
	}
	update_visibility(p);
	char *args = NULL;
	if (selected(p->scope_dd) == SCOPE_EACH) {
		args = g_strdup("theme");
	} else {
		char *c1 = get_color(p->color1), *c2 = get_color(p->color2), *bg = get_color(p->bg);
		switch (selected(p->type_dd)) {
		case TYPE_SOLID:
			args = g_strdup_printf("solid %s", c1);
			break;
		case TYPE_GRADIENT:
			args = g_strdup_printf("gradient %s %s %s", c1, c2,
				selected(p->dir_dd) == 1 ? "horizontal" : "vertical");
			break;
		case TYPE_IMAGE:
			if (p->image_path) {
				args = g_strdup_printf(strchr(p->image_path, ' ') ? "image \"%s\" %s %s" :
					"image %s %s %s", p->image_path, fit_names[selected(p->fit_dd)], bg);
			} else {
				settings_status(p->s, "Choose a picture to use it as the wallpaper");
			}
			break;
		default:
			args = g_strdup("none");
			break;
		}
		g_free(c1);
		g_free(c2);
		g_free(bg);
	}
	if (args) {
		confdoc_set(p->s->common, p->s->common->root, "wallpaper", NULL, args);
		settings_common_changed(p->s, false);
		settings_command(p->s, "wallpaper %s", args);
		update_preview(p);
		g_free(args);
	}
}

static void on_setting_changed(GObject *object, GParamSpec *pspec, gpointer data) {
	apply(data);
}

static void rebuild_theme_rows(struct wallpaper_page *p);

struct choose_request {
	struct wallpaper_page *p;
	char *theme; // NULL for all themes
};

static void choose_request_free(gpointer data, GClosure *closure) {
	struct choose_request *r = data;
	g_free(r->theme);
	g_free(r);
}

/* Copies a picture to ~/.config/tileWin/wallpapers/<basename>.<ext>. */
static char *install_picture(GFile *source, const char *basename, char **error) {
	char *name = g_file_get_basename(source);
	char *dot = strrchr(name, '.');
	char *ext = dot ? g_ascii_strdown(dot + 1, -1) : NULL;
	g_free(name);
	if (!ext || index_of(image_exts, ext, -1) < 0) {
		*error = g_strdup("Unsupported picture type (use JPEG, PNG, WebP or SVG)");
		g_free(ext);
		return NULL;
	}
	char *config_dir = tw_config_dir();
	char *dir = g_build_filename(config_dir, "wallpapers", NULL);
	free(config_dir);
	g_mkdir_with_parents(dir, 0755);
	char *dest = g_strdup_printf("%s/%s.%s", dir, basename, ext);
	char *tmp = g_strdup_printf("%s/.%s.%s.tmp", dir, basename, ext);
	GFile *tmp_file = g_file_new_for_path(tmp);
	GError *gerror = NULL;
	if (g_file_copy(source, tmp_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &gerror)) {
		ui_wallpaper_remove_dropins(basename);
		if (g_rename(tmp, dest) != 0) {
			*error = g_strdup_printf("Could not create %s", dest);
			g_clear_pointer(&dest, g_free);
		}
	} else {
		*error = g_strdup(gerror->message);
		g_clear_pointer(&dest, g_free);
		g_error_free(gerror);
	}
	g_object_unref(tmp_file);
	g_free(tmp);
	g_free(dir);
	g_free(ext);
	return dest;
}

static void picture_theme_changed(struct wallpaper_page *p, const char *theme) {
	char *current = settings_current_theme();
	if (strcmp(current, theme) == 0) {
		settings_command(p->s, "wallpaper theme");
	}
	g_free(current);
	rebuild_theme_rows(p);
	update_preview(p);
	theme_page_refresh(p->s);
}

static void on_picture_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct choose_request *r = data;
	struct wallpaper_page *p = r->p;
	GError *gerror = NULL;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &gerror);
	if (file) {
		char *error = NULL;
		char *dest = install_picture(file, r->theme ? r->theme : ALL_THEMES, &error);
		if (!dest) {
			settings_status(p->s, "%s", error);
		} else if (r->theme) {
			picture_theme_changed(p, r->theme);
			settings_status(p->s, "Wallpaper of %s set to %s", r->theme, dest);
		} else {
			g_free(p->image_path);
			p->image_path = g_strdup(dest);
			char *base = g_path_get_basename(dest);
			gtk_button_set_label(GTK_BUTTON(p->image_button), base);
			g_free(base);
			apply(p);
		}
		g_free(dest);
		g_free(error);
		g_object_unref(file);
	} else if (gerror && !g_error_matches(gerror, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
		settings_status(p->s, "%s", gerror->message);
	}
	g_clear_error(&gerror);
	choose_request_free(r, NULL);
}

static void choose_picture(struct wallpaper_page *p, const char *theme) {
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Choose a wallpaper");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Pictures");
	for (int i = 0; image_exts[i]; i++) {
		gtk_file_filter_add_suffix(filter, image_exts[i]);
	}
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	if (pictures) {
		GFile *folder = g_file_new_for_path(pictures);
		gtk_file_dialog_set_initial_folder(dialog, folder);
		g_object_unref(folder);
	}
	struct choose_request *r = g_new0(struct choose_request, 1);
	r->p = p;
	r->theme = g_strdup(theme);
	gtk_file_dialog_open(dialog, p->s->window, NULL, on_picture_chosen, r);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void on_choose_all(GtkButton *button, gpointer data) {
	choose_picture(data, NULL);
}

static void on_choose_theme(GtkButton *button, gpointer data) {
	struct choose_request *r = data;
	choose_picture(r->p, r->theme);
}

static void on_reset_theme(GtkButton *button, gpointer data) {
	struct choose_request *r = data;
	ui_wallpaper_remove_dropins(r->theme);
	settings_status(r->p->s, "%s uses its default wallpaper again", r->theme);
	char *theme = g_strdup(r->theme);
	picture_theme_changed(r->p, theme); // destroys the button and r
	g_free(theme);
}

static void rebuild_theme_rows(struct wallpaper_page *p) {
	gtk_list_box_remove_all(GTK_LIST_BOX(p->themes_list));
	list_t *names = tw_theme_list();
	for (int i = 0; names && i < names->length; i++) {
		const char *name = names->items[i];
		char *error = NULL;
		struct tw_theme *theme = tw_theme_load(name, &error);
		free(error);
		char *dropin = ui_wallpaper_dropin(name);
		char *base = dropin ? g_path_get_basename(dropin) : NULL;
		char *subtitle = dropin ? g_strdup_printf("Own picture (%s)", base) :
			g_strdup("Default wallpaper of the theme");

		GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		GtkWidget *choose = gtk_button_new_with_label("Choose picture…");
		struct choose_request *r = g_new0(struct choose_request, 1);
		r->p = p;
		r->theme = g_strdup(name);
		g_signal_connect_data(choose, "clicked", G_CALLBACK(on_choose_theme), r,
			choose_request_free, 0);
		gtk_box_append(GTK_BOX(buttons), choose);
		GtkWidget *reset = gtk_button_new_with_label("Reset");
		gtk_widget_set_sensitive(reset, dropin != NULL);
		r = g_new0(struct choose_request, 1);
		r->p = p;
		r->theme = g_strdup(name);
		g_signal_connect_data(reset, "clicked", G_CALLBACK(on_reset_theme), r,
			choose_request_free, 0);
		gtk_box_append(GTK_BOX(buttons), reset);

		GtkWidget *row = ui_row(p->themes_list, theme && theme->title ? theme->title : name,
			subtitle, buttons);
		GtkWidget *picture = gtk_picture_new();
		ui_wallpaper_picture_fill(picture, NULL, name, 112, 70);
		gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
		gtk_widget_set_size_request(picture, 112, 70);
		gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
		gtk_widget_add_css_class(picture, "tw-thumb");
		gtk_box_prepend(GTK_BOX(ui_row_box(row)), picture);

		g_free(subtitle);
		g_free(base);
		g_free(dropin);
		if (theme) {
			tw_theme_free(theme);
		}
	}
	if (names) {
		list_free_items_and_destroy(names);
	}
}

void wallpaper_page_refresh(struct settings *s) {
	struct wallpaper_page *p = s->wallpaper_page;
	if (!p) {
		return;
	}
	p->updating = true;
	struct cstmt *st = confdoc_child(s->common->root, "wallpaper", NULL);
	const char *type = cstmt_arg(st, 0);
	bool each = !type || g_ascii_strcasecmp(type, "theme") == 0;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->scope_dd), each ? SCOPE_EACH : SCOPE_ALL);

	char *theme_name = settings_current_theme();
	char *error = NULL;
	struct tw_theme *theme = tw_theme_load(theme_name, &error);
	free(error);
	const char *theme_color = theme ? tw_theme_str(theme, "wallpaper.color", "#3a6ea5") :
		"#3a6ea5";
	const char *theme_color2 = theme ? tw_theme_str(theme, "wallpaper.color2", theme_color) :
		theme_color;

	int type_index = each ? TYPE_SOLID : index_of(type_names, type, TYPE_SOLID);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->type_dd), type_index);
	g_clear_pointer(&p->image_path, g_free);
	if (type_index == TYPE_IMAGE) {
		p->image_path = g_strdup(cstmt_arg(st, 1));
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->fit_dd),
			index_of(fit_names, cstmt_arg(st, 2), 0));
		set_color(p->bg, cstmt_arg(st, 3), "#000000");
		set_color(p->color1, NULL, theme_color);
		set_color(p->color2, NULL, theme_color2);
	} else {
		const char *c1 = each ? NULL : cstmt_arg(st, 1);
		const char *c2 = each ? NULL : cstmt_arg(st, 2);
		set_color(p->color1, c1, theme_color);
		set_color(p->color2, c2 ? c2 : c1, theme_color2);
		const char *dir = each ? NULL : cstmt_arg(st, 3);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->dir_dd),
			dir && g_ascii_strcasecmp(dir, "horizontal") == 0);
		set_color(p->bg, NULL, "#000000");
	}
	char *base = p->image_path ? g_path_get_basename(p->image_path) : NULL;
	gtk_button_set_label(GTK_BUTTON(p->image_button), base ? base : "Choose picture…");
	g_free(base);
	if (theme) {
		tw_theme_free(theme);
	}
	g_free(theme_name);

	update_visibility(p);
	rebuild_theme_rows(p);
	update_preview(p);
	p->updating = false;
}

static GtkWidget *color_button(struct wallpaper_page *p) {
	GtkWidget *button = gtk_color_dialog_button_new(gtk_color_dialog_new());
	g_signal_connect(button, "notify::rgba", G_CALLBACK(on_setting_changed), p);
	return button;
}

static GtkWidget *dropdown(struct wallpaper_page *p, const char *const *strings) {
	GtkWidget *dd = gtk_drop_down_new_from_strings(strings);
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_setting_changed), p);
	return dd;
}

GtkWidget *wallpaper_page_new(struct settings *s) {
	struct wallpaper_page *p = g_new0(struct wallpaper_page, 1);
	p->s = s;
	GtkWidget *content;
	GtkWidget *page = ui_page("Wallpaper",
		"Use the wallpaper that belongs to each theme, your own picture per theme, or one wallpaper for all themes.",
		&content);

	p->preview = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(p->preview), GTK_CONTENT_FIT_COVER);
	gtk_widget_set_size_request(p->preview, 384, 240);
	gtk_widget_set_halign(p->preview, GTK_ALIGN_START);
	gtk_widget_set_overflow(p->preview, GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(p->preview, "tw-thumb");
	gtk_widget_set_margin_top(p->preview, 12);
	gtk_box_append(GTK_BOX(content), p->preview);
	p->preview_caption = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->preview_caption), 0);
	gtk_widget_add_css_class(p->preview_caption, "dim-label");
	gtk_box_append(GTK_BOX(content), p->preview_caption);

	GtkWidget *group = ui_group(content, NULL, NULL);
	p->scope_dd = dropdown(p, (const char *const[]){
		"Each theme has its own wallpaper", "One wallpaper for all themes", NULL });
	ui_row(group, "Wallpaper", NULL, p->scope_dd);

	GtkWidget *all = ui_group(content, "Wallpaper for all themes", NULL);
	p->all_group = gtk_widget_get_parent(all);
	p->type_dd = dropdown(p, (const char *const[]){
		"Solid color", "Gradient", "Picture", "None (drawn by another program)", NULL });
	ui_row(all, "Type", NULL, p->type_dd);
	p->color1 = color_button(p);
	p->row_color1 = ui_row(all, "Color", NULL, p->color1);
	p->color2 = color_button(p);
	p->row_color2 = ui_row(all, "Second color", NULL, p->color2);
	p->dir_dd = dropdown(p, (const char *const[]){ "Top to bottom", "Left to right", NULL });
	p->row_dir = ui_row(all, "Direction", NULL, p->dir_dd);
	p->image_button = gtk_button_new_with_label("Choose picture…");
	g_signal_connect(p->image_button, "clicked", G_CALLBACK(on_choose_all), p);
	p->row_image = ui_row(all, "Picture", "The picture is copied to ~/.config/tileWin/wallpapers.",
		p->image_button);
	p->fit_dd = dropdown(p, (const char *const[]){ "Fill", "Fit", "Stretch", "Center", NULL });
	p->row_fit = ui_row(all, "Fit", NULL, p->fit_dd);
	p->bg = color_button(p);
	p->row_bg = ui_row(all, "Background color", "Visible around pictures that don't fill the screen.",
		p->bg);

	p->themes_list = ui_group(content, "Wallpaper per theme",
		"Pick a picture to replace the default wallpaper of a theme.");
	p->themes_group = gtk_widget_get_parent(p->themes_list);

	s->wallpaper_page = p;
	wallpaper_page_refresh(s);
	return page;
}
