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
	"Same as the running kernel", "Only this computer", "This computer and common hardware",
	"Everything", NULL };
static const char *const profile_hints[] = {
	"Everything the kernel you run now has; what is new in the version gets its default. "
	"The safest choice. Builds in about an hour.",
	"Only the drivers in use right now. Builds in minutes and is small, but what is not "
	"plugged in now (a USB stick, a printer, a phone) may not work: plug in everything "
	"you use before building.",
	"What this computer uses, and the drivers for the parts one may put in instead: "
	"graphics cards of AMD, Intel and NVIDIA, NVMe and SATA disks, USB devices, Wi-Fi and "
	"network cards, sound, webcams, Bluetooth, the common file systems.",
	"Every driver and feature that can be a module, on top of the running kernel. The "
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
			"one once is %s.", loader && *loader ? loader : "The firmware", esp ? esp : "the EFI partition",
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

static void on_info(struct kernel_page *p, bool ok, const char *out, const char *err) {
	g_free(p->info);
	p->info = g_strdup(out);
	show_info(p);
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

static void on_compression(GObject *dd, GParamSpec *pspec, gpointer data) {
	struct kernel_page *p = data;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->comp_dd));
	set_subtitle(p->comp_row, comp_hints[i < G_N_ELEMENTS(comp_hints) ? i : 0]);
	check_tools(p);
}

static void on_check(struct kernel_page *p, bool ok, const char *out, const char *err) {
	g_ptr_array_set_size(p->missing, 0);
	GString *names = g_string_new(NULL);
	char **lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *colon = strchr(lines[i], ':');
		if (!colon) {
			continue;
		}
		*colon = '\0';
		g_string_append_printf(names, "%s%s", names->len ? ", " : "", lines[i]);
		bool have = false;
		for (guint k = 0; k < p->missing->len; k++) {
			have = have || strcmp(p->missing->pdata[k], colon + 1) == 0;
		}
		if (!have) {
			g_ptr_array_add(p->missing, g_strdup(colon + 1));
		}
	}
	g_strfreev(lines);
	char *text = g_strdup_printf("Not installed: %s", names->str);
	set_subtitle(p->missing_row, text);
	g_free(text);
	gtk_widget_set_visible(p->missing_row, p->missing->len > 0);
	g_string_free(names, TRUE);
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

static void on_build(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	if (building(p)) {
		return;
	}
	guint v = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->version_dd));
	const char *version = v < p->versions->len ? p->versions->pdata[v] : "latest";
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

static void on_row_button(GtkButton *button, gpointer data) {
	struct kernel_page *p = data;
	const char *action = g_object_get_data(G_OBJECT(button), "action");
	const char *kver = g_object_get_data(G_OBJECT(button), "kver");
	bool installed = g_object_get_data(G_OBJECT(button), "installed") != NULL;
	if (strcmp(action, "install") == 0) {
		act(p, action, kver, installed);
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
	bool any = false, build_running = false;
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
		bool on = installed && strcmp(installed, "no") != 0;
		bool is_running = installed && strcmp(installed, "running") == 0;
		GString *sub = g_string_new(profile_name(profile));
		if (comp) {
			g_string_append_printf(sub, " · %s", comp);
		}
		if (patch && strcmp(patch, "on") == 0) {
			g_string_append(sub, " · live patching");
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
		g_strfreev(f);
	}
	g_strfreev(lines);
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
static gboolean on_tick(gpointer data) {
	struct kernel_page *p = data;
	if (gtk_widget_get_mapped(p->built_list) && !p->busy) {
		refresh_list(p);
	}
	return G_SOURCE_CONTINUE;
}

/* ---------- the page ---------- */

GtkWidget *kernel_page_new(struct settings *s) {
	struct kernel_page *p = g_new0(struct kernel_page, 1);
	p->s = s;
	p->versions = g_ptr_array_new_with_free_func(g_free);
	p->missing = g_ptr_array_new_with_free_func(g_free);
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

	run_root(p, on_info, "info", NULL, NULL);
	run_tool(p, on_versions, "versions", NULL);
	run_tool(p, on_foreign, "foreign", NULL);
	check_tools(p);
	refresh_list(p);
	p->timer = g_timeout_add_seconds(1, on_tick, p);
	return page;
}
