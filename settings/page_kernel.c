#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>
#include <glib/gstdio.h>
#include "settings.h"

/*
 * Kernel page: builds Linux kernels of kernel.org with tilewin-kernel (as the
 * user, in the background: closing the window does not stop a build) and puts
 * them on the computer, takes them off, loads or boots them with
 * tilewin-kernel-root through pkexec, which always asks for the password. How
 * the boot loader is left alone is told at the top of tilewin-kernel-root.
 *
 * For tests: TILEWIN_KERNEL_TOOL and TILEWIN_KERNEL_ROOT_HELPER are the two
 * scripts, the second run as it is, without pkexec; TILEWIN_KERNEL_NO_SYSTEMD
 * starts a build without systemd-run.
 */

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif
#ifndef TILEWIN_BINDIR
#define TILEWIN_BINDIR "/usr/local/bin"
#endif

static const char *const profile_values[] = { "running", "device", "portable", "full", NULL };
static const char *const profile_labels[] = {
	"Same as the distribution's kernel", "Only this computer", "This computer and common hardware",
	"Everything", NULL };
static const char *const profile_hints[] = {
	"Everything the kernel of your distribution has, also while one built here runs; what "
	"is new in the version gets its default. The safest choice. Builds in about an hour.",
	"Only the drivers this computer used: the ones loaded now and, with \"Remember the "
	"drivers in use\" on, every one seen since. Builds in minutes and is small; what was "
	"never plugged in (a new USB device) does not work.",
	"What this computer used, and the drivers for the parts one may put in instead: "
	"graphics cards of AMD, Intel and NVIDIA, NVMe and SATA disks, USB devices, Wi-Fi and "
	"network cards, sound, webcams, Bluetooth, the common file systems.",
	"Every driver and feature that can be a module, on top of the distribution's kernel. The "
	"biggest: builds for hours and needs about 60 GB while it does.",
};

static const char *const comp_values[] = { "zstd", "xz", "lz4", "gzip", "lzo", NULL };
static const char *const comp_labels[] = { "Zstandard (zstd)", "XZ", "LZ4", "Gzip", "LZO", NULL };
static const char *const comp_hints[] = {
	"Small and very fast to unpack, so the computer starts quickly. What most distributions "
	"use.",
	"The smallest: fits the most kernels on the EFI partition. Slowest to unpack, so the "
	"start takes a moment longer, and slow to build.",
	"The fastest to unpack: the quickest start, but the biggest files. The modules stay "
	"uncompressed, which loads them fastest.",
	"Works with every boot loader and tool, old ones too. Middling size and speed.",
	"Fast to unpack and bigger than Gzip. The modules stay uncompressed.",
};

struct kernel_page {
	struct settings *s;
	GtkWidget *running_row, *boot_row, *space_row, *load_row, *patch_row, *foreign_row;
	GtkWidget *version_dd, *profile_dd, *profile_row, *comp_dd, *comp_row, *livepatch;
	GtkWidget *missing_row, *missing_label, *tools_button, *kexec_button;
	GtkWidget *build_button, *cancel_button, *progress_row, *progress_label, *spinner;
	GtkWidget *built_list, *built_empty, *message;
	GtkWidget *native, *seen_switch, *seen_row, *name_entry;
	GtkWidget *fixes_group, *fixes_list;     // the livepatches of one kernel with a tree
	char *fixes_kver, *fixes;               // its name and what livepatches said last
	bool fixes_running, fixes_installed;    // whether it runs, whether it is on the computer
	GtkWidget *sched_row, *sched_dd, *mode_dd, *sched_apply, *sched_boot, *sched_full_dd;
	GtkWidget *full_mode_dd;
	GtkWidget *sched_install, *sched_rows[3]; // the rows that need scx_loader
	GPtrArray *scheds;  // char *, the schedulers of sched_dd after [0], the kernel's own
	GDBusProxy *scx;    // scx_loader
	char *scx_seen;     // "SCHEDULER MODE" the dropdowns were last set to
	char *scx_want;     // the scheduler "Use it now" asked for, until it is checked
	guint scx_want_mode;
	int ticks;
	bool seen_refreshing, full_refreshing;
	GPtrArray *versions; // char *, what the version dropdown offers; [0] is "latest"
	GPtrArray *missing;  // char *, packages a build still needs
	char *info;          // what tilewin-kernel-root info said
	char *list;          // what tilewin-kernel list said last
	char *pending, *pending_action; // the kernel a confirmation waits for
	guint timer;
	bool busy; // a root helper is running
};

/* ---------- running the scripts ---------- */

static char *tool_path(void) {
	const char *test = g_getenv("TILEWIN_KERNEL_TOOL");
	return g_strdup(test && *test ? test : TILEWIN_BINDIR "/tilewin-kernel");
}

static char *root_helper_path(void) {
	const char *test = g_getenv("TILEWIN_KERNEL_ROOT_HELPER");
	return g_strdup(test && *test ? test : TILEWIN_LIBEXECDIR "/tilewin-kernel-root");
}

static char *store_dir(void) {
	return g_build_filename(g_get_user_data_dir(), "tileWin", "kernels", NULL);
}

typedef void (*done_fn)(struct kernel_page *p, bool ok, const char *out, const char *err);

struct call {
	struct kernel_page *p;
	done_fn done;
};

static void call_finished(GObject *source, GAsyncResult *result, gpointer data) {
	struct call *c = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		&error) && g_subprocess_get_successful(G_SUBPROCESS(source));
	if (error && !err) {
		err = g_strdup(error->message);
	}
	// 126/127: pkexec was refused or found no agent to ask
	if (!ok && g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
			(g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 126 ||
			g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 127) &&
			!g_getenv("TILEWIN_KERNEL_ROOT_HELPER") && (!err || !*err)) {
		g_free(err);
		err = g_strdup("not authorized");
	}
	if (c->done) {
		c->done(c->p, ok, out ? out : "", err ? g_strstrip(err) : "");
	}
	g_clear_error(&error);
	g_free(out);
	g_free(err);
	g_free(c);
}

static void run(struct kernel_page *p, const char *const *argv, done_fn done) {
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	if (!proc) {
		if (done) {
			done(p, false, "", error->message);
		}
		g_error_free(error);
		return;
	}
	struct call *c = g_new0(struct call, 1);
	c->p = p;
	c->done = done;
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, call_finished, c);
	g_object_unref(proc);
}

/* tilewin-kernel with up to three arguments. */
static void run_tool(struct kernel_page *p, done_fn done, const char *a, const char *b) {
	char *tool = tool_path();
	const char *argv[] = { "bash", tool, a, b, NULL };
	run(p, argv, done);
	g_free(tool);
}

/* tilewin-kernel-root through pkexec; info needs no root and goes without it. */
static void run_root(struct kernel_page *p, done_fn done, const char *a, const char *b,
		const char *c) {
	char *helper = root_helper_path();
	const char *argv[8];
	int n = 0;
	if (strcmp(a, "info") != 0 && !g_getenv("TILEWIN_KERNEL_ROOT_HELPER")) {
		argv[n++] = "pkexec";
		argv[n++] = helper;
	} else {
		argv[n++] = "bash";
		argv[n++] = helper;
	}
	argv[n++] = a;
	argv[n++] = b;
	if (b) {
		argv[n++] = c;
	}
	argv[n] = NULL;
	run(p, argv, done);
	g_free(helper);
}

/* ---------- key=value lines ---------- */

static char *value_of(const char *text, const char *key) {
	if (!text) {
		return NULL;
	}
	size_t len = strlen(key);
	for (const char *line = text; line && *line; ) {
		const char *end = strpbrk(line, "\t\n");
		if (strncmp(line, key, len) == 0 && line[len] == '=') {
			const char *v = line + len + 1;
			return g_strndup(v, end ? (size_t)(end - v) : strlen(v));
		}
		line = end ? end + 1 : NULL;
	}
	return NULL;
}

static void set_subtitle(GtkWidget *row, const char *text) {
	// the row of ui_row: box > labels > title, subtitle
	GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
	GtkWidget *labels = gtk_widget_get_first_child(box);
	GtkWidget *subtitle = gtk_widget_get_next_sibling(gtk_widget_get_first_child(labels));
	if (subtitle) {
		gtk_label_set_text(GTK_LABEL(subtitle), text);
	}
}

/* What went wrong, or (as a note, without the colour) what is being done. */
static void show_message(struct kernel_page *p, const char *text, bool error) {
	if (error) {
		gtk_widget_add_css_class(p->message, "error");
	} else {
		gtk_widget_remove_css_class(p->message, "error");
	}
	gtk_label_set_text(GTK_LABEL(p->message), text ? text : "");
	gtk_widget_set_visible(p->message, text && *text);
}

static void set_message(struct kernel_page *p, const char *text) {
	show_message(p, text, true);
}

static void set_note(struct kernel_page *p, const char *text) {
	show_message(p, text, false);
}

/* ---------- this computer ---------- */

static void show_info(struct kernel_page *p) {
	char *loader = value_of(p->info, "loader"), *method = value_of(p->info, "method");
	char *esp = value_of(p->info, "esp"), *free_kb = value_of(p->info, "esp_free_kb");
	char *kexec = value_of(p->info, "kexec"), *patch = value_of(p->info, "livepatch");
	char *initramfs = value_of(p->info, "initramfs");
	char *boot;
	if (method && g_str_has_prefix(method, "uki")) {
		boot = g_strdup_printf("%s. New kernels go to %s/EFI/Linux as unified kernel images, "
			"which it lists by itself; its settings and grub.cfg are not touched. Booting "
			"one once is %s.", loader && *loader ? loader : "The boot loader is not known: this "
			"start came straight from the firmware (Boot into does that)", esp ? esp :
			"the EFI partition",
			strstr(method, "oneshot") ? "systemd-boot's one-shot entry" :
			"the firmware's BootNext: the start after it is the usual one again");
	} else if (method && g_str_has_prefix(method, "grub")) {
		boot = g_strdup_printf("%s without unified kernel images. New kernels go to /boot and "
			"grub.cfg is made anew, replacing the old one only when it has no faults; the old "
			"one is kept as grub.cfg.tilewin-backup.", loader && *loader ? loader : "GRUB");
	} else {
		boot = g_strdup("No boot loader was found that kernels can be added to: they can be "
			"built, not put on the computer.");
	}
	char *secure = value_of(p->info, "secureboot");
	if (secure && strcmp(secure, "on") == 0) {
		char *more = g_strconcat(boot, " Secure Boot is on: the firmware starts only signed "
			"images, and the kernels built here are not signed (sbctl signs them). Booting one "
			"once does no harm: the start after it is the usual one.", NULL);
		g_free(boot);
		boot = more;
	}
	g_free(secure);
	if (!initramfs || !*initramfs) {
		char *more = g_strconcat(boot, " Neither mkinitcpio nor dracut is installed.", NULL);
		g_free(boot);
		boot = more;
	}
	set_subtitle(p->boot_row, boot);
	char *space = free_kb && *free_kb ? g_strdup_printf("%" G_GUINT64_FORMAT " MB free on %s; "
		"a kernel takes 50 to 200 MB of it", g_ascii_strtoull(free_kb, NULL, 10) / 1024, esp) :
		g_strdup("Not known");
	set_subtitle(p->space_row, space);
	bool no_tools = kexec && strcmp(kexec, "no: install kexec-tools") == 0;
	char *no = kexec && g_str_has_prefix(kexec, "no: ") ? g_strdup_printf("Not possible: %s",
		kexec + 4) : g_strdup("Not possible");
	set_subtitle(p->load_row, kexec && strcmp(kexec, "yes") == 0 ?
		"Yes: a kernel can be started straight from the running one (kexec), without the "
		"firmware and the boot menu. The programs still end, as on a restart." :
		no_tools ? "Not possible yet: it needs kexec-tools, which starts a kernel straight from "
		"the running one, without the firmware and the boot menu" : no);
	gtk_widget_set_visible(p->kexec_button, no_tools);
	g_free(no);
	set_subtitle(p->patch_row, patch && strcmp(patch, "yes") == 0 ?
		"The running kernel takes live patches" :
		"The running kernel takes no live patches; switch it on below for a kernel you build");
	g_free(boot);
	g_free(space);
	g_free(loader);
	g_free(method);
	g_free(esp);
	g_free(free_kb);
	g_free(kexec);
	g_free(patch);
	g_free(initramfs);
}

static void scx_show(struct kernel_page *p);

static void on_info(struct kernel_page *p, bool ok, const char *out, const char *err) {
	g_free(p->info);
	p->info = g_strdup(out);
	show_info(p);
	scx_show(p);
}

/* Drivers in use that came for the distribution's kernel only. */
static void on_foreign(struct kernel_page *p, bool ok, const char *out, const char *err) {
	GString *modules = g_string_new(NULL);
	GPtrArray *packages = g_ptr_array_new_with_free_func(g_free);
	char **lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], "\t", 2);
		if (f[0] && *f[0]) {
			g_string_append_printf(modules, "%s%s", modules->len ? ", " : "", f[0]);
			bool have = false;
			for (guint k = 0; f[1] && *f[1] && k < packages->len; k++) {
				have = have || strcmp(packages->pdata[k], f[1]) == 0;
			}
			if (f[1] && *f[1] && !have) {
				g_ptr_array_add(packages, g_strdup(f[1]));
			}
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	if (modules->len) {
		GString *text = g_string_new(NULL);
		g_string_append_printf(text, "%s in use now will be missing: ", modules->str);
		if (packages->len) {
			for (guint k = 0; k < packages->len; k++) {
				g_string_append_printf(text, "%s%s", k ? ", " : "", (char *)packages->pdata[k]);
			}
			g_string_append(text, packages->len > 1 ? " are" : " is");
			g_string_append(text, " built for the distribution's kernel only. The DKMS kind of "
				"the package (");
			for (guint k = 0; k < packages->len; k++) {
				g_string_append_printf(text, "%s%s-dkms", k ? ", " : "", (char *)packages->pdata[k]);
			}
			g_string_append(text, ", in its place) builds them for every kernel.");
		} else {
			g_string_append(text, "they were built for the distribution's kernel only.");
		}
		set_subtitle(p->foreign_row, text->str);
		g_string_free(text, TRUE);
	}
	gtk_widget_set_visible(p->foreign_row, modules->len > 0);
	g_string_free(modules, TRUE);
	g_ptr_array_unref(packages);
}

/* ---------- the new kernel ---------- */

static void on_versions(struct kernel_page *p, bool ok, const char *out, const char *err) {
	GtkStringList *strings = gtk_string_list_new(NULL);
	g_ptr_array_set_size(p->versions, 0);
	char **lines = g_strsplit(out, "\n", -1);
	const char *latest = NULL;
	for (int i = 0; lines[i] && !latest; i++) {
		const char *tab = strchr(lines[i], '\t');
		if (tab && (g_str_has_suffix(tab, "latest") || strstr(tab, "latest,"))) {
			latest = lines[i];
		}
	}
	char *first = latest ? g_strdup_printf("Latest (now %.*s)", (int)strcspn(latest, "\t"), latest) :
		g_strdup("Latest");
	gtk_string_list_append(strings, first);
	g_free(first);
	g_ptr_array_add(p->versions, g_strdup("latest"));
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], "\t", 2);
		if (f[0] && *f[0]) {
			char *label = f[1] && *f[1] ? g_strdup_printf("%s (%s)", f[0], f[1]) : g_strdup(f[0]);
			gtk_string_list_append(strings, label);
			g_ptr_array_add(p->versions, g_strdup(f[0]));
			g_free(label);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	ui_drop_down_set_strings(GTK_DROP_DOWN(p->version_dd), strings);
	g_object_unref(strings);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->version_dd), 0);
	if (!ok) {
		set_message(p, *err ? err : "kernel.org could not be reached: only \"Latest\" can be built");
	}
}

static const char *selected(GtkWidget *dd, const char *const *values) {
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	guint n = 0;
	while (values[n]) {
		n++;
	}
	return values[i < n ? i : 0];
}

static void on_profile(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct kernel_page *p = data;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->profile_dd));
	set_subtitle(p->profile_row, profile_hints[i < G_N_ELEMENTS(profile_hints) ? i : 0]);
}

static void check_tools(struct kernel_page *p);
static bool building(struct kernel_page *p);

static void on_compression(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct kernel_page *p = data;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->comp_dd));
	set_subtitle(p->comp_row, comp_hints[i < G_N_ELEMENTS(comp_hints) ? i : 0]);
	check_tools(p);
}

/*
 * PROGRAM:PACKAGE lines, PROGRAM:PACKAGE:optional for what a build goes
 * without: the Rust tools (or the Rust drivers are left out) and ccache.
 */
static void on_check(struct kernel_page *p, bool ok, const char *out, const char *err) {
	g_ptr_array_set_size(p->missing, 0);
	GString *needed = g_string_new(NULL), *rust = g_string_new(NULL);
	bool ccache = false, patchutils = false;
	char **lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], ":", 3);
		if (f[0] && f[1]) {
			bool optional = f[2] && strcmp(f[2], "optional") == 0;
			if (!optional) {
				g_string_append_printf(needed, "%s%s", needed->len ? ", " : "", f[0]);
			} else if (strcmp(f[0], "ccache") == 0) {
				ccache = true;
			} else if (strcmp(f[0], "recountdiff") == 0) {
				patchutils = true;
			} else {
				g_string_append_printf(rust, "%s%s", rust->len ? ", " : "", f[0]);
			}
			bool have = false;
			for (guint k = 0; k < p->missing->len; k++) {
				have = have || strcmp(p->missing->pdata[k], f[1]) == 0;
			}
			if (!have) {
				g_ptr_array_add(p->missing, g_strdup(f[1]));
			}
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	GString *text = g_string_new(NULL);
	if (needed->len) {
		g_string_append_printf(text, "Not installed: %s. ", needed->str);
	}
	if (rust->len) {
		g_string_append_printf(text, "Without %s the kernel leaves out its Rust drivers (the "
			"QR code of the crash screen, NVIDIA's nova). ", rust->str);
	}
	if (ccache) {
		g_string_append(text, "With ccache the next build of a kernel takes minutes, not the "
			"whole time again. ");
	}
	if (patchutils) {
		g_string_append(text, "Live fixes need patchutils.");
	}
	set_subtitle(p->missing_row, g_strstrip(text->str));
	gtk_widget_set_visible(p->missing_row, p->missing->len > 0);
	gtk_widget_set_sensitive(p->build_button, !needed->len && !building(p));
	g_string_free(text, TRUE);
	g_string_free(needed, TRUE);
	g_string_free(rust, TRUE);
}

static void check_tools(struct kernel_page *p) {
	run_tool(p, on_check, "check", selected(p->comp_dd, comp_values));
}

static void refresh_list(struct kernel_page *p);

static void on_tools_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	p->busy = false;
	set_message(p, ok ? NULL : *err ? err : "The programs could not be installed");
	check_tools(p);
	run_root(p, on_info, "info", NULL, NULL);
	refresh_list(p);
}

static void on_tools(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (p->missing->len == 0 || p->busy) {
		return;
	}
	// pkexec tilewin-kernel-root tools PACKAGE...
	char *helper = root_helper_path();
	GPtrArray *argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_getenv("TILEWIN_KERNEL_ROOT_HELPER") ? "bash" : "pkexec");
	g_ptr_array_add(argv, helper);
	g_ptr_array_add(argv, "tools");
	for (guint i = 0; i < p->missing->len; i++) {
		g_ptr_array_add(argv, p->missing->pdata[i]);
	}
	g_ptr_array_add(argv, NULL);
	p->busy = true;
	set_note(p, "Installing the programs…");
	run(p, (const char *const *)argv->pdata, on_tools_done);
	g_ptr_array_free(argv, TRUE);
	g_free(helper);
}

static void on_kexec_tools(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (p->busy) {
		return;
	}
	p->busy = true;
	set_note(p, "Installing kexec-tools…");
	run_root(p, on_tools_done, "tools", "kexec-tools", NULL);
}

static bool building(struct kernel_page *p);

// What may follow the version in a kernel's name, as tilewin-kernel takes it:
// what uname -r, file names and the boot menu do with.
static bool valid_kernel_name(const char *name) {
	size_t n = strlen(name);
	if (n == 0 || n > 32 || !g_ascii_isalnum(name[0])) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!g_ascii_isalnum(name[i]) && !strchr("._+-", name[i])) {
			return false;
		}
	}
	return true;
}

// Whether a kernel of that name is built or on the computer already. The
// build would refuse it too, but after the page could still say so.
static bool kernel_name_taken(const char *version, const char *name) {
	char *release = strchr(strchr(version, '.') + 1, '.') ? g_strdup(version) :
		g_strdup_printf("%s.0", version);
	char *kver = g_strdup_printf("%s-%s", release, name);
	char *store = store_dir();
	char *built = g_build_filename(store, kver, NULL);
	char *installed = g_build_filename("/usr/lib/modules", kver, NULL);
	bool taken = g_file_test(built, G_FILE_TEST_EXISTS) || g_file_test(installed, G_FILE_TEST_EXISTS);
	g_free(release);
	g_free(kver);
	g_free(store);
	g_free(built);
	g_free(installed);
	return taken;
}

static void on_build(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (building(p)) {
		return;
	}
	guint v = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->version_dd));
	const char *version = v < p->versions->len ? p->versions->pdata[v] : "latest";
	char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->name_entry))));
	if (*name && !valid_kernel_name(name)) {
		set_message(p, "A name is up to 32 letters, digits and . _ + -, starting with a letter or digit");
		g_free(name);
		return;
	}
	// "latest" is the release after it in the list
	const char *release = strcmp(version, "latest") == 0 && p->versions->len > 1 ?
		p->versions->pdata[1] : version;
	if (*name && strcmp(release, "latest") != 0 && strchr(release, '.') &&
			kernel_name_taken(release, name)) {
		char *text = g_strdup_printf("There is a kernel called %s already: choose another name, "
			"or delete it", name);
		set_message(p, text);
		g_free(text);
		g_free(name);
		return;
	}
	char *tool = tool_path();
	const char *profile = selected(p->profile_dd, profile_values);
	const char *comp = selected(p->comp_dd, comp_values);
	const char *patch = gtk_switch_get_active(GTK_SWITCH(p->livepatch)) ? "on" : "off";
	// in a unit of its own, so that it goes on when the settings close
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	char *systemd_run = g_find_program_in_path("systemd-run");
	if (systemd_run && !g_getenv("TILEWIN_KERNEL_NO_SYSTEMD")) {
		g_ptr_array_add(argv, g_strdup(systemd_run));
		g_ptr_array_add(argv, g_strdup("--user"));
		g_ptr_array_add(argv, g_strdup("--collect"));
		g_ptr_array_add(argv, g_strdup("--quiet"));
		g_ptr_array_add(argv, g_strdup("--unit=tilewin-kernel-build"));
		const char *pass[] = { "XDG_DATA_HOME", "XDG_CACHE_HOME", "TILEWIN_KERNEL_FRAGMENTS",
			"TILEWIN_KERNEL_MIRROR", "TILEWIN_KERNEL_RELEASES", "PATH", NULL };
		for (int i = 0; pass[i]; i++) {
			if (g_getenv(pass[i])) {
				g_ptr_array_add(argv, g_strdup_printf("--setenv=%s=%s", pass[i], g_getenv(pass[i])));
			}
		}
	} else {
		g_ptr_array_add(argv, g_strdup("setsid"));
		g_ptr_array_add(argv, g_strdup("-f"));
	}
	g_free(systemd_run);
	g_ptr_array_add(argv, g_strdup("bash"));
	g_ptr_array_add(argv, tool);
	g_ptr_array_add(argv, g_strdup("build"));
	g_ptr_array_add(argv, g_strdup(version));
	g_ptr_array_add(argv, g_strdup(profile));
	g_ptr_array_add(argv, g_strdup(comp));
	g_ptr_array_add(argv, g_strdup(patch));
	g_ptr_array_add(argv, g_strdup(gtk_switch_get_active(GTK_SWITCH(p->native)) ? "on" : "off"));
	g_ptr_array_add(argv, name);
	g_ptr_array_add(argv, NULL);
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv((const char *const *)argv->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	if (!proc) {
		set_message(p, error->message);
		g_error_free(error);
	} else {
		set_message(p, NULL);
		g_object_unref(proc);
	}
	g_ptr_array_unref(argv);
	gtk_label_set_text(GTK_LABEL(p->progress_label), "Starting…");
	gtk_widget_set_visible(p->progress_row, TRUE);
	gtk_widget_set_sensitive(p->build_button, FALSE);
	// the list picks it up once the build has written its first files
	g_timeout_add_once(800, (GSourceOnceFunc)refresh_list, p);
}

/* ---------- the drivers this computer used ---------- */

static void run_systemctl(struct kernel_page *p, done_fn done, const char *what) {
	const char *argv[] = { "systemctl", "--user", what, "tilewin-kernel-modules.timer", NULL };
	const char *now[] = { "systemctl", "--user", what, "--now", "tilewin-kernel-modules.timer",
		NULL };
	run(p, strcmp(what, "is-enabled") == 0 ? argv : now, done);
}

static void on_seen(struct kernel_page *p, bool ok, const char *out, const char *err) {
	char **f = g_strsplit(out, "\t", 2);
	long count = f[0] ? strtol(f[0], NULL, 10) : 0;
	char *text;
	if (count > 0 && f[1] && *f[1]) {
		GDateTime *t = g_date_time_new_from_unix_local(g_ascii_strtoll(f[1], NULL, 10));
		char *when = t ? g_date_time_format(t, "%e %B %Y") : g_strdup("");
		text = g_strdup_printf("%ld drivers seen since %s. Only this computer and Common "
			"hardware keep every one of them, not only the ones loaded at the build.", count,
			g_strstrip(when));
		g_free(when);
		if (t) {
			g_date_time_unref(t);
		}
	} else {
		text = g_strdup("Notes the drivers loaded every ten minutes, so that Only this computer "
			"keeps the USB stick of last week too.");
	}
	set_subtitle(p->seen_row, text);
	g_free(text);
	g_strfreev(f);
}

static void on_seen_state(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (strstr(out, "not-found") || strstr(err, "not-found") || strstr(err, "No such file")) {
		// the timer comes with the install of tileWin
		gtk_widget_set_sensitive(p->seen_switch, FALSE);
		set_subtitle(p->seen_row, "Comes with the install of tileWin, which this one was "
			"before: install it again, then switch it on here.");
		return;
	}
	gtk_widget_set_sensitive(p->seen_switch, TRUE);
	p->seen_refreshing = true;
	gtk_switch_set_active(GTK_SWITCH(p->seen_switch), ok);
	p->seen_refreshing = false;
	// on from the first time the page is opened: the user may switch it off
	char *asked = g_build_filename(g_get_user_state_dir(), "tileWin", "kernel-modules-asked", NULL);
	if (!ok && !g_file_test(asked, G_FILE_TEST_EXISTS)) {
		char *dir = g_path_get_dirname(asked);
		g_mkdir_with_parents(dir, 0755);
		g_file_set_contents(asked, "", 0, NULL);
		g_free(dir);
		gtk_switch_set_active(GTK_SWITCH(p->seen_switch), TRUE);
	}
	g_free(asked);
	run_tool(p, on_seen, "seen", NULL);
}

static void on_seen_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (!ok) {
		set_message(p, *err ? err : "The timer that notes the drivers could not be switched");
	}
	run_tool(p, on_seen, "seen", NULL);
}

static void on_seen_switch(GObject *sw, GParamSpec *pspec, gpointer data) {
	struct kernel_page *p = data;
	if (p->seen_refreshing) {
		return;
	}
	bool on = gtk_switch_get_active(GTK_SWITCH(p->seen_switch));
	run_systemctl(p, on_seen_done, on ? "enable" : "disable");
}

static void on_cancel_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (!ok) {
		set_message(p, err);
	}
	refresh_list(p);
}

static void on_cancel(GtkButton *button, gpointer data) {
	run_tool(data, on_cancel_done, "cancel", NULL);
}

/* ---------- the kernels built ---------- */

static const char *profile_name(const char *value) {
	for (int i = 0; profile_values[i]; i++) {
		if (value && strcmp(value, profile_values[i]) == 0) {
			return profile_labels[i];
		}
	}
	return value ? value : "";
}

static bool building(struct kernel_page *p) {
	char *dir = store_dir();
	char *current = g_build_filename(dir, "current", NULL);
	bool yes = p->list && strstr(p->list, "\tbuilding\t") && g_file_test(current, G_FILE_TEST_EXISTS);
	g_free(current);
	g_free(dir);
	return yes;
}

static void on_action_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	p->busy = false;
	gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), FALSE);
	if (ok) {
		set_message(p, NULL);
		settings_status(p->s, "%s", *out ? g_strstrip((char *)out) : "Done");
	} else {
		set_message(p, *err ? err : "It did not work");
	}
	run_root(p, on_info, "info", NULL, NULL);
	refresh_list(p);
}

static void on_deleted(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (!ok) {
		set_message(p, *err ? err : "The kernel could not be deleted");
	}
	refresh_list(p);
}

/* Taking it off went well: now the files the build left. */
static void on_removed_for_delete(struct kernel_page *p, bool ok, const char *out,
		const char *err) {
	p->busy = false;
	gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), FALSE);
	if (!ok) {
		set_message(p, *err ? err : "The kernel could not be taken off");
		refresh_list(p);
		return;
	}
	run_root(p, on_info, "info", NULL, NULL);
	if (p->pending) {
		run_tool(p, on_deleted, "delete", p->pending);
	}
}

static void act(struct kernel_page *p, const char *action, const char *kver, bool installed) {
	if (p->busy) {
		return;
	}
	char *store = store_dir();
	p->busy = true;
	gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), TRUE);
	const char *doing = strcmp(action, "install") == 0 ? "Putting the kernel on the computer: "
		"making its boot image and the DKMS modules takes a minute or two…" :
		strcmp(action, "remove") == 0 || (strcmp(action, "delete") == 0 && installed) ?
		"Taking the kernel off the computer…" : strcmp(action, "delete") == 0 ? NULL :
		installed ? "Getting ready to start the kernel…" : "Putting the kernel on the computer "
		"first: this takes a minute or two, then it starts…";
	set_note(p, doing);
	if (strcmp(action, "install") == 0) {
		run_root(p, on_action_done, "install", kver, store);
	} else if (strcmp(action, "remove") == 0) {
		run_root(p, on_action_done, "remove", kver, NULL);
	} else if (strcmp(action, "delete") == 0) {
		if (installed) {
			run_root(p, on_removed_for_delete, "remove", kver, NULL);
		} else {
			p->busy = false;
			gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), FALSE);
			run_tool(p, on_deleted, "delete", kver);
		}
	} else {
		// load or boot: a kernel not on the computer yet is put on first
		run_root(p, on_action_done, action, kver, installed ? NULL : store);
	}
	g_free(store);
}

static void on_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
	struct kernel_page *p = data;
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
	if (button == 1 && p->pending && p->pending_action) {
		const char *installed = g_object_get_data(G_OBJECT(source), "installed");
		act(p, p->pending_action, p->pending, installed != NULL);
	}
}

static void confirm(struct kernel_page *p, const char *action, const char *kver,
		bool installed) {
	g_free(p->pending);
	g_free(p->pending_action);
	p->pending = g_strdup(kver);
	p->pending_action = g_strdup(action);
	GtkAlertDialog *dialog;
	const char *ok;
	if (strcmp(action, "boot") == 0) {
		dialog = gtk_alert_dialog_new("Restart into %s now?", kver);
		gtk_alert_dialog_set_detail(dialog, "Save your work first: every program ends and the "
			"computer restarts. It starts this kernel once; the start after it is the usual "
			"one again, so a kernel that does not start does no harm. Pick it in the boot menu "
			"to start it again.");
		ok = "Restart";
	} else if (strcmp(action, "load") == 0) {
		dialog = gtk_alert_dialog_new("Start %s now?", kver);
		gtk_alert_dialog_set_detail(dialog, "Save your work first: every program ends, then the "
			"new kernel starts straight away, without the firmware and the boot menu (kexec). "
			"The next restart is the usual kernel again.");
		ok = "Start it";
	} else if (strcmp(action, "remove") == 0) {
		dialog = gtk_alert_dialog_new("Take %s off the computer?", kver);
		gtk_alert_dialog_set_detail(dialog, "It leaves the boot menu and its modules are "
			"removed. The build stays here, to put it on again.");
		ok = "Take off";
	} else {
		dialog = gtk_alert_dialog_new("Delete %s?", kver);
		gtk_alert_dialog_set_detail(dialog, installed ? "It is taken off the computer and its "
			"build is deleted." : "Its build is deleted.");
		ok = "Delete";
	}
	gtk_alert_dialog_set_buttons(dialog, (const char *const[]){ "Cancel", ok, NULL });
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	if (installed) {
		g_object_set_data(G_OBJECT(dialog), "installed", GINT_TO_POINTER(1));
	}
	gtk_alert_dialog_choose(dialog, p->s->window, NULL, on_confirmed, p);
	g_object_unref(dialog);
}

static void choose_fix(struct kernel_page *p);
static void security_build(struct kernel_page *p);
static void refresh_list(struct kernel_page *p);

static void on_row_button(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	const char *action = g_object_get_data(G_OBJECT(button), "action");
	const char *kver = g_object_get_data(G_OBJECT(button), "kver");
	bool installed = g_object_get_data(G_OBJECT(button), "installed") != NULL;
	if (strcmp(action, "install") == 0) {
		act(p, action, kver, installed);
	} else if (strcmp(action, "fix") == 0 || strcmp(action, "security") == 0) {
		// the Live fixes below show that kernel's from now on
		if (g_strcmp0(p->fixes_kver, kver) != 0) {
			g_free(p->fixes_kver);
			p->fixes_kver = g_strdup(kver);
			g_clear_pointer(&p->fixes, g_free);
			g_clear_pointer(&p->list, g_free);
			refresh_list(p);
		}
		if (strcmp(action, "fix") == 0) {
			choose_fix(p);
		} else {
			security_build(p);
		}
	} else if (strcmp(action, "log") == 0) {
		char *dir = store_dir();
		char *log = g_build_filename(dir, kver, "log", NULL);
		char *uri = g_filename_to_uri(log, NULL, NULL);
		g_app_info_launch_default_for_uri(uri, NULL, NULL);
		g_free(uri);
		g_free(log);
		g_free(dir);
	} else {
		confirm(p, action, kver, installed);
	}
}

static GtkWidget *row_button(struct kernel_page *p, const char *label, const char *action,
		const char *kver, bool installed, bool sensitive) {
	GtkWidget *b = gtk_button_new_with_label(label);
	g_object_set_data_full(G_OBJECT(b), "action", g_strdup(action), g_free);
	g_object_set_data_full(G_OBJECT(b), "kver", g_strdup(kver), g_free);
	if (installed) {
		g_object_set_data(G_OBJECT(b), "installed", GINT_TO_POINTER(1));
	}
	gtk_widget_set_sensitive(b, sensitive);
	g_signal_connect(b, "clicked", G_CALLBACK(on_row_button), p);
	return b;
}

/* ---------- live fixes of a kernel with its build tree ---------- */

static void on_fix_built(struct kernel_page *p, bool ok, const char *out, const char *err);

static void on_fix_chosen(GObject *source, GAsyncResult *result, gpointer data) {
	struct kernel_page *p = data;
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!file || !p->fixes_kver) {
		g_clear_object(&file);
		return;
	}
	char *path = g_file_get_path(file);
	g_object_unref(file);
	set_note(p, "Making the livepatch: klp-build compiles the kernel with the patch and "
		"without, and keeps what changed. This takes a few minutes…");
	char *tool = tool_path();
	const char *argv[] = { "bash", tool, "patch-build", p->fixes_kver, path, NULL };
	run(p, argv, on_fix_built);
	g_free(tool);
	g_free(path);
}

static void choose_fix(struct kernel_page *p) {
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Pick the patch to make a live fix of");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Patches (.patch, .diff)");
	gtk_file_filter_add_pattern(filter, "*.patch");
	gtk_file_filter_add_pattern(filter, "*.diff");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	g_object_unref(filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	g_object_unref(filters);
	gtk_file_dialog_open(dialog, p->s->window, NULL, on_fix_chosen, p);
	g_object_unref(dialog);
}

static void refresh_fixes(struct kernel_page *p);

static void on_fix_built(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (ok) {
		set_note(p, "The livepatch is ready: apply it below.");
	} else {
		set_message(p, *err ? err : "The livepatch could not be made: see its log");
	}
	refresh_fixes(p);
}

static void on_security_built(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (ok) {
		set_note(p, *out ? g_strstrip((char *)out) : "The fixes are ready: apply them below.");
	} else {
		set_message(p, *err ? err : "The fixes could not be made: see their log");
	}
	g_clear_pointer(&p->fixes, g_free);
	refresh_fixes(p);
}

static void security_build(struct kernel_page *p) {
	if (!p->fixes_kver) {
		return;
	}
	set_note(p, "Making a livepatch of kernel.org's fixes for this kernel: the newest release of "
		"its series is downloaded, and klp-build turns every fixed file it can into the "
		"livepatch. This takes a while, the first time most…");
	char *tool = tool_path();
	const char *argv[] = { "bash", tool, "security-build", p->fixes_kver, NULL };
	run(p, argv, on_security_built);
	g_free(tool);
	g_clear_pointer(&p->fixes, g_free);
	g_timeout_add_once(1500, (GSourceOnceFunc)refresh_fixes, p);
}

static void on_fix_action_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	p->busy = false;
	gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), FALSE);
	if (ok) {
		set_message(p, NULL);
		settings_status(p->s, "%s", *out ? g_strstrip((char *)out) : "Done");
	} else {
		set_message(p, *err ? err : "It did not work");
	}
	g_clear_pointer(&p->fixes, g_free);
	refresh_fixes(p);
}

static void on_fix_button(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	const char *action = g_object_get_data(G_OBJECT(button), "action");
	const char *name = g_object_get_data(G_OBJECT(button), "name");
	if (!p->fixes_kver || p->busy) {
		return;
	}
	if (strcmp(action, "log") == 0) {
		char *dir = store_dir();
		char *file = g_strdup_printf("%s.log", name);
		char *log = g_build_filename(dir, p->fixes_kver, "livepatches", file, NULL);
		char *uri = g_filename_to_uri(log, NULL, NULL);
		g_app_info_launch_default_for_uri(uri, NULL, NULL);
		g_free(uri);
		g_free(log);
		g_free(file);
		g_free(dir);
		return;
	}
	p->busy = true;
	gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), TRUE);
	if (strcmp(action, "apply") == 0) {
		set_note(p, "Applying the livepatch: the running programs go on as they are…");
		char *store = store_dir();
		char *helper = root_helper_path();
		const char *argv[] = { g_getenv("TILEWIN_KERNEL_ROOT_HELPER") ? "bash" : "pkexec",
			helper, "livepatch-apply", p->fixes_kver, name, store, NULL };
		run(p, argv, on_fix_action_done);
		g_free(helper);
		g_free(store);
	} else if (strcmp(action, "start-on") == 0 || strcmp(action, "start-off") == 0) {
		bool on = strcmp(action, "start-on") == 0;
		set_note(p, on ? "Setting the livepatch for every start of this kernel…" :
			"Taking the livepatch off the start…");
		char *store = store_dir();
		char *helper = root_helper_path();
		const char *argv[] = { g_getenv("TILEWIN_KERNEL_ROOT_HELPER") ? "bash" : "pkexec",
			helper, "livepatch-start", p->fixes_kver, name, store, on ? "on" : "off", NULL };
		run(p, argv, on_fix_action_done);
		g_free(helper);
		g_free(store);
	} else {
		set_note(p, "Taking the livepatch out…");
		run_root(p, on_fix_action_done, "livepatch-undo", name, NULL);
	}
}

static GtkWidget *fix_button(struct kernel_page *p, const char *label, const char *action,
		const char *name) {
	GtkWidget *b = gtk_button_new_with_label(label);
	g_object_set_data_full(G_OBJECT(b), "action", g_strdup(action), g_free);
	g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name), g_free);
	gtk_widget_set_sensitive(b, !p->busy || strcmp(action, "log") == 0);
	g_signal_connect(b, "clicked", G_CALLBACK(on_fix_button), p);
	return b;
}

/* NAME<tab>STATE<tab>PROGRESS lines of tilewin-kernel livepatches. */
static void on_fixes(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (p->fixes && strcmp(p->fixes, out) == 0) {
		return;
	}
	g_free(p->fixes);
	p->fixes = g_strdup(out);
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->fixes_list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->fixes_list), child);
	}
	char *title = g_strdup_printf("Linux %s", p->fixes_kver ? p->fixes_kver : "");
	ui_row(p->fixes_list, title, p->fixes_running ? "Running now: a fix applied takes hold at "
		"once, every program goes on" : p->fixes_installed ? "Not running: set a fix for its "
		"every start, or apply it once it runs" : "Not on the computer: add it to the boot "
		"menu to set fixes for its start", NULL);
	g_free(title);
	bool any = false;
	char **lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], "\t", 4);
		if (f[0] && *f[0] && f[1]) {
			any = true;
			const char *state = f[1];
			bool ready = strcmp(state, "built") == 0 || strcmp(state, "applied") == 0;
			bool at_start = f[2] && f[3] && strcmp(f[3], "yes") == 0;
			GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			if (strcmp(state, "built") == 0 && p->fixes_running) {
				gtk_box_append(GTK_BOX(buttons), fix_button(p, "Apply", "apply", f[0]));
			} else if (strcmp(state, "applied") == 0) {
				gtk_box_append(GTK_BOX(buttons), fix_button(p, "Undo", "undo", f[0]));
			}
			if (ready && (p->fixes_installed || at_start)) {
				gtk_box_append(GTK_BOX(buttons), at_start ?
					fix_button(p, "Not at start", "start-off", f[0]) :
					fix_button(p, "Apply at start", "start-on", f[0]));
			}
			gtk_box_append(GTK_BOX(buttons), fix_button(p, "Log", "log", f[0]));
			GString *what = g_string_new(strcmp(state, "applied") == 0 ? "Applied: the running "
				"kernel has it now" : f[2] && *f[2] ? f[2] : state);
			if (strcmp(state, "applied") == 0 && f[2] && *f[2]) {
				g_string_append_printf(what, "\n%s", f[2]);
			}
			if (at_start) {
				g_string_append(what, "\nApplied at every start of this kernel");
			}
			char *label = g_str_has_prefix(f[0], "security_") ?
				g_strdup("Security fixes of kernel.org") : g_strdup(f[0]);
			ui_row(p->fixes_list, label, what->str, buttons);
			g_free(label);
			g_string_free(what, TRUE);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	if (!any) {
		ui_row(p->fixes_list, "No live fix yet", "\"Security fixes\" makes one of kernel.org's "
			"fixes for this kernel; \"Fix live…\" one of a patch of your own", NULL);
	}
}

static void refresh_fixes(struct kernel_page *p) {
	if (p->fixes_kver) {
		run_tool(p, on_fixes, "livepatches", p->fixes_kver);
	}
}

static char *size_text(const char *kb) {
	return kb ? g_format_size(g_ascii_strtoull(kb, NULL, 10) * 1024) : g_strdup("");
}

static void show_list(struct kernel_page *p) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->built_list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->built_list), child);
	}
	char *kexec = value_of(p->info, "kexec");
	bool can_load = kexec && strcmp(kexec, "yes") == 0;
	g_free(kexec);
	char *method = value_of(p->info, "method");
	bool can_install = method && !g_str_has_prefix(method, "none");
	g_free(method);
	bool any = false, build_running = false, fixes_kver_seen = false;
	char *first_tree = NULL, *running_tree = NULL;
	char **lines = g_strsplit(p->list ? p->list : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], "\t", 3);
		if (!f[0] || !*f[0] || !f[1]) {
			g_strfreev(f);
			continue;
		}
		any = true;
		const char *kver = f[0], *state = f[1], *rest = f[2] ? f[2] : "";
		char *installed = value_of(rest, "installed"), *profile = value_of(rest, "profile");
		char *comp = value_of(rest, "compression"), *patch = value_of(rest, "livepatch");
		char *size = value_of(rest, "size"), *built = value_of(rest, "built");
		char *progress = value_of(rest, "progress");
		char *native = value_of(rest, "native"), *rust = value_of(rest, "rust");
		char *tree = value_of(rest, "tree");
		bool on = installed && strcmp(installed, "no") != 0;
		bool is_running = installed && strcmp(installed, "running") == 0;
		GString *sub = g_string_new(profile_name(profile));
		if (comp) {
			g_string_append_printf(sub, " · %s", comp);
		}
		if (patch && strcmp(patch, "on") == 0) {
			g_string_append(sub, " · live patching");
		}
		if (native && strcmp(native, "on") == 0) {
			g_string_append(sub, " · for this processor");
		}
		if (strcmp(state, "built") == 0) {
			char *s = size_text(size);
			g_string_append_printf(sub, " · %s", s);
			g_free(s);
			if (built) {
				GDateTime *t = g_date_time_new_from_unix_local(g_ascii_strtoll(built, NULL, 10));
				char *when = t ? g_date_time_format(t, "%e %b %Y, %H:%M") : NULL;
				if (when) {
					g_string_append_printf(sub, " · built %s", g_strstrip(when));
				}
				g_free(when);
				if (t) {
					g_date_time_unref(t);
				}
			}
			g_string_append(sub, is_running ? "\nRunning now" : on ?
				"\nOn the computer: in the boot menu" : "\nNot on the computer");
			if (rust && g_str_has_prefix(rust, "off: ")) {
				g_string_append_printf(sub, "\nWithout its Rust drivers: %s", rust + 5);
			}
			if (native && g_str_has_prefix(native, "off: ")) {
				g_string_append_printf(sub, "\nNot for this processor only: %s", native + 5);
			}
		} else if (strcmp(state, "building") == 0) {
			build_running = true;
			g_string_append_printf(sub, "\n%s", progress && *progress ? progress : "Building…");
			gtk_label_set_text(GTK_LABEL(p->progress_label), progress && *progress ?
				progress : "Building…");
		} else {
			g_string_append_printf(sub, "\n%s", progress && *progress ? progress :
				strcmp(state, "cancelled") == 0 ? "Cancelled" : "Failed");
		}
		char *title = g_strdup_printf("Linux %s", kver);
		GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		bool ready = strcmp(state, "built") == 0;
		if (ready && !on) {
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Add to boot menu", "install", kver,
				false, can_install && !p->busy));
		}
		if (ready && !is_running) {
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Load", "load", kver, on,
				can_load && can_install && !p->busy));
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Boot into…", "boot", kver, on,
				can_install && !p->busy));
		}
		if (on && !is_running) {
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Take off", "remove", kver, on,
				!p->busy));
		}
		if (ready && tree) {
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Security fixes", "security", kver,
				on, !p->busy));
			gtk_box_append(GTK_BOX(buttons), row_button(p, "Fix live…", "fix", kver, on,
				!p->busy));
			if (!first_tree) {
				first_tree = g_strdup(kver);
			}
			if (is_running) {
				g_free(running_tree);
				running_tree = g_strdup(kver);
			}
			if (g_strcmp0(p->fixes_kver, kver) == 0) {
				fixes_kver_seen = true;
				p->fixes_running = is_running;
				p->fixes_installed = on;
			}
		}
		gtk_box_append(GTK_BOX(buttons), row_button(p, "Log", "log", kver, on, true));
		if (strcmp(state, "building") != 0 && !is_running) {
			GtkWidget *del = row_button(p, "Delete", "delete", kver, on, !p->busy);
			gtk_widget_add_css_class(del, "destructive-action");
			gtk_box_append(GTK_BOX(buttons), del);
		}
		ui_row(p->built_list, title, sub->str, buttons);
		g_free(title);
		g_string_free(sub, TRUE);
		g_free(installed);
		g_free(profile);
		g_free(comp);
		g_free(patch);
		g_free(size);
		g_free(built);
		g_free(progress);
		g_free(native);
		g_free(rust);
		g_free(tree);
		g_strfreev(f);
	}
	g_strfreev(lines);
	// the fixes of the kernel picked last, else of the running one, else the first
	if (!fixes_kver_seen) {
		g_free(p->fixes_kver);
		p->fixes_kver = g_strdup(running_tree ? running_tree : first_tree);
		g_clear_pointer(&p->fixes, g_free);
		if (p->fixes_kver) {
			// what the loop saw of it: show the list again with it picked
			g_free(running_tree);
			g_free(first_tree);
			show_list(p);
			return;
		}
	}
	g_free(running_tree);
	g_free(first_tree);
	gtk_widget_set_visible(p->fixes_group, p->fixes_kver != NULL);
	if (!any) {
		ui_row(p->built_list, "No kernel built yet", NULL, NULL);
	}
	gtk_widget_set_visible(p->progress_row, build_running);
	gtk_widget_set_sensitive(p->build_button, !build_running);
}

static void on_list(struct kernel_page *p, bool ok, const char *out, const char *err) {
	if (p->list && strcmp(p->list, out) == 0) {
		return;
	}
	g_free(p->list);
	p->list = g_strdup(out);
	show_list(p);
}

static void refresh_list(struct kernel_page *p) {
	run_tool(p, on_list, "list", NULL);
}

/* While the page is on screen: the progress of a build, every second. */
static void scx_refresh(struct kernel_page *p);

static gboolean on_tick(gpointer data) {
	struct kernel_page *p = data;
	if (gtk_widget_get_mapped(p->built_list) && !p->busy) {
		refresh_list(p);
		refresh_fixes(p);
		// a scheduler the taskbar or anyone else switched to, every few seconds
		if (++p->ticks % 3 == 0) {
			scx_refresh(p);
		}
	}
	return G_SOURCE_CONTINUE;
}

/* ---------- the CPU scheduler (sched_ext, through scx_loader) ---------- */

static const char *const mode_values[] = { "Auto", "Gaming", "PowerSave", "LowLatency", "Server",
	NULL };
static const char *const mode_labels[] = { "Automatic", "Gaming", "Power saving", "Low latency",
	"Server", NULL };

/* What the better known ones are for; the others go by their name. */
static const char *sched_hint(const char *name) {
	static const struct {
		const char *name, *hint;
	} hints[] = {
		{ "scx_lavd", "games and a quick desktop" },
		{ "scx_bpfland", "a quick desktop under load" },
		{ "scx_cosmos", "light, for every day" },
		{ "scx_flash", "even and quick" },
		{ "scx_rusty", "heavy work on many cores" },
		{ "scx_p2dq", "many cores" },
		{ "scx_tickless", "servers" },
		{ "scx_rustland", "an experiment in user space" },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(hints); i++) {
		if (strcmp(hints[i].name, name) == 0) {
			return hints[i].hint;
		}
	}
	return NULL;
}

static char *sched_label(const char *name) {
	const char *hint = sched_hint(name);
	return hint ? g_strdup_printf("%s (%s)", name, hint) : g_strdup(name);
}

static GBusType scx_bus(void) {
	const char *which = g_getenv("TILEWIN_SCX_BUS");
	return which && strcmp(which, "session") == 0 ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
}

static char *scx_string(struct kernel_page *p, const char *property) {
	GVariant *v = p->scx ? g_dbus_proxy_get_cached_property(p->scx, property) : NULL;
	char *text = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ?
		g_variant_dup_string(v, NULL) : NULL;
	if (v) {
		g_variant_unref(v);
	}
	return text;
}

static void scx_show(struct kernel_page *p) {
	char *state = value_of(p->info, "sched_ext"), *have = value_of(p->info, "scx");
	bool kernel_has = state && strcmp(state, "none") != 0;
	bool installed = have && strcmp(have, "yes") == 0;
	char *current = scx_string(p, "CurrentScheduler");
	GVariant *mode = p->scx ? g_dbus_proxy_get_cached_property(p->scx, "SchedulerMode") : NULL;
	guint m = mode && g_variant_is_of_type(mode, G_VARIANT_TYPE_UINT32) ?
		g_variant_get_uint32(mode) : 0;
	if (mode) {
		g_variant_unref(mode);
	}
	bool running = current && *current && strcmp(current, "unknown") != 0;
	char *text;
	if (!kernel_has) {
		text = g_strdup("The running kernel has no sched_ext: its own scheduler (EEVDF) runs. "
			"Kernels built here keep sched_ext when the distribution's kernel has it.");
	} else if (!installed) {
		text = g_strdup("sched_ext lets a scheduler of its own decide which program runs next, "
			"loaded and swapped while the computer runs. The schedulers come with scx-scheds "
			"and scx-tools.");
	} else if (running) {
		text = g_strdup_printf("Now: %s, %s", current, mode_labels[m < 5 ? m : 0]);
	} else {
		text = g_strdup("Now: the kernel's own (EEVDF)");
	}
	set_subtitle(p->sched_row, text);
	g_free(text);
	gtk_widget_set_visible(p->sched_install, kernel_has && !installed);
	for (int i = 0; i < 3; i++) {
		gtk_widget_set_visible(p->sched_rows[i], kernel_has && installed);
	}

	// the schedulers there are, once
	GVariant *list = p->scx ? g_dbus_proxy_get_cached_property(p->scx, "SupportedSchedulers") : NULL;
	if (list && g_variant_is_of_type(list, G_VARIANT_TYPE_STRING_ARRAY) && p->scheds->len == 0) {
		GtkStringList *strings = gtk_string_list_new(NULL);
		GtkStringList *full = gtk_string_list_new(NULL);
		gtk_string_list_append(strings, "The kernel's own (EEVDF)");
		gtk_string_list_append(full, "Off: leave it as it is");
		gsize n;
		const char **names = g_variant_get_strv(list, &n);
		for (gsize i = 0; i < n; i++) {
			char *label = sched_label(names[i]);
			gtk_string_list_append(strings, label);
			gtk_string_list_append(full, label);
			g_free(label);
			g_ptr_array_add(p->scheds, g_strdup(names[i]));
		}
		g_free(names);
		ui_drop_down_set_strings(GTK_DROP_DOWN(p->sched_dd), strings);
		p->full_refreshing = true;
		ui_drop_down_set_strings(GTK_DROP_DOWN(p->sched_full_dd), full);
		const char *chosen = cstmt_arg(confdoc_child(p->s->taskbar->root, "fullscreen_scheduler",
			NULL), 0);
		guint at = 0;
		for (guint i = 0; chosen && i < p->scheds->len; i++) {
			if (strcmp(p->scheds->pdata[i], chosen) == 0) {
				at = i + 1;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->sched_full_dd), at);
		const char *full_mode = cstmt_arg(confdoc_child(p->s->taskbar->root,
			"fullscreen_scheduler_mode", NULL), 0);
		guint fm = 1;
		for (guint i = 0; full_mode && mode_values[i]; i++) {
			if (g_ascii_strcasecmp(mode_values[i], full_mode) == 0) {
				fm = i;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->full_mode_dd), fm);
		p->full_refreshing = false;
		g_object_unref(strings);
		g_object_unref(full);
	}
	if (list) {
		g_variant_unref(list);
	}
	// what runs is what the dropdowns show, when it changes: not over a choice
	// being made
	char *seen = g_strdup_printf("%s %u", running ? current : "", m);
	if (p->scheds->len && g_strcmp0(seen, p->scx_seen) != 0) {
		guint at = 0;
		for (guint i = 0; running && i < p->scheds->len; i++) {
			if (strcmp(p->scheds->pdata[i], current) == 0) {
				at = i + 1;
			}
		}
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->sched_dd), at);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), running && m < 5 ? m : 1);
		g_free(p->scx_seen);
		p->scx_seen = seen;
	} else {
		g_free(seen);
	}
	g_free(current);
	g_free(state);
	g_free(have);
}

static void on_scx_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
		gpointer data) {
	scx_show(data);
}

static void on_scx_proxy(GObject *source, GAsyncResult *result, gpointer data) {
	struct kernel_page *p = data;
	p->scx = g_dbus_proxy_new_for_bus_finish(result, NULL);
	if (p->scx) {
		g_signal_connect(p->scx, "g-properties-changed", G_CALLBACK(on_scx_changed), p);
	}
	scx_show(p);
}

/*
 * scx_loader says its properties emit changes, but sends no
 * PropertiesChanged: what the proxy has cached stays what it was when the
 * page connected. So they are asked for again, and put into the cache.
 */
typedef void (*scx_fn)(struct kernel_page *p);

struct scx_get {
	struct kernel_page *p;
	scx_fn then;
};

static void on_scx_all(GObject *source, GAsyncResult *result, gpointer data) {
	struct scx_get *g = data;
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
	struct kernel_page *p = g->p;
	if (v && p->scx) {
		GVariant *all = g_variant_get_child_value(v, 0);
		GVariantIter iter;
		const char *key;
		GVariant *value;
		g_variant_iter_init(&iter, all);
		while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
			g_dbus_proxy_set_cached_property(p->scx, key, value);
			g_variant_unref(value);
		}
		g_variant_unref(all);
		g_variant_unref(v);
		scx_show(p);
	}
	if (g->then) {
		g->then(p);
	}
	g_free(g);
}

static void scx_get_all(struct kernel_page *p, scx_fn then) {
	if (!p->scx) {
		if (then) {
			then(p);
		}
		return;
	}
	struct scx_get *g = g_new0(struct scx_get, 1);
	g->p = p;
	g->then = then;
	g_dbus_connection_call(g_dbus_proxy_get_connection(p->scx), "org.scx.Loader",
		"/org/scx/Loader", "org.freedesktop.DBus.Properties", "GetAll",
		g_variant_new("(s)", "org.scx.Loader"), G_VARIANT_TYPE("(a{sv})"),
		G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_scx_all, g);
}

static void scx_refresh(struct kernel_page *p) {
	char *owner = p->scx ? g_dbus_proxy_get_name_owner(p->scx) : NULL;
	if (owner) {
		scx_get_all(p, NULL);
	}
	g_free(owner);
}

static void scx_connect(struct kernel_page *p) {
	g_clear_object(&p->scx);
	g_ptr_array_set_size(p->scheds, 0);
	g_dbus_proxy_new_for_bus(scx_bus(), G_DBUS_PROXY_FLAGS_NONE, NULL, "org.scx.Loader",
		"/org/scx/Loader", "org.scx.Loader", NULL, on_scx_proxy, p);
}

/* Whether what "Use it now" asked for runs: scx_loader answers before it knows. */
static void on_scx_checked(struct kernel_page *p) {
	if (!p->scx_want) {
		return;
	}
	char *current = scx_string(p, "CurrentScheduler");
	bool running = current && *current && strcmp(current, "unknown") != 0;
	bool ok = *p->scx_want ? running && strcmp(current, p->scx_want) == 0 : !running;
	if (ok) {
		set_message(p, NULL);
		settings_status(p->s, *p->scx_want ? "%s runs now" : "The kernel's own scheduler runs now",
			p->scx_want);
	} else {
		char *text = *p->scx_want ? g_strdup_printf("%s did not start: scx_loader's log says why "
			"(journalctl -u scx_loader)", p->scx_want) :
			g_strdup("The scheduler did not stop: scx_loader's log says why (journalctl -u scx_loader)");
		set_message(p, text);
		g_free(text);
	}
	g_clear_pointer(&p->scx_want, g_free);
	g_free(current);
	gtk_widget_set_sensitive(p->sched_apply, TRUE);
}

static gboolean scx_check_later(gpointer data) {
	scx_get_all(data, on_scx_checked);
	return G_SOURCE_REMOVE;
}

static void on_scx_call(GObject *source, GAsyncResult *result, gpointer data) {
	struct kernel_page *p = data;
	GError *error = NULL;
	GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
	if (v) {
		g_variant_unref(v);
		set_note(p, "Switching the scheduler…");
		// a scheduler takes a second or two to load
		g_timeout_add(3000, scx_check_later, p);
	} else {
		char *text = g_strdup_printf("The scheduler could not be changed: %s",
			error ? error->message : "scx_loader did not answer");
		set_message(p, text);
		g_free(text);
		g_clear_pointer(&p->scx_want, g_free);
		gtk_widget_set_sensitive(p->sched_apply, TRUE);
	}
	g_clear_error(&error);
}

/* With what runs now, not what the page saw when it opened: Start while one
 * runs is refused by scx_loader (in its log only). */
static void sched_apply_now(struct kernel_page *p) {
	char *current = scx_string(p, "CurrentScheduler");
	bool running = current && *current && strcmp(current, "unknown") != 0;
	if (!*p->scx_want) {
		if (!running) {
			on_scx_checked(p);
		} else {
			g_dbus_proxy_call(p->scx, "StopScheduler", NULL,
				G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 30000, NULL, on_scx_call, p);
		}
	} else {
		g_dbus_proxy_call(p->scx, running ? "SwitchScheduler" : "StartScheduler",
			g_variant_new("(su)", p->scx_want, p->scx_want_mode),
			G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 30000, NULL, on_scx_call, p);
	}
	g_free(current);
}

static void on_sched_apply(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (!p->scx || p->scx_want) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->sched_dd));
	guint m = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->mode_dd));
	p->scx_want = g_strdup(i == 0 || i > p->scheds->len ? "" : (const char *)p->scheds->pdata[i - 1]);
	p->scx_want_mode = m < 5 ? m : 0;
	gtk_widget_set_sensitive(p->sched_apply, FALSE);
	scx_get_all(p, sched_apply_now);
}

static void on_sched_boot(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (p->busy) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->sched_dd));
	guint m = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->mode_dd));
	p->busy = true;
	set_note(p, NULL);
	if (i == 0 || i > p->scheds->len) {
		run_root(p, on_action_done, "scheduler-default", "none", "Auto");
	} else {
		run_root(p, on_action_done, "scheduler-default", p->scheds->pdata[i - 1],
			mode_values[m < 5 ? m : 0]);
	}
}

static void on_sched_full(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct kernel_page *p = data;
	if (p->full_refreshing) {
		return;
	}
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->sched_full_dd));
	confdoc_set(p->s->taskbar, p->s->taskbar->root, "fullscreen_scheduler", NULL,
		i == 0 || i > p->scheds->len ? NULL : p->scheds->pdata[i - 1]);
	// Gaming is what the taskbar takes when nothing is written
	guint m = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->full_mode_dd));
	confdoc_set(p->s->taskbar, p->s->taskbar->root, "fullscreen_scheduler_mode", NULL,
		m == 1 || m >= 5 ? NULL : mode_values[m]);
	settings_taskbar_changed(p->s);
}

static void on_scx_tools_done(struct kernel_page *p, bool ok, const char *out, const char *err) {
	on_tools_done(p, ok, out, err);
	// scx_loader is there now: it answers on the bus
	scx_connect(p);
}

static void on_scx_tools(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (p->busy) {
		return;
	}
	p->busy = true;
	set_note(p, "Installing scx-scheds and scx-tools…");
	char *helper = root_helper_path();
	const char *argv[] = { g_getenv("TILEWIN_KERNEL_ROOT_HELPER") ? "bash" : "pkexec", helper,
		"tools", "scx-scheds", "scx-tools", NULL };
	run(p, argv, on_scx_tools_done);
	g_free(helper);
}

/* ---------- the page ---------- */

GtkWidget *kernel_page_new(struct settings *s) {
	struct kernel_page *p = g_new0(struct kernel_page, 1);
	p->s = s;
	p->versions = g_ptr_array_new_with_free_func(g_free);
	p->missing = g_ptr_array_new_with_free_func(g_free);
	p->scheds = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(p->versions, g_strdup("latest"));
	GtkWidget *content;
	GtkWidget *page = ui_page("Kernel", "Build Linux kernels of kernel.org for this computer "
		"and start them. The kernel of your distribution stays as it is, and so does the boot "
		"menu's own setup.", &content);

	p->message = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(p->message), TRUE);
	gtk_label_set_xalign(GTK_LABEL(p->message), 0);
	gtk_widget_add_css_class(p->message, "error");
	gtk_widget_set_visible(p->message, FALSE);
	gtk_box_append(GTK_BOX(content), p->message);

	GtkWidget *group = ui_group(content, "This computer", NULL);
	struct utsname un;
	p->running_row = ui_row(group, "Running kernel", uname(&un) == 0 ? un.release : "", NULL);
	p->boot_row = ui_row(group, "Boot", "Looking…", NULL);
	p->space_row = ui_row(group, "EFI partition", "Looking…", NULL);
	p->kexec_button = gtk_button_new_with_label("Install…");
	g_signal_connect(p->kexec_button, "clicked", G_CALLBACK(on_kexec_tools), p);
	gtk_widget_set_visible(p->kexec_button, FALSE);
	p->load_row = ui_row(group, "Load without a restart", "Looking…", p->kexec_button);
	p->patch_row = ui_row(group, "Live patching", "Looking…", NULL);
	p->foreign_row = ui_row(group, "Drivers a new kernel will not have", "", NULL);
	gtk_widget_add_css_class(p->foreign_row, "warning");
	gtk_widget_set_visible(p->foreign_row, FALSE);

	group = ui_group(content, "New kernel", "The source is downloaded from kernel.org and "
		"checked against its checksums, then built here in the background. Closing the "
		"settings does not stop it.");
	const char *const loading[] = { "Latest", NULL };
	p->version_dd = gtk_drop_down_new_from_strings(loading);
	ui_row(group, "Version", "\"Latest\" is the newest stable release when the build starts",
		p->version_dd);
	p->name_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->name_entry), "tilewin1");
	gtk_entry_set_max_length(GTK_ENTRY(p->name_entry), 32);
	ui_row(group, "Name", "What follows the version in uname -r and the boot menu, as in "
		"7.3.0-mycustomname. Left empty, it is tilewin and a number counting up.", p->name_entry);
	p->profile_dd = gtk_drop_down_new_from_strings(profile_labels);
	p->profile_row = ui_row(group, "What goes in", profile_hints[0], p->profile_dd);
	g_signal_connect(p->profile_dd, "notify::selected", G_CALLBACK(on_profile), p);
	p->comp_dd = gtk_drop_down_new_from_strings(comp_labels);
	p->comp_row = ui_row(group, "Compression", comp_hints[0], p->comp_dd);
	g_signal_connect(p->comp_dd, "notify::selected", G_CALLBACK(on_compression), p);
	p->livepatch = gtk_switch_new();
	ui_row(group, "Live patching", "Lets fixes be applied to the running kernel without a "
		"restart (CONFIG_LIVEPATCH, for kpatch and livepatch modules). A whole other kernel "
		"version cannot be patched in: that always takes a restart or Load.", p->livepatch);
	p->native = gtk_switch_new();
	ui_row(group, "For this processor only", "Built for the instructions of this computer's "
		"processor (X86_NATIVE_CPU): a little faster, but it may not start on another computer. "
		"Leave it off for a kernel that has to go elsewhere.", p->native);
	p->seen_switch = gtk_switch_new();
	g_signal_connect(p->seen_switch, "notify::active", G_CALLBACK(on_seen_switch), p);
	p->seen_row = ui_row(group, "Remember the drivers in use", "", p->seen_switch);
	p->tools_button = gtk_button_new_with_label("Install…");
	g_signal_connect(p->tools_button, "clicked", G_CALLBACK(on_tools), p);
	p->missing_row = ui_row(group, "Programs a build needs", "", p->tools_button);
	gtk_widget_set_visible(p->missing_row, FALSE);
	p->build_button = gtk_button_new_with_label("Build");
	gtk_widget_add_css_class(p->build_button, "suggested-action");
	g_signal_connect(p->build_button, "clicked", G_CALLBACK(on_build), p);
	ui_row(group, "Build it", "Uses every core at a low priority, so the desktop stays smooth",
		p->build_button);
	GtkWidget *progress_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	p->cancel_button = gtk_button_new_with_label("Cancel");
	g_signal_connect(p->cancel_button, "clicked", G_CALLBACK(on_cancel), p);
	gtk_box_append(GTK_BOX(progress_box), p->cancel_button);
	p->progress_row = ui_row(group, "Building", "", progress_box);
	p->progress_label = gtk_widget_get_next_sibling(gtk_widget_get_first_child(
		gtk_widget_get_first_child(gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(p->progress_row)))));
	gtk_widget_set_visible(p->progress_row, FALSE);

	p->built_list = ui_group(content, "Built kernels", "Add one to the boot menu, start it "
		"now (Load) or restart into it once (Boot into), take it off again or delete it. The "
		"running kernel cannot be taken off.");
	p->spinner = gtk_spinner_new();
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(p->built_list)), p->spinner);

	p->fixes_list = ui_group(content, "Live fixes", "Changes to a kernel built with live "
		"patching that take hold at once, without a restart: every program goes on with all it "
		"has open. \"Security fixes\" turns what kernel.org fixed in the newest release of the "
		"kernel's series into one; \"Fix live…\" a patch of your own (a .patch file, a fix from "
		"the mailing list, say). The kernel's own klp-build makes them, in the build tree kept "
		"for it, also for a kernel that does not run now. They change what functions do: a fix "
		"to a header or to the data the kernel keeps is left out (its log lists them) and needs "
		"the newest release built. Applied, a fix lasts until the next restart; \"Apply at "
		"start\" applies it at every start of that kernel. Nobody looked over a fix made here as "
		"a distribution does: if the computer acts up after one, Undo takes it out again.");
	p->fixes_group = gtk_widget_get_parent(p->fixes_list);
	gtk_widget_set_visible(p->fixes_group, FALSE);

	group = ui_group(content, "CPU scheduler", "Which program runs next on which core. With "
		"sched_ext a scheduler of its own takes over from the kernel's, swapped while every "
		"program goes on running; when one stops or fails, the kernel's own is back at once.");
	p->sched_install = gtk_button_new_with_label("Install…");
	g_signal_connect(p->sched_install, "clicked", G_CALLBACK(on_scx_tools), p);
	gtk_widget_set_visible(p->sched_install, FALSE);
	p->sched_row = ui_row(group, "Scheduler", "Looking…", p->sched_install);
	GtkWidget *sched_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	const char *const none[] = { "The kernel's own (EEVDF)", NULL };
	p->sched_dd = gtk_drop_down_new_from_strings(none);
	p->mode_dd = gtk_drop_down_new_from_strings(mode_labels);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), 1);
	gtk_box_append(GTK_BOX(sched_box), p->sched_dd);
	gtk_box_append(GTK_BOX(sched_box), p->mode_dd);
	p->sched_rows[0] = ui_row(group, "Choose", "The scheduler and what it leans to: games, "
		"saving power, the quickest answer to every click", sched_box);
	GtkWidget *sched_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->sched_apply = gtk_button_new_with_label("Use it now");
	gtk_widget_add_css_class(p->sched_apply, "suggested-action");
	g_signal_connect(p->sched_apply, "clicked", G_CALLBACK(on_sched_apply), p);
	p->sched_boot = gtk_button_new_with_label("Start it with the computer");
	g_signal_connect(p->sched_boot, "clicked", G_CALLBACK(on_sched_boot), p);
	gtk_box_append(GTK_BOX(sched_buttons), p->sched_boot);
	gtk_box_append(GTK_BOX(sched_buttons), p->sched_apply);
	p->sched_rows[1] = ui_row(group, "Apply", "Now, or from every start on (the kernel's own: "
		"none at start)", sched_buttons);
	const char *const off[] = { "Off: leave it as it is", NULL };
	p->sched_full_dd = gtk_drop_down_new_from_strings(off);
	g_signal_connect(p->sched_full_dd, "notify::selected", G_CALLBACK(on_sched_full), p);
	p->full_mode_dd = gtk_drop_down_new_from_strings(mode_labels);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->full_mode_dd), 1);
	g_signal_connect(p->full_mode_dd, "notify::selected", G_CALLBACK(on_sched_full), p);
	GtkWidget *full_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(full_box), p->sched_full_dd);
	gtk_box_append(GTK_BOX(full_box), p->full_mode_dd);
	p->sched_rows[2] = ui_row(group, "While a window fills a screen", "A game in fullscreen, "
		"say: the taskbar switches to this scheduler in this mode, and back to what ran before "
		"once no window fills a screen", full_box);
	for (int i = 0; i < 3; i++) {
		gtk_widget_set_visible(p->sched_rows[i], FALSE);
	}

	run_root(p, on_info, "info", NULL, NULL);
	run_tool(p, on_versions, "versions", NULL);
	run_tool(p, on_foreign, "foreign", NULL);
	check_tools(p);
	run_systemctl(p, on_seen_state, "is-enabled");
	scx_connect(p);
	refresh_list(p);
	p->timer = g_timeout_add_seconds(1, on_tick, p);
	return page;
}
