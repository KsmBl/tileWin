#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_tiling;

/* Tiling. */

#define TILES_MAX 9

enum tile_kind {
	TILE_TERMINAL,
	TILE_EDITOR,
	TILE_MONITOR,
	TILE_MUSIC,
	TILE_PICTURE,
	TILE_KINDS,
};

static const char *const tile_titles[TILE_KINDS] = {
	"fish ~/Projects/tileWin", "main.c - Editor", "System monitor", "Music", "Holiday.png",
};

static const char *const terminal_lines[] = {
	"git status", "On branch main, nothing to commit", "meson test -C build",
	"Ok: 19  Fail: 0", "htop", "ls ~/Desktop", "notes.txt  todo.txt  Projects",
	"make -j8", "[100%] Built target tileWin", "ssh pi@garden", "uptime",
	"up 12 days, 3 users, load average: 0.21", "cowsay tiling!", "vim README.md",
};

struct tile {
	struct tile *a, *b, *parent;
	bool leaf, vertical, closing;
	double ratio, target;
	enum tile_kind kind;
	double born, hue;
	unsigned seed;
	double x, y, w, h; // where it was drawn last
};

struct tiling {
	struct tile *root, *focus;
	double t, next_change, u;
	int max;               // windows at most
};

static struct tile *tile_new(enum tile_kind kind, double born) {
	struct tile *t = calloc(1, sizeof(*t));
	t->leaf = true;
	t->kind = kind;
	t->born = born;
	t->hue = saver_random();
	t->seed = (unsigned)(saver_random() * 1e9);
	return t;
}

static void tile_free(struct tile *t) {
	if (t) {
		tile_free(t->a);
		tile_free(t->b);
		free(t);
	}
}

static int tile_leaves(struct tile *t, struct tile **out, int count) {
	if (!t) {
		return count;
	}
	if (t->leaf) {
		if (out) {
			out[count] = t;
		}
		return count + 1;
	}
	count = tile_leaves(t->a, out, count);
	return tile_leaves(t->b, out, count);
}

static void *tiling_create(int width, int height, const struct saver_options *options) {
	struct tiling *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	s->root = tile_new(TILE_TERMINAL, 0);
	s->root->x = s->root->y = 0;
	s->root->w = width;
	s->root->h = height;
	s->focus = s->root;
	s->next_change = 0.8;
	static const int maxes[] = { TILES_MAX, 4, 2 };
	s->max = maxes[saver_choice(options, &saver_tiling, "windows")];
	return s;
}

/* A new window next to a tiled one, which moves over to make room. */
static void tiling_split(struct tiling *s, struct tile *leaf) {
	struct tile *old = tile_new(leaf->kind, leaf->born);
	old->hue = leaf->hue;
	old->seed = leaf->seed;
	struct tile *fresh = tile_new((enum tile_kind)(saver_random() * TILE_KINDS), s->t);
	leaf->leaf = false;
	leaf->vertical = leaf->w >= leaf->h;
	leaf->a = old;
	leaf->b = fresh;
	old->parent = fresh->parent = leaf;
	old->x = leaf->x;
	old->y = leaf->y;
	old->w = leaf->w;
	old->h = leaf->h;
	leaf->ratio = 1;
	leaf->target = saver_between(0.42, 0.58);
	s->focus = fresh;
}

static void tiling_close(struct tiling *s, struct tile *leaf) {
	struct tile *parent = leaf->parent;
	if (!parent || parent->closing) {
		return;
	}
	parent->closing = true;
	leaf->closing = true;
	parent->target = leaf == parent->a ? 0 : 1;
}

/* The window beside a closed one takes over its place. */
static void tiling_finish_close(struct tiling *s, struct tile *parent) {
	struct tile *gone = parent->a->closing ? parent->a : parent->b;
	struct tile *stays = gone == parent->a ? parent->b : parent->a;
	struct tile *grand = parent->parent;
	stays->parent = grand;
	if (!grand) {
		s->root = stays;
	} else if (grand->a == parent) {
		grand->a = stays;
	} else {
		grand->b = stays;
	}
	if (s->focus == gone || s->focus == parent) {
		s->focus = stays;
	}
	parent->a = parent->b = NULL;
	tile_free(gone);
	free(parent);
}

static void tiling_animate(struct tiling *s, struct tile *t, double dt) {
	if (!t || t->leaf) {
		return;
	}
	t->ratio += (t->target - t->ratio) * saver_clamp(dt * 5, 0, 1);
	tiling_animate(s, t->a, dt);
	tiling_animate(s, t->b, dt);
	if (t->closing && fabs(t->ratio - t->target) < 0.004) {
		tiling_finish_close(s, t);
	}
}

static void tile_text(cairo_t *cr, double x, double y, double size, const char *text,
		double r, double g, double b) {
	cairo_set_font_size(cr, size);
	cairo_move_to(cr, x, y);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_show_text(cr, text);
}

static void tile_content(struct tiling *s, cairo_t *cr, struct tile *t, double x, double y,
		double w, double h) {
	double u = s->u, age = s->t - t->born;
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	switch (t->kind) {
	case TILE_TERMINAL: {
		// a shell that keeps typing
		cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		double size = u * 13, line = size * 1.5;
		int fits = (int)((h - line * 0.5) / line), typed = (int)(age * 1.6);
		int first = typed - fits + 1 > 0 ? typed - fits + 1 : 0;
		int count = sizeof(terminal_lines) / sizeof(terminal_lines[0]);
		for (int i = first; i <= typed; i++) {
			const char *text = terminal_lines[(i + t->seed) % count];
			bool command = ((i + t->seed) % count) % 2 == 0;
			double ly = y + line * (i - first + 1);
			char shown[64];
			snprintf(shown, sizeof(shown), "%s", text);
			if (i == typed) {
				// the line being typed, letter by letter
				size_t n = (size_t)((age * 1.6 - typed) * 30);
				shown[n < strlen(shown) ? n : strlen(shown)] = '\0';
			}
			double lx = x + u * 8;
			if (command) {
				tile_text(cr, lx, ly, size, "> ", 0.65, 0.89, 0.63);
				lx += size * 1.3;
			}
			tile_text(cr, lx, ly, size, shown, 0.8, 0.84, 0.96);
		}
		break;
	}
	case TILE_EDITOR: {
		// lines of code, as colored bars scrolling by
		static const double colors[][3] = {
			{ 0.8, 0.65, 0.97 }, { 0.54, 0.71, 0.98 }, { 0.65, 0.89, 0.63 },
			{ 0.98, 0.7, 0.53 }, { 0.42, 0.44, 0.53 },
		};
		double line = u * 18, scroll = fmod(age * u * 12, line);
		int start = (int)(age * u * 12 / line);
		for (int i = 0; y + i * line - scroll < y + h; i++) {
			unsigned hash = (unsigned)(start + i) * 2654435761u + t->seed;
			double lx = x + u * 10 + (hash % 4) * u * 16, ly = y + u * 8 + i * line - scroll;
			int parts = 1 + (hash >> 4) % 4;
			for (int k = 0; k < parts && hash % 7; k++) {
				unsigned h2 = hash * (k + 3) >> 7;
				double pw = u * (18 + h2 % 70);
				const double *c = colors[(h2 >> 3) % 5];
				cairo_rectangle(cr, lx, ly, pw, line * 0.45);
				cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.85);
				cairo_fill(cr);
				lx += pw + u * 8;
			}
		}
		break;
	}
	case TILE_MONITOR: {
		// a chart of a made-up processor
		double base = y + h - u * 10, top = y + u * 10;
		cairo_move_to(cr, x, base);
		for (double px = 0; px <= w; px += fmax(2, u * 4)) {
			double v = 0.5 + 0.25 * sin((px / w) * 9 + s->t * 1.7 + t->seed) +
				0.2 * sin((px / w) * 23 - s->t * 2.3);
			cairo_line_to(cr, x + px, base - (base - top) * saver_clamp(v, 0, 1));
		}
		cairo_line_to(cr, x + w, base);
		cairo_close_path(cr);
		saver_set_hsva(cr, t->hue, 0.55, 1, 0.35);
		cairo_fill_preserve(cr);
		saver_set_hsva(cr, t->hue, 0.5, 1, 0.9);
		cairo_set_line_width(cr, fmax(1, u * 2));
		cairo_stroke(cr);
		break;
	}
	case TILE_MUSIC: {
		// an equalizer
		int bars = 16;
		double bw = w / (bars * 1.5);
		for (int i = 0; i < bars; i++) {
			double v = fabs(sin(s->t * (2 + i * 0.37) + i * 1.3 + t->seed)) * 0.8 + 0.1;
			double bh = (h - u * 20) * v;
			cairo_rectangle(cr, x + bw * 0.5 + i * bw * 1.5, y + h - u * 10 - bh, bw, bh);
			saver_set_hsva(cr, t->hue + i * 0.02, 0.6, 1, 0.9);
			cairo_fill(cr);
		}
		break;
	}
	case TILE_PICTURE:
	case TILE_KINDS: {
		// a sunset
		cairo_pattern_t *sky = cairo_pattern_create_linear(0, y, 0, y + h);
		cairo_pattern_add_color_stop_rgb(sky, 0, 0.2, 0.2, 0.5);
		cairo_pattern_add_color_stop_rgb(sky, 0.7, 0.98, 0.55, 0.45);
		cairo_rectangle(cr, x, y, w, h);
		cairo_set_source(cr, sky);
		cairo_fill(cr);
		cairo_pattern_destroy(sky);
		cairo_arc(cr, x + w * 0.62, y + h * 0.62, fmin(w, h) * 0.14, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 1, 0.85, 0.55);
		cairo_fill(cr);
		cairo_move_to(cr, x, y + h);
		cairo_line_to(cr, x + w * 0.3, y + h * 0.6);
		cairo_line_to(cr, x + w * 0.55, y + h * 0.85);
		cairo_line_to(cr, x + w * 0.8, y + h * 0.55);
		cairo_line_to(cr, x + w, y + h * 0.8);
		cairo_line_to(cr, x + w, y + h);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, 0.12, 0.1, 0.22);
		cairo_fill(cr);
		break;
	}
	}
	cairo_restore(cr);
}

static void tiling_layout(struct tiling *s, cairo_t *cr, struct tile *t, double x, double y,
		double w, double h) {
	t->x = x;
	t->y = y;
	t->w = w;
	t->h = h;
	if (!t->leaf) {
		double r = saver_clamp(t->ratio, 0, 1);
		if (t->vertical) {
			tiling_layout(s, cr, t->a, x, y, w * r, h);
			tiling_layout(s, cr, t->b, x + w * r, y, w * (1 - r), h);
		} else {
			tiling_layout(s, cr, t->a, x, y, w, h * r);
			tiling_layout(s, cr, t->b, x, y + h * r, w, h * (1 - r));
		}
		return;
	}
	double u = s->u, gap = u * 7;
	double wx = x + gap, wy = y + gap, ww = w - 2 * gap, wh = h - 2 * gap;
	if (ww < u * 12 || wh < u * 12) {
		return; // just opening or closing
	}
	// the window: dark, with a lavender frame when it has the focus
	double radius = u * 8;
	cairo_new_sub_path(cr);
	cairo_arc(cr, wx + ww - radius, wy + radius, radius, -M_PI / 2, 0);
	cairo_arc(cr, wx + ww - radius, wy + wh - radius, radius, 0, M_PI / 2);
	cairo_arc(cr, wx + radius, wy + wh - radius, radius, M_PI / 2, M_PI);
	cairo_arc(cr, wx + radius, wy + radius, radius, M_PI, 1.5 * M_PI);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.118, 0.118, 0.18);
	cairo_fill_preserve(cr);
	bool focused = t == s->focus;
	if (focused) {
		cairo_set_source_rgb(cr, 0.447, 0.529, 0.992);
	} else {
		cairo_set_source_rgb(cr, 0.27, 0.28, 0.35);
	}
	cairo_set_line_width(cr, focused ? u * 3 : u * 1.5);
	cairo_stroke(cr);
	double bar = u * 26;
	if (wh > bar * 2) {
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_save(cr);
		cairo_rectangle(cr, wx, wy, ww - u * 60, bar);
		cairo_clip(cr);
		tile_text(cr, wx + u * 12, wy + bar * 0.68, u * 12.5, tile_titles[t->kind],
			0.8, 0.84, 0.96);
		cairo_restore(cr);
		for (int i = 0; i < 3; i++) {
			static const double dots[3][3] = {
				{ 0.98, 0.7, 0.53 }, { 0.98, 0.89, 0.69 }, { 0.65, 0.89, 0.63 },
			};
			cairo_arc(cr, wx + ww - u * (16 + i * 16), wy + bar / 2, u * 4.5, 0, 2 * M_PI);
			cairo_set_source_rgb(cr, dots[i][0], dots[i][1], dots[i][2]);
			cairo_fill(cr);
		}
		tile_content(s, cr, t, wx + u * 4, wy + bar, ww - u * 8, wh - bar - u * 4);
	}
}

static void tiling_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct tiling *s = state;
	s->t += dt;
	tiling_animate(s, s->root, dt);
	if (s->t >= s->next_change) {
		s->next_change = s->t + saver_between(0.9, 1.9);
		struct tile *leaves[64];
		int count = tile_leaves(s->root, leaves, 0);
		bool busy = false;
		for (int i = 0; i < count; i++) {
			busy = busy || leaves[i]->closing || (leaves[i]->parent &&
				fabs(leaves[i]->parent->ratio - leaves[i]->parent->target) > 0.02);
		}
		if (!busy) {
			// a busy desk most of the time, now and then an empty one
			bool open = count < (s->max < 3 ? s->max : 3) ||
				(count < s->max && saver_random() < 0.55);
			struct tile *pick = leaves[(int)(saver_random() * count)];
			if (open) {
				tiling_split(s, pick);
			} else {
				tiling_close(s, pick);
			}
		}
	}
	cairo_pattern_t *wall = cairo_pattern_create_linear(0, 0, width, height);
	cairo_pattern_add_color_stop_rgb(wall, 0, 0.067, 0.067, 0.106);
	cairo_pattern_add_color_stop_rgb(wall, 1, 0.12, 0.1, 0.2);
	cairo_set_source(cr, wall);
	cairo_paint(cr);
	cairo_pattern_destroy(wall);
	double margin = s->u * 12;
	tiling_layout(s, cr, s->root, margin, margin, width - 2 * margin, height - 2 * margin);
}

static void tiling_destroy(void *state) {
	struct tiling *s = state;
	tile_free(s->root);
	free(s);
}

static const char *const tiles_values[] = { "nine", "four", "two", NULL };
static const char *const tiles_labels[] = { "Up to nine", "Up to four", "Up to two", NULL };
static const struct saver_option tiling_options[] = {
	{ "windows", "Windows", NULL, SAVER_CHOICE, tiles_values, tiles_labels, false, NULL },
	{ 0 },
};

const struct saver saver_tiling = {
	.name = "tiling",
	.title = "Tiling",
	.description = "Windows opening, closing and making room for each other the way tile "
		"mode arranges them",
	.create = tiling_create,
	.draw = tiling_draw,
	.options = tiling_options,
	.destroy = tiling_destroy,
};

TILEWIN_SAVER(.saver = &saver_tiling, .order = 130, .shot_seconds = 8);
