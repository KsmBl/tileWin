/*
 * The Processor page: how fast each core of the processor may run, its clock
 * limited to anything between its lowest and its highest, for a computer that
 * runs cooler, quieter and longer on its battery.
 *
 * A row per core (the threads of a core go together, as they share its clock),
 * the performance and efficient cores of Intel's hybrid processors in groups of
 * their own; a slider for all cores at once; Full speed takes every limit away.
 * Each row says how fast the core runs right now.
 *
 * The limits go through tilewin-cpu-root (pkexec), which writes them to
 * scaling_max_freq of each CPU and keeps them for the next start
 * (tilewin-cpu-limits.service). The page reads /sys/devices/system/cpu, or
 * $TILEWIN_CPU_ROOT/sys/... in the tests, where TILEWIN_CPU_ROOT_HELPER names
 * the helper to run with bash instead of pkexec.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

#define APPLY_MS 700 // after the last move of a slider
#define STEP_KHZ 100000

enum kind { CORE_PLAIN, CORE_PERFORMANCE, CORE_EFFICIENT };

struct core {
	int number;      // 1, 2, ... in the order of their first CPU
	GArray *cpus;    // int: the logical CPUs (threads) of this core
	int min_khz, max_khz;
	enum kind kind;
	GtkWidget *scale, *now;
	bool dirty;
};

struct cpu_page {
	struct settings *s;
	GPtrArray *cores;
	GtkWidget *all, *full;
	guint apply_timer, tick;
	bool updating, busy;
};

static char *sys_path(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static char *sys_path(const char *fmt, ...) {
	const char *root = g_getenv("TILEWIN_CPU_ROOT");
	va_list args;
	va_start(args, fmt);
	char *tail = g_strdup_vprintf(fmt, args);
	va_end(args);
	char *path = g_strdup_printf("%s/sys%s", root ? root : "", tail);
	g_free(tail);
	return path;
}

static long read_number(const char *path) {
	char *text = NULL;
	long value = -1;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		value = strtol(text, NULL, 10);
	}
	g_free(text);
	return value;
}

static long cpu_value(int cpu, const char *file) {
	char *path = sys_path("/devices/system/cpu/cpu%d/%s", cpu, file);
	long value = read_number(path);
	g_free(path);
	return value;
}

/* A CPU list as the kernel writes it ("0-15", "16-27", "0,2,4-7"). */
static bool in_list(const char *list, int cpu) {
	if (!list) {
		return false;
	}
	char **parts = g_strsplit(list, ",", -1);
	bool found = false;
	for (int i = 0; parts[i] && !found; i++) {
		int a = -1, b = -1;
		int n = sscanf(parts[i], "%d-%d", &a, &b);
		found = (n == 1 && cpu == a) || (n == 2 && cpu >= a && cpu <= b);
	}
	g_strfreev(parts);
	return found;
}

static char *read_line(const char *path) {
	char *text = NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		return NULL;
	}
	return g_strstrip(text);
}

static void core_free(gpointer data) {
	struct core *c = data;
	g_array_free(c->cpus, TRUE);
	g_free(c);
}

static int core_first(const struct core *c) {
	return g_array_index(c->cpus, int, 0);
}

static gint core_cmp(gconstpointer a, gconstpointer b) {
	return core_first(*(struct core **)a) - core_first(*(struct core **)b);
}

/* The cores, their threads together, in the order of their first CPU. */
static GPtrArray *find_cores(void) {
	GPtrArray *cores = g_ptr_array_new_with_free_func(core_free);
	GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	char *dir_path = sys_path("/devices/system/cpu");
	char *pcores_path = sys_path("/devices/cpu_core/cpus");
	char *ecores_path = sys_path("/devices/cpu_atom/cpus");
	char *pcores = read_line(pcores_path), *ecores = read_line(ecores_path);
	GDir *dir = g_dir_open(dir_path, 0, NULL);
	const char *name;
	while (dir && (name = g_dir_read_name(dir))) {
		int cpu;
		char rest;
		if (sscanf(name, "cpu%d%c", &cpu, &rest) != 1) {
			continue;
		}
		long low = cpu_value(cpu, "cpufreq/cpuinfo_min_freq");
		long high = cpu_value(cpu, "cpufreq/cpuinfo_max_freq");
		if (low <= 0 || high <= low) {
			continue; // offline, or no clock to set
		}
		long package = cpu_value(cpu, "topology/physical_package_id");
		long core_id = cpu_value(cpu, "topology/core_id");
		// without a topology, a CPU is a core of its own
		char *key = core_id < 0 ? g_strdup_printf("cpu%d", cpu) :
			g_strdup_printf("%ld:%ld", package, core_id);
		struct core *c = g_hash_table_lookup(by_key, key);
		if (!c) {
			c = g_new0(struct core, 1);
			c->cpus = g_array_new(FALSE, FALSE, sizeof(int));
			c->min_khz = (int)low;
			c->max_khz = (int)high;
			c->kind = in_list(pcores, cpu) ? CORE_PERFORMANCE :
				in_list(ecores, cpu) ? CORE_EFFICIENT : CORE_PLAIN;
			g_hash_table_insert(by_key, key, c);
			g_ptr_array_add(cores, c);
		} else {
			g_free(key);
			c->min_khz = MAX(c->min_khz, (int)low);
			c->max_khz = MIN(c->max_khz, (int)high);
		}
		g_array_append_val(c->cpus, cpu);
	}
	if (dir) {
		g_dir_close(dir);
	}
	for (guint i = 0; i < cores->len; i++) {
		struct core *c = cores->pdata[i];
		// the threads in order: a plain insertion sort, there are two at most
		int *v = (int *)c->cpus->data;
		for (guint a = 1; a < c->cpus->len; a++) {
			for (guint b = a; b > 0 && v[b - 1] > v[b]; b--) {
				int t = v[b];
				v[b] = v[b - 1];
				v[b - 1] = t;
			}
		}
	}
	g_ptr_array_sort(cores, core_cmp);
	for (guint i = 0; i < cores->len; i++) {
		((struct core *)cores->pdata[i])->number = (int)i + 1;
	}
	g_hash_table_destroy(by_key);
	g_free(dir_path);
	g_free(pcores_path);
	g_free(ecores_path);
	g_free(pcores);
	g_free(ecores);
	return cores;
}

static char *ghz(double khz) {
	return g_strdup_printf("%.1f GHz", khz / 1e6);
}

static char *format_value(GtkScale *scale, double value, gpointer data) {
	return ghz(value);
}

/* The limit a core has now: the highest of its threads'. */
static int core_limit(const struct core *c) {
	long limit = 0;
	for (guint i = 0; i < c->cpus->len; i++) {
		long v = cpu_value(g_array_index(c->cpus, int, i), "cpufreq/scaling_max_freq");
		limit = MAX(limit, v);
	}
	return limit > 0 ? (int)limit : c->max_khz;
}

/* How fast it runs this moment: the fastest of its threads. */
static long core_now(const struct core *c) {
	long now = 0;
	for (guint i = 0; i < c->cpus->len; i++) {
		long v = cpu_value(g_array_index(c->cpus, int, i), "cpufreq/scaling_cur_freq");
		now = MAX(now, v);
	}
	return now;
}

/* The steps of 100 MHz, and the highest clock itself at the end. */
static int snapped(const struct core *c, double value) {
	int khz = (int)(round(value / STEP_KHZ) * STEP_KHZ);
	return CLAMP(khz, c->min_khz, c->max_khz);
}

static char *helper_path(void) {
	const char *test = g_getenv("TILEWIN_CPU_ROOT_HELPER");
	return g_strdup(test && *test ? test : TILEWIN_LIBEXECDIR "/tilewin-cpu-root");
}

static void read_limits(struct cpu_page *p) {
	p->updating = true;
	int highest = 0;
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		int limit = core_limit(c);
		gtk_range_set_value(GTK_RANGE(c->scale), limit);
		c->dirty = false;
		highest = MAX(highest, limit);
	}
	if (p->all) {
		gtk_range_set_value(GTK_RANGE(p->all), highest);
	}
	p->updating = false;
}

static void on_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct cpu_page *p = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err,
		&error) && g_subprocess_get_successful(G_SUBPROCESS(source));
	p->busy = false;
	if (ok) {
		settings_status(p->s, "%s", out && *out ? g_strstrip(out) : "Limited");
	} else {
		const char *why = err && *g_strstrip(err) ? err : error ? error->message :
			"The limit was not set";
		settings_status(p->s, "Processor: %s", why);
	}
	g_clear_error(&error);
	g_free(out);
	g_free(err);
	read_limits(p); // what the cores have now, set or not
	gtk_widget_set_sensitive(p->full, TRUE);
}

static void run_helper(struct cpu_page *p, GPtrArray *args) {
	char *helper = helper_path();
	GPtrArray *argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_getenv("TILEWIN_CPU_ROOT_HELPER") ? "bash" : "pkexec");
	g_ptr_array_add(argv, helper);
	for (guint i = 0; i < args->len; i++) {
		g_ptr_array_add(argv, args->pdata[i]);
	}
	g_ptr_array_add(argv, NULL);
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv((const char *const *)argv->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	if (proc) {
		p->busy = true;
		gtk_widget_set_sensitive(p->full, FALSE);
		g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_done, p);
		g_object_unref(proc);
	} else {
		settings_status(p->s, "Processor: %s", error->message);
		g_error_free(error);
		read_limits(p);
	}
	g_ptr_array_free(argv, TRUE);
	g_free(helper);
}

/* "set CPU=KHZ..." for the cores moved since the last time, or NULL. */
static GPtrArray *moved_args(struct cpu_page *p) {
	GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(args, g_strdup("set"));
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		if (!c->dirty) {
			continue;
		}
		int khz = snapped(c, gtk_range_get_value(GTK_RANGE(c->scale)));
		for (guint t = 0; t < c->cpus->len; t++) {
			g_ptr_array_add(args, g_strdup_printf("%d=%d", g_array_index(c->cpus, int, t), khz));
		}
		c->dirty = false;
	}
	if (args->len <= 1) {
		g_ptr_array_free(args, TRUE);
		return NULL;
	}
	return args;
}

static gboolean apply_now(gpointer data) {
	struct cpu_page *p = data;
	p->apply_timer = 0;
	if (p->busy) {
		p->apply_timer = g_timeout_add(APPLY_MS, apply_now, p); // after this one
		return G_SOURCE_REMOVE;
	}
	GPtrArray *args = moved_args(p);
	if (args) {
		run_helper(p, args);
		g_ptr_array_free(args, TRUE);
	}
	return G_SOURCE_REMOVE;
}

static void apply_later(struct cpu_page *p) {
	if (p->apply_timer) {
		g_source_remove(p->apply_timer);
	}
	p->apply_timer = g_timeout_add(APPLY_MS, apply_now, p);
}

static void on_core(GtkRange *range, gpointer data) {
	struct cpu_page *p = data;
	if (p->updating) {
		return;
	}
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		if (GTK_RANGE(c->scale) == range) {
			c->dirty = true;
		}
	}
	apply_later(p);
}

static void on_all(GtkRange *range, gpointer data) {
	struct cpu_page *p = data;
	if (p->updating) {
		return;
	}
	double value = gtk_range_get_value(range);
	p->updating = true;
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		int khz = snapped(c, value);
		if ((int)gtk_range_get_value(GTK_RANGE(c->scale)) != khz) {
			gtk_range_set_value(GTK_RANGE(c->scale), khz);
			c->dirty = true;
		}
	}
	p->updating = false;
	apply_later(p);
}

static void on_full(GtkButton *button, gpointer data) {
	struct cpu_page *p = data;
	if (p->busy) {
		return;
	}
	if (p->apply_timer) {
		g_source_remove(p->apply_timer);
		p->apply_timer = 0;
	}
	GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(args, g_strdup("reset"));
	run_helper(p, args);
	g_ptr_array_free(args, TRUE);
}

static gboolean on_tick(gpointer data) {
	struct cpu_page *p = data;
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		long now = core_now(c);
		char *text = now > 0 ? g_strdup_printf("now %.1f GHz", now / 1e6) : g_strdup("");
		gtk_label_set_text(GTK_LABEL(c->now), text);
		g_free(text);
	}
	return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *widget, gpointer data) {
	struct cpu_page *p = data;
	if (!p->busy && !p->apply_timer) {
		read_limits(p); // changed elsewhere in the meantime
	}
	on_tick(p);
	if (!p->tick) {
		p->tick = g_timeout_add_seconds(1, on_tick, p);
	}
}

static void on_unmap(GtkWidget *widget, gpointer data) {
	struct cpu_page *p = data;
	if (p->tick) {
		g_source_remove(p->tick);
		p->tick = 0;
	}
}

static void on_destroy(GtkWidget *widget, gpointer data) {
	struct cpu_page *p = data;
	if (p->tick) {
		g_source_remove(p->tick);
	}
	if (p->apply_timer) {
		// what was moved last still counts, with nobody left to hear how it went
		g_source_remove(p->apply_timer);
		GPtrArray *args = moved_args(p);
		if (args) {
			char *helper = helper_path();
			g_ptr_array_insert(args, 0, helper);
			g_ptr_array_insert(args, 0, g_strdup(g_getenv("TILEWIN_CPU_ROOT_HELPER") ?
				"bash" : "pkexec"));
			g_ptr_array_add(args, NULL);
			g_spawn_async(NULL, (char **)args->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
				NULL, NULL);
			g_ptr_array_free(args, TRUE);
		}
	}
	g_ptr_array_free(p->cores, TRUE);
	g_free(p);
}

static GtkWidget *new_scale(int min_khz, int max_khz) {
	GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min_khz, max_khz,
		STEP_KHZ);
	gtk_range_set_increments(GTK_RANGE(scale), STEP_KHZ, 10 * STEP_KHZ);
	gtk_range_set_round_digits(GTK_RANGE(scale), 0);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
	gtk_scale_set_format_value_func(GTK_SCALE(scale), format_value, NULL, NULL);
	gtk_widget_set_size_request(scale, 260, -1);
	return scale;
}

static char *threads_text(const struct core *c) {
	GString *s = g_string_new(c->cpus->len == 1 ? "CPU " : "CPUs ");
	for (guint i = 0; i < c->cpus->len; i++) {
		g_string_append_printf(s, "%s%d", i == 0 ? "" : i + 1 == c->cpus->len ? " and " : ", ",
			g_array_index(c->cpus, int, i));
	}
	return g_string_free(s, FALSE);
}

static void add_cores(struct cpu_page *p, GtkWidget *content, enum kind kind, const char *title,
		const char *description) {
	GtkWidget *group = NULL;
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		if (c->kind != kind) {
			continue;
		}
		if (!group) {
			group = ui_group(content, title, description);
		}
		char *name = g_strdup_printf("Core %d", c->number);
		char *threads = threads_text(c);
		char *top = ghz(c->max_khz);
		char *sub = g_strdup_printf("%s, up to %s", threads, top);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		c->scale = new_scale(c->min_khz, c->max_khz);
		g_signal_connect(c->scale, "value-changed", G_CALLBACK(on_core), p);
		gtk_box_append(GTK_BOX(box), c->scale);
		c->now = gtk_label_new("");
		gtk_widget_add_css_class(c->now, "dim-label");
		gtk_label_set_width_chars(GTK_LABEL(c->now), 12);
		gtk_label_set_xalign(GTK_LABEL(c->now), 0);
		gtk_box_append(GTK_BOX(box), c->now);
		ui_row(group, name, sub, box);
		g_free(name);
		g_free(threads);
		g_free(top);
		g_free(sub);
	}
}

GtkWidget *cpu_page_new(struct settings *s) {
	struct cpu_page *p = g_new0(struct cpu_page, 1);
	p->s = s;
	p->cores = find_cores();
	GtkWidget *content;
	GtkWidget *page = ui_page("Processor",
		"How fast each core of the processor may run. A lower limit keeps the computer "
		"cooler and quieter and its battery lasting longer; the limits stay after a restart.",
		&content);
	g_signal_connect(page, "destroy", G_CALLBACK(on_destroy), p);
	g_signal_connect(page, "map", G_CALLBACK(on_map), p);
	g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), p);

	if (p->cores->len == 0) {
		GtkWidget *group = ui_group(content, "Cores", NULL);
		ui_empty_row(group, "computer-symbolic", "No clock to set",
			"The processor of this computer does not let Linux set the speed of its cores "
			"(no cpufreq driver), or it runs in a virtual machine.");
		return page;
	}

	int lowest = G_MAXINT, highest = 0;
	for (guint i = 0; i < p->cores->len; i++) {
		struct core *c = p->cores->pdata[i];
		lowest = MIN(lowest, c->min_khz);
		highest = MAX(highest, c->max_khz);
	}
	GtkWidget *all = ui_group(content, "All cores", NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	p->all = new_scale(lowest, highest);
	g_signal_connect(p->all, "value-changed", G_CALLBACK(on_all), p);
	gtk_box_append(GTK_BOX(box), p->all);
	p->full = gtk_button_new_with_label("Full speed");
	gtk_widget_set_tooltip_text(p->full, "Takes every limit away");
	g_signal_connect(p->full, "clicked", G_CALLBACK(on_full), p);
	gtk_box_append(GTK_BOX(box), p->full);
	ui_row(all, "Limit every core", "Each core to this, or to its own highest when that is "
		"lower", box);

	bool hybrid = false;
	for (guint i = 0; i < p->cores->len; i++) {
		hybrid = hybrid || ((struct core *)p->cores->pdata[i])->kind != CORE_PLAIN;
	}
	if (hybrid) {
		add_cores(p, content, CORE_PERFORMANCE, "Performance cores",
			"The P-cores: the fast ones, for what has to be done quickly");
		add_cores(p, content, CORE_EFFICIENT, "Efficient cores",
			"The E-cores: slower and frugal, for what runs in the background");
		add_cores(p, content, CORE_PLAIN, "Other cores", NULL);
	} else {
		add_cores(p, content, CORE_PLAIN, "Cores", NULL);
	}
	read_limits(p);
	return page;
}
