#define _GNU_SOURCE
#include <dirent.h>
#include <json.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "settings.h"
#include "tw_paths.h"
#include "tw_theme.h"

/*
 * About page: what a fetch tool prints in a terminal, laid out as a page
 * instead. The logo keeps the character of one -- it is drawn from characters,
 * in the accent colour -- and everything beside it is an ordinary row, so it
 * can be read, selected and copied like the rest of the settings.
 *
 * Nothing here shells out to fastfetch or neofetch: every line is read from
 * /proc, /sys, /etc/os-release or the compositor, so the page is the same with
 * or without them installed and costs nothing to open.
 */

#define LOGO_COUNT 3

static const char *const logo_names[LOGO_COUNT] = { "normal", "tiny", "uwu" };

/* Four panes, the way tileWin arranges a screen. */
static const char *const logo_normal =
	"   ▄▄▄▄▄▄▄▄▄   ▄▄▄▄▄▄▄▄▄\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"   ▀▀▀▀▀▀▀▀▀   ▀▀▀▀▀▀▀▀▀\n"
	"   ▄▄▄▄▄▄▄▄▄   ▄▄▄▄▄▄▄▄▄\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"  █████████   █████████\n"
	"   ▀▀▀▀▀▀▀▀▀   ▀▀▀▀▀▀▀▀▀";

static const char *const logo_tiny =
	"▛▀▜ ▛▀▜\n"
	"▙▄▟ ▙▄▟\n"
	"▛▀▜ ▛▀▜\n"
	"▙▄▟ ▙▄▟";

/*
 * The uwu logo is tileWin's own, written in the spirit of the fetch tools that
 * do this rather than taken from one: uwufetch is under the GPL and tileWin is
 * under the MIT licence, so its art could not be carried in here even if it
 * were copied faithfully. Ears, a >w< face and a nya are the whole joke.
 */
static const char *const logo_uwu =
	"   /\\   /\\     ▛▀▜ ▛▀▜\n"
	"  (  =^w^=  )  ▙▄▟ ▙▄▟\n"
	"   (  uwu  )   ▛▀▜ ▛▀▜\n"
	"    \\  ~  /    ▙▄▟ ▙▄▟\n"
	"     ^^ ^^     tiwoWin nyaa~";

struct about_page {
	struct settings *s;
	GtkWidget *logo, *logo_dd, *name, *subtitle;
	GString *as_text; // what the copy button hands over
};

/* ---------- reading the machine ---------- */

static char *first_line_of(const char *path) {
	char *text = NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		return NULL;
	}
	g_strstrip(text);
	char *newline = strchr(text, '\n');
	if (newline) {
		*newline = '\0';
	}
	return *text ? text : (g_free(text), NULL);
}

/* A "key=value" or "key: value" out of a file such as os-release or cpuinfo. */
static char *field_of(const char *path, const char *key, char separator) {
	char *text = NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		return NULL;
	}
	char *found = NULL;
	size_t key_len = strlen(key);
	for (char *save = NULL, *line = strtok_r(text, "\n", &save); line && !found;
			line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, key, key_len) != 0) {
			continue;
		}
		char *value = strchr(line + key_len, separator);
		if (!value) {
			continue;
		}
		value++;
		while (*value == ' ' || *value == '\t' || *value == '"') {
			value++;
		}
		size_t n = strlen(value);
		while (n > 0 && (value[n - 1] == '"' || value[n - 1] == ' ')) {
			value[--n] = '\0';
		}
		found = *value ? g_strdup(value) : NULL;
	}
	g_free(text);
	return found;
}

static char *human_bytes(double bytes) {
	static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	size_t unit = 0;
	while (bytes >= 1024 && unit + 1 < G_N_ELEMENTS(units)) {
		bytes /= 1024;
		unit++;
	}
	return g_strdup_printf(bytes < 10 && unit > 1 ? "%.1f %s" : "%.0f %s", bytes, units[unit]);
}

static char *about_uptime(void) {
	char *text = first_line_of("/proc/uptime");
	double seconds = text ? g_ascii_strtod(text, NULL) : 0;
	g_free(text);
	if (seconds < 1) {
		return NULL;
	}
	int days = (int)(seconds / 86400);
	int hours = (int)(seconds / 3600) % 24;
	int minutes = (int)(seconds / 60) % 60;
	if (days) {
		return g_strdup_printf("%d day%s, %d hour%s, %d min", days, days == 1 ? "" : "s",
			hours, hours == 1 ? "" : "s", minutes);
	}
	if (hours) {
		return g_strdup_printf("%d hour%s, %d min", hours, hours == 1 ? "" : "s", minutes);
	}
	return g_strdup_printf("%d min", minutes);
}

/* Counts packages without asking a package manager, which would cost a process. */
static char *about_packages(void) {
	GString *out = g_string_new(NULL);
	GDir *dir = g_dir_open("/var/lib/pacman/local", 0, NULL);
	if (dir) {
		int n = 0;
		while (g_dir_read_name(dir)) {
			n++;
		}
		g_dir_close(dir);
		if (n > 0) {
			g_string_append_printf(out, "%d (pacman)", n - 1); // minus the database
		}
	}
	char *status = NULL;
	if (!out->len && g_file_get_contents("/var/lib/dpkg/status", &status, NULL, NULL)) {
		int n = 0;
		for (const char *p = status; (p = strstr(p, "\nPackage: ")); p++) {
			n++;
		}
		g_free(status);
		if (n > 0) {
			g_string_append_printf(out, "%d (dpkg)", n);
		}
	}
	char *flatpak = g_build_filename(g_get_home_dir(), ".local/share/flatpak/app", NULL);
	for (int i = 0; i < 2; i++) {
		GDir *fp = g_dir_open(i ? flatpak : "/var/lib/flatpak/app", 0, NULL);
		int n = 0;
		while (fp && g_dir_read_name(fp)) {
			n++;
		}
		if (fp) {
			g_dir_close(fp);
		}
		if (n > 0) {
			g_string_append_printf(out, "%s%d (flatpak)", out->len ? ", " : "", n);
		}
	}
	g_free(flatpak);
	return out->len ? g_string_free(out, FALSE) : (g_string_free(out, TRUE), NULL);
}

static char *about_cpu(void) {
	char *model = field_of("/proc/cpuinfo", "model name", ':');
	if (!model) {
		model = field_of("/proc/cpuinfo", "Model", ':'); // arm boards
	}
	if (!model) {
		return NULL;
	}
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	char *out = cores > 0 ? g_strdup_printf("%s (%ld)", model, cores) : g_strdup(model);
	g_free(model);
	return out;
}

/* The name of a graphics card, from the kernel rather than from lspci. */
static char *about_gpu(void) {
	GDir *dir = g_dir_open("/sys/class/drm", 0, NULL);
	const char *name;
	GString *out = g_string_new(NULL);
	while (dir && (name = g_dir_read_name(dir))) {
		if (strncmp(name, "card", 4) != 0 || strchr(name, '-')) {
			continue; // card0-HDMI-A-1 and friends are connectors
		}
		char *path = g_strdup_printf("/sys/class/drm/%s/device/uevent", name);
		char *driver = field_of(path, "DRIVER", '=');
		g_free(path);
		path = g_strdup_printf("/sys/class/drm/%s/device/vendor", name);
		char *vendor = first_line_of(path);
		g_free(path);
		static const struct {
			const char *id, *name;
		} vendors[] = {
			{ "0x1002", "AMD" }, { "0x10de", "NVIDIA" }, { "0x8086", "Intel" },
		};
		const char *maker = NULL;
		for (size_t i = 0; vendor && i < G_N_ELEMENTS(vendors); i++) {
			if (strcmp(vendor, vendors[i].id) == 0) {
				maker = vendors[i].name;
			}
		}
		if (driver || maker) {
			g_string_append_printf(out, "%s%s%s%s", out->len ? ", " : "",
				maker ? maker : "", maker && driver ? " " : "", driver ? driver : "");
		}
		g_free(driver);
		g_free(vendor);
	}
	if (dir) {
		g_dir_close(dir);
	}
	return out->len ? g_string_free(out, FALSE) : (g_string_free(out, TRUE), NULL);
}

static char *about_memory(void) {
	char *total_kb = field_of("/proc/meminfo", "MemTotal", ':');
	char *avail_kb = field_of("/proc/meminfo", "MemAvailable", ':');
	if (!total_kb || !avail_kb) {
		g_free(total_kb);
		g_free(avail_kb);
		return NULL;
	}
	double total = g_ascii_strtod(total_kb, NULL) * 1024;
	double used = total - g_ascii_strtod(avail_kb, NULL) * 1024;
	g_free(total_kb);
	g_free(avail_kb);
	char *used_text = human_bytes(used);
	char *total_text = human_bytes(total);
	char *out = g_strdup_printf("%s of %s (%.0f%%)", used_text, total_text,
		total > 0 ? used * 100 / total : 0);
	g_free(used_text);
	g_free(total_text);
	return out;
}

static char *about_swap(void) {
	char *total_kb = field_of("/proc/meminfo", "SwapTotal", ':');
	char *free_kb = field_of("/proc/meminfo", "SwapFree", ':');
	double total = total_kb ? g_ascii_strtod(total_kb, NULL) * 1024 : 0;
	double used = total - (free_kb ? g_ascii_strtod(free_kb, NULL) * 1024 : 0);
	g_free(total_kb);
	g_free(free_kb);
	if (total < 1) {
		return g_strdup("none");
	}
	char *used_text = human_bytes(used);
	char *total_text = human_bytes(total);
	char *out = g_strdup_printf("%s of %s", used_text, total_text);
	g_free(used_text);
	g_free(total_text);
	return out;
}

static char *about_disk(void) {
	struct statvfs st;
	if (statvfs("/", &st) != 0 || st.f_blocks == 0) {
		return NULL;
	}
	double total = (double)st.f_blocks * st.f_frsize;
	double used = total - (double)st.f_bfree * st.f_frsize;
	char *used_text = human_bytes(used);
	char *total_text = human_bytes(total);
	char *out = g_strdup_printf("%s of %s (%.0f%%)", used_text, total_text,
		used * 100 / total);
	g_free(used_text);
	g_free(total_text);
	return out;
}

static char *about_battery(void) {
	GDir *dir = g_dir_open("/sys/class/power_supply", 0, NULL);
	const char *name;
	char *out = NULL;
	while (dir && !out && (name = g_dir_read_name(dir))) {
		char *path = g_strdup_printf("/sys/class/power_supply/%s/capacity", name);
		char *capacity = first_line_of(path);
		g_free(path);
		if (!capacity) {
			continue;
		}
		path = g_strdup_printf("/sys/class/power_supply/%s/status", name);
		char *status = first_line_of(path);
		g_free(path);
		out = g_strdup_printf("%s%%%s%s", capacity, status ? ", " : "",
			status ? status : "");
		g_free(capacity);
		g_free(status);
	}
	if (dir) {
		g_dir_close(dir);
	}
	return out;
}

/* Every screen the compositor is driving, as "1920x1080 @ 60 Hz". */
static char *about_screens(void) {
	char *json = tw_ipc_request(2 /* IPC_GET_OUTPUTS */, NULL);
	if (!json) {
		return NULL;
	}
	struct json_object *outputs = json_tokener_parse(json);
	free(json);
	GString *out = g_string_new(NULL);
	for (size_t i = 0; outputs && json_object_is_type(outputs, json_type_array) &&
			i < json_object_array_length(outputs); i++) {
		struct json_object *o = json_object_array_get_idx(outputs, i), *mode, *value;
		if (!json_object_object_get_ex(o, "current_mode", &mode)) {
			continue;
		}
		int width = json_object_object_get_ex(mode, "width", &value) ?
			json_object_get_int(value) : 0;
		int height = json_object_object_get_ex(mode, "height", &value) ?
			json_object_get_int(value) : 0;
		int refresh = json_object_object_get_ex(mode, "refresh", &value) ?
			json_object_get_int(value) : 0;
		if (width <= 0 || height <= 0) {
			continue;
		}
		g_string_append_printf(out, "%s%dx%d", out->len ? ", " : "", width, height);
		if (refresh > 0) {
			g_string_append_printf(out, " @ %.0f Hz", refresh / 1000.0);
		}
	}
	if (outputs) {
		json_object_put(outputs);
	}
	return out->len ? g_string_free(out, FALSE) : (g_string_free(out, TRUE), NULL);
}

/* ---------- the logo ---------- */

static char *logo_file(void) {
	char *dir = tw_config_dir();
	char *path = dir ? g_build_filename(dir, "about-logo", NULL) : NULL;
	free(dir);
	return path;
}

static guint logo_index(void) {
	char *path = logo_file();
	char *saved = path ? tw_read_first_line(path) : NULL;
	g_free(path);
	guint index = 0;
	for (guint i = 0; saved && i < LOGO_COUNT; i++) {
		if (g_ascii_strcasecmp(saved, logo_names[i]) == 0) {
			index = i;
		}
	}
	free(saved);
	return index;
}

static const char *logo_art(guint index) {
	return index == 1 ? logo_tiny : index == 2 ? logo_uwu : logo_normal;
}

static void show_logo(struct about_page *p, guint index) {
	char *markup = g_markup_printf_escaped("<span font_family=\"monospace\">%s</span>",
		logo_art(index));
	gtk_label_set_markup(GTK_LABEL(p->logo), markup);
	g_free(markup);
}

static void on_logo_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct about_page *p = data;
	guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (index >= LOGO_COUNT) {
		return;
	}
	show_logo(p, index);
	char *path = logo_file();
	if (path) {
		tw_write_string(path, logo_names[index]);
	}
	g_free(path);
}

/* ---------- the page ---------- */

/* A row whose value can be selected and copied, and nothing at all when the
 * machine has no answer for it. */
static void add_row(struct about_page *p, GtkWidget *group, const char *title, char *value) {
	if (!value || !*value) {
		g_free(value);
		return;
	}
	GtkWidget *label = gtk_label_new(value);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 1);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(label, TRUE);
	ui_row(group, title, NULL, label);
	g_string_append_printf(p->as_text, "%s: %s\n", title, value);
	g_free(value);
}

static void on_copy(GtkButton *button, gpointer data) {
	struct about_page *p = data;
	GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
	gdk_clipboard_set_text(clipboard, p->as_text->str);
	settings_status(p->s, "The whole page was copied");
}

GtkWidget *about_page_new(struct settings *s) {
	struct about_page *p = g_new0(struct about_page, 1);
	p->s = s;
	p->as_text = g_string_new(NULL);
	GtkWidget *content;
	GtkWidget *page = ui_page("About",
		"What this computer is running, and what tileWin is running it with.", &content);

	// the hero: the logo beside who and what this is
	GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
	gtk_widget_set_margin_bottom(hero, 8);
	p->logo = gtk_label_new(NULL);
	gtk_label_set_selectable(GTK_LABEL(p->logo), TRUE);
	gtk_widget_add_css_class(p->logo, "tw-logo");
	gtk_widget_set_valign(p->logo, GTK_ALIGN_CENTER);
	show_logo(p, logo_index());
	gtk_box_append(GTK_BOX(hero), p->logo);

	GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_valign(titles, GTK_ALIGN_CENTER);
	char *host = first_line_of("/etc/hostname");
	if (!host) {
		host = g_strdup(g_get_host_name());
	}
	char *who = g_strdup_printf("%s@%s", g_get_user_name(), host);
	p->name = gtk_label_new(who);
	gtk_widget_add_css_class(p->name, "tw-title");
	gtk_label_set_xalign(GTK_LABEL(p->name), 0);
	gtk_label_set_selectable(GTK_LABEL(p->name), TRUE);
	gtk_box_append(GTK_BOX(titles), p->name);
	char *pretty = field_of("/etc/os-release", "PRETTY_NAME", '=');
	p->subtitle = gtk_label_new(pretty ? pretty : "Linux");
	gtk_widget_add_css_class(p->subtitle, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(p->subtitle), 0);
	gtk_box_append(GTK_BOX(titles), p->subtitle);
	GtkWidget *version = gtk_label_new("tileWin " SWAY_VERSION);
	gtk_widget_add_css_class(version, "dim-label");
	gtk_widget_add_css_class(version, "tw-caption");
	gtk_label_set_xalign(GTK_LABEL(version), 0);
	gtk_label_set_selectable(GTK_LABEL(version), TRUE);
	gtk_box_append(GTK_BOX(titles), version);
	gtk_box_append(GTK_BOX(hero), titles);
	gtk_box_append(GTK_BOX(content), hero);
	g_string_append_printf(p->as_text, "%s\n%s\ntileWin %s\n\n", who,
		pretty ? pretty : "Linux", SWAY_VERSION);

	struct utsname un;
	bool have_uname = uname(&un) == 0;

	GtkWidget *system = ui_group(content, "System", NULL);
	add_row(p, system, "Operating system", pretty ? g_strdup(pretty) : NULL);
	char *product = first_line_of("/sys/devices/virtual/dmi/id/product_name");
	add_row(p, system, "Model", product);
	add_row(p, system, "Kernel", have_uname ?
		g_strdup_printf("%s %s", un.sysname, un.release) : NULL);
	add_row(p, system, "Architecture", have_uname ? g_strdup(un.machine) : NULL);
	add_row(p, system, "Uptime", about_uptime());
	add_row(p, system, "Packages", about_packages());
	const char *shell = g_getenv("SHELL");
	add_row(p, system, "Shell", shell ? g_path_get_basename(shell) : NULL);
	const char *lang = g_getenv("LANG");
	add_row(p, system, "Locale", lang ? g_strdup(lang) : NULL);

	GtkWidget *desktop = ui_group(content, "Desktop", NULL);
	add_row(p, desktop, "Window manager", g_strdup("tileWin " SWAY_VERSION));
	char *mode = settings_current_mode();
	add_row(p, desktop, "Mode", g_strdup(strcmp(mode, "tile") == 0 ?
		"Tile mode" : "Window mode"));
	g_free(mode);
	add_row(p, desktop, "Theme", settings_current_theme());
	add_row(p, desktop, "Screens", about_screens());
	struct cstmt *term = confdoc_child(s->common->root, "set", "$term");
	char *term_command = term ? cstmt_raw_args(s->common, term) : NULL;
	if (term_command && g_str_has_prefix(term_command, "$term ")) {
		char *bare = g_strdup(term_command + 6);
		g_free(term_command);
		term_command = bare;
	}
	add_row(p, desktop, "Terminal", term_command);

	GtkWidget *hardware = ui_group(content, "Hardware", NULL);
	add_row(p, hardware, "Processor", about_cpu());
	add_row(p, hardware, "Graphics", about_gpu());
	add_row(p, hardware, "Memory", about_memory());
	add_row(p, hardware, "Swap", about_swap());
	add_row(p, hardware, "Disk (/)", about_disk());
	add_row(p, hardware, "Battery", about_battery());

	GtkWidget *look = ui_group(content, "This page", NULL);
	p->logo_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Normal", "Tiny", "UwU", NULL });
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->logo_dd), logo_index());
	g_signal_connect(p->logo_dd, "notify::selected", G_CALLBACK(on_logo_selected), p);
	ui_row(look, "Logo", "Which logo is drawn beside the details", p->logo_dd);
	GtkWidget *copy = gtk_button_new_with_label("Copy everything");
	g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), p);
	ui_row(look, "Copy", "Puts the whole page on the clipboard as text", copy);

	g_free(who);
	g_free(host);
	g_free(pretty);
	s->about_page = p;
	return page;
}
