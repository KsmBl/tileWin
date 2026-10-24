/*
 * Properties of a desktop icon, like the Properties dialog of Windows: what it
 * is, where it is, how big it is, what is in it and when it changed. For a
 * shortcut (a Type=Link or Type=Application .desktop file) it tells about what
 * the shortcut opens, and names the shortcut file itself at the bottom. The
 * size of a folder is counted on a thread of its own and fills in as it goes.
 */
#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <grp.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "popup.h"
#include "stringop.h"
#include "textfield.h"
#include "tw_desktop.h"

#define ROWS_MAX 16
#define LABEL_W 120
#define VALUE_LINES 3

enum {
	PROPS_HS_OK = 1,
	PROPS_HS_LOCATION,
};

/* What a folder holds, counted on a thread; shared until both sides let go. */
struct du {
	gint refs;
	gint cancel;
	GMutex lock;
	guint64 bytes, disk, files, dirs;
	bool done;
	char *path;
};

struct props_row {
	char *label;
	char *value;
	int height;
};

struct props_dialog {
	char *title;
	char **icons;
	char *location; // the folder "Open location" opens, or NULL
	struct props_row rows[ROWS_MAX];
	int count;
	int separator; // the rows from here on are about the shortcut file
	int size_row, contains_row; // filled in by the count, or -1
	struct du *du;
	struct loop_timer *timer;
	double px, py;
	bool inside;
};

/* ---------- counting ---------- */

static void du_unref(struct du *du) {
	if (g_atomic_int_dec_and_test(&du->refs)) {
		g_mutex_clear(&du->lock);
		free(du->path);
		free(du);
	}
}

/* Walks the tree without following links, one folder at a time. */
static gpointer du_thread(gpointer data) {
	struct du *du = data;
	GPtrArray *todo = g_ptr_array_new();
	g_ptr_array_add(todo, strdup(du->path));
	while (todo->len && !g_atomic_int_get(&du->cancel)) {
		char *dir = g_ptr_array_remove_index(todo, todo->len - 1);
		int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
		if (!d && fd >= 0) {
			close(fd);
		}
		struct dirent *de;
		while (d && (de = readdir(d)) && !g_atomic_int_get(&du->cancel)) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			struct stat st;
			if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
				continue;
			}
			g_mutex_lock(&du->lock);
			du->bytes += st.st_size;
			du->disk += (guint64)st.st_blocks * 512;
			if (S_ISDIR(st.st_mode)) {
				du->dirs++;
			} else {
				du->files++;
			}
			g_mutex_unlock(&du->lock);
			if (S_ISDIR(st.st_mode)) {
				g_ptr_array_add(todo, format_str("%s/%s", dir, de->d_name));
			}
		}
		if (d) {
			closedir(d);
		}
		free(dir);
	}
	g_ptr_array_set_free_func(todo, free);
	g_ptr_array_free(todo, TRUE);
	g_mutex_lock(&du->lock);
	du->done = true;
	g_mutex_unlock(&du->lock);
	du_unref(du);
	return NULL;
}

static struct du *du_start(const char *path) {
	struct du *du = calloc(1, sizeof(*du));
	du->refs = 2; // the dialog and the thread
	du->path = strdup(path);
	g_mutex_init(&du->lock);
	GThread *thread = g_thread_try_new("tw-du", du_thread, du, NULL);
	if (!thread) {
		du->done = true;
		du->refs = 1;
		return du;
	}
	g_thread_unref(thread);
	return du;
}

/* ---------- the values ---------- */

/* 1234567 as "1,234,567". */
static char *grouped(guint64 n) {
	char digits[32];
	snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, n);
	GString *out = g_string_new(NULL);
	size_t len = strlen(digits);
	for (size_t i = 0; i < len; i++) {
		if (i && (len - i) % 3 == 0) {
			g_string_append_c(out, ',');
		}
		g_string_append_c(out, digits[i]);
	}
	return g_string_free(out, FALSE);
}

/* "1.2 MB (1,234,567 bytes)", the way Windows writes a size. */
static char *size_text(guint64 bytes) {
	char *nice = g_format_size(bytes);
	char *exact = grouped(bytes);
	char *text = format_str("%s (%s byte%s)", nice, exact, bytes == 1 ? "" : "s");
	g_free(nice);
	g_free(exact);
	return text;
}

static char *time_text(time_t when) {
	char buf[64];
	struct tm tm;
	localtime_r(&when, &tm);
	strftime(buf, sizeof(buf), "%A, %d %B %Y, %H:%M:%S", &tm);
	return strdup(buf);
}

static char *mode_text(const struct stat *st) {
	char bits[10];
	const char *flags = "rwxrwxrwx";
	for (int i = 0; i < 9; i++) {
		bits[i] = st->st_mode & (0400 >> i) ? flags[i] : '-';
	}
	bits[9] = '\0';
	struct passwd *pw = getpwuid(st->st_uid);
	struct group *gr = getgrgid(st->st_gid);
	char *owner = pw ? strdup(pw->pw_name) : format_str("%u", (unsigned)st->st_uid);
	char *group = gr ? strdup(gr->gr_name) : format_str("%u", (unsigned)st->st_gid);
	char *text = format_str("%s (%03o), owner %s, group %s", bits, st->st_mode & 0777,
		owner, group);
	free(owner);
	free(group);
	return text;
}

static void add_row(struct props_dialog *d, const char *label, char *value) {
	if (!value || d->count >= ROWS_MAX) {
		free(value);
		return;
	}
	d->rows[d->count].label = strdup(label);
	d->rows[d->count].value = value;
	d->count++;
}

static void set_row(struct props_dialog *d, int row, char *value) {
	if (row < 0) {
		free(value);
		return;
	}
	free(d->rows[row].value);
	d->rows[row].value = value;
}

/* "Free 12.3 GB of 500.1 GB" for the file system a path is on. */
static char *disk_text(const char *path) {
	struct statvfs vfs;
	if (statvfs(path, &vfs) != 0 || vfs.f_blocks == 0) {
		return NULL;
	}
	char *free_text = g_format_size((guint64)vfs.f_bavail * vfs.f_frsize);
	char *total = g_format_size((guint64)vfs.f_blocks * vfs.f_frsize);
	char *text = format_str("%s free of %s", free_text, total);
	g_free(free_text);
	g_free(total);
	return text;
}

static char *mount_text(const char *path) {
	GFile *file = g_file_new_for_path(path);
	GMount *mount = g_file_find_enclosing_mount(file, NULL, NULL);
	char *text = NULL;
	if (mount) {
		char *name = g_mount_get_name(mount);
		GFile *root = g_mount_get_root(mount);
		char *root_path = g_file_get_path(root);
		text = format_str("%s (%s)", name, root_path ? root_path : "");
		g_free(root_path);
		g_object_unref(root);
		g_free(name);
		g_object_unref(mount);
	}
	g_object_unref(file);
	return text;
}

/* The content type of a file, from its name and its first bytes. */
static char *content_type_of(const char *path, const struct stat *st) {
	if (S_ISDIR(st->st_mode)) {
		return g_strdup("inode/directory");
	}
	guchar head[4096];
	gsize got = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd >= 0 && S_ISREG(st->st_mode)) {
		ssize_t n = read(fd, head, sizeof(head));
		got = n > 0 ? (gsize)n : 0;
	}
	if (fd >= 0) {
		close(fd);
	}
	return g_content_type_guess(path, got ? head : NULL, got, NULL);
}

/* "Text document (text/plain)", or "Folder". */
static char *type_text(const char *type) {
	if (strcmp(type, "inode/directory") == 0) {
		return strdup("Folder");
	}
	char *desc = g_content_type_get_description(type);
	char *mime = g_content_type_get_mime_type(type);
	char *text = mime && desc && strcmp(desc, mime) != 0 ?
		format_str("%s (%s)", desc, mime) : strdup(mime ? mime : desc ? desc : "File");
	g_free(mime);
	g_free(desc);
	return text;
}

/* The program a double-click starts for this kind of file, by its name. */
static char *app_for_type(const char *type) {
	GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
	if (!app) {
		return strdup("No program is set for this kind of file");
	}
	const char *id = g_app_info_get_id(app);
	char *text = id ? format_str("%s (%s)", g_app_info_get_display_name(app), id) :
		strdup(g_app_info_get_display_name(app));
	g_object_unref(app);
	return text;
}

/*
 * The rows about a file or folder on this computer: the icon itself, or what
 * a shortcut opens. A folder is counted in the background.
 */
static void add_local_rows(struct props_dialog *d, const char *path, bool target) {
	struct stat st, lst;
	if (stat(path, &st) != 0) {
		add_row(d, target ? "Target" : "Location", strdup(path));
		add_row(d, "Status", strdup(target ?
			"What this shortcut points to is not there (moved, deleted, or a drive that is not connected)" :
			"Not there any more"));
		return;
	}
	char *type = content_type_of(path, &st);
	if (target) {
		add_row(d, "Target", strdup(path));
		add_row(d, "Target type", type ? type_text(type) : NULL);
	} else {
		add_row(d, "Type", type ? type_text(type) : strdup("File"));
	}
	add_row(d, "Opens with", type ? app_for_type(type) : NULL);
	g_free(type);
	char *parent = g_path_get_dirname(path);
	add_row(d, "Location", strdup(parent));
	free(d->location);
	d->location = strdup(parent);
	g_free(parent);
	if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
		char *real = g_file_read_link(path, NULL);
		add_row(d, "Links to", strdup(real ? real : "?"));
		g_free(real);
	}
	if (S_ISDIR(st.st_mode)) {
		d->size_row = d->count;
		add_row(d, "Size", strdup("Counting..."));
		d->contains_row = d->count;
		add_row(d, "Contains", strdup("Counting..."));
		d->du = du_start(path);
	} else {
		add_row(d, "Size", size_text(st.st_size));
		add_row(d, "Size on disk", size_text((guint64)st.st_blocks * 512));
	}
	add_row(d, "Drive", mount_text(path));
	add_row(d, "Disk space", disk_text(path));
	add_row(d, "Modified", time_text(st.st_mtime));
	add_row(d, "Accessed", time_text(st.st_atime));
	add_row(d, "Permissions", mode_text(&st));
}

/* The folder the trash keeps its files in. */
static char *trash_files_dir(void) {
	return format_str("%s/Trash/files", g_get_user_data_dir());
}

/* The program a URL opens with, by its name: "failBrauwser". */
static char *opens_with(const char *url) {
	char *scheme = g_uri_parse_scheme(url);
	GAppInfo *app = scheme ? g_app_info_get_default_for_uri_scheme(scheme) : NULL;
	if (!app) {
		app = g_app_info_get_default_for_type("inode/directory", FALSE);
	}
	char *name = app ? strdup(g_app_info_get_display_name(app)) : NULL;
	g_clear_object(&app);
	g_free(scheme);
	return name;
}

static void add_link_rows(struct props_dialog *d, const struct tw_desktop_entry *e) {
	const char *url = e->url ? e->url : "";
	char *full = strncmp(url, "trash:", 6) == 0 ? strdup("trash:///") : strdup(url);
	char *path = g_filename_from_uri(full, NULL, NULL);
	char *scheme = g_uri_parse_scheme(full);
	if (path) {
		struct stat st;
		bool there = stat(path, &st) == 0;
		add_row(d, "Type", strdup(!there ? "Link" : S_ISDIR(st.st_mode) ?
			"Link to a folder" : "Link to a file"));
		add_local_rows(d, path, true);
	} else if (scheme && strcmp(scheme, "trash") == 0) {
		add_row(d, "Type", strdup("Link to the trash"));
		add_row(d, "Target", strdup(full));
		char *files = trash_files_dir();
		d->size_row = d->count;
		add_row(d, "Size", strdup("Counting..."));
		d->contains_row = d->count;
		add_row(d, "Contains", strdup("Counting..."));
		d->du = du_start(files);
		add_row(d, "Kept in", files);
		add_row(d, "Opens with", opens_with(full));
	} else {
		const char *kind = "Link to a place";
		if (!scheme) {
			kind = "Link";
		} else if (strcmp(scheme, "mtp") == 0 || strcmp(scheme, "gphoto2") == 0 ||
				strcmp(scheme, "afc") == 0) {
			kind = "Link to a phone or camera";
		} else if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0) {
			kind = "Link to a web page";
		} else if (strcmp(scheme, "smb") == 0 || strcmp(scheme, "sftp") == 0 ||
				strcmp(scheme, "ftp") == 0 || strcmp(scheme, "dav") == 0 ||
				strcmp(scheme, "davs") == 0 || strcmp(scheme, "nfs") == 0) {
			kind = "Link to a network share";
		}
		add_row(d, "Type", strdup(kind));
		add_row(d, "Target", g_uri_unescape_string(full, NULL));
		add_row(d, "Opens with", opens_with(full));
	}
	g_free(scheme);
	g_free(path);
	free(full);
}

static void add_app_rows(struct props_dialog *d, const struct tw_desktop_entry *e) {
	add_row(d, "Type", strdup("Program shortcut"));
	add_row(d, "Command", e->exec ? strdup(e->exec) : NULL);
	if (e->exec) {
		char *cmd = tw_desktop_exec_command(e);
		int argc = 0;
		char **argv = NULL;
		if (cmd && g_shell_parse_argv(cmd, &argc, &argv, NULL) && argc > 0) {
			char *program = g_find_program_in_path(argv[0]);
			add_row(d, "Program", program ? strdup(program) :
				format_str("%s (not found)", argv[0]));
			g_free(program);
		}
		g_strfreev(argv);
		free(cmd);
	}
	add_row(d, "Comment", e->comment ? strdup(e->comment) : NULL);
	if (e->terminal) {
		add_row(d, "Runs", strdup("In a terminal"));
	}
	add_row(d, "Icon", e->icon ? strdup(e->icon) : NULL);
}

/* ---------- the dialog ---------- */

static const char *font_of(struct panel *panel) {
	return tw_theme_str(panel->theme, "menu.font", bar_font(panel));
}

static void measure_rows(struct panel *panel, struct props_dialog *d, int value_w) {
	cairo_t *cr = popup_scratch_cairo();
	for (int i = 0; i < d->count; i++) {
		int h = pd_text_wrapped(cr, font_of(panel), d->rows[i].value, 0, 0, value_w,
			VALUE_LINES, 0, false);
		d->rows[i].height = h > 20 ? h + 2 : 22;
	}
}

static void du_poll(void *data) {
	struct popup *p = data;
	struct props_dialog *d = p->data;
	d->timer = NULL;
	g_mutex_lock(&d->du->lock);
	guint64 bytes = d->du->bytes, disk = d->du->disk;
	guint64 files = d->du->files, dirs = d->du->dirs;
	bool done = d->du->done;
	g_mutex_unlock(&d->du->lock);
	char *size = size_text(bytes);
	char *on_disk = g_format_size(disk);
	set_row(d, d->size_row, format_str("%s%s, %s on disk", done ? "" : "Counting... ",
		size, on_disk));
	free(size);
	g_free(on_disk);
	char *f = grouped(files), *g = grouped(dirs);
	set_row(d, d->contains_row, format_str("%s file%s, %s folder%s", f, files == 1 ? "" : "s",
		g, dirs == 1 ? "" : "s"));
	g_free(f);
	g_free(g);
	popup_set_dirty(p);
	if (!done) {
		d->timer = loop_add_timer(p->panel->loop, 250, du_poll, p);
	}
}

static bool props_hover(struct props_dialog *d, double x, double y, double w, double h) {
	return d->inside && d->px >= x && d->px < x + w && d->py >= y && d->py < y + h;
}

static void props_render(struct popup *p, cairo_t *cr) {
	struct props_dialog *d = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	int M = popup_shadow_margin(panel);
	int W = p->surface->width, H = p->surface->height;
	popup_draw_frame(panel, cr, W, H, M, "menu");

	uint32_t fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	uint32_t bg = tw_theme_color(t, "menu.bg", 0xf2f2f2ff);
	int luma = (int)((bg >> 24 & 0xff) * 299 + (bg >> 16 & 0xff) * 587 + (bg >> 8 & 0xff) * 114) / 1000;
	bool dark = luma < 128;
	uint32_t dim = tw_theme_color(t, "menu.disabled_fg", dark ? 0xb0b0b0ff : 0x606060ff);
	uint32_t line = dark ? 0xffffff30 : 0x00000030;
	const char *font = font_of(panel);

	int x0 = M + 18, cw = W - 2 * M - 36, y = M + 14;
	cairo_surface_t *icon = NULL;
	for (int k = 0; d->icons && d->icons[k] && !icon; k++) {
		if (tw_icon_theme_has(d->icons[k])) {
			icon = apps_icon(panel, d->icons[k], 32 * p->surface->scale);
		}
	}
	for (int k = 0; d->icons && d->icons[k] && !icon; k++) {
		icon = apps_icon(panel, d->icons[k], 32 * p->surface->scale);
	}
	pd_icon(cr, icon, x0, y, 32);
	pd_text(cr, bar_bold_font(panel), d->title, x0 + 44, y, cw - 44, 32, fg, PD_LEFT);
	y += 44;
	pd_rect(cr, x0, y, cw, 1, line);
	y += 10;

	int vx = x0 + LABEL_W, vw = cw - LABEL_W;
	for (int i = 0; i < d->count; i++) {
		if (i == d->separator) {
			y += 4;
			pd_rect(cr, x0, y, cw, 1, line);
			y += 9;
		}
		pd_text(cr, font, d->rows[i].label, x0, y, LABEL_W - 8, 20, dim, PD_LEFT);
		pd_text_wrapped(cr, font, d->rows[i].value, vx, y + 1, vw, VALUE_LINES, fg, true);
		y += d->rows[i].height;
	}

	int bh = 30, by = H - M - bh - 14;
	uint32_t accent = style == PS_CLASSIC ? 0x000080ff :
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff);
	const struct {
		int x, w;
		const char *label;
		bool primary;
		int kind;
	} buttons[] = {
		{ W - M - 18 - 90, 90, "OK", true, PROPS_HS_OK },
		{ x0, 140, "Open location", false, PROPS_HS_LOCATION },
	};
	for (size_t i = 0; i < (d->location ? 2 : 1); i++) {
		int bx = buttons[i].x, bw = buttons[i].w;
		bool hover = props_hover(d, bx, by, bw, bh);
		if (style == PS_CLASSIC) {
			pd_rect(cr, bx, by, bw, bh, 0xc0c0c0ff);
			pd_bevel(cr, bx, by, bw, bh, false);
			pd_text(cr, buttons[i].primary ? bar_bold_font(panel) : font, buttons[i].label,
				bx, by, bw, bh, 0x000000ff, PD_CENTER);
		} else {
			cairo_new_path(cr);
			pd_rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 4);
			uint32_t overlay = dark ? 0xffffff00 : 0x00000000;
			pd_color(cr, buttons[i].primary ? accent : overlay | (hover ? 0x30 : 0x14));
			cairo_fill_preserve(cr);
			pd_color(cr, overlay | 0x30);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			pd_text(cr, font, buttons[i].label, bx, by, bw, bh,
				buttons[i].primary ? 0xffffffff : fg, PD_CENTER);
		}
		psurface_add_hotspot(p->surface, bx, by, bw, bh, NULL, buttons[i].kind, 0, NULL);
	}
}

static void props_motion(struct popup *p, double x, double y) {
	struct props_dialog *d = p->data;
	d->px = x;
	d->py = y;
	d->inside = true;
	popup_set_dirty(p);
}

static void props_leave(struct popup *p) {
	struct props_dialog *d = p->data;
	d->inside = false;
	popup_set_dirty(p);
}

static void open_location(struct popup *p) {
	struct props_dialog *d = p->data;
	char *quoted = g_shell_quote(d->location);
	ipc_panel_commandf(p->panel, "exec xdg-open %s", quoted);
	g_free(quoted);
	popup_close_later(p->panel);
}

static void props_button(struct popup *p, double x, double y, uint32_t button, bool pressed) {
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct props_dialog *d = p->data;
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == PROPS_HS_OK) {
		popup_close_later(p->panel);
	} else if (hs && hs->kind == PROPS_HS_LOCATION && d->location) {
		open_location(p);
	}
}

static void props_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	switch (sym) {
	case XKB_KEY_Escape:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		popup_close_later(p->panel);
		break;
	default:
		break;
	}
}

static void props_destroy(struct popup *p) {
	struct props_dialog *d = p->data;
	if (d->timer) {
		loop_remove_timer(p->panel->loop, d->timer);
	}
	if (d->du) {
		g_atomic_int_set(&d->du->cancel, 1);
		du_unref(d->du);
	}
	for (int i = 0; i < d->count; i++) {
		free(d->rows[i].label);
		free(d->rows[i].value);
	}
	g_strfreev(d->icons);
	free(d->location);
	free(d->title);
	free(d);
}

static const struct popup_vtable props_vtable = {
	.render = props_render,
	.motion = props_motion,
	.leave = props_leave,
	.button = props_button,
	.key = props_key,
	.destroy = props_destroy,
};

void deskprops_open(struct panel *panel, struct panel_output *output, const char *path,
		const char *label, char *const *icons) {
	if (!output) {
		return;
	}
	struct props_dialog *d = calloc(1, sizeof(*d));
	d->title = strdup(label);
	d->icons = g_strdupv((char **)icons);
	d->size_row = d->contains_row = -1;
	size_t len = strlen(path);
	struct tw_desktop_entry *entry = len > 8 && strcmp(path + len - 8, ".desktop") == 0 ?
		tw_desktop_load(path, strrchr(path, '/') + 1) : NULL;
	if (entry && entry->link) {
		add_link_rows(d, entry);
	} else if (entry) {
		add_app_rows(d, entry);
	} else {
		add_local_rows(d, path, false);
	}
	d->separator = d->count;
	if (entry) {
		struct stat st;
		add_row(d, "Shortcut file", strdup(path));
		if (stat(path, &st) == 0) {
			add_row(d, "Shortcut size", size_text(st.st_size));
			add_row(d, "Shortcut changed", time_text(st.st_mtime));
		}
		tw_desktop_entry_free(entry);
	}

	int M = popup_shadow_margin(panel);
	int width = 560 + 2 * M;
	measure_rows(panel, d, 560 - 36 - LABEL_W);
	int rows_h = 0;
	for (int i = 0; i < d->count; i++) {
		rows_h += d->rows[i].height;
	}
	int height = 14 + 44 + 10 + rows_h + (d->separator < d->count ? 14 : 0) + 20 + 30 + 14;
	height = height > output->height - 40 ? output->height - 40 : height;
	height += 2 * M;
	struct popup *p = popup_create(panel, POPUP_DIALOG, NULL, output, (output->width - width) / 2,
		(output->height - height) / 3, width, height, &props_vtable, d);
	if (p && d->du) {
		d->timer = loop_add_timer(panel->loop, 100, du_poll, p);
	}
}

/* ---------- open with ---------- */

/*
 * The file or folder on this computer an icon opens: the icon itself, or what
 * a Type=Link shortcut points to. NULL for programs and places elsewhere.
 */
char *deskprops_local_target(const char *path) {
	size_t len = strlen(path);
	if (len <= 8 || strcmp(path + len - 8, ".desktop") != 0) {
		return strdup(path);
	}
	struct tw_desktop_entry *e = tw_desktop_load(path, strrchr(path, '/') + 1);
	char *target = NULL;
	if (e && e->link && e->url) {
		char *local = g_filename_from_uri(e->url, NULL, NULL);
		target = local ? strdup(local) : NULL;
		g_free(local);
	} else if (!e) {
		target = strdup(path); // a broken .desktop file is a text file
	}
	if (e) {
		tw_desktop_entry_free(e);
	}
	return target;
}

char *deskprops_content_type(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? content_type_of(path, &st) : NULL;
}

void deskprops_open_with(struct panel *panel, const char *path, const char *app_id) {
	char *target = deskprops_local_target(path);
	GDesktopAppInfo *app = target ? g_desktop_app_info_new(app_id) : NULL;
	const char *exec = app ? g_app_info_get_commandline(G_APP_INFO(app)) : NULL;
	if (exec) {
		char *uri = g_filename_to_uri(target, NULL, NULL);
		char *cmd = jumplist_exec_with(exec, target, uri ? uri : target);
		ipc_panel_commandf(panel, "exec %s", cmd);
		free(cmd);
		g_free(uri);
	}
	g_clear_object(&app);
	free(target);
}
