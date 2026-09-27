#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saver_noise.h"
#include "doomsday.h"

/*
 * Decay: the desktop eaten by time. Days flicker past faster and faster and
 * the years count up in a corner. Each thing ages as what it is made of:
 *
 *  - the desktop behind is plaster: it yellows, stains creep over it, grime
 *    settles;
 *  - the insides of the windows are glass: a film of grime, dirt running
 *    down in streaks, rings where water dried, a milky haze; one day
 *    something hits it and it cracks in a web from that point, straight
 *    lines out and rings between, and later the shards fall out of the
 *    frame, those nearest the break first;
 *  - the frames (title bars and borders) are wood or stone. Wood: the paint
 *    fades and peels to the grain, the wood greys, rots in patches and splits
 *    along the grain into long planks. Stone: the paint wears off, the stone
 *    pits, lichen spreads, cracks run jagged, and it breaks into blocks;
 *  - the taskbar is stone.
 *
 * What comes off falls as a rigid body: real polygons with their mass and
 * spin, colliding with each other, with the windows still standing and with
 * the rubble, bouncing and sliding as their material does (glass skitters,
 * wood knocks and bounces, stone thuds and stays), resting on each other in
 * a heap, and breaking when they land hard: glass shatters into shards from
 * where it hit and a spray of glittering splinters, wood splits along its
 * grain and throws fibres, stone breaks into chunks in a puff of dust. The
 * pieces cast shadows as they fall, glass flashes as it turns, and the
 * shards in the heap glint. In the end the rubble crumbles and dust drifts
 * over it: nothing is left but dust, dunes of it, and motes of it drifting
 * through the light.
 *
 * The windows and the taskbar are the real ones: the picture of the screen
 * from before the saver, and where they are from tileWin; what was behind
 * them is the wallpaper tileWin reports, or the colour of the desktop around.
 */

#define WINDOWS_MAX 48
#define OWNERS_MAX (WINDOWS_MAX + 4)
#define PIECES_MAX 4000
#define CRACKS_MAX 16000
#define POLY_MAX 10
#define BODIES_MAX 1500      // falling and at rest together
#define CONTACTS_MAX 6000
#define BUCKET 48            // pixels of a column of the rubble, to find what is near
#define GRAINS_MAX 1400
#define MOTES_MAX 500
#define GLINTS_MAX 60
#define LAYERS 3             // pieces fall at different depths, in front of each other
#define ROW_STEP 16          // the ageing is brought up to date on every 16th row a frame
#define SCENE_EVERY 6        // frames to build the slow parts in (in 4 of them a part each)
#define STEP_MAX (1 / 90.0)  // seconds of a step of the physics at most (five at most a frame)
#define FLYING_MAX 150       // pieces in the air at once; more wait a moment before they come off
#define RESTING_MAX 700      // pieces at rest kept in the way; the lowest are buried anyway

enum material { WALL, GLASS, WOOD, STONE };

/* How each material falls, bounces and breaks. */
struct stuff {
	float density, bounce, friction;
	float breaks;            // the speed it hits with that breaks it, in thousandths a second
	float smallest;          // a piece smaller across than this (thousandths) does not break
};

static const struct stuff stuffs[4] = {
	[WALL] = { 1.5f, 0.1f, 0.6f, 1e9f, 1e9f },
	[GLASS] = { 2.5f, 0.18f, 0.28f, 190, 11 },
	[WOOD] = { 0.7f, 0.34f, 0.55f, 560, 18 },
	[STONE] = { 2.4f, 0.07f, 0.85f, 360, 14 },
};

struct piece {
	float px[POLY_MAX], py[POLY_MAX];
	int n;
	float cx, cy, area;
	int owner;
	uint8_t mat;
	float fall_at;           // seconds: when it comes off
	bool fallen;
};

/* A crack: a short straight stretch, showing from at on, drawn as its material cracks. */
struct crack {
	float x0, y0, x1, y1;
	float at, grow;          // when it starts, and how long it takes to run its length
	int owner;
	uint8_t mat;
	uint32_t seed;
};

struct body {
	cairo_surface_t *img;    // its picture, while it moves
	float ox, oy;            // its middle in the picture
	float lx[POLY_MAX], ly[POLY_MAX]; // its outline about the middle
	float wx[POLY_MAX], wy[POLY_MAX]; // and where that is now
	float nx[POLY_MAX], ny[POLY_MAX]; // the outward normals of its edges, now
	int n;
	float x, y, a, vx, vy, w;
	float inv_m, inv_i, radius, area;
	float x0, y0, x1, y1;    // the box around it
	float still, phase;
	float hit, hit_x, hit_y; // the hardest it struck something this frame, and where
	int generation;          // how often it broke already
	uint8_t mat;
	uint8_t layer;           // how far from the wall it falls: only the same layer meets it
	bool alive, asleep;      // asleep: at rest in the rubble for good
	int perch;               // lying still on the top of a window standing, -1 not
};

struct contact {
	int a, b;                // bodies, a -1 for the ground and the walls, -2 - k window k
	float nx, ny;            // from a to b
	float px, py, pen;
	float kn, kt, bias, jn, jt, mu;
};

enum grain_kind { DUST, FIBRE, GLITTER };

struct grain {
	float x, y, vx, vy, life, age, size;
	uint8_t kind;
	bool settles;            // falls and settles on the ground, instead of drifting
};

struct mote {
	float x, y, phase, size;
};

struct glint {
	float x, y, age, life, size;
};

enum { INTACT, CRUMBLING, GONE };

/* A cobweb in a corner: of a window's glass, or of the screen (owner -1). */
struct web {
	float x, y, size, at;
	int corner;              // 0 top left, 1 top right, 2 bottom right, 3 bottom left
	int owner;
};

#define WEBS_MAX 120

struct decay {
	int width, height;
	double u, t, span;       // span: seconds until all is dust
	int frame, part;
	cairo_surface_t *orig, *bare; // the desktop, and without the windows and taskbars
	cairo_surface_t *view;        // aged, with the pieces that are gone gone
	cairo_surface_t *scene, *next; // the slow parts, and the next of them, built a part a frame
	cairo_surface_t *rubble;      // the pieces at rest
	cairo_surface_t *rays;        // shafts of light, drawn once
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	uint8_t frame_mat[OWNERS_MAX]; // what each frame is made of
	float edge[OWNERS_MAX];       // how wide the frame of each is, at the sides and bottom
	uint32_t flat[OWNERS_MAX];    // the colour of each, for what of it was hidden
	short *owner;                 // per pixel: the window or taskbar that shows, -1 none
	uint8_t *src;                 // per pixel: 0 the picture, 1 behind, 2 + k flat of k
	uint8_t *mat;                 // per pixel: what it is made of
	uint8_t *age_n, *stain_n, *grain_n, *wood_n; // per pixel noise
	uint8_t *peel_n, *mould_n;    // where the plaster flakes off first, where mould grows
	uint8_t *dirt;                // how much dirt the rain washes down the wall there
	struct piece *pieces;
	int piece_count;
	int first[OWNERS_MAX], last[OWNERS_MAX];
	float start[OWNERS_MAX], end[OWNERS_MAX];
	int state[OWNERS_MAX];
	struct crack *cracks;
	int crack_count;
	struct body *bodies;
	int body_count;               // slots used, alive or not
	short **buckets;              // per column of BUCKET: the bodies at rest in it
	int *bucket_len, *bucket_cap, bucket_count;
	struct contact *contacts;
	int contact_count;
	struct grain grains[GRAINS_MAX];
	int grain_next;
	struct mote motes[MOTES_MAX];
	struct glint glints[GLINTS_MAX];
	float *heap, *dust, *dune;    // per column: top of the rubble, the dust, how high it drifts
	int fallen, resting, shattered;
	int flying;                   // in the air now
	double light, day, shake;
	bool years;
	PangoLayout *layout;
	float rubble_top;
	struct web webs[WEBS_MAX];
	int web_count;
	cairo_surface_t *web_img;     // a cobweb in the top left corner of its picture
	cairo_surface_t *puff;        // a soft round of dust
};

static double progress(struct decay *s) {
	return s->t / s->span;
}

static inline float cross2(float ax, float ay, float bx, float by) {
	return ax * by - ay * bx;
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

/* ---------- breaking the windows up, before anything happens ---------- */

/* A list of polygons being cut, all of one owner and material. */
struct cutting {
	float x[160][POLY_MAX], y[160][POLY_MAX];
	int n[160], count;
};

static void add_crack(struct decay *s, float x0, float y0, float x1, float y1, float at,
		float grow, int owner, uint8_t mat) {
	if (owner < -1) {
		return; // the cut of a piece in the air: no crack on anything
	}
	// in short stretches, each shown only where the one it runs through still shows
	double len = hypot(x1 - x0, y1 - y0), most = fmax(4, s->u * 16);
	int parts = (int)ceil(len / most);
	for (int k = 0; k < parts && s->crack_count < CRACKS_MAX; k++) {
		float a = (float)k / parts, b = (float)(k + 1) / parts;
		s->cracks[s->crack_count++] = (struct crack){ x0 + (x1 - x0) * a, y0 + (y1 - y0) * a,
			x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, at + grow * a, grow / parts, owner, mat,
			(uint32_t)(saver_random() * 1e9) };
	}
}

/*
 * Cuts the polygon k of a cutting along the line through (px, py) with normal
 * (nx, ny); the crack along the cut shows from at on (for an owner). False when
 * the line misses it or a part would be too small.
 */
static bool cut_one(struct decay *s, struct cutting *c, int k, float px, float py, float nx,
		float ny, float at, float grow, int owner, uint8_t mat) {
	if (c->count >= 160) {
		return false;
	}
	float ax[POLY_MAX], ay[POLY_MAX], bx[POLY_MAX], by[POLY_MAX], cut[2][2], cx, cy;
	int cuts = 0;
	int na = clip_side(c->x[k], c->y[k], c->n[k], px, py, nx, ny, 1, ax, ay, cut, &cuts);
	int nb = clip_side(c->x[k], c->y[k], c->n[k], px, py, nx, ny, -1, bx, by, NULL, NULL);
	float min_area = (float)(s->u * s->u * 16);
	if (na < 3 || nb < 3 || cuts < 2 || poly_area(ax, ay, na, &cx, &cy) < min_area ||
			poly_area(bx, by, nb, &cx, &cy) < min_area) {
		return false;
	}
	memcpy(c->x[k], ax, sizeof(float) * na);
	memcpy(c->y[k], ay, sizeof(float) * na);
	c->n[k] = na;
	memcpy(c->x[c->count], bx, sizeof(float) * nb);
	memcpy(c->y[c->count], by, sizeof(float) * nb);
	c->n[c->count++] = nb;
	add_crack(s, cut[0][0], cut[0][1], cut[1][0], cut[1][1], at, grow, owner, mat);
	return true;
}

/* Cuts every polygon of a cutting the line crosses. */
static void cut_all(struct decay *s, struct cutting *c, float px, float py, float nx, float ny,
		float at, float grow, int owner, uint8_t mat) {
	int count = c->count;
	for (int k = 0; k < count; k++) {
		cut_one(s, c, k, px, py, nx, ny, at, grow, owner, mat);
	}
}

static void start_cutting(struct cutting *c, float x0, float y0, float x1, float y1) {
	c->count = 1;
	c->n[0] = 4;
	float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 };
	memcpy(c->x[0], px, sizeof(px));
	memcpy(c->y[0], py, sizeof(py));
}

/* The pieces of a cutting become pieces of the owner, coming off between from and to. */
static void keep_pieces(struct decay *s, struct cutting *c, int owner, uint8_t mat, float from,
		float to, float fx, float fy, float reach) {
	for (int k = 0; k < c->count && s->piece_count < PIECES_MAX; k++) {
		struct piece *p = &s->pieces[s->piece_count++];
		memcpy(p->px, c->x[k], sizeof(float) * c->n[k]);
		memcpy(p->py, c->y[k], sizeof(float) * c->n[k]);
		p->n = c->n[k];
		p->owner = owner;
		p->mat = mat;
		p->area = poly_area(p->px, p->py, p->n, &p->cx, &p->cy);
		p->fallen = false;
		// the nearest to (fx, fy) first, with a good deal of chance
		float d = hypotf(p->cx - fx, p->cy - fy) / fmaxf(1, reach);
		p->fall_at = from + (to - from) * fminf(1, fmaxf(0, d * 0.65f + (float)saver_random() * 0.35f));
	}
}

/* A pane of glass: a web of cracks from where it was hit, falling out nearest that first. */
static void break_glass(struct decay *s, int owner, float x0, float y0, float x1, float y1,
		float hit_at, float from, float to) {
	struct cutting *c = malloc(sizeof(*c));
	start_cutting(c, x0, y0, x1, y1);
	double u = s->u;
	float px = x0 + (x1 - x0) * (float)saver_between(0.25, 0.75);
	float py = y0 + (y1 - y0) * (float)saver_between(0.25, 0.75);
	float reach = 0;
	for (int k = 0; k < 4; k++) {
		reach = fmaxf(reach, hypotf((k & 1 ? x1 : x0) - px, (k & 2 ? y1 : y0) - py));
	}
	// straight lines out from the break, all at once
	int radials = 6 + (int)(saver_random() * 4);
	double base = saver_between(0, M_PI);
	for (int k = 0; k < radials; k++) {
		double a = base + k * M_PI / radials + saver_between(-0.12, 0.12);
		cut_all(s, c, px, py, (float)-sin(a), (float)cos(a), hit_at, 1.2f, owner, GLASS);
	}
	// rings between them, further out, spreading later
	int wedges = c->count;
	for (int k = 0; k < wedges; k++) {
		float cx, cy;
		poly_area(c->x[k], c->y[k], c->n[k], &cx, &cy);
		float dx = cx - px, dy = cy - py, d = fmaxf(0.01f, hypotf(dx, dy));
		dx /= d;
		dy /= d;
		for (float r = reach * (float)saver_between(0.1, 0.18); r < reach; r *= (float)saver_between(1.5, 2)) {
			// the ring cuts only the wedge it runs across, where it is now
			for (int j = 0; j < c->count; j++) {
				float jx, jy;
				poly_area(c->x[j], c->y[j], c->n[j], &jx, &jy);
				if (fabsf((jx - px) * dy - (jy - py) * dx) > reach * 0.35f ||
						(jx - px) * dx + (jy - py) * dy < 0) {
					continue;
				}
				float tilt = (float)saver_between(-0.3, 0.3);
				if (cut_one(s, c, j, px + dx * r, py + dy * r, dx + tilt * dy, dy - tilt * dx,
						hit_at + 2 + r / reach * (float)(s->span * 0.05), 2.5f, owner, GLASS)) {
					break;
				}
			}
		}
	}
	// big pieces far out break once more
	for (int k = 0; k < c->count; k++) {
		float cx, cy;
		if (poly_area(c->x[k], c->y[k], c->n[k], &cx, &cy) > (u * 90) * (u * 90)) {
			double a = saver_between(0, M_PI);
			cut_one(s, c, k, cx, cy, (float)cos(a), (float)sin(a),
				hit_at + (float)(s->span * saver_between(0.04, 0.1)), 3, owner, GLASS);
		}
	}
	keep_pieces(s, c, owner, GLASS, from, to, px, py, reach);
	free(c);
}

/* A frame: wood splits into planks along its length, stone into blocks. */
static void break_frame(struct decay *s, int owner, uint8_t mat, float x0, float y0, float x1,
		float y1, float crack_at, float from, float to) {
	double u = s->u;
	if (x1 - x0 < 2 || y1 - y0 < 2) {
		return;
	}
	struct cutting *c = malloc(sizeof(*c));
	start_cutting(c, x0, y0, x1, y1);
	bool across = x1 - x0 >= y1 - y0; // lying, or standing
	float len = across ? x1 - x0 : y1 - y0, thick = across ? y1 - y0 : x1 - x0;
	float span = (float)s->span;
	if (mat == WOOD) {
		// split along the grain first, early, then across into planks
		int splits = (int)(thick / (u * 11));
		for (int k = 1; k <= splits; k++) {
			float at = thick * k / (splits + 1) + (float)saver_between(-0.1, 0.1) * thick / (splits + 1);
			float tilt = (float)saver_between(-0.01, 0.01);
			if (across) {
				cut_all(s, c, x0, y0 + at, tilt, 1, crack_at, span * 0.08f, owner, WOOD);
			} else {
				cut_all(s, c, x0 + at, y0, 1, tilt, crack_at, span * 0.08f, owner, WOOD);
			}
		}
		for (float at = (float)(u * saver_between(60, 150)); at < len - u * 30;
				at += (float)(u * saver_between(70, 170))) {
			float tilt = (float)saver_between(-0.25, 0.25);
			float when = crack_at + span * (float)saver_between(0.05, 0.12);
			if (across) {
				cut_all(s, c, x0 + at, y0, 1, tilt, when, 2, owner, WOOD);
			} else {
				cut_all(s, c, x0, y0 + at, tilt, 1, when, 2, owner, WOOD);
			}
		}
	} else {
		// stone: courses of blocks, offset, and chipped here and there
		int rows = (int)fmaxf(1, roundf(thick / (u * 20)));
		for (int k = 1; k < rows; k++) {
			float at = thick * k / rows;
			if (across) {
				cut_all(s, c, x0, y0 + at, (float)saver_between(-0.05, 0.05), 1, crack_at, 1, owner, STONE);
			} else {
				cut_all(s, c, x0 + at, y0, 1, (float)saver_between(-0.05, 0.05), crack_at, 1, owner, STONE);
			}
		}
		for (float at = (float)(u * saver_between(20, 50)); at < len - u * 12;
				at += (float)(u * saver_between(28, 55))) {
			float tilt = (float)saver_between(-0.15, 0.15);
			float when = crack_at + span * (float)saver_between(0, 0.08);
			int count = c->count;
			for (int k = 0; k < count; k++) {
				if (saver_random() < 0.8) {
					float jog = (float)saver_between(-8, 8) * (float)u;
					if (across) {
						cut_one(s, c, k, x0 + at + jog, y0, 1, tilt, when, 1.5f, owner, STONE);
					} else {
						cut_one(s, c, k, x0, y0 + at + jog, tilt, 1, when, 1.5f, owner, STONE);
					}
				}
			}
		}
		int count = c->count;
		for (int k = 0; k < count; k++) {
			if (saver_random() < 0.3) {
				float cx, cy;
				poly_area(c->x[k], c->y[k], c->n[k], &cx, &cy);
				double a = saver_between(0, M_PI);
				cut_one(s, c, k, cx, cy, (float)cos(a), (float)sin(a),
					crack_at + span * (float)saver_between(0.03, 0.1), 1.5f, owner, STONE);
			}
		}
	}
	keep_pieces(s, c, owner, mat, from, to, (x0 + x1) / 2, y1, len);
	free(c);
}

/* A crack in the plaster of the wall: wandering, branching now and then. */
static void crack_wall(struct decay *s, float x, float y, float angle, float len, float at, int depth) {
	double u = s->u;
	float walked = 0;
	while (walked < len) {
		// plaster breaks in straight runs with a sharp turn now and then
		float step = (float)(u * saver_between(8, 20));
		angle += saver_random() < 0.25 ? (float)saver_between(-0.7, 0.7) : (float)saver_between(-0.12, 0.12);
		float nx = x + cosf(angle) * step, ny = y + sinf(angle) * step;
		add_crack(s, x, y, nx, ny, at + walked / len * (float)(s->span * 0.08), 0.6f, -1, WALL);
		x = nx;
		y = ny;
		walked += step;
		if (depth < 2 && saver_random() < 0.07) {
			crack_wall(s, x, y, angle + (float)saver_between(0.5, 1.1) * (saver_random() < 0.5 ? -1 : 1),
				(len - walked) * (float)saver_between(0.3, 0.6), at + walked / len * (float)(s->span * 0.08),
				depth + 1);
		}
	}
}

/* When each window and taskbar falls apart: the top window first, one under another after it. */
static void schedule(struct decay *s) {
	double span = s->span;
	for (int i = s->count - 1; i >= 0; i--) {
		struct saver_window *w = &s->wins[i];
		double at = span * (0.4 + (s->count - 1 - i) * 0.03 + saver_between(0, 0.03));
		for (int j = i + 1; j < s->count; j++) {
			struct saver_window *o = &s->wins[j];
			bool overlap = o->x < w->x + w->w && w->x < o->x + o->w && o->y < w->y + w->h &&
				w->y < o->y + o->h;
			if (overlap) {
				at = fmax(at, s->end[j] + span * 0.01);
			}
		}
		s->start[i] = (float)at;
		s->end[i] = (float)(at + span * 0.16);
	}
	double latest = span * 0.4;
	for (int i = 0; i < s->count; i++) {
		latest = fmax(latest, s->end[i]);
	}
	if (latest > span * 0.86) {
		double k = (span * 0.86 - span * 0.4) / (latest - span * 0.4);
		for (int i = 0; i < s->count; i++) {
			s->start[i] = (float)(span * 0.4 + (s->start[i] - span * 0.4) * k);
			s->end[i] = (float)(span * 0.4 + (s->end[i] - span * 0.4) * k);
		}
	}
	for (int k = 0; k < s->bar_count; k++) {
		s->start[s->count + k] = (float)(span * saver_between(0.86, 0.88));
		s->end[s->count + k] = (float)(span * 0.96);
	}
}

/* The glass of a window: inside its frame. */
static void glass_of(struct decay *s, int o, float *x0, float *y0, float *x1, float *y1) {
	struct saver_window *w = &s->wins[o];
	float e = s->edge[o];
	*x0 = (float)fmax(0, w->x + e);
	*y0 = (float)fmax(0, w->y + w->title_h);
	*x1 = (float)fmin(s->width, w->x + w->w - e);
	*y1 = (float)fmin(s->height, w->y + w->h - e);
}

static void make_pieces(struct decay *s) {
	s->pieces = calloc(PIECES_MAX, sizeof(struct piece));
	s->cracks = calloc(CRACKS_MAX, sizeof(struct crack));
	schedule(s);
	for (int o = 0; o < s->count + s->bar_count; o++) {
		s->first[o] = s->piece_count;
		float dur = s->end[o] - s->start[o];
		if (o >= s->count) {
			struct saver_window *b = &s->bars[o - s->count];
			break_frame(s, o, STONE, (float)fmax(0, b->x), (float)fmax(0, b->y),
				(float)fmin(s->width, b->x + b->w), (float)fmin(s->height, b->y + b->h),
				s->start[o] - (float)(s->span * 0.35), s->start[o], s->end[o]);
		} else {
			struct saver_window *w = &s->wins[o];
			float gx0, gy0, gx1, gy1;
			glass_of(s, o, &gx0, &gy0, &gx1, &gy1);
			float wx0 = (float)fmax(0, w->x), wy0 = (float)fmax(0, w->y);
			float wx1 = (float)fmin(s->width, w->x + w->w), wy1 = (float)fmin(s->height, w->y + w->h);
			uint8_t m = s->frame_mat[o];
			// the glass cracks first, and falls out in the first half; then the frame goes
			if (gx1 - gx0 > 4 && gy1 - gy0 > 4) {
				float hit = s->start[o] - (float)(s->span * saver_between(0.08, 0.15));
				break_glass(s, o, gx0, gy0, gx1, gy1, hit, s->start[o], s->start[o] + dur * 0.45f);
			}
			float crack_at = s->start[o] - (float)(s->span * 0.3);
			float frame_from = s->start[o] + dur * 0.5f;
			// the bottom and the sides first, the title bar last
			break_frame(s, o, m, wx0, gy1, wx1, wy1, crack_at, frame_from, frame_from + dur * 0.2f);
			break_frame(s, o, m, wx0, gy0, gx0, gy1, crack_at, frame_from + dur * 0.1f,
				frame_from + dur * 0.35f);
			break_frame(s, o, m, gx1, gy0, wx1, gy1, crack_at, frame_from + dur * 0.1f,
				frame_from + dur * 0.35f);
			break_frame(s, o, m, wx0, wy0, wx1, gy0, crack_at + (float)(s->span * 0.04),
				frame_from + dur * 0.3f, s->end[o]);
		}
		s->last[o] = s->piece_count; // (none at all: it is cleared away when its time comes)
	}
}

/* ---------- pixels ---------- */

static uint8_t material_of(struct decay *s, int o, int x, int y) {
	if (o < 0) {
		return WALL;
	}
	if (o >= s->count) {
		return STONE;
	}
	float gx0, gy0, gx1, gy1;
	glass_of(s, o, &gx0, &gy0, &gx1, &gy1);
	return x + 0.5f >= gx0 && x + 0.5f < gx1 && y + 0.5f >= gy0 && y + 0.5f < gy1 ? GLASS :
		s->frame_mat[o];
}

static inline uint32_t source_px(struct decay *s, int i, const uint32_t *orig, const uint32_t *bare) {
	uint8_t src = s->src[i];
	return src == 0 ? orig[i] : src == 1 ? bare[i] : s->flat[src - 2];
}

static inline uint32_t greyish(uint32_t p, uint32_t tint, int k) {
	int lum = (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8;
	uint32_t grey = 0xff000000u | (uint32_t)lum << 16 | (uint32_t)lum << 8 | (uint32_t)lum;
	return mix_px(p, mix_px(grey, tint, 70), k);
}

/* A brick of the wall under the plaster, mortar between, each brick its own. */
static inline uint32_t brick(struct decay *s, int x, int y, uint8_t gr) {
	int h = (int)fmax(4, s->u * 13), w = (int)(h * 2.3), m = (int)fmax(1, s->u * 1.8);
	int row = y / h, bx = x + (row & 1) * w / 2, col = bx / w;
	int yy = y % h, xx = bx % w;
	if (yy < m || xx < m) {
		return mix_px(0xffa09884, 0xff7a7262, gr);
	}
	uint32_t shade = hash2(col, row, 23) & 255;
	uint32_t b = mix_px(0xff7a3e28, 0xffa4623e, (int)shade);
	b = mix_px(b, 0xff4e2a1c, gr & 63);
	if (yy < m + 2) {
		b = mix_px(b, 0xffc07a50, 60); // lit along its top
	} else if (yy > h - 3) {
		b = mix_px(b, 0xff3a2016, 60);
	}
	return b;
}

/* The dust over it all at the end. */
static inline uint32_t aged_more(struct decay *s, int i, uint32_t p, float k3) {
	if (k3 > 0) {
		uint32_t dust = mix_px(0xff9c907c, 0xff766c5c, (int)(s->age_n[i] * 0.8f + (s->grain_n[i] & 31)));
		p = mix_px(p, dust, (int)(k3 * 256));
	}
	return p;
}

/* A pixel aged as what it is made of. */
static inline uint32_t aged(struct decay *s, int i, int x, int y, uint32_t p, float P) {
	float a = P * 1.08f + (s->age_n[i] / 255.0f - 0.5f) * 0.24f;
	if (a <= 0.01f) {
		return p;
	}
	float k1 = smooth(0.02f, 0.45f, a), k2 = smooth(0.28f, 0.8f, a), k3 = smooth(0.72f, 1.06f, a);
	float st = s->stain_n[i] / 255.0f;
	uint8_t gr = s->grain_n[i];
	switch (s->mat[i]) {
	case GLASS: {
		// a film of grime, dirt run down in streaks, rings where water dried, a milky haze
		p = greyish(p, 0xff8a8272, (int)(k1 * 0.45f * 256));
		p = mix_px(p, 0xff605a4e, (int)(k1 * 0.22f * 256));
		// streaks: run down in places, soft, fading out lower down
		float streak = noise(x / (float)fmax(1, s->u * 7), 0.5f, 0, 7);
		float run = s->age_n[i] / 255.0f;
		if (streak > 0.72f && run > 0.35f) {
			p = mix_px(p, 0xff4c4436, (int)(fminf(1, (streak - 0.72f) * 5) * (run - 0.35f) * 0.5f * k2 * 256));
		}
		float thr = 1 - k2 * 0.45f;
		if (st > thr - 0.02f && st < thr) {
			p = mix_px(p, 0xffd8d2c2, (int)(0.4f * k2 * 256));
		} else if (st > thr) {
			p = mix_px(p, 0xffb0aa9a, (int)(0.2f * k2 * 256));
		}
		p = mix_px(p, 0xffa8a294, (int)(k2 * 0.3f * 256));
		break;
	}
	case WOOD: {
		// the paint fades and peels; under it the grain, greying, and rot
		p = greyish(p, 0xffa89a80, (int)(k1 * 0.6f * 256));
		float g = s->wood_n[i] / 255.0f;
		uint32_t wood = mix_px(0xff5a3c22, 0xffa07a4c, (int)(g * 256));
		wood = mix_px(wood, 0xff8e887c, (int)(k2 * 0.75f * 256)); // silver with the weather
		if (gr < 18) {
			wood = mix_px(wood, 0xff2c2016, 120);
		}
		float peel = 1 - k1 * 0.95f;
		if (st > peel) {
			p = mix_px(p, wood, (int)(fminf(1, (st - peel) * 25) * 256));
		} else if (st > peel - 0.015f) {
			p = mix_px(p, 0xfff0e8d8, 90); // the curling edge of the paint
		}
		if (st > 1 - k2 * 0.22f) {
			p = mix_px(p, 0xff2a2016, (int)(0.7f * k2 * 256)); // rot
		}
		break;
	}
	case STONE: {
		// the paint wears off; the stone pits, darkens, and lichen spreads
		p = greyish(p, 0xff9a968c, (int)(k1 * 0.55f * 256));
		// mottled, with a fine grain and a pit here and there
		uint32_t stone = mix_px(0xff6e6a62, 0xffa29e94, (int)(s->age_n[i] * 0.6f + st * 100 + (gr & 31)));
		if (gr < 10) {
			stone = mix_px(stone, 0xff38352f, 130); // pits
		}
		float wear = 1 - k1 * 0.9f;
		if (st > wear) {
			p = mix_px(p, stone, (int)(fminf(1, (st - wear) * 10) * 256));
		}
		p = mix_px(p, 0xff4a463e, (int)(k2 * 0.25f * 256));
		float lichen = s->age_n[i] / 255.0f;
		if (lichen > 1 - k2 * 0.4f && st > 0.4f) {
			p = mix_px(p, gr & 1 ? 0xffa4a85c : 0xffb87a3e, (int)(0.6f * k2 * 256));
		}
		break;
	}
	default: {
		// plaster: it yellows and fades, unevenly
		int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
		int lum = (r * 77 + g * 150 + b * 29) >> 8;
		uint32_t sepia = 0xff000000u | (uint32_t)(lum * 1.02f + 22 > 255 ? 255 : lum * 1.02f + 22) << 16 |
			(uint32_t)(lum * 0.9f + 10) << 8 | (uint32_t)(lum * 0.68f + 4);
		p = mix_px(p, sepia, (int)(k1 * 0.8f * 256));
		p = mix_px(p, 0xffc4b28e, (int)(k1 * 0.25f * 256));
		p = mix_px(p, s->age_n[i] > 128 ? 0xffd8ccae : 0xff8a7a5c, (int)(k1 * abs(s->age_n[i] - 128) * 0.25f));
		// the plaster flakes off: brickwork under it, the broken edge light, a shadow under it
		float peel = s->peel_n[i] / 255.0f, off = 1 - k2 * 0.42f;
		if (peel > off) {
			return aged_more(s, i, brick(s, x, y, gr), k3);
		}
		if (peel > off - 0.012f) {
			p = mix_px(p, 0xffeee6d4, (int)(0.8f * k2 * 256));
		} else if (peel > off - 0.03f) {
			p = mix_px(p, 0xff3a3024, (int)(0.3f * k2 * 256));
		}
		// water stains: pale inside, with a brown tide line wherever one dried, ring in ring
		float thr = 1 - k2 * 0.38f;
		if (st > thr) {
			p = mix_px(p, 0xffa08058, (int)(fminf(1, (st - thr) * 10) * 0.2f * 256));
			for (int ring = 1; ring <= 2; ring++) {
				float at = thr + ring * 0.04f * ring;
				if (fabsf(st - at) < 0.005f) {
					p = mix_px(p, 0xff6a4a2a, (int)(0.35f * 256));
				}
			}
		}
		if (fabsf(st - thr) < 0.007f && k2 > 0) {
			p = mix_px(p, 0xff5a3c20, (int)(0.5f * k2 * 256));
		}
		// dirt: washed down the wall under the windows, settled low and in the corners
		p = mix_px(p, 0xff3a3226, (int)(s->dirt[i] * k2 * 0.6f));
		// mould, spreading up from below: dark, speckled, greenish
		float mould = s->mould_n[i] / 255.0f + (float)y / s->height * 0.25f;
		if (mould > 1.18f - k2 * 0.3f) {
			p = mix_px(p, gr < 150 ? 0xff242a1c : 0xff4a5234, gr < 150 ? 170 : 90);
		}
		if (gr < 10 * k2) {
			p = mix_px(p, 0xff2c2218, 50);
		}
		break;
	}
	}
	return aged_more(s, i, p, k3);
}

static void update_span(struct decay *s, int y, int x0, int x1) {
	uint32_t *orig = (uint32_t *)cairo_image_surface_get_data(s->orig);
	uint32_t *bare = (uint32_t *)cairo_image_surface_get_data(s->bare);
	uint32_t *view = (uint32_t *)cairo_image_surface_get_data(s->view);
	int W = s->width;
	float P = (float)progress(s);
	for (int x = x0; x < x1; x++) {
		int i = y * W + x;
		view[i] = aged(s, i, x, y, source_px(s, i, orig, bare), P);
	}
}

static void update_rows(struct decay *s) {
	cairo_surface_flush(s->view);
	for (int y = s->frame % ROW_STEP; y < s->height; y += ROW_STEP) {
		update_span(s, y, 0, s->width);
	}
	cairo_surface_mark_dirty(s->view);
}

/* ---------- dust, fibres, glitter ---------- */

static struct grain *new_grain(struct decay *s) {
	struct grain *g = &s->grains[s->grain_next];
	s->grain_next = (s->grain_next + 1) % GRAINS_MAX;
	return g;
}

/* What breaking or striking throws up: dust, fibres of wood, splinters of glass. */
static void spray(struct decay *s, uint8_t mat, float x, float y, int n, float power) {
	double u = s->u;
	for (int k = 0; k < n; k++) {
		double a = saver_between(-M_PI, 0), v = saver_between(20, 110) * u * power;
		struct grain *g = new_grain(s);
		*g = (struct grain){ x + (float)saver_between(-3, 3) * (float)u, y, (float)(cos(a) * v),
			(float)(sin(a) * v), (float)saver_between(0.8, 2.4), 0, 0, DUST, false };
		switch (mat) {
		case GLASS:
			g->kind = GLITTER;
			g->settles = true;
			g->size = (float)(u * saver_between(0.8, 2));
			g->life = (float)saver_between(4, 12);
			g->vy -= (float)(u * saver_between(30, 120) * power);
			break;
		case WOOD:
			g->kind = saver_random() < 0.5 ? FIBRE : DUST;
			g->settles = g->kind == FIBRE;
			g->size = (float)(u * saver_between(2, 6));
			break;
		default:
			g->size = (float)(u * saver_between(2, 5.5));
			break;
		}
	}
}

/* The ground at a column: the dust. */
static float ground(struct decay *s, float x) {
	int c = (int)x;
	c = c < 0 ? 0 : c >= s->width ? s->width - 1 : c;
	return s->height - s->dust[c];
}

/* ---------- bodies ---------- */

static void body_pose(struct body *b) {
	float ca = cosf(b->a), sa = sinf(b->a);
	b->x0 = b->y0 = 1e9f;
	b->x1 = b->y1 = -1e9f;
	for (int i = 0; i < b->n; i++) {
		b->wx[i] = b->x + b->lx[i] * ca - b->ly[i] * sa;
		b->wy[i] = b->y + b->lx[i] * sa + b->ly[i] * ca;
		b->x0 = fminf(b->x0, b->wx[i]);
		b->x1 = fmaxf(b->x1, b->wx[i]);
		b->y0 = fminf(b->y0, b->wy[i]);
		b->y1 = fmaxf(b->y1, b->wy[i]);
	}
	for (int i = 0; i < b->n; i++) {
		int j = (i + 1) % b->n;
		float dx = b->wx[j] - b->wx[i], dy = b->wy[j] - b->wy[i], d = fmaxf(1e-4f, hypotf(dx, dy));
		b->nx[i] = dy / d;
		b->ny[i] = -dx / d;
	}
}

/* A body of a polygon given about its middle: mass and inertia from its material. */
static struct body *new_body(struct decay *s, const float *lx, const float *ly, int n,
		uint8_t mat, float area) {
	struct body *b = NULL;
	for (int k = 0; k < s->body_count && !b; k++) {
		b = s->bodies[k].alive ? NULL : &s->bodies[k];
	}
	if (!b && s->body_count < BODIES_MAX) {
		b = &s->bodies[s->body_count++];
	}
	if (!b) {
		return NULL;
	}
	*b = (struct body){ .n = n, .mat = mat, .area = area, .alive = true, .perch = -1 };
	// wound so that the normals point out
	double signed_area = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		signed_area += (double)lx[i] * ly[j] - (double)lx[j] * ly[i];
	}
	for (int i = 0; i < n; i++) {
		int k = signed_area >= 0 ? i : n - 1 - i;
		b->lx[i] = lx[k];
		b->ly[i] = ly[k];
	}
	double inertia = 0, r = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		double c = fabs((double)b->lx[i] * b->ly[j] - (double)b->lx[j] * b->ly[i]);
		inertia += c * (b->lx[i] * b->lx[i] + b->lx[i] * b->lx[j] + b->lx[j] * b->lx[j] +
			b->ly[i] * b->ly[i] + b->ly[i] * b->ly[j] + b->ly[j] * b->ly[j]);
		r = fmax(r, hypot(b->lx[i], b->ly[i]));
	}
	double mass = fmax(1, area) * stuffs[mat].density;
	inertia *= stuffs[mat].density / 12;
	b->inv_m = (float)(1 / mass);
	b->inv_i = (float)(1 / fmax(1, inertia));
	b->radius = (float)r;
	b->phase = (float)saver_between(0, 6.3);
	return b;
}

/* The rubble resting in a column band, to find what a falling body can meet. */
static void bucket_add(struct decay *s, int index) {
	struct body *b = &s->bodies[index];
	int from = (int)fmaxf(0, b->x0 / BUCKET), to = (int)fminf(s->bucket_count - 1, b->x1 / BUCKET);
	for (int k = from; k <= to; k++) {
		if (s->bucket_len[k] >= s->bucket_cap[k]) {
			s->bucket_cap[k] = s->bucket_cap[k] ? s->bucket_cap[k] * 2 : 32;
			s->buckets[k] = realloc(s->buckets[k], sizeof(short) * s->bucket_cap[k]);
		}
		s->buckets[k][s->bucket_len[k]++] = (short)index;
	}
}

static void bucket_remove(struct decay *s, int index) {
	for (int k = 0; k < s->bucket_count; k++) {
		for (int i = 0; i < s->bucket_len[k]; i++) {
			if (s->buckets[k][i] == index) {
				s->buckets[k][i] = s->buckets[k][--s->bucket_len[k]];
				i--;
			}
		}
	}
}

/* Its picture, with its broken edges as its material breaks. */
static void finish_picture(struct decay *s, cairo_surface_t *img, const float *px, const float *py,
		int n, float dx, float dy, uint8_t mat) {
	double u = s->u;
	cairo_t *cr = cairo_create(img);
	cairo_new_path(cr);
	for (int i = 0; i < n; i++) {
		cairo_line_to(cr, px[i] + dx, py[i] + dy);
	}
	cairo_close_path(cr);
	cairo_clip_preserve(cr);
	if (mat == GLASS) {
		// glass: a bright edge where the light runs along the break, a sheen across it
		cairo_set_line_width(cr, fmax(1.2, u * 1.8));
		cairo_set_source_rgba(cr, 0.78, 0.96, 0.88, 0.8); // the green of glass seen edgewise
		cairo_stroke(cr);
		int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
		cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, w, h);
		cairo_pattern_add_color_stop_rgba(p, 0.3, 1, 1, 1, 0);
		cairo_pattern_add_color_stop_rgba(p, 0.45, 1, 1, 1, 0.16);
		cairo_pattern_add_color_stop_rgba(p, 0.55, 1, 1, 1, 0);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
	} else {
		// wood: a dark splintered edge; stone: chipped, lighter where fresh
		cairo_set_line_width(cr, fmax(1.5, u * (mat == STONE ? 3 : 2.2)));
		if (mat == STONE) {
			cairo_set_source_rgba(cr, 0.72, 0.7, 0.64, 0.55);
		} else {
			cairo_set_source_rgba(cr, 0.2, 0.13, 0.07, 0.65);
		}
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	cairo_surface_mark_dirty(img);
}

/*
 * Whether a pixel is in a window, by the whole pixels it was given at the
 * start (so that what is handed to it is taken away with it too).
 */
static bool in_window(struct decay *s, struct saver_window *w, int x, int y) {
	return x >= (int)fmax(0, w->x) && x < (int)fmin(s->width, w->x + w->w) &&
		y >= (int)fmax(0, w->y) && y < (int)fmin(s->height, w->y + w->h);
}

/* Takes a piece off: its picture, what shows behind it, and it falls. */
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
	int iw = bx1 - bx0 + 2, ih = by1 - by0 + 2;
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
			dst[(y - by0 + 1) * ds + x - bx0 + 1] = view[i] | 0xff000000u;
			pixels++;
			// what was under it: the window below, intact still, or behind them all
			int below = -1;
			for (int j = (o < s->count ? o : s->count) - 1; j >= 0; j--) {
				if (s->state[j] != GONE && in_window(s, &s->wins[j], x, y)) {
					below = j;
					break;
				}
			}
			s->owner[i] = (short)below;
			s->src[i] = below >= 0 ? (uint8_t)(2 + below) : 1;
			s->mat[i] = material_of(s, below, x, y);
		}
	}
	cairo_surface_mark_dirty(img);
	for (int y = by0; y < by1; y++) {
		update_span(s, y, bx0, bx1);
	}
	cairo_surface_mark_dirty(s->view);
	float lx[POLY_MAX], ly[POLY_MAX];
	for (int i = 0; i < p->n; i++) {
		lx[i] = p->px[i] - p->cx;
		ly[i] = p->py[i] - p->cy;
	}
	struct body *b = pixels >= 4 ? new_body(s, lx, ly, p->n, p->mat, p->area) : NULL;
	if (!b) {
		cairo_surface_destroy(img);
		spray(s, p->mat, p->cx, p->cy, 12, 0.6f);
		return;
	}
	finish_picture(s, img, p->px, p->py, p->n, (float)(1 - bx0), (float)(1 - by0), p->mat);
	b->img = img;
	b->ox = p->cx - bx0 + 1;
	b->oy = p->cy - by0 + 1;
	b->x = p->cx;
	b->y = p->cy;
	b->layer = (uint8_t)(saver_random() * LAYERS);
	double u = s->u;
	// out of the frame: glass tips forward and slides, the frame sags and drops
	b->vx = (float)(saver_between(-20, 20) * u);
	b->vy = (float)(saver_between(-5, 20) * u);
	b->w = (float)saver_between(-0.8, 0.8);
	body_pose(b);
	spray(s, p->mat, p->cx, y1, p->mat == GLASS ? 3 : 6, 0.4f);
}

/* ---------- the physics ---------- */

static void add_contact(struct decay *s, int a, int b, float nx, float ny, float px, float py,
		float pen) {
	if (s->contact_count >= CONTACTS_MAX) {
		return;
	}
	s->contacts[s->contact_count++] = (struct contact){ a, b, nx, ny, px, py, pen, 0, 0, 0, 0, 0, 0 };
}

/* The edge of a with the most separation from b: how much, and which. */
static float max_separation(const struct body *a, const struct body *b, int *edge) {
	float best = -1e9f;
	*edge = 0;
	for (int i = 0; i < a->n; i++) {
		float least = 1e9f;
		for (int j = 0; j < b->n; j++) {
			float d = a->nx[i] * (b->wx[j] - a->wx[i]) + a->ny[i] * (b->wy[j] - a->wy[i]);
			least = fminf(least, d);
		}
		if (least > best) {
			best = least;
			*edge = i;
		}
	}
	return best;
}

/* Two convex bodies: where they overlap, from the face of one they cut into least. */
static void collide_pair(struct decay *s, int ia, int ib) {
	struct body *a = &s->bodies[ia], *b = &s->bodies[ib];
	if (a->layer != b->layer || a->x1 < b->x0 || b->x1 < a->x0 || a->y1 < b->y0 || b->y1 < a->y0) {
		return;
	}
	int ea, eb;
	float sa = max_separation(a, b, &ea);
	if (sa > 0) {
		return;
	}
	float sb = max_separation(b, a, &eb);
	if (sb > 0) {
		return;
	}
	struct body *ref = a, *inc = b;
	int ir = ia, ii = ib, e = ea;
	if (sb > sa * 0.95f + 0.01f) {
		ref = b;
		inc = a;
		ir = ib;
		ii = ia;
		e = eb;
	}
	float nx = ref->nx[e], ny = ref->ny[e];
	int e2 = (e + 1) % ref->n;
	float v1x = ref->wx[e], v1y = ref->wy[e], v2x = ref->wx[e2], v2y = ref->wy[e2];
	// the edge of the other facing it most
	int j = 0;
	float most = 1e9f;
	for (int k = 0; k < inc->n; k++) {
		float d = inc->nx[k] * nx + inc->ny[k] * ny;
		if (d < most) {
			most = d;
			j = k;
		}
	}
	int j2 = (j + 1) % inc->n;
	float px[2] = { inc->wx[j], inc->wx[j2] }, py[2] = { inc->wy[j], inc->wy[j2] };
	float tx = v2x - v1x, ty = v2y - v1y, tl = fmaxf(1e-4f, hypotf(tx, ty));
	tx /= tl;
	ty /= tl;
	// clipped to the sides of the face
	for (int side = 0; side < 2; side++) {
		float ox = side ? -tx : tx, oy = side ? -ty : ty;
		float off = side ? -(tx * v2x + ty * v2y) : tx * v1x + ty * v1y;
		float d0 = ox * px[0] + oy * py[0] - off, d1 = ox * px[1] + oy * py[1] - off;
		if (d0 < 0 && d1 < 0) {
			return;
		}
		if (d0 < 0 || d1 < 0) {
			float k = d0 / (d0 - d1);
			float cx = px[0] + (px[1] - px[0]) * k, cy = py[0] + (py[1] - py[0]) * k;
			if (d0 < 0) {
				px[0] = cx;
				py[0] = cy;
			} else {
				px[1] = cx;
				py[1] = cy;
			}
		}
	}
	for (int k = 0; k < 2; k++) {
		float sep = nx * (px[k] - v1x) + ny * (py[k] - v1y);
		if (sep < 0) {
			add_contact(s, ir, ii, nx, ny, px[k], py[k], -sep);
		}
	}
}

/* Where a falling body meets the ground, the sides of the screen, a window still standing. */
static void collide_world(struct decay *s, int index) {
	struct body *b = &s->bodies[index];
	double u = s->u;
	for (int i = 0; i < b->n; i++) {
		float x = b->wx[i], y = b->wy[i];
		float floor = ground(s, x);
		if (y > floor) {
			add_contact(s, -1, index, 0, -1, x, y, y - floor);
		}
		if (x < 0) {
			add_contact(s, -1, index, 1, 0, x, y, -x);
		} else if (x > s->width) {
			add_contact(s, -1, index, -1, 0, x, y, x - s->width);
		}
		// the tops of windows still whole, met from above only
		for (int j = 0; j < s->count; j++) {
			struct saver_window *w = &s->wins[j];
			if (s->state[j] == INTACT && x >= w->x && x < w->x + w->w && y > w->y &&
					y < w->y + u * 14 && b->y < w->y - b->radius * 0.2f) {
				add_contact(s, -2 - j, index, 0, -1, x, y, y - (float)w->y);
			}
		}
	}
}

static void prepare_contacts(struct decay *s) {
	double u = s->u;
	for (int k = 0; k < s->contact_count; k++) {
		struct contact *c = &s->contacts[k];
		struct body *a = c->a >= 0 ? &s->bodies[c->a] : NULL, *b = &s->bodies[c->b];
		bool fa = !a || a->asleep || a->perch >= 0, fb = b->asleep || b->perch >= 0;
		float ima = fa ? 0 : a->inv_m, iia = fa ? 0 : a->inv_i;
		float imb = fb ? 0 : b->inv_m, iib = fb ? 0 : b->inv_i;
		float rax = a ? c->px - a->x : 0, ray = a ? c->py - a->y : 0;
		float rbx = c->px - b->x, rby = c->py - b->y;
		float tx = -c->ny, ty = c->nx;
		float rna = cross2(rax, ray, c->nx, c->ny), rnb = cross2(rbx, rby, c->nx, c->ny);
		float rta = cross2(rax, ray, tx, ty), rtb = cross2(rbx, rby, tx, ty);
		c->kn = ima + imb + iia * rna * rna + iib * rnb * rnb;
		c->kt = ima + imb + iia * rta * rta + iib * rtb * rtb;
		// how hard they come together: a bounce back, and a blow that may break them
		float vax = a ? a->vx - a->w * ray : 0, vay = a ? a->vy + a->w * rax : 0;
		float vbx = b->vx - b->w * rby, vby = b->vy + b->w * rbx;
		float vn = (vbx - vax) * c->nx + (vby - vay) * c->ny;
		uint8_t ma = a ? a->mat : STONE, mb = b->mat;
		float bounce = fmaxf(stuffs[mb].bounce, a ? stuffs[ma].bounce : 0);
		c->bias = vn < -u * 45 ? -bounce * vn : 0;
		c->mu = sqrtf(stuffs[mb].friction * (a ? stuffs[ma].friction : 0.7f));
		c->jn = c->jt = 0;
		if (-vn > b->hit) {
			b->hit = -vn;
			b->hit_x = c->px;
			b->hit_y = c->py;
		}
		if (a && -vn > a->hit) {
			a->hit = -vn;
			a->hit_x = c->px;
			a->hit_y = c->py;
		}
	}
}

static void apply(struct body *b, float jx, float jy, float rx, float ry) {
	if (!b || b->asleep || b->perch >= 0) {
		return;
	}
	b->vx += jx * b->inv_m;
	b->vy += jy * b->inv_m;
	b->w += b->inv_i * cross2(rx, ry, jx, jy);
}

static void solve_contacts(struct decay *s) {
	for (int it = 0; it < 8; it++) {
		for (int k = 0; k < s->contact_count; k++) {
			struct contact *c = &s->contacts[k];
			struct body *a = c->a >= 0 ? &s->bodies[c->a] : NULL, *b = &s->bodies[c->b];
			float rax = a ? c->px - a->x : 0, ray = a ? c->py - a->y : 0;
			float rbx = c->px - b->x, rby = c->py - b->y;
			float vax = a ? a->vx - a->w * ray : 0, vay = a ? a->vy + a->w * rax : 0;
			float vbx = b->vx - b->w * rby, vby = b->vy + b->w * rbx;
			float vn = (vbx - vax) * c->nx + (vby - vay) * c->ny;
			float dj = (c->bias - vn) / fmaxf(1e-9f, c->kn);
			float old = c->jn;
			c->jn = fmaxf(0, old + dj);
			dj = c->jn - old;
			apply(a, -dj * c->nx, -dj * c->ny, rax, ray);
			apply(b, dj * c->nx, dj * c->ny, rbx, rby);
			// friction along the surface, no more than the push allows
			float tx = -c->ny, ty = c->nx;
			vax = a ? a->vx - a->w * ray : 0;
			vay = a ? a->vy + a->w * rax : 0;
			vbx = b->vx - b->w * rby;
			vby = b->vy + b->w * rbx;
			float vt = (vbx - vax) * tx + (vby - vay) * ty;
			float djt = -vt / fmaxf(1e-9f, c->kt);
			float oldt = c->jt, most = c->mu * c->jn;
			c->jt = fmaxf(-most, fminf(most, oldt + djt));
			djt = c->jt - oldt;
			apply(a, -djt * tx, -djt * ty, rax, ray);
			apply(b, djt * tx, djt * ty, rbx, rby);
		}
	}
}

/* Pushes what still overlaps apart, gently, so nothing sinks in. */
static void separate(struct decay *s) {
	float slop = (float)fmax(0.3, s->u * 0.4);
	for (int k = 0; k < s->contact_count; k++) {
		struct contact *c = &s->contacts[k];
		struct body *a = c->a >= 0 && !s->bodies[c->a].asleep && s->bodies[c->a].perch < 0 ?
			&s->bodies[c->a] : NULL;
		struct body *b = s->bodies[c->b].asleep || s->bodies[c->b].perch >= 0 ? NULL : &s->bodies[c->b];
		float ima = a ? a->inv_m : 0, imb = b ? b->inv_m : 0;
		if (ima + imb <= 0 || c->pen <= slop) {
			continue;
		}
		float move = (c->pen - slop) * 0.45f / (ima + imb);
		if (a) {
			a->x -= c->nx * move * ima;
			a->y -= c->ny * move * ima;
		}
		if (b) {
			b->x += c->nx * move * imb;
			b->y += c->ny * move * imb;
		}
	}
}

static void settle(struct decay *s, int index);
static void shatter(struct decay *s, int index);
static void draw_side(struct decay *s, cairo_t *cr, struct body *b);
static void draw_ledge_dust(struct decay *s, cairo_t *cr);
static void draw_webs(struct decay *s, cairo_t *cr);

/* One step of all that falls: pulled down, colliding, pushed apart, moved on. */
static void physics_step(struct decay *s, double h) {
	double u = s->u;
	float g = (float)(u * 1500);
	s->contact_count = 0;
	// the falling ones in order across the screen: each meets only those it reaches
	static short order[BODIES_MAX];
	int n = 0;
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive || b->asleep) {
			continue;
		}
		int at = n++; // (perched ones too: they are in the way)
		while (at > 0 && s->bodies[order[at - 1]].x0 > b->x0) {
			order[at] = order[at - 1];
			at--;
		}
		order[at] = (short)k;
	}
	for (int i = 0; i < n; i++) {
		struct body *b = &s->bodies[order[i]];
		for (int j = i + 1; j < n && s->bodies[order[j]].x0 <= b->x1; j++) {
			if (b->perch < 0 || s->bodies[order[j]].perch < 0) {
				collide_pair(s, order[i], order[j]);
			}
		}
	}
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive || b->asleep || b->perch >= 0) {
			continue;
		}
		b->vy += g * (float)h;
		b->w *= 1 - (float)(0.4 * h); // the air
		collide_world(s, k);
		int from = (int)fmaxf(0, b->x0 / BUCKET), to = (int)fminf(s->bucket_count - 1, b->x1 / BUCKET);
		for (int c = from; c <= to; c++) {
			for (int i = 0; i < s->bucket_len[c]; i++) {
				int j = s->buckets[c][i];
				// (met once: in the first band both are in)
				int first = (int)fmaxf(0, s->bodies[j].x0 / BUCKET);
				if (c == (first > from ? first : from)) {
					collide_pair(s, j, k);
				}
			}
		}
	}
	prepare_contacts(s);
	solve_contacts(s);
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive || b->asleep || b->perch >= 0) {
			continue;
		}
		b->x += b->vx * (float)h;
		b->y += b->vy * (float)h;
		b->a += b->w * (float)h;
	}
	separate(s);
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (b->alive && !b->asleep && b->perch < 0) {
			body_pose(b);
		}
	}
}

static void physics(struct decay *s, double dt) {
	int steps = (int)ceil(dt / STEP_MAX);
	steps = steps < 1 ? 1 : steps > 5 ? 5 : steps;
	// what holds each up: the ground or the rubble (it may settle there for good), or a
	// window standing, or something lying on one (then it may only perch)
	static bool grounded[BODIES_MAX];
	static short held[BODIES_MAX];
	for (int k = 0; k < s->body_count; k++) {
		s->bodies[k].hit = 0;
		grounded[k] = false;
		held[k] = -1;
	}
	for (int i = 0; i < steps; i++) {
		physics_step(s, dt / steps);
		for (int k = 0; k < s->contact_count; k++) {
			struct contact *c = &s->contacts[k];
			if (c->a == -1) {
				grounded[c->b] = true;
			} else if (c->a <= -2) {
				held[c->b] = (short)(-2 - c->a);
			} else {
				struct body *a = &s->bodies[c->a], *b = &s->bodies[c->b];
				if (a->asleep) {
					grounded[c->b] = true;
				} else if (a->perch >= 0) {
					held[c->b] = (short)a->perch;
				}
				if (b->asleep) {
					grounded[c->a] = true;
				} else if (b->perch >= 0) {
					held[c->a] = (short)b->perch;
				}
			}
		}
	}
	double u = s->u;
	int count = s->body_count;
	for (int k = 0; k < count; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive || b->asleep) {
			continue;
		}
		const struct stuff *m = &stuffs[b->mat];
		float across = sqrtf(b->area);
		// glass may break twice over, wood and stone once
		if (b->hit > m->breaks * u && across > m->smallest * u &&
				b->generation < (b->mat == GLASS ? 2 : 1)) {
			shatter(s, k);
			continue;
		}
		if (b->hit > u * 140) {
			spray(s, b->mat, b->hit_x, b->hit_y, 2 + (int)(b->hit / (u * 120)), 0.5f);
			if (b->mat == STONE && b->area > (u * 25) * (u * 25)) {
				s->shake = fmax(s->shake, fmin(1, b->hit / (u * 900)));
			}
		}
		if (b->perch >= 0) {
			continue;
		}
		bool slow = fabsf(b->vx) < u * 20 && fabsf(b->vy) < u * 20 && fabsf(b->w) < 0.4f;
		b->still = (grounded[k] || held[k] >= 0) && slow ? b->still + (float)dt : 0;
		if (b->still > 0.3f && grounded[k]) {
			settle(s, k);
		} else if (b->still > 0.3f && held[k] >= 0) {
			b->perch = held[k]; // lying on a window: until it goes
			b->vx = b->vy = b->w = 0;
		} else if (b->y - b->radius > s->height + 100 || !isfinite(b->x) || !isfinite(b->y)) {
			if (b->img) {
				cairo_surface_destroy(b->img);
			}
			b->img = NULL;
			b->alive = false;
			s->resting++;
		}
	}
}

/* A body at rest joins the rubble for good: drawn into it, and in the way of the rest. */
static void settle(struct decay *s, int index) {
	struct body *b = &s->bodies[index];
	cairo_t *cr = cairo_create(s->rubble);
	draw_side(s, cr, b);
	cairo_translate(cr, b->x, b->y);
	cairo_rotate(cr, b->a);
	cairo_set_source_surface(cr, b->img, -b->ox, -b->oy);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(b->img);
	b->img = NULL;
	b->asleep = true;
	b->vx = b->vy = b->w = 0;
	s->resting++;
	s->rubble_top = fminf(s->rubble_top, b->y0);
	for (int c = (int)fmaxf(0, b->x0); c < (int)fminf(s->width, b->x1); c++) {
		s->heap[c] = fmaxf(s->heap[c], s->height - b->y0);
	}
	// too much rubble to keep track of: the lowest piece is buried anyway
	int resting = 0, lowest = -1;
	for (int k = 0; k < s->body_count; k++) {
		if (s->bodies[k].alive && s->bodies[k].asleep) {
			resting++;
			if (lowest < 0 || s->bodies[k].y > s->bodies[lowest].y) {
				lowest = k;
			}
		}
	}
	if (resting > RESTING_MAX && lowest >= 0 && lowest != index) {
		s->bodies[lowest].alive = false;
		bucket_remove(s, lowest);
	}
	bucket_add(s, index);
}

/*
 * A body that hit hard breaks as its material does: glass into shards from
 * where it hit and a spray of splinters, wood along its length into splinters
 * and fibres, stone into chunks in a puff of dust.
 */
static void shatter(struct decay *s, int index) {
	struct body parent = s->bodies[index];
	struct body *old = &s->bodies[index];
	old->alive = false;
	old->img = NULL;
	s->shattered++;
	double u = s->u;
	float ca = cosf(parent.a), sa = sinf(parent.a);
	// where it hit, in its own frame
	float hx = parent.hit_x - parent.x, hy = parent.hit_y - parent.y;
	float ix = hx * ca + hy * sa, iy = -hx * sa + hy * ca;
	struct cutting *c = malloc(sizeof(*c));
	c->count = 1;
	c->n[0] = parent.n;
	memcpy(c->x[0], parent.lx, sizeof(float) * parent.n);
	memcpy(c->y[0], parent.ly, sizeof(float) * parent.n);
	if (parent.mat == GLASS) {
		float px = ix * 0.8f, py = iy * 0.8f;
		int lines = 2 + (int)(saver_random() * 3);
		double base = saver_between(0, M_PI);
		for (int k = 0; k < lines; k++) {
			double a = base + k * M_PI / lines + saver_between(-0.2, 0.2);
			cut_all(s, c, px, py, (float)-sin(a), (float)cos(a), 0, 0, -2, GLASS);
		}
	} else if (parent.mat == WOOD) {
		// along its longest edge: the grain
		float best = 0, gx = 1, gy = 0;
		for (int i = 0; i < parent.n; i++) {
			int j = (i + 1) % parent.n;
			float dx = parent.lx[j] - parent.lx[i], dy = parent.ly[j] - parent.ly[i], d = hypotf(dx, dy);
			if (d > best) {
				best = d;
				gx = dx / d;
				gy = dy / d;
			}
		}
		cut_all(s, c, (float)saver_between(-2, 2) * (float)u, (float)saver_between(-2, 2) * (float)u,
			-gy, gx, 0, 0, -2, WOOD);
		if (saver_random() < 0.5) {
			cut_all(s, c, ix * 0.5f, iy * 0.5f, gx + (float)saver_between(-0.3, 0.3), gy, 0, 0, -2, WOOD);
		}
	} else {
		int lines = 1 + (int)(saver_random() * 2);
		for (int k = 0; k < lines; k++) {
			double a = saver_between(0, M_PI);
			cut_all(s, c, (float)saver_between(-0.3, 0.3) * parent.radius,
				(float)saver_between(-0.3, 0.3) * parent.radius, (float)cos(a), (float)sin(a), 0, 0, -2,
				STONE);
		}
	}
	for (int k = 0; k < c->count; k++) {
		float cx, cy;
		float area = poly_area(c->x[k], c->y[k], c->n[k], &cx, &cy);
		if (sqrtf(area) < u * 6) {
			spray(s, parent.mat, parent.x + cx * ca - cy * sa, parent.y + cx * sa + cy * ca, 3, 0.8f);
			continue;
		}
		float lx[POLY_MAX], ly[POLY_MAX];
		for (int i = 0; i < c->n[k]; i++) {
			lx[i] = c->x[k][i] - cx;
			ly[i] = c->y[k][i] - cy;
		}
		struct body *f = new_body(s, lx, ly, c->n[k], parent.mat, area);
		if (!f) {
			spray(s, parent.mat, parent.x, parent.y, 6, 0.8f);
			continue;
		}
		// its picture: the part of the whole one inside it
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		for (int i = 0; i < c->n[k]; i++) {
			x0 = fminf(x0, c->x[k][i]);
			x1 = fmaxf(x1, c->x[k][i]);
			y0 = fminf(y0, c->y[k][i]);
			y1 = fmaxf(y1, c->y[k][i]);
		}
		int iw = (int)ceilf(x1 - x0) + 3, ih = (int)ceilf(y1 - y0) + 3;
		cairo_surface_t *part = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
		cairo_t *cr = cairo_create(part);
		cairo_translate(cr, -x0 + 1, -y0 + 1);
		cairo_new_path(cr);
		for (int i = 0; i < c->n[k]; i++) {
			cairo_line_to(cr, c->x[k][i], c->y[k][i]);
		}
		cairo_close_path(cr);
		cairo_clip(cr);
		cairo_set_source_surface(cr, parent.img, -parent.ox, -parent.oy);
		cairo_paint(cr);
		cairo_destroy(cr);
		finish_picture(s, part, c->x[k], c->y[k], c->n[k], -x0 + 1, -y0 + 1, parent.mat);
		f->img = part;
		f->ox = cx - x0 + 1;
		f->oy = cy - y0 + 1;
		f->x = parent.x + cx * ca - cy * sa;
		f->y = parent.y + cx * sa + cy * ca;
		f->a = parent.a;
		f->generation = parent.generation + 1;
		f->layer = saver_random() < 0.7 ? parent.layer : (uint8_t)(saver_random() * LAYERS);
		// moving on as the whole moved there, flung apart from where it hit
		float rx = f->x - parent.x, ry = f->y - parent.y;
		f->vx = parent.vx - parent.w * ry;
		f->vy = parent.vy + parent.w * rx;
		float dx = f->x - parent.hit_x, dy = f->y - parent.hit_y, d = fmaxf(0.01f, hypotf(dx, dy));
		float fling = (float)(u * (parent.mat == GLASS ? saver_between(60, 170) :
			parent.mat == WOOD ? saver_between(30, 90) : saver_between(15, 50)));
		f->vx += dx / d * fling;
		f->vy = fminf(f->vy, 0) * 0 + dy / d * fling - fabsf(parent.vy) * stuffs[parent.mat].bounce;
		f->w = parent.w + (float)saver_between(-5, 5) * (parent.mat == GLASS ? 1.5f : 0.6f);
		body_pose(f);
	}
	free(c);
	spray(s, parent.mat, parent.hit_x, parent.hit_y,
		parent.mat == GLASS ? 26 : parent.mat == WOOD ? 14 : 18, 1.1f);
	if (parent.mat != GLASS) {
		s->shake = fmax(s->shake, fmin(1, parent.area / ((u * 60) * (u * 60)) * 0.6));
	}
	cairo_surface_destroy(parent.img);
}

/* ---------- the slow parts ---------- */

static void draw_cracks(struct decay *s, cairo_t *cr) {
	double u = s->u, t = s->t;
	int W = s->width, H = s->height;
	static const double colour[4][4] = {
		[WALL] = { 0.1, 0.07, 0.04, 0.85 }, [GLASS] = { 0.03, 0.03, 0.04, 0.55 },
		[WOOD] = { 0.12, 0.07, 0.03, 0.85 }, [STONE] = { 0.08, 0.07, 0.06, 0.85 },
	};
	static const double width[4] = { 1.1, 0.9, 1.4, 1.8 };
	static const double jag[4] = { 1.2, 0.4, 1.2, 3 };
	for (int m = WALL; m <= STONE; m++) {
		cairo_new_path(cr);
		for (int k = 0; k < s->crack_count; k++) {
			struct crack *c = &s->cracks[k];
			if (c->mat != m || c->at > t) {
				continue;
			}
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
			float f = c->grow > 0 ? fminf(1, (float)(t - c->at) / c->grow) : 1;
			int steps = (int)fmaxf(2, len / (float)(u * 4));
			cairo_move_to(cr, c->x0, c->y0);
			for (int i = 1; i <= steps; i++) {
				float k2 = (float)i / steps * f;
				float j = i == steps && f >= 1 ? 0 :
					((hash2((int)c->seed, i, 5) & 255) / 255.0f - 0.5f) * (float)(u * jag[m]);
				cairo_line_to(cr, c->x0 + (c->x1 - c->x0) * k2 + nx * j, c->y0 + (c->y1 - c->y0) * k2 + ny * j);
			}
		}
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		if (m == GLASS) {
			// glass: the break catches the light, with a dark shadow of it beside
			cairo_save(cr);
			cairo_translate(cr, u * 0.8, u * 0.8);
			cairo_set_line_width(cr, fmax(0.6, u * width[m]));
			cairo_set_source_rgba(cr, colour[m][0], colour[m][1], colour[m][2], colour[m][3]);
			cairo_stroke_preserve(cr);
			cairo_restore(cr);
			cairo_set_line_width(cr, fmax(0.6, u * width[m]));
			cairo_set_source_rgba(cr, 0.95, 0.97, 1, 0.75);
			cairo_stroke(cr);
			continue;
		}
		cairo_save(cr);
		cairo_translate(cr, u * 0.7, u * 0.7);
		cairo_set_line_width(cr, fmax(0.6, u * 0.8));
		cairo_set_source_rgba(cr, 1, 0.95, 0.82, 0.18); // the lit lip of it
		cairo_stroke_preserve(cr);
		cairo_restore(cr);
		cairo_set_line_width(cr, fmax(0.8, u * width[m]));
		cairo_set_source_rgba(cr, colour[m][0], colour[m][1], colour[m][2], colour[m][3]);
		cairo_stroke(cr);
	}
	// gaps opening where a piece works loose
	cairo_new_path(cr);
	for (int k = 0; k < s->piece_count; k++) {
		struct piece *p = &s->pieces[k];
		if (p->fallen || t < p->fall_at - s->span * 0.03) {
			continue;
		}
		cairo_move_to(cr, p->px[0], p->py[0]);
		for (int i = 1; i < p->n; i++) {
			cairo_line_to(cr, p->px[i], p->py[i]);
		}
		cairo_close_path(cr);
	}
	cairo_set_line_width(cr, fmax(1, u * 1.6));
	cairo_set_source_rgba(cr, 0.05, 0.03, 0.02, 0.5);
	cairo_stroke(cr);
}

/* The rubble, and the dust drifting over it. */
static void draw_ground(struct decay *s, cairo_t *cr) {
	double W = s->width, H = s->height;
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
		draw_ledge_dust(s, cr);
		draw_webs(s, cr);
	}
	if (part < 0 || part == 3) {
		double P = progress(s);
		if (P > 0.82 && part >= 0) {
			// the rubble greys and crumbles away into the dust
			// (this part is built five times a second: gone to a trace by 1.02)
			cairo_t *rc = cairo_create(s->rubble);
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
		cairo_surface_t *shown = s->scene;
		s->scene = s->next;
		s->next = shown;
	}
	cairo_destroy(cr);
}

/* A cobweb spun into a corner: threads fanning out, the spiral sagging between them. */
static cairo_surface_t *make_web(int size) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	int threads = 7;
	double angle[9];
	for (int k = 0; k < threads; k++) {
		angle[k] = (M_PI / 2) * (k + saver_between(0.1, 0.4)) / (threads - 0.5);
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, cos(angle[k]) * size, sin(angle[k]) * size);
	}
	for (double r = size * 0.1; r < size * 0.95; r *= saver_between(1.12, 1.22)) {
		for (int k = 0; k + 1 < threads; k++) {
			double a0 = angle[k], a1 = angle[k + 1];
			double x0 = cos(a0) * r, y0 = sin(a0) * r, x1 = cos(a1) * r, y1 = sin(a1) * r;
			double mx = cos((a0 + a1) / 2) * r * 0.86, my = sin((a0 + a1) / 2) * r * 0.86;
			cairo_move_to(cr, x0, y0);
			cairo_curve_to(cr, mx, my, mx, my, x1, y1);
		}
	}
	cairo_set_line_width(cr, fmax(0.5, size / 220.0));
	cairo_set_source_rgba(cr, 0.92, 0.9, 0.86, 0.5);
	cairo_stroke(cr);
	// a few strands torn loose, hanging
	for (int k = 0; k < 3; k++) {
		double x = size * saver_between(0.2, 0.7);
		cairo_move_to(cr, x, x * saver_between(0.1, 0.4));
		cairo_curve_to(cr, x + size * 0.05, size * 0.5, x - size * 0.03, size * 0.6, x, size * 0.75);
	}
	cairo_set_source_rgba(cr, 0.92, 0.9, 0.86, 0.25);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return img;
}

/* Where cobwebs will be spun: the upper corners of the glass of windows, the screen's corners. */
static void make_webs(struct decay *s) {
	double u = s->u;
	s->web_img = make_web((int)fmax(16, u * 160));
	for (int o = 0; o < s->count && s->web_count < WEBS_MAX - 4; o++) {
		float x0, y0, x1, y1;
		glass_of(s, o, &x0, &y0, &x1, &y1);
		if (x1 - x0 < u * 60 || y1 - y0 < u * 60) {
			continue;
		}
		for (int c = 0; c < 4; c++) {
			if (saver_random() < (c < 2 ? 0.6 : 0.3)) {
				s->webs[s->web_count++] = (struct web){ c == 0 || c == 3 ? x0 : x1, c < 2 ? y0 : y1,
					(float)(u * saver_between(35, 90)), (float)(s->span * saver_between(0.15, 0.4)), c, o };
			}
		}
	}
	for (int c = 0; c < 4 && s->web_count < WEBS_MAX; c++) {
		s->webs[s->web_count++] = (struct web){ c == 0 || c == 3 ? 0 : (float)s->width,
			c < 2 ? 0 : (float)s->height, (float)(u * saver_between(90, 170)),
			(float)(s->span * saver_between(0.1, 0.35)), c, -1 };
	}
}

static void draw_webs(struct decay *s, cairo_t *cr) {
	double P = progress(s);
	int W = s->width, size = cairo_image_surface_get_width(s->web_img);
	for (int k = 0; k < s->web_count; k++) {
		struct web *w = &s->webs[k];
		double grown = smooth(w->at, w->at + (float)(s->span * 0.12), (float)s->t);
		if (grown <= 0.01) {
			continue;
		}
		// a window's web goes when the glass there falls
		if (w->owner >= 0) {
			int x = (int)(w->x + (w->corner == 0 || w->corner == 3 ? 3 : -4));
			int y = (int)(w->y + (w->corner < 2 ? 3 : -4));
			if (x < 0 || y < 0 || x >= W || y >= s->height || s->owner[y * W + x] != w->owner) {
				continue;
			}
		}
		double fade = 1 - smooth(0.95f, 1.15f, (float)P); // gone into the dust at the end
		cairo_save(cr);
		cairo_translate(cr, w->x, w->y);
		cairo_rotate(cr, w->corner * M_PI / 2);
		double k2 = w->size * (0.4 + 0.6 * grown) / size;
		cairo_scale(cr, k2, k2);
		cairo_set_source_surface(cr, s->web_img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		cairo_paint_with_alpha(cr, grown * fade);
		cairo_restore(cr);
	}
}

/* Dust gathered on the tops of the windows and the taskbar, where they still stand. */
static void draw_ledge_dust(struct decay *s, cairo_t *cr) {
	double u = s->u, P = progress(s);
	double deep = u * 5 * smooth(0.12f, 0.75f, (float)P);
	if (deep < 0.4) {
		return;
	}
	int W = s->width;
	cairo_new_path(cr);
	for (int o = 0; o < s->count + s->bar_count; o++) {
		if (s->state[o] == GONE) {
			continue;
		}
		struct saver_window *w = o < s->count ? &s->wins[o] : &s->bars[o - s->count];
		int y = (int)w->y;
		if (y < 2 || y + 2 >= s->height) {
			continue;
		}
		for (int x = (int)fmax(0, w->x); x < (int)fmin(W, w->x + w->w); x += 2) {
			if (s->owner[(y + 2) * W + x] != o) {
				continue;
			}
			double h = deep * (0.5 + noise(x / (float)(u * 18), (float)o, 0, 151));
			cairo_rectangle(cr, x, y - h, 2, h + 1);
		}
	}
	cairo_set_source_rgba(cr, 0.64, 0.6, 0.52, 0.85);
	cairo_fill(cr);
}

/* A soft round of dust, for the clouds breaking things throws up. */
static cairo_surface_t *make_puff(int size) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	double c = size / 2.0;
	cairo_pattern_t *p = cairo_pattern_create_radial(c, c, 0, c, c, c);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.74, 0.68, 0.58, 0.7);
	cairo_pattern_add_color_stop_rgba(p, 0.5, 0.7, 0.64, 0.54, 0.35);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.7, 0.64, 0.54, 0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	cairo_destroy(cr);
	return img;
}

/* Shafts of light from a high window, drawn once; they drift by sliding the picture. */
static cairo_surface_t *make_rays(int width, int height) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(img);
	double W = width, H = height;
	for (int k = 0; k < 3; k++) {
		double x = W * (0.2 + 0.28 * k), w = W * (0.05 + 0.03 * k), slant = H * 0.45;
		cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + slant, H);
		cairo_pattern_add_color_stop_rgba(p, 0, 1, 0.9, 0.7, 0.12);
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
	cairo_destroy(cr);
	return img;
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
	for (int o = 0; o < s->count; o++) {
		s->frame_mat[o] = saver_random() < 0.55 ? WOOD : STONE;
		s->edge[o] = (float)fmax(s->wins[o].border, round(u * 4));
	}
	size_t n = (size_t)width * height;
	s->owner = malloc(sizeof(short) * n);
	s->src = calloc(n, 1);
	s->mat = calloc(n, 1);
	s->age_n = malloc(n);
	s->stain_n = malloc(n);
	s->grain_n = malloc(n);
	s->wood_n = calloc(n, 1);
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
			s->mat[i] = material_of(s, s->owner[i], x, y);
			if (s->mat[i] == WOOD) {
				// the grain: long wavy lines along the frame
				struct saver_window *w = &s->wins[s->owner[i]];
				bool lying = y < w->y + w->title_h || y >= w->y + w->h - s->edge[s->owner[i]];
				float along = lying ? (float)x : (float)y, across = lying ? (float)y : (float)x;
				float wave = noise(along / (float)(u * 110), across / (float)(u * 14), 0, 51) * 6 +
					noise(along / (float)(u * 30), across / (float)(u * 6), 0, 52) * 1.5f;
				float g = 0.5f + 0.5f * sinf((across / (float)fmax(1, u * 2.6) + wave) * 2.2f);
				s->wood_n[i] = (uint8_t)(g * 200 + (hash2(x, y, 3) & 55));
			}
		}
	}
	free(ag.v);
	free(sg.v);
	// the wall: where plaster flakes off, where mould grows, where dirt runs down
	s->peel_n = malloc(n);
	s->mould_n = malloc(n);
	s->dirt = calloc(n, 1);
	struct grid pg, mg;
	grid_make(&pg, width, height, 4, (float)fmax(4, u * 70), 131);
	grid_make(&mg, width, height, 4, (float)fmax(4, u * 25), 137);
	float *streak = malloc(sizeof(float) * width);
	for (int x = 0; x < width; x++) {
		// a few runs, uneven in strength
		float k = noise(x / (float)fmax(1, u * 3.5), 0.5f, 0, 139) * 0.55f +
			noise(x / (float)fmax(1, u * 40), 1.5f, 0, 141) * 0.45f;
		streak[x] = smooth(0.62f, 0.9f, k);
	}
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t i = (size_t)y * width + x;
			float pn = grid_at(&pg, x, y);
			s->peel_n[i] = (uint8_t)fminf(255, fmaxf(0, (pn - 0.3f) * 1.8f * 255));
			s->mould_n[i] = (uint8_t)fminf(255, grid_at(&mg, x, y) * 255);
			// settled low down and in the corners
			float low = smooth(0.55f, 1, (float)y / height) * 0.35f;
			float corner = (1 - smooth(0, 0.12f, fminf((float)x / width, 1 - (float)x / width))) * 0.2f;
			s->dirt[i] = (uint8_t)fminf(255, (low + corner) * 255);
		}
	}
	// under each window, what the rain washed off it runs down in streaks
	for (int o = 0; o < s->count; o++) {
		struct saver_window *w = &s->wins[o];
		int y0 = (int)fmax(0, w->y + w->h);
		for (int y = y0; y < height; y++) {
			float fade = expf(-(y - y0) / (float)(u * 160));
			if (fade < 0.02f) {
				break;
			}
			// each run wavers as it goes down, and some stop sooner
			int wave = (int)(noise((float)(y - y0) / (float)(u * 30), 0.5f, 0, 143) * u * 6);
			for (int x = (int)fmax(0, w->x); x < (int)fmin(width, w->x + w->w); x++) {
				int from = x + wave;
				if (from < 0 || from >= width) {
					continue;
				}
				float reach = 0.4f + 0.6f * (hash2(from / 3, 0, 145) & 255) / 255.0f;
				float f = expf(-(y - y0) / (float)(u * 160 * reach));
				size_t i = (size_t)y * width + x;
				s->dirt[i] = (uint8_t)fminf(255, s->dirt[i] + streak[from] * f * 150);
			}
		}
	}
	free(streak);
	free(pg.v);
	free(mg.v);
	s->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(s->view);
	cairo_set_source_surface(cr, s->orig, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	s->scene = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->next = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->rubble = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->rays = make_rays(width, height);
	s->puff = make_puff((int)fmax(8, u * 24));
	s->heap = calloc(width, sizeof(float));
	s->dust = calloc(width, sizeof(float));
	s->dune = malloc(sizeof(float) * width);
	for (int x = 0; x < width; x++) {
		s->dune[x] = fbm(x / (float)fmax(4, u * 260), 0.5f, 0, 91, 3);
	}
	s->rubble_top = (float)height;
	s->bodies = calloc(BODIES_MAX, sizeof(struct body));
	s->contacts = malloc(sizeof(struct contact) * CONTACTS_MAX);
	s->bucket_count = width / BUCKET + 1;
	s->buckets = calloc(s->bucket_count, sizeof(short *));
	s->bucket_len = calloc(s->bucket_count, sizeof(int));
	s->bucket_cap = calloc(s->bucket_count, sizeof(int));
	make_pieces(s);
	for (int k = 0; k < 14; k++) {
		crack_wall(s, (float)(saver_random() * width), (float)(saver_random() * height * 0.8),
			(float)saver_between(0, 2 * M_PI), (float)(u * saver_between(80, 280)),
			(float)(s->span * saver_between(0.2, 0.65)), 0);
	}
	make_webs(s);
	for (int i = 0; i < MOTES_MAX; i++) {
		s->motes[i] = (struct mote){ (float)(saver_random() * width), (float)(saver_random() * height),
			(float)saver_between(0, 6.3), (float)(u * saver_between(0.8, 2.2)) };
	}
	s->light = 1;
	build_scene(s, -1);
	return s;
}

/* The side of a piece, its thickness, showing below and right of it as it turns. */
static void draw_side(struct decay *s, cairo_t *cr, struct body *b) {
	static const double side[4][3] = { [WALL] = { 0.3, 0.28, 0.24 }, [GLASS] = { 0.5, 0.72, 0.64 },
		[WOOD] = { 0.3, 0.19, 0.1 }, [STONE] = { 0.36, 0.34, 0.3 } };
	double d = s->u * (b->mat == GLASS ? 1.3 : 2.6);
	cairo_save(cr);
	cairo_translate(cr, b->x + d * 0.6, b->y + d);
	cairo_rotate(cr, b->a);
	cairo_set_source_rgba(cr, side[b->mat][0], side[b->mat][1], side[b->mat][2], 0.95);
	cairo_mask_surface(cr, b->img, -b->ox, -b->oy);
	cairo_restore(cr);
}

static void draw_body(struct decay *s, cairo_t *cr, struct body *b) {
	double u = s->u;
	// its shadow on the wall behind, then it
	cairo_save(cr);
	cairo_translate(cr, b->x + u * 7, b->y + u * 9);
	cairo_rotate(cr, b->a);
	cairo_set_source_rgba(cr, 0.05, 0.04, 0.03, b->mat == GLASS ? 0.16 : 0.3);
	cairo_mask_surface(cr, b->img, -b->ox, -b->oy);
	cairo_restore(cr);
	draw_side(s, cr, b);
	cairo_save(cr);
	cairo_translate(cr, b->x, b->y);
	cairo_rotate(cr, b->a);
	cairo_set_source_surface(cr, b->img, -b->ox, -b->oy);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	if (b->mat == GLASS) {
		// glass flashes as it turns to the light
		double flash = pow(fmax(0, cos(b->a * 2 + b->phase)), 14);
		if (flash > 0.05) {
			cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
			cairo_set_source_surface(cr, b->img, -b->ox, -b->oy);
			cairo_paint_with_alpha(cr, flash * 0.55);
		}
	}
	cairo_restore(cr);
}

static void decay_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct decay *s = state;
	double u = s->u, W = width, H = height;
	s->t += dt;
	s->frame++;
	double P = progress(s);
	s->day += dt * 2 * M_PI / (14 - 10 * smooth(0, 0.9f, (float)P));
	s->light = 0.5 + 0.5 * sin(s->day);
	s->shake = fmax(0, s->shake - dt * 3);
	// pieces come off
	int owners = s->count + s->bar_count;
	for (int o = 0; o < owners; o++) {
		if (s->state[o] == GONE) {
			continue;
		}
		if (s->state[o] == INTACT && s->t >= s->start[o]) {
			s->state[o] = CRUMBLING;
			// what lay on it falls
			for (int k = 0; k < s->body_count; k++) {
				struct body *b = &s->bodies[k];
				if (b->alive && b->perch == o) {
					b->perch = -1;
					b->still = 0;
				}
			}
		}
		if (s->state[o] != CRUMBLING) {
			continue;
		}
		bool all = true;
		for (int k = s->first[o]; k < s->last[o]; k++) {
			struct piece *p = &s->pieces[k];
			if (!p->fallen && s->t >= p->fall_at) {
				if (s->flying < FLYING_MAX) {
					come_off(s, p);
					s->flying++;
				} else {
					p->fall_at = (float)(s->t + saver_between(0.2, 0.6));
				}
			}
			all = all && p->fallen;
			if (!p->fallen && s->t > p->fall_at - s->span * 0.03 && saver_random() < dt * 2) {
				int i = (int)(saver_random() * p->n);
				struct grain *g = new_grain(s);
				*g = (struct grain){ p->px[i], p->py[i], 0, 0, 3, 0, (float)(u * saver_between(0.8, 1.8)),
					p->mat == GLASS ? GLITTER : DUST, true };
			}
		}
		if (all) {
			s->state[o] = GONE;
			// what of it no piece took (a thread of pixels between two) goes with it
			struct saver_window *w = o < s->count ? &s->wins[o] : &s->bars[o - s->count];
			int x0 = (int)fmax(0, w->x), y0 = (int)fmax(0, w->y);
			int x1 = (int)fmin(s->width, w->x + w->w), y1 = (int)fmin(s->height, w->y + w->h);
			for (int y = y0; y < y1; y++) {
				bool changed = false;
				for (int x = x0; x < x1; x++) {
					int i = y * s->width + x;
					if (s->owner[i] != o) {
						continue;
					}
					int below = -1;
					for (int j = (o < s->count ? o : s->count) - 1; j >= 0 && below < 0; j--) {
						if (s->state[j] != GONE && in_window(s, &s->wins[j], x, y)) {
							below = j;
						}
					}
					s->owner[i] = (short)below;
					s->src[i] = below >= 0 ? (uint8_t)(2 + below) : 1;
					s->mat[i] = material_of(s, below, x, y);
					changed = true;
				}
				if (changed) {
					cairo_surface_flush(s->view);
					update_span(s, y, x0, x1);
					cairo_surface_mark_dirty(s->view);
				}
			}
		}
	}
	update_rows(s);
	physics(s, dt);
	s->flying = 0;
	for (int k = 0; k < s->body_count; k++) {
		s->flying += s->bodies[k].alive && !s->bodies[k].asleep && s->bodies[k].perch < 0;
	}
	// the dust settles over the rubble at the end, in dunes
	float crumble = (float)(u * (0.02 + 2.5 * smooth(0.82f, 1.05f, (float)P)) * dt);
	for (int x = 0; x < s->width; x++) {
		float dune = (float)(u * (6 + 60 * s->dune[x] * s->dune[x]));
		float want = s->heap[x] * smooth(0.82f, 1, (float)P) + dune * smooth(0.6f, 1, (float)P);
		if (want > s->dust[x]) {
			float close = (float)((want - s->dust[x]) * 0.25 * smooth(0.85f, 1, (float)P) * dt);
			s->dust[x] = fminf(want, s->dust[x] + fmaxf(crumble, close));
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

	cairo_save(cr);
	if (s->shake > 0.01) {
		double k = s->shake * s->shake * u * 5;
		cairo_translate(cr, sin(s->t * 71) * k, cos(s->t * 53) * k * 0.6);
	}
	cairo_set_source_surface(cr, s->scene, 0, 0);
	if (s->shake > 0.01) {
		cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	}
	cairo_paint(cr);
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (b->alive && !b->asleep && b->img) {
			draw_body(s, cr, b);
		}
	}
	// dust, fibres and glitter
	for (int k = 0; k < GRAINS_MAX; k++) {
		struct grain *g = &s->grains[k];
		if (g->age >= g->life) {
			continue;
		}
		g->age += (float)dt;
		if (g->settles) {
			g->vy += (float)(u * (g->kind == FIBRE ? 300 : 900) * dt);
			g->vx -= g->vx * (float)fmin(1, (g->kind == FIBRE ? 3 : 0.5) * dt);
		} else {
			g->vx -= g->vx * (float)fmin(1, 2.5 * dt);
			g->vy -= g->vy * (float)fmin(1, 2.5 * dt) - (float)(u * 12 * dt);
		}
		g->x += g->vx * (float)dt;
		g->y += g->vy * (float)dt;
		float floor = ground(s, g->x);
		if (g->settles && g->y >= floor) {
			g->y = floor;
			if (g->kind == GLITTER && fabsf(g->vy) > u * 60) {
				g->vy = -g->vy * 0.3f; // a splinter skips
				g->vx *= 0.6f;
			} else {
				g->vx = g->vy = 0;
			}
		}
	}
	for (int kind = DUST; kind <= GLITTER; kind++) {
		cairo_new_path(cr);
		for (int k = 0; k < GRAINS_MAX; k++) {
			struct grain *g = &s->grains[k];
			if (g->age >= g->life || g->kind != kind) {
				continue;
			}
			double fade = fmin(1, (g->life - g->age) * 1.5);
			if (kind == FIBRE) {
				cairo_move_to(cr, g->x, g->y);
				cairo_line_to(cr, g->x + cos(g->age * 3 + k) * g->size, g->y + sin(g->age * 3 + k) * g->size * 0.4);
			} else if (kind == GLITTER) {
				// a splinter of glass: now and then it catches the light
				double sparkle = pow(fmax(0, sin(g->age * 9 + k)), 6) * fade;
				double r = g->size * (0.5 + sparkle);
				cairo_rectangle(cr, g->x - r / 2, g->y - r / 2, r, r);
			} else {
				// dust: a soft round, growing as it spreads, fading
				double r = g->size * (1.2 + 2 * g->age / g->life);
				double k = r / cairo_image_surface_get_width(s->puff);
				cairo_save(cr);
				cairo_translate(cr, g->x - r / 2, g->y - r / 2);
				cairo_scale(cr, k, k);
				cairo_set_source_surface(cr, s->puff, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
				cairo_paint_with_alpha(cr, fade * 0.8);
				cairo_restore(cr);
			}
		}
		if (kind == FIBRE) {
			cairo_set_line_width(cr, fmax(0.6, u * 0.7));
			cairo_set_source_rgba(cr, 0.4, 0.28, 0.16, 0.8);
			cairo_stroke(cr);
		} else if (kind == GLITTER) {
			cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
			cairo_set_source_rgba(cr, 0.85, 0.9, 1, 0.8);
			cairo_fill(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		}
	}
	// the shards in the heap glint
	if (s->resting > 0 && P < 1 && s->body_count > 0) {
		for (int k = 0; k < 3; k++) {
			struct body *b = &s->bodies[(int)(saver_random() * s->body_count)];
			if (!b->alive || !b->asleep || b->mat != GLASS || saver_random() > dt * 12) {
				continue;
			}
			for (int i = 0; i < GLINTS_MAX; i++) {
				if (s->glints[i].age >= s->glints[i].life) {
					int v = (int)(saver_random() * b->n);
					s->glints[i] = (struct glint){ b->wx[v], b->wy[v], 0, (float)saver_between(0.2, 0.6),
						(float)(u * saver_between(2, 5)) };
					break;
				}
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
	cairo_set_source_rgba(cr, 1, 1, 1, 0.9 * s->light + 0.1);
	cairo_stroke(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_restore(cr);
	// the days: light through a high window, the nights
	double day = s->light;
	if (day > 0.05) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		// the sun moves across the room in a day
		double across = fmod(s->day / (2 * M_PI) + 0.25, 1);
		cairo_set_source_surface(cr, s->rays, round((across - 0.5) * W * 0.9), 0);
		cairo_paint_with_alpha(cr, day);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	double dark = (1 - day) * 0.45 + 0.12 * smooth(0.3f, 1, (float)P);
	cairo_set_source_rgba(cr, 0.04, 0.035, 0.06, fmin(0.7, dark));
	cairo_paint(cr);
	// motes drifting, bright in the light
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

static void decay_destroy(void *state) {
	struct decay *s = state;
	for (int k = 0; k < s->body_count; k++) {
		if (s->bodies[k].img) {
			cairo_surface_destroy(s->bodies[k].img);
		}
	}
	for (int k = 0; k < s->bucket_count; k++) {
		free(s->buckets[k]);
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
	cairo_surface_destroy(s->rays);
	cairo_surface_destroy(s->puff);
	cairo_surface_destroy(s->web_img);
	free(s->buckets);
	free(s->bucket_len);
	free(s->bucket_cap);
	free(s->bodies);
	free(s->contacts);
	free(s->owner);
	free(s->src);
	free(s->mat);
	free(s->age_n);
	free(s->stain_n);
	free(s->grain_n);
	free(s->wood_n);
	free(s->peel_n);
	free(s->mould_n);
	free(s->dirt);
	free(s->pieces);
	free(s->cracks);
	free(s->heap);
	free(s->dust);
	free(s->dune);
	free(s);
}

void saver_decay_stats(void *state, struct decay_stats *out) {
	struct decay *s = state;
	*out = (struct decay_stats){ .pieces = s->piece_count, .cracks = s->crack_count,
		.fallen = s->fallen, .resting = s->resting, .shattered = s->shattered,
		.progress = progress(s), .inside = true };
	for (int k = 0; k < s->body_count; k++) {
		struct body *b = &s->bodies[k];
		if (!b->alive) {
			continue;
		}
		if (!b->asleep) {
			out->flying++;
		}
		if (!isfinite(b->x) || !isfinite(b->y) || b->x < -b->radius - 5 ||
				b->x > s->width + b->radius + 5) {
			out->inside = false;
		}
		// nothing at rest sinks through the bottom
		if (b->asleep && b->y0 > s->height + 2) {
			out->inside = false;
		}
	}
	for (int x = 0; x < s->width; x++) {
		out->rubble += s->heap[x] / s->width;
		out->dust += s->dust[x] / s->width;
	}
	for (int o = 0; o < s->count + s->bar_count; o++) {
		out->standing += s->state[o] != GONE;
	}
	long same = 0, all = 0;
	cairo_surface_flush(s->view);
	const uint32_t *v = (const uint32_t *)cairo_image_surface_get_data(s->view);
	const uint32_t *o = (const uint32_t *)cairo_image_surface_get_data(s->orig);
	for (int i = 0; i < s->width * s->height; i += 13) {
		same += (v[i] & 0xffffff) == (o[i] & 0xffffff);
		all++;
		out->glass += s->mat[i] == GLASS;
		out->wood += s->mat[i] == WOOD;
		out->stone += s->mat[i] == STONE;
	}
	out->untouched = all ? (double)same / all : 0;
}

const struct saver saver_decay = {
	.name = "decay",
	.title = "Decay",
	.description = "The desktop eaten by time: glass cracking and shattering, wood rotting, "
		"stone crumbling, all to dust",
	.wants_desktop = true,
	.covers = true,
	.create = decay_create,
	.draw = decay_draw,
	.destroy = decay_destroy,
};
