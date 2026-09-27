#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "saver_util.h"

/* The saver, here: its settings are read with it. */
extern const struct saver saver_wordclock;

/* Word Clock. */

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
	{ "color", "Colour", NULL, SAVER_CHOICE, color_values, color_labels, false, NULL },
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

TILEWIN_SAVER(.saver = &saver_wordclock, .order = 120, .shot_seconds = 3);
