#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "saver_noise.h"
#include "saver_util.h"

/*
 * Jungle: the desktop overgrown. The light turns green and close. Moss
 * creeps up the windows from their lower edges and in from their corners,
 * over the taskbar and up the desktop from the bottom. Vines sprout from the
 * bottom of the screen and from the windows, climb along their edges, creep
 * over the title bars and on up, branch, curl tendrils and put out leaves of
 * several kinds one after another; more hang down from the top. Flowers open
 * on them, stay a while, wilt and drop their petals, and open again, now and
 * then in another colour. Giant fronds unfurl in from the edges of the screen
 * and sway. Butterflies flit about and settle on the flowers, leaves and
 * petals drift down, and days pass: in the day light falls in shafts through
 * the canopy, at night fireflies glow. After some minutes all is jungle.
 *
 * The windows and the taskbar are the real ones: the picture of the screen
 * from before the saver, and where they are from tileWin.
 */

#define WINDOWS_MAX 48
#define VINES_MAX 420
#define LEAVES_MAX 700      // growing now; grown ones are drawn into the plants for good
#define FLOWERS_MAX 180
#define FRONDS_MAX 9
#define BUTTERFLIES 7
#define FIREFLIES 60
#define BITS_MAX 260        // petals and leaves falling, pollen
#define LEAF_KINDS 4
#define LEAF_SHADES 5
#define FROND_KINDS 3
#define ROW_STEP 4
#define SCENE_EVERY 6
#define MOSS_RAMP 6.0f
#define LEAF_GROWS 0.7   // seconds a leaf takes to unfold
#define NEVER 1e9f

struct vine {
	float x, y, angle, len, max_len, speed, width, leaf_next, start;
	int depth, side;
	uint32_t seed;
	bool alive, down, started;
};

struct leaf {
	float x, y, angle, size, born;
	uint8_t kind, shade;
};

enum flower_state { BUD, OPENING, OPEN, WILTING, RESTING };

struct flower {
	float x, y, size, t, spin;   // t: in its state
	float hold;                  // how long it stays so
	uint8_t petals, colour;
	enum flower_state state;
};

struct frond {
	float x, y, angle, length, born, phase;
	int kind;
};

struct butterfly {
	float x, y, vx, vy, flap, rest, hue;
	int perch;                   // the flower it sits on, -1 flying
};

struct bit {
	float x, y, vx, vy, angle, spin, life, age, size;
	uint8_t kind;                // 0 a petal, 1 a leaf, 2 pollen
	uint32_t colour;
};

struct jungle {
	int width, height;
	double u, t, span;
	int frame;
	int part;                 // of the slow parts to build next
	cairo_surface_t *orig, *lush;  // the desktop, and in the green light of the jungle
	cairo_surface_t *view;         // with its moss
	cairo_surface_t *plants;       // the vines and leaves grown, drawn for good
	cairo_surface_t *scene, *next;
	cairo_surface_t *leaf_img[LEAF_KINDS][LEAF_SHADES];
	cairo_surface_t *frond_img[FROND_KINDS];
	cairo_surface_t *glow;
	cairo_surface_t *rays;
	cairo_surface_t *flower_img[6][2][2]; // colour, five or six petals, fresh or wilted
	int flower_px;
	int leaf_size;                 // of the pictures of the leaves
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	// the moss, worked out on a thread
	float *moss_at;                // per pixel: when the moss reaches it
	uint8_t *moss_tex;
	float *row_first, *row_last;
	pthread_t worker;
	bool working;
	atomic_bool ready;
	int intro_rows;
	struct vine vines[VINES_MAX];
	int vine_count;
	struct leaf leaves[LEAVES_MAX];
	int leaf_count;
	long leaves_grown;
	struct flower flowers[FLOWERS_MAX];
	int flower_count;
	long blooms;
	struct frond fronds[FRONDS_MAX];
	int frond_count;
	struct butterfly butterflies[BUTTERFLIES];
	struct bit bits[BITS_MAX];
	int bit_next;
	float flies[FIREFLIES][4];     // x, y, phase, drift
	double day, night, wind;
	bool creatures, nights;
};

static double progress(struct jungle *s) {
	return s->t / s->span;
}

/* ---------- the pictures made beforehand ---------- */

/* A leaf pointing right from its stalk at the left middle: its outline. */
static void leaf_outline(cairo_t *cr, int kind, double L, double W) {
	double h = W / 2;
	switch (kind) {
	case 0: // ovate, pointed
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, L * 0.25, -h * 1.1, L * 0.7, -h * 0.9, L, 0);
		cairo_curve_to(cr, L * 0.7, h * 0.9, L * 0.25, h * 1.1, 0, 0);
		break;
	case 1: // a heart, like a philodendron
		cairo_move_to(cr, L * 0.12, 0);
		cairo_curve_to(cr, -L * 0.05, -h * 1.2, L * 0.55, -h * 1.3, L, 0);
		cairo_curve_to(cr, L * 0.55, h * 1.3, -L * 0.05, h * 1.2, L * 0.12, 0);
		break;
	case 2: // long and narrow
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, L * 0.3, -h * 0.55, L * 0.75, -h * 0.45, L, 0);
		cairo_curve_to(cr, L * 0.75, h * 0.45, L * 0.3, h * 0.55, 0, 0);
		break;
	default: // round
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, L * 0.1, -h * 1.3, L * 0.95, -h * 1.25, L, 0);
		cairo_curve_to(cr, L * 0.95, h * 1.25, L * 0.1, h * 1.3, 0, 0);
		break;
	}
	cairo_close_path(cr);
}

static const double leaf_shades[LEAF_SHADES][3] = {
	{ 0.13, 0.32, 0.1 }, { 0.2, 0.45, 0.12 }, { 0.36, 0.6, 0.16 }, { 0.1, 0.26, 0.14 },
	{ 0.45, 0.62, 0.2 },
};

static cairo_surface_t *make_leaf(int size, int kind, int shade) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	double L = size * 0.96, W = kind == 2 ? size * 0.4 : size * 0.7;
	cairo_translate(cr, 1, size / 2.0);
	const double *c = leaf_shades[shade];
	leaf_outline(cr, kind, L, W);
	cairo_pattern_t *p = cairo_pattern_create_linear(0, -W / 2, 0, W / 2);
	cairo_pattern_add_color_stop_rgb(p, 0, c[0] * 1.25, c[1] * 1.2, c[2] * 1.1);
	cairo_pattern_add_color_stop_rgb(p, 0.5, c[0], c[1], c[2]);
	cairo_pattern_add_color_stop_rgb(p, 1, c[0] * 0.6, c[1] * 0.65, c[2] * 0.6);
	cairo_set_source(cr, p);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	cairo_set_line_width(cr, fmax(0.6, size * 0.02));
	cairo_set_source_rgba(cr, 0.05, 0.12, 0.04, 0.6);
	cairo_stroke(cr);
	// the midrib and the veins
	cairo_move_to(cr, 0, 0);
	cairo_curve_to(cr, L * 0.4, -size * 0.02, L * 0.7, 0, L * 0.95, 0);
	for (int k = 1; k < 6; k++) {
		double x = L * k / 7.0, v = W * 0.32 * (1 - k / 8.0);
		cairo_move_to(cr, x, 0);
		cairo_line_to(cr, x + v * 0.8, -v);
		cairo_move_to(cr, x, 0);
		cairo_line_to(cr, x + v * 0.8, v);
	}
	cairo_set_line_width(cr, fmax(0.5, size * 0.025));
	cairo_set_source_rgba(cr, 0.75, 0.9, 0.5, 0.45);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return img;
}

/* A giant leaf, its stalk at the left middle: a monstera, a palm frond, a banana leaf. */
static cairo_surface_t *make_frond(int length, int kind) {
	int h = (int)(length * (kind == 1 ? 0.55 : 0.5));
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, length, h);
	cairo_t *cr = cairo_create(img);
	double L = length - 2, H2 = h / 2.0;
	cairo_translate(cr, 1, H2);
	cairo_pattern_t *p = cairo_pattern_create_linear(0, -H2, L * 0.3, H2);
	cairo_pattern_add_color_stop_rgb(p, 0, 0.2, 0.46, 0.14);
	cairo_pattern_add_color_stop_rgb(p, 0.6, 0.1, 0.3, 0.08);
	cairo_pattern_add_color_stop_rgb(p, 1, 0.05, 0.18, 0.05);
	if (kind == 1) {
		// a palm frond: a curving rib with long leaflets swept back from it
		for (int k = 0; k < 26; k++) {
			double f = 0.08 + 0.9 * k / 26.0, x = L * f, y = -sin(f * M_PI) * H2 * 0.15;
			double ll = H2 * 0.95 * sin(f * M_PI * 0.95 + 0.1);
			for (int side = -1; side <= 1; side += 2) {
				cairo_save(cr);
				cairo_translate(cr, x, y);
				cairo_rotate(cr, side * 1.05 + 0.2);
				cairo_scale(cr, ll, ll * 0.12);
				cairo_move_to(cr, 0, 0);
				cairo_curve_to(cr, 0.3, -1, 0.8, -0.6, 1, 0);
				cairo_curve_to(cr, 0.8, 0.6, 0.3, 1, 0, 0);
				cairo_restore(cr);
			}
		}
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, L * 0.3, -H2 * 0.15, L * 0.7, -H2 * 0.15, L, 0);
		cairo_set_line_width(cr, fmax(1, length * 0.008));
		cairo_set_source_rgba(cr, 0.55, 0.6, 0.3, 0.9);
		cairo_stroke(cr);
	} else {
		// a monstera (a heart) or a banana leaf (long), with its ribs
		if (kind == 0) {
			cairo_move_to(cr, L * 0.1, 0);
			cairo_curve_to(cr, -L * 0.05, -H2 * 1.15, L * 0.6, -H2 * 1.1, L, -H2 * 0.05);
			cairo_curve_to(cr, L * 0.6, H2 * 1.1, -L * 0.05, H2 * 1.15, L * 0.1, 0);
		} else {
			cairo_move_to(cr, 0, 0);
			cairo_curve_to(cr, L * 0.15, -H2 * 0.8, L * 0.8, -H2 * 0.75, L, 0);
			cairo_curve_to(cr, L * 0.8, H2 * 0.75, L * 0.15, H2 * 0.8, 0, 0);
		}
		cairo_close_path(cr);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		// the ribs
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, L * 0.97, 0);
		int ribs = kind == 0 ? 9 : 22;
		for (int k = 1; k < ribs; k++) {
			double x = L * (0.08 + 0.85 * k / ribs), v = H2 * (kind == 0 ? 0.9 : 0.7);
			cairo_move_to(cr, x, 0);
			cairo_curve_to(cr, x + v * 0.2, -v * 0.4, x + v * 0.45, -v * 0.7, x + v * 0.5, -v);
			cairo_move_to(cr, x, 0);
			cairo_curve_to(cr, x + v * 0.2, v * 0.4, x + v * 0.45, v * 0.7, x + v * 0.5, v);
		}
		cairo_set_line_width(cr, fmax(0.8, length * 0.004));
		cairo_set_source_rgba(cr, 0.6, 0.8, 0.35, 0.35);
		cairo_stroke(cr);
		// the splits and holes of a monstera, the tears of a banana leaf
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		for (int k = 0; k < (kind == 0 ? 7 : 5); k++) {
			double x = L * saver_between(0.2, 0.85);
			for (int side = -1; side <= 1; side += 2) {
				if (kind == 2 && saver_random() < 0.5) {
					continue;
				}
				double from = H2 * saver_between(0.15, 0.4), to = H2 * 1.3;
				cairo_move_to(cr, x, side * from);
				cairo_line_to(cr, x + H2 * 0.3, side * to);
				cairo_set_line_width(cr, fmax(1.5, length * (kind == 0 ? 0.012 : 0.006)));
				cairo_stroke(cr);
				if (kind == 0 && saver_random() < 0.6) {
					cairo_save(cr);
					cairo_translate(cr, x - L * 0.04, side * H2 * saver_between(0.25, 0.45));
					cairo_scale(cr, L * 0.025, H2 * 0.08);
					cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
					cairo_restore(cr);
					cairo_fill(cr);
				}
			}
		}
	}
	cairo_destroy(cr);
	return img;
}

static cairo_surface_t *make_glow(double r) {
	int size = (int)ceil(r * 2 + 2);
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	double c = size / 2.0;
	cairo_pattern_t *p = cairo_pattern_create_radial(c, c, 0, c, c, r);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.95, 1, 0.6, 1);
	cairo_pattern_add_color_stop_rgba(p, 0.2, 0.7, 1, 0.3, 0.6);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.4, 0.9, 0.2, 0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	cairo_destroy(cr);
	return img;
}

/* The desktop in the close green light under the canopy. */
static cairo_surface_t *lush_light(cairo_surface_t *desktop, int width, int height) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_surface_flush(desktop);
	const uint32_t *src = (const uint32_t *)cairo_image_surface_get_data(desktop);
	uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(out);
	int ss = cairo_image_surface_get_stride(desktop) / 4, ds = cairo_image_surface_get_stride(out) / 4;
	for (int y = 0; y < height; y++) {
		float dy = (y - height * 0.5f) / (height * 0.5f);
		for (int x = 0; x < width; x++) {
			uint32_t p = src[y * ss + x];
			float r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			float dx = (x - width * 0.5f) / (width * 0.5f);
			float edge = 1 - 0.4f * smooth(0.45f, 1.45f, sqrtf(dx * dx + dy * dy));
			float lum = r * 0.3f + g * 0.59f + b * 0.11f;
			r = (r * 0.55f + lum * 0.2f) * 0.8f * edge;
			g = (g * 0.6f + lum * 0.3f + 18) * 0.92f * edge;
			b = (b * 0.5f + lum * 0.15f) * 0.7f * edge;
			dst[y * ds + x] = 0xff000000u | (uint32_t)fminf(255, r) << 16 |
				(uint32_t)fminf(255, g) << 8 | (uint32_t)fminf(255, b);
		}
	}
	cairo_surface_mark_dirty(out);
	return out;
}

/* ---------- the moss ---------- */

/*
 * When the moss reaches each pixel: up the windows and the taskbar from their
 * lower edges and in from their corners, up the desktop from the bottom and
 * out from under the windows, along ragged fronts, all covered near the end.
 */
static void *grow_moss(void *data) {
	struct jungle *s = data;
	int W = s->width, H = s->height;
	double u = s->u;
	struct grid n1, n2, n3;
	grid_make(&n1, W, H, 4, (float)fmax(4, u * 90), 11);
	grid_make(&n2, W, H, 4, (float)fmax(4, u * 9), 23);
	grid_make(&n3, W, H, 4, (float)fmax(4, u * 30), 37);
	float span = (float)s->span;
	// the desktop: from the bottom up
	for (int y = 0; y < H; y++) {
		float up = (float)(H - y) / H;
		for (int x = 0; x < W; x++) {
			int i = y * W + x;
			float f = up * 0.95f + (grid_at(&n1, x, y) - 0.5f) * 0.45f;
			s->moss_at[i] = span * (0.12f + 0.8f * fmaxf(0, f));
		}
	}
	// the windows and taskbars, the upper over the lower, each from below and its corners
	for (int k = 0; k < s->count + s->bar_count; k++) {
		struct saver_window *w = k < s->count ? &s->wins[k] : &s->bars[k - s->count];
		int x0 = (int)fmax(0, w->x), y0 = (int)fmax(0, w->y);
		int x1 = (int)fmin(W, w->x + w->w), y1 = (int)fmin(H, w->y + w->h);
		float bw = (float)fmax(1, x1 - x0), bh = (float)fmax(1, y1 - y0);
		float begin = k < s->count ? 0.08f + 0.02f * k : 0.04f;
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				float up = (y1 - y) / bh;
				float dx = fminf(x - x0, x1 - 1 - x) / bw, dy = fminf(y - y0, y1 - 1 - y) / bh;
				float corner = sqrtf(dx * dy) * 1.6f;
				float f = fminf(up * 0.9f, corner) + (grid_at(&n1, x, y) - 0.5f) * 0.35f;
				s->moss_at[y * W + x] = span * (begin + 0.8f * fmaxf(0, f));
			}
		}
	}
	for (int y = 0; y < H; y++) {
		float first = NEVER, last = -1;
		for (int x = 0; x < W; x++) {
			int i = y * W + x;
			float tex = grid_at(&n2, x, y) * 0.6f + grid_at(&n3, x, y) * 0.4f;
			s->moss_tex[i] = (uint8_t)fminf(255, tex * 280 + (hash2(x, y, 5) & 31));
			first = fminf(first, s->moss_at[i]);
			last = fmaxf(last, s->moss_at[i]);
		}
		s->row_first[y] = first;
		s->row_last[y] = last;
	}
	free(n1.v);
	free(n2.v);
	free(n3.v);
	atomic_store(&s->ready, true);
	return NULL;
}

static inline uint32_t moss_colour(uint8_t tex) {
	// dark in the hollows, bright green on the cushions, a yellow tip here and there
	uint32_t c = tex < 128 ? mix_px(0xff1f3312, 0xff4a6e22, tex * 2) :
		mix_px(0xff4a6e22, 0xff86a83a, (tex - 128) * 2);
	return (tex & 7) == 0 && tex > 170 ? mix_px(c, 0xffc8c060, 150) : c;
}

static void update_rows(struct jungle *s) {
	int W = s->width;
	float t = (float)s->t;
	bool intro = s->intro_rows > 0, ready = atomic_load(&s->ready);
	if (!intro && !ready) {
		return;
	}
	int k_intro = (int)(256 * fmin(1, s->t / 4));
	uint32_t *orig = (uint32_t *)cairo_image_surface_get_data(s->orig);
	uint32_t *lush = (uint32_t *)cairo_image_surface_get_data(s->lush);
	uint32_t *view = (uint32_t *)cairo_image_surface_get_data(s->view);
	cairo_surface_flush(s->view);
	for (int y = s->frame % ROW_STEP; y < s->height; y += ROW_STEP) {
		bool busy = ready && t >= s->row_first[y] && t <= s->row_last[y] + MOSS_RAMP + 1.5f;
		if (!intro && !busy) {
			continue;
		}
		for (int x = 0; x < W; x++) {
			int i = y * W + x;
			float at = ready ? s->moss_at[i] : NEVER;
			bool growing = at < t && t - at <= MOSS_RAMP + 1.5f;
			if (!intro && !growing) {
				continue;
			}
			uint32_t p = k_intro < 256 ? mix_px(orig[i], lush[i], k_intro) : lush[i];
			if (at < t) {
				float a = fminf(1, (t - at) / MOSS_RAMP);
				p = mix_px(p, moss_colour(s->moss_tex[i]), (int)(a * 0.95f * 256));
			}
			view[i] = p;
		}
	}
	cairo_surface_mark_dirty(s->view);
	if (s->t >= 4 && s->intro_rows > 0) {
		s->intro_rows--;
	}
}

/* ---------- vines ---------- */

static struct vine *new_vine(struct jungle *s, float x, float y, float angle, float max_len,
		int depth, bool down, float start) {
	if (s->vine_count >= VINES_MAX) {
		return NULL;
	}
	struct vine *v = &s->vines[s->vine_count++];
	double u = s->u;
	*v = (struct vine){ .x = x, .y = y, .angle = angle, .max_len = max_len,
		.speed = (float)(u * saver_between(16, 34)), .width = (float)(u * (3.4 - depth * 0.8)),
		.leaf_next = (float)(u * saver_between(4, 12)), .start = start, .depth = depth,
		.side = saver_random() < 0.5 ? -1 : 1, .seed = (uint32_t)(saver_random() * 1e9),
		.alive = true, .down = down };
	v->width = fmaxf(1, v->width);
	return v;
}

/* Where the vines sprout, and when: along the bottom, at the windows, from the top. */
static void plan_vines(struct jungle *s) {
	double u = s->u, W = s->width, H = s->height, span = s->span;
	float floor = (float)H;
	for (int k = 0; k < s->bar_count; k++) {
		if (s->bars[k].y > H * 0.5) {
			floor = (float)fmin(floor, s->bars[k].y);
		}
	}
	for (double x = saver_between(0, u * 40); x < W; x += u * saver_between(30, 70)) {
		new_vine(s, (float)x, floor, (float)(-M_PI / 2 + saver_between(-0.3, 0.3)),
			(float)(H * saver_between(0.5, 1.3)), 0, false, (float)(span * saver_between(0.01, 0.5)));
	}
	for (int i = 0; i < s->count; i++) {
		struct saver_window *w = &s->wins[i];
		for (int k = 0; k < 5; k++) {
			double x = k < 2 ? w->x + (k ? w->w - 2 : 2) : w->x + saver_between(0.1, 0.9) * w->w;
			double y = w->y + w->h - 2;
			new_vine(s, (float)x, (float)fmin(y, floor), (float)(-M_PI / 2 + saver_between(-0.2, 0.2)),
				(float)(H * saver_between(0.3, 0.8)), 0, false,
				(float)(span * saver_between(0.06, 0.6)));
		}
	}
	for (double x = saver_between(0, u * 80); x < W; x += u * saver_between(70, 160)) {
		new_vine(s, (float)x, -2, (float)(M_PI / 2 + saver_between(-0.2, 0.2)),
			(float)(H * saver_between(0.2, 0.6)), 1, true, (float)(span * saver_between(0.2, 0.75)));
	}
	// and late on anywhere on the moss, to fill it all
	for (int k = 0; k < 40; k++) {
		new_vine(s, (float)(saver_random() * W), (float)(H * saver_between(0.3, 1)),
			(float)(-M_PI / 2 + saver_between(-0.8, 0.8)), (float)(H * saver_between(0.2, 0.5)), 1,
			false, (float)(span * saver_between(0.5, 0.9)));
	}
}

static void add_leaf(struct jungle *s, cairo_t *plants, float x, float y, float angle, float size);

static void add_flower(struct jungle *s, float x, float y, float size) {
	if (s->flower_count >= FLOWERS_MAX) {
		return;
	}
	s->flowers[s->flower_count++] = (struct flower){ x, y, size, 0, (float)saver_between(0, 6.3),
		(float)saver_between(2, 6), (uint8_t)(5 + saver_random() * 2), (uint8_t)(saver_random() * 6),
		BUD };
}

/* A tendril: a little spiral off the vine, added to the path. */
static void tendril(struct jungle *s, cairo_t *cr, float x, float y, float angle) {
	double u = s->u, r = u * saver_between(3, 6);
	cairo_move_to(cr, x, y);
	double turn = saver_random() < 0.5 ? 1 : -1;
	for (int k = 1; k <= 24; k++) {
		double f = k / 24.0, a = angle + turn * f * 4 * M_PI, rr = r * (1 - f * 0.8);
		cairo_line_to(cr, x + cos(angle) * r * 1.5 * f + cos(a) * rr * f,
			y + sin(angle) * r * 1.5 * f + sin(a) * rr * f);
	}
}

/*
 * What grows is drawn at once where it stays (the plants) and into the ground
 * shown now and the one being built, so it grows smoothly frame by frame.
 */
static void stroke_everywhere(struct jungle *s, cairo_t *cr) {
	cairo_surface_t *also[2] = { s->scene, s->next };
	cairo_path_t *path = cairo_copy_path(cr);
	for (int k = 0; k < 2; k++) {
		cairo_t *o = cairo_create(also[k]);
		cairo_append_path(o, path);
		cairo_set_line_cap(o, cairo_get_line_cap(cr));
		cairo_set_line_join(o, cairo_get_line_join(cr));
		cairo_set_line_width(o, cairo_get_line_width(cr));
		cairo_set_source(o, cairo_get_source(cr));
		cairo_stroke(o);
		cairo_destroy(o);
	}
	cairo_path_destroy(path);
	cairo_stroke(cr);
}

/* The vines grow on: the new bits drawn into the plants for good. */
static void grow_vines(struct jungle *s, double dt) {
	double u = s->u, W = s->width, H = s->height;
	cairo_t *cr = cairo_create(s->plants);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	float curls[64][3];
	int curl_count = 0;
	for (int k = 0; k < s->vine_count; k++) {
		struct vine *v = &s->vines[k];
		if (!v->alive || s->t < v->start) {
			continue;
		}
		double left = v->speed * dt * (0.6 + 0.4 * sin(s->t * 0.3 + v->seed));
		cairo_new_path(cr);
		cairo_move_to(cr, v->x, v->y);
		while (left > 0 && v->alive) {
			float step = (float)fmin(left, u * 2.5);
			left -= step;
			// up (or down), wandering, and along the edges of windows it meets
			// branches reach out sideways, all of them meander
			double want = v->down ? M_PI / 2 : -M_PI / 2 + (v->depth ? v->side * 0.8 : 0);
			want += sin(v->len / (u * 70) + v->seed % 100) * 0.9 +
				sin(v->len / (u * 23) + (v->seed % 37) * 1.7) * 0.4;
			for (int i = 0; i < s->count; i++) {
				struct saver_window *w = &s->wins[i];
				bool beside = v->y > w->y && v->y < w->y + w->h;
				if (beside && (fabs(v->x - w->x) < u * 9 || fabs(v->x - (w->x + w->w)) < u * 9)) {
					want = v->down ? M_PI / 2 : -M_PI / 2; // climbing up the side
				}
				if (!v->down && v->x > w->x && v->x < w->x + w->w && v->y > w->y - u * 6 &&
						v->y < w->y + u * 3 && sin(v->len / (u * 120) + v->seed) > -0.2) {
					want = v->side > 0 ? 0 : M_PI; // creeping along the top
				}
			}
			double d = atan2(sin(want - v->angle), cos(want - v->angle));
			v->angle += (float)fmax(-0.09, fmin(0.09, d * 0.3));
			v->x += cosf(v->angle) * step;
			v->y += sinf(v->angle) * step;
			v->len += step;
			cairo_line_to(cr, v->x, v->y);
			if (v->len >= v->leaf_next) {
				v->leaf_next += (float)(u * saver_between(6, 12));
				v->side = -v->side;
				float size = (float)(u * saver_between(12, 32) * (1 - v->depth * 0.15) *
					(1 - 0.3 * v->len / v->max_len));
				add_leaf(s, cr, v->x, v->y, v->angle + v->side * (float)saver_between(0.6, 1.3), size);
				double r = saver_random();
				if (r < 0.07) {
					add_flower(s, v->x + cosf(v->angle + v->side) * size * 0.4f,
						v->y + sinf(v->angle + v->side) * size * 0.4f, (float)(u * saver_between(8, 16)));
				} else if (r < 0.12 && curl_count < 64) {
					curls[curl_count][0] = v->x;
					curls[curl_count][1] = v->y;
					curls[curl_count++][2] = v->angle - v->side * 1.2f;
				} else if (r < 0.26 && v->depth < 3 && v->max_len - v->len > u * 60) {
					new_vine(s, v->x, v->y, v->angle + v->side * (float)saver_between(0.6, 1.4),
						(v->max_len - v->len) * (float)saver_between(0.4, 0.8), v->depth + 1, v->down,
						(float)s->t);
				}
			}
			if (v->len >= v->max_len || v->y < -20 || v->y > H + 20 || v->x < -20 || v->x > W + 20) {
				v->alive = false;
			}
		}
		// thick near the root, thin to the tip, dark and woody to green
		float taper = 1 - 0.65f * fminf(1, v->len / v->max_len);
		cairo_set_line_width(cr, fmaxf(0.8f, v->width * taper));
		double g = 0.28 + 0.1 * v->depth;
		cairo_set_source_rgb(cr, 0.18 + 0.05 * v->depth, g + 0.08, 0.1);
		stroke_everywhere(s, cr);
	}
	if (curl_count) {
		cairo_new_path(cr);
		for (int k = 0; k < curl_count; k++) {
			tendril(s, cr, curls[k][0], curls[k][1], curls[k][2]);
		}
		cairo_set_line_width(cr, fmax(0.6, u * 0.7));
		cairo_set_source_rgba(cr, 0.35, 0.55, 0.18, 0.9);
		stroke_everywhere(s, cr);
	}
	cairo_destroy(cr);
}

/* A leaf: growing now, drawn with the slow parts, and into the plants once grown. */
static void add_leaf(struct jungle *s, cairo_t *plants, float x, float y, float angle, float size) {
	struct leaf l = { x, y, angle, size, (float)s->t, (uint8_t)(saver_random() * LEAF_KINDS),
		(uint8_t)(saver_random() * LEAF_SHADES) };
	if (s->leaf_count < LEAVES_MAX) {
		s->leaves[s->leaf_count++] = l;
	}
	(void)plants;
}

static void draw_leaf(struct jungle *s, cairo_t *cr, struct leaf *l, double grown) {
	double k = l->size / s->leaf_size * grown;
	if (k <= 0.01) {
		return;
	}
	cairo_save(cr);
	cairo_translate(cr, l->x, l->y);
	cairo_rotate(cr, l->angle);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s->leaf_img[l->kind][l->shade], 0, -s->leaf_size / 2.0);
	// (the default filter works out a scaled down picture far more carefully, and slowly)
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* Leaves grown in full go into the plants for good. */
static void settle_leaves(struct jungle *s) {
	cairo_t *cr[3] = { NULL };
	cairo_surface_t *into[3] = { s->plants, s->scene, s->next };
	int kept = 0;
	for (int i = 0; i < s->leaf_count; i++) {
		struct leaf *l = &s->leaves[i];
		if (s->t - l->born >= LEAF_GROWS) {
			for (int k = 0; k < 3; k++) {
				if (!cr[k]) {
					cr[k] = cairo_create(into[k]);
				}
				draw_leaf(s, cr[k], l, 1);
			}
			s->leaves_grown++;
		} else {
			s->leaves[kept++] = *l;
		}
	}
	s->leaf_count = kept;
	for (int k = 0; k < 3; k++) {
		if (cr[k]) {
			cairo_destroy(cr[k]);
		}
	}
}

/* ---------- flowers ---------- */

static const double flower_colours[6][3] = {
	{ 0.9, 0.12, 0.2 }, { 0.72, 0.3, 0.85 }, { 1, 0.78, 0.15 }, { 0.98, 0.95, 0.9 },
	{ 1, 0.45, 0.65 }, { 1, 0.5, 0.1 },
};

static struct bit *new_bit(struct jungle *s) {
	struct bit *b = &s->bits[s->bit_next];
	s->bit_next = (s->bit_next + 1) % BITS_MAX;
	return b;
}

/* Buds open, flowers stay, wilt, drop their petals, rest, and bloom again. */
static void flowers_step(struct jungle *s, double dt) {
	for (int i = 0; i < s->flower_count; i++) {
		struct flower *f = &s->flowers[i];
		f->t += (float)dt;
		if (f->t < f->hold) {
			continue;
		}
		f->t = 0;
		switch (f->state) {
		case BUD: f->state = OPENING; f->hold = 3; break;
		case OPENING: f->state = OPEN; f->hold = (float)saver_between(20, 50); s->blooms++; break;
		case OPEN: f->state = WILTING; f->hold = 6; break;
		case WILTING:
			f->state = RESTING;
			f->hold = (float)saver_between(8, 30);
			for (int k = 0; k < f->petals; k++) {
				const double *c = flower_colours[f->colour];
				*new_bit(s) = (struct bit){ f->x, f->y, (float)(saver_between(-20, 20) * s->u), 0,
					(float)saver_between(0, 6.3), (float)saver_between(-3, 3), (float)saver_between(4, 8), 0,
					f->size * 0.5f, 0, 0xff000000u | (uint32_t)(c[0] * 200) << 16 |
					(uint32_t)(c[1] * 200) << 8 | (uint32_t)(c[2] * 200) };
			}
			break;
		default:
			f->state = BUD;
			f->hold = (float)saver_between(2, 5);
			if (saver_random() < 0.35) {
				f->colour = (uint8_t)(saver_random() * 6); // another colour this time
			}
			break;
		}
	}
}

/* How open a flower is, 0 a bud to 1; wilting, how far. */
static void flower_look(struct flower *f, double *open, double *wilt) {
	*wilt = 0;
	switch (f->state) {
	case BUD: *open = 0.12 * f->t / f->hold; break;
	case OPENING: *open = 0.12 + 0.88 * smooth(0, 1, f->t / f->hold); break;
	case OPEN: *open = 1; break;
	case WILTING: *open = 1 - 0.3 * f->t / f->hold; *wilt = f->t / f->hold; break;
	default: *open = 0; break;
	}
}

/* A flower open in full, of a colour and a number of petals, fresh or wilted. */
static cairo_surface_t *make_flower(int size, int colour, int petals, bool wilted) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	cairo_translate(cr, size / 2.0, size / 2.0);
	double r = size / 2.0 / 1.12, wilt = wilted ? 1 : 0;
	const double *c = flower_colours[colour];
	// the petals, drooping and darkening as it wilts
	for (int k = 0; k < petals; k++) {
		cairo_save(cr);
		cairo_rotate(cr, k * 2 * M_PI / petals);
		cairo_translate(cr, r * 0.5, 0);
		cairo_scale(cr, r * 0.55, r * (0.3 - 0.12 * wilt));
		cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
		cairo_restore(cr);
	}
	double dim = 1 - 0.45 * wilt;
	cairo_pattern_t *p = cairo_pattern_create_radial(0, 0, r * 0.1, 0, 0, r * 1.05);
	cairo_pattern_add_color_stop_rgb(p, 0, c[0] * 0.55 * dim, c[1] * 0.4 * dim, c[2] * 0.4 * dim);
	cairo_pattern_add_color_stop_rgb(p, 0.5, c[0] * dim, c[1] * dim, c[2] * dim);
	cairo_pattern_add_color_stop_rgb(p, 1, fmin(1, c[0] * 1.15) * dim, fmin(1, c[1] * 1.15) * dim,
		fmin(1, c[2] * 1.15) * dim);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	// the heart of it
	cairo_arc(cr, 0, 0, fmax(1, r * 0.18), 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 0.85 * dim, 0.3 * dim);
	cairo_fill(cr);
	cairo_destroy(cr);
	return img;
}

/* The flowers, from their pictures: opening, swaying, wilting into the wilted one. */
static void draw_flowers(struct jungle *s, cairo_t *cr) {
	int half = s->flower_px / 2;
	for (int i = 0; i < s->flower_count; i++) {
		struct flower *f = &s->flowers[i];
		double open, wilt;
		flower_look(f, &open, &wilt);
		if (open <= 0.02) {
			continue;
		}
		double k = f->size * open / (half / 1.12), sway = sin(s->t * 1.3 + f->spin) * 0.08 * (1 + s->wind);
		int petals = f->petals > 5;
		cairo_save(cr);
		cairo_translate(cr, f->x, f->y + wilt * f->size * 0.4);
		cairo_rotate(cr, f->spin + sway);
		cairo_scale(cr, k, k * (1 - 0.35 * wilt));
		if (wilt < 1) {
			cairo_set_source_surface(cr, s->flower_img[f->colour][petals][0], -half, -half);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_paint_with_alpha(cr, 1 - wilt);
		}
		if (wilt > 0) {
			cairo_set_source_surface(cr, s->flower_img[f->colour][petals][1], -half, -half);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_paint_with_alpha(cr, wilt);
		}
		cairo_restore(cr);
	}
}

/* ---------- the giant fronds ---------- */

static void plan_fronds(struct jungle *s) {
	double W = s->width, H = s->height, u = s->u, span = s->span;
	// from the corners and edges, pointing in and up (or down from the top corners)
	static const double spots[FRONDS_MAX][3] = {
		{ 0, 1, -0.6 }, { 1, 1, M_PI + 0.6 }, { 0, 0.55, -0.15 }, { 1, 0.6, M_PI + 0.2 },
		{ 0.3, 1, -1.2 }, { 0.72, 1, M_PI + 1.25 }, { 0, 0, 0.7 }, { 1, 0, M_PI - 0.75 },
		{ 0.5, 1, -1.5 },
	};
	for (int k = 0; k < FRONDS_MAX; k++) {
		s->fronds[k] = (struct frond){ (float)(spots[k][0] * W), (float)(spots[k][1] * H + u * 10),
			(float)(spots[k][2] + saver_between(-0.15, 0.15)), (float)(u * saver_between(280, 420)),
			(float)(span * (0.22 + 0.075 * k + saver_between(0, 0.05))), (float)saver_between(0, 6.3),
			k % FROND_KINDS };
	}
	s->frond_count = FRONDS_MAX;
}

static void draw_fronds(struct jungle *s, cairo_t *cr, int from, int to) {
	for (int k = from; k < to && k < s->frond_count; k++) {
		struct frond *f = &s->fronds[k];
		if (s->t < f->born) {
			continue;
		}
		double grown = smooth(0, 25, (float)(s->t - f->born));
		cairo_surface_t *img = s->frond_img[f->kind];
		int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
		double sway = sin(s->t * 0.55 + f->phase) * 0.05 * (1 + 1.5 * s->wind) +
			sin(s->t * 1.7 + f->phase) * 0.01;
		double k2 = f->length / iw * (0.3 + 0.7 * grown);
		cairo_save(cr);
		cairo_translate(cr, f->x, f->y);
		cairo_rotate(cr, f->angle + sway - (1 - grown) * 0.6); // unfurling up into place
		cairo_scale(cr, k2, k2 * (0.35 + 0.65 * grown));
		cairo_set_source_surface(cr, img, 0, -ih / 2.0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		if (grown * 3 >= 1) {
			cairo_paint(cr);
		} else {
			cairo_paint_with_alpha(cr, grown * 3);
		}
		cairo_restore(cr);
	}
}

/* ---------- light ---------- */

/* Shafts of light through the canopy, drawn once; they drift by sliding the picture. */
static cairo_surface_t *make_rays(int width, int height) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(img);
	double W = width, H = height;
	for (int k = 0; k < 4; k++) {
		double x = W * (0.12 + 0.25 * k), w = W * (0.03 + 0.02 * (k % 2)), slant = H * (k % 2 ? -0.25 : 0.3);
		cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + slant, H);
		cairo_pattern_add_color_stop_rgba(p, 0, 0.9, 1, 0.6, 0.16);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.9, 1, 0.6, 0);
		cairo_move_to(cr, x, 0);
		cairo_line_to(cr, x + w, 0);
		cairo_line_to(cr, x + w * 2.5 + slant, H);
		cairo_line_to(cr, x + slant, H);
		cairo_close_path(cr);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	cairo_destroy(cr);
	return img;
}

static void draw_light(struct jungle *s, cairo_t *cr) {
	double W = s->width, day = 1 - s->night;
	if (day > 0.05) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		cairo_set_source_surface(cr, s->rays, round(sin(s->t * 0.07) * W * 0.04), 0);
		cairo_paint_with_alpha(cr, day);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	if (s->night > 0.02) {
		cairo_set_source_rgba(cr, 0.01, 0.03, 0.08, 0.62 * s->night);
		cairo_paint(cr);
	}
}

/* The slow parts, built into the next picture a part a frame (-1: all at once), then shown. */
static void build_scene(struct jungle *s, int part) {
	cairo_t *cr = cairo_create(s->next);
	if (part <= 0) {
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_surface(cr, s->view, 0, 0);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_set_source_surface(cr, s->plants, 0, 0);
		cairo_paint(cr);
	}
	if (part < 0 || part == 1) {
		cairo_surface_t *shown = s->scene;
		s->scene = s->next;
		s->next = shown;
	}
	cairo_destroy(cr);
}

/* ---------- the saver ---------- */

static void *jungle_create(int width, int height, const struct saver_options *options) {
	struct jungle *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	double u = s->u = saver_unit(width, height);
	static const double spans[] = { 360, 720, 150 };
	s->span = spans[saver_choice(options, &saver_doomsday, "jungle_pace")];
	s->creatures = saver_toggle(options, &saver_doomsday, "jungle_creatures");
	s->nights = saver_toggle(options, &saver_doomsday, "jungle_nights");
	s->orig = saver_desktop(options, width, height, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	s->lush = lush_light(s->orig, width, height);
	s->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_t *cr = cairo_create(s->view);
	cairo_set_source_surface(cr, s->orig, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	s->plants = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->scene = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->next = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	s->leaf_size = (int)fmax(12, u * 48);
	for (int k = 0; k < LEAF_KINDS; k++) {
		for (int c = 0; c < LEAF_SHADES; c++) {
			s->leaf_img[k][c] = make_leaf(s->leaf_size, k, c);
		}
	}
	for (int k = 0; k < FROND_KINDS; k++) {
		s->frond_img[k] = make_frond((int)fmax(40, u * 420), k);
	}
	s->glow = make_glow(fmax(2, u * 7));
	s->rays = make_rays(width, height);
	s->flower_px = (int)fmax(10, u * 40);
	for (int c = 0; c < 6; c++) {
		for (int p = 0; p < 2; p++) {
			for (int w = 0; w < 2; w++) {
				s->flower_img[c][p][w] = make_flower(s->flower_px, c, 5 + p, w);
			}
		}
	}
	size_t n = (size_t)width * height;
	s->moss_at = malloc(sizeof(float) * n);
	s->moss_tex = malloc(n);
	s->row_first = malloc(sizeof(float) * height);
	s->row_last = malloc(sizeof(float) * height);
	s->working = pthread_create(&s->worker, NULL, grow_moss, s) == 0;
	if (!s->working) {
		grow_moss(s);
	}
	plan_vines(s);
	plan_fronds(s);
	for (int i = 0; i < BUTTERFLIES; i++) {
		s->butterflies[i] = (struct butterfly){ (float)(saver_random() * width),
			(float)(saver_random() * height), 0, 0, (float)saver_between(0, 6.3), 0,
			(float)(saver_random()), -1 };
	}
	for (int i = 0; i < FIREFLIES; i++) {
		s->flies[i][0] = (float)(saver_random() * width);
		s->flies[i][1] = (float)(height * saver_between(0.2, 1));
		s->flies[i][2] = (float)saver_between(0, 6.3);
		s->flies[i][3] = (float)saver_between(0, 6.3);
	}
	s->intro_rows = ROW_STEP + 1;
	s->day = -M_PI / 2; // it starts in the day
	build_scene(s, -1);
	return s;
}

static void draw_butterfly(struct jungle *s, cairo_t *cr, struct butterfly *b) {
	double u = s->u, size = u * 9;
	double open = fabs(sin(b->flap));
	double r, g, bl;
	saver_hsv(b->hue, 0.85, 0.95, &r, &g, &bl);
	cairo_save(cr);
	cairo_translate(cr, b->x, b->y);
	cairo_rotate(cr, atan2(b->vy, b->vx) * 0.15);
	for (int side = -1; side <= 1; side += 2) {
		cairo_save(cr);
		cairo_scale(cr, side * (0.2 + 0.8 * open), 1);
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, size * 0.3, -size * 1.2, size * 1.3, -size * 1.1, size * 1.1, -size * 0.1);
		cairo_curve_to(cr, size * 1.2, size * 0.5, size * 0.5, size * 1.1, 0, size * 0.2);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, r, g, bl);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, fmax(0.6, u * 0.8));
		cairo_set_source_rgba(cr, 0.05, 0.03, 0.02, 0.8);
		cairo_stroke(cr);
		cairo_restore(cr);
	}
	cairo_move_to(cr, 0, -size * 0.5);
	cairo_line_to(cr, 0, size * 0.5);
	cairo_set_line_width(cr, fmax(1, u * 1.4));
	cairo_set_source_rgb(cr, 0.08, 0.05, 0.03);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void jungle_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct jungle *s = state;
	double u = s->u, W = width, H = height;
	s->t += dt;
	s->frame++;
	// days and nights, and the breeze
	if (s->nights) {
		s->day += dt * 2 * M_PI / 90;
		s->night = smooth(0.1f, 0.7f, (float)sin(s->day)) * smooth(10, 30, (float)s->t);
	}
	s->wind = 0.5 + 0.5 * sin(s->t * 0.13) * sin(s->t * 0.037 + 1);
	update_rows(s);
	grow_vines(s, dt);
	flowers_step(s, dt);
	settle_leaves(s);
	if (s->part < 2) {
		build_scene(s, s->part); // the first part after the whole one built at the start
	}
	s->part = (s->part + 1) % SCENE_EVERY;
	// the ground, mossed and grown over; then what moves on it, every frame
	cairo_set_source_surface(cr, s->scene, 0, 0);
	cairo_paint(cr);
	for (int i = 0; i < s->leaf_count; i++) {
		struct leaf *l = &s->leaves[i];
		draw_leaf(s, cr, l, smooth(0, LEAF_GROWS, (float)(s->t - l->born)));
	}
	draw_flowers(s, cr);
	draw_fronds(s, cr, 0, FRONDS_MAX);
	draw_light(s, cr);
	// leaves now and then let go of the canopy, and pollen drifts
	if (saver_random() < dt * 0.8 * fmin(1, progress(s) * 3)) {
		*new_bit(s) = (struct bit){ (float)(saver_random() * W), (float)(-u * 10),
			(float)(saver_between(-10, 10) * u), 0, (float)saver_between(0, 6.3),
			(float)saver_between(-2, 2), 14, 0, (float)(u * saver_between(8, 14)), 1, 0 };
	}
	if (saver_random() < dt * 6) {
		*new_bit(s) = (struct bit){ (float)(saver_random() * W), (float)(saver_random() * H), 0, 0,
			0, 0, (float)saver_between(4, 9), 0, (float)(u * saver_between(0.8, 1.8)), 2, 0 };
	}
	for (int i = 0; i < BITS_MAX; i++) {
		struct bit *b = &s->bits[i];
		if (b->age >= b->life) {
			continue;
		}
		b->age += (float)dt;
		if (b->kind == 2) {
			b->x += (float)((sin(b->age + i) * 6 + s->wind * 10) * u * dt);
			b->y += (float)(cos(b->age * 0.7 + i) * 4 * u * dt);
			cairo_rectangle(cr, b->x, b->y, b->size, b->size);
			cairo_set_source_rgba(cr, 1, 0.95, 0.6, 0.5 * (1 - s->night) * sin(M_PI * b->age / b->life));
			cairo_fill(cr);
			continue;
		}
		// fluttering down, sideways with the breeze
		b->vx += (float)((s->wind * 25 * u - b->vx) * dt + sin(b->age * 3 + i) * u * 40 * dt);
		b->vy = (float)(u * (b->kind ? 30 : 22) * (1 + 0.5 * sin(b->age * 2.3 + i)));
		b->x += b->vx * (float)dt;
		b->y += b->vy * (float)dt;
		b->angle += b->spin * (float)dt;
		if (b->y > H + 20) {
			b->age = b->life;
			continue;
		}
		double fade = fmin(1, (b->life - b->age) * 2);
		cairo_save(cr);
		cairo_translate(cr, b->x, b->y);
		cairo_rotate(cr, b->angle);
		if (b->kind == 1) {
			double k = b->size / s->leaf_size;
			cairo_scale(cr, k, k * fabs(cos(b->age * 2.5 + i)) + 0.05);
			cairo_set_source_surface(cr, s->leaf_img[i % LEAF_KINDS][4], -s->leaf_size / 2.0,
				-s->leaf_size / 2.0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_paint_with_alpha(cr, fade);
		} else {
			cairo_scale(cr, b->size, b->size * 0.55 * (0.3 + 0.7 * fabs(cos(b->age * 3 + i))));
			cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
			uint32_t c = b->colour;
			cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
				(c & 255) / 255.0, fade);
			cairo_fill(cr);
		}
		cairo_restore(cr);
	}
	if (!s->creatures) {
		return;
	}
	// butterflies in the day, settling on open flowers now and then
	double day = 1 - s->night;
	for (int i = 0; i < BUTTERFLIES && day > 0.2; i++) {
		struct butterfly *b = &s->butterflies[i];
		if (b->perch >= 0) {
			struct flower *f = &s->flowers[b->perch];
			b->rest -= (float)dt;
			b->flap += (float)(dt * 2);
			b->x = f->x;
			b->y = f->y - (float)(u * 3);
			if (b->rest <= 0 || f->state != OPEN) {
				b->perch = -1;
				b->vy = (float)(-u * 60);
			}
		} else {
			b->flap += (float)(dt * 22);
			b->vx += (float)(saver_between(-1, 1) * u * 400 * dt);
			b->vy += (float)(saver_between(-1, 1) * u * 400 * dt + sin(b->flap) * u * 80 * dt);
			float speed = hypotf(b->vx, b->vy), most = (float)(u * 90);
			if (speed > most) {
				b->vx *= most / speed;
				b->vy *= most / speed;
			}
			// back in from the edges
			if (b->x < W * 0.05) b->vx += (float)(u * 200 * dt);
			if (b->x > W * 0.95) b->vx -= (float)(u * 200 * dt);
			if (b->y < H * 0.05) b->vy += (float)(u * 200 * dt);
			if (b->y > H * 0.9) b->vy -= (float)(u * 200 * dt);
			b->x += b->vx * (float)dt;
			b->y += b->vy * (float)dt;
			for (int k = 0; k < s->flower_count && saver_random() < dt * 2; k++) {
				struct flower *f = &s->flowers[(int)(saver_random() * s->flower_count)];
				if (f->state == OPEN && hypotf(f->x - b->x, f->y - b->y) < u * 120) {
					b->perch = (int)(f - s->flowers);
					b->rest = (float)saver_between(2, 6);
					break;
				}
			}
		}
		draw_butterfly(s, cr, b);
	}
	// fireflies at night
	if (s->night > 0.05) {
		int half = cairo_image_surface_get_width(s->glow) / 2;
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		for (int i = 0; i < FIREFLIES; i++) {
			float *f = s->flies[i];
			f[2] += (float)dt;
			f[0] += (float)(sin(f[2] * 0.4 + f[3]) * u * 18 * dt);
			f[1] += (float)(cos(f[2] * 0.3 + f[3] * 2) * u * 12 * dt);
			double pulse = pow(fmax(0, sin(f[2] * 1.7 + f[3])), 3);
			if (pulse < 0.02) {
				continue;
			}
			cairo_set_source_surface(cr, s->glow, round(f[0]) - half, round(f[1]) - half);
			cairo_paint_with_alpha(cr, pulse * s->night);
		}
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
}

static void jungle_destroy(void *state) {
	struct jungle *s = state;
	if (s->working) {
		pthread_join(s->worker, NULL);
	}
	cairo_surface_destroy(s->orig);
	cairo_surface_destroy(s->lush);
	cairo_surface_destroy(s->view);
	cairo_surface_destroy(s->plants);
	cairo_surface_destroy(s->scene);
	cairo_surface_destroy(s->next);
	cairo_surface_destroy(s->glow);
	cairo_surface_destroy(s->rays);
	for (int c = 0; c < 6; c++) {
		for (int p = 0; p < 2; p++) {
			for (int w = 0; w < 2; w++) {
				cairo_surface_destroy(s->flower_img[c][p][w]);
			}
		}
	}
	for (int k = 0; k < LEAF_KINDS; k++) {
		for (int c = 0; c < LEAF_SHADES; c++) {
			cairo_surface_destroy(s->leaf_img[k][c]);
		}
	}
	for (int k = 0; k < FROND_KINDS; k++) {
		cairo_surface_destroy(s->frond_img[k]);
	}
	free(s->moss_at);
	free(s->moss_tex);
	free(s->row_first);
	free(s->row_last);
	free(s);
}

void saver_jungle_stats(void *state, struct jungle_stats *out) {
	struct jungle *s = state;
	if (s->working) {
		pthread_join(s->worker, NULL);
		s->working = false;
	}
	*out = (struct jungle_stats){ .vines = s->vine_count, .leaves = s->leaves_grown + s->leaf_count,
		.flowers = s->flower_count, .blooms = s->blooms, .progress = progress(s),
		.night = s->night, .inside = true };
	for (int i = 0; i < s->vine_count; i++) {
		out->growing += s->vines[i].alive && s->t >= s->vines[i].start;
		out->length += s->vines[i].len;
	}
	for (int i = 0; i < s->flower_count; i++) {
		out->open += s->flowers[i].state == OPEN;
	}
	for (int k = 0; k < s->frond_count; k++) {
		out->fronds += s->t >= s->fronds[k].born;
	}
	for (int i = 0; i < BUTTERFLIES; i++) {
		struct butterfly *b = &s->butterflies[i];
		if (!isfinite(b->x) || !isfinite(b->y) || b->x < -s->width * 0.2 || b->x > s->width * 1.2 ||
				b->y < -s->height * 0.2 || b->y > s->height * 1.2) {
			out->inside = false;
		}
	}
	long mossy = 0, all = 0;
	for (int i = 0; i < s->width * s->height; i += 7) {
		mossy += s->moss_at[i] <= s->t;
		all++;
	}
	out->moss = all ? (double)mossy / all : 0;
	// how much of the screen the plants cover
	cairo_surface_flush(s->plants);
	const uint32_t *p = (const uint32_t *)cairo_image_surface_get_data(s->plants);
	int stride = cairo_image_surface_get_stride(s->plants) / 4;
	long covered = 0, seen = 0;
	for (int y = 0; y < s->height; y += 3) {
		for (int x = 0; x < s->width; x += 3) {
			covered += (p[y * stride + x] >> 24) > 128;
			seen++;
		}
	}
	out->covered = seen ? (double)covered / seen : 0;
}

const struct saver saver_jungle = {
	.name = "jungle",
	.title = "Jungle",
	.description = "The desktop overgrown: moss, vines, leaves, flowers, butterflies",
	.wants_desktop = true,
	.covers = true,
	.create = jungle_create,
	.draw = jungle_draw,
	.destroy = jungle_destroy,
};
