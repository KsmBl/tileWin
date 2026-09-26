#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "saver_util.h"

/*
 * Screen savers of tileWin's own: northern lights that burn brighter the
 * harder the computer works, a clock that spells out the time, and windows
 * tiling themselves the way tile mode does.
 */

/* ================= Aurora ================= */

#define AURORA_STARS 260
#define AURORA_LEVELS 8
#define AURORA_MOODS 3
#define AURORA_CURTAINS 3
#define AURORA_HILLS 64

struct aurora {
	double sx[AURORA_STARS], sy[AURORA_STARS], sb[AURORA_STARS], sp[AURORA_STARS];
	double hills[AURORA_HILLS + 1];
	// a curtain in AURORA_LEVELS strengths, calm, busy and very busy
	cairo_pattern_t *curtain[AURORA_MOODS][AURORA_LEVELS];
	double t, load, target, since_cpu;
	unsigned long long cpu_total, cpu_idle;
	int mood;         // 0 follows the processor, 1 always calm, 2 always stormy
	bool stars;
};

/* How busy the processor was since the last look, 0 to 1. */
static void aurora_sample_cpu(struct aurora *a) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return;
	}
	unsigned long long v[8] = { 0 };
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
		&v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 5) {
		return;
	}
	unsigned long long idle = v[3] + v[4], total = 0;
	for (int i = 0; i < 8; i++) {
		total += v[i];
	}
	if (a->cpu_total && total > a->cpu_total) {
		a->target = 1 - (double)(idle - a->cpu_idle) / (total - a->cpu_total);
	}
	a->cpu_total = total;
	a->cpu_idle = idle;
}

static void *aurora_create(int width, int height, const struct saver_options *options) {
	struct aurora *a = calloc(1, sizeof(*a));
	for (int i = 0; i < AURORA_STARS; i++) {
		a->sx[i] = saver_random();
		a->sy[i] = saver_random() * saver_random() * 0.75; // more of them up high
		a->sb[i] = saver_between(0.2, 1);
		a->sp[i] = saver_between(0, 2 * M_PI);
	}
	double p1 = saver_between(0, 6), p2 = saver_between(0, 6), p3 = saver_between(0, 6);
	for (int i = 0; i <= AURORA_HILLS; i++) {
		double x = i / (double)AURORA_HILLS;
		a->hills[i] = 0.84 - 0.07 * sin(x * 5 + p1) - 0.04 * sin(x * 13 + p2) -
			0.015 * sin(x * 37 + p3);
	}
	static const double tops[AURORA_MOODS][3] = {
		{ 0.35, 0.3, 0.95 }, { 0.75, 0.25, 0.85 }, { 0.95, 0.25, 0.4 },
	};
	for (int m = 0; m < AURORA_MOODS; m++) {
		for (int l = 0; l < AURORA_LEVELS; l++) {
			double s = (l + 1) / (double)AURORA_LEVELS;
			cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, 1);
			cairo_pattern_add_color_stop_rgba(p, 0, tops[m][0], tops[m][1], tops[m][2], 0);
			cairo_pattern_add_color_stop_rgba(p, 0.45, tops[m][0] * 0.6, 0.55, tops[m][2] * 0.7,
				0.22 * s);
			cairo_pattern_add_color_stop_rgba(p, 0.86, 0.25, 1, 0.55, 0.8 * s);
			cairo_pattern_add_color_stop_rgba(p, 0.95, 0.65, 1, 0.8, 0.9 * s);
			cairo_pattern_add_color_stop_rgba(p, 1, 0.3, 1, 0.6, 0);
			a->curtain[m][l] = p;
		}
	}
	aurora_sample_cpu(a);
	a->load = a->target = 0.2;
	a->mood = saver_choice(options, &saver_aurora, "mood");
	a->stars = saver_toggle(options, &saver_aurora, "stars");
	return a;
}

static void aurora_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct aurora *a = state;
	a->t += dt;
	a->since_cpu += dt;
	if (a->mood == 1 || a->mood == 2) {
		a->target = a->mood == 1 ? 0.15 : 0.9;
	} else if (a->since_cpu >= 1) {
		a->since_cpu = 0;
		aurora_sample_cpu(a);
	}
	a->load += (a->target - a->load) * saver_clamp(dt * 1.2, 0, 1);
	double load = saver_clamp(a->load, 0, 1);

	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgb(sky, 0, 0.01, 0.015, 0.05);
	cairo_pattern_add_color_stop_rgb(sky, 0.8, 0.02, 0.07, 0.1);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);
	for (int i = 0; a->stars && i < AURORA_STARS; i++) {
		double twinkle = 0.6 + 0.4 * sin(a->t * 2 + a->sp[i]);
		cairo_rectangle(cr, a->sx[i] * width, a->sy[i] * height, 1.2, 1.2);
		cairo_set_source_rgba(cr, 1, 1, 1, a->sb[i] * twinkle * 0.8);
		cairo_fill(cr);
	}

	// the curtains: they fold and sway, faster and brighter when the computer works
	int mood = load < 0.4 ? 0 : load < 0.75 ? 1 : 2;
	double step = fmax(1.5, width / 360.0), tau = 2 * M_PI;
	double sway = a->t * (0.6 + 1.4 * load);
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int c = 0; c < AURORA_CURTAINS; c++) {
		for (double x = 0; x < width; x += step) {
			double u = x / width;
			double base = height * (0.42 + 0.07 * c) +
				height * 0.08 * sin(u * tau * 1.1 + a->t * 0.15 + c * 2) +
				height * 0.04 * sin(u * tau * 3.7 - a->t * 0.33 + c);
			double tall = height * (0.2 + 0.1 * sin(u * tau * 0.8 + a->t * 0.2 + c * 1.3) +
				0.05 * sin(u * tau * 5 + sway * 0.6));
			double bright = (0.6 + 0.4 * sin(u * tau * 7 + sway * 0.9 + c * 3)) *
				(0.5 + 0.5 * sin(u * tau * 2.3 - a->t * 0.4 + c));
			bright *= 0.45 + 0.8 * load;
			int level = (int)(saver_clamp(bright, 0, 0.999) * AURORA_LEVELS);
			if (level <= 0 || tall < 1) {
				continue;
			}
			double top = base - tall;
			cairo_matrix_t m;
			cairo_matrix_init(&m, 1, 0, 0, 1 / tall, 0, -top / tall);
			cairo_pattern_t *p = a->curtain[mood][level - 1];
			cairo_pattern_set_matrix(p, &m);
			cairo_rectangle(cr, x, top, step + 0.6, tall);
			cairo_set_source(cr, p);
			cairo_fill(cr);
		}
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	// the hills in front
	cairo_move_to(cr, 0, height);
	for (int i = 0; i <= AURORA_HILLS; i++) {
		cairo_line_to(cr, i * width / (double)AURORA_HILLS, a->hills[i] * height);
	}
	cairo_line_to(cr, width, height);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.006, 0.012, 0.02);
	cairo_fill(cr);
}

static void aurora_destroy(void *state) {
	struct aurora *a = state;
	for (int m = 0; m < AURORA_MOODS; m++) {
		for (int l = 0; l < AURORA_LEVELS; l++) {
			cairo_pattern_destroy(a->curtain[m][l]);
		}
	}
	free(a);
}

static const char *const mood_values[] = { "cpu", "calm", "stormy", NULL };
static const char *const mood_labels[] = { "Follows the processor", "Always calm", "Always stormy",
	NULL };
static const struct saver_option aurora_options[] = {
	{ "mood", "Northern lights", "Following the processor, they grow wilder the harder it works",
		SAVER_CHOICE, mood_values, mood_labels, false },
	{ "stars", "Stars", NULL, SAVER_TOGGLE, NULL, NULL, true },
	{ 0 },
};

const struct saver saver_aurora = {
	.name = "aurora",
	.title = "Aurora",
	.description = "Northern lights over the hills that glow brighter and turn red the "
		"harder the computer works",
	.resolution = 0.5,
	.create = aurora_create,
	.draw = aurora_draw,
	.options = aurora_options,
	.destroy = aurora_destroy,
};

/* ================= Word Clock ================= */

#define WORD_COLS 11
#define WORD_ROWS 10

static const char *const word_grid[WORD_ROWS] = {
	"ITLISASAMPM",
	"ACQUARTERDC",
	"TWENTYFIVEX",
	"HALFSTENFTO",
	"PASTERUNINE",
	"ONESIXTHREE",
	"FOURFIVETWO",
	"EIGHTELEVEN",
	"SEVENTWELVE",
	"TENSEOCLOCK",
};

struct word {
	int row, col, len;
};

enum {
	W_IT, W_IS, W_A, W_QUARTER, W_TWENTY, W_FIVE_M, W_HALF, W_TEN_M, W_TO, W_PAST, W_OCLOCK,
	W_HOURS, // ONE .. TWELVE follow
};

static const struct word words[] = {
	[W_IT] = { 0, 0, 2 }, [W_IS] = { 0, 3, 2 }, [W_A] = { 1, 0, 1 },
	[W_QUARTER] = { 1, 2, 7 }, [W_TWENTY] = { 2, 0, 6 }, [W_FIVE_M] = { 2, 6, 4 },
	[W_HALF] = { 3, 0, 4 }, [W_TEN_M] = { 3, 5, 3 }, [W_TO] = { 3, 9, 2 },
	[W_PAST] = { 4, 0, 4 }, [W_OCLOCK] = { 9, 5, 6 },
	[W_HOURS + 0] = { 5, 0, 3 },  // one
	[W_HOURS + 1] = { 6, 8, 3 },  // two
	[W_HOURS + 2] = { 5, 6, 5 },  // three
	[W_HOURS + 3] = { 6, 0, 4 },  // four
	[W_HOURS + 4] = { 6, 4, 4 },  // five
	[W_HOURS + 5] = { 5, 3, 3 },  // six
	[W_HOURS + 6] = { 8, 0, 5 },  // seven
	[W_HOURS + 7] = { 7, 0, 5 },  // eight
	[W_HOURS + 8] = { 4, 7, 4 },  // nine
	[W_HOURS + 9] = { 9, 0, 3 },  // ten
	[W_HOURS + 10] = { 7, 5, 6 }, // eleven
	[W_HOURS + 11] = { 8, 5, 6 }, // twelve
};

struct wordclock {
	double color[3];       // of the lit letters
	double glow[WORD_ROWS][WORD_COLS], dots[4];
	cairo_surface_t *dim, *lit; // the whole grid, unlit and lit
	double cell, t;
	int width, height;
};

static void wordclock_render_grid(struct wordclock *s, cairo_surface_t *target, bool lit) {
	cairo_t *cr = cairo_create(target);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	char font[96];
	snprintf(font, sizeof(font), "DejaVu Sans Mono, Noto Sans Mono, monospace Bold %dpx",
		(int)(s->cell * 0.52));
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	for (int r = 0; r < WORD_ROWS; r++) {
		for (int c = 0; c < WORD_COLS; c++) {
			char letter[2] = { word_grid[r][c], 0 };
			pango_layout_set_text(layout, letter, 1);
			int lw, lh;
			pango_layout_get_pixel_size(layout, &lw, &lh);
			double x = c * s->cell + (s->cell - lw) / 2, y = r * s->cell + (s->cell - lh) / 2;
			if (lit) {
				// a soft glow around the letter, then the letter
				for (int k = 0; k < 8; k++) {
					double a = k * M_PI / 4, d = s->cell * 0.035;
					cairo_move_to(cr, x + cos(a) * d, y + sin(a) * d);
					cairo_set_source_rgba(cr, s->color[0] * 0.8, s->color[1] * 0.85, s->color[2],
						0.12);
					pango_cairo_show_layout(cr, layout);
				}
				cairo_set_source_rgb(cr, s->color[0], s->color[1], s->color[2]);
			} else {
				cairo_set_source_rgb(cr, 0.13, 0.14, 0.16);
			}
			cairo_move_to(cr, x, y);
			pango_cairo_show_layout(cr, layout);
		}
	}
	g_object_unref(layout);
	cairo_destroy(cr);
}

static void *wordclock_create(int width, int height, const struct saver_options *options) {
	struct wordclock *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	static const double colors[][3] = { { 1, 1, 1 }, { 0.45, 1, 0.55 }, { 0.45, 0.75, 1 },
		{ 1, 0.72, 0.25 }, { 1, 0.45, 0.6 } };
	memcpy(s->color, colors[saver_choice(options, &saver_wordclock, "color")], sizeof(s->color));
	s->cell = floor(fmin(width * 0.7 / WORD_COLS, height * 0.7 / WORD_ROWS));
	if (s->cell < 4) {
		s->cell = 4;
	}
	int gw = (int)(s->cell * WORD_COLS), gh = (int)(s->cell * WORD_ROWS);
	s->dim = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
	s->lit = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
	wordclock_render_grid(s, s->dim, false);
	wordclock_render_grid(s, s->lit, true);
	return s;
}

/* Which words say the time; the minutes past the fives are dots. */
static void wordclock_words(const struct tm *tm, bool lit[WORD_ROWS][WORD_COLS], int *dots) {
	memset(lit, 0, sizeof(bool) * WORD_ROWS * WORD_COLS);
	int five = tm->tm_min / 5, hour = tm->tm_hour % 12;
	*dots = tm->tm_min % 5;
	int on[6], count = 0;
	on[count++] = W_IT;
	on[count++] = W_IS;
	static const int minute_words[12][3] = {
		{ -1, -1, -1 }, { W_FIVE_M, W_PAST, -1 }, { W_TEN_M, W_PAST, -1 },
		{ W_A, W_QUARTER, W_PAST }, { W_TWENTY, W_PAST, -1 }, { W_TWENTY, W_FIVE_M, W_PAST },
		{ W_HALF, W_PAST, -1 }, { W_TWENTY, W_FIVE_M, W_TO }, { W_TWENTY, W_TO, -1 },
		{ W_A, W_QUARTER, W_TO }, { W_TEN_M, W_TO, -1 }, { W_FIVE_M, W_TO, -1 },
	};
	for (int i = 0; i < 3; i++) {
		if (minute_words[five][i] >= 0) {
			on[count++] = minute_words[five][i];
		}
	}
	if (five >= 7) {
		hour = (hour + 1) % 12; // "to" the next hour
	}
	on[count++] = W_HOURS + (hour + 11) % 12;
	if (five == 0) {
		on[count++] = W_OCLOCK;
	}
	for (int i = 0; i < count; i++) {
		const struct word *w = &words[on[i]];
		for (int k = 0; k < w->len; k++) {
			lit[w->row][w->col + k] = true;
		}
	}
}

static void wordclock_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct wordclock *s = state;
	s->t += dt;
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	bool lit[WORD_ROWS][WORD_COLS];
	int dots;
	wordclock_words(&tm, lit, &dots);
	// letters fade in and out instead of switching
	double rate = saver_clamp(dt * 2.5, 0, 1);
	for (int r = 0; r < WORD_ROWS; r++) {
		for (int c = 0; c < WORD_COLS; c++) {
			s->glow[r][c] += ((lit[r][c] ? 1 : 0) - s->glow[r][c]) * rate;
		}
	}
	for (int i = 0; i < 4; i++) {
		s->dots[i] += ((i < dots ? 1 : 0) - s->dots[i]) * rate;
	}
	// it wanders a little, so nothing burns into the screen
	double gw = s->cell * WORD_COLS, gh = s->cell * WORD_ROWS;
	double ox = floor((width - gw) / 2 + sin(s->t * 0.05) * width * 0.04);
	double oy = floor((height - gh) / 2 + cos(s->t * 0.037) * height * 0.04);
	cairo_set_source_surface(cr, s->dim, ox, oy);
	cairo_paint(cr);
	for (int r = 0; r < WORD_ROWS; r++) {
		for (int c = 0; c < WORD_COLS; c++) {
			if (s->glow[r][c] < 0.01) {
				continue;
			}
			cairo_save(cr);
			cairo_rectangle(cr, ox + c * s->cell, oy + r * s->cell, s->cell, s->cell);
			cairo_clip(cr);
			cairo_set_source_surface(cr, s->lit, ox, oy);
			cairo_paint_with_alpha(cr, s->glow[r][c]);
			cairo_restore(cr);
		}
	}
	// the minutes between the fives, one dot in each corner
	double dx[4] = { -0.35, gw / s->cell + 0.35, gw / s->cell + 0.35, -0.35 };
	double dy[4] = { -0.35, -0.35, gh / s->cell + 0.35, gh / s->cell + 0.35 };
	for (int i = 0; i < 4; i++) {
		cairo_arc(cr, ox + dx[i] * s->cell, oy + dy[i] * s->cell, s->cell * 0.07, 0, 2 * M_PI);
		double b = s->dots[i];
		cairo_set_source_rgb(cr, 0.13 + (s->color[0] - 0.13) * b, 0.13 + (s->color[1] - 0.13) * b,
			0.14 + (s->color[2] - 0.14) * b);
		cairo_fill(cr);
	}
}

static void wordclock_destroy(void *state) {
	struct wordclock *s = state;
	cairo_surface_destroy(s->dim);
	cairo_surface_destroy(s->lit);
	free(s);
}

static const char *const color_values[] = { "white", "green", "blue", "amber", "pink", NULL };
static const char *const color_labels[] = { "White", "Green", "Blue", "Amber", "Pink", NULL };
static const struct saver_option wordclock_options[] = {
	{ "color", "Colour", NULL, SAVER_CHOICE, color_values, color_labels, false },
	{ 0 },
};

const struct saver saver_wordclock = {
	.name = "wordclock",
	.title = "Word Clock",
	.description = "The time spelled out in a grid of letters, \"it is twenty past ten\"",
	.create = wordclock_create,
	.draw = wordclock_draw,
	.options = wordclock_options,
	.destroy = wordclock_destroy,
};

/* ================= Tiling ================= */

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
	{ "windows", "Windows", NULL, SAVER_CHOICE, tiles_values, tiles_labels, false },
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
