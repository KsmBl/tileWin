#include <dirent.h>
#include <dlfcn.h>
#include <pango/pangocairo.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "saver_util.h"
#include "stringop.h"
#include "tw_paths.h"
#include "twconf.h"

/*
 * The screen savers, each loaded from a directory of its own (see savers.h),
 * and what runs one.
 */

struct loaded {
	const struct saver_module *module;
	char *dir, *screenshot;
};

static struct loaded *loaded;
static int loaded_count;
static bool load_tried;
const struct saver **savers;
int saver_count;

const struct saver_module *saver_load_dir(const char *dir, char **why) {
	*why = NULL;
	char *path = format_str("%s/saver.so", dir);
	if (access(path, R_OK) != 0) {
		*why = format_str("%s: no saver.so in it", dir);
		free(path);
		return NULL;
	}
	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	free(path);
	if (!handle) {
		*why = format_str("%s: %s", dir, dlerror());
		return NULL;
	}
	const struct saver_module *m = dlsym(handle, "tilewin_saver_module");
	const struct saver *sv = m ? m->saver : NULL;
	if (!m || m->api != TILEWIN_SAVER_API || !sv || !sv->name || !*sv->name || !sv->title ||
			!sv->create || !sv->draw || !sv->destroy) {
		*why = format_str("%s: saver.so gives no tilewin_saver_module of version %d, with a "
			"name, a title, create, draw and destroy", dir, TILEWIN_SAVER_API);
		dlclose(handle);
		return NULL;
	}
	return m; // (stays loaded for as long as the program runs)
}

static int by_order(const void *a, const void *b) {
	const struct loaded *x = a, *y = b;
	int ox = x->module->order > 0 ? x->module->order : 1 << 20;
	int oy = y->module->order > 0 ? y->module->order : 1 << 20;
	return ox != oy ? ox - oy : strcasecmp(x->module->saver->title, y->module->saver->title);
}

static int by_name(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool known(const char *name) {
	for (int i = 0; i < loaded_count; i++) {
		if (strcasecmp(loaded[i].module->saver->name, name) == 0) {
			return true;
		}
	}
	return false;
}

/* The savers in the directories of one folder, in the order of their names. */
static void load_folder(const char *folder) {
	DIR *d = opendir(folder);
	if (!d) {
		return;
	}
	char **names = NULL;
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] != '.') {
			names = realloc(names, sizeof(char *) * (count + 1));
			names[count++] = strdup(e->d_name);
		}
	}
	closedir(d);
	qsort(names, count, sizeof(char *), by_name);
	for (int i = 0; i < count; i++) {
		char *dir = format_str("%s/%s", folder, names[i]), *why;
		struct stat st;
		if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			const struct saver_module *m = saver_load_dir(dir, &why);
			if (!m) {
				fprintf(stderr, "screen saver %s\n", why);
				free(why);
			} else if (!known(m->saver->name)) {
				loaded = realloc(loaded, sizeof(*loaded) * (loaded_count + 1));
				char *shot = format_str("%s/screenshot.png", dir);
				if (access(shot, R_OK) != 0) {
					free(shot);
					shot = NULL;
				}
				loaded[loaded_count++] = (struct loaded){ m, dir, shot };
				dir = NULL;
			}
		}
		free(dir);
		free(names[i]);
	}
	free(names);
}

void saver_load_all(void) {
	if (load_tried) {
		return;
	}
	load_tried = true;
	// the folders asked for, then the user's own, then those that come with tileWin
	const char *env = getenv("TILEWIN_SAVERS");
	if (env && *env) {
		char *copy = strdup(env), *save = NULL;
		for (char *f = strtok_r(copy, ":", &save); f; f = strtok_r(NULL, ":", &save)) {
			load_folder(f);
		}
		free(copy);
	}
	const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char *mine = data && data[0] == '/' ? format_str("%s/tileWin/screensavers", data) :
		home ? format_str("%s/.local/share/tileWin/screensavers", home) : NULL;
	if (mine) {
		load_folder(mine);
		free(mine);
	}
	char *theirs = format_str("%s/screensavers", tw_data_dir());
	load_folder(theirs);
	free(theirs);
	qsort(loaded, loaded_count, sizeof(*loaded), by_order);
	savers = malloc(sizeof(*savers) * (loaded_count + 1));
	for (int i = 0; i < loaded_count; i++) {
		savers[i] = loaded[i].module->saver;
	}
	saver_count = loaded_count;
}

static struct loaded *loaded_of(const struct saver *saver) {
	saver_load_all();
	for (int i = 0; i < loaded_count; i++) {
		if (loaded[i].module->saver == saver) {
			return &loaded[i];
		}
	}
	return NULL;
}

const char *saver_dir(const struct saver *saver) {
	struct loaded *l = loaded_of(saver);
	return l ? l->dir : NULL;
}

const char *saver_screenshot(const struct saver *saver) {
	struct loaded *l = loaded_of(saver);
	return l ? l->screenshot : NULL;
}

static uint64_t random_state;

double saver_random(void) {
	if (!random_state) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		random_state = (uint64_t)ts.tv_nsec * 2654435761u + (uint64_t)ts.tv_sec + 1;
	}
	// xorshift64*
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;
	return ((random_state * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}

void saver_hsv(double h, double s, double v, double *r, double *g, double *b) {
	h = fmod(h, 1.0);
	if (h < 0) {
		h += 1;
	}
	double i = floor(h * 6), f = h * 6 - i;
	double p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
	switch ((int)i % 6) {
	case 0: *r = v; *g = t; *b = p; break;
	case 1: *r = q; *g = v; *b = p; break;
	case 2: *r = p; *g = v; *b = t; break;
	case 3: *r = p; *g = q; *b = v; break;
	case 4: *r = t; *g = p; *b = v; break;
	default: *r = v; *g = p; *b = q; break;
	}
}

void saver_set_hsva(cairo_t *cr, double h, double s, double v, double a) {
	double r, g, b;
	saver_hsv(h, s, v, &r, &g, &b);
	cairo_set_source_rgba(cr, r, g, b, a);
}

/* The saver of the list that runs a hidden one of that name, and that one, or NULL. */
static const struct saver *hidden_saver(const char *name, const struct saver **listed) {
	for (int i = 0; i < loaded_count; i++) {
		for (const struct saver *const *h = loaded[i].module->hidden; h && *h; h++) {
			if (strcasecmp((*h)->name, name) == 0) {
				*listed = loaded[i].module->saver;
				return *h;
			}
		}
	}
	return NULL;
}

const struct saver *saver_find(const char *name) {
	saver_load_all();
	if (!name || !*name || strcasecmp(name, "none") == 0) {
		return NULL;
	}
	if (strcasecmp(name, "random") == 0) {
		// Blank is no surprise worth waiting for
		int others = 0;
		for (int i = 0; i < saver_count; i++) {
			others += strcmp(savers[i]->name, "blank") != 0;
		}
		int pick = (int)(saver_random() * others);
		for (int i = 0; i < saver_count; i++) {
			if (strcmp(savers[i]->name, "blank") != 0 && pick-- == 0) {
				return savers[i];
			}
		}
		return NULL;
	}
	for (int i = 0; i < saver_count; i++) {
		if (strcasecmp(savers[i]->name, name) == 0) {
			return savers[i];
		}
	}
	const struct saver *listed;
	return hidden_saver(name, &listed);
}

const struct saver *saver_listed(const char *name) {
	saver_load_all();
	const struct saver *listed = NULL;
	if (name && hidden_saver(name, &listed)) {
		return listed;
	}
	return name && strcasecmp(name, "random") != 0 ? saver_find(name) : NULL;
}

/* ---------- running one ---------- */

struct saver_run {
	const struct saver *saver;
	void *state;
	int width, height;         // of the area
	int inner_w, inner_h;      // what the saver draws on
	cairo_surface_t *inner;    // for a saver drawn smaller and scaled up
	double speed;
	struct saver_stats *stats; // with show_stats, or NULL
};

/* ---------- frames a second and the processor ---------- */

struct saver_stats {
	double since;              // when the figures were last worked out
	int frames;                // drawn since
	double busy;               // seconds spent drawing them
	double process_cpu;        // this program's processor time then
	unsigned long long all, idle; // of the whole processor then, from /proc/stat
	char text[128];
	PangoLayout *layout;
};

static double monotonic(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double process_cpu(void) {
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The time the whole processor has run and idled so far, in ticks; false without /proc. */
static bool cpu_ticks(unsigned long long *all, unsigned long long *idle) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return false;
	}
	unsigned long long v[8] = { 0 };
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
		&v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 4) {
		return false;
	}
	*all = 0;
	for (int i = 0; i < 8; i++) {
		*all += v[i];
	}
	*idle = v[3] + v[4]; // idle and waiting for the disk
	return true;
}

static struct saver_stats *stats_new(void) {
	struct saver_stats *st = calloc(1, sizeof(*st));
	st->since = monotonic();
	st->process_cpu = process_cpu();
	cpu_ticks(&st->all, &st->idle);
	snprintf(st->text, sizeof(st->text), "… fps\nCPU …");
	return st;
}

/* Counts a frame that took busy seconds; once a second the figures are worked out anew. */
static void stats_frame(struct saver_stats *st, double busy) {
	st->frames++;
	st->busy += busy;
	double now = monotonic(), span = now - st->since;
	if (span < 1) {
		return;
	}
	double cpu = process_cpu();
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	// this program's share of the whole machine, so it compares with the figure for all
	double own = (cpu - st->process_cpu) / span / (cores > 0 ? cores : 1) * 100;
	unsigned long long all = 0, idle = 0;
	double total = -1;
	if (cpu_ticks(&all, &idle) && all > st->all) {
		total = 100.0 * (1 - (double)(idle - st->idle) / (double)(all - st->all));
	}
	if (total >= 0) {
		snprintf(st->text, sizeof(st->text), "%.0f fps · %.1f ms a frame\nCPU %.0f%% · "
			"the screen saver %.1f%%", st->frames / span, st->busy / st->frames * 1000,
			total, own);
	} else {
		snprintf(st->text, sizeof(st->text), "%.0f fps · %.1f ms a frame\n"
			"the screen saver %.1f%% CPU", st->frames / span, st->busy / st->frames * 1000, own);
	}
	if (st->layout) {
		pango_layout_set_text(st->layout, st->text, -1);
	}
	st->since = now;
	st->frames = 0;
	st->busy = 0;
	st->process_cpu = cpu;
	st->all = all;
	st->idle = idle;
}

/* The figures in the top left corner, on a dark plate, sized to the area. */
static void stats_draw(struct saver_stats *st, cairo_t *cr, int width, int height) {
	double size = fmax(7, fmin(width, height) / 55.0);
	if (!st->layout) {
		st->layout = pango_cairo_create_layout(cr);
		PangoFontDescription *font = pango_font_description_from_string("monospace");
		pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
		pango_layout_set_font_description(st->layout, font);
		pango_font_description_free(font);
		pango_layout_set_text(st->layout, st->text, -1);
	} else {
		pango_cairo_update_layout(cr, st->layout);
	}
	int tw, th;
	pango_layout_get_pixel_size(st->layout, &tw, &th);
	double pad = size * 0.6, x = size, y = size;
	cairo_save(cr);
	cairo_new_path(cr);
	double r = size * 0.4, w = tw + 2 * pad, h = th + 2 * pad;
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	cairo_fill(cr);
	cairo_move_to(cr, x + pad, y + pad);
	cairo_set_source_rgb(cr, 0.95, 0.97, 1);
	pango_cairo_show_layout(cr, st->layout);
	cairo_restore(cr);
}

static void stats_free(struct saver_stats *st) {
	if (st && st->layout) {
		g_object_unref(st->layout);
	}
	free(st);
}

struct saver_run *saver_run_new(const struct saver *saver, int width, int height,
		const struct saver_options *options) {
	struct saver_run *run = calloc(1, sizeof(*run));
	run->saver = saver;
	run->width = width;
	run->height = height;
	run->speed = options && options->speed > 0 ? saver_clamp(options->speed, 0.25, 4) : 1;
	double res = saver->resolution > 0 && saver->resolution < 1 ? saver->resolution : 1;
	run->inner_w = (int)ceil(width * res);
	run->inner_h = (int)ceil(height * res);
	if (res < 1) {
		run->inner = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, run->inner_w, run->inner_h);
	}
	if (options && options->show_stats) {
		run->stats = stats_new();
	}
	struct saver_options defaults = { .speed = 1, .photo_seconds = 8 };
	run->state = saver->create(run->inner_w, run->inner_h, options ? options : &defaults);
	return run;
}

void saver_run_draw(struct saver_run *run, cairo_t *cr, double seconds) {
	double dt = saver_clamp(seconds, 0, 0.25) * run->speed; // a stall is no leap
	double started = run->stats ? monotonic() : 0;
	cairo_t *target = cr;
	if (run->inner) {
		target = cairo_create(run->inner);
	}
	if (!run->saver->covers) { // (a whole screen of nothing costs as much as a picture)
		cairo_save(target);
		cairo_set_operator(target, CAIRO_OPERATOR_SOURCE);
		if (run->saver->transparent) {
			cairo_set_source_rgba(target, 0, 0, 0, 0);
		} else {
			cairo_set_source_rgb(target, 0, 0, 0);
		}
		cairo_paint(target);
		cairo_restore(target);
	}
	cairo_save(target);
	run->saver->draw(run->state, target, run->inner_w, run->inner_h, dt);
	cairo_restore(target);
	if (run->inner) {
		cairo_destroy(target);
		cairo_surface_flush(run->inner);
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_scale(cr, (double)run->width / run->inner_w, (double)run->height / run->inner_h);
		cairo_set_source_surface(cr, run->inner, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	if (run->stats) {
		stats_frame(run->stats, monotonic() - started);
		stats_draw(run->stats, cr, run->width, run->height);
	}
}

void saver_run_free(struct saver_run *run) {
	if (!run) {
		return;
	}
	run->saver->destroy(run->state);
	stats_free(run->stats);
	if (run->inner) {
		cairo_surface_destroy(run->inner);
	}
	free(run);
}

/* ---------- options ---------- */

/*
 * What is written for a setting of a saver, or NULL. A key that names a saver
 * itself, like Doomsday's "hellfire_flash", is found without the prefix too:
 * that is how it was written while it was a saver of its own.
 */
static const char *setting(const struct saver_options *options, const struct saver *saver,
		const char *key) {
	size_t name_len = strlen(saver->name);
	const char *bare = NULL;
	for (int i = 0; options && i + 1 < options->setting_count; i += 2) {
		const char *name = options->settings[i];
		if (strncmp(name, saver->name, name_len) == 0 && name[name_len] == '_' &&
				strcmp(name + name_len + 1, key) == 0) {
			return options->settings[i + 1];
		}
		if (strchr(key, '_') && strcmp(name, key) == 0) {
			bare = options->settings[i + 1];
		}
	}
	return bare;
}

static const struct saver_option *find_option(const struct saver *saver, const char *key) {
	for (const struct saver_option *o = saver->options; o && o->key; o++) {
		if (strcmp(o->key, key) == 0) {
			return o;
		}
	}
	return NULL;
}

int saver_choice(const struct saver_options *options, const struct saver *saver,
		const char *key) {
	const struct saver_option *o = find_option(saver, key);
	const char *value = setting(options, saver, key);
	for (int i = 0; o && value && o->values[i]; i++) {
		if (strcasecmp(o->values[i], value) == 0) {
			return i;
		}
	}
	return 0;
}

bool saver_option_shown(const struct saver_options *options, const struct saver *saver,
		const struct saver_option *option) {
	const char *eq = option->when ? strchr(option->when, '=') : NULL;
	if (!eq) {
		return true;
	}
	char key[64];
	snprintf(key, sizeof(key), "%.*s", (int)(eq - option->when), option->when);
	const struct saver_option *o = find_option(saver, key);
	return o && o->values && strcasecmp(o->values[saver_choice(options, saver, key)], eq + 1) == 0;
}

bool saver_toggle(const struct saver_options *options, const struct saver *saver,
		const char *key) {
	const struct saver_option *o = find_option(saver, key);
	const char *value = setting(options, saver, key);
	if (!value) {
		return o ? o->on : false;
	}
	return strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 ||
		strcasecmp(value, "true") == 0;
}

char *saver_options_load(struct saver_options *options) {
	*options = (struct saver_options){ .speed = 1, .photo_seconds = 8 };
	char *dir = tw_config_dir();
	char *path = dir ? format_str("%s/taskbar.conf", dir) : NULL;
	free(dir);
	char *error = NULL;
	struct twconf_node *root = path ? twconf_parse_file(path, &error) : NULL;
	free(error);
	free(path);
	struct twconf_node *block = root ? twconf_child(root, "screensaver") : NULL;
	char *name = NULL;
	if (block) {
		const char *value = twconf_value(block, "name");
		name = value ? strdup(value) : NULL;
		value = twconf_value(block, "speed");
		if (value) {
			options->speed = saver_clamp(strtod(value, NULL), 0.25, 4);
		}
		value = twconf_value(block, "text");
		options->text = value ? strdup(value) : NULL;
		value = twconf_value(block, "photos");
		options->photos = value ? tw_expand_home(value) : NULL;
		value = twconf_value(block, "stats");
		options->show_stats = value && (strcasecmp(value, "yes") == 0 ||
			strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0);
		value = twconf_value(block, "photo_seconds");
		if (value && atoi(value) >= 2) {
			options->photo_seconds = atoi(value);
		}
		// the savers' own settings, whatever they are: each saver looks for its own
		int count = twconf_count(block);
		const char **settings = calloc(2 * count + 1, sizeof(char *));
		int n = 0;
		for (int i = 0; i < count; i++) {
			struct twconf_node *node = twconf_at(block, i);
			if (node->argc >= 1 && strchr(node->name, '_')) {
				settings[n++] = strdup(node->name);
				settings[n++] = strdup(node->argv[0]);
			}
		}
		options->settings = settings;
		options->setting_count = n;
	}
	twconf_free(root);
	return name;
}

void saver_options_finish(struct saver_options *options) {
	free((char *)options->text);
	free((char *)options->photos);
	options->text = options->photos = NULL;
	for (int i = 0; i < options->setting_count; i++) {
		free((char *)options->settings[i]);
	}
	free((void *)options->settings);
	options->settings = NULL;
	options->setting_count = 0;
}
