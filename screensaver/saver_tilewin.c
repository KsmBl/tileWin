/*
 * The windows tileWin shows on a screen, for the savers drawn over them
 * (Diggers, Hellfire): where each is, its title bar and border, and where the
 * taskbars are. Without tileWin (the preview of the settings) there are none.
 */
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "ipc-client.h"
#include "ipc.h"
#include "saver_util.h"
#include "tw_desktop.h"

#define FAKE_MAX 8

static int rect_int(json_object *rect, const char *key) {
	json_object *v;
	return rect && json_object_object_get_ex(rect, key, &v) ? json_object_get_int(v) : 0;
}

static void collect_windows(json_object *node, int ox, int oy, struct saver_window *wins,
		int max, int *count) {
	json_object *v;
	bool view = json_object_object_get_ex(node, "pid", &v) && json_object_get_int(v) > 0;
	if (view) {
		bool visible = json_object_object_get_ex(node, "visible", &v) &&
			json_object_get_boolean(v);
		bool minimized = json_object_object_get_ex(node, "minimized", &v) &&
			json_object_get_boolean(v);
		json_object *rect = NULL, *deco = NULL;
		json_object_object_get_ex(node, "rect", &rect);
		json_object_object_get_ex(node, "deco_rect", &deco);
		if (visible && !minimized && *count < max) {
			// the rect is what the app draws; the title bar sits on top of it
			int title = rect_int(deco, "height");
			struct saver_window *b = &wins[(*count)++];
			b->x = rect_int(rect, "x") - ox;
			b->y = rect_int(rect, "y") - title - oy;
			b->w = rect_int(rect, "width");
			b->h = rect_int(rect, "height") + title;
			b->title_h = title;
			b->border = json_object_object_get_ex(node, "current_border_width", &v) ?
				json_object_get_int(v) : 0;
			const char *name = json_object_object_get_ex(node, "name", &v) ?
				json_object_get_string(v) : NULL;
			snprintf(b->title, sizeof(b->title), "%s", name ? name : "");
		}
	}
	static const char *const children[] = { "nodes", "floating_nodes" };
	for (int k = 0; k < 2; k++) {
		json_object *list;
		if (!json_object_object_get_ex(node, children[k], &list)) {
			continue;
		}
		for (size_t i = 0; i < json_object_array_length(list); i++) {
			collect_windows(json_object_array_get_idx(list, i), ox, oy, wins, max, count);
		}
	}
}

/* The socket of tileWin, or -1; unlike ipc_open_socket, not being able to is no end. */
static int connect_tilewin(void) {
	const char *path = getenv("TILEWINSOCK");
	path = path ? path : getenv("SWAYSOCK");
	if (!path) {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

/*
 * The taskbars of the screen: what its workspaces leave free of it, at the
 * top, the bottom or a side. tileWin lists no taskbar, but a workspace is as
 * big as the screen less the taskbars.
 */
static void find_bars(json_object *output, int ox, int oy, int ow, int oh,
		struct saver_window *bars, int *bar_count) {
	*bar_count = 0;
	json_object *workspaces, *v, *rect;
	if (!json_object_object_get_ex(output, "nodes", &workspaces)) {
		return;
	}
	for (size_t i = 0; i < json_object_array_length(workspaces); i++) {
		json_object *ws = json_object_array_get_idx(workspaces, i);
		if (!json_object_object_get_ex(ws, "type", &v) ||
				strcmp(json_object_get_string(v), "workspace") != 0 ||
				!json_object_object_get_ex(ws, "rect", &rect)) {
			continue;
		}
		double x = rect_int(rect, "x") - ox, y = rect_int(rect, "y") - oy;
		double w = rect_int(rect, "width"), h = rect_int(rect, "height");
		if (w <= 0 || h <= 0) {
			continue;
		}
		struct saver_window strips[4] = {
			{ .x = 0, .y = 0, .w = ow, .h = y },                 // above
			{ .x = 0, .y = y + h, .w = ow, .h = oh - (y + h) },  // below
			{ .x = 0, .y = 0, .w = x, .h = oh },                 // left
			{ .x = x + w, .y = 0, .w = ow - (x + w), .h = oh },  // right
		};
		for (int k = 0; k < 4; k++) {
			if (strips[k].w > 0 && strips[k].h > 0) {
				bars[(*bar_count)++] = strips[k];
			}
		}
		return; // every workspace of a screen has the same size
	}
}

bool saver_tilewin_windows(const char *output, struct saver_window *wins, int max,
		int *count, struct saver_window *bars, int *bar_count) {
	if (!output || !*output) {
		return false;
	}
	int fd = connect_tilewin();
	if (fd < 0) {
		return false;
	}
	uint32_t len = 0;
	char *reply = ipc_single_command(fd, IPC_GET_TREE, NULL, &len);
	close(fd);
	json_object *tree = reply ? json_tokener_parse(reply) : NULL;
	free(reply);
	if (!tree) {
		return false;
	}
	*count = 0;
	bool found = false;
	json_object *outputs;
	if (json_object_object_get_ex(tree, "nodes", &outputs)) {
		for (size_t i = 0; i < json_object_array_length(outputs); i++) {
			json_object *o = json_object_array_get_idx(outputs, i), *name, *rect;
			if (json_object_object_get_ex(o, "name", &name) &&
					strcmp(json_object_get_string(name), output) == 0 &&
					json_object_object_get_ex(o, "rect", &rect)) {
				int ox = rect_int(rect, "x"), oy = rect_int(rect, "y");
				collect_windows(o, ox, oy, wins, max, count);
				if (bars) {
					find_bars(o, ox, oy, rect_int(rect, "width"), rect_int(rect, "height"),
						bars, bar_count);
				}
				found = true;
			}
		}
	}
	json_object_put(tree);
	return found;
}

/* ---------- windows of its own ---------- */

/* Windows of its own, for the preview or when there is no picture of the screen. */
void saver_fake_wallpaper(cairo_t *cr, int width, int height) {
	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgb(sky, 0, 0.18, 0.45, 0.78);
	cairo_pattern_add_color_stop_rgb(sky, 0.7, 0.45, 0.7, 0.9);
	cairo_pattern_add_color_stop_rgb(sky, 1, 0.3, 0.6, 0.25);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);
}

static void draw_fake_desktop(cairo_t *cr, int width, int height, struct saver_window *wins,
		int count, struct saver_window *bars, int bar_count) {
	saver_fake_wallpaper(cr, width, height);
	double u = saver_unit(width, height);
	for (int i = 0; i < count; i++) {
		struct saver_window *b = &wins[i];
		cairo_rectangle(cr, b->x, b->y, b->w, b->h);
		cairo_set_source_rgb(cr, 0.97, 0.97, 0.98);
		cairo_fill(cr);
		cairo_rectangle(cr, b->x, b->y, b->w, b->title_h);
		cairo_pattern_t *title = cairo_pattern_create_linear(0, b->y, 0, b->y + b->title_h);
		cairo_pattern_add_color_stop_rgb(title, 0, 0.2, 0.45, 0.85);
		cairo_pattern_add_color_stop_rgb(title, 1, 0.1, 0.3, 0.7);
		cairo_set_source(cr, title);
		cairo_fill(cr);
		cairo_pattern_destroy(title);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, u * 13);
		cairo_move_to(cr, b->x + u * 8, b->y + b->title_h * 0.7);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_show_text(cr, b->title);
		cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, u * 12);
		static const char *const words[] = { "The", "windows", "were", "open", "and", "the",
			"work", "of", "the", "day", "lay", "in", "them", "line", "by", "line," };
		int k = i * 5;
		for (double ly = b->y + b->title_h + u * 22; ly < b->y + b->h - u * 8; ly += u * 17) {
			double lx = b->x + u * 12;
			while (lx < b->x + b->w - u * 60) {
				const char *wd = words[k++ % 16];
				cairo_text_extents_t e;
				cairo_text_extents(cr, wd, &e);
				cairo_move_to(cr, lx, ly);
				cairo_set_source_rgb(cr, 0.15, 0.17, 0.22);
				cairo_show_text(cr, wd);
				lx += e.x_advance + u * 5;
			}
		}
		cairo_rectangle(cr, b->x + 0.5, b->y + 0.5, b->w - 1, b->h - 1);
		cairo_set_source_rgb(cr, 0.25, 0.3, 0.4);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	for (int i = 0; i < bar_count; i++) {
		cairo_rectangle(cr, bars[i].x, bars[i].y, bars[i].w, bars[i].h);
		cairo_set_source_rgb(cr, 0.12, 0.13, 0.16);
		cairo_fill(cr);
	}
}

static void make_fake_windows(int width, int height, struct saver_window *wins, int *count,
		struct saver_window *bars, int *bar_count) {
	static const char *const titles[] = { "Documents", "Notes - Editor", "Music",
		"Holiday photos", "Terminal" };
	*count = 3 + (int)(saver_random() * 2);
	for (int i = 0; i < *count; i++) {
		struct saver_window *b = &wins[i];
		b->w = width * saver_between(0.25, 0.4);
		b->h = height * saver_between(0.3, 0.5);
		b->x = saver_between(0.03, 0.97) * (width - b->w);
		b->y = saver_between(0.05, 0.85) * (height * 0.93 - b->h);
		b->title_h = fmax(8, saver_unit(width, height) * 26);
		b->border = 1;
		snprintf(b->title, sizeof(b->title), "%s", titles[i % 5]);
	}
	double bar = fmax(8, height * 0.05);
	bars[0] = (struct saver_window){ .x = 0, .y = height - bar, .w = width, .h = bar };
	*bar_count = 1;
}


cairo_surface_t *saver_desktop(const struct saver_options *options, int width, int height,
		struct saver_window *wins, int max, int *count, struct saver_window *bars,
		int *bar_count) {
	*count = *bar_count = 0;
	bool real = saver_tilewin_windows(options->output, wins, max, count, bars, bar_count);
	if (!real) {
		struct saver_window made[FAKE_MAX];
		int n = 0;
		make_fake_windows(width, height, made, &n, bars, bar_count);
		*count = n < max ? n : max;
		memcpy(wins, made, sizeof(*wins) * *count);
	}
	cairo_surface_t *picture = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(picture);
	if (options->desktop && real) {
		cairo_scale(cr, (double)width / cairo_image_surface_get_width(options->desktop),
			(double)height / cairo_image_surface_get_height(options->desktop));
		cairo_set_source_surface(cr, options->desktop, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
	} else {
		for (int i = 0; i < *count; i++) {
			if (wins[i].title_h <= 0) {
				wins[i].title_h = fmax(8, saver_unit(width, height) * 26);
			}
		}
		draw_fake_desktop(cr, width, height, wins, *count, bars, *bar_count);
	}
	cairo_destroy(cr);
	cairo_surface_flush(picture);
	return picture;
}

/* ---------- the wallpaper ---------- */

static void set_hex(cairo_pattern_t *p, double offset, const char *hex) {
	unsigned v = 0;
	if (hex && hex[0] == '#') {
		v = (unsigned)strtoul(hex + 1, NULL, 16);
	}
	cairo_pattern_add_color_stop_rgb(p, offset, (v >> 16 & 255) / 255.0, (v >> 8 & 255) / 255.0,
		(v & 255) / 255.0);
}

static const char *jstring(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) ? json_object_get_string(v) : NULL;
}

cairo_surface_t *saver_wallpaper(const char *output, int width, int height) {
	if (!output || !*output) {
		return NULL;
	}
	int fd = connect_tilewin();
	if (fd < 0) {
		return NULL;
	}
	uint32_t len = 0;
	char *reply = ipc_single_command(fd, IPC_GET_OUTPUTS, NULL, &len);
	close(fd);
	json_object *outputs = reply ? json_tokener_parse(reply) : NULL;
	free(reply);
	json_object *wall = NULL;
	for (size_t i = 0; outputs && i < json_object_array_length(outputs); i++) {
		json_object *o = json_object_array_get_idx(outputs, i);
		const char *name = jstring(o, "name");
		if (name && strcmp(name, output) == 0) {
			json_object_object_get_ex(o, "tw_wallpaper", &wall);
		}
	}
	if (!wall) {
		json_object_put(outputs);
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(out);
	const char *type = jstring(wall, "type"), *image = jstring(wall, "image");
	const char *mode = jstring(wall, "mode");
	json_object *v;
	bool vertical = !json_object_object_get_ex(wall, "vertical", &v) || json_object_get_boolean(v);
	cairo_pattern_t *fill = type && strcmp(type, "gradient") == 0 ?
		(vertical ? cairo_pattern_create_linear(0, 0, 0, height) :
			cairo_pattern_create_linear(0, 0, width, 0)) :
		cairo_pattern_create_linear(0, 0, 0, 1);
	set_hex(fill, 0, jstring(wall, "color"));
	set_hex(fill, 1, type && strcmp(type, "gradient") == 0 ? jstring(wall, "color2") :
		jstring(wall, "color"));
	cairo_set_source(cr, fill);
	cairo_paint(cr);
	cairo_pattern_destroy(fill);
	if (type && strcmp(type, "image") == 0 && image) {
		bool cover = !mode || strcmp(mode, "fill") == 0;
		cairo_surface_t *picture = cover ? tw_image_render_cover(image, width, height) :
			tw_image_load(image);
		if (picture) {
			int iw = cairo_image_surface_get_width(picture), ih = cairo_image_surface_get_height(picture);
			if (cover) {
				cairo_scale(cr, (double)width / iw, (double)height / ih);
			} else if (strcmp(mode, "stretch") == 0) {
				cairo_scale(cr, (double)width / iw, (double)height / ih);
			} else if (strcmp(mode, "fit") == 0) {
				double k = fmin((double)width / iw, (double)height / ih);
				cairo_translate(cr, (width - iw * k) / 2, (height - ih * k) / 2);
				cairo_scale(cr, k, k);
			} else { // center
				cairo_translate(cr, (width - iw) / 2.0, (height - ih) / 2.0);
			}
			cairo_set_source_surface(cr, picture, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_surface_destroy(picture);
		}
	}
	cairo_destroy(cr);
	cairo_surface_flush(out);
	json_object_put(outputs);
	return out;
}
