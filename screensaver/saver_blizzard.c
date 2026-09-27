#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Blizzard: the desktop snowed in. The light turns cold and flat, and snow
 * comes in on a wind that rises to a blizzard and falls off again: a far
 * haze of fine flakes behind the windows, flakes that drive past and settle,
 * big soft ones close in front, and now and then a crystal drifting by close
 * enough to see its six arms. The snow piles up on the tops of the windows
 * that are really open and on the taskbar, soft and lumpy, heaped to the
 * angle it holds and no further: what is pushed over an end gathers in a
 * cornice until a lump of it breaks off, falls, and bursts on whatever is
 * below. Gusts blow powder off the crests and whiten the air, streams of
 * blowing snow run along the bottom, and a drift rises there until the
 * taskbar is buried. Icicles grow down from the lower edges of the windows
 * and from the snow over the title bars, and a gust now and then breaks one
 * off to shatter below. Frost creeps over the glass of the windows from
 * their edges and corners: a haze first, which blurs what is behind it, and
 * ice ferns branching into it, leaving a clearer hollow in the middle; and
 * later it grows in from the edges of the screen itself, on which a flake
 * sometimes lands and melts.
 *
 * The windows and the taskbar are the real ones: the picture of the screen
 * from before the saver, and where they are from tileWin.
 */

#define WINDOWS_MAX 48
#define LEDGES_MAX 256
#define COL_LEDGES 8
#define FAR_FLAKES 1500
#define MID_FLAKES 800
#define NEAR_FLAKES 64
#define CRYSTALS 8
#define LENS_MAX 12
#define CLUMPS_MAX 40
#define POWDER_MAX 520
#define ICICLES_MAX 420
#define SHARDS_MAX 24
#define GLINTS_MAX 90
#define BOKEH_SIZES 5
#define GLASS_TILE 64
#define ROW_STEP 4         // the frost is brought up to date on every fourth row a frame
#define FROST_RAMP 4.0f    // seconds a spot of frost takes to set
#define INTRO 4.0          // seconds the light takes to turn wintry
#define NEVER 1e9f
#define TASKBAR -1
#define GROUND -2

struct flake {
	float x, y, px, py, size, phase;
};

struct near_flake {
	float x, y, size, phase, rot, spin;
	int crystal;         // a crystal close by, -1 for a soft blurred flake
};

/* Something snow lies on: the top of a window, of the taskbar, or the bottom of the screen. */
struct ledge {
	int x0, n;           // the first column and how many
	float y;             // the surface
	int owner;           // the window, TASKBAR or GROUND
	float *h, *cap;      // depth of the snow per column, and the most there is room for
	float *lump;         // how much each column takes of what falls, for a lumpy heap
	float spill[2];      // snow pushed over the left and right end, not yet broken off
};

struct clump {
	float x, y, vx, vy, r, rot, spin;
	bool alive;
};

struct powder {
	float x, y, vx, vy, size, life, age;
	bool ice;            // glittering ice, not snow dust
};

struct icicle {
	float x, y, len, max, w, grow_at, rate, seed;
	int owner;
};

struct shard {
	float x, y, vx, vy, len, w, rot, spin;
	bool alive;
};

struct glint {
	float x, y, size, life, age;
};

struct lens {
	float x, y, size, rot, age, life;
	int crystal;
	bool alive;
};

/* When each pixel freezes, and how thick its frost gets; per row the first and last time. */
struct frost_map {
	float *freeze;
	uint8_t *dens;
	float *row_first, *row_last;
};

struct blizzard {
	int width, height;
	double u, t;
	int frame;
	cairo_surface_t *orig;    // the desktop as it was
	cairo_surface_t *winter;  // in the cold light of the snow
	uint32_t *blurred;        // and that blurred, what frosted glass shows
	cairo_surface_t *view;    // the desktop now: turned cold, frosted over
	cairo_surface_t *glass;   // the frost on the screen itself, and its cold edges
	cairo_surface_t *fog[2];  // blowing snow, each layer tileable sideways: in the air,
	int fog_h;                // and along the ground, this high
	cairo_surface_t *bokeh[BOKEH_SIZES];
	float bokeh_r[BOKEH_SIZES];
	cairo_surface_t *crystal[CRYSTALS];
	int crystal_size;
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	struct ledge ledges[LEDGES_MAX];
	int ledge_count;
	short *cols;              // per column the ledges in it, top first, -1 after the last
	struct flake far[FAR_FLAKES], mid[MID_FLAKES];
	struct near_flake near[NEAR_FLAKES];
	struct clump clumps[CLUMPS_MAX];
	struct powder powder[POWDER_MAX];
	int powder_next;
	struct icicle icicles[ICICLES_MAX];
	int icicle_count;
	struct shard shards[SHARDS_MAX];
	struct glint glints[GLINTS_MAX];
	struct lens lens[LENS_MAX];
	struct frost_map frost, glass_frost;
	pthread_t worker;         // working out the frost and the blurred desktop
	bool working;             // it was started, and is to be joined
	atomic_bool ready;        // they are there
	uint64_t frost_rng;
	uint8_t *tiles;           // which tiles of GLASS_TILE have frost on the glass
	int tiles_w, tiles_h;
	double fog_x[2];          // how far the blowing snow has moved on
	cairo_surface_t *ice;     // the icicles, drawn anew only as they grow
	double ice_drawn;         // when last
	float glass_start;        // when the frost on the screen begins
	int intro_rows;           // frames of updating every row still to come after the intro
	// the weather
	double storm;             // how hard it snows, 0 to about 1.6
	double gust, wind, dir;
	// its settings
	double amount, whiteout;
	bool frost_on, bury;
};

/* ---------- noise ---------- */

static uint32_t hash2(int x, int y, uint32_t seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/* Value noise, repeating every period cells sideways (0 for never). */
static float noise(float x, float y, int period, uint32_t seed) {
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = xi, x1 = xi + 1;
	if (period > 0) {
		x0 = ((xi % period) + period) % period;
		x1 = (x0 + 1) % period;
	}
	float a = (hash2(x0, yi, seed) & 0xffff) / 65535.0f;
	float b = (hash2(x1, yi, seed) & 0xffff) / 65535.0f;
	float c = (hash2(x0, yi + 1, seed) & 0xffff) / 65535.0f;
	float d = (hash2(x1, yi + 1, seed) & 0xffff) / 65535.0f;
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float fbm(float x, float y, int period, uint32_t seed, int octaves) {
	float n = 0, amp = 0.5f, total = 0;
	for (int o = 0; o < octaves; o++) {
		n += noise(x, y, period, seed + o * 7) * amp;
		total += amp;
		x *= 2;
		y *= 2;
		period *= 2;
		amp *= 0.5f;
	}
	return n / total;
}

/* Noise worked out on a coarse grid and read between its points: for every pixel, cheaply. */
struct grid {
	int gw, gh, step;
	float *v;
};

static void grid_make(struct grid *g, int width, int height, int step, float cell, uint32_t seed) {
	g->step = step;
	g->gw = width / step + 2;
	g->gh = height / step + 2;
	g->v = malloc(sizeof(float) * g->gw * g->gh);
	for (int y = 0; y < g->gh; y++) {
		for (int x = 0; x < g->gw; x++) {
			g->v[y * g->gw + x] = fbm(x * step / cell, y * step / cell, 0, seed, 4);
		}
	}
}

static float grid_at(const struct grid *g, int x, int y) {
	int gx = x / g->step, gy = y / g->step;
	float fx = (float)(x % g->step) / g->step, fy = (float)(y % g->step) / g->step;
	const float *r0 = g->v + gy * g->gw + gx, *r1 = r0 + g->gw;
	return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
}

static float smooth(float edge0, float edge1, float x) {
	float k = (x - edge0) / (edge1 - edge0);
	k = k < 0 ? 0 : k > 1 ? 1 : k;
	return k * k * (3 - 2 * k);
}

/* ---------- pixels ---------- */

static inline uint32_t mix_px(uint32_t a, uint32_t b, int k) {
	// k from 0 (a) to 256 (b), each channel at once in two lanes
	uint32_t rb = ((a & 0xff00ff) * (256 - k) + (b & 0xff00ff) * k) >> 8 & 0xff00ff;
	uint32_t g = ((a & 0x00ff00) * (256 - k) + (b & 0x00ff00) * k) >> 8 & 0x00ff00;
	return 0xff000000u | rb | g;
}

/* The desktop in the flat, cold light of snow: most colour gone, blue, and hazy. */
static cairo_surface_t *winter_light(cairo_surface_t *desktop, int width, int height) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_surface_flush(desktop);
	const uint32_t *src = (const uint32_t *)cairo_image_surface_get_data(desktop);
	uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(out);
	int ss = cairo_image_surface_get_stride(desktop) / 4, ds = cairo_image_surface_get_stride(out) / 4;
	for (int y = 0; y < height; y++) {
		// the air thicker with snow towards the top
		float haze = 22 * (1 - (float)y / height);
		float dy = (y - height * 0.5f) / (height * 0.5f);
		for (int x = 0; x < width; x++) {
			uint32_t p = src[y * ss + x];
			float r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			float lum = r * 0.3f + g * 0.59f + b * 0.11f;
			// darker and bluer towards the edges of the screen, as through a cold window
			float dx = (x - width * 0.5f) / (width * 0.5f);
			float edge = 1 - 0.28f * smooth(0.55f, 1.5f, sqrtf(dx * dx + dy * dy));
			r = ((r * 0.4f + lum * 0.6f) * 0.62f + 38 + haze) * edge;
			g = ((g * 0.4f + lum * 0.6f) * 0.66f + 44 + haze) * edge;
			b = ((b * 0.4f + lum * 0.6f) * 0.7f + 58 + haze * 1.1f) * (0.3f + 0.7f * edge);
			dst[y * ds + x] = 0xff000000u | (uint32_t)fminf(255, r) << 16 |
				(uint32_t)fminf(255, g) << 8 | (uint32_t)fminf(255, b);
		}
	}
	cairo_surface_mark_dirty(out);
	return out;
}

/* A box blur of radius r, in place, along rows (dx 1) or columns. */
static void blur_line(uint32_t *px, int count, int step, int r, uint32_t *tmp) {
	for (int i = 0; i < count; i++) {
		tmp[i] = px[i * step];
	}
	int sr = 0, sg = 0, sb = 0, n = 2 * r + 1;
	for (int i = -r; i <= r; i++) {
		uint32_t p = tmp[i < 0 ? 0 : i >= count ? count - 1 : i];
		sr += (p >> 16) & 255;
		sg += (p >> 8) & 255;
		sb += p & 255;
	}
	for (int i = 0; i < count; i++) {
		px[i * step] = 0xff000000u | (uint32_t)(sr / n) << 16 | (uint32_t)(sg / n) << 8 |
			(uint32_t)(sb / n);
		uint32_t out = tmp[i - r < 0 ? 0 : i - r], in = tmp[i + r + 1 >= count ? count - 1 : i + r + 1];
		sr += (int)((in >> 16) & 255) - (int)((out >> 16) & 255);
		sg += (int)((in >> 8) & 255) - (int)((out >> 8) & 255);
		sb += (int)(in & 255) - (int)(out & 255);
	}
}

static uint32_t *blurred_copy(cairo_surface_t *src, int width, int height, int r) {
	uint32_t *out = malloc(sizeof(uint32_t) * width * height);
	const uint32_t *in = (const uint32_t *)cairo_image_surface_get_data(src);
	int stride = cairo_image_surface_get_stride(src) / 4;
	for (int y = 0; y < height; y++) {
		memcpy(out + y * width, in + y * stride, sizeof(uint32_t) * width);
	}
	uint32_t *tmp = malloc(sizeof(uint32_t) * (width > height ? width : height));
	for (int pass = 0; pass < 3; pass++) { // three boxes come close to a gaussian
		for (int y = 0; y < height; y++) {
			blur_line(out + y * width, width, 1, r, tmp);
		}
		for (int x = 0; x < width; x++) {
			blur_line(out + x, height, width, r, tmp);
		}
	}
	free(tmp);
	return out;
}

/* ---------- the pictures made beforehand ---------- */

/*
 * Blowing snow: streaks of it, stretched along the wind, repeating sideways; thicker
 * low down and a little at the top, or for streams along the ground only low down.
 */
static cairo_surface_t *make_fog(int width, int height, double u, uint32_t seed, float thick,
		bool ground) {
	int gw = width / 4 + 1, gh = height / 4 + 1;
	cairo_surface_t *half = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(half);
	int stride = cairo_image_surface_get_stride(half) / 4;
	float cell = (float)fmax(4, u * 110) / 4;
	int period = (int)fmaxf(1, roundf(gw / (cell * 3.5f)));
	float cx = (float)gw / period; // cells sideways, whole ones so it repeats
	for (int y = 0; y < gh; y++) {
		float fy = (float)y / gh;
		float low = ground ? fy * sqrtf(fy) * 1.2f :
			fy * fy * 1.1f + 0.25f * (1 - fy) * (1 - fy) * (1 - fy) + 0.18f;
		for (int x = 0; x < gw; x++) {
			float n = fbm(x / cx, y / cell, period, seed, 4);
			float d = smooth(0.32f, 0.82f, n) * low * thick;
			d = d > 1 ? 1 : d;
			uint32_t a = (uint32_t)(d * 255);
			uint32_t r = (uint32_t)(d * 0.9f * 255), g = (uint32_t)(d * 0.93f * 255),
				b = (uint32_t)(d * 0.98f * 255);
			px[y * stride + x] = a << 24 | r << 16 | g << 8 | b;
		}
	}
	cairo_surface_mark_dirty(half);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(out);
	cairo_scale(cr, (double)width / (gw - 1), (double)height / (gh - 1));
	cairo_set_source_surface(cr, half, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(half);
	return out;
}

/* A flake close to the eye, out of focus: a soft bright disc. */
static cairo_surface_t *make_bokeh(double r) {
	int size = (int)ceil(r * 2 + 2);
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	double c = size / 2.0;
	cairo_pattern_t *p = cairo_pattern_create_radial(c, c, 0, c, c, r);
	cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.75);
	cairo_pattern_add_color_stop_rgba(p, 0.55, 0.95, 0.97, 1, 0.5);
	cairo_pattern_add_color_stop_rgba(p, 0.85, 0.9, 0.94, 1, 0.22);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.9, 0.94, 1, 0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	cairo_destroy(cr);
	return img;
}

/* One arm of a crystal, pointing up from the middle, its branches mirrored on both sides. */
static void crystal_arm(cairo_t *cr, double len, double *branch_at, double *branch_len,
		int branches, int depth) {
	cairo_move_to(cr, 0, 0);
	cairo_line_to(cr, 0, -len);
	for (int b = 0; b < branches; b++) {
		double y = -len * branch_at[b], l = len * branch_len[b];
		for (int side = -1; side <= 1; side += 2) {
			double ex = side * l * sin(M_PI / 3), ey = y - l * cos(M_PI / 3);
			cairo_move_to(cr, 0, y);
			cairo_line_to(cr, ex, ey);
			if (depth > 0 && l > len * 0.12) {
				// twigs off the branch, the same way again but smaller
				for (int k = 1; k <= 2; k++) {
					double f = k / 3.0, tl = l * 0.32 * (1 - f * 0.4);
					double bx = ex * f, by = y + (ey - y) * f;
					// sixty degrees off the branch: one along the arm, one outwards
					cairo_move_to(cr, bx, by);
					cairo_line_to(cr, bx, by - tl);
					cairo_move_to(cr, bx, by);
					cairo_line_to(cr, bx + side * tl * sin(M_PI / 3), by + tl * cos(M_PI / 3));
				}
			}
		}
	}
}

/* A snow crystal with six arms, as they look under a lens: a plate, branches, twigs. */
static cairo_surface_t *make_crystal(int size, int kind) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	cairo_translate(cr, size / 2.0, size / 2.0);
	double R = size * 0.44;
	double at[5], len[5];
	int branches = 2 + kind % 3;
	for (int b = 0; b < branches; b++) {
		at[b] = 0.3 + 0.6 * (b + saver_between(0.1, 0.8)) / branches;
		len[b] = saver_between(0.14, 0.36) * (1.15 - at[b] * 0.6);
	}
	int depth = kind % 2;
	double plate = R * saver_between(0.1, 0.24);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	// the plate in the middle, a hexagon with a second one inside
	for (int ring = 0; ring < 2; ring++) {
		double r = plate * (ring ? 0.55 : 1);
		for (int k = 0; k <= 6; k++) {
			double a = k * M_PI / 3 + M_PI / 6;
			if (k == 0) {
				cairo_move_to(cr, cos(a) * r, sin(a) * r);
			} else {
				cairo_line_to(cr, cos(a) * r, sin(a) * r);
			}
		}
		cairo_close_path(cr);
	}
	cairo_set_source_rgba(cr, 0.82, 0.9, 1, 0.3);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, fmax(0.6, size * 0.01));
	cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
	cairo_stroke(cr);
	// the arms: a wide faint glow, the body, and a bright core
	static const double widths[] = { 0.07, 0.034, 0.012 };
	static const double alphas[] = { 0.1, 0.55, 0.95 };
	for (int pass = 0; pass < 3; pass++) {
		for (int arm = 0; arm < 6; arm++) {
			cairo_save(cr);
			cairo_rotate(cr, arm * M_PI / 3);
			crystal_arm(cr, R, at, len, branches, depth);
			cairo_restore(cr);
		}
		cairo_set_line_width(cr, fmax(0.5, size * widths[pass]));
		if (pass == 0) {
			cairo_set_source_rgba(cr, 0.7, 0.82, 1, alphas[pass]);
		} else {
			cairo_set_source_rgba(cr, 0.93 + 0.07 * pass / 2, 0.97, 1, alphas[pass]);
		}
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	return img;
}

/* ---------- where the snow lies ---------- */

/* Whether a point is behind a window further up the stack than above, or behind a taskbar. */
static bool covered(struct blizzard *s, int above, double x, double y) {
	for (int j = above + 1; j < s->count; j++) {
		struct saver_window *b = &s->wins[j];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			return true;
		}
	}
	for (int j = 0; j < s->bar_count; j++) {
		struct saver_window *b = &s->bars[j];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			return true;
		}
	}
	return false;
}

/* The topmost window at a point, -1 for none. */
static int window_at(struct blizzard *s, float x, float y) {
	for (int i = s->count - 1; i >= 0; i--) {
		struct saver_window *b = &s->wins[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			return i;
		}
	}
	return -1;
}

static struct ledge *add_ledge(struct blizzard *s, int x0, int x1, float y, int owner) {
	if (s->ledge_count >= LEDGES_MAX || x1 - x0 < 3) {
		return NULL;
	}
	struct ledge *l = &s->ledges[s->ledge_count++];
	*l = (struct ledge){ .x0 = x0, .n = x1 - x0, .y = y, .owner = owner };
	l->h = calloc(l->n, sizeof(float));
	l->cap = malloc(l->n * sizeof(float));
	l->lump = malloc(l->n * sizeof(float));
	double u = s->u;
	uint32_t seed = (uint32_t)(saver_random() * 100000);
	for (int c = 0; c < l->n; c++) {
		float x = (float)(x0 + c);
		// no heap is quite even: some parts take more of what falls, and hold more
		float n = fbm(x / (float)(u * 60), 0.5f, 0, seed, 3);
		l->lump[c] = 0.45f + 1.1f * n;
		l->cap[c] = (float)(u * 44 * (0.7 + 0.6 * n));
	}
	return l;
}

/* The tops of the windows where they show, the taskbar, and the bottom of the screen. */
static void make_ledges(struct blizzard *s) {
	double u = s->u, W = s->width, H = s->height;
	for (int i = 0; i < s->count; i++) {
		struct saver_window *w = &s->wins[i];
		if (w->y < u * 8 || w->y >= H) {
			continue; // no room above it
		}
		int x0 = (int)fmax(0, ceil(w->x)), x1 = (int)fmin(W, floor(w->x + w->w));
		int start = -1;
		float room[8192];
		for (int x = x0; x <= x1; x++) {
			bool open = x < x1 && !covered(s, i, x + 0.5, w->y + 0.5);
			float r = 0;
			if (open) {
				// no higher than the room under a window in front of it above
				r = (float)w->y;
				for (int j = i + 1; j < s->count; j++) {
					struct saver_window *f = &s->wins[j];
					if (x + 0.5 >= f->x && x + 0.5 < f->x + f->w && f->y + f->h <= w->y + 0.5) {
						r = fminf(r, (float)(w->y - (f->y + f->h)));
					}
				}
				open = r >= 2;
			}
			if (open && start < 0) {
				start = x;
			}
			if (open && x - start < 8192) {
				room[x - start] = r;
			}
			if (!open && start >= 0) {
				struct ledge *l = add_ledge(s, start, x, (float)w->y, i);
				for (int c = 0; l && c < l->n && c < 8192; c++) {
					l->cap[c] = fminf(l->cap[c], room[c]);
				}
				start = -1;
			}
		}
	}
	bool bottom_bar = false;
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		if (b->y < H * 0.5) {
			continue; // at the top: nothing lands on it
		}
		bottom_bar = true;
		add_ledge(s, (int)fmax(0, b->x), (int)fmin(W, b->x + b->w), (float)b->y, TASKBAR);
	}
	if (s->bury) {
		// the drift at the bottom, which buries the taskbar
		double deep = u * 70;
		for (int i = 0; i < s->bar_count; i++) {
			if (s->bars[i].y >= H * 0.5) {
				deep = s->bars[i].h;
			}
		}
		struct ledge *l = add_ledge(s, 0, s->width, (float)H, GROUND);
		uint32_t seed = (uint32_t)(saver_random() * 100000);
		for (int c = 0; l && c < l->n; c++) {
			// dunes: crests rising over the taskbar, hollows between
			float n = fbm(c / (float)(u * 220), 0.3f, 0, seed, 3);
			l->cap[c] = (float)(deep * (bottom_bar ? 0.5 + 0.75 * n : 0.3 + 0.7 * n));
			l->lump[c] = 0.3f + 1.4f * n;
		}
	}
	// per column, the ledges in it, top first
	s->cols = malloc(sizeof(short) * s->width * COL_LEDGES);
	for (int i = 0; i < s->width * COL_LEDGES; i++) {
		s->cols[i] = -1;
	}
	for (int k = 0; k < s->ledge_count; k++) {
		struct ledge *l = &s->ledges[k];
		for (int c = 0; c < l->n; c++) {
			short *col = s->cols + (l->x0 + c) * COL_LEDGES;
			int at = 0;
			while (at < COL_LEDGES && col[at] >= 0 && s->ledges[col[at]].y <= l->y) {
				at++;
			}
			if (at >= COL_LEDGES) {
				continue;
			}
			memmove(col + at + 1, col + at, sizeof(short) * (COL_LEDGES - at - 1));
			col[at] = (short)k;
		}
	}
}

/* The first snow surface at or below y in a column, and its ledge; the bottom when none. */
static float surface_below(struct blizzard *s, float x, float y, struct ledge **out) {
	*out = NULL;
	int c = (int)x;
	if (c < 0 || c >= s->width) {
		return (float)s->height + 1000;
	}
	short *col = s->cols + c * COL_LEDGES;
	for (int k = 0; k < COL_LEDGES && col[k] >= 0; k++) {
		struct ledge *l = &s->ledges[col[k]];
		float top = l->y - l->h[c - l->x0];
		if (top >= y - 1) {
			*out = l;
			return top;
		}
	}
	return (float)s->height + 1000;
}

/* Snow onto a ledge around x: amount is pixels of area, spread over a few columns. */
static void deposit(struct blizzard *s, struct ledge *l, float x, float amount, float spread) {
	int c = (int)(x - l->x0);
	int r = (int)fmaxf(1, spread);
	float total = 0;
	for (int k = -r; k <= r; k++) {
		total += (float)(r + 1 - abs(k));
	}
	for (int k = -r; k <= r; k++) {
		int i = c + k;
		if (i < 0 || i >= l->n) {
			continue;
		}
		l->h[i] += amount * (r + 1 - abs(k)) / total * l->lump[i];
	}
}

static struct powder *new_powder(struct blizzard *s) {
	struct powder *p = &s->powder[s->powder_next];
	s->powder_next = (s->powder_next + 1) % POWDER_MAX;
	return p;
}

static void puff(struct blizzard *s, float x, float y, int bits, float power, bool ice) {
	double u = s->u;
	for (int k = 0; k < bits; k++) {
		double a = saver_between(-M_PI * 0.95, -M_PI * 0.05), v = saver_between(20, 110) * u * power;
		*new_powder(s) = (struct powder){ x + (float)saver_between(-3, 3) * (float)u, y,
			(float)(cos(a) * v), (float)(sin(a) * v), (float)(saver_between(1.2, 3.6) * u),
			(float)saver_between(0.5, 1.4), 0, ice && saver_random() < 0.6 };
	}
}

/* A lump breaking off an end of a ledge. */
static void break_off(struct blizzard *s, struct ledge *l, int side) {
	for (int i = 0; i < CLUMPS_MAX; i++) {
		struct clump *c = &s->clumps[i];
		if (c->alive) {
			continue;
		}
		int col = side ? l->n - 1 : 0;
		float r = sqrtf(l->spill[side] / (float)M_PI);
		double out = side ? 1 : -1;
		*c = (struct clump){ (float)(l->x0 + col + out * r), l->y - l->h[col] * 0.5f,
			(float)(out * saver_between(5, 30) * s->u + s->wind * 0.15), 0,
			fmaxf(1.5f, r), (float)saver_between(0, 6), (float)saver_between(-3, 3), true };
		l->spill[side] = 0;
		puff(s, c->x, c->y, 5, 0.4f, false);
		return;
	}
}

/* The snow on the ledges: new snow, heaped no steeper than it holds, overhanging ends breaking. */
static void settle(struct blizzard *s, double dt) {
	double u = s->u;
	float slope = 1.0f;
	// (some from the start, so there is snow to see soon)
	float grow = (float)(u * 0.2 * fmax(s->storm, 0.7 * s->amount) * dt);
	for (int k = 0; k < s->ledge_count; k++) {
		struct ledge *l = &s->ledges[k];
		float *h = l->h;
		float g = l->owner == GROUND ? grow * 1.5f : grow;
		for (int c = 0; c < l->n; c++) {
			h[c] += g * l->lump[c];
		}
		// no steeper than it holds: what is too steep slides down beside it
		for (int pass = 0; pass < 2; pass++) {
			for (int c = 0; c + 1 < l->n; c++) {
				float d = h[c] - h[c + 1];
				if (fabsf(d) > slope) {
					float m = (fabsf(d) - slope) * 0.5f * (d > 0 ? 1 : -1);
					h[c] -= m;
					h[c + 1] += m;
				}
			}
		}
		// soft: settled a little into itself
		float prev = h[0];
		for (int c = 1; c + 1 < l->n; c++) {
			float here = h[c];
			h[c] += 0.2f * (prev + h[c + 1] - 2 * here);
			prev = here;
		}
		// rounded at the ends, where what is too much goes over and gathers
		float edge = (float)(u * 1.5), round_w = fminf((float)(u * 16), l->n * 0.3f);
		for (int c = 0; c < l->n; c++) {
			int from_end = c < l->n - 1 - c ? c : l->n - 1 - c;
			float k = 1 - fminf(1, (from_end + 0.5f) / round_w);
			float most = fminf(l->cap[c], edge + (float)(u * 40) * sqrtf(1 - k * k) * 0.7f +
				(from_end + 0.5f) * 0.25f);
			if (l->owner == GROUND) {
				most = l->cap[c]; // the screen goes on beyond the edges
			}
			if (h[c] > most) {
				if (l->owner != GROUND && from_end < 30 + (int)(u * 20)) {
					l->spill[c < l->n / 2 ? 0 : 1] += h[c] - most;
				}
				h[c] = most;
			}
		}
		for (int side = 0; side < 2; side++) {
			if (l->spill[side] > u * u * 70 * saver_between(0.6, 1.6)) {
				break_off(s, l, side);
			}
		}
		// gusts blow powder off the crests
		if (s->gust > 0.45 && l->owner != GROUND &&
				saver_random() < dt * s->gust * l->n / (u * 90)) {
			int c = (int)(saver_random() * l->n);
			if (h[c] > u * 3) {
				h[c] -= (float)(u * 0.4);
				float x = (float)(l->x0 + c), y = l->y - h[c];
				for (int b = 0; b < 3; b++) {
					*new_powder(s) = (struct powder){ x, y - (float)(saver_random() * u * 3),
						(float)(s->wind * saver_between(0.4, 0.9)), (float)(-saver_between(5, 40) * u),
						(float)(saver_between(1, 2.6) * u), (float)saver_between(0.8, 2), 0, false };
				}
			}
		}
	}
}

/* ---------- ice ---------- */

/* Icicles along the lower edges of the windows, and short ones off the snow over title bars. */
static void make_icicles(struct blizzard *s) {
	double u = s->u, H = s->height;
	for (int i = 0; i < s->count; i++) {
		struct saver_window *w = &s->wins[i];
		double yb = w->y + w->h;
		if (yb >= H - u * 6 || yb <= 0) {
			continue;
		}
		double x = w->x + saver_between(2, 10) * u;
		while (x < w->x + w->w - 2 * u && s->icicle_count < ICICLES_MAX) {
			double here = x;
			x += saver_between(4, 13) * u;
			if (here < 0 || here >= s->width || covered(s, i, here, yb - 0.5) ||
					covered(s, i, here, yb + 1) || saver_random() < 0.35) {
				continue;
			}
			// longer towards the middle of the edge, as the water runs there
			double mid = 1 - fabs((here - w->x) / w->w - 0.5) * 1.4;
			double max = u * saver_between(6, 58) * (0.45 + 0.8 * mid) * saver_between(0.5, 1.1);
			// no further than the next thing below, and not behind a window in front
			double room = 0;
			while (room < max && yb + room < H && !covered(s, i, here, yb + room + 1) &&
					window_at(s, (float)here, (float)(yb + room + 1)) <= i) {
				room += 1;
			}
			max = fmin(max, room * 0.85);
			if (max < u * 3) {
				continue;
			}
			s->icicles[s->icicle_count++] = (struct icicle){ (float)here, (float)yb, 0, (float)max,
				(float)(u * saver_between(1.6, 2.2) * sqrt(max / u) * 0.55), (float)saver_between(10, 70),
				(float)(max / saver_between(50, 130)), (float)saver_between(0, 100), i };
		}
	}
	// short ones hanging from the snow over the title bars
	for (int k = 0; k < s->ledge_count; k++) {
		struct ledge *l = &s->ledges[k];
		if (l->owner < 0) {
			continue;
		}
		double x = l->x0 + saver_between(4, 20) * u;
		while (x < l->x0 + l->n - 3 * u && s->icicle_count < ICICLES_MAX) {
			double here = x;
			x += saver_between(8, 30) * u;
			double max = u * saver_between(3, 13);
			if (covered(s, l->owner, here, l->y + max) || saver_random() < 0.4) {
				continue;
			}
			s->icicles[s->icicle_count++] = (struct icicle){ (float)here, l->y, 0, (float)max,
				(float)(u * saver_between(1.4, 2.4)), (float)saver_between(30, 110),
				(float)(max / saver_between(40, 90)), (float)saver_between(0, 100), l->owner };
		}
	}
}

/* ---------- frost ---------- */

/*
 * The frost is worked out on a thread of its own while the saver starts (it
 * takes a while, and there is none for the first seconds anyway), with random
 * numbers of its own so the two do not share any.
 */
static double frost_random(struct blizzard *s) {
	s->frost_rng ^= s->frost_rng >> 12;
	s->frost_rng ^= s->frost_rng << 25;
	s->frost_rng ^= s->frost_rng >> 27;
	return ((s->frost_rng * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}

static double frost_between(struct blizzard *s, double low, double high) {
	return low + (high - low) * frost_random(s);
}

static void frost_alloc(struct frost_map *m, int width, int height) {
	size_t n = (size_t)width * height;
	m->freeze = malloc(sizeof(float) * n);
	m->dens = calloc(n, 1);
	for (size_t i = 0; i < n; i++) {
		m->freeze[i] = NEVER;
	}
	m->row_first = malloc(sizeof(float) * height);
	m->row_last = malloc(sizeof(float) * height);
}

static void frost_free(struct frost_map *m) {
	free(m->freeze);
	free(m->dens);
	free(m->row_first);
	free(m->row_last);
}

/* A part of the screen, in whole pixels, inside it. */
struct box {
	int x0, y0, x1, y1;
};

static struct box box_of(struct blizzard *s, double x, double y, double w, double h) {
	struct box b = { (int)fmax(0, round(x)), (int)fmax(0, round(y)),
		(int)fmin(s->width, round(x + w)), (int)fmin(s->height, round(y + h)) };
	return b;
}

static void frost_clear(struct blizzard *s, struct frost_map *m, struct box b) {
	for (int y = b.y0; y < b.y1; y++) {
		for (int x = b.x0; x < b.x1; x++) {
			m->freeze[y * s->width + x] = NEVER;
			m->dens[y * s->width + x] = 0;
		}
	}
}

/*
 * A haze of frost creeping in from the edges of a box, furthest at its
 * corners and ragged, leaving the middle clear: it starts at t0 and reaches
 * as far as it goes after grow seconds.
 */
static void frost_haze(struct blizzard *s, struct frost_map *m, struct box b, float t0,
		float reach, float grow, float strength, float corners, const struct grid *n1,
		const struct grid *n2) {
	// in a corner the frost comes from two sides and gets further; along the edges less so
	// away from the corners (further than reach * 3 from two edges) the frost goes no
	// further in than twice its widest reach there: the middle of such a row is skipped
	float most = reach * 3.3f;
	for (int y = b.y0; y < b.y1; y++) {
		float dy = fminf(y - b.y0, b.y1 - 1 - y);
		for (int x = b.x0; x < b.x1; x++) {
			if (dy > most && x == b.x0 + (int)most && b.x1 - (int)most > x) {
				x = b.x1 - (int)most; // the middle of the row is too far from any edge
			}
			float dx = fminf(x - b.x0, b.x1 - 1 - x);
			float near = fminf(dx, dy), far = fmaxf(dx, dy);
			float corner = 1 - smooth(0, reach * 3, far);
			float d = near * (0.5f + 0.5f * (1 - corner));
			float a = grid_at(n1, x, y), f = grid_at(n2, x, y);
			float r = reach * (0.3f + 1.3f * a * a) * (1 + corners * corner);
			if (d >= r) {
				continue;
			}
			float k = d / r;
			int i = y * s->width + x;
			float t = t0 + k * grow * (0.8f + 0.4f * f);
			// frost that gathers in the corners (the screen's) thins out along the edges
			float thin = corners > 3 ? 0.1f + 0.9f * corner : 1;
			float dens = thin * strength * (1 - k) * (1 - k) * (0.35f + 0.65f * f) + thin *
				(float)(hash2(x, y, 3) & 15) * (1 - k); // grain
			if (dens > m->dens[i]) {
				m->dens[i] = (uint8_t)fminf(255, dens);
			}
			if (t < m->freeze[i]) {
				m->freeze[i] = t;
			}
		}
	}
}

/* A dot of an ice fern: frost as thick as dens, set at time t, as far as it covers. */
static void frost_dot(struct blizzard *s, struct frost_map *m, struct box b, float cx, float cy,
		float r, float dens, float t) {
	int x0 = (int)fmaxf(b.x0, floorf(cx - r - 1)), x1 = (int)fminf(b.x1 - 1, ceilf(cx + r + 1));
	int y0 = (int)fmaxf(b.y0, floorf(cy - r - 1)), y1 = (int)fminf(b.y1 - 1, ceilf(cy + r + 1));
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float cover = r + 0.5f - hypotf(x + 0.5f - cx, y + 0.5f - cy);
			if (cover <= 0) {
				continue;
			}
			cover = fminf(1, cover);
			int i = y * s->width + x;
			float d = dens * cover;
			if (d > m->dens[i]) {
				m->dens[i] = (uint8_t)d;
			}
			if (cover > 0.3f && t < m->freeze[i]) {
				m->freeze[i] = t;
			}
		}
	}
}

/*
 * An ice fern from (x, y) along angle, len long, growing at speed pixels a
 * second from time t: a stem that bends a little, with branches off both sides
 * at sixty degrees, and those branching again.
 */
static void frost_fern(struct blizzard *s, struct frost_map *m, struct box b, float x, float y,
		float angle, float len, float width, float t, float speed, float dens, int depth) {
	double u = s->u;
	float step = 0.6f, walked = 0;
	// a feather: branches close together on both sides, shorter towards the tip
	float gap = (float)(u * (depth ? 1.6 : 2.4));
	float next_branch = gap * (float)frost_between(s, 0.5, 1.5);
	float bend = (float)frost_between(s, -0.012, 0.012) / (float)fmax(0.3, u) / (1 + depth);
	while (walked < len) {
		float f = walked / len;
		frost_dot(s, m, b, x, y, width * (1 - f * 0.7f), dens * (1 - f * 0.3f), t + walked / speed);
		x += cosf(angle) * step;
		y += sinf(angle) * step;
		angle += bend * step;
		bend += (float)frost_between(s, -0.002, 0.002) / (float)fmax(0.3, u);
		walked += step;
		if (x < b.x0 || x >= b.x1 || y < b.y0 || y >= b.y1) {
			return;
		}
		if (depth < 2 && walked >= next_branch && walked < len * 0.92f) {
			float blen = fminf((len - walked) * 0.55f, len * (depth ? 0.3f : 0.24f)) * (1 - f * 0.5f) *
				(float)frost_between(s, 0.6, 1.2);
			for (int side = -1; side <= 1; side += 2) {
				if (frost_random(s) < (depth ? 0.55 : 0.85)) {
					float a = angle + side * (float)(M_PI / 3 + frost_between(s, -0.15, 0.1));
					frost_fern(s, m, b, x, y, a, blen * (float)frost_between(s, 0.75, 1.1), width * 0.65f,
						t + walked / speed, speed * 0.8f, dens * 0.85f, depth + 1);
				}
			}
			next_branch = walked + gap * (float)frost_between(s, 0.7, 1.4);
		}
	}
}

/* Ferns growing in from the edges of a box, more of them from its corners. */
static void frost_ferns(struct blizzard *s, struct frost_map *m, struct box b, float t0,
		float reach, float grow, float dens, float every, double cornered) {
	double u = s->u;
	float w = (float)(b.x1 - b.x0), h = (float)(b.y1 - b.y0);
	int stems = (int)(2 * (w + h) / (u * every));
	for (int k = 0; k < stems; k++) {
		// a point along the edge, rather near a corner
		double along = frost_random(s);
		along = along < 0.5 ? pow(along * 2, cornered) / 2 : 1 - pow((1 - along) * 2, cornered) / 2;
		int edge = (int)(frost_random(s) * 4);
		float x, y, angle;
		switch (edge) {
		case 0: x = b.x0 + (float)along * w; y = (float)b.y0 + 0.5f; angle = (float)M_PI / 2; break;
		case 1: x = b.x0 + (float)along * w; y = b.y1 - 0.5f; angle = -(float)M_PI / 2; break;
		case 2: x = (float)b.x0 + 0.5f; y = b.y0 + (float)along * h; angle = 0; break;
		default: x = b.x1 - 0.5f; y = b.y0 + (float)along * h; angle = (float)M_PI; break;
		}
		// towards the middle of the corner it is near, not only straight in
		float corner = (float)fabs(along - 0.5) * 2;
		angle += (float)frost_between(s, -0.6, 0.6) * (0.5f + corner * 0.5f);
		float len = reach * (float)frost_between(s, 0.45, 1.25) * (float)(0.3 + 0.7 * pow(corner, cornered));
		float start = t0 + (float)frost_between(s, 0, grow * 0.35);
		float speed = reach / (grow * 0.75f);
		frost_fern(s, m, b, x, y, angle, len, (float)fmax(0.6, u * 0.85), start, speed, dens, 0);
	}
}

static void frost_rows(struct blizzard *s, struct frost_map *m) {
	for (int y = 0; y < s->height; y++) {
		float first = NEVER, last = -1;
		for (int x = 0; x < s->width; x++) {
			float t = m->freeze[y * s->width + x];
			if (t < NEVER && m->dens[y * s->width + x]) {
				first = fminf(first, t);
				last = fmaxf(last, t);
			}
		}
		m->row_first[y] = first;
		m->row_last[y] = last;
	}
}

static void make_frost(struct blizzard *s) {
	double u = s->u;
	struct grid n1, n2;
	grid_make(&n1, s->width, s->height, 4, (float)fmax(4, u * 70), 101);
	grid_make(&n2, s->width, s->height, 4, (float)fmax(4, u * 12), 202);
	frost_alloc(&s->frost, s->width, s->height);
	for (int i = 0; i < s->count; i++) {
		struct saver_window *w = &s->wins[i];
		struct box whole = box_of(s, w->x, w->y, w->w, w->h);
		frost_clear(s, &s->frost, whole); // a window in front hides the frost of those behind
		struct box glass = box_of(s, w->x, w->y + w->title_h, w->w, w->h - w->title_h);
		float t0 = (float)frost_between(s, 7, 16) + i * 1.5f;
		float reach = (float)fmin(glass.x1 - glass.x0, glass.y1 - glass.y0) * 0.2f;
		if (glass.x1 - glass.x0 > 4 && glass.y1 - glass.y0 > 4) {
			frost_haze(s, &s->frost, glass, t0, reach, 110, 70, 0.6f, &n1, &n2);
			frost_ferns(s, &s->frost, glass, t0 + 4, reach * 1.7f, 120, 255, 14, 1.3);
		}
	}
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		struct box bar = box_of(s, b->x, b->y, b->w, b->h);
		frost_clear(s, &s->frost, bar);
		frost_haze(s, &s->frost, bar, 18, (float)(bar.y1 - bar.y0) * 0.5f, 120, 70, 0.5f, &n1, &n2);
	}
	frost_rows(s, &s->frost);
	// the screen itself frosts over from its edges, later, slowly
	frost_alloc(&s->glass_frost, s->width, s->height);
	struct box screen = { 0, 0, s->width, s->height };
	float reach = (float)fmin(s->width, s->height) * 0.018f;
	s->glass_start = 22;
	frost_haze(s, &s->glass_frost, screen, s->glass_start, reach, 160, 90, 9, &n1, &n2);
	frost_ferns(s, &s->glass_frost, screen, s->glass_start + 4, reach * 8, 170, 250, 22, 3);
	frost_rows(s, &s->glass_frost);
	free(n1.v);
	free(n2.v);
}

/* The frost on the screen's glass at a pixel, premultiplied. */
static uint32_t glass_px(float a) {
	return (uint32_t)(a * 255) << 24 | (uint32_t)(a * 0.93f * 255) << 16 |
		(uint32_t)(a * 0.97f * 255) << 8 | (uint32_t)(a * 255);
}

/* The tiles of the screen with frost on its glass, the only ones it is drawn in. */
static void glass_tiles(struct blizzard *s) {
	int tw = (s->width + GLASS_TILE - 1) / GLASS_TILE, th = (s->height + GLASS_TILE - 1) / GLASS_TILE;
	s->tiles = calloc((size_t)tw * th, 1);
	s->tiles_w = tw;
	s->tiles_h = th;
	for (int y = 0; y < s->height; y++) {
		for (int x = 0; x < s->width; x++) {
			if (s->glass_frost.dens[y * s->width + x]) {
				s->tiles[(y / GLASS_TILE) * tw + x / GLASS_TILE] = 1;
			}
		}
	}
}

/* The desktop and the screen's glass brought up to date on every ROW_STEP-th row. */
static void update_rows(struct blizzard *s) {
	int W = s->width;
	float t = (float)s->t;
	bool intro = s->intro_rows > 0;
	bool frost = s->frost_on && atomic_load(&s->ready);
	if (!intro && !frost) {
		return;
	}
	int k_intro = (int)(256 * fmin(1, s->t / INTRO));
	uint32_t *orig = (uint32_t *)cairo_image_surface_get_data(s->orig);
	uint32_t *winter = (uint32_t *)cairo_image_surface_get_data(s->winter);
	uint32_t *view = (uint32_t *)cairo_image_surface_get_data(s->view);
	uint32_t *glass = (uint32_t *)cairo_image_surface_get_data(s->glass);
	int os = cairo_image_surface_get_stride(s->orig) / 4, ws = cairo_image_surface_get_stride(s->winter) / 4;
	int vs = cairo_image_surface_get_stride(s->view) / 4, gs = cairo_image_surface_get_stride(s->glass) / 4;
	const uint32_t frost_col = 0xffe6f0fe;
	cairo_surface_flush(s->view);
	cairo_surface_flush(s->glass);
	for (int y = s->frame % ROW_STEP; y < s->height; y += ROW_STEP) {
		struct frost_map *m = &s->frost;
		bool busy = frost && t >= m->row_first[y] && t <= m->row_last[y] + FROST_RAMP + 1.5f;
		if (intro || busy) {
			uint32_t *o = orig + y * os, *w = winter + y * ws, *v = view + y * vs;
			const uint32_t *bl = frost ? s->blurred + y * W : NULL;
			const float *fz = frost ? m->freeze + y * W : NULL;
			const uint8_t *dn = frost ? m->dens + y * W : NULL;
			for (int x = 0; x < W; x++) {
				if (!intro && (!dn[x] || fz[x] >= t || t - fz[x] > FROST_RAMP + 1.5f)) {
					continue; // as it was: no frost there yet, or set for a while
				}
				uint32_t p = k_intro < 256 ? mix_px(o[x], w[x], k_intro) : w[x];
				if (frost && dn[x] && fz[x] < t) {
					float a = dn[x] / 255.0f * fminf(1, (t - fz[x]) / FROST_RAMP);
					p = mix_px(p, bl[x], (int)fminf(256, a * 2.4f * 256));
					uint32_t col = frost_col;
					if (a > 0.5f) {
						// white ice on white shows by its shade: the crystals go steel blue
						int lum = (int)(((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8;
						float k = smooth(140, 230, (float)lum) * smooth(0.5f, 0.95f, a) * 0.8f;
						col = mix_px(frost_col, 0xff7288ad, (int)(k * 256));
					}
					p = mix_px(p, col, (int)(a * 0.92f * 256));
				}
				v[x] = p;
			}
		}
		m = &s->glass_frost;
		if (frost && t >= m->row_first[y] && t <= m->row_last[y] + FROST_RAMP + 1.5f) {
			uint32_t *g = glass + y * gs;
			const float *fz = m->freeze + y * W;
			const uint8_t *dn = m->dens + y * W;
			for (int x = 0; x < W; x++) {
				if (!dn[x] || fz[x] >= t || t - fz[x] > FROST_RAMP + 1.5f) {
					continue;
				}
				float a = dn[x] / 255.0f * fminf(1, (t - fz[x]) / FROST_RAMP) * 0.9f;
				g[x] = glass_px(a);
			}
		}
	}
	cairo_surface_mark_dirty(s->view);
	cairo_surface_mark_dirty(s->glass);
	if (s->t >= INTRO && s->intro_rows > 0) {
		s->intro_rows--;
	}
}

/* ---------- the snowfall ---------- */

/* A flake put back in, upwind of where it will show: above the screen or beside it. */
static void flake_reset(struct blizzard *s, struct flake *f, float fall, float drift, bool anywhere) {
	double W = s->width, H = s->height;
	if (anywhere) {
		f->x = (float)(saver_random() * W);
		f->y = (float)(saver_random() * H);
	} else {
		// a flake entering from the side the wind blows from, as often as that side lets in
		double side = fabs(drift) / (fabs(drift) + fall) * H / (H + W) * 2;
		if (saver_random() < side) {
			f->x = drift > 0 ? (float)saver_between(-20, -2) : (float)(W + saver_between(2, 20));
			f->y = (float)(saver_random() * H);
		} else {
			f->x = (float)(saver_random() * W);
			f->y = (float)saver_between(-30, -2);
		}
	}
	f->px = f->x;
	f->py = f->y;
	f->phase = (float)saver_between(0, 2 * M_PI);
}

/* How a flake moves at a point: the wind, eddies in it, and flutter. */
static void flake_velocity(struct blizzard *s, float x, float y, float phase, float depth,
		float fall, float *vx, float *vy) {
	double u = s->u, t = s->t;
	double eddy = s->storm * u * 70;
	*vx = (float)(s->wind * depth + eddy * sin(y / (u * 90) + t * 1.7 + phase) +
		u * 26 * sin(t * 2.3 + phase * 3));
	*vy = (float)(fall * (1 + 0.3 * sin(t * 1.9 + phase)) +
		eddy * 0.6 * sin(x / (u * 140) + t * 1.1 + phase * 0.5));
	if (*vy < fall * 0.15f) {
		*vy = fall * 0.15f; // an eddy slows it, but it comes down
	}
}

static void move_flake(struct blizzard *s, struct flake *f, float fall, float depth, double dt) {
	float vx, vy;
	flake_velocity(s, f->x, f->y, f->phase, depth, fall, &vx, &vy);
	f->px = f->x;
	f->py = f->y;
	f->x += vx * (float)dt;
	f->y += vy * (float)dt;
}

static bool flake_gone(struct blizzard *s, struct flake *f) {
	return f->y > s->height + 10 || f->x < -40 || f->x > s->width + 40;
}

/* ---------- the saver ---------- */

/* What takes long: the frost, and the blurred desktop it shows. */
static void *prepare(void *data) {
	struct blizzard *s = data;
	s->blurred = blurred_copy(s->winter, s->width, s->height, (int)fmax(1, s->u * 3.5));
	make_frost(s);
	glass_tiles(s);
	atomic_store(&s->ready, true);
	return NULL;
}

/* Waits for the frost to be worked out, if it is being. */
static void finish_preparing(struct blizzard *s) {
	if (s->working) {
		pthread_join(s->worker, NULL);
		s->working = false;
	}
}

static void *blizzard_create(int width, int height, const struct saver_options *options) {
	struct blizzard *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	double u = s->u = saver_unit(width, height);
	static const double amounts[] = { 1, 0.5, 1.35 };
	int snow = saver_choice(options, &saver_doomsday, "blizzard_snow");
	s->amount = amounts[snow];
	s->whiteout = snow == 2 ? 1.8 : snow == 1 ? 0.5 : 1;
	s->frost_on = saver_toggle(options, &saver_doomsday, "blizzard_frost");
	s->bury = saver_toggle(options, &saver_doomsday, "blizzard_bury");
	s->orig = saver_desktop(options, width, height, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	s->winter = winter_light(s->orig, width, height);
	s->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(s->view);
	cairo_set_source_surface(cr, s->orig, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	s->glass = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->ice = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->fog[0] = make_fog(width, height, u, 5, 0.75f, false);
	s->fog_h = (int)(height * 0.45);
	s->fog[1] = make_fog(width, s->fog_h, u * 0.6, 17, 1.0f, true);
	for (int k = 0; k < BOKEH_SIZES; k++) {
		s->bokeh_r[k] = (float)fmax(1.5, u * (2.5 + k * k * 2.2));
		s->bokeh[k] = make_bokeh(s->bokeh_r[k]);
	}
	s->crystal_size = (int)fmax(16, u * 110);
	for (int k = 0; k < CRYSTALS; k++) {
		s->crystal[k] = make_crystal(s->crystal_size, k);
	}
	make_ledges(s);
	make_icicles(s);
	s->frost_rng = (uint64_t)(saver_random() * 9007199254740992.0) | 1;
	if (s->frost_on) {
		s->working = pthread_create(&s->worker, NULL, prepare, s) == 0;
		if (!s->working) {
			prepare(s);
		}
	}
	s->dir = saver_random() < 0.5 ? -1 : 1;
	for (int i = 0; i < FAR_FLAKES; i++) {
		flake_reset(s, &s->far[i], 1, 0, true);
		s->far[i].size = (float)fmax(0.8, u * saver_between(0.9, 2));
	}
	for (int i = 0; i < MID_FLAKES; i++) {
		flake_reset(s, &s->mid[i], 1, 0, true);
		s->mid[i].size = (float)fmax(1, u * saver_between(1.8, 3.8));
	}
	for (int i = 0; i < NEAR_FLAKES; i++) {
		struct near_flake *f = &s->near[i];
		*f = (struct near_flake){ (float)(saver_random() * width), (float)(saver_random() * height),
			(float)(u * saver_between(6, 26)), (float)saver_between(0, 6.3), (float)saver_between(0, 6.3),
			(float)saver_between(-0.8, 0.8), i % 16 == 0 ? (i / 16) % CRYSTALS : -1 };
		if (f->crystal >= 0) {
			f->size = (float)(u * saver_between(34, 70));
		}
	}
	s->intro_rows = ROW_STEP + 1;
	return s;
}

static void draw_ledges(struct blizzard *s, cairo_t *cr, bool ground) {
	double u = s->u;
	for (int k = 0; k < s->ledge_count; k++) {
		struct ledge *l = &s->ledges[k];
		if ((l->owner == GROUND) != ground) {
			continue;
		}
		float top = 0;
		for (int c = 0; c < l->n; c++) {
			top = fmaxf(top, l->h[c]);
		}
		if (top < 0.4f) {
			continue;
		}
		double lip = ground ? 0 : fmin(u * 1.6, top * 0.3);
		// its shadow on what it lies on, under the rim
		if (!ground) {
			cairo_pattern_t *sh = cairo_pattern_create_linear(0, l->y, 0, l->y + lip + u * 6);
			cairo_pattern_add_color_stop_rgba(sh, 0, 0.05, 0.1, 0.25, 0.35 * fmin(1, top / (u * 5)));
			cairo_pattern_add_color_stop_rgba(sh, 1, 0.05, 0.1, 0.25, 0);
			cairo_set_source(cr, sh);
			cairo_rectangle(cr, l->x0, l->y, l->n, lip + u * 6);
			cairo_fill(cr);
			cairo_pattern_destroy(sh);
		}
		// the heap along its crest
		int step = l->n > 400 ? 3 : 2;
		double x1 = l->x0 + l->n, h0 = l->h[0], h1 = l->h[l->n - 1];
		// a cornice: the heap bulges out over each end a little
		double over0 = ground ? 0 : fmin(h0 * 0.45, u * 4), over1 = ground ? 0 : fmin(h1 * 0.45, u * 4);
		cairo_new_path(cr);
		cairo_move_to(cr, l->x0 + over0 * 0.2, l->y + lip);
		cairo_curve_to(cr, l->x0 - over0, l->y + lip * 0.5, l->x0 - over0, l->y - h0 * 0.7,
			l->x0 + 0.5, l->y - h0);
		for (int c = step; c < l->n; c += step) {
			cairo_line_to(cr, l->x0 + c + 0.5, l->y - l->h[c]);
		}
		cairo_line_to(cr, x1 - 0.5, l->y - h1);
		cairo_curve_to(cr, x1 + over1, l->y - h1 * 0.7, x1 + over1, l->y + lip * 0.5,
			x1 - over1 * 0.2, l->y + lip);
		cairo_close_path(cr);
		// white at the crest, blue in the shade low down
		cairo_pattern_t *p = cairo_pattern_create_linear(0, l->y - top, 0, l->y + lip);
		cairo_pattern_add_color_stop_rgb(p, 0, 0.99, 0.995, 1);
		cairo_pattern_add_color_stop_rgb(p, 0.45, 0.93, 0.955, 0.99);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.7, 0.77, 0.89);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		// the lit crest
		cairo_new_path(cr);
		for (int c = 0; c < l->n; c += step) {
			double y = l->y - l->h[c] + u * 0.6;
			if (c == 0) {
				cairo_move_to(cr, l->x0 + c + 0.5, y);
			} else {
				cairo_line_to(cr, l->x0 + c + 0.5, y);
			}
		}
		cairo_set_line_width(cr, fmax(0.8, u * 1.1));
		cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
		cairo_stroke(cr);
	}
}

static void grow_icicles(struct blizzard *s, double dt) {
	for (int i = 0; i < s->icicle_count; i++) {
		struct icicle *c = &s->icicles[i];
		if (s->t > c->grow_at && c->len < c->max) {
			c->len = fminf(c->max, c->len + c->rate * (float)(dt * (0.5 + s->storm * 0.5)));
		}
	}
}

/* The icicles as they are now, into their own layer. */
static void draw_icicles(struct blizzard *s) {
	double u = s->u;
	cairo_t *cr = cairo_create(s->ice);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	for (int i = 0; i < s->icicle_count; i++) {
		struct icicle *c = &s->icicles[i];
		if (c->len < 1) {
			continue;
		}
		// narrowing to a point, with the bulges the water froze in on its way down
		double half = c->w / 2;
		cairo_move_to(cr, c->x - half, c->y);
		for (int k = 1; k <= 6; k++) {
			double f = k / 6.0, wide = half * pow(1 - f, 0.85) * (1 + 0.14 * sin(f * 11 + c->seed));
			cairo_line_to(cr, c->x - wide + sin(c->seed) * f * u * 0.6, c->y + c->len * f);
		}
		for (int k = 5; k >= 0; k--) {
			double f = k / 6.0, wide = half * pow(1 - f, 0.85) * (1 + 0.14 * sin(f * 11 + c->seed + 2));
			cairo_line_to(cr, c->x + wide + sin(c->seed) * f * u * 0.6, c->y + c->len * f);
		}
		cairo_close_path(cr);
	}
	cairo_set_source_rgba(cr, 0.8, 0.9, 1, 0.55);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, fmax(0.5, u * 0.5));
	cairo_set_source_rgba(cr, 0.5, 0.62, 0.8, 0.35);
	cairo_stroke(cr);
	// the light running down the front of each
	for (int i = 0; i < s->icicle_count; i++) {
		struct icicle *c = &s->icicles[i];
		if (c->len < 3) {
			continue;
		}
		cairo_move_to(cr, c->x - c->w * 0.18, c->y + 1);
		cairo_line_to(cr, c->x - c->w * 0.04 + sin(c->seed) * u * 0.4, c->y + c->len * 0.8);
	}
	cairo_set_line_width(cr, fmax(0.6, u * 0.7));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
	cairo_stroke(cr);
	cairo_destroy(cr);
}

/* A glint at the tips, now and then. */
static void icicle_glints(struct blizzard *s, cairo_t *cr) {
	double u = s->u;
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	cairo_new_path(cr);
	for (int i = 0; i < s->icicle_count; i++) {
		struct icicle *c = &s->icicles[i];
		if (c->len < 4 || sin(s->t * 1.3 + c->seed * 7) < 0.85) {
			continue;
		}
		double r = u * 1.2;
		cairo_rectangle(cr, c->x - r / 2, c->y + c->len - r, r, r);
	}
	cairo_set_source_rgba(cr, 0.9, 0.95, 1, 0.9);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

/* The lumps, the falling icicles and the powder: moved, landed and drawn. */
static void draw_falling(struct blizzard *s, cairo_t *cr, double dt) {
	double u = s->u;
	for (int i = 0; i < CLUMPS_MAX; i++) {
		struct clump *c = &s->clumps[i];
		if (!c->alive) {
			continue;
		}
		float py = c->y;
		c->vy += (float)(u * 700 * dt);
		c->vx += (float)((s->wind * 0.2 - c->vx) * dt);
		c->x += c->vx * (float)dt;
		c->y += c->vy * (float)dt;
		c->rot += c->spin * (float)dt;
		struct ledge *l;
		float floor = surface_below(s, c->x, py, &l);
		if (c->y + c->r * 0.5f >= floor) {
			if (l) {
				deposit(s, l, c->x, (float)M_PI * c->r * c->r * 0.8f, c->r * 1.5f);
			}
			puff(s, c->x, floor, 10 + (int)(c->r / u), 0.8f, false);
			c->alive = false;
			continue;
		}
		if (c->y > s->height + c->r * 2) {
			c->alive = false;
			continue;
		}
		// a lump of snow: three soft rounds, shaded underneath
		cairo_new_path(cr);
		for (int k = 0; k < 3; k++) {
			double a = c->rot + k * 2.1, r = c->r * (k ? 0.7 : 1);
			cairo_new_sub_path(cr);
			cairo_arc(cr, c->x + cos(a) * c->r * 0.45 * (k > 0), c->y + sin(a) * c->r * 0.45 * (k > 0),
				r, 0, 2 * M_PI);
		}
		cairo_pattern_t *p = cairo_pattern_create_linear(0, c->y - c->r, 0, c->y + c->r);
		cairo_pattern_add_color_stop_rgb(p, 0, 1, 1, 1);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.72, 0.79, 0.9);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	// icicles a gust breaks off
	for (int i = 0; i < s->icicle_count && s->gust > 0.6; i++) {
		struct icicle *c = &s->icicles[i];
		if (c->len > c->max * 0.6f && c->len > u * 8 && saver_random() < dt * 0.012 * s->gust) {
			for (int k = 0; k < SHARDS_MAX; k++) {
				if (!s->shards[k].alive) {
					s->shards[k] = (struct shard){ c->x, c->y + c->len * 0.5f, (float)(s->wind * 0.05), 0,
						c->len, c->w, 0, (float)saver_between(-2, 2), true };
					c->len = 0;
					c->grow_at = (float)(s->t + saver_between(4, 20));
					s->ice_drawn = -1; // to be drawn without it at once
					break;
				}
			}
		}
	}
	for (int i = 0; i < SHARDS_MAX; i++) {
		struct shard *d = &s->shards[i];
		if (!d->alive) {
			continue;
		}
		float py = d->y;
		d->vy += (float)(u * 900 * dt);
		d->x += d->vx * (float)dt;
		d->y += d->vy * (float)dt;
		d->rot += d->spin * (float)dt;
		struct ledge *l;
		float floor = surface_below(s, d->x, py + d->len * 0.5f, &l);
		if (d->y + d->len * 0.5f >= floor || d->y > s->height + d->len) {
			// it shatters into glittering bits
			for (int k = 0; k < 14; k++) {
				double a = saver_between(-M_PI, 0), v = saver_between(40, 200) * u;
				*new_powder(s) = (struct powder){ d->x, floor - 1, (float)(cos(a) * v), (float)(sin(a) * v),
					(float)(u * saver_between(0.8, 2)), (float)saver_between(0.3, 0.9), 0, true };
			}
			d->alive = false;
			continue;
		}
		cairo_save(cr);
		cairo_translate(cr, d->x, d->y);
		cairo_rotate(cr, d->rot);
		cairo_move_to(cr, -d->w / 2, -d->len / 2);
		cairo_line_to(cr, d->w / 2, -d->len / 2);
		cairo_line_to(cr, 0, d->len / 2);
		cairo_close_path(cr);
		cairo_restore(cr);
		cairo_set_source_rgba(cr, 0.82, 0.92, 1, 0.75);
		cairo_fill(cr);
	}
	// powder and glittering ice: the dust as small soft flakes, the ice as bright points
	cairo_surface_t *dust = s->bokeh[0];
	int half = cairo_image_surface_get_width(dust) / 2;
	for (int i = 0; i < POWDER_MAX; i++) {
		struct powder *p = &s->powder[i];
		if (p->age >= p->life) {
			continue;
		}
		p->age += (float)dt;
		double drag = p->ice ? 0.3 : 2.2;
		p->vx += (float)((s->wind * (p->ice ? 0 : 0.8) - p->vx) * fmin(1, drag * dt));
		p->vy += (float)(u * (p->ice ? 600 : 40) * dt);
		p->vy -= (float)(p->vy * fmin(1, drag * 0.5 * dt));
		p->x += p->vx * (float)dt;
		p->y += p->vy * (float)dt;
		double k = 1 - p->age / p->life;
		if (p->ice) {
			continue; // drawn below, all together
		}
		cairo_set_source_surface(cr, dust, round(p->x) - half, round(p->y) - half);
		cairo_paint_with_alpha(cr, 0.9 * k);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	cairo_new_path(cr);
	for (int i = 0; i < POWDER_MAX; i++) {
		struct powder *p = &s->powder[i];
		if (p->age < p->life && p->ice) {
			double r = p->size * (1 - p->age / p->life);
			cairo_rectangle(cr, p->x - r / 2, p->y - r / 2, r, r);
		}
	}
	cairo_set_source_rgba(cr, 0.85, 0.93, 1, 0.9);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

/* Glints of light on the snow, here and there on its surface. */
static void draw_glints(struct blizzard *s, cairo_t *cr, double dt) {
	double u = s->u;
	int spawn = (int)(dt * 40 + saver_random());
	for (int n = 0; n < spawn && s->ledge_count; n++) {
		struct ledge *l = &s->ledges[(int)(saver_random() * s->ledge_count)];
		int c = (int)(saver_random() * l->n);
		if (l->h[c] < u * 1.5) {
			continue;
		}
		for (int i = 0; i < GLINTS_MAX; i++) {
			struct glint *g = &s->glints[i];
			if (g->age >= g->life) {
				*g = (struct glint){ (float)(l->x0 + c), l->y - l->h[c] + (float)(saver_random() *
					fmin(l->h[c], u * 5)), (float)(u * saver_between(1.5, 3.5)),
					(float)saver_between(0.15, 0.5), 0 };
				break;
			}
		}
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	cairo_new_path(cr);
	for (int i = 0; i < GLINTS_MAX; i++) {
		struct glint *g = &s->glints[i];
		if (g->age >= g->life) {
			continue;
		}
		g->age += (float)dt;
		double r = g->size * sin(M_PI * fmin(1, g->age / g->life));
		cairo_move_to(cr, g->x - r, g->y);
		cairo_line_to(cr, g->x + r, g->y);
		cairo_move_to(cr, g->x, g->y - r);
		cairo_line_to(cr, g->x, g->y + r);
	}
	cairo_set_line_width(cr, fmax(0.6, u * 0.6));
	cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
	cairo_stroke(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

static void draw_crystal(struct blizzard *s, cairo_t *cr, int crystal, double x, double y,
		double size, double rot, double alpha) {
	double k = size / s->crystal_size;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_rotate(cr, rot);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s->crystal[crystal], -s->crystal_size / 2.0, -s->crystal_size / 2.0);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

static void blizzard_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct blizzard *s = state;
	double u = s->u, W = width, H = height;
	s->t += dt;
	s->frame++;
	// the weather: rising to a blizzard over the first twenty seconds, then coming and going
	double build = smooth(0, 20, (float)s->t);
	double swell = 0.78 + 0.22 * sin(s->t * 2 * M_PI / 85 - M_PI / 2);
	s->gust += (saver_random() < dt * 0.13 * (0.4 + build) ? 1 : 0) - s->gust * dt * 0.3;
	s->gust = fmin(s->gust, 1.6);
	s->storm = s->amount * (0.3 + 0.7 * build) * swell * (1 + 0.35 * s->gust);
	s->wind = s->dir * u * (30 + 260 * s->storm * (0.7 + 0.3 * sin(s->t * 0.07)) +
		420 * s->gust * s->amount * build);
	update_rows(s);

	// the desktop, cold and frosting over
	cairo_set_source_surface(cr, s->view, 0, 0);
	cairo_paint(cr);
	// the far snow, a fine haze of it, behind the windows
	int far = (int)(FAR_FLAKES * fmin(1, 0.25 + 0.6 * s->storm));
	cairo_new_path(cr);
	for (int i = 0; i < far; i++) {
		struct flake *f = &s->far[i];
		move_flake(s, f, (float)(u * 40), 0.55f, dt);
		if (flake_gone(s, f)) {
			flake_reset(s, f, (float)(u * 40), (float)(s->wind * 0.55), false);
		}
		if (window_at(s, f->x, f->y) >= 0) {
			continue;
		}
		cairo_rectangle(cr, f->x, f->y, f->size, f->size);
	}
	cairo_set_source_rgba(cr, 0.93, 0.95, 1, 0.6);
	cairo_fill(cr);
	// blowing snow far off
	// (at whole pixels: between them every pixel of it would have to be worked out anew)
	double fog_off[2];
	for (int l = 0; l < 2; l++) {
		s->fog_x[l] = fmod(s->fog_x[l] + s->wind * (l ? 1.4 : 0.6) * dt + W, W);
		fog_off[l] = round(s->fog_x[l]);
	}
	double fog0 = fmin(0.9, 0.15 + 0.35 * s->storm * s->whiteout);
	cairo_set_source_surface(cr, s->fog[0], fog_off[0], 0);
	cairo_paint_with_alpha(cr, fog0);
	cairo_set_source_surface(cr, s->fog[0], fog_off[0] - W, 0);
	cairo_paint_with_alpha(cr, fog0);
	// the ice and the snow on the windows and the taskbar
	settle(s, dt);
	grow_icicles(s, dt);
	if (s->t - s->ice_drawn > 0.5) {
		draw_icicles(s);
		s->ice_drawn = s->t;
	}
	cairo_new_path(cr);
	for (int i = 0; i < s->icicle_count; i++) {
		struct icicle *c = &s->icicles[i];
		if (c->len >= 1) {
			cairo_rectangle(cr, floor(c->x - c->w) - 1, floor(c->y) - 1, ceil(c->w * 2) + 3,
				ceil(c->len) + 3);
		}
	}
	cairo_set_source_surface(cr, s->ice, 0, 0);
	cairo_fill(cr);
	icicle_glints(s, cr);
	draw_ledges(s, cr, false);
	draw_glints(s, cr, dt);
	// the snow driving past and settling
	int mid = (int)(MID_FLAKES * fmin(1, 0.2 + 0.55 * s->storm));
	for (int i = 0; i < mid; i++) {
		move_flake(s, &s->mid[i], (float)(u * 95), 1, dt);
	}
	for (int pass = 0; pass < 3; pass++) {
		cairo_new_path(cr);
		for (int i = pass; i < mid; i += 3) {
			struct flake *f = &s->mid[i];
			struct ledge *l;
			float floor = surface_below(s, f->x, f->py, &l);
			if (l && f->y >= floor) {
				deposit(s, l, f->x, (float)(u * u * 1.4), (float)(u * 2));
				flake_reset(s, f, (float)(u * 95), (float)s->wind, false);
				continue;
			}
			if (flake_gone(s, f)) {
				flake_reset(s, f, (float)(u * 95), (float)s->wind, false);
				continue;
			}
			// a round flake, drawn out a little along its way when the wind drives it
			float dx = f->x - f->px, dy = f->y - f->py, len = hypotf(dx, dy);
			float tail = fminf(len * 0.35f, (float)(u * (2.5 + 5 * s->gust))) / fmaxf(len, 0.01f);
			cairo_move_to(cr, f->x - dx * tail, f->y - dy * tail);
			cairo_line_to(cr, f->x + 0.01, f->y);
		}
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_width(cr, fmax(1, u * (2 + pass * 1.3)));
		cairo_set_source_rgba(cr, 0.97, 0.98, 1, 0.95 - pass * 0.12);
		cairo_stroke(cr);
	}
	draw_falling(s, cr, dt);
	// the drift at the bottom, in front of the taskbar
	draw_ledges(s, cr, true);
	// blowing snow close by, and the air turning white in the gusts
	double fog1 = fmin(0.95, (0.08 + 0.55 * s->gust * build) * s->whiteout);
	if (fog1 > 0.02) {
		cairo_set_source_surface(cr, s->fog[1], fog_off[1], H - s->fog_h);
		cairo_paint_with_alpha(cr, fog1);
		cairo_set_source_surface(cr, s->fog[1], fog_off[1] - W, H - s->fog_h);
		cairo_paint_with_alpha(cr, fog1);
	}
	double veil = fmin(0.5, (0.05 * s->storm + 0.16 * s->gust * build) * s->whiteout);
	if (veil > 0.01) {
		cairo_set_source_rgba(cr, 0.86, 0.9, 0.96, veil);
		cairo_paint(cr);
	}
	// big soft flakes right in front, and now and then a crystal drifting by
	int near = (int)(NEAR_FLAKES * fmin(1, 0.3 + 0.5 * s->storm));
	for (int i = 0; i < near; i++) {
		struct near_flake *f = &s->near[i];
		float vx, vy, fall = f->size * 9;
		flake_velocity(s, f->x, f->y, f->phase, 1.9f, fall, &vx, &vy);
		f->x += vx * (float)dt;
		f->y += vy * (float)dt;
		f->rot += f->spin * (float)dt;
		double r = f->size;
		if (f->y - r > H || f->x + r < -60 || f->x - r > W + 60) {
			struct flake tmp;
			flake_reset(s, &tmp, fall, vx, false);
			f->x = tmp.x;
			f->y = tmp.y - (float)r;
			continue;
		}
		if (f->crystal >= 0) {
			draw_crystal(s, cr, f->crystal, f->x, f->y, r, f->rot, 0.85);
			continue;
		}
		int k = 0;
		while (k < BOKEH_SIZES - 1 && s->bokeh_r[k] < r) {
			k++;
		}
		int half = cairo_image_surface_get_width(s->bokeh[k]) / 2;
		cairo_set_source_surface(cr, s->bokeh[k], round(f->x) - half, round(f->y) - half);
		cairo_paint_with_alpha(cr, 0.6);
	}
	// flakes caught on the screen itself, melting into drops
	if (saver_random() < dt * 0.3 * s->storm) {
		for (int i = 0; i < LENS_MAX; i++) {
			if (!s->lens[i].alive) {
				s->lens[i] = (struct lens){ (float)(saver_random() * W), (float)(saver_random() * H),
					(float)(u * saver_between(14, 36)), (float)saver_between(0, 6.3), 0,
					(float)saver_between(5, 10), (int)(saver_random() * CRYSTALS), true };
				break;
			}
		}
	}
	for (int i = 0; i < LENS_MAX; i++) {
		struct lens *l = &s->lens[i];
		if (!l->alive) {
			continue;
		}
		l->age += (float)dt;
		double k = l->age / l->life;
		if (k >= 1) {
			l->alive = false;
			continue;
		}
		double crystal = k < 0.5 ? 1 : 1 - smooth(0.5f, 0.8f, (float)k);
		if (crystal > 0.01) {
			draw_crystal(s, cr, l->crystal, l->x, l->y, l->size * (0.8 + 0.2 * crystal), l->rot,
				0.95 * crystal * fmin(1, l->age * 8));
		}
		double drop = smooth(0.45f, 0.75f, (float)k) * (1 - smooth(0.85f, 1, (float)k));
		if (drop > 0.01) {
			double r = l->size * 0.3, y = l->y + smooth(0.7f, 1, (float)k) * u * 18;
			cairo_pattern_t *p = cairo_pattern_create_radial(l->x - r * 0.3, y - r * 0.3, r * 0.1,
				l->x, y, r);
			cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.5 * drop);
			cairo_pattern_add_color_stop_rgba(p, 0.7, 0.75, 0.82, 0.95, 0.12 * drop);
			cairo_pattern_add_color_stop_rgba(p, 0.92, 0.1, 0.14, 0.25, 0.35 * drop);
			cairo_pattern_add_color_stop_rgba(p, 1, 0.9, 0.95, 1, 0.3 * drop);
			cairo_set_source(cr, p);
			cairo_arc(cr, l->x, y, r, 0, 2 * M_PI);
			cairo_fill(cr);
			cairo_pattern_destroy(p);
		}
	}
	// the frost on the screen itself
	if (atomic_load(&s->ready) && s->tiles && s->t > s->glass_start) {
		cairo_new_path(cr);
		for (int ty = 0; ty < s->tiles_h; ty++) {
			for (int tx = 0; tx < s->tiles_w; tx++) {
				if (s->tiles[ty * s->tiles_w + tx]) {
					cairo_rectangle(cr, tx * GLASS_TILE, ty * GLASS_TILE, GLASS_TILE, GLASS_TILE);
				}
			}
		}
		cairo_set_source_surface(cr, s->glass, 0, 0);
		cairo_fill(cr);
	}
}

static void blizzard_destroy(void *state) {
	struct blizzard *s = state;
	finish_preparing(s);
	cairo_surface_destroy(s->orig);
	cairo_surface_destroy(s->winter);
	cairo_surface_destroy(s->view);
	cairo_surface_destroy(s->glass);
	cairo_surface_destroy(s->ice);
	cairo_surface_destroy(s->fog[0]);
	cairo_surface_destroy(s->fog[1]);
	for (int k = 0; k < BOKEH_SIZES; k++) {
		cairo_surface_destroy(s->bokeh[k]);
	}
	for (int k = 0; k < CRYSTALS; k++) {
		cairo_surface_destroy(s->crystal[k]);
	}
	for (int k = 0; k < s->ledge_count; k++) {
		free(s->ledges[k].h);
		free(s->ledges[k].cap);
		free(s->ledges[k].lump);
	}
	free(s->cols);
	free(s->tiles);
	free(s->blurred);
	if (atomic_load(&s->ready)) {
		frost_free(&s->frost);
		frost_free(&s->glass_frost);
	}
	free(s);
}

void saver_blizzard_stats(void *state, struct blizzard_stats *out) {
	struct blizzard *s = state;
	finish_preparing(s);
	*out = (struct blizzard_stats){ .ledges = s->ledge_count, .icicles = s->icicle_count,
		.fits = true, .storm = s->storm };
	for (int k = 0; k < s->ledge_count; k++) {
		struct ledge *l = &s->ledges[k];
		for (int c = 0; c < l->n; c++) {
			if (l->owner == GROUND) {
				out->ground += l->h[c];
			} else if (l->owner == TASKBAR) {
				out->taskbar += l->h[c];
			} else {
				out->windows += l->h[c];
				out->window_columns++;
				if (l->h[c] > out->deepest) {
					out->deepest = l->h[c];
					out->deepest_x = l->x0 + c + 0.5;
					out->deepest_y = l->y - l->h[c];
				}
			}
			if (l->h[c] < -0.01f || l->h[c] > l->cap[c] + 0.01f) {
				out->fits = false;
			}
			// snow lies only where the surface shows
			if (l->owner >= 0 && covered(s, l->owner, l->x0 + c + 0.5, l->y + 0.5)) {
				out->fits = false;
			}
		}
	}
	for (int i = 0; i < s->icicle_count; i++) {
		out->icicle_length += s->icicles[i].len;
	}
	if (s->frost_on) {
		long frosted = 0, inside = 0, outside = 0;
		for (int y = 0; y < s->height; y++) {
			for (int x = 0; x < s->width; x++) {
				int i = y * s->width + x;
				bool on = s->frost.dens[i] && s->frost.freeze[i] <= s->t;
				frosted += on;
				bool in = window_at(s, x + 0.5f, y + 0.5f) >= 0;
				for (int b = 0; b < s->bar_count && !in; b++) {
					struct saver_window *r = &s->bars[b];
					in = x + 0.5 >= r->x && x + 0.5 < r->x + r->w && y + 0.5 >= r->y && y + 0.5 < r->y + r->h;
				}
				inside += on && in;
				outside += on && !in;
			}
		}
		out->frost = (double)frosted / ((double)s->width * s->height);
		out->frost_outside = outside;
		(void)inside;
	}
}

const struct saver saver_blizzard = {
	.name = "blizzard",
	.title = "Blizzard",
	.description = "Snow piling up on the windows and the taskbar, icicles, frost over the glass",
	.wants_desktop = true,
	.create = blizzard_create,
	.draw = blizzard_draw,
	.destroy = blizzard_destroy,
};
