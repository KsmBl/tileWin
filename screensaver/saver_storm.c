#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Thunderstorm: the desktop under a storm. Its light goes grey and cold,
 * clouds roll along the top, and the rain comes down in three depths: a far
 * veil behind the windows, rain that drums on the tops of the windows and the
 * taskbar and splashes there, and heavy near streaks in front of it all. Water
 * gathers in drops on the glass of the windows, runs down them in wandering
 * trails and drips off their corners and lower edges; the taskbar is wet and
 * shines. Lightning forks down out of the clouds, into the distance, into the
 * taskbar or into a window, whose light goes out for a moment and which keeps
 * a smoking scorch mark; each flash shows the desktop in its true colours for
 * an instant, and the thunder follows, the nearer the sooner and the harder
 * it shakes. Gusts slant the rain and drive it in sheets, and now and then a
 * big drop runs down the screen itself.
 *
 * The windows and the taskbar are the real ones: the picture of the screen
 * from before the saver, and where they are from tileWin.
 */

#define WINDOWS_MAX 48
#define FAR_DROPS 900
#define MID_DROPS 650
#define NEAR_DROPS 110
#define SPLASHES_MAX 500
#define DRIPS_MAX 160
#define BEADS_MAX 320     // drops on the glass of the windows
#define LENS_MAX 7        // drops on the screen itself
#define BOLT_POINTS 900
#define SPARKS_MAX 160
#define SCORCH_MAX 8
#define BEAD_SIZES 6      // drops on the glass, drawn beforehand in these sizes

struct drop {
	float x, y, speed, len;
};

struct splash {
	float x, y, vx, vy, life, age;
	bool ring;
};

struct drip {
	float x, y, vy;
	bool alive;
};

struct bead {
	int win;
	float x, y, r;       // on the screen
	float sx, sy;        // where it started sliding, for its trail
	float vy, wobble;
	bool sliding, alive;
};

struct lens {
	float x, y, r, vy, life;
	bool alive;
};

struct bolt_point {
	float x, y;
	uint8_t level;       // 0 the main channel, more for branches
	bool start;          // a new line begins here
};

struct spark {
	float x, y, vx, vy, life, age;
};

struct scorch {
	float x, y, age;
	bool alive;
};

struct storm {
	int width, height;
	double u, t;
	cairo_surface_t *dark, *bright; // the desktop in storm light, and in its own colours
	cairo_surface_t *clouds[2];     // two layers, each tileable sideways
	cairo_surface_t *bead_img[BEAD_SIZES][2]; // a drop, and lit by a flash
	float bead_r[BEAD_SIZES];
	int cloud_h;
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	float *roof;         // per column: the first surface rain lands on
	struct drop far[FAR_DROPS], mid[MID_DROPS], near[NEAR_DROPS];
	struct splash splashes[SPLASHES_MAX];
	int splash_next;
	struct drip drips[DRIPS_MAX];
	struct bead beads[BEADS_MAX];
	struct lens lens[LENS_MAX];
	struct spark sparks[SPARKS_MAX];
	int spark_next;
	struct scorch scorch[SCORCH_MAX];
	// lightning
	struct bolt_point bolt[BOLT_POINTS];
	int bolt_len;
	double bolt_t;       // since the strike, < 0 for none
	double next_bolt;
	double thunder_at, thunder_power, shake;
	int struck_win;      // a window hit, -1 for none
	bool struck_bar;
	double struck_t;
	double bolt_x, bolt_y; // where it came out of the clouds
	// the wind
	double wind, gust;
	// its settings
	double rain_amount, bolt_pace;
	bool hits;           // lightning into the windows and the taskbar
};

/* ---------- noise ---------- */

static uint32_t hash2(int x, int y, uint32_t seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/* Value noise that repeats every period cells sideways, so the clouds can scroll round. */
static float tile_noise(float x, float y, int period, uint32_t seed) {
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = ((xi % period) + period) % period, x1 = (x0 + 1) % period;
	float a = (hash2(x0, yi, seed) & 0xffff) / 65535.0f;
	float b = (hash2(x1, yi, seed) & 0xffff) / 65535.0f;
	float c = (hash2(x0, yi + 1, seed) & 0xffff) / 65535.0f;
	float d = (hash2(x1, yi + 1, seed) & 0xffff) / 65535.0f;
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* ---------- the pictures ---------- */

/* The desktop in the grey, cold light of a storm. */
static cairo_surface_t *storm_light(cairo_surface_t *desktop, int width, int height) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_surface_flush(desktop);
	const uint32_t *src = (const uint32_t *)cairo_image_surface_get_data(desktop);
	uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(out);
	int ss = cairo_image_surface_get_stride(desktop) / 4, ds = cairo_image_surface_get_stride(out) / 4;
	for (int y = 0; y < height; y++) {
		// darker towards the top, under the clouds
		float light = 0.3f + 0.12f * y / height;
		for (int x = 0; x < width; x++) {
			uint32_t p = src[y * ss + x];
			float r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			float lum = r * 0.3f + g * 0.59f + b * 0.11f;
			// most of the colour gone, a blue cast, the whole dim
			r = (r * 0.35f + lum * 0.65f) * light * 0.85f;
			g = (g * 0.35f + lum * 0.65f) * light * 0.95f;
			b = (b * 0.35f + lum * 0.65f) * light * 1.2f + 6;
			dst[y * ds + x] = 0xff000000u | (uint32_t)fminf(255, r) << 16 |
				(uint32_t)fminf(255, g) << 8 | (uint32_t)fminf(255, b);
		}
	}
	cairo_surface_mark_dirty(out);
	return out;
}

/* A layer of cloud: heavy, dark and ragged at its lower edge, repeating sideways. */
static cairo_surface_t *make_clouds(int width, int height, float scale, uint32_t seed,
		float shade) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(out);
	int stride = cairo_image_surface_get_stride(out) / 4;
	float cell = 160 * scale; // pixels of the coarsest noise
	int period = (int)fmaxf(1, roundf(width / cell));
	cell = (float)width / period;
	for (int y = 0; y < height; y++) {
		float fall = (float)y / height; // thinning out downwards
		for (int x = 0; x < width; x++) {
			float n = 0, amp = 0.5f, total = 0;
			int p = period;
			float fx = x / cell, fy = y / cell;
			for (int o = 0; o < 5; o++) {
				n += tile_noise(fx, fy, p, seed + o) * amp;
				total += amp;
				fx *= 2;
				fy *= 2;
				p *= 2;
				amp *= 0.5f;
			}
			n /= total;
			float density = n * 1.5f - 0.2f - fall * 1.05f;
			density = density < 0 ? 0 : density > 1 ? 1 : density;
			density = density * density * (3 - 2 * density);
			// darker underneath, where no light comes through
			float c = shade * (0.8f + 0.6f * n) * (1 - 0.35f * fall);
			float a = density * 0.95f;
			uint32_t r = (uint32_t)(c * 0.9f * a * 255), g = (uint32_t)(c * 0.95f * a * 255),
				b = (uint32_t)(c * 1.1f * a * 255);
			px[y * stride + x] = (uint32_t)(a * 255) << 24 | (r > 255 ? 255 : r) << 16 |
				(g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
		}
	}
	cairo_surface_mark_dirty(out);
	return out;
}

/* ---------- the surfaces the rain lands on ---------- */

static void make_roofs(struct storm *s) {
	for (int x = 0; x < s->width; x++) {
		s->roof[x] = s->height;
	}
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		for (int x = (int)fmax(0, b->x); x < (int)fmin(s->width, b->x + b->w); x++) {
			s->roof[x] = fminf(s->roof[x], (float)b->y);
		}
	}
	for (int i = 0; i < s->count; i++) {
		struct saver_window *b = &s->wins[i];
		for (int x = (int)fmax(0, b->x); x < (int)fmin(s->width, b->x + b->w); x++) {
			s->roof[x] = fminf(s->roof[x], (float)b->y);
		}
	}
}

/* The first surface under a point: a window's top or the taskbar below it, or the bottom. */
static float surface_below(struct storm *s, float x, float y) {
	float best = s->height;
	for (int i = 0; i < s->count; i++) {
		struct saver_window *b = &s->wins[i];
		if (x >= b->x && x < b->x + b->w && b->y >= y && b->y < best) {
			best = (float)b->y;
		}
	}
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		if (x >= b->x && x < b->x + b->w && b->y >= y && b->y < best) {
			best = (float)b->y;
		}
	}
	return best;
}

/* Whether a point is on a window, and which: the topmost. */
static int window_at(struct storm *s, float x, float y) {
	for (int i = s->count - 1; i >= 0; i--) {
		struct saver_window *b = &s->wins[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			return i;
		}
	}
	return -1;
}

/* ---------- rain ---------- */

static void drop_reset(struct storm *s, struct drop *d, float speed, float len, bool anywhere) {
	double slant = s->wind * s->height;
	d->x = (float)saver_between(-fabs(slant) - 50, s->width + fabs(slant) + 50);
	d->y = anywhere ? (float)saver_between(-s->height * 0.2, s->height) :
		(float)saver_between(-s->height * 0.25, -10);
	d->speed = speed * (float)saver_between(0.85, 1.15);
	d->len = len * (float)saver_between(0.7, 1.3);
}

static void splash(struct storm *s, float x, float y, int bits, float power) {
	double u = s->u;
	for (int k = 0; k < bits; k++) {
		struct splash *p = &s->splashes[s->splash_next];
		s->splash_next = (s->splash_next + 1) % SPLASHES_MAX;
		double a = saver_between(-M_PI * 0.95, -M_PI * 0.05);
		double v = saver_between(40, 150) * u * power;
		*p = (struct splash){ x, y - 1, (float)(cos(a) * v + s->wind * 80 * u),
			(float)(sin(a) * v), (float)saver_between(0.2, 0.45), 0, false };
	}
	if (saver_random() < 0.5) {
		struct splash *p = &s->splashes[s->splash_next];
		s->splash_next = (s->splash_next + 1) % SPLASHES_MAX;
		*p = (struct splash){ x, y, 0, 0, 0.3f, 0, true };
	}
}

static void drip_from(struct storm *s, float x, float y) {
	for (int i = 0; i < DRIPS_MAX; i++) {
		if (!s->drips[i].alive) {
			s->drips[i] = (struct drip){ x, y, (float)(saver_between(0, 30) * s->u), true };
			return;
		}
	}
}

/* The rain in one depth: moved on, and drawn as one path of streaks. */
static void rain_layer(struct storm *s, cairo_t *cr, struct drop *drops, int n, double dt,
		bool lands, bool behind, float speed, float len, float width, double alpha) {
	double vx = s->wind * speed;
	cairo_new_path(cr);
	for (int i = 0; i < n; i++) {
		struct drop *d = &drops[i];
		float px = d->x, py = d->y;
		d->x += (float)(vx * d->speed / speed * dt);
		d->y += (float)(d->speed * dt);
		if (lands) {
			int col = (int)d->x;
			float roof = col >= 0 && col < s->width ? s->roof[col] : s->height;
			if (d->y >= roof && py < roof + 2) {
				if (saver_random() < 0.35) {
					splash(s, d->x, roof, 2 + (int)(saver_random() * 2), 1);
				}
				drop_reset(s, d, speed, len, false);
				continue;
			}
		}
		if (d->y - d->len > s->height) {
			drop_reset(s, d, speed, len, false);
			continue;
		}
		if (behind && window_at(s, d->x, d->y + d->len * 0.5f) >= 0) {
			continue; // behind a window
		}
		// the streak it leaves on the eye in a frame, along the way it falls
		double k = d->len / d->speed;
		cairo_move_to(cr, px, py);
		cairo_line_to(cr, px - vx * k * 0.2 + (d->x - px), py + d->len);
	}
	cairo_set_line_width(cr, width);
	cairo_set_source_rgba(cr, 0.72, 0.78, 0.9, alpha);
	// thin, fast streaks: the cheaper edge is as good to the eye
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
	cairo_stroke(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
}

/* ---------- water on the glass of the windows ---------- */

static void beads_step(struct storm *s, double dt, double rain) {
	double u = s->u;
	// new drops land on the glass
	int spawn = (int)(dt * 60 * rain * (s->count > 0));
	for (int k = 0; k < spawn; k++) {
		struct bead *b = NULL;
		for (int i = 0; i < BEADS_MAX && !b; i++) {
			b = s->beads[i].alive ? NULL : &s->beads[i];
		}
		if (!b || !s->count) {
			break;
		}
		int w = (int)(saver_random() * s->count);
		struct saver_window *win = &s->wins[w];
		float x = (float)(win->x + saver_random() * win->w);
		float y = (float)(win->y + win->title_h + saver_random() * (win->h - win->title_h));
		if (window_at(s, x, y) != w) {
			continue; // behind another window
		}
		*b = (struct bead){ w, x, y, (float)(saver_between(1.2, 3.2) * u), x, y, 0,
			(float)saver_random() * 6.28f, false, true };
	}
	for (int i = 0; i < BEADS_MAX; i++) {
		struct bead *b = &s->beads[i];
		if (!b->alive) {
			continue;
		}
		struct saver_window *win = &s->wins[b->win];
		if (!b->sliding) {
			b->r += (float)(dt * u * 0.35 * rain * saver_random()); // more rain joins it
			if (b->r > u * 4.2f) {
				b->sliding = true;
				b->sx = b->x;
				b->sy = b->y;
			}
			continue;
		}
		// down the glass, not quite straight, faster as it gathers what it meets
		b->vy = fminf(b->vy + (float)(dt * u * 260), (float)(u * 190));
		b->y += b->vy * (float)dt;
		b->wobble += (float)dt * 3;
		b->x += (float)(sin(b->wobble) * u * 12 * dt);
		if (b->y > win->y + win->h - b->r) {
			drip_from(s, b->x, (float)(win->y + win->h)); // off the lower edge
			b->alive = false;
		}
	}
}

/*
 * The water on the glass, all windows at once: the trails, then the drops, then
 * their glints, each in one go. A drop starts where its window shows and stays
 * inside it, so nothing needs cutting to the windows.
 */
static void draw_beads(struct storm *s, cairo_t *cr, double flash) {
	double u = s->u;
	cairo_new_path(cr);
	for (int i = 0; i < BEADS_MAX; i++) {
		struct bead *b = &s->beads[i];
		if (!b->alive || !b->sliding) {
			continue;
		}
		// the wet trail it leaves behind, wandering a little
		cairo_move_to(cr, b->sx, b->sy);
		for (float y = b->sy + (float)(u * 10); y < b->y; y += (float)(u * 10)) {
			float k = (y - b->sy) / fmaxf(1, b->y - b->sy);
			cairo_line_to(cr, b->sx + (b->x - b->sx) * k + sin(y * 0.05 + b->win) * u * 1.5, y);
		}
		cairo_line_to(cr, b->x, b->y);
	}
	cairo_set_line_width(cr, fmax(1, u * 2.2));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, 0.75, 0.82, 0.95, 0.16 + 0.25 * flash);
	cairo_stroke(cr);
	// the drops, as drawn beforehand in the nearest size, lit ones in a flash
	int lit = flash > 0.3;
	for (int i = 0; i < BEADS_MAX; i++) {
		struct bead *b = &s->beads[i];
		if (!b->alive) {
			continue;
		}
		int k = 0;
		while (k < BEAD_SIZES - 1 && s->bead_r[k + 1] <= b->r) {
			k++;
		}
		cairo_surface_t *img = s->bead_img[k][lit];
		int half = cairo_image_surface_get_width(img) / 2;
		cairo_set_source_surface(cr, img, round(b->x) - half, round(b->y) - half);
		cairo_paint(cr);
	}
}

/* A drop on glass: clear inside a darker rim, a glint where the light is. */
static cairo_surface_t *make_bead(double r, double u, bool lit) {
	int size = (int)ceil(r * 2 + 4);
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(img);
	double c = size / 2.0;
	cairo_arc(cr, c, c, r, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.55, 0.62, 0.75, lit ? 0.5 : 0.3);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 0.05, 0.07, 0.12, 0.35);
	cairo_set_line_width(cr, fmax(0.8, u * 0.8));
	cairo_stroke(cr);
	cairo_arc(cr, c - r * 0.35, c - r * 0.35, r * 0.3, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, lit ? 1 : 0.55);
	cairo_fill(cr);
	cairo_destroy(cr);
	return img;
}

/* ---------- lightning ---------- */

/* A jagged channel from a to b, forking now and then; appended to the bolt. */
static void bolt_line(struct storm *s, float ax, float ay, float bx, float by, int level,
		int depth) {
	if (s->bolt_len >= BOLT_POINTS - 2) {
		return;
	}
	float len = hypotf(bx - ax, by - ay);
	int steps = (int)fmaxf(4, len / (float)(s->u * 14));
	s->bolt[s->bolt_len++] = (struct bolt_point){ ax, ay, (uint8_t)level, true };
	float nx = -(by - ay) / len, ny = (bx - ax) / len; // across the way it goes
	float drift = 0;
	for (int k = 1; k <= steps && s->bolt_len < BOLT_POINTS - 1; k++) {
		float f = (float)k / steps;
		drift += (float)saver_between(-1, 1) * len / steps * 0.9f;
		drift *= 0.85f;
		float x = ax + (bx - ax) * f + nx * drift, y = ay + (by - ay) * f + ny * drift;
		if (k == steps) {
			x = bx;
			y = by;
		}
		s->bolt[s->bolt_len++] = (struct bolt_point){ x, y, (uint8_t)level, false };
		// a fork, shorter and going off to a side, down
		if (depth < 3 && k < steps - 2 && saver_random() < 0.09 / (depth + 1)) {
			float angle = atan2f(by - ay, bx - ax) + (float)saver_between(-1.1, 1.1);
			float blen = len * (float)saver_between(0.15, 0.4) / (depth + 1);
			bolt_line(s, x, y, x + cosf(angle) * blen, y + fabsf(sinf(angle)) * blen, level + 1,
				depth + 1);
			// back on the main channel: it goes on from here
			s->bolt[s->bolt_len++] = (struct bolt_point){ x, y, (uint8_t)level, true };
		}
	}
}

static void strike(struct storm *s) {
	s->bolt_len = 0;
	s->bolt_t = 0;
	s->struck_win = -1;
	s->struck_bar = false;
	double r = saver_random();
	if (!s->hits && r < 0.5) {
		r = 0.5 + r; // no strikes into the windows or the taskbar: elsewhere
	}
	float sx = (float)saver_between(0.05, 0.95) * s->width;
	float sy = (float)saver_between(0.06, 0.2) * s->height;
	float tx, ty;
	if (r < 0.35 && s->count > 0) {
		// into a window: the middle of its title bar, somewhere along it
		int w = (int)(saver_random() * s->count);
		struct saver_window *win = &s->wins[w];
		tx = (float)(win->x + saver_between(0.15, 0.85) * win->w);
		ty = (float)(win->y + fmax(2, win->title_h * 0.5));
		if (window_at(s, tx, ty) != w) {
			w = window_at(s, tx, ty);
		}
		s->struck_win = w;
	} else if (r < 0.5 && s->bar_count > 0) {
		struct saver_window *bar = &s->bars[0];
		tx = (float)(bar->x + saver_between(0.1, 0.9) * bar->w);
		ty = (float)bar->y;
		s->struck_bar = true;
	} else if (r < 0.8) {
		// far away, into the ground beyond the desktop
		tx = sx + (float)saver_between(-0.2, 0.2) * s->width;
		ty = (float)(s->height * saver_between(0.55, 0.9));
	} else {
		// within the clouds, from one to another
		tx = sx + (float)saver_between(-0.4, 0.4) * s->width;
		ty = sy + (float)saver_between(0, 0.12) * s->height;
	}
	if (s->struck_win >= 0 || s->struck_bar || r < 0.8) {
		// down from well above it, from beyond the top of the screen if need be
		sy = fminf(sy, ty - (float)(s->height * saver_between(0.35, 0.6)));
		float reach = (ty - sy) * 0.45f;
		sx = tx + (float)saver_between(-reach, reach);
	}
	s->bolt_x = sx;
	s->bolt_y = sy;
	bolt_line(s, sx, sy, tx, ty, 0, 0);
	s->struck_t = 0;
	// the nearer, the sooner and the louder the thunder
	bool near = s->struck_win >= 0 || s->struck_bar;
	double distance = near ? saver_between(0.05, 0.3) : r < 0.8 ? saver_between(0.4, 1) :
		saver_between(0.7, 1);
	s->thunder_at = 0.15 + distance * 2.6;
	s->thunder_power = 1.25 - distance;
	if (near) {
		for (int k = 0; k < 40; k++) {
			struct spark *p = &s->sparks[s->spark_next];
			s->spark_next = (s->spark_next + 1) % SPARKS_MAX;
			double a = saver_between(-M_PI, 0), v = saver_between(80, 420) * s->u;
			*p = (struct spark){ tx, ty, (float)(cos(a) * v), (float)(sin(a) * v),
				(float)saver_between(0.3, 1.1), 0 };
		}
		if (s->struck_win >= 0) {
			for (int i = 0; i < SCORCH_MAX; i++) {
				if (!s->scorch[i].alive || i == SCORCH_MAX - 1) {
					s->scorch[i] = (struct scorch){ tx, ty, 0, true };
					break;
				}
			}
		}
	}
	s->next_bolt = saver_between(3.5, 12) * s->bolt_pace *
		(saver_random() < 0.2 ? 0.25 : 1); // now and then a second soon after
}

/* How bright the flash is, over its first second: a few pulses, then gone. */
static double flash_level(double t) {
	if (t < 0) {
		return 0;
	}
	static const double pulses[][3] = { { 0, 0.05, 1 }, { 0.09, 0.05, 0.6 }, { 0.2, 0.08, 0.85 },
		{ 0.34, 0.25, 0.35 } };
	double f = 0;
	for (int i = 0; i < 4; i++) {
		double k = (t - pulses[i][0]) / pulses[i][1];
		if (k >= 0) {
			f = fmax(f, pulses[i][2] * exp(-k * 1.6));
		}
	}
	return f;
}

static void draw_bolt(struct storm *s, cairo_t *cr, double f) {
	if (f <= 0.02 || s->bolt_len < 2) {
		return;
	}
	double u = s->u;
	static const double glow[][3] = { { 22, 0.05, 0 }, { 10, 0.12, 0 }, { 4.5, 0.35, 0 },
		{ 1.6, 1, 1 } };
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	for (int g = 0; g < 4; g++) {
		for (int level = 0; level < 4; level++) {
			cairo_new_path(cr);
			bool any = false;
			for (int i = 0; i < s->bolt_len; i++) {
				struct bolt_point *p = &s->bolt[i];
				if (p->level != level) {
					continue;
				}
				if (p->start || i == 0 || s->bolt[i - 1].level != level) {
					cairo_move_to(cr, p->x, p->y);
				} else {
					cairo_line_to(cr, p->x, p->y);
				}
				any = true;
			}
			if (!any) {
				continue;
			}
			double thin = 1.0 / (1 + level * 0.9);
			cairo_set_line_width(cr, fmax(0.8, glow[g][0] * u * thin));
			double w = glow[g][2];
			cairo_set_source_rgba(cr, 0.65 + 0.35 * w, 0.72 + 0.28 * w, 1, glow[g][1] * f * thin);
			cairo_stroke(cr);
		}
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

/* ---------- the saver ---------- */

static void *storm_create(int width, int height, const struct saver_options *options) {
	struct storm *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->u = saver_unit(width, height);
	cairo_surface_t *desktop = saver_desktop(options, width, height, s->wins, WINDOWS_MAX,
		&s->count, s->bars, &s->bar_count);
	s->bright = desktop;
	s->dark = storm_light(desktop, width, height);
	s->cloud_h = (int)(height * 0.42);
	s->clouds[0] = make_clouds(width, s->cloud_h, (float)s->u * 1.6f, 11, 0.16f);
	s->clouds[1] = make_clouds(width, s->cloud_h, (float)s->u, 29, 0.24f);
	for (int k = 0; k < BEAD_SIZES; k++) {
		s->bead_r[k] = (float)(s->u * (1.2 + k * 0.7));
		s->bead_img[k][0] = make_bead(s->bead_r[k], s->u, false);
		s->bead_img[k][1] = make_bead(s->bead_r[k], s->u, true);
	}
	s->roof = calloc(width, sizeof(float));
	make_roofs(s);
	double u = s->u;
	for (int i = 0; i < FAR_DROPS; i++) {
		drop_reset(s, &s->far[i], (float)(u * 900), (float)(u * 22), true);
	}
	for (int i = 0; i < MID_DROPS; i++) {
		drop_reset(s, &s->mid[i], (float)(u * 1300), (float)(u * 34), true);
	}
	for (int i = 0; i < NEAR_DROPS; i++) {
		drop_reset(s, &s->near[i], (float)(u * 2300), (float)(u * 90), true);
	}
	static const double amounts[] = { 1, 0.55, 1.5 }, paces[] = { 1, 0.45, 2.5 };
	s->rain_amount = amounts[saver_choice(options, &saver_storm, "rain")];
	s->bolt_pace = paces[saver_choice(options, &saver_storm, "lightning")];
	s->hits = saver_toggle(options, &saver_storm, "strikes");
	s->bolt_t = -1;
	s->next_bolt = saver_between(1.5, 4);
	s->thunder_at = -1;
	s->struck_win = -1;
	return s;
}

static void storm_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct storm *s = state;
	double u = s->u, W = width, H = height;
	s->t += dt;
	// the wind: a slow swing, and gusts that drive the rain harder and more aslant
	s->gust += (saver_random() < dt * 0.15 ? 1 : 0) - s->gust * dt * 0.35;
	s->gust = fmin(s->gust, 1.5);
	s->wind = 0.12 + 0.08 * sin(s->t * 0.13) + 0.22 * s->gust;
	double rain = (1 + 0.6 * s->gust) * s->rain_amount;
	// lightning
	s->next_bolt -= dt;
	if (s->next_bolt <= 0) {
		strike(s);
	}
	if (s->bolt_t >= 0) {
		s->bolt_t += dt;
		if (s->bolt_t > 1.4) {
			s->bolt_t = -1;
		}
	}
	double f = flash_level(s->bolt_t);
	if (s->thunder_at >= 0) {
		s->thunder_at -= dt;
		if (s->thunder_at < 0) {
			s->shake = s->thunder_power; // the thunder: a rumble going through everything
			s->gust = fmin(1.5, s->gust + 0.4 * s->thunder_power);
			s->thunder_at = -1;
		}
	}
	s->shake = fmax(0, s->shake - dt * 0.55);
	s->struck_t += dt;

	cairo_save(cr);
	if (s->shake > 0) {
		double k = s->shake * s->shake * u * 9;
		cairo_translate(cr, sin(s->t * 57) * k + sin(s->t * 23) * k * 0.6, cos(s->t * 41) * k * 0.7);
	}
	// the desktop in storm light; a flash shows it as it is
	cairo_set_source_surface(cr, s->dark, 0, 0);
	cairo_paint(cr);
	if (f > 0.02) {
		// the flash lights it up: its colours show, but in a cold white light
		cairo_set_source_surface(cr, s->bright, 0, 0);
		cairo_paint_with_alpha(cr, fmin(0.55, f * 0.6));
	}
	// a struck window loses its light for a moment, flickering back
	if (s->struck_win >= 0 && s->struck_t < 2.2) {
		struct saver_window *win = &s->wins[s->struck_win];
		double out = s->struck_t < 0.1 ? 0 : s->struck_t > 1.6 ? (2.2 - s->struck_t) / 0.6 : 1;
		out *= 0.75 + 0.25 * (fmod(s->struck_t * 13, 1) < 0.5);
		cairo_rectangle(cr, win->x, win->y + win->title_h, win->w, win->h - win->title_h);
		cairo_set_source_rgba(cr, 0.01, 0.01, 0.03, 0.8 * out);
		cairo_fill(cr);
	}
	if (s->struck_bar && s->struck_t < 1.5 && s->bar_count) {
		struct saver_window *bar = &s->bars[0];
		double out = s->struck_t > 1 ? (1.5 - s->struck_t) / 0.5 : 1;
		out *= 0.7 + 0.3 * (fmod(s->struck_t * 11, 1) < 0.5);
		cairo_rectangle(cr, bar->x, bar->y, bar->w, bar->h);
		cairo_set_source_rgba(cr, 0, 0, 0.02, 0.75 * out);
		cairo_fill(cr);
	}
	// scorch marks where it struck, smoking
	for (int i = 0; i < SCORCH_MAX; i++) {
		struct scorch *m = &s->scorch[i];
		if (!m->alive) {
			continue;
		}
		m->age += (float)dt;
		double fade = fmax(0, 1 - m->age / 40);
		if (fade <= 0) {
			m->alive = false;
			continue;
		}
		double r = u * 26;
		cairo_pattern_t *p = cairo_pattern_create_radial(m->x, m->y, 0, m->x, m->y, r);
		cairo_pattern_add_color_stop_rgba(p, 0, 0.02, 0.01, 0.01, 0.85 * fade);
		cairo_pattern_add_color_stop_rgba(p, 0.5, 0.08, 0.05, 0.03, 0.5 * fade);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.1, 0.07, 0.05, 0);
		cairo_set_source(cr, p);
		cairo_arc(cr, m->x, m->y, r, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		if (m->age < 1.5) {
			cairo_arc(cr, m->x, m->y, u * 5, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 1, 0.6, 0.2, 1 - m->age / 1.5); // still glowing
			cairo_fill(cr);
		}
		for (int k = 0; k < 4; k++) { // smoke rising off it
			double age = fmod(m->age * 0.6 + k * 0.25, 1);
			double sx = m->x + sin(m->age * 1.3 + k) * u * 8 + s->wind * age * u * 120;
			double sy = m->y - age * u * 90;
			cairo_arc(cr, sx, sy, u * (6 + age * 18), 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 0.3, 0.3, 0.33, 0.25 * (1 - age) * fade);
			cairo_fill(cr);
		}
	}
	// the far rain, behind the windows
	rain_layer(s, cr, s->far, (int)(FAR_DROPS * fmin(1, 0.7 * s->rain_amount)), dt, false, true, (float)(u * 900), (float)(u * 22),
		(float)fmax(0.7, u * 0.9), 0.16 + 0.25 * f);
	// the clouds, rolling on, lit from within by the flash
	for (int l = 0; l < 2; l++) {
		double speed = (l ? 14 : 7) * u * (1 + s->gust);
		int off = (int)fmod(s->t * speed, W);
		cairo_set_source_surface(cr, s->clouds[l], off, 0);
		cairo_paint(cr);
		cairo_set_source_surface(cr, s->clouds[l], off - W, 0);
		cairo_paint(cr);
	}
	if (f > 0.02) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		double r = W * 0.45;
		cairo_pattern_t *p = cairo_pattern_create_radial(s->bolt_x, s->bolt_y, 0, s->bolt_x,
			s->bolt_y, r);
		cairo_pattern_add_color_stop_rgba(p, 0, 0.75, 0.8, 1, 0.55 * f);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.6, 0.65, 0.9, 0);
		cairo_set_source(cr, p);
		cairo_arc(cr, s->bolt_x, s->bolt_y, r, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	// water on the glass
	beads_step(s, dt, rain);
	draw_beads(s, cr, f);
	// the rain that lands on the windows and the taskbar
	rain_layer(s, cr, s->mid, (int)(MID_DROPS * fmin(1, 0.45 * rain)), dt, true, false,
		(float)(u * 1300), (float)(u * 34), (float)fmax(0.9, u * 1.3), 0.26 + 0.3 * f);
	// runoff off the corners of the windows
	for (int i = 0; i < s->count; i++) {
		struct saver_window *win = &s->wins[i];
		if (saver_random() < dt * 3 * rain) {
			drip_from(s, (float)(win->x + (saver_random() < 0.5 ? 1 : win->w - 1)),
				(float)(win->y + saver_between(0, win->title_h)));
		}
	}
	cairo_new_path(cr);
	for (int i = 0; i < DRIPS_MAX; i++) {
		struct drip *d = &s->drips[i];
		if (!d->alive) {
			continue;
		}
		float py = d->y;
		d->vy += (float)(u * 1200 * dt);
		d->y += d->vy * (float)dt;
		float floor = surface_below(s, d->x, py + 1);
		if (d->y >= floor) {
			splash(s, d->x, floor, 2, 0.6f);
			d->alive = false;
			continue;
		}
		cairo_move_to(cr, d->x, py);
		cairo_line_to(cr, d->x, d->y + u * 3);
	}
	cairo_set_line_width(cr, fmax(1, u * 1.6));
	cairo_set_source_rgba(cr, 0.75, 0.82, 0.95, 0.5 + 0.4 * f);
	cairo_stroke(cr);
	// the splashes
	cairo_new_path(cr);
	for (int i = 0; i < SPLASHES_MAX; i++) {
		struct splash *p = &s->splashes[i];
		if (p->age >= p->life || p->ring) {
			continue;
		}
		p->age += (float)dt;
		p->vy += (float)(u * 900 * dt);
		p->x += p->vx * (float)dt;
		p->y += p->vy * (float)dt;
		cairo_move_to(cr, p->x, p->y);
		cairo_line_to(cr, p->x - p->vx * 0.012, p->y - p->vy * 0.012);
	}
	cairo_set_line_width(cr, fmax(0.8, u * 1.2));
	cairo_set_source_rgba(cr, 0.8, 0.85, 0.95, 0.45 + 0.3 * f);
	cairo_stroke(cr);
	// the rings they leave, flat, in two strengths so they go in two strokes
	for (int pass = 0; pass < 2; pass++) {
		cairo_new_path(cr);
		for (int i = 0; i < SPLASHES_MAX; i++) {
			struct splash *p = &s->splashes[i];
			if (p->age >= p->life || !p->ring) {
				continue;
			}
			double k = p->age / p->life;
			if ((k < 0.5) != (pass == 0)) {
				continue;
			}
			if (pass == 1) {
				p->age += (float)dt;
			}
			double r = u * (2 + 9 * k);
			for (int a = 0; a <= 10; a++) {
				double ang = a * M_PI / 5;
				double x = p->x + cos(ang) * r, y = p->y + sin(ang) * r * 0.25;
				if (a == 0) {
					cairo_move_to(cr, x, y);
				} else {
					cairo_line_to(cr, x, y);
				}
			}
		}
		cairo_set_line_width(cr, fmax(0.6, u));
		cairo_set_source_rgba(cr, 0.8, 0.86, 0.96, pass == 0 ? 0.35 : 0.15);
		cairo_stroke(cr);
	}
	for (int i = 0; i < SPLASHES_MAX; i++) {
		struct splash *p = &s->splashes[i];
		if (p->ring && p->age < p->life && p->age / p->life < 0.5) {
			p->age += (float)dt;
		}
	}
	// the wet taskbar: a sheen along its edge, spray above it
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *bar = &s->bars[i];
		if (bar->y < H * 0.5) {
			continue;
		}
		cairo_pattern_t *mist = cairo_pattern_create_linear(0, bar->y - u * 60, 0, bar->y);
		cairo_pattern_add_color_stop_rgba(mist, 0, 0.7, 0.75, 0.85, 0);
		cairo_pattern_add_color_stop_rgba(mist, 1, 0.7, 0.75, 0.85, 0.1 + 0.06 * s->gust);
		cairo_set_source(cr, mist);
		cairo_rectangle(cr, bar->x, bar->y - u * 60, bar->w, u * 60);
		cairo_fill(cr);
		cairo_pattern_destroy(mist);
		cairo_pattern_t *sheen = cairo_pattern_create_linear(0, bar->y, 0, bar->y + bar->h);
		cairo_pattern_add_color_stop_rgba(sheen, 0, 0.8, 0.85, 1, 0.22 + 0.5 * f);
		cairo_pattern_add_color_stop_rgba(sheen, 0.15, 0.8, 0.85, 1, 0.05 + 0.2 * f);
		cairo_pattern_add_color_stop_rgba(sheen, 1, 0.8, 0.85, 1, 0.02 + 0.08 * f);
		cairo_set_source(cr, sheen);
		cairo_rectangle(cr, bar->x, bar->y, bar->w, bar->h);
		cairo_fill(cr);
		cairo_pattern_destroy(sheen);
	}
	// gusts drive the rain in sheets
	if (s->gust > 0.1) {
		double x = fmod(s->t * u * 700, W * 1.6) - W * 0.3, half = W * 0.18;
		cairo_pattern_t *sheet = cairo_pattern_create_linear(x - half, 0, x + half, 0);
		cairo_pattern_add_color_stop_rgba(sheet, 0, 0.7, 0.75, 0.85, 0);
		cairo_pattern_add_color_stop_rgba(sheet, 0.5, 0.7, 0.75, 0.85, 0.07 * fmin(1, s->gust));
		cairo_pattern_add_color_stop_rgba(sheet, 1, 0.7, 0.75, 0.85, 0);
		cairo_set_source(cr, sheet);
		cairo_rectangle(cr, x - half, 0, 2 * half, H);
		cairo_fill(cr);
		cairo_pattern_destroy(sheet);
	}
	// the lightning, and its light on everything
	draw_bolt(s, cr, f);
	if (f > 0.02) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		cairo_set_source_rgba(cr, 0.5, 0.56, 0.8, 0.3 * f);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	// sparks where it struck
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int i = 0; i < SPARKS_MAX; i++) {
		struct spark *p = &s->sparks[i];
		if (p->age >= p->life) {
			continue;
		}
		p->age += (float)dt;
		p->vy += (float)(u * 700 * dt);
		float px = p->x, py = p->y;
		p->x += p->vx * (float)dt;
		p->y += p->vy * (float)dt;
		double k = 1 - p->age / p->life;
		cairo_move_to(cr, px, py);
		cairo_line_to(cr, p->x, p->y);
		cairo_set_line_width(cr, fmax(1, u * 1.8));
		cairo_set_source_rgba(cr, 1, 0.75 + 0.2 * k, 0.4 + 0.5 * k, k);
		cairo_stroke(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	// the near rain, heavy streaks in front of it all
	rain_layer(s, cr, s->near, (int)(NEAR_DROPS * fmin(1, 0.4 * rain)), dt, false, false,
		(float)(u * 2300), (float)(u * 90), (float)fmax(1.2, u * 2.4), 0.12 + 0.2 * f);
	cairo_restore(cr);
	// now and then a big drop runs down the screen itself
	if (saver_random() < dt * 0.3 * rain) {
		for (int i = 0; i < LENS_MAX; i++) {
			if (!s->lens[i].alive) {
				s->lens[i] = (struct lens){ (float)(saver_random() * W), (float)(saver_random() * H * 0.7),
					(float)(saver_between(10, 26) * u), 0, (float)saver_between(4, 9), true };
				break;
			}
		}
	}
	for (int i = 0; i < LENS_MAX; i++) {
		struct lens *l = &s->lens[i];
		if (!l->alive) {
			continue;
		}
		l->life -= (float)dt;
		l->vy = fminf(l->vy + (float)(u * 25 * dt), (float)(u * 60));
		l->y += l->vy * (float)dt;
		if (l->life <= 0 || l->y - l->r > H) {
			l->alive = false;
			continue;
		}
		double a = fmin(1, l->life);
		cairo_pattern_t *p = cairo_pattern_create_radial(l->x - l->r * 0.3, l->y - l->r * 0.3,
			l->r * 0.1, l->x, l->y, l->r);
		cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.18 * a);
		cairo_pattern_add_color_stop_rgba(p, 0.7, 0.6, 0.66, 0.8, 0.06 * a);
		cairo_pattern_add_color_stop_rgba(p, 0.92, 0.1, 0.12, 0.18, 0.3 * a);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.8, 0.85, 1, 0.2 * a);
		cairo_set_source(cr, p);
		cairo_arc(cr, l->x, l->y, l->r, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
}

static void storm_destroy(void *state) {
	struct storm *s = state;
	cairo_surface_destroy(s->dark);
	cairo_surface_destroy(s->bright);
	cairo_surface_destroy(s->clouds[0]);
	cairo_surface_destroy(s->clouds[1]);
	for (int k = 0; k < BEAD_SIZES; k++) {
		cairo_surface_destroy(s->bead_img[k][0]);
		cairo_surface_destroy(s->bead_img[k][1]);
	}
	free(s->roof);
	free(s);
}

static const char *const rain_values[] = { "heavy", "light", "downpour", NULL };
static const char *const rain_labels[] = { "Heavy", "Light", "Downpour", NULL };
static const char *const bolt_values[] = { "sometimes", "often", "rarely", NULL };
static const char *const bolt_labels[] = { "Now and then", "Often", "Rarely", NULL };
static const struct saver_option storm_options[] = {
	{ "rain", "Rain", NULL, SAVER_CHOICE, rain_values, rain_labels, false },
	{ "lightning", "Lightning", NULL, SAVER_CHOICE, bolt_values, bolt_labels, false },
	{ "strikes", "Strikes into windows", "Lightning hits the windows and the taskbar too",
		SAVER_TOGGLE, NULL, NULL, true },
	{ 0 },
};

const struct saver saver_storm = {
	.name = "thunderstorm",
	.title = "Thunderstorm",
	.description = "Rain on the windows and the taskbar, lightning striking into them, thunder",
	.wants_desktop = true,
	.options = storm_options,
	.create = storm_create,
	.draw = storm_draw,
	.destroy = storm_destroy,
};
