#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "saver_util.h"

/*
 * Hellfire: the day the windows burn. A white flash from a blast beyond the
 * top of the screen, then the heat: everything discolours like paper held to
 * a flame, the windows catch fire and burn through from a few spots each,
 * glowing edges eating across them, the text still showing in the charred
 * sheet, while flames, sparks and smoke rise. The shock wave follows and tears
 * the charred windows away as ash. What is left are scraps of the window
 * decorations and the taskbar, smouldering for ever at their edges and slowly
 * turning brown and red, on scorched ground under falling ash.
 *
 * The windows are the real ones: the picture of the screen taken before the
 * saver started. Without it (the preview of the settings) it brings its own.
 *
 * Each pixel is blended from a few pictures made at the start (the desktop,
 * the burnt ground behind the windows) by amounts kept per cell of a coarse
 * grid (discoloured, charred, gone, glowing), smoothly in between, in one
 * pass; flames, smoke, sparks and ash are sprites over it.
 */

#define CELL 4             // pixels per cell of the burn grid
#define WINDOWS_MAX 48
#define FLAMES_MAX 500
#define SMOKE_MAX 90
#define SPARKS_MAX 260
#define ASH_MAX 420
#define FLASH_AT 1.0       // seconds of the desktop as it was
#define SHOCK_AT 6.5       // seconds after the flash the shock wave comes
#define SHOCK_TIME 1.7     // and how long it takes over the screen
#define CHAR_TIME 2.4      // from the fire front reaching a spot to charred
#define THREADS_MAX 8      // the blend is shared out over the cores

enum kind {
	KIND_GROUND,   // the wallpaper: scorched and charred, never gone
	KIND_BODY,     // what a window shows: burns away
	KIND_DECO,     // title bars, borders, the taskbar: burn away, but for
	KIND_REMNANT,  // the scraps of them that smoulder on
	KIND_BAR,      // the taskbar while planning: decoration, of which less is left
};

enum channel { CH_SCORCH, CH_CHAR, CH_GONE, CH_EMBER, CH_HOT, CH_GLOW, CHANNELS };

struct flame {
	float x, y, vx, vy, life, age, size;
};

struct smoke {
	float x, y, vx, vy, life, age, size, shade;
};

struct spark {
	float x, y, vx, vy, life, age;
};

struct ash {
	float x, y, vx, vy, life, age, size, spin, angle, shade;
};

struct hellfire {
	int width, height;
	double u, t;
	int gw, gh;
	uint32_t *base;          // the desktop
	uint32_t *hell;          // the burnt ground behind the windows
	uint8_t *grain;          // per pixel: fine noise that makes the burnt edges ragged
	cairo_surface_t *frame;  // what the blend writes
	// per cell, set at the start
	float *burn;             // when the fire reaches it (seconds after the flash)
	float *scorch;           // when the heat of the flash discolours it
	float *gone_at;          // when it is blown away or crumbles
	float *crawl;            // scraps: when the slow fire at their edges passes
	float *keep;             // decoration: over 0.5 a scrap that stays, smoothly in between
	float *phase;            // for the flicker
	unsigned char *kind;
	// per cell, each frame
	uint8_t *ch[CHANNELS];
	uint16_t *blur;          // for the glow
	// the blend
	int *col_i, *col_w;      // per pixel column: the cell left of it and how far in
	uint16_t *row[CHANNELS]; // one row of cells, blended between two rows of the grid
	uint16_t *vig_col, *vig_row;
	uint16_t *dist;          // per pixel: how far from the blast, in pixels
	int dist_max;
	uint16_t *fire_lut, *ring_lut; // per distance: the glare of the fireball, the wave
	uint16_t *dust_lut;      // and the dark dust behind the wave
	int threads;
	double ox, oy;           // the blast
	double diag;
	struct flame flames[FLAMES_MAX];
	struct smoke smoke[SMOKE_MAX];
	struct spark sparks[SPARKS_MAX];
	struct ash ash[ASH_MAX];
	int flame_next, smoke_next, spark_next, ash_next;
	cairo_surface_t *flame_sprite, *core_sprite, *smoke_sprite;
	double wind;
};

/* ---------- noise ---------- */

static uint32_t hash2(int x, int y, uint32_t seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static float value_noise(float x, float y, uint32_t seed) {
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	float a = (hash2(xi, yi, seed) & 0xffff) / 65535.0f;
	float b = (hash2(xi + 1, yi, seed) & 0xffff) / 65535.0f;
	float c = (hash2(xi, yi + 1, seed) & 0xffff) / 65535.0f;
	float d = (hash2(xi + 1, yi + 1, seed) & 0xffff) / 65535.0f;
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* Fractal noise in [0, 1): rough edges on every scale, like burning paper. */
static float fbm(float x, float y, uint32_t seed) {
	float sum = 0, amp = 0.5f, total = 0;
	for (int i = 0; i < 5; i++) {
		sum += value_noise(x, y, seed + i * 17) * amp;
		total += amp;
		x *= 2.03f;
		y *= 2.03f;
		amp *= 0.5f;
	}
	return sum / total;
}

static float smooth01(float v) {
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	return v * v * (3 - 2 * v);
}

/* ---------- the pictures ---------- */

/* Windows of its own, for the preview or when there is no picture of the screen. */
static void draw_fake_desktop(struct hellfire *s, cairo_t *cr, struct saver_window *wins,
		int count, struct saver_window *bars, int bar_count) {
	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, s->height);
	cairo_pattern_add_color_stop_rgb(sky, 0, 0.18, 0.45, 0.78);
	cairo_pattern_add_color_stop_rgb(sky, 0.7, 0.45, 0.7, 0.9);
	cairo_pattern_add_color_stop_rgb(sky, 1, 0.3, 0.6, 0.25);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);
	double u = s->u;
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

static void make_fake_windows(struct hellfire *s, struct saver_window *wins, int *count,
		struct saver_window *bars, int *bar_count) {
	static const char *const titles[] = { "Documents", "Notes - Editor", "Music",
		"Holiday photos", "Terminal" };
	*count = 3 + (int)(saver_random() * 2);
	for (int i = 0; i < *count; i++) {
		struct saver_window *b = &wins[i];
		b->w = s->width * saver_between(0.25, 0.4);
		b->h = s->height * saver_between(0.3, 0.5);
		b->x = saver_between(0.03, 0.97) * (s->width - b->w);
		b->y = saver_between(0.05, 0.85) * (s->height * 0.93 - b->h);
		b->title_h = fmax(8, s->u * 26);
		b->border = 1;
		snprintf(b->title, sizeof(b->title), "%s", titles[i % 5]);
	}
	double bar = fmax(8, s->height * 0.05);
	bars[0] = (struct saver_window){ .x = 0, .y = s->height - bar, .w = s->width, .h = bar };
	*bar_count = 1;
}

/* The burnt ground: charcoal with cracks, lit red from below. */
static void make_hell(struct hellfire *s) {
	for (int y = 0; y < s->height; y++) {
		float glow = (float)y / s->height;
		glow = glow * glow;
		for (int x = 0; x < s->width; x++) {
			float n = fbm(x * 0.012f, y * 0.012f, 91);
			float crack = fbm(x * 0.05f, y * 0.05f, 7);
			float c = fabsf(crack - 0.5f) < 0.02f ? 0.55f : 1; // thin dark cracks
			int r = (int)((16 + n * 26 + glow * 55) * c);
			int g = (int)((9 + n * 14 + glow * 13) * c);
			int b = (int)((7 + n * 9 + glow * 4) * c);
			s->hell[y * s->width + x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
}

static cairo_surface_t *radial_sprite(int size, const double stops[][4], int count) {
	cairo_surface_t *sprite = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(sprite);
	cairo_pattern_t *p = cairo_pattern_create_radial(size / 2.0, size / 2.0, 0, size / 2.0,
		size / 2.0, size / 2.0);
	for (int i = 0; i < count; i++) {
		double a = stops[i][3];
		cairo_pattern_add_color_stop_rgba(p, i / (double)(count - 1), stops[i][0], stops[i][1],
			stops[i][2], a);
	}
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	cairo_destroy(cr);
	return sprite;
}

/* ---------- where it burns and when ---------- */

static void mark(struct hellfire *s, double x0, double y0, double x1, double y1, int kind) {
	int cx0 = (int)fmax(0, floor(x0 / CELL)), cx1 = (int)fmin(s->gw, ceil(x1 / CELL));
	int cy0 = (int)fmax(0, floor(y0 / CELL)), cy1 = (int)fmin(s->gh, ceil(y1 / CELL));
	for (int cy = cy0; cy < cy1; cy++) {
		for (int cx = cx0; cx < cx1; cx++) {
			s->kind[cy * s->gw + cx] = (unsigned char)kind;
		}
	}
}

/* When the fire reaches each cell of a window: from a few spots, with ragged fronts. */
static void ignite(struct hellfire *s, struct saver_window *b, int index) {
	double bx = b->x - b->border, by = b->y - b->border;
	double bw = b->w + 2 * b->border, bh = b->h + 2 * b->border;
	double start = saver_between(0.8, 4.5);
	double speed = hypot(bw, bh) / saver_between(11, 18); // pixels a second
	int spots = 2 + (int)(saver_random() * 3);
	double sx[5], sy[5], st[5];
	for (int i = 0; i < spots; i++) {
		// the first where the heat of the blast hits first: the side facing it
		sx[i] = bx + bw * saver_between(0.05, 0.95);
		sy[i] = i == 0 ? by + bh * saver_between(0, 0.15) : by + bh * saver_between(0.1, 0.95);
		st[i] = start + (i ? saver_between(0.5, 6) : 0);
	}
	int cx0 = (int)fmax(0, floor(bx / CELL)), cx1 = (int)fmin(s->gw, ceil((bx + bw) / CELL));
	int cy0 = (int)fmax(0, floor(by / CELL)), cy1 = (int)fmin(s->gh, ceil((by + bh) / CELL));
	for (int cy = cy0; cy < cy1; cy++) {
		for (int cx = cx0; cx < cx1; cx++) {
			double px = (cx + 0.5) * CELL, py = (cy + 0.5) * CELL;
			double best = INFINITY;
			for (int i = 0; i < spots; i++) {
				double d = hypot(px - sx[i], py - sy[i]);
				// the front runs faster up, the way fire does
				d *= py < sy[i] ? 0.75 : 1.15;
				best = fmin(best, st[i] + d / speed);
			}
			float n = fbm(px * 0.018f, py * 0.018f, 300 + index);
			s->burn[cy * s->gw + cx] = (float)(best * (0.7 + 0.6 * n) + n * 1.5);
		}
	}
}

static void plan(struct hellfire *s, struct saver_window *wins, int count,
		struct saver_window *bars, int bar_count) {
	int cells = s->gw * s->gh;
	memset(s->kind, KIND_GROUND, cells);
	for (int i = 0; i < cells; i++) {
		int cx = i % s->gw, cy = i / s->gw;
		float px = (cx + 0.5f) * CELL, py = (cy + 0.5f) * CELL;
		// the ground is seared by the heat soon after the flash, some of it sooner
		s->burn[i] = 1.5f + fbm(px * 0.008f, py * 0.008f, 11) * 9;
		// flickering together with its neighbours, as flames do, not cell by cell
		s->phase[i] = fbm(px * 0.03f, py * 0.03f, 5) * 18.85f; // three turns
	}
	for (int i = 0; i < count; i++) {
		struct saver_window *b = &wins[i];
		double bd = b->border;
		// the whole window, then what is not its content: the decoration
		mark(s, b->x - bd, b->y - bd, b->x + b->w + bd, b->y + b->h + bd, KIND_DECO);
		mark(s, b->x + fmax(bd, 2), b->y + b->title_h, b->x + b->w - fmax(bd, 2),
			b->y + b->h - fmax(bd, 2), KIND_BODY);
		ignite(s, b, i);
	}
	for (int i = 0; i < bar_count; i++) {
		struct saver_window *b = &bars[i];
		mark(s, b->x, b->y, b->x + b->w, b->y + b->h, KIND_BAR);
		struct saver_window whole = *b;
		whole.border = 0;
		ignite(s, &whole, 90 + i);
	}
	double shock_speed = s->diag * 1.25 / SHOCK_TIME;
	for (int i = 0; i < cells; i++) {
		int cx = i % s->gw, cy = i / s->gw;
		float px = (cx + 0.5f) * CELL, py = (cy + 0.5f) * CELL;
		double d = hypot(px - s->ox, py - s->oy);
		float n = fbm(px * 0.01f, py * 0.01f, 23);
		s->scorch[i] = (float)(0.1 + (d - fabs(s->oy)) / (s->diag * 2.2) + n * 0.6);
		double shock = SHOCK_AT + (d - fabs(s->oy)) / shock_speed;
		if (s->kind[i] == KIND_DECO || s->kind[i] == KIND_BAR) {
			// scraps of the decoration are left, in blotches; of the broad taskbar fewer
			float keep = fbm(px * 0.02f, py * 0.02f, 77) - (s->kind[i] == KIND_BAR ? 0.1f : 0);
			s->kind[i] = KIND_DECO;
			s->keep[i] = keep;
			if (keep > 0.47f) {
				s->kind[i] = KIND_REMNANT;
				s->burn[i] += 3 + keep * 6;
				s->crawl[i] = s->burn[i] + 12 + fbm(px * 0.03f, py * 0.03f, 41) * 240;
			}
		}
		if (s->kind[i] != KIND_GROUND) {
			double charred = s->burn[i] + CHAR_TIME;
			// charred before the wave comes: torn away with it; later ones crumble
			s->gone_at[i] = charred <= shock ? (float)(shock + n * 0.25) :
				(float)(charred + 1.2 + n * 2.5);
		} else {
			s->gone_at[i] = INFINITY;
		}
	}
}

/* ---------- each frame: the cells ---------- */

static void update_cells(struct hellfire *s, double t) {
	int cells = s->gw * s->gh;
	float ft = (float)t;
	for (int i = 0; i < cells; i++) {
		int kind = s->kind[i];
		float a = ft - s->burn[i];
		float ph = s->phase[i];
		float flicker = 0.72f + 0.28f * sinf(ft * 9.3f + ph) * sinf(ft * 3.1f + ph * 1.7f);
		// the flash browns everything a little at once, the heat of a fire nearby fully
		float flash = smooth01((ft - s->scorch[i]) / 2.2f);
		float sc = 0, chr = 0, gone = 0, ember = 0, hot = 0;
		// the burning edge: a thin line, white hot, and red hot char just behind it
		float band = (a - 0.1f) / 0.22f;
		float front = band > -3 && band < 3 ? expf(-band * band) : 0;
		float afterglow = a > 0 && a < 12 ? expf(-a / 1.4f) : 0;
		switch (kind) {
		case KIND_GROUND:
			// the ground chars slowly in spreading patches, down to the burnt earth
			// the ground chars softly in spreading patches, glowing a little as it does
			sc = flash * 0.5f + smooth01((a + 6) / 6) * 0.4f;
			chr = smooth01(a / 7);
			hot = afterglow * 0.12f * flicker;
			break;
		case KIND_BODY:
		case KIND_DECO:
			sc = fmaxf(flash * 0.55f, smooth01((a + 3.5f) / 3.5f));
			chr = smooth01(a / CHAR_TIME);
			gone = smooth01((ft - s->gone_at[i]) / 0.5f);
			ember = front * flicker * (1 - gone);
			hot = afterglow * 0.55f * flicker * (1 - gone);
			break;
		case KIND_REMNANT: {
			// a scrap that stays: it browns and reddens slowly, chars a little, and a
			// slow fire crawls across it now and then
			// each part at its own pace: some brown early, some red, some charring
			float pace = 0.55f + ph / 18.85f * 0.9f;
			sc = flash * 0.25f + smooth01(a * pace / 60) * 0.75f;
			chr = 0.15f * smooth01(a / 3) + (0.25f + 0.3f * (ph / 18.85f)) * smooth01(a / 90);
			float c = (ft - s->crawl[i]) / 1.6f;
			float crawl = c > -3 && c < 3 ? expf(-c * c) : 0;
			ember = fmaxf(front * 0.6f, crawl * 0.7f) * flicker;
			// its outline where the rest burns away, between the cells of the grid
			float edge = smooth01((s->keep[i] - 0.47f) / 0.07f);
			gone = (1 - edge) * smooth01((ft - s->gone_at[i]) / 0.5f);
			hot = smooth01(a * (1.6f - pace) / 90) * (0.16f + 0.1f * sinf(ft * 0.7f + ph)) +
				afterglow * 0.3f;
			break;
		}
		}
		s->ch[CH_SCORCH][i] = (uint8_t)(sc * 255);
		s->ch[CH_CHAR][i] = (uint8_t)(chr * 255);
		s->ch[CH_GONE][i] = (uint8_t)(gone * 255);
		s->ch[CH_EMBER][i] = (uint8_t)(fminf(1, ember) * 255);
		s->ch[CH_HOT][i] = (uint8_t)(fminf(1, hot) * 255);
	}
	// the torn edges of the scraps glow where the rest has gone
	for (int cy = 1; cy < s->gh - 1; cy++) {
		for (int cx = 1; cx < s->gw - 1; cx++) {
			int i = cy * s->gw + cx;
			if (s->kind[i] != KIND_REMNANT) {
				continue;
			}
			int g = s->ch[CH_GONE][i - 1] | s->ch[CH_GONE][i + 1] | s->ch[CH_GONE][i - s->gw] |
				s->ch[CH_GONE][i + s->gw];
			if (g > 128) {
				float ph = s->phase[i];
				float e = 0.27f + 0.2f * sinf(ft * 2.3f + ph) * sinf(ft * 5.1f + ph * 3);
				int v = (int)(e * 255);
				if (v > s->ch[CH_EMBER][i]) {
					s->ch[CH_EMBER][i] = (uint8_t)v;
				}
			}
		}
	}
	// the glow: the embers spread wide, horizontally then vertically
	int r = 4;
	for (int cy = 0; cy < s->gh; cy++) {
		const uint8_t *src = s->ch[CH_EMBER] + cy * s->gw;
		uint16_t *dst = s->blur + cy * s->gw;
		int sum = 0;
		for (int cx = -r; cx < s->gw + r; cx++) {
			int in = cx + r, out = cx - r - 1;
			if (in >= 0 && in < s->gw) {
				sum += src[in];
			}
			if (out >= 0 && out < s->gw) {
				sum -= src[out];
			}
			if (cx >= 0 && cx < s->gw) {
				dst[cx] = (uint16_t)sum;
			}
		}
	}
	int norm = (2 * r + 1) * (2 * r + 1);
	for (int cx = 0; cx < s->gw; cx++) {
		int sum = 0;
		for (int cy = -r; cy < s->gh + r; cy++) {
			int in = cy + r, out = cy - r - 1;
			if (in >= 0 && in < s->gh) {
				sum += s->blur[in * s->gw + cx];
			}
			if (out >= 0 && out < s->gh) {
				sum -= s->blur[out * s->gw + cx];
			}
			if (cy >= 0 && cy < s->gh) {
				int v = sum * 2 / norm;
				s->ch[CH_GLOW][cy * s->gw + cx] = (uint8_t)(v > 255 ? 255 : v);
			}
		}
	}
}

/* ---------- each frame: the pixels ---------- */

struct band {
	struct hellfire *s;
	int y0, y1;
	int tr, tg, tb, bright, fl, ghost;
	uint16_t *row[CHANNELS]; // this band's own row of cells
};

static void *blend_band(void *data) {
	struct band *band = data;
	struct hellfire *s = band->s;
	int W = s->width, gw = s->gw;
	uint32_t *out = (uint32_t *)cairo_image_surface_get_data(s->frame);
	int stride = cairo_image_surface_get_stride(s->frame) / 4;
	int tr = band->tr, tg = band->tg, tb = band->tb, bright = band->bright, fl = band->fl;
	int ghost = band->ghost;
	for (int y = band->y0; y < band->y1; y++) {
		// the two rows of cells around this row of pixels, blended
		float gy = (y + 0.5f) / CELL - 0.5f;
		int y0 = (int)floorf(gy);
		int wy = (int)((gy - y0) * 256);
		if (y0 < 0) {
			y0 = 0;
			wy = 0;
		}
		int y1 = y0 + 1 < s->gh ? y0 + 1 : y0;
		for (int c = 0; c < CHANNELS; c++) {
			const uint8_t *a = s->ch[c] + y0 * gw, *b = s->ch[c] + y1 * gw;
			uint16_t *row = band->row[c];
			for (int i = 0; i < gw; i++) {
				row[i] = (uint16_t)((a[i] * (256 - wy) + b[i] * wy) >> 8);
			}
		}
		const uint32_t *bp = s->base + y * W, *hp = s->hell + y * W;
		uint32_t *op = out + y * stride;
		int vr = s->vig_row[y] * bright >> 8;
		const uint16_t *sc_r = band->row[CH_SCORCH], *ch_r = band->row[CH_CHAR];
		const uint16_t *gn_r = band->row[CH_GONE], *em_r = band->row[CH_EMBER];
		const uint16_t *ht_r = band->row[CH_HOT], *gl_r = band->row[CH_GLOW];
		const uint16_t *dp = s->dist + y * W;
		for (int x = 0; x < W; x++) {
			int i0 = s->col_i[x], wx = s->col_w[x], i1 = i0 + 1 < gw ? i0 + 1 : i0;
#define LERP(r) ((r[i0] * (256 - wx) + r[i1] * wx) >> 8)
			int sc = LERP(sc_r), chr = LERP(ch_r), gn = LERP(gn_r);
			int em = LERP(em_r), ht = LERP(ht_r), gl = LERP(gl_r);
#undef LERP
			if (gn > 0 && gn < 255) {
				// a torn edge down to the pixel, not the smooth one of the coarse grid
				int ragged = ((gn - 128) * 4) + ((int)s->grain[y * W + x] - 128) * 2 + 128;
				gn = ragged < 0 ? 0 : ragged > 255 ? 255 : ragged;
			}
			uint32_t p = bp[x];
			int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			int lum = (r * 77 + g * 150 + b * 29) >> 8;
			if (sc) { // yellowed and browned like paper near a flame
				int sr = (lum * 196 >> 8) + 34, sg = (lum * 146 >> 8) + 20, sb = (lum * 96 >> 8) + 9;
				r += (sr - r) * sc >> 8;
				g += (sg - g) * sc >> 8;
				b += (sb - b) * sc >> 8;
			}
			if (chr) {
				// charred to the burnt ground, the writing still showing faintly a while
				uint32_t q = hp[x];
				int cr = (int)((q >> 16) & 255) + (lum * ghost >> 8);
				int cg = (int)((q >> 8) & 255) + (lum * ghost * 2 / 3 >> 8);
				int cb = (int)(q & 255) + (lum * ghost / 2 >> 8);
				r += (cr - r) * chr >> 8;
				g += (cg - g) * chr >> 8;
				b += (cb - b) * chr >> 8;
			}
			if (gn) {
				uint32_t q = hp[x];
				r += ((int)((q >> 16) & 255) - r) * gn >> 8;
				g += ((int)((q >> 8) & 255) - g) * gn >> 8;
				b += ((int)(q & 255) - b) * gn >> 8;
			}
			// the glowing edge, white hot in its middle, and its red glow around; the glare
			// of the fireball and the wall of the shock wave by the distance from the blast
			int d = dp[x], fire = s->fire_lut[d], ring = s->ring_lut[d], dust = s->dust_lut[d];
			if (dust) {
				r = r * (256 - dust) >> 8;
				g = g * (256 - dust) >> 8;
				b = b * (256 - dust) >> 8;
			}
			r += (em * 255 + ht * 150 + gl * 170 + fire * 255 + ring * 250) >> 8;
			g += (em * 186 + ht * 30 + gl * 52 + fire * 120 + ring * 190) >> 8;
			b += (em * 88 + ht * 4 + gl * 8 + fire * 35 + ring * 130) >> 8;
			int v = vr * s->vig_col[x] >> 8;
			r = (r * v >> 8) * tr >> 8;
			g = (g * v >> 8) * tg >> 8;
			b = (b * v >> 8) * tb >> 8;
			if (r > 255) {
				// hotter than red can show: towards white, not a lemon yellow
				int over = r - 255;
				g += over >> 1;
				b += over >> 2;
			}
			if (fl) {
				r += ((255 - r) * fl) >> 8;
				g += ((255 - g) * fl) >> 8;
				b += ((245 - b) * fl) >> 8;
			}
			r = r > 255 ? 255 : r < 0 ? 0 : r;
			g = g > 255 ? 255 : g < 0 ? 0 : g;
			b = b > 255 ? 255 : b < 0 ? 0 : b;
			op[x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
	return NULL;
}

static void blend(struct hellfire *s, double t) {
	cairo_surface_flush(s->frame);
	// the flash: white, then a hot orange that dims towards the end
	double flash = t < 0 ? 0 : t < 0.08 ? t / 0.08 : t < 0.35 ? 1 : exp(-(t - 0.35) / 0.7);
	double heat = smooth01((float)(t / 3));
	double dusk = smooth01((float)((t - 18) / 40));
	// the fireball beyond the top of the screen, dimming to a red glare
	double fire = t < 0 ? 0 : t < 0.3 ? t / 0.3 : 0.22 + 0.78 * exp(-(t - 0.3) / 6);
	fire *= 0.9 + 0.1 * sin(t * 4.3) * sin(t * 1.7);
	double reach = s->diag * (0.55 + 0.25 * smooth01((float)(t / 8)));
	// and the wave: a wall of hot dust rolling out from it
	double wt = t - SHOCK_AT;
	double wave_r = fabs(s->oy) + wt * s->diag * 1.25 / SHOCK_TIME;
	double wave = wt > 0 && wt < SHOCK_TIME + 0.6 ? 1 - wt / (SHOCK_TIME + 0.6) : 0;
	double thick = s->u * 320;
	for (int d = 0; d <= s->dist_max; d++) {
		double k = d / reach;
		s->fire_lut[d] = (uint16_t)(k < 1 ? fire * 150 * (1 - k) * (1 - k) : 0);
		// a hot rim, and dark dust rolling behind it
		double w = (wave_r - d) / thick; // 0 at the front, 1 behind it
		double rim = w < -0.08 || w > 0.3 ? 0 : w < 0 ? (w + 0.08) / 0.08 : 1 - w / 0.3;
		double dust = w < 0 || w > 1 ? 0 : (w < 0.1 ? w / 0.1 : 1) * (1 - w);
		s->ring_lut[d] = (uint16_t)(wave * rim * 55);
		s->dust_lut[d] = (uint16_t)(wave * dust * 150);
	}
	struct band bands[THREADS_MAX];
	pthread_t ids[THREADS_MAX];
	int n = s->threads;
	for (int i = 0; i < n; i++) {
		bands[i] = (struct band){ s, s->height * i / n, s->height * (i + 1) / n,
			256, (int)(256 - 38 * heat - 20 * dusk), (int)(256 - 95 * heat - 40 * dusk),
			(int)(256 * (1 - 0.28 * dusk)), (int)(flash * 256),
			(int)(34 * (1 - smooth01((float)((t - 12) / 15)))), { 0 } };
		for (int c = 0; c < CHANNELS; c++) {
			bands[i].row[c] = s->row[c] + i * s->gw;
		}
	}
	int started = 0;
	for (int i = 1; i < n; i++) {
		if (pthread_create(&ids[i], NULL, blend_band, &bands[i]) != 0) {
			break;
		}
		started = i;
	}
	blend_band(&bands[0]);
	for (int i = started + 1; i < n; i++) {
		blend_band(&bands[i]); // a thread that could not be started: done here
	}
	for (int i = 1; i <= started; i++) {
		pthread_join(ids[i], NULL);
	}
	cairo_surface_mark_dirty(s->frame);
}

/* ---------- flames, smoke, sparks and ash ---------- */

static void spawn(struct hellfire *s, double t, double dt) {
	double u = s->u;
	int samples = (int)(s->gw * s->gh * dt * 0.5);
	samples = samples > 2500 ? 2500 : samples;
	for (int k = 0; k < samples; k++) {
		int i = (int)(saver_random() * s->gw * s->gh);
		int em = s->ch[CH_EMBER][i], gn = s->ch[CH_GONE][i];
		float x = (i % s->gw + (float)saver_random()) * CELL;
		float y = (i / s->gw + (float)saver_random()) * CELL;
		if (em > 110 && gn < 200 && saver_random() < em / 255.0 * 0.5) {
			struct flame *f = &s->flames[s->flame_next];
			s->flame_next = (s->flame_next + 1) % FLAMES_MAX;
			float big = s->kind[i] == KIND_GROUND ? 0.6f : s->kind[i] == KIND_REMNANT ? 0.75f : 1;
			*f = (struct flame){ x, y, (float)(saver_between(-12, 12) * u),
				(float)(-saver_between(40, 95) * u), (float)saver_between(0.45, 1.0), 0,
				(float)(saver_between(9, 20) * u * big * (0.5 + em / 510.0)) };
			if (saver_random() < 0.12) {
				struct spark *sp = &s->sparks[s->spark_next];
				s->spark_next = (s->spark_next + 1) % SPARKS_MAX;
				*sp = (struct spark){ x, y, (float)(saver_between(-40, 40) * u),
					(float)(-saver_between(80, 220) * u), (float)saver_between(0.8, 2.2), 0 };
			}
			if (saver_random() < 0.05) {
				struct smoke *sm = &s->smoke[s->smoke_next];
				s->smoke_next = (s->smoke_next + 1) % SMOKE_MAX;
				*sm = (struct smoke){ x, y - (float)(20 * u), (float)(saver_between(-6, 6) * u),
					(float)(-saver_between(18, 40) * u), (float)saver_between(5, 9), 0,
					(float)(saver_between(40, 80) * u), (float)saver_between(0.1, 0.25) };
			}
		}
		// what crumbles goes up as ash, and what the wave tears off flies with it
		if (gn > 20 && gn < 200 && saver_random() < 0.35) {
			struct ash *a = &s->ash[s->ash_next];
			s->ash_next = (s->ash_next + 1) % ASH_MAX;
			double dx = x - s->ox, dy = y - s->oy, d = hypot(dx, dy);
			bool wave = fabs(t - s->gone_at[i]) < 1 && s->gone_at[i] < SHOCK_AT + SHOCK_TIME + 1;
			double v = wave ? saver_between(300, 700) * u : saver_between(10, 50) * u;
			*a = (struct ash){ x, y, (float)(dx / d * v), (float)(dy / d * v - (wave ? 0 : 30 * u)),
				(float)saver_between(2, 5), 0, (float)(saver_between(1.5, 4.5) * u),
				(float)saver_between(-8, 8), (float)saver_between(0, 6.28),
				(float)saver_between(0.08, 0.4) };
		}
	}
	// ash falls out of the sky all the time once the wave has come
	if (t > SHOCK_AT && saver_random() < dt * 25) {
		struct ash *a = &s->ash[s->ash_next];
		s->ash_next = (s->ash_next + 1) % ASH_MAX;
		*a = (struct ash){ (float)saver_between(-0.1, 1) * s->width, (float)(-10 * u),
			(float)(saver_between(10, 40) * u), (float)(saver_between(15, 45) * u),
			(float)saver_between(8, 16), 0, (float)(saver_between(1.5, 3.5) * u),
			(float)saver_between(-4, 4), (float)saver_between(0, 6.28),
			(float)saver_between(0.12, 0.3) };
	}
}

static void draw_sprite(cairo_t *cr, cairo_surface_t *sprite, double x, double y, double size,
		double tall, double alpha) {
	if (alpha <= 0.004 || size < 1) {
		return;
	}
	int w = cairo_image_surface_get_width(sprite);
	cairo_save(cr);
	cairo_translate(cr, x - size / 2, y - size * tall / 2);
	cairo_scale(cr, size / w, size * tall / w);
	cairo_set_source_surface(cr, sprite, 0, 0);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

static void draw_particles(struct hellfire *s, cairo_t *cr, double dt) {
	double u = s->u;
	// smoke first, under the fire
	for (int i = 0; i < SMOKE_MAX; i++) {
		struct smoke *m = &s->smoke[i];
		if (m->age >= m->life) {
			continue;
		}
		m->age += (float)dt;
		m->x += (float)((m->vx + s->wind * u) * dt);
		m->y += (float)(m->vy * dt);
		float f = m->age / m->life;
		float alpha = m->shade * sinf(f * (float)M_PI);
		draw_sprite(cr, s->smoke_sprite, m->x, m->y, m->size * (1 + f * 2.5f), 1, alpha);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int i = 0; i < FLAMES_MAX; i++) {
		struct flame *f = &s->flames[i];
		if (f->age >= f->life) {
			continue;
		}
		f->age += (float)dt;
		f->vx += (float)(s->wind * u * 0.6 * dt);
		f->x += (float)(f->vx * dt);
		f->y += (float)(f->vy * dt);
		float k = f->age / f->life;
		// licking up: bright and small at the root, wider and redder as it rises
		float size = f->size * (0.6f + 0.9f * k);
		// a tongue: tall and narrow, the root brighter
		draw_sprite(cr, s->flame_sprite, f->x, f->y - size * 0.5f, size * 1.1f, 2.2, 0.6 * (1 - k));
		draw_sprite(cr, s->core_sprite, f->x, f->y, size * 0.55f, 1.8, 0.75 * (1 - k) * (1 - k));
	}
	for (int i = 0; i < SPARKS_MAX; i++) {
		struct spark *p = &s->sparks[i];
		if (p->age >= p->life) {
			continue;
		}
		p->age += (float)dt;
		p->vx += (float)((s->wind * u + saver_between(-60, 60) * u) * dt);
		p->vy += (float)(20 * u * dt);
		p->x += (float)(p->vx * dt);
		p->y += (float)(p->vy * dt);
		float k = 1 - p->age / p->life;
		double tw = 0.6 + 0.4 * sin(p->age * 30 + i);
		cairo_rectangle(cr, p->x, p->y, fmax(1, u * 2), fmax(1, u * 2));
		cairo_set_source_rgba(cr, 1, 0.6 + 0.3 * k, 0.2, k * tw);
		cairo_fill(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	for (int i = 0; i < ASH_MAX; i++) {
		struct ash *a = &s->ash[i];
		if (a->age >= a->life) {
			continue;
		}
		a->age += (float)dt;
		// slowed by the air, fluttering down
		a->vx += (float)((s->wind * u - a->vx) * fmin(1, dt * 1.2));
		a->vy += (float)((25 * u - a->vy) * fmin(1, dt * 1.2));
		a->x += (float)((a->vx + sin(a->age * 3 + i) * 15 * u) * dt);
		a->y += (float)(a->vy * dt);
		a->angle += (float)(a->spin * dt);
		float k = fminf(1, (a->life - a->age) * 2);
		cairo_save(cr);
		cairo_translate(cr, a->x, a->y);
		cairo_rotate(cr, a->angle);
		cairo_scale(cr, 1, 0.35 + 0.65 * fabs(sin(a->age * 2.5 + i)));
		cairo_rectangle(cr, -a->size / 2, -a->size / 2, a->size, a->size);
		cairo_restore(cr);
		cairo_set_source_rgba(cr, a->shade * 1.1, a->shade * 0.95, a->shade * 0.85, 0.8 * k);
		cairo_fill(cr);
	}
}

/* ---------- the saver ---------- */

static void *hellfire_create(int width, int height, const struct saver_options *options) {
	struct hellfire *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->u = saver_unit(width, height);
	s->diag = hypot(width, height);
	s->gw = (width + CELL - 1) / CELL + 1;
	s->gh = (height + CELL - 1) / CELL + 1;
	int cells = s->gw * s->gh;
	s->burn = calloc(cells, sizeof(float));
	s->scorch = calloc(cells, sizeof(float));
	s->gone_at = calloc(cells, sizeof(float));
	s->crawl = calloc(cells, sizeof(float));
	s->keep = calloc(cells, sizeof(float));
	s->phase = calloc(cells, sizeof(float));
	s->kind = calloc(cells, 1);
	s->blur = calloc(cells, sizeof(uint16_t));
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	s->threads = cores < 1 ? 1 : cores > THREADS_MAX ? THREADS_MAX : (int)cores;
	for (int c = 0; c < CHANNELS; c++) {
		s->ch[c] = calloc(cells, 1);
		s->row[c] = calloc((size_t)s->gw * s->threads, sizeof(uint16_t));
	}
	s->col_i = calloc(width, sizeof(int));
	s->col_w = calloc(width, sizeof(int));
	s->vig_col = calloc(width, sizeof(uint16_t));
	s->vig_row = calloc(height, sizeof(uint16_t));
	for (int x = 0; x < width; x++) {
		float gx = (x + 0.5f) / CELL - 0.5f;
		int i0 = (int)floorf(gx);
		s->col_w[x] = i0 < 0 ? 0 : (int)((gx - i0) * 256);
		s->col_i[x] = i0 < 0 ? 0 : i0;
		double d = (x - width / 2.0) / (width / 2.0);
		s->vig_col[x] = (uint16_t)(256 * (1 - 0.3 * d * d));
	}
	for (int y = 0; y < height; y++) {
		double d = (y - height / 2.0) / (height / 2.0);
		s->vig_row[y] = (uint16_t)(256 * (1 - 0.3 * d * d));
	}
	// the blast: high above the screen, somewhere along it
	s->ox = width * saver_between(0.15, 0.85);
	s->oy = -height * saver_between(0.25, 0.45);
	s->wind = saver_between(-25, 25);
	s->dist = malloc((size_t)width * height * sizeof(uint16_t));
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int d = (int)hypot(x - s->ox, y - s->oy);
			s->dist[y * width + x] = (uint16_t)(d > 65535 ? 65535 : d);
			s->dist_max = d > s->dist_max ? d : s->dist_max;
		}
	}
	s->fire_lut = calloc(s->dist_max + 1, sizeof(uint16_t));
	s->ring_lut = calloc(s->dist_max + 1, sizeof(uint16_t));
	s->dust_lut = calloc(s->dist_max + 1, sizeof(uint16_t));

	// the desktop: the real one, or windows of its own
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count = 0, bar_count = 0;
	bool real = saver_tilewin_windows(options->output, wins, WINDOWS_MAX, &count, bars,
		&bar_count);
	if (!real) {
		make_fake_windows(s, wins, &count, bars, &bar_count);
	}
	s->frame = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(s->frame);
	if (options->desktop && real) {
		cairo_scale(cr, (double)width / cairo_image_surface_get_width(options->desktop),
			(double)height / cairo_image_surface_get_height(options->desktop));
		cairo_set_source_surface(cr, options->desktop, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
	} else {
		for (int i = 0; i < count; i++) {
			wins[i].title_h = wins[i].title_h > 0 ? wins[i].title_h : fmax(8, s->u * 26);
		}
		draw_fake_desktop(s, cr, wins, count, bars, bar_count);
	}
	cairo_destroy(cr);
	cairo_surface_flush(s->frame);
	s->base = malloc((size_t)width * height * 4);
	s->hell = malloc((size_t)width * height * 4);
	s->grain = malloc((size_t)width * height);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			float n = value_noise(x * 0.35f, y * 0.35f, 61) * 0.6f +
				value_noise(x * 0.9f, y * 0.9f, 62) * 0.4f;
			s->grain[y * width + x] = (uint8_t)(n * 255);
		}
	}
	uint32_t *data = (uint32_t *)cairo_image_surface_get_data(s->frame);
	int stride = cairo_image_surface_get_stride(s->frame) / 4;
	for (int y = 0; y < height; y++) {
		memcpy(s->base + y * width, data + y * stride, width * 4);
	}
	make_hell(s);
	plan(s, wins, count, bars, bar_count);

	static const double flame[][4] = { { 1, 0.55, 0.12, 0.9 }, { 0.95, 0.3, 0.05, 0.55 },
		{ 0.6, 0.08, 0.02, 0.2 }, { 0.3, 0.02, 0, 0 } };
	static const double core[][4] = { { 1, 0.95, 0.75, 1 }, { 1, 0.75, 0.3, 0.6 },
		{ 1, 0.45, 0.1, 0 } };
	static const double smoke[][4] = { { 0.04, 0.03, 0.03, 1 }, { 0.05, 0.04, 0.035, 0.6 },
		{ 0.06, 0.05, 0.04, 0 } };
	s->flame_sprite = radial_sprite(64, flame, 4);
	s->core_sprite = radial_sprite(48, core, 3);
	s->smoke_sprite = radial_sprite(96, smoke, 3);
	return s;
}

static void hellfire_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct hellfire *s = state;
	s->t += dt;
	double t = s->t - FLASH_AT; // seconds since the flash
	update_cells(s, t < 0 ? -100 : t);
	blend(s, t);
	double u = s->u;
	cairo_save(cr);
	// the shock wave shakes the ground as it passes
	double shake = t > SHOCK_AT && t < SHOCK_AT + SHOCK_TIME + 1.2 ?
		(1 - (t - SHOCK_AT) / (SHOCK_TIME + 1.2)) * u * 7 : 0;
	if (shake > 0) {
		cairo_translate(cr, sin(t * 71) * shake, cos(t * 53) * shake);
	}
	cairo_set_source_surface(cr, s->frame, 0, 0);
	cairo_paint(cr);
	if (t > 0) {
		spawn(s, t, dt);
		draw_particles(s, cr, dt);
	}
	cairo_restore(cr);
}

static void hellfire_destroy(void *state) {
	struct hellfire *s = state;
	free(s->burn);
	free(s->scorch);
	free(s->gone_at);
	free(s->crawl);
	free(s->keep);
	free(s->phase);
	free(s->kind);
	free(s->blur);
	for (int c = 0; c < CHANNELS; c++) {
		free(s->ch[c]);
		free(s->row[c]);
	}
	free(s->col_i);
	free(s->col_w);
	free(s->vig_col);
	free(s->vig_row);
	free(s->dist);
	free(s->fire_lut);
	free(s->ring_lut);
	free(s->dust_lut);
	free(s->base);
	free(s->hell);
	free(s->grain);
	cairo_surface_destroy(s->frame);
	cairo_surface_destroy(s->flame_sprite);
	cairo_surface_destroy(s->core_sprite);
	cairo_surface_destroy(s->smoke_sprite);
	free(s);
}

const struct saver saver_hellfire = {
	.name = "hellfire",
	.title = "Hellfire",
	.description = "The day the windows burn: a flash, the heat, the shock wave, and "
		"scraps of the windows smouldering on",
	.wants_desktop = true,
	.create = hellfire_create,
	.draw = hellfire_draw,
	.destroy = hellfire_destroy,
};
