#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/arrange.h"
#include "sway/tree/root.h"
#include "log.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "tw_paths.h"

enum tw_mode tw_mode = TW_MODE_WINDOW;
struct tw_theme *tw_theme = NULL;
int tw_theme_generation = 1;

static enum tw_mode pending_mode;
static struct tw_theme *pending_theme; // theme of the mode switched to
static int wallpaper_generation = 1;

const char *tw_mode_name(enum tw_mode mode) {
	return mode == TW_MODE_WINDOW ? "window" : "tile";
}

bool tw_parse_mode(const char *name, enum tw_mode *mode) {
	if (strcasecmp(name, "window") == 0 || strcasecmp(name, "windows") == 0) {
		*mode = TW_MODE_WINDOW;
		return true;
	}
	if (strcasecmp(name, "tile") == 0 || strcasecmp(name, "tiling") == 0) {
		*mode = TW_MODE_TILE;
		return true;
	}
	return false;
}

bool tw_is_window_mode(void) {
	return tw_mode == TW_MODE_WINDOW;
}

static char *mode_state_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	char *path = format_str("%s/mode", dir);
	free(dir);
	return path;
}

static void save_mode(void) {
	char *path = mode_state_path();
	if (path) {
		char *content = format_str("%s\n", tw_mode_name(tw_mode));
		tw_write_string(path, content);
		free(content);
		free(path);
	}
}

static struct tw_theme *load_theme_or_fallback(const char *name) {
	char *error = NULL;
	struct tw_theme *theme = tw_theme_load(name, &error);
	if (!theme) {
		sway_log(SWAY_ERROR, "Failed to load theme '%s': %s", name,
			error ? error : "unknown error");
		free(error);
		error = NULL;
		if (strcmp(name, TW_DEFAULT_THEME) != 0) {
			theme = tw_theme_load(TW_DEFAULT_THEME, &error);
			free(error);
		}
	}
	if (!theme) {
		// built-in renderer defaults still work without any keys
		theme = calloc(1, sizeof(*theme));
		theme->kv = create_list();
		theme->name = strdup(TW_DEFAULT_THEME);
		theme->title = strdup("Windows 10 (built-in)");
		theme->style = strdup("win10");
	}
	return theme;
}

static void apply_app_color_scheme(bool dark);
static void apply_app_icons(void);

void tw_init(const char *mode_override) {
	enum tw_mode mode = TW_MODE_WINDOW;
	if (mode_override) {
		if (!tw_parse_mode(mode_override, &mode)) {
			sway_log(SWAY_ERROR, "Unknown mode '%s', using window mode", mode_override);
		}
	} else {
		char *path = mode_state_path();
		char *saved = path ? tw_read_first_line(path) : NULL;
		if (saved) {
			tw_parse_mode(saved, &mode);
		}
		free(saved);
		free(path);
	}
	tw_mode = mode;

	char *name = tw_theme_mode_name(tw_mode_name(tw_mode));
	tw_theme = load_theme_or_fallback(name);
	free(name);
	// the taskbar and tools read the active theme from current-theme
	tw_theme_save_current(tw_theme->name);
	char *icons = tw_theme_icon_dir(tw_theme);
	tw_icon_set_theme_dir(icons);
	free(icons);
	if (tw_color_scheme_is_set()) {
		// apps may have been changed by another desktop since the last session
		apply_app_color_scheme(tw_color_scheme_is_dark());
	}
	apply_app_icons();
	sway_log(SWAY_INFO, "tileWin starting in %s mode with theme %s",
		tw_mode_name(tw_mode), tw_theme->name);
}

void tw_fini(void) {
	tw_icon_cache_clear();
	tw_theme_free(tw_theme);
	tw_theme = NULL;
}

void tw_load_theme_tile_config(struct sway_config *cfg) {
	if (!tw_theme || !tw_theme->dir) {
		return;
	}
	char *path = tw_theme_file(tw_theme, "tile.conf");
	if (access(path, R_OK) == 0) {
		load_include_configs(path, cfg, &cfg->swaynag_config_errors);
	}
	free(path);
}

void tw_add_default_bindings(struct sway_config *cfg) {
	static const char *defaults[][2] = {
		{ "XF86AudioRaiseVolume", "volume-up" },
		{ "XF86AudioLowerVolume", "volume-down" },
		{ "XF86AudioMute", "mute" },
		{ "XF86AudioMicMute", "mic-mute" },
		{ "XF86MonBrightnessUp", "brightness-up" },
		{ "XF86MonBrightnessDown", "brightness-down" },
		{ "XF86AudioPlay", "play-pause" },
		{ "XF86AudioPause", "play-pause" },
		{ "XF86AudioNext", "next" },
		{ "XF86AudioPrev", "previous" },
		{ "XF86AudioStop", "stop" },
	};
	if (!cfg->modes || cfg->modes->length == 0) {
		return;
	}
	struct sway_mode *mode = cfg->modes->items[0]; // "default"
	for (int j = 0; j < mode->keysym_bindings->length; j++) {
		struct sway_binding *binding = mode->keysym_bindings->items[j];
		// configs from before the task view bound Win+Tab to Alt+Tab
		if (binding->modifiers == WLR_MODIFIER_LOGO && binding->keys->length == 1 &&
				*(xkb_keysym_t *)binding->keys->items[0] == XKB_KEY_Tab &&
				binding->command && strcmp(binding->command, "alttab next") == 0) {
			free(binding->command);
			binding->command = strdup("taskview");
		}
	}
	struct sway_mode *current = cfg->current_mode;
	cfg->current_mode = mode;
	for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		xkb_keysym_t sym = xkb_keysym_from_name(defaults[i][0], XKB_KEYSYM_NO_FLAGS);
		bool bound = false;
		for (int j = 0; j < mode->keysym_bindings->length && !bound; j++) {
			struct sway_binding *binding = mode->keysym_bindings->items[j];
			bound = binding->modifiers == 0 && binding->keys->length == 1 &&
				*(xkb_keysym_t *)binding->keys->items[0] == sym;
		}
		if (bound) {
			continue;
		}
		char *cmd = format_str("bindsym --locked --no-warn %s exec tilewin-media %s",
			defaults[i][0], defaults[i][1]);
		struct cmd_results *result = config_command(cmd, NULL);
		if (result && result->status != CMD_SUCCESS) {
			sway_log(SWAY_ERROR, "Default binding '%s' failed: %s", cmd,
				result->error ? result->error : "?");
		}
		free_cmd_results(result);
		free(cmd);
	}
	cfg->current_mode = current;
}

json_object *tw_describe_state(void) {
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "mode", json_object_new_string(tw_mode_name(tw_mode)));
	json_object_object_add(obj, "theme",
		json_object_new_string(tw_theme ? tw_theme->name : ""));
	for (int i = 0; i < 2; i++) {
		const char *mode = i == 0 ? "window" : "tile";
		char *name = tw_theme_mode_name(mode);
		char *key = format_str("theme_%s", mode);
		json_object_object_add(obj, key, json_object_new_string(name));
		free(key);
		free(name);
	}
	json_object_object_add(obj, "theme_title",
		json_object_new_string(tw_theme && tw_theme->title ? tw_theme->title : ""));
	json_object_object_add(obj, "color_scheme",
		json_object_new_string(tw_theme && tw_theme->dark ? "dark" : "light"));
	json_object_object_add(obj, "theme_style",
		json_object_new_string(tw_theme && tw_theme->style ? tw_theme->style : ""));
	json_object_object_add(obj, "theme_dir",
		tw_theme && tw_theme->dir ? json_object_new_string(tw_theme->dir) : NULL);
	json_object_object_add(obj, "data_dir", json_object_new_string(tw_data_dir()));
	json_object_object_add(obj, "version", json_object_new_string(SWAY_VERSION));
	json_object_object_add(obj, "panel_pid", json_object_new_int(tw_panel_pid()));
	return obj;
}

static void emit_state_event(const char *change) {
	ipc_event_tilewin(change, tw_describe_state());
}

void tw_after_reload(void) {
	tw_wallpaper_invalidate();
	tw_panel_config_reloaded();
	apply_app_icons();
}

static void mark_container_dirty(struct sway_container *con, void *data);

/* Makes theme the active theme; the config has to be reloaded afterwards. */
static void apply_theme(struct tw_theme *theme) {
	struct tw_theme *old = tw_theme;
	tw_theme = theme;
	if (old != theme) {
		tw_theme_free(old);
	}
	tw_theme_generation++;
	char *icons = tw_theme_icon_dir(theme);
	tw_icon_set_theme_dir(icons);
	free(icons);
	tw_icon_cache_clear();
	tw_theme_save_current(theme->name);
}

static void do_mode_switch(void *data) {
	enum tw_mode mode = pending_mode;
	struct tw_theme *theme = pending_theme;
	pending_theme = NULL;
	if (mode == tw_mode) {
		tw_theme_free(theme);
		return;
	}
	tw_mode = mode;
	save_mode();
	if (theme) {
		apply_theme(theme);
	}
	reload_config_now();
	if (mode == TW_MODE_WINDOW) {
		tw_convert_to_window_mode();
	} else {
		tw_convert_to_tile_mode();
	}
	if (theme) {
		root_for_each_container(mark_container_dirty, NULL);
	}
	arrange_root();
	transaction_commit_dirty();
	emit_state_event("mode");
	if (theme) {
		emit_state_event("theme");
	}
}

bool tw_request_mode(enum tw_mode mode, char **error) {
	if (mode == tw_mode) {
		return true;
	}
	// every mode switches to the theme it used last
	char *name = tw_theme_mode_name(tw_mode_name(mode));
	struct tw_theme *theme = NULL;
	if (!tw_theme || strcmp(name, tw_theme->name) != 0) {
		char *theme_error = NULL;
		theme = tw_theme_load(name, &theme_error);
		if (!theme) {
			sway_log(SWAY_ERROR, "Keeping the theme, cannot load '%s': %s", name,
				theme_error ? theme_error : "unknown error");
		}
		free(theme_error);
	}
	free(name);

	const char *path = config->user_config_path ? config->current_config_path : NULL;
	enum tw_mode old = tw_mode;
	struct tw_theme *old_theme = tw_theme;
	tw_mode = mode;
	if (theme) {
		tw_theme = theme;
	}
	bool valid = load_main_config(path, true, true);
	if (!valid && theme) {
		// the mode's config may have errors with that theme: keep the current one
		tw_theme = old_theme;
		tw_theme_free(theme);
		theme = NULL;
		valid = load_main_config(path, true, true);
	}
	tw_mode = old;
	tw_theme = old_theme;
	if (!valid) {
		tw_theme_free(theme);
		*error = format_str("The %s mode config has errors, not switching",
			tw_mode_name(mode));
		return false;
	}
	tw_theme_free(pending_theme);
	pending_theme = theme;
	pending_mode = mode;
	wl_event_loop_add_idle(server.wl_event_loop, do_mode_switch, NULL);
	return true;
}

static void mark_container_dirty(struct sway_container *con, void *data) {
	con->tw.frame_cache.valid = false;
	node_set_dirty(&con->node);
}

static void do_theme_switch(void *data) {
	reload_config_now();
	root_for_each_container(mark_container_dirty, NULL);
	arrange_root();
	transaction_commit_dirty();
	emit_state_event("theme");
}

/* Runs tilewin-color-scheme so GTK, GNOME and KDE apps follow the scheme. */
static void apply_app_color_scheme(bool dark) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execlp("tilewin-color-scheme", "tilewin-color-scheme", dark ? "dark" : "light",
				(char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

/*
 * Runs tilewin-app-icons so file managers and other apps use the theme's
 * icons (or the icon theme it names), unless turned off.
 */
static void apply_app_icons(void) {
	if (!tw_theme || getenv("TILEWIN_NO_APP_TWEAKS")) {
		return;
	}
	char *dir = tw_theme_icon_dir(tw_theme);
	const char *set = tw_theme_str(tw_theme, "icons.set", tw_theme->name);
	const char *fallback = tw_theme_str(tw_theme, "icons.theme", NULL);
	if (!fallback || strcmp(fallback, "hicolor") == 0) {
		fallback = "-";
	}
	const char *title = tw_theme->title ? tw_theme->title : tw_theme->name;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execlp("tilewin-app-icons", "tilewin-app-icons", "apply", set, dir ? dir : "-",
				fallback, title, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
	free(dir);
}

bool tw_set_color_scheme(bool dark, char **error) {
	if (!tw_color_scheme_save(dark)) {
		*error = strdup("Cannot save the color scheme");
		return false;
	}
	apply_app_color_scheme(dark);
	return tw_request_theme(tw_theme ? tw_theme->name : TW_DEFAULT_THEME, error);
}

bool tw_request_theme(const char *name, char **error) {
	struct tw_theme *theme = tw_theme_load(name, error);
	if (!theme) {
		return false;
	}
	struct tw_theme *old = tw_theme;
	tw_theme = theme;
	const char *path = config->user_config_path ? config->current_config_path : NULL;
	if (!load_main_config(path, true, true)) {
		tw_theme = old;
		tw_theme_free(theme);
		*error = strdup("Config has errors with this theme, not switching");
		return false;
	}
	tw_theme = old;
	// save for the mode before current-theme changes, the other mode keeps its theme
	tw_theme_save_mode(tw_mode_name(tw_mode), theme->name);
	apply_theme(theme);
	wl_event_loop_add_idle(server.wl_event_loop, do_theme_switch, NULL);
	return true;
}

bool tw_set_mode_theme(enum tw_mode mode, const char *name, char **error) {
	if (mode == tw_mode) {
		return tw_request_theme(name, error);
	}
	struct tw_theme *theme = tw_theme_load(name, error);
	if (!theme) {
		return false;
	}
	bool ok = tw_theme_save_mode(tw_mode_name(mode), theme->name);
	tw_theme_free(theme);
	if (!ok) {
		*error = strdup("Cannot save the theme");
		return false;
	}
	emit_state_event("theme");
	return true;
}

/* ---------- wallpaper ---------- */

enum wallpaper_type {
	WALLPAPER_NONE,
	WALLPAPER_SOLID,
	WALLPAPER_GRADIENT,
	WALLPAPER_IMAGE,
};

struct wallpaper_spec {
	enum wallpaper_type type;
	uint32_t color1, color2;
	bool vertical;
	char *image;
	char *mode; // fill, fit, stretch, center, tile
};

static void wallpaper_spec_finish(struct wallpaper_spec *spec) {
	free(spec->image);
	free(spec->mode);
}

static enum wallpaper_type parse_type(const char *s) {
	if (!s || strcasecmp(s, "none") == 0) {
		return WALLPAPER_NONE;
	}
	if (strcasecmp(s, "solid") == 0 || strcasecmp(s, "color") == 0) {
		return WALLPAPER_SOLID;
	}
	if (strcasecmp(s, "gradient") == 0) {
		return WALLPAPER_GRADIENT;
	}
	if (strcasecmp(s, "image") == 0) {
		return WALLPAPER_IMAGE;
	}
	return WALLPAPER_NONE;
}

static void wallpaper_spec_get(struct wallpaper_spec *spec) {
	memset(spec, 0, sizeof(*spec));
	spec->color1 = 0x000000ff;
	const char *override = config ? config->tw_wallpaper : NULL;
	if (override && strcasecmp(override, "theme") != 0) {
		int argc = 0;
		char **argv = split_args(override, &argc);
		spec->type = argc > 0 ? parse_type(argv[0]) : WALLPAPER_NONE;
		if (spec->type == WALLPAPER_SOLID || spec->type == WALLPAPER_GRADIENT) {
			if (argc > 1) {
				tw_parse_color(argv[1], &spec->color1);
			}
			spec->color2 = spec->color1;
			if (argc > 2) {
				tw_parse_color(argv[2], &spec->color2);
			}
			spec->vertical = !(argc > 3 && strcasecmp(argv[3], "horizontal") == 0);
		} else if (spec->type == WALLPAPER_IMAGE && argc > 1) {
			spec->image = tw_expand_home(argv[1]);
			spec->mode = strdup(argc > 2 ? argv[2] : "fill");
			if (argc > 3) {
				tw_parse_color(argv[3], &spec->color1);
			}
		}
		free_argv(argc, argv);
		return;
	}
	// ~/.config/tileWin/wallpapers/<theme>.<ext> replaces the theme's wallpaper
	char *config_dir = tw_config_dir();
	static const char *exts[] = { "jpg", "jpeg", "png", "webp", "svg" };
	for (size_t i = 0; config_dir && tw_theme && i < sizeof(exts) / sizeof(exts[0]); i++) {
		char *path = format_str("%s/wallpapers/%s.%s", config_dir, tw_theme->name, exts[i]);
		if (access(path, R_OK) == 0) {
			spec->type = WALLPAPER_IMAGE;
			spec->image = path;
			spec->mode = strdup("fill");
			spec->color1 = tw_theme_color(tw_theme, "wallpaper.color", 0x000000ff);
			free(config_dir);
			return;
		}
		free(path);
	}
	free(config_dir);
	spec->type = parse_type(tw_theme_str(tw_theme, "wallpaper.type", "solid"));
	spec->color1 = tw_theme_color(tw_theme, "wallpaper.color", 0x3a6ea5ff);
	spec->color2 = tw_theme_color(tw_theme, "wallpaper.color2", spec->color1);
	spec->vertical = strcasecmp(tw_theme_str(tw_theme, "wallpaper.direction", "vertical"),
		"horizontal") != 0;
	const char *image = tw_theme_str(tw_theme, "wallpaper.image", NULL);
	if (image) {
		if (image[0] == '/' || image[0] == '~' || !tw_theme->dir) {
			spec->image = tw_expand_home(image);
		} else {
			spec->image = tw_theme_file(tw_theme, image);
		}
	}
	spec->mode = strdup(tw_theme_str(tw_theme, "wallpaper.mode", "fill"));
}

/*
 * Renders a wallpaper at the output's pixel size. The result is cached as a
 * PNG in ~/.cache/tileWin so SVG wallpapers are only rendered once per size.
 */
static cairo_surface_t *wallpaper_render_cached(const char *path, int width, int height) {
	// very large outputs are upscaled from at most 3840 pixels wide
	if (width > 3840) {
		height = height * 3840 / width;
		width = 3840;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return NULL;
	}
	unsigned long hash = 5381;
	for (const char *p = path; *p; p++) {
		hash = hash * 33 + (unsigned char)*p;
	}
	hash = hash * 33 + (unsigned long)st.st_mtime;
	hash = hash * 33 + (unsigned long)st.st_size;

	char *cache_dir = tw_cache_dir();
	char *cache = cache_dir ? format_str("%s/wallpapers/%lx-%dx%d.png", cache_dir, hash,
		width, height) : NULL;
	free(cache_dir);
	if (cache && access(cache, R_OK) == 0) {
		cairo_surface_t *surface = cairo_image_surface_create_from_png(cache);
		if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS &&
				cairo_image_surface_get_width(surface) == width) {
			free(cache);
			return surface;
		}
		cairo_surface_destroy(surface);
	}
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	cairo_surface_t *surface = tw_image_render_cover(path, width, height);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sway_log(SWAY_DEBUG, "Rendered wallpaper %s at %dx%d in %ld ms", path, width, height,
		(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
	// image decoders and SVG filters leave large freed heap blocks behind
	malloc_trim(0);
	if (surface && cache) {
		char *slash = strrchr(cache, '/');
		*slash = '\0';
		tw_mkdir_p(cache);
		*slash = '/';
		cairo_surface_write_to_png(surface, cache);
	}
	free(cache);
	return surface;
}

static void set_rect_color(struct wlr_scene_rect *rect, uint32_t c) {
	float color[4] = {
		(c >> 24 & 0xff) / 255.0f, (c >> 16 & 0xff) / 255.0f,
		(c >> 8 & 0xff) / 255.0f, 1.0f,
	};
	wlr_scene_rect_set_color(rect, color);
}

void tw_wallpaper_update(struct sway_output *output) {
	if (!output || !output->wlr_output || !output->layers.shell_background) {
		return;
	}
	int w = output->width, h = output->height;
	float scale = output->wlr_output->scale;
	if (output->tw_wallpaper_generation == wallpaper_generation &&
			output->tw_wallpaper_width == w && output->tw_wallpaper_height == h &&
			output->tw_wallpaper_scale == scale) {
		return;
	}
	output->tw_wallpaper_generation = wallpaper_generation;
	output->tw_wallpaper_width = w;
	output->tw_wallpaper_height = h;
	output->tw_wallpaper_scale = scale;

	if (output->tw_wallpaper) {
		wlr_scene_node_destroy(&output->tw_wallpaper->node);
		output->tw_wallpaper = NULL;
	}
	if (w <= 0 || h <= 0) {
		return;
	}

	struct wallpaper_spec spec;
	wallpaper_spec_get(&spec);
	if (spec.type == WALLPAPER_NONE) {
		wallpaper_spec_finish(&spec);
		return;
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_create(output->layers.shell_background);
	if (!tree) {
		wallpaper_spec_finish(&spec);
		return;
	}
	output->tw_wallpaper = tree;
	wlr_scene_node_lower_to_bottom(&tree->node);

	struct wlr_scene_rect *base = wlr_scene_rect_create(tree, w, h,
		(float[4]){ 0, 0, 0, 1 });
	if (base) {
		set_rect_color(base, spec.color1);
	}

	if (spec.type == WALLPAPER_GRADIENT) {
		// a tiny gradient strip scaled up by the renderer
		int len = 256;
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			spec.vertical ? 1 : len, spec.vertical ? len : 1);
		cairo_t *cr = cairo_create(surface);
		cairo_pattern_t *p = spec.vertical ?
			cairo_pattern_create_linear(0, 0, 0, len) :
			cairo_pattern_create_linear(0, 0, len, 0);
		uint32_t a = spec.color1, b = spec.color2;
		cairo_pattern_add_color_stop_rgb(p, 0, (a >> 24 & 0xff) / 255.0,
			(a >> 16 & 0xff) / 255.0, (a >> 8 & 0xff) / 255.0);
		cairo_pattern_add_color_stop_rgb(p, 1, (b >> 24 & 0xff) / 255.0,
			(b >> 16 & 0xff) / 255.0, (b >> 8 & 0xff) / 255.0);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
		cairo_destroy(cr);
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(tree, NULL);
		if (buffer) {
			tw_scene_buffer_set_surface(buffer, surface, w, h);
			wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
		} else {
			cairo_surface_destroy(surface);
		}
	} else if (spec.type == WALLPAPER_IMAGE && spec.image &&
			(!spec.mode || strcasecmp(spec.mode, "fill") == 0)) {
		cairo_surface_t *image = wallpaper_render_cached(spec.image,
			(int)ceil(w * scale), (int)ceil(h * scale));
		struct wlr_scene_buffer *buffer = image ? wlr_scene_buffer_create(tree, NULL) : NULL;
		if (buffer) {
			tw_scene_buffer_set_surface(buffer, image, w, h);
		} else if (image) {
			cairo_surface_destroy(image);
		} else {
			sway_log(SWAY_ERROR, "Unable to load wallpaper %s", spec.image);
		}
	} else if (spec.type == WALLPAPER_IMAGE && spec.image) {
		cairo_surface_t *image = tw_image_load(spec.image);
		if (!image) {
			sway_log(SWAY_ERROR, "Unable to load wallpaper %s", spec.image);
		} else {
			int iw = cairo_image_surface_get_width(image);
			int ih = cairo_image_surface_get_height(image);
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(tree, NULL);
			if (!buffer || iw <= 0 || ih <= 0) {
				cairo_surface_destroy(image);
			} else {
				const char *mode = spec.mode ? spec.mode : "fill";
				struct wlr_fbox src = { 0, 0, iw, ih };
				int dw = w, dh = h, dx = 0, dy = 0;
				if (strcasecmp(mode, "fill") == 0) {
					double s = fmax((double)w / iw, (double)h / ih);
					src.width = w / s;
					src.height = h / s;
					src.x = (iw - src.width) / 2;
					src.y = (ih - src.height) / 2;
				} else if (strcasecmp(mode, "fit") == 0) {
					double s = fmin((double)w / iw, (double)h / ih);
					dw = iw * s;
					dh = ih * s;
					dx = (w - dw) / 2;
					dy = (h - dh) / 2;
				} else if (strcasecmp(mode, "center") == 0) {
					dw = iw;
					dh = ih;
					dx = (w - iw) / 2;
					dy = (h - ih) / 2;
				}
				tw_scene_buffer_set_surface(buffer, image, dw, dh);
				if (strcasecmp(mode, "fill") == 0) {
					wlr_scene_buffer_set_source_box(buffer, &src);
				}
				wlr_scene_node_set_position(&buffer->node, dx, dy);
			}
		}
	}
	wallpaper_spec_finish(&spec);
}

void tw_wallpaper_invalidate(void) {
	wallpaper_generation++;
	if (!root) {
		return;
	}
	for (int i = 0; i < root->outputs->length; i++) {
		tw_wallpaper_update(root->outputs->items[i]);
	}
}
