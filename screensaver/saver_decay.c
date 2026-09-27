#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saver_noise.h"
#include "saver_util.h"

/*
 * Decay: the desktop eaten by time. Days flicker past faster and faster and
 * the years count up in a corner. Everything yellows and fades like old
 * paper, stains creep over it and grime settles; cracks run through the
 * windows and the taskbar and branch, widen where a piece is working loose,
 * and dust trickles out of them. Then the windows fall apart, the one on top
 * first: piece after piece comes off, tumbles down, knocks against the
 * windows still standing and against the other pieces, shatters when it
 * lands hard and comes to rest on the rubble at the bottom. Where a piece
 * was, what lay behind it shows, itself ageing. Last the taskbar crumbles,
 * the rubble crumbles into dust, and at the end there is nothing but dust:
 * the screen in its colour, dunes of it at the bottom, and motes of it
 * drifting through shafts of light.
 *
 * The windows and the taskbar are the real ones: the picture of the screen
 * from before the saver, and where they are from tileWin; what was behind
 * them is the wallpaper tileWin reports, or the colour of the desktop around.
 */

#define WINDOWS_MAX 48
#define OWNERS_MAX (WINDOWS_MAX + 4)
#define PIECES_MAX 1600
#define CRACKS_MAX 8000
#define POLY_MAX 12
#define BODIES_MAX 200
#define GRAINS_MAX 900
#define MOTES_MAX 500
#define ROW_STEP 16      // the ageing is brought up to date on every 16th row a frame
#define SCENE_EVERY 6    // frames to build the slow parts in (in 4 of them a part each)

struct piece {
	float px[POLY_MAX], py[POLY_MAX];
	int n;
	float cx, cy, area;
	int owner;           // the window, or count + the taskbar
	float fall_at;       // seconds: when it comes off
	bool fallen;
};

/* A crack: a straight stretch of a cut between pieces, showing from at on. */
struct crack {
	float x0, y0, x1, y1;
	float at;
	int owner;
	uint32_t seed;
};

/* A piece off the wall, falling. */
struct body {
	cairo_surface_t *img;
	float ox, oy;        // its middle in the picture
	float lx[POLY_MAX], ly[POLY_MAX]; // its outline about the middle
	int n;
	float x, y, vx, vy, angle, spin, radius, area;
	float still;         // seconds it has lain still
	int bounces;
	bool alive;
};

struct grain {
	float x, y, vx, vy, life, age, size;
	bool falls;          // dust that settles on the heap, not a puff
};

struct mote {
	float x, y, phase, size;
};

enum { INTACT, CRUMBLING, GONE };

struct decay {
	int width, height;
	double u, t, span;   // span: seconds until all is dust
	int frame;
	int part;                 // of the slow parts to build next
	cairo_surface_t *orig, *bare; // the desktop, and without the windows and taskbars
	cairo_surface_t *view;        // aged, with the pieces that are gone gone
	cairo_surface_t *scene, *next; // the slow parts, and the next of them, built a part a frame
	cairo_surface_t *rubble;      // the pieces at rest
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	uint32_t flat[OWNERS_MAX];    // the colour of each, for what of it was hidden
	short *owner;                 // per pixel: the window or taskbar that shows, -1 none
	uint8_t *src;                 // per pixel: 0 the picture, 1 behind, 2 + k flat of k
	uint8_t *age_n, *stain_n, *grain_n; // per pixel noise: early or late, stains, grain
	struct piece *pieces;
	int piece_count;
	int first[OWNERS_MAX], last[OWNERS_MAX]; // the pieces of each
	float start[OWNERS_MAX], end[OWNERS_MAX]; // when each falls apart, seconds
	int state[OWNERS_MAX];
	struct crack *cracks;
	int crack_count;
	struct body bodies[BODIES_MAX];
	struct grain grains[GRAINS_MAX];
	int grain_next;
	struct mote motes[MOTES_MAX];
	float *heap, *dust, *top;     // per column: the rubble, the dust on it; scratch
	float *dune;                  // per column: how high the dust drifts over it, 0 to 1
	int fallen, resting, shattered;
	double light;                 // of the days passing, 0 night to 1 day
	double day;                   // the phase of the days
	bool years;
	PangoLayout *layout;
	float rubble_top;             // the highest the rubble goes
};

/* The progress to dust: 0 at the start, 1 all dust (it goes on a little). */
static double progress(struct decay *s) {
	return s->t / s->span;
}

/* ---------- polygons ---------- */

static float poly_area(const float *x, const float *y, int n, float *cx, float *cy) {
	double a = 0, sx = 0, sy = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		double c = (double)x[i] * y[j] - (double)x[j] * y[i];
		a += c;
		sx += (x[i] + x[j]) * c;
		sy += (y[i] + y[j]) * c;
	}
	a *= 0.5;
	if (fabs(a) < 1e-6) {
		*cx = x[0];
		*cy = y[0];
		return 0;
	}
	*cx = (float)(sx / (6 * a));
	*cy = (float)(sy / (6 * a));
	return (float)fabs(a);
}

/* Whether a point is inside a convex polygon, of either winding. */
static bool inside(const float *x, const float *y, int n, float px, float py) {
	int sign = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		float c = (x[j] - x[i]) * (py - y[i]) - (y[j] - y[i]) * (px - x[i]);
		int sg = c > 0 ? 1 : c < 0 ? -1 : 0;
		if (sg && sign && sg != sign) {
			return false;
		}
		if (sg) {
			sign = sg;
		}
	}
	return true;
}

/*
 * The part of a convex polygon on one side of the line through (cx, cy) with
 * normal (nx, ny); the points where the line cuts it into cut, when given.
 */
static int clip_side(const float *x, const float *y, int n, float cx, float cy, float nx,
		float ny, float side, float *ox, float *oy, float cut[2][2], int *cuts) {
	int m = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		float di = ((x[i] - cx) * nx + (y[i] - cy) * ny) * side;
		float dj = ((x[j] - cx) * nx + (y[j] - cy) * ny) * side;
		if (di >= 0 && m < POLY_MAX) {
			ox[m] = x[i];
			oy[m++] = y[i];
		}
		if ((di >= 0) != (dj >= 0) && m < POLY_MAX) {
			float k = di / (di - dj);
			ox[m] = x[i] + (x[j] - x[i]) * k;
			oy[m] = y[i] + (y[j] - y[i]) * k;
			if (cut && *cuts < 2) {
				cut[*cuts][0] = ox[m];
				cut[*cuts][1] = oy[m];
				(*cuts)++;
			}
			m++;
		}
	}
	return m;
}

/* ---------- the pieces and the cracks between them ---------- */

static void add_piece(struct decay *s, const float *x, const float *y, int n, int owner) {
	if (s->piece_count >= PIECES_MAX || n < 3) {
		return;
	}
	struct piece *p = &s->pieces[s->piece_count++];
	memcpy(p->px, x, sizeof(float) * n);
	memcpy(p->py, y, sizeof(float) * n);
	p->n = n;
	p->owner = owner;
	p->area = poly_area(x, y, n, &p->cx, &p->cy);
	p->fallen = false;
}

static void add_crack(struct decay *s, float x0, float y0, float x1, float y1, float at, int owner) {
	// in short stretches, each shown only where the one it runs through still shows
	double len = hypot(x1 - x0, y1 - y0), most = fmax(4, s->u * 18);
	int parts = (int)ceil(len / most);
	for (int k = 0; k < parts && s->crack_count < CRACKS_MAX; k++) {
		float a = (float)k / parts, b = (float)(k + 1) / parts;
		s->cracks[s->crack_count++] = (struct crack){ x0 + (x1 - x0) * a, y0 + (y1 - y0) * a,
			x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, at + (float)(a * s->span * 0.02), owner,
			(uint32_t)(saver_random() * 1e9) };
	}
}

/* Splits a polygon along straight cracks until the pieces are about target in area. */
static void split(struct decay *s, const float *x, const float *y, int n, int owner, int depth,
		float crack_at, float target) {
	float cx, cy;
	float area = poly_area(x, y, n, &cx, &cy);
	if (area < target * saver_between(0.6, 1.5) || depth > 16 || n < 3) {
		add_piece(s, x, y, n, owner);
		return;
	}
	float x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0];
	for (int i = 1; i < n; i++) {
		x0 = fminf(x0, x[i]);
		x1 = fmaxf(x1, x[i]);
		y0 = fminf(y0, y[i]);
		y1 = fmaxf(y1, y[i]);
	}
	// across the longer way, not quite straight, not quite through the middle
	double angle = (x1 - x0 > y1 - y0 ? 0 : M_PI / 2) + saver_between(-0.55, 0.55);
	float nx = (float)cos(angle), ny = (float)sin(angle);
	float px = cx + (float)saver_between(-0.2, 0.2) * (x1 - x0);
	float py = cy + (float)saver_between(-0.2, 0.2) * (y1 - y0);
	float ax[POLY_MAX], ay[POLY_MAX], bx[POLY_MAX], by[POLY_MAX], cut[2][2];
	int cuts = 0;
	int na = clip_side(x, y, n, px, py, nx, ny, 1, ax, ay, cut, &cuts);
	int nb = clip_side(x, y, n, px, py, nx, ny, -1, bx, by, NULL, NULL);
	if (na < 3 || nb < 3 || cuts < 2) {
		add_piece(s, x, y, n, owner);
		return;
	}
	add_crack(s, cut[0][0], cut[0][1], cut[1][0], cut[1][1],
		crack_at + (float)(depth * s->span * 0.018 + saver_between(0, s->span * 0.025)), owner);
	split(s, ax, ay, na, owner, depth + 1, crack_at, target);
	split(s, bx, by, nb, owner, depth + 1, crack_at, target);
}

/* When each window and taskbar falls apart: the top window first, one under another after it. */
static void schedule(struct decay *s) {
	double span = s->span;
	for (int i = s->count - 1; i >= 0; i--) {
		struct saver_window *w = &s->wins[i];
		double at = span * (0.42 + (s->count - 1 - i) * 0.03 + saver_between(0, 0.03));
		for (int j = i + 1; j < s->count; j++) {
			struct saver_window *o = &s->wins[j];
			bool overlap = o->x < w->x + w->w && w->x < o->x + o->w && o->y < w->y + w->h &&
				w->y < o->y + o->h;
			if (overlap) {
				at = fmax(at, s->end[j] + span * 0.01);
			}
		}
		s->start[i] = (float)at;
		s->end[i] = (float)(at + span * 0.14);
	}
	// all windows down before the taskbar goes: squeezed into time if need be
	double latest = span * 0.42;
	for (int i = 0; i < s->count; i++) {
		latest = fmax(latest, s->end[i]);
	}
	if (latest > span * 0.88) {
		double k = (span * 0.88 - span * 0.42) / (latest - span * 0.42);
		for (int i = 0; i < s->count; i++) {
			s->start[i] = (float)(span * 0.42 + (s->start[i] - span * 0.42) * k);
			s->end[i] = (float)(span * 0.42 + (s->end[i] - span * 0.42) * k);
		}
	}
	for (int k = 0; k < s->bar_count; k++) {
		s->start[s->count + k] = (float)(span * saver_between(0.88, 0.9));
		s->end[s->count + k] = (float)(span * 0.97);
	}
}

static void make_pieces(struct decay *s) {
	double u = s->u;
	s->pieces = calloc(PIECES_MAX, sizeof(struct piece));
	s->cracks = calloc(CRACKS_MAX, sizeof(struct crack));
	schedule(s);
	int owners = s->count + s->bar_count;
	for (int o = 0; o < owners; o++) {
		struct saver_window *w = o < s->count ? &s->wins[o] : &s->bars[o - s->count];
		float x0 = (float)fmax(0, w->x), y0 = (float)fmax(0, w->y);
		float x1 = (float)fmin(s->width, w->x + w->w), y1 = (float)fmin(s->height, w->y + w->h);
		s->first[o] = s->last[o] = s->piece_count;
		if (x1 - x0 < 2 || y1 - y0 < 2) {
			s->state[o] = GONE;
			continue;
		}
		float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 };
		// pieces a hand across, but no more than a hundred to a window
		double target = fmax((u * 72) * (u * 72), (x1 - x0) * (y1 - y0) / 100.0);
		float crack_at = (float)(s->start[o] - s->span * saver_between(0.2, 0.26));
		split(s, px, py, 4, o, 0, fmaxf((float)(s->span * 0.08), crack_at), (float)target);
		s->last[o] = s->piece_count;
		// the lower pieces come off first, then those above, with a good deal of chance
		float h = y1 - y0, dur = s->end[o] - s->start[o];
		for (int k = s->first[o]; k < s->last[o]; k++) {
			struct piece *p = &s->pieces[k];
			float from_bottom = (y1 - p->cy) / h;
			p->fall_at = s->start[o] + dur * fminf(1, fmaxf(0, from_bottom * 0.6f +
				(float)saver_random() * 0.4f));
		}
	}
}

/* ---------- pixels ---------- */

/* What shows at a pixel, before ageing. */
static inline uint32_t source_px(struct decay *s, int i, const uint32_t *orig, const uint32_t *bare) {
	uint8_t src = s->src[i];
	return src == 0 ? orig[i] : src == 1 ? bare[i] : s->flat[src - 2];
}

/* A pixel aged: yellowed and faded, stained, grimy, and at last dust. */
static inline uint32_t aged(struct decay *s, int i, uint32_t p, float P) {
	float a = P * 1.08f + (s->age_n[i] / 255.0f - 0.5f) * 0.24f;
	if (a <= 0.01f) {
		return p;
	}
	int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
	int lum = (r * 77 + g * 150 + b * 29) >> 8;
	float k1 = smooth(0.02f, 0.45f, a), k2 = smooth(0.28f, 0.8f, a), k3 = smooth(0.7f, 1.06f, a);
	// yellowed like old paper, and faded
	uint32_t sepia = 0xff000000u | (uint32_t)(lum * 1.02f + 22 > 255 ? 255 : lum * 1.02f + 22) << 16 |
		(uint32_t)(lum * 0.9f + 10) << 8 | (uint32_t)(lum * 0.68f + 4);
	p = mix_px(p, sepia, (int)(k1 * 0.85f * 256));
	p = mix_px(p, 0xffc4b28e, (int)(k1 * 0.28f * 256));
	// stains creeping out, with darker rims, and grime
	float st = s->stain_n[i] / 255.0f, thr = 1 - k2 * 0.5f;
	if (st > thr) {
		p = mix_px(p, 0xff5a4128, (int)(fminf(1, (st - thr) * 12) * 0.5f * 256));
	} else if (st > thr - 0.025f && k2 > 0) {
		p = mix_px(p, 0xff3e2b1a, (int)(0.3f * k2 * 256));
	}
	uint8_t gr = s->grain_n[i];
	if (gr < 40 * k2) {
		p = mix_px(p, 0xff2c2218, 90);
	}
	// and dust
	if (k3 > 0) {
		uint32_t dust = mix_px(0xff9c907c, 0xff6c6254, gr);
		p = mix_px(p, dust, (int)(k3 * 256));
	}
	return p;
}

static void update_span(struct decay *s, int y, int x0, int x1) {
	uint32_t *orig = (uint32_t *)cairo_image_surface_get_data(s->orig);
	uint32_t *bare = (uint32_t *)cairo_image_surface_get_data(s->bare);
	uint32_t *view = (uint32_t *)cairo_image_surface_get_data(s->view);
	int W = s->width;
	float P = (float)progress(s);
	for (int x = x0; x < x1; x++) {
		int i = y * W + x;
		view[i] = aged(s, i, source_px(s, i, orig, bare), P);
	}
}

/* Every ROW_STEP-th row brought up to date: ageing is slow. */
static void update_rows(struct decay *s) {
	cairo_surface_flush(s->view);
	for (int y = s->frame % ROW_STEP; y < s->height; y += ROW_STEP) {
		update_span(s, y, 0, s->width);
	}
	cairo_surface_mark_dirty(s->view);
}

/* ---------- dust ---------- */

static struct grain *new_grain(struct decay *s) {
	struct grain *g = &s->grains[s->grain_next];
	s->grain_next = (s->grain_next + 1) % GRAINS_MAX;
	return g;
}

static void puff(struct decay *s, float x, float y, int n, float power) {
	double u = s->u;
	for (int k = 0; k < n; k++) {
		double a = saver_between(-M_PI, 0), v = saver_between(10, 90) * u * power;
		*new_grain(s) = (struct grain){ x + (float)saver_between(-4, 4) * (float)u, y,
			(float)(cos(a) * v), (float)(sin(a) * v), (float)saver_between(0.8, 2.2), 0,
			(float)(u * saver_between(1.2, 3.5)), false };
	}
}

/* The ground at a column: the rubble or the dust on it, whichever is higher. */
static float ground(struct decay *s, float x) {
	int c = (int)x;
	c = c < 0 ? 0 : c >= s->width ? s->width - 1 : c;
	return s->height - fmaxf(s->heap[c], s->dust[c]);
}

/* ---------- pieces coming off ---------- */

/* Takes a piece off the wall: its picture, what shows behind it, and it falls. */
static void come_off(struct decay *s, struct piece *p) {
	p->fallen = true;
	s->fallen++;
	int o = p->owner, W = s->width;
	float x0 = p->px[0], x1 = p->px[0], y0 = p->py[0], y1 = p->py[0];
	for (int i = 1; i < p->n; i++) {
		x0 = fminf(x0, p->px[i]);
		x1 = fmaxf(x1, p->px[i]);
		y0 = fminf(y0, p->py[i]);
		y1 = fmaxf(y1, p->py[i]);
	}
	int bx0 = (int)fmaxf(0, floorf(x0)), by0 = (int)fmaxf(0, floorf(y0));
	int bx1 = (int)fminf(W, ceilf(x1)), by1 = (int)fminf(s->height, ceilf(y1));
	if (bx1 <= bx0 || by1 <= by0) {
		return;
	}
	int iw = bx1 - bx0, ih = by1 - by0;
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
	uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(img);
	int ds = cairo_image_surface_get_stride(img) / 4;
	cairo_surface_flush(s->view);
	const uint32_t *view = (const uint32_t *)cairo_image_surface_get_data(s->view);
	long pixels = 0;
	for (int y = by0; y < by1; y++) {
		for (int x = bx0; x < bx1; x++) {
			int i = y * W + x;
			if (s->owner[i] != o || !inside(p->px, p->py, p->n, x + 0.5f, y + 0.5f)) {
				continue;
			}
			dst[(y - by0) * ds + x - bx0] = view[i] | 0xff000000u;
			pixels++;
			// what was under it: the window below, intact still, or behind them all
			int below = -1;
			int from = o < s->count ? o - 1 : s->count - 1;
			for (int j = from; j >= 0; j--) {
				struct saver_window *w = &s->wins[j];
				if (s->state[j] != GONE && x + 0.5 >= w->x && x + 0.5 < w->x + w->w &&
						y + 0.5 >= w->y && y + 0.5 < w->y + w->h) {
					below = j;
					break;
				}
			}
			s->owner[i] = (short)below;
			s->src[i] = below >= 0 ? (uint8_t)(2 + below) : 1;
		}
	}
	cairo_surface_mark_dirty(img);
	for (int y = by0; y < by1; y++) {
		update_span(s, y, bx0, bx1);
	}
	cairo_surface_mark_dirty(s->view);
	if (pixels < 4) {
		cairo_surface_destroy(img);
		return;
	}
	// its broken edge, dark
	cairo_t *cr = cairo_create(img);
	cairo_new_path(cr);
	for (int i = 0; i < p->n; i++) {
		cairo_line_to(cr, p->px[i] - bx0, p->py[i] - by0);
	}
	cairo_close_path(cr);
	cairo_clip_preserve(cr);
	cairo_set_line_width(cr, fmax(1.5, s->u * 2.5));
	cairo_set_source_rgba(cr, 0.12, 0.09, 0.06, 0.55);
	cairo_stroke(cr);
	cairo_destroy(cr);
	for (int k = 0; k < BODIES_MAX; k++) {
		struct body *b = &s->bodies[k];
		if (b->alive) {
			continue;
		}
		*b = (struct body){ .img = img, .ox = p->cx - bx0, .oy = p->cy - by0, .n = p->n,
			.x = p->cx, .y = p->cy, .vx = (float)(saver_between(-25, 25) * s->u),
			.vy = (float)(saver_between(-10, 25) * s->u), .spin = (float)saver_between(-1.2, 1.2),
			.area = p->area, .alive = true };
		float r = 0;
		for (int i = 0; i < p->n; i++) {
			b->lx[i] = p->px[i] - p->cx;
			b->ly[i] = p->py[i] - p->cy;
			r = fmaxf(r, hypotf(b->lx[i], b->ly[i]));
		}
		b->radius = r;
		puff(s, p->cx, p->cy + (y1 - p->cy), 6, 0.5f);
		return;
	}
	cairo_surface_destroy(img); // no room: it crumbles to dust on the spot
	puff(s, p->cx, p->cy, 20, 0.8f);
}

/* A piece at rest: into the rubble, and the rubble higher under it. */
static void settle(struct decay *s, struct body *b) {
	cairo_t *cr = cairo_create(s->rubble);
	cairo_translate(cr, b->x, b->y);
	cairo_rotate(cr, b->angle);
	cairo_set_source_surface(cr, b->img, -b->ox, -b->oy);
	cairo_paint(cr);
	cairo_destroy(cr);
	// the heap grows under it by about its thickness: pieces lie flat and on each other
	float ca = cosf(b->angle), sa = sinf(b->angle), x0 = b->x, x1 = b->x;
	for (int i = 0; i < b->n; i++) {
		float x = b->x + b->lx[i] * ca - b->ly[i] * sa, y = b->y + b->lx[i] * sa + b->ly[i] * ca;
		x0 = fminf(x0, x);
		x1 = fmaxf(x1, x);
		s->rubble_top = fminf(s->rubble_top, y);
	}
	float thick = b->area / fmaxf(1, x1 - x0) * 0.25f;
	for (int c = (int)fmaxf(0, x0); c < (int)fminf(s->width, x1); c++) {
		s->heap[c] += thick;
	}
	cairo_surface_destroy(b->img);
	b->img = NULL;
	b->alive = false;
	s->resting++;
}

/* A piece that landed hard breaks in two or three; bits too small to fall become dust. */
static void shatter(struct decay *s, struct body *b) {
	float lx[POLY_MAX], ly[POLY_MAX];
	int n = b->n;
	memcpy(lx, b->lx, sizeof(lx));
	memcpy(ly, b->ly, sizeof(ly));
	cairo_surface_t *img = b->img;
	struct body parent = *b;
	b->alive = false;
	b->img = NULL;
	s->shattered++;
	double a = saver_between(0, M_PI);
	float nx = (float)cos(a), ny = (float)sin(a);
	float px = (float)saver_between(-0.2, 0.2) * parent.radius, py = (float)saver_between(-0.2, 0.2) * parent.radius;
	for (int side = -1; side <= 1; side += 2) {
		float fx[POLY_MAX], fy[POLY_MAX];
		int m = clip_side(lx, ly, n, px, py, nx, ny, (float)side, fx, fy, NULL, NULL);
		float cx, cy, area = m >= 3 ? poly_area(fx, fy, m, &cx, &cy) : 0;
		if (area < (s->u * 9) * (s->u * 9)) {
			if (area > 0) {
				puff(s, parent.x, parent.y, 10, 0.7f);
			}
			continue;
		}
		struct body *f = NULL;
		for (int k = 0; k < BODIES_MAX && !f; k++) {
			f = s->bodies[k].alive ? NULL : &s->bodies[k];
		}
		if (!f) {
			puff(s, parent.x, parent.y, 10, 0.7f);
			continue;
		}
		// its picture: the part of the whole one inside it
		float x0 = fx[0], x1 = fx[0], y0 = fy[0], y1 = fy[0];
		for (int i = 1; i < m; i++) {
			x0 = fminf(x0, fx[i]);
			x1 = fmaxf(x1, fx[i]);
			y0 = fminf(y0, fy[i]);
			y1 = fmaxf(y1, fy[i]);
		}
		int iw = (int)ceilf(x1 - x0) + 2, ih = (int)ceilf(y1 - y0) + 2;
		cairo_surface_t *part = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
		cairo_t *cr = cairo_create(part);
		cairo_translate(cr, -x0 + 1, -y0 + 1);
		cairo_new_path(cr);
		for (int i = 0; i < m; i++) {
			cairo_line_to(cr, fx[i], fy[i]);
		}
		cairo_close_path(cr);
		cairo_clip(cr);
		cairo_set_source_surface(cr, img, -parent.ox, -parent.oy);
		cairo_paint(cr);
		cairo_destroy(cr);
		float ca = cosf(parent.angle), sa = sinf(parent.angle);
		float r = 0;
		*f = (struct body){ .img = part, .ox = cx - x0 + 1, .oy = cy - y0 + 1, .n = m,
			.x = parent.x + cx * ca - cy * sa, .y = parent.y + cx * sa + cy * ca,
			.angle = parent.angle, .area = area, .bounces = parent.bounces, .alive = true };
		for (int i = 0; i < m; i++) {
			f->lx[i] = fx[i] - cx;
			f->ly[i] = fy[i] - cy;
			r = fmaxf(r, hypotf(f->lx[i], f->ly[i]));
		}
		f->radius = r;
		// flung apart, up a little
		float out = (float)(saver_between(40, 140) * s->u);
		float dx = f->x - parent.x, dy = f->y - parent.y, d = fmaxf(0.01f, hypotf(dx, dy));
		f->vx = parent.vx * 0.4f + dx / d * out;
		f->vy = -fabsf(parent.vy) * 0.25f - (float)(saver_between(20, 80) * s->u);
		f->spin = (float)saver_between(-4, 4);
	}
	cairo_surface_destroy(img);
	puff(s, parent.x, parent.y, 14, 1);
}

/* One piece falling: pulled down, off the edges, the ground, the windows still up. */
static void body_step(struct decay *s, struct body *b, double dt) {
	double u = s->u;
	b->vy += (float)(u * 900 * dt);
	b->vx -= b->vx * (float)(0.1 * dt);
	float px = b->x, py = b->y;
	b->x += b->vx * (float)dt;
	b->y += b->vy * (float)dt;
	b->angle += b->spin * (float)dt;
	// the sides of the screen are walls
	if (b->x - b->radius * 0.5f < 0) {
		b->x = b->radius * 0.5f;
		b->vx = fabsf(b->vx) * 0.4f;
	}
	if (b->x + b->radius * 0.5f > s->width) {
		b->x = s->width - b->radius * 0.5f;
		b->vx = -fabsf(b->vx) * 0.4f;
	}
	// the deepest corner under whatever it meets
	float ca = cosf(b->angle), sa = sinf(b->angle);
	float deepest = 0, hit_x = b->x;
	int on_window = -1;
	for (int i = 0; i < b->n; i++) {
		float x = b->x + b->lx[i] * ca - b->ly[i] * sa, y = b->y + b->lx[i] * sa + b->ly[i] * ca;
		float floor = ground(s, x);
		int window = -1;
		// the top of a window still standing, when it came down onto it from above
		for (int j = 0; j < s->count; j++) {
			struct saver_window *w = &s->wins[j];
			if (s->state[j] == INTACT && x >= w->x && x < w->x + w->w && py + (y - b->y) <= w->y + 1 &&
					y >= w->y && w->y < floor) {
				floor = (float)w->y;
				window = j;
			}
		}
		if (y - floor > deepest) {
			deepest = y - floor;
			hit_x = x;
			on_window = window;
		}
	}
	(void)px;
	bool touching = deepest > 0;
	if (touching) {
		b->y -= deepest;
		float impact = b->vy;
		if (impact > u * 380 && b->area > (u * 22) * (u * 22) && b->bounces < 2) {
			b->bounces++;
			shatter(s, b);
			return;
		}
		if (impact > 0) {
			b->vy = -impact * 0.25f;
			// struck off its middle, it turns
			b->spin += (hit_x - b->x) / fmaxf(1, b->radius) * impact / fmaxf(1, b->radius) * 0.35f;
			if (impact > u * 150) {
				puff(s, hit_x, b->y + b->radius * 0.5f, 4 + (int)(impact / (u * 100)), 0.6f);
			}
		}
		b->vx *= 0.8f;
		b->spin *= 0.85f;
		b->bounces++;
		if (on_window >= 0) {
			// it does not stay on a window: it slides and tips off the nearer end
			struct saver_window *w = &s->wins[on_window];
			b->vx += (b->x < w->x + w->w / 2 ? -1 : 1) * (float)(u * 260 * dt);
			b->spin += (b->x < w->x + w->w / 2 ? -1 : 1) * (float)(1.5 * dt);
		}
	}
	if (touching && on_window < 0 && fabsf(b->vy) < u * 45 && fabsf(b->vx) < u * 35) {
		b->still += (float)dt;
	} else {
		b->still = 0;
	}
	if (b->still > 0.3f || b->y - b->radius > s->height + 50) {
		settle(s, b);
	}
}

/* Pieces in the air knock into each other. */
static void collide(struct decay *s) {
	for (int i = 0; i < BODIES_MAX; i++) {
		struct body *a = &s->bodies[i];
		if (!a->alive) {
			continue;
		}
		for (int j = i + 1; j < BODIES_MAX; j++) {
			struct body *b = &s->bodies[j];
			if (!b->alive) {
				continue;
			}
			float dx = b->x - a->x, dy = b->y - a->y, d = hypotf(dx, dy);
			float r = (a->radius + b->radius) * 0.7f;
			if (d >= r || d < 0.01f) {
				continue;
			}
			float nx = dx / d, ny = dy / d, push = (r - d) * 0.5f;
			a->x -= nx * push;
			a->y -= ny * push;
			b->x += nx * push;
			b->y += ny * push;
			float rel = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
			if (rel < 0) {
				float ma = a->area, mb = b->area, k = -1.3f * rel / (ma + mb);
				a->vx -= nx * k * mb;
				a->vy -= ny * k * mb;
				b->vx += nx * k * ma;
				b->vy += ny * k * ma;
				a->spin += (float)saver_between(-1, 1) * fabsf(rel) / fmaxf(1, a->radius) * 0.3f;
				b->spin += (float)saver_between(-1, 1) * fabsf(rel) / fmaxf(1, b->radius) * 0.3f;
			}
		}
	}
}

/* ---------- the slow parts ---------- */

static void draw_cracks(struct decay *s, cairo_t *cr) {
	double u = s->u, t = s->t;
	int W = s->width, H = s->height;
	cairo_new_path(cr);
	for (int k = 0; k < s->crack_count; k++) {
		struct crack *c = &s->cracks[k];
		if (c->at > t) {
			continue;
		}
		// only where the one it runs through still shows, on either side of it
		float mx = (c->x0 + c->x1) / 2, my = (c->y0 + c->y1) / 2;
		float len = hypotf(c->x1 - c->x0, c->y1 - c->y0);
		if (len < 0.5f) {
			continue;
		}
		float nx = -(c->y1 - c->y0) / len, ny = (c->x1 - c->x0) / len;
		bool shows = false;
		for (int side = -1; side <= 1 && !shows; side += 2) {
			int x = (int)(mx + nx * 2 * side), y = (int)(my + ny * 2 * side);
			shows = x >= 0 && y >= 0 && x < W && y < H && s->owner[y * W + x] == c->owner;
		}
		if (!shows) {
			continue;
		}
		// growing along, jagged
		float f = fminf(1, (float)(t - c->at) / (float)(s->span * 0.03));
		int steps = (int)fmaxf(2, len / (float)(u * 4));
		cairo_move_to(cr, c->x0, c->y0);
		for (int i = 1; i <= steps; i++) {
			float k2 = (float)i / steps * f;
			float j = i == steps && f >= 1 ? 0 :
				((hash2((int)c->seed, i, 5) & 255) / 255.0f - 0.5f) * (float)(u * 2.2);
			cairo_line_to(cr, c->x0 + (c->x1 - c->x0) * k2 + nx * j, c->y0 + (c->y1 - c->y0) * k2 + ny * j);
		}
	}
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_save(cr);
	cairo_translate(cr, u * 0.7, u * 0.7);
	cairo_set_line_width(cr, fmax(0.6, u * 0.8));
	cairo_set_source_rgba(cr, 1, 0.95, 0.82, 0.22); // the lit lip of the crack
	cairo_stroke_preserve(cr);
	cairo_restore(cr);
	cairo_set_line_width(cr, fmax(0.8, u * 1.3));
	cairo_set_source_rgba(cr, 0.1, 0.07, 0.04, 0.85);
	cairo_stroke(cr);
	// gaps opening where a piece works loose
	cairo_new_path(cr);
	for (int k = 0; k < s->piece_count; k++) {
		struct piece *p = &s->pieces[k];
		if (p->fallen || t < p->fall_at - s->span * 0.035) {
			continue;
		}
		cairo_move_to(cr, p->px[0], p->py[0]);
		for (int i = 1; i < p->n; i++) {
			cairo_line_to(cr, p->px[i], p->py[i]);
		}
		cairo_close_path(cr);
	}
	cairo_set_line_width(cr, fmax(1, u * 1.7));
	cairo_set_source_rgba(cr, 0.06, 0.04, 0.02, 0.45);
	cairo_stroke(cr);
}

/* The rubble, and the dust dunes over it. */
static void draw_ground(struct decay *s, cairo_t *cr) {
	double u = s->u, W = s->width, H = s->height;
	if (s->rubble_top < H) {
		double top = fmax(0, s->rubble_top - 2);
		cairo_rectangle(cr, 0, top, W, H - top);
		cairo_set_source_surface(cr, s->rubble, 0, 0);
		cairo_fill(cr);
	}
	float most = 0;
	for (int x = 0; x < s->width; x++) {
		most = fmaxf(most, s->dust[x]);
	}
	if (most < 0.5f) {
		return;
	}
	cairo_new_path(cr);
	cairo_move_to(cr, 0, H);
	for (int x = 0; x < s->width; x += 3) {
		cairo_line_to(cr, x, H - s->dust[x]);
	}
	cairo_line_to(cr, W, H - s->dust[s->width - 1]);
	cairo_line_to(cr, W, H);
	cairo_close_path(cr);
	cairo_pattern_t *p = cairo_pattern_create_linear(0, H - most, 0, H);
	cairo_pattern_add_color_stop_rgb(p, 0, 0.7, 0.64, 0.55);
	cairo_pattern_add_color_stop_rgb(p, 0.4, 0.58, 0.52, 0.44);
	cairo_pattern_add_color_stop_rgb(p, 1, 0.38, 0.33, 0.27);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	(void)u;
}

/* The days going by: shafts of light, the night, the years. */
static void draw_time(struct decay *s, cairo_t *cr) {
	double u = s->u, W = s->width, H = s->height, P = progress(s);
	double day = s->light;
	// shafts of light from a high window, in the day
	if (day > 0.05) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		for (int k = 0; k < 3; k++) {
			double x = W * (0.2 + 0.28 * k) + sin(s->t * 0.05 + k) * W * 0.04, w = W * (0.05 + 0.03 * k);
			double slant = H * 0.45;
			cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + slant, H);
			cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.9, 0.7, 0.12 * day);
			cairo_pattern_add_color_stop_rgba(p, 1, 1, 0.9, 0.7, 0);
			cairo_move_to(cr, x, 0);
			cairo_line_to(cr, x + w, 0);
			cairo_line_to(cr, x + w + slant, H);
			cairo_line_to(cr, x + slant, H);
			cairo_close_path(cr);
			cairo_set_source(cr, p);
			cairo_fill(cr);
			cairo_pattern_destroy(p);
		}
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	// the night, and the dimness of the ages
	double dark = (1 - day) * 0.45 + 0.12 * smooth(0.3f, 1, (float)P);
	cairo_set_source_rgba(cr, 0.04, 0.035, 0.06, fmin(0.7, dark));
	cairo_paint(cr);
	if (s->years) {
		double years = pow(10, 4.3 * fmin(P, 1.2)) - 1;
		char text[64];
		long y = 2026 + (long)years;
		if (y < 100000) {
			snprintf(text, sizeof(text), "%ld", y);
		} else {
			snprintf(text, sizeof(text), "%ld,%03ld", y / 1000, y % 1000);
		}
		if (!s->layout) {
			s->layout = pango_cairo_create_layout(cr);
			PangoFontDescription *font = pango_font_description_from_string("serif italic");
			pango_font_description_set_absolute_size(font, fmax(8, u * 26) * PANGO_SCALE);
			pango_layout_set_font_description(s->layout, font);
			pango_font_description_free(font);
		} else {
			pango_cairo_update_layout(cr, s->layout);
		}
		pango_layout_set_text(s->layout, text, -1);
		int tw, th;
		pango_layout_get_pixel_size(s->layout, &tw, &th);
		cairo_move_to(cr, W - tw - u * 28, H - th - u * 22);
		cairo_set_source_rgba(cr, 0.92, 0.84, 0.66, 0.5 * (1 - smooth(1, 1.25f, (float)P)) + 0.15);
		pango_cairo_show_layout(cr, s->layout);
	}
}

/* The slow parts, built into the next picture a part a frame (-1: all at once), then shown. */
static void build_scene(struct decay *s, int part) {
	cairo_t *cr = cairo_create(s->next);
	if (part <= 0) {
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_surface(cr, s->view, 0, 0);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	if (part < 0 || part == 1) {
		draw_cracks(s, cr);
	}
	if (part < 0 || part == 2) {
		double P = progress(s);
		if (P > 0.82 && part >= 0) {
			// the rubble greys and crumbles away into the dust
			cairo_t *rc = cairo_create(s->rubble);
			// (this part is built five times a second: gone to a trace by 1.02)
			double a = 1 - pow(0.005, 1 / (0.2 * s->span * 5));
			cairo_set_operator(rc, CAIRO_OPERATOR_ATOP);
			cairo_set_source_rgba(rc, 0.55, 0.5, 0.43, fmin(1, a * 3));
			cairo_paint(rc);
			cairo_set_operator(rc, CAIRO_OPERATOR_DEST_OUT);
			cairo_set_source_rgba(rc, 0, 0, 0, a);
			cairo_paint(rc);
			cairo_destroy(rc);
		}
		draw_ground(s, cr);
	}
	if (part < 0 || part == 3) {
		draw_time(s, cr);
		cairo_surface_t *shown = s->scene;
		s->scene = s->next;
		s->next = shown;
	}
	cairo_destroy(cr);
}

/* ---------- the saver ---------- */

static void *decay_create(int width, int height, const struct saver_options *options) {
	struct decay *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	double u = s->u = saver_unit(width, height);
	static const double spans[] = { 360, 720, 150 };
	s->span = spans[saver_choice(options, &saver_doomsday, "decay_pace")];
	s->years = saver_toggle(options, &saver_doomsday, "decay_years");
	bool real = saver_tilewin_windows(options->output, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	s->orig = saver_desktop(options, width, height, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	s->bare = saver_bare_desktop(options, s->orig, width, height, s->wins, s->count, s->bars,
		s->bar_count, fmax(8, u * 14), real, true);
	size_t n = (size_t)width * height;
	s->owner = malloc(sizeof(short) * n);
	s->src = calloc(n, 1);
	s->age_n = malloc(n);
	s->stain_n = malloc(n);
	s->grain_n = malloc(n);
	for (size_t i = 0; i < n; i++) {
		s->owner[i] = -1;
	}
	int owners = s->count + s->bar_count;
	for (int o = 0; o < owners; o++) {
		struct saver_window *w = o < s->count ? &s->wins[o] : &s->bars[o - s->count];
		int x0 = (int)fmax(0, w->x), y0 = (int)fmax(0, w->y);
		int x1 = (int)fmin(width, w->x + w->w), y1 = (int)fmin(height, w->y + w->h);
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				s->owner[y * width + x] = (short)o;
			}
		}
	}
	// the colour of each, from what shows of it
	cairo_surface_flush(s->orig);
	const uint32_t *px = (const uint32_t *)cairo_image_surface_get_data(s->orig);
	double sum[OWNERS_MAX][3] = { { 0 } };
	long count[OWNERS_MAX] = { 0 };
	for (size_t i = 0; i < n; i += 7) {
		int o = s->owner[i];
		if (o >= 0) {
			sum[o][0] += (px[i] >> 16) & 255;
			sum[o][1] += (px[i] >> 8) & 255;
			sum[o][2] += px[i] & 255;
			count[o]++;
		}
	}
	for (int o = 0; o < owners; o++) {
		long c = count[o] ? count[o] : 1;
		s->flat[o] = 0xff000000u | (uint32_t)(sum[o][0] / c) << 16 | (uint32_t)(sum[o][1] / c) << 8 |
			(uint32_t)(sum[o][2] / c);
	}
	// noise for every pixel: where it ages early or late, where stains spread, grain
	struct grid ag, sg;
	grid_make(&ag, width, height, 4, (float)fmax(4, u * 160), 71);
	grid_make(&sg, width, height, 4, (float)fmax(4, u * 45), 83);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t i = (size_t)y * width + x;
			s->age_n[i] = (uint8_t)fminf(255, grid_at(&ag, x, y) * 255);
			float st = grid_at(&sg, x, y);
			s->stain_n[i] = (uint8_t)fminf(255, fmaxf(0, (st - 0.25f) * 2 * 255));
			s->grain_n[i] = (uint8_t)(hash2(x, y, 17) & 255);
		}
	}
	free(ag.v);
	free(sg.v);
	s->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(s->view);
	cairo_set_source_surface(cr, s->orig, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	s->scene = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->next = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->rubble = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->heap = calloc(width, sizeof(float));
	s->dust = calloc(width, sizeof(float));
	s->top = calloc(width, sizeof(float));
	s->dune = malloc(sizeof(float) * width);
	for (int x = 0; x < width; x++) {
		s->dune[x] = fbm(x / (float)fmax(4, u * 260), 0.5f, 0, 91, 3);
	}
	s->rubble_top = (float)height;
	make_pieces(s);
	for (int i = 0; i < MOTES_MAX; i++) {
		s->motes[i] = (struct mote){ (float)(saver_random() * width), (float)(saver_random() * height),
			(float)saver_between(0, 6.3), (float)(u * saver_between(0.8, 2.2)) };
	}
	s->light = 1;
	build_scene(s, -1);
	return s;
}

static void decay_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct decay *s = state;
	double u = s->u, W = width, H = height;
	s->t += dt;
	s->frame++;
	double P = progress(s);
	// the days go by, faster and faster
	s->day += dt * 2 * M_PI / (14 - 10 * smooth(0, 0.9f, (float)P));
	s->light = 0.5 + 0.5 * sin(s->day);
	// pieces come off
	int owners = s->count + s->bar_count;
	for (int o = 0; o < owners; o++) {
		if (s->state[o] == GONE) {
			continue;
		}
		if (s->state[o] == INTACT && s->t >= s->start[o]) {
			s->state[o] = CRUMBLING;
		}
		if (s->state[o] != CRUMBLING) {
			continue;
		}
		bool all = true;
		for (int k = s->first[o]; k < s->last[o]; k++) {
			struct piece *p = &s->pieces[k];
			if (!p->fallen && s->t >= p->fall_at) {
				come_off(s, p);
			}
			all = all && p->fallen;
			// dust trickles from the cracks of a piece working loose
			if (!p->fallen && s->t > p->fall_at - s->span * 0.035 && saver_random() < dt * 3) {
				int i = (int)(saver_random() * p->n);
				*new_grain(s) = (struct grain){ p->px[i], p->py[i], 0, 0, 3, 0,
					(float)(u * saver_between(0.8, 1.8)), true };
			}
		}
		if (all) {
			s->state[o] = GONE;
		}
	}
	update_rows(s);
	// the rubble crumbles to dust; the dust settles, flat and smooth
	float crumble = (float)(u * (0.02 + 2.5 * smooth(0.82f, 1.05f, (float)P)) * dt);
	for (int x = 0; x < s->width; x++) {
		// over the rubble, in dunes
		float dune = (float)(u * (6 + 60 * s->dune[x] * s->dune[x]));
		float want = s->heap[x] + dune;
		if (want > s->dust[x]) {
			// at the end the dust closes over the rubble fast, whatever is left of it
			float close = (float)((want - s->dust[x]) * 0.25 * smooth(0.85f, 1, (float)P) * dt);
			s->dust[x] = fminf(want, s->dust[x] + fmaxf(crumble, close));
		}
		s->dust[x] += (float)(u * 0.03 * smooth(0.5f, 1, (float)P) * dt);
	}
	for (int x = 0; x + 1 < s->width; x++) { // rubble slides where it is heaped too steeply
		float d = s->heap[x] - s->heap[x + 1];
		if (fabsf(d) > 1.2f) {
			float m = (fabsf(d) - 1.2f) * 0.5f * (d > 0 ? 1 : -1);
			s->heap[x] -= m;
			s->heap[x + 1] += m;
		}
	}
	for (int pass = 0; pass < 2; pass++) {
		float prev = s->dust[0];
		for (int x = 1; x + 1 < s->width; x++) {
			float here = s->dust[x], d = here - s->dust[x + 1];
			if (fabsf(d) > 0.45f) {
				float m = (fabsf(d) - 0.45f) * 0.5f * (d > 0 ? 1 : -1);
				s->dust[x] -= m;
				s->dust[x + 1] += m;
			}
			s->dust[x] += 0.15f * (prev + s->dust[x + 1] - 2 * s->dust[x]);
			prev = here;
		}
	}
	if (s->part < 4) {
		build_scene(s, s->part); // the first part after the whole one built at the start
	}
	s->part = (s->part + 1) % SCENE_EVERY;

	cairo_set_source_surface(cr, s->scene, 0, 0);
	cairo_paint(cr);
	// the pieces in the air
	collide(s);
	for (int k = 0; k < BODIES_MAX; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive) {
			continue;
		}
		body_step(s, b, dt);
		if (!b->alive || !b->img) {
			continue;
		}
		cairo_save(cr);
		cairo_translate(cr, b->x, b->y);
		cairo_rotate(cr, b->angle);
		cairo_set_source_surface(cr, b->img, -b->ox, -b->oy);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	// dust: puffs and trickles, drawn as grains in two strengths
	for (int level = 0; level < 2; level++) {
		cairo_new_path(cr);
		for (int i = 0; i < GRAINS_MAX; i++) {
			struct grain *g = &s->grains[i];
			if (g->age >= g->life) {
				continue;
			}
			if (level == 0) {
				g->age += (float)dt;
				if (g->falls) {
					g->vy += (float)(u * 500 * dt);
				} else {
					g->vx -= g->vx * (float)fmin(1, 2.5 * dt);
					g->vy -= g->vy * (float)fmin(1, 2.5 * dt) - (float)(u * 12 * dt);
				}
				g->x += g->vx * (float)dt;
				g->y += g->vy * (float)dt;
				if (g->falls && g->y >= ground(s, g->x)) {
					int c = (int)g->x;
					if (c >= 0 && c < s->width) {
						s->dust[c] += (float)(u * 0.4);
					}
					g->age = g->life;
					continue;
				}
			}
			double k = 1 - g->age / g->life;
			if ((k > 0.5) != (level == 1)) {
				continue;
			}
			cairo_rectangle(cr, g->x - g->size / 2, g->y - g->size / 2, g->size, g->size);
		}
		cairo_set_source_rgba(cr, 0.72, 0.65, 0.54, level ? 0.7 : 0.35);
		cairo_fill(cr);
	}
	// motes drifting, bright where a shaft of light catches them
	cairo_new_path(cr);
	double wind = sin(s->t * 0.1) * u * 6;
	for (int i = 0; i < MOTES_MAX * (0.3 + 0.7 * fmin(1, P * 1.5)); i++) {
		struct mote *m = &s->motes[i];
		m->phase += (float)dt;
		m->x += (float)((wind + sin(m->phase * 0.7) * u * 5) * dt);
		m->y += (float)((cos(m->phase * 0.5) * u * 4 + u * 1.5) * dt);
		if (m->y > H) {
			m->y = 0;
		}
		if (m->x < 0) {
			m->x += (float)W;
		}
		if (m->x > W) {
			m->x -= (float)W;
		}
		cairo_rectangle(cr, m->x, m->y, m->size, m->size);
	}
	cairo_set_source_rgba(cr, 1, 0.93, 0.78, 0.18 + 0.4 * s->light);
	cairo_fill(cr);
}

static void decay_destroy(void *state) {
	struct decay *s = state;
	for (int k = 0; k < BODIES_MAX; k++) {
		if (s->bodies[k].img) {
			cairo_surface_destroy(s->bodies[k].img);
		}
	}
	if (s->layout) {
		g_object_unref(s->layout);
	}
	cairo_surface_destroy(s->orig);
	cairo_surface_destroy(s->bare);
	cairo_surface_destroy(s->view);
	cairo_surface_destroy(s->scene);
	cairo_surface_destroy(s->next);
	cairo_surface_destroy(s->rubble);
	free(s->owner);
	free(s->src);
	free(s->age_n);
	free(s->stain_n);
	free(s->grain_n);
	free(s->pieces);
	free(s->cracks);
	free(s->heap);
	free(s->dust);
	free(s->top);
	free(s->dune);
	free(s);
}

void saver_decay_stats(void *state, struct decay_stats *out) {
	struct decay *s = state;
	*out = (struct decay_stats){ .pieces = s->piece_count, .cracks = s->crack_count,
		.fallen = s->fallen, .resting = s->resting, .shattered = s->shattered,
		.progress = progress(s), .inside = true };
	for (int k = 0; k < BODIES_MAX; k++) {
		struct body *b = &s->bodies[k];
		if (b->alive) {
			out->flying++;
			if (!isfinite(b->x) || !isfinite(b->y) || b->x < -b->radius - 5 ||
					b->x > s->width + b->radius + 5) {
				out->inside = false;
			}
		}
	}
	for (int x = 0; x < s->width; x++) {
		out->rubble += s->heap[x] / s->width;
		out->dust += s->dust[x] / s->width;
	}
	for (int o = 0; o < s->count + s->bar_count; o++) {
		out->standing += s->state[o] != GONE;
	}
	// how much of the screen is still the picture it was, not aged
	long same = 0, all = 0;
	cairo_surface_flush(s->view);
	const uint32_t *v = (const uint32_t *)cairo_image_surface_get_data(s->view);
	const uint32_t *o = (const uint32_t *)cairo_image_surface_get_data(s->orig);
	for (int i = 0; i < s->width * s->height; i += 13) {
		same += (v[i] & 0xffffff) == (o[i] & 0xffffff);
		all++;
	}
	out->untouched = all ? (double)same / all : 0;
}

const struct saver saver_decay = {
	.name = "decay",
	.title = "Decay",
	.description = "The desktop eaten by time: yellowing, cracking, falling apart into dust",
	.wants_desktop = true,
	.covers = true,
	.create = decay_create,
	.draw = decay_draw,
	.destroy = decay_destroy,
};
