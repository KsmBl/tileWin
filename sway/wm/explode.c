/*
 * The explosion that closes a window ("animation close explode"). The window
 * charges up with a glow, then bursts: it shatters into shards (small ones
 * near the blast, big ones further away) that fly outward, tumble and fall,
 * while a flash, a shock wave with a dust ring, billowing fireballs that burn
 * out into smoke, sparks with motion streaks and floating embers go off, the
 * fire lights up everything around it and the screen shakes for a moment.
 *
 * The fire, smoke, glow, ring and spark images are made from noise once, in a
 * thread, when the style is picked. Light is drawn additively: pixels whose
 * color is above their alpha brighten what is behind them instead of covering
 * it, which is what makes fire and sparks glow.
 */
#include <cairo.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include "sway/input/cursor.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/scene_descriptor.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "list.h"
#include "log.h"

#define CHARGE_MS 90   // glow building up before the window bursts
#define TAU 6.283185307179586
#define MAX_SHARDS 220

/* ---------- images ---------- */

#define PUFF_SIZE 200
#define PUFF_FRAMES 24
#define GLOW_SIZE 128
#define LIGHT_SIZE 96
#define RING_SIZE 256
#define SPARK_SIZE 24

enum sprite {
	SPRITE_GLOW,
	SPRITE_LIGHT,
	SPRITE_RING,
	SPRITE_SPARK,
	SPRITE_PUFF, // the first of PUFF_FRAMES frames of a fireball burning out
	SPRITE_COUNT = SPRITE_PUFF + PUFF_FRAMES,
};

enum {
	IMAGES_NONE,
	IMAGES_MAKING,
	IMAGES_MADE,
	IMAGES_UPLOADED,
	IMAGES_FAILED,
};

struct image {
	int size;
	uint32_t *pixels; // premultiplied ARGB, color may be above alpha (light)
};

static struct image images[SPRITE_COUNT];
static struct wlr_buffer *buffers[SPRITE_COUNT];
static atomic_int images_state = IMAGES_NONE;

static double clamp01(double v) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static double smoothstep(double edge0, double edge1, double x) {
	double t = clamp01((x - edge0) / (edge1 - edge0));
	return t * t * (3 - 2 * t);
}

static double ease_out(double t, double power) {
	return 1 - pow(1 - clamp01(t), power);
}

/* Improved Perlin noise with a fixed shuffled permutation. */
static unsigned char perm[512];

static void noise_init(void) {
	for (int i = 0; i < 256; i++) {
		perm[i] = i;
	}
	uint32_t s = 0x9e3779b9;
	for (int i = 255; i > 0; i--) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		int j = s % (i + 1);
		unsigned char tmp = perm[i];
		perm[i] = perm[j];
		perm[j] = tmp;
	}
	for (int i = 0; i < 256; i++) {
		perm[256 + i] = perm[i];
	}
}

static double fade(double t) {
	return t * t * t * (t * (t * 6 - 15) + 10);
}

static double grad(int hash, double x, double y, double z) {
	int h = hash & 15;
	double u = h < 8 ? x : y;
	double v = h < 4 ? y : h == 12 || h == 14 ? x : z;
	return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

static double noise3(double x, double y, double z) {
	double fx = floor(x), fy = floor(y), fz = floor(z);
	int X = (int)fx & 255, Y = (int)fy & 255, Z = (int)fz & 255;
	x -= fx;
	y -= fy;
	z -= fz;
	double u = fade(x), v = fade(y), w = fade(z);
	int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z;
	int B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
	return lerp(
		lerp(lerp(grad(perm[AA], x, y, z), grad(perm[BA], x - 1, y, z), u),
			lerp(grad(perm[AB], x, y - 1, z), grad(perm[BB], x - 1, y - 1, z), u), v),
		lerp(lerp(grad(perm[AA + 1], x, y, z - 1), grad(perm[BA + 1], x - 1, y, z - 1), u),
			lerp(grad(perm[AB + 1], x, y - 1, z - 1),
				grad(perm[BB + 1], x - 1, y - 1, z - 1), u), v), w);
}

static double fbm(double x, double y, double z, int octaves) {
	double sum = 0, amplitude = 0.5;
	for (int i = 0; i < octaves; i++) {
		sum += amplitude * noise3(x, y, z);
		x = x * 2.03 + 17.1;
		y = y * 2.03 - 9.7;
		z = z * 2.03 + 3.3;
		amplitude *= 0.5;
	}
	return sum;
}

static uint32_t pixel(double r, double g, double b, double a) {
	return (uint32_t)lround(clamp01(a) * 255) << 24 | (uint32_t)lround(clamp01(r) * 255) << 16 |
		(uint32_t)lround(clamp01(g) * 255) << 8 | (uint32_t)lround(clamp01(b) * 255);
}

/* Color of fire at a temperature from 0 (dull red) to about 1.15 (white hot). */
static void fire_color(double heat, double *r, double *g, double *b) {
	static const double stops[][4] = {
		{ 0.00, 0.25, 0.03, 0.01 },
		{ 0.22, 0.62, 0.09, 0.01 },
		{ 0.45, 1.00, 0.36, 0.04 },
		{ 0.70, 1.00, 0.68, 0.16 },
		{ 0.92, 1.00, 0.93, 0.62 },
		{ 1.15, 1.00, 1.00, 0.92 },
	};
	const int count = sizeof(stops) / sizeof(stops[0]);
	if (heat <= stops[0][0]) {
		*r = stops[0][1];
		*g = stops[0][2];
		*b = stops[0][3];
		return;
	}
	for (int i = 1; i < count; i++) {
		if (heat <= stops[i][0] || i == count - 1) {
			double t = clamp01((heat - stops[i - 1][0]) / (stops[i][0] - stops[i - 1][0]));
			*r = lerp(stops[i - 1][1], stops[i][1], t);
			*g = lerp(stops[i - 1][2], stops[i][2], t);
			*b = lerp(stops[i - 1][3], stops[i][3], t);
			return;
		}
	}
}

static uint32_t *image_new(enum sprite sprite, int size) {
	images[sprite].size = size;
	images[sprite].pixels = calloc((size_t)size * size, sizeof(uint32_t));
	return images[sprite].pixels;
}

/* A billowing ball of fire at frame / PUFF_FRAMES of burning out into smoke. */
static void make_puff(int frame) {
	uint32_t *px = image_new(SPRITE_PUFF + frame, PUFF_SIZE);
	if (!px) {
		return;
	}
	double u = (double)frame / (PUFF_FRAMES - 1);
	double radius = 0.5 + 0.26 * ease_out(u, 2);
	for (int j = 0; j < PUFF_SIZE; j++) {
		for (int i = 0; i < PUFF_SIZE; i++) {
			double x = (i + 0.5) / PUFF_SIZE * 2 - 1, y = (j + 0.5) / PUFF_SIZE * 2 - 1;
			double r = sqrt(x * x + y * y);
			if (r > 1) {
				continue;
			}
			// domain-warped noise rolls and rises as the frames go on
			double w = fbm(x * 1.7 + 11.3, y * 1.7 - u * 0.8, 1.7 + u * 0.7, 2);
			double n = fbm(x * 2.6 + w * 1.1, y * 2.6 + w * 1.1 + u * 1.9, 5.3 + u * 1.2, 4);
			double edge = radius + n * 0.34;
			double density = smoothstep(edge, edge - 0.3, r) * smoothstep(1, 0.9, r);
			// thin parts burn away first, thick wisps stay a little longer
			density *= smoothstep(1.0, 0.62, u + 0.42 * (0.35 - n) * u);
			if (density <= 0.002) {
				continue;
			}
			double core = clamp01(1 - r / (radius + 0.1));
			double heat = density * pow(clamp01(1 - u * 1.18), 1.35) *
				(0.45 + 0.65 * core + 0.55 * n);
			heat = heat < 0 ? 0 : heat > 1.15 ? 1.15 : heat;
			// smoke thickens as the fire cools; the fire inside lights it warm
			double smoke = density * (0.18 + 0.62 * smoothstep(0.05, 0.55, u));
			double shade = 0.06 + 0.09 * clamp01(0.5 + n) + 0.05 * (1 - u);
			double sr = shade * 1.08 + heat * 0.25, sg = shade * 0.98 + heat * 0.08,
				sb = shade * 0.92;
			double fr, fg, fb;
			fire_color(heat, &fr, &fg, &fb);
			double emission = smoothstep(0.02, 0.7, heat) * 1.05;
			double cover = clamp01(emission);
			double a = smoke * (1 - cover) + cover * 0.6 * density;
			px[j * PUFF_SIZE + i] = pixel(sr * smoke * (1 - cover) + fr * emission,
				sg * smoke * (1 - cover) + fg * emission,
				sb * smoke * (1 - cover) + fb * emission, a);
		}
	}
}

static void make_glow(void) {
	uint32_t *px = image_new(SPRITE_GLOW, GLOW_SIZE);
	for (int j = 0; px && j < GLOW_SIZE; j++) {
		for (int i = 0; i < GLOW_SIZE; i++) {
			double x = (i + 0.5) / GLOW_SIZE * 2 - 1, y = (j + 0.5) / GLOW_SIZE * 2 - 1;
			double r2 = x * x + y * y, r = sqrt(r2);
			double taper = clamp01(1 - r2);
			double v = (exp(-r2 * 7) * 0.9 + 0.35 * exp(-r2 * 2.2)) * taper * taper;
			px[j * GLOW_SIZE + i] = pixel(v, v * (0.82 - 0.3 * r), v * clamp01(0.5 - 0.42 * r),
				v * 0.22);
		}
	}
}

/* Pure light, no coverage: warm light the fire casts on the windows around. */
static void make_light(void) {
	uint32_t *px = image_new(SPRITE_LIGHT, LIGHT_SIZE);
	for (int j = 0; px && j < LIGHT_SIZE; j++) {
		for (int i = 0; i < LIGHT_SIZE; i++) {
			double x = (i + 0.5) / LIGHT_SIZE * 2 - 1, y = (j + 0.5) / LIGHT_SIZE * 2 - 1;
			double r2 = x * x + y * y;
			double taper = clamp01(1 - r2);
			double v = taper * taper * (0.5 * exp(-r2 * 3) + 0.5) * 0.55;
			px[j * LIGHT_SIZE + i] = pixel(0.9 * v, 0.42 * v, 0.12 * v, 0);
		}
	}
}

/* A shock wave: a thin bright ring with a ragged dust ring around it. */
static void make_ring(void) {
	uint32_t *px = image_new(SPRITE_RING, RING_SIZE);
	for (int j = 0; px && j < RING_SIZE; j++) {
		for (int i = 0; i < RING_SIZE; i++) {
			double x = (i + 0.5) / RING_SIZE * 2 - 1, y = (j + 0.5) / RING_SIZE * 2 - 1;
			double r = sqrt(x * x + y * y);
			if (r > 1) {
				continue;
			}
			double angle = atan2(y, x);
			double n = fbm(cos(angle) * 2.5 + 4, sin(angle) * 2.5 + 4, r * 3, 3);
			double width = 0.035 + 0.02 * n;
			double bright = exp(-pow((r - 0.78) / width, 2)) * (0.7 + 0.5 * n);
			double haze = exp(-pow((r - 0.7) / 0.16, 2)) * 0.18;
			double dust = exp(-pow((r - 0.84) / 0.09, 2)) * clamp01(0.35 + 0.8 * n) * 0.45;
			double taper = smoothstep(1.0, 0.95, r);
			double light = (bright + haze) * taper;
			dust *= taper * 0.7;
			px[j * RING_SIZE + i] = pixel(dust * 0.30 + light, dust * 0.24 + light * 0.9,
				dust * 0.18 + light * 0.75, dust + light * 0.1);
		}
	}
}

static void make_spark(void) {
	uint32_t *px = image_new(SPRITE_SPARK, SPARK_SIZE);
	for (int j = 0; px && j < SPARK_SIZE; j++) {
		for (int i = 0; i < SPARK_SIZE; i++) {
			double x = (i + 0.5) / SPARK_SIZE * 2 - 1, y = (j + 0.5) / SPARK_SIZE * 2 - 1;
			double r2 = x * x + y * y;
			double hot = exp(-r2 * 28), halo = exp(-r2 * 5) * clamp01(1 - sqrt(r2));
			px[j * SPARK_SIZE + i] = pixel(hot + halo * 0.95, hot * 0.97 + halo * 0.5,
				hot * 0.85 + halo * 0.12, hot * 0.7 + halo * 0.15);
		}
	}
}

static void *make_images(void *data) {
	noise_init();
	make_glow();
	make_light();
	make_ring();
	make_spark();
	for (int i = 0; i < PUFF_FRAMES; i++) {
		make_puff(i);
	}
	atomic_store(&images_state, IMAGES_MADE);
	return NULL;
}

void tw_explosion_prepare(void) {
	int expected = IMAGES_NONE;
	if (!atomic_compare_exchange_strong(&images_state, &expected, IMAGES_MAKING)) {
		return;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	if (pthread_create(&thread, &attr, make_images, NULL) != 0) {
		sway_log(SWAY_ERROR, "Cannot start making the explosion images");
		atomic_store(&images_state, IMAGES_FAILED);
	}
	pthread_attr_destroy(&attr);
}

static void upload_images(void) {
	for (int i = 0; i < SPRITE_COUNT; i++) {
		struct image *image = &images[i];
		if (!image->pixels) {
			continue;
		}
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			image->size, image->size);
		if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
			cairo_surface_flush(surface);
			unsigned char *data = cairo_image_surface_get_data(surface);
			int stride = cairo_image_surface_get_stride(surface);
			for (int y = 0; y < image->size; y++) {
				memcpy(data + y * stride, image->pixels + y * image->size,
					(size_t)image->size * 4);
			}
			cairo_surface_mark_dirty(surface);
			buffers[i] = tw_buffer_from_surface(surface);
		} else {
			cairo_surface_destroy(surface);
		}
		free(image->pixels);
		image->pixels = NULL;
	}
}

static bool images_ready(void) {
	tw_explosion_prepare();
	if (atomic_load(&images_state) == IMAGES_MADE) {
		upload_images();
		atomic_store(&images_state, IMAGES_UPLOADED);
	}
	return atomic_load(&images_state) == IMAGES_UPLOADED;
}

void tw_explosion_release(void) {
	if (atomic_load(&images_state) != IMAGES_UPLOADED) {
		return;
	}
	for (int i = 0; i < SPRITE_COUNT; i++) {
		if (buffers[i]) {
			wlr_buffer_drop(buffers[i]);
			buffers[i] = NULL;
		}
	}
	atomic_store(&images_state, IMAGES_NONE);
}

/* ---------- the explosion ---------- */

enum layer {
	LAYER_LIGHT,
	LAYER_SHARDS,
	LAYER_SMOKE,
	LAYER_FIRE,
	LAYER_RING,
	LAYER_SPARKS,
	LAYER_FLASH,
	LAYER_COUNT,
};

enum particle_type {
	P_SHARD,
	P_FIREBALL,
	P_SMOKE,
	P_SPARK,
	P_EMBER,
	P_CHARGE,
	P_FLASH,
	P_CORE,
	P_LIGHT,
	P_RING,
	P_DUST_RING,
};

struct shard_node {
	struct wlr_scene_node *node;
	bool rect;
	float color[4];
	double x, y, w, h; // relative to the shard's top left corner
};

struct particle {
	enum particle_type type;
	struct wlr_scene_tree *tree;    // shard
	list_t *nodes;                  // shard: struct shard_node *
	struct wlr_scene_rect *scorch;  // shard: glows hot, then chars
	double scorch_x, scorch_y, scorch_w, scorch_h; // its part inside the frame
	double heat;                    // shard: how close to the blast it was
	struct wlr_scene_buffer *a, *b; // sprites; b: the next fire frame, faded in
	int frame_a, frame_b;
	float alpha, alpha_a, alpha_b;  // read by output_configure_scene
	double delay, life;             // ms
	double x, y;                    // center at the start
	double vx, vy, vz;              // px/s; vz: towards the viewer
	double drag, gravity;
	double w, h, size;
	double spin_x, spin_y;
	double seed;
};

struct tw_explosion {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *layers[LAYER_COUNT];
	list_t *particles;
	double bx, by;        // where it goes off
	double width, height, diag, power;
	double length;        // ms until the last particle is gone
};

static uint64_t rng = 88172645463325252ull;

static double rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng >> 11) * (1.0 / 9007199254740992.0);
}

static double range(double a, double b) {
	return a + (b - a) * rnd();
}

static double hash01(double x, double y) {
	double v = sin(x * 12.9898 + y * 78.233) * 43758.5453;
	return v - floor(v);
}

static struct particle *particle_new(struct tw_explosion *e, enum particle_type type) {
	struct particle *p = calloc(1, sizeof(*p));
	if (!p) {
		return NULL;
	}
	p->type = type;
	p->frame_a = p->frame_b = -1;
	p->seed = range(0, 1000);
	list_add(e->particles, p);
	return p;
}

static struct wlr_scene_buffer *sprite_node(struct wlr_scene_tree *parent, enum sprite sprite,
		float *alpha) {
	if (!buffers[sprite]) {
		return NULL;
	}
	struct wlr_scene_buffer *node = wlr_scene_buffer_create(parent, buffers[sprite]);
	if (!node) {
		return NULL;
	}
	wlr_scene_buffer_set_filter_mode(node, WLR_SCALE_FILTER_BILINEAR);
	scene_descriptor_assign(&node->node, SWAY_SCENE_DESC_TW_ANIMATION, alpha);
	wlr_scene_node_set_enabled(&node->node, false);
	return node;
}

static struct particle *sprite_particle(struct tw_explosion *e, enum particle_type type,
		enum layer layer, enum sprite sprite, bool two) {
	struct particle *p = particle_new(e, type);
	if (!p) {
		return NULL;
	}
	p->a = sprite_node(e->layers[layer], sprite, &p->alpha_a);
	if (two) {
		p->b = sprite_node(e->layers[layer], sprite, &p->alpha_b);
	}
	return p;
}

static void show_sprite(struct wlr_scene_buffer *node, double cx, double cy, double w, double h) {
	if (!node) {
		return;
	}
	int iw = (int)fmax(1, round(w)), ih = (int)fmax(1, round(h));
	wlr_scene_buffer_set_dest_size(node, iw, ih);
	wlr_scene_node_set_position(&node->node, (int)round(cx - iw / 2.0),
		(int)round(cy - ih / 2.0));
	wlr_scene_node_set_enabled(&node->node, true);
}

static void hide_sprite(struct wlr_scene_buffer *node) {
	if (node && node->node.enabled) {
		wlr_scene_node_set_enabled(&node->node, false);
	}
}

static void set_frame(struct wlr_scene_buffer *node, int *current, int frame) {
	if (node && *current != frame && buffers[SPRITE_PUFF + frame]) {
		wlr_scene_buffer_set_buffer(node, buffers[SPRITE_PUFF + frame]);
		*current = frame;
	}
}

/* Where a particle is after t seconds: thrown with its velocity, slowed by air, pulled by gravity. */
static void ballistic(const struct particle *p, double t, double *x, double *y) {
	double move = p->drag > 0 ? (1 - exp(-p->drag * t)) / p->drag : t;
	*x = p->x + p->vx * move;
	*y = p->y + p->vy * move + 0.5 * p->gravity * t * t;
}

/* ---------- shattering the window ---------- */

struct source {
	struct wlr_scene_buffer *buffer; // or rect
	struct wlr_scene_rect *rect;
	double x, y, w, h;
};

struct sources {
	struct source *items;
	int count, capacity;
};

static void add_source(struct sources *s, struct source source) {
	if (s->count == s->capacity) {
		int capacity = s->capacity ? s->capacity * 2 : 16;
		struct source *items = realloc(s->items, capacity * sizeof(*items));
		if (!items) {
			return;
		}
		s->items = items;
		s->capacity = capacity;
	}
	s->items[s->count++] = source;
}

static void collect(struct sources *s, struct wlr_scene_node *node, double x, double y, bool top) {
	if (!node->enabled && !top) {
		return;
	}
	if (!top) {
		x += node->x;
		y += node->y;
	}
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			collect(s, child, x, y, false);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (buffer->buffer) {
			int w = buffer->dst_width > 0 ? buffer->dst_width : buffer->buffer->width;
			int h = buffer->dst_height > 0 ? buffer->dst_height : buffer->buffer->height;
			add_source(s, (struct source){ buffer, NULL, x, y, w, h });
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (rect->width > 0 && rect->height > 0 && rect->color[3] > 0) {
			add_source(s, (struct source){ NULL, rect, x, y, rect->width, rect->height });
		}
		break;
	}
	}
}

struct cell {
	double x, y, w, h;
};

/* Cracks the window into cells, finer the closer they are to the blast. */
static int shatter(struct cell *cells, struct cell bounds, double bx, double by, double diag) {
	int count = 1;
	cells[0] = bounds;
	for (bool split = true; split && count < MAX_SHARDS;) {
		split = false;
		int before = count;
		for (int i = 0; i < before && count < MAX_SHARDS; i++) {
			struct cell c = cells[i];
			double d = hypot(c.x + c.w / 2 - bx, c.y + c.h / 2 - by);
			double target = (26 + 130 * clamp01(d / (0.8 * diag))) *
				(0.85 + 0.35 * hash01(c.x * 0.37, c.y * 1.31));
			if (fmax(c.w, c.h) <= target) {
				continue;
			}
			bool across = c.w > c.h * 1.25 ? true : c.h > c.w * 1.25 ? false :
				hash01(c.y, c.x) < 0.5;
			double t = 0.32 + 0.36 * hash01(c.w + c.x, c.h - c.y);
			if (across) {
				cells[i].w = c.w * t;
				cells[count++] = (struct cell){ c.x + c.w * t, c.y, c.w * (1 - t), c.h };
			} else {
				cells[i].h = c.h * t;
				cells[count++] = (struct cell){ c.x, c.y + c.h * t, c.w, c.h * (1 - t) };
			}
			split = true;
		}
	}
	return count;
}

static void add_shard(struct tw_explosion *e, struct cell c, const struct sources *sources,
		const struct wlr_box *solid) {
	struct wlr_scene_tree *tree = wlr_scene_tree_create(e->layers[LAYER_SHARDS]);
	if (!tree) {
		return;
	}
	list_t *nodes = create_list();
	for (int i = 0; i < sources->count; i++) {
		const struct source *s = &sources->items[i];
		double x0 = fmax(c.x, s->x), y0 = fmax(c.y, s->y);
		double x1 = fmin(c.x + c.w, s->x + s->w), y1 = fmin(c.y + c.h, s->y + s->h);
		// a rotated buffer is not cut: it goes with the shard holding its center
		bool whole = s->buffer && s->buffer->transform != WL_OUTPUT_TRANSFORM_NORMAL;
		if (whole) {
			double mx = s->x + s->w / 2, my = s->y + s->h / 2;
			if (mx < c.x || mx >= c.x + c.w || my < c.y || my >= c.y + c.h) {
				continue;
			}
			x0 = s->x;
			y0 = s->y;
			x1 = s->x + s->w;
			y1 = s->y + s->h;
		}
		if (x1 - x0 < 0.5 || y1 - y0 < 0.5) {
			continue;
		}
		struct shard_node *n = calloc(1, sizeof(*n));
		if (!n) {
			continue;
		}
		*n = (struct shard_node){ .x = x0 - c.x, .y = y0 - c.y, .w = x1 - x0, .h = y1 - y0 };
		if (s->buffer) {
			struct wlr_scene_buffer *copy = wlr_scene_buffer_create(tree, s->buffer->buffer);
			if (!copy) {
				free(n);
				continue;
			}
			wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
			wlr_scene_buffer_set_transfer_function(copy, s->buffer->transfer_function);
			wlr_scene_buffer_set_primaries(copy, s->buffer->primaries);
			wlr_scene_buffer_set_transform(copy, s->buffer->transform);
			struct wlr_fbox base = s->buffer->src_box;
			if (wlr_fbox_empty(&base)) {
				base = (struct wlr_fbox){ 0, 0, s->buffer->buffer->width,
					s->buffer->buffer->height };
			}
			if (!whole) {
				base = (struct wlr_fbox){
					base.x + (x0 - s->x) / s->w * base.width,
					base.y + (y0 - s->y) / s->h * base.height,
					(x1 - x0) / s->w * base.width,
					(y1 - y0) / s->h * base.height,
				};
			}
			wlr_scene_buffer_set_source_box(copy, &base);
			n->node = &copy->node;
		} else {
			struct wlr_scene_rect *copy = wlr_scene_rect_create(tree, (int)ceil(n->w),
				(int)ceil(n->h), s->rect->color);
			if (!copy) {
				free(n);
				continue;
			}
			n->rect = true;
			memcpy(n->color, s->rect->color, sizeof(n->color));
			n->node = &copy->node;
		}
		list_add(nodes, n);
	}
	struct particle *p = nodes->length ? particle_new(e, P_SHARD) : NULL;
	if (!p) {
		list_free_items_and_destroy(nodes);
		wlr_scene_node_destroy(&tree->node);
		return;
	}
	p->tree = tree;
	p->nodes = nodes;
	scene_descriptor_assign(&tree->node, SWAY_SCENE_DESC_TW_ANIMATION, &p->alpha);
	p->alpha = 1;
	p->w = c.w;
	p->h = c.h;
	p->x = c.x + c.w / 2;
	p->y = c.y + c.h / 2;

	double dx = p->x - e->bx, dy = p->y - e->by, dist = hypot(dx, dy);
	double nx, ny;
	if (dist > 1) {
		nx = dx / dist;
		ny = dy / dist;
	} else {
		double angle = range(0, TAU);
		nx = cos(angle);
		ny = sin(angle);
	}
	double near = exp(-dist / (0.55 * e->diag));
	double size_factor = fmin(1.9, fmax(0.55, 70 / sqrt(c.w * c.h)));
	double speed = range(330, 780) * (0.35 + 0.9 * near) * size_factor * e->power;
	double side = range(-0.35, 0.35);
	p->vx = (nx - ny * side) * speed;
	p->vy = (ny + nx * side) * speed - range(80, 300) * e->power;
	p->vz = range(-0.5, 1.0) * (0.3 + near);
	p->heat = near;
	// only where the window is opaque: its frame, not a shadow around it
	double sx0 = solid ? fmax(c.x, solid->x) : 0, sy0 = solid ? fmax(c.y, solid->y) : 0;
	double sx1 = solid ? fmin(c.x + c.w, solid->x + solid->width) : 0;
	double sy1 = solid ? fmin(c.y + c.h, solid->y + solid->height) : 0;
	if (sx1 - sx0 >= 1 && sy1 - sy0 >= 1) {
		static const float clear[4] = { 0, 0, 0, 0 };
		p->scorch = wlr_scene_rect_create(tree, 1, 1, clear);
		if (p->scorch) {
			wlr_scene_node_set_enabled(&p->scorch->node, false);
			p->scorch_x = sx0 - c.x;
			p->scorch_y = sy0 - c.y;
			p->scorch_w = sx1 - sx0;
			p->scorch_h = sy1 - sy0;
		}
	}
	p->drag = range(1.1, 1.9);
	p->gravity = range(1300, 1700);
	double fast = range(4, 16) * (near + 0.3), slow = range(0, 4);
	bool flip = rnd() < 0.5;
	p->spin_x = (flip ? fast : slow) * (rnd() < 0.5 ? -1 : 1);
	p->spin_y = (flip ? slow : fast) * (rnd() < 0.5 ? -1 : 1);
	// the pressure wave reaches far pieces a moment later
	p->delay = CHARGE_MS + dist / e->diag * 25;
	p->life = range(1150, 1650);
}

static void update_shard(struct tw_explosion *e, struct particle *p, double ms) {
	double age = ms - p->delay;
	if (age >= p->life) {
		if (p->tree->node.enabled) {
			wlr_scene_node_set_enabled(&p->tree->node, false);
		}
		return;
	}
	if (age < 0) {
		// the whole window shakes as one while the glow builds up; the pieces
		// sit on exact pixels so no cracks show yet
		double charge = clamp01(ms / CHARGE_MS);
		wlr_scene_node_set_position(&p->tree->node, (int)round(charge * 1.6 * sin(ms * 0.9)),
			(int)round(charge * 1.2 * cos(ms * 1.1)));
		double left = p->x - p->w / 2, top = p->y - p->h / 2;
		for (int i = 0; i < p->nodes->length; i++) {
			struct shard_node *n = p->nodes->items[i];
			int x0 = (int)round(left + n->x), y0 = (int)round(top + n->y);
			int w = (int)fmax(1, round(left + n->x + n->w) - x0);
			int h = (int)fmax(1, round(top + n->y + n->h) - y0);
			wlr_scene_node_set_position(n->node, x0, y0);
			if (n->rect) {
				wlr_scene_rect_set_size(wlr_scene_rect_from_node(n->node), w, h);
			} else {
				wlr_scene_buffer_set_dest_size(wlr_scene_buffer_from_node(n->node), w, h);
			}
		}
		p->alpha = 1;
		return;
	}
	double x, y, t = age / 1000;
	ballistic(p, t, &x, &y);
	double depth = fmin(1.6, fmax(0.5, 1 + p->vz * (1 - exp(-2.2 * t)) * 0.45));
	// tumbling: a piece turning over looks thinner along that axis
	double scale_x = depth * fmax(0.05, fabs(cos(p->spin_x * t)));
	double scale_y = depth * fmax(0.05, fabs(cos(p->spin_y * t)));
	p->alpha = 1 - smoothstep(0.5, 1.0, age / p->life);
	wlr_scene_node_set_position(&p->tree->node, (int)round(x), (int)round(y));
	if (p->scorch) {
		// a red glow near the blast at first, then soot: darker the closer it was
		double hot = p->heat * exp(-t / 0.15);
		double charred = smoothstep(0.03, 0.6, t) * (0.35 + 0.45 * p->heat);
		double a = clamp01(hot * 0.35 + charred * (1 - hot));
		double mix = a > 0 ? hot * 0.35 / a : 0;
		a *= p->alpha;
		// scene rect colors are premultiplied
		float color[4] = {
			a * lerp(0.05, 1.0, mix), a * lerp(0.035, 0.3, mix), a * lerp(0.025, 0.04, mix), a,
		};
		int w = (int)fmax(1, round(p->scorch_w * scale_x));
		int h = (int)fmax(1, round(p->scorch_h * scale_y));
		wlr_scene_rect_set_color(p->scorch, color);
		wlr_scene_rect_set_size(p->scorch, w, h);
		wlr_scene_node_set_position(&p->scorch->node,
			(int)round((p->scorch_x - p->w / 2) * scale_x),
			(int)round((p->scorch_y - p->h / 2) * scale_y));
		wlr_scene_node_set_enabled(&p->scorch->node, true);
	}
	for (int i = 0; i < p->nodes->length; i++) {
		struct shard_node *n = p->nodes->items[i];
		int w = (int)fmax(1, round(n->w * scale_x)), h = (int)fmax(1, round(n->h * scale_y));
		wlr_scene_node_set_position(n->node, (int)round((n->x - p->w / 2) * scale_x),
			(int)round((n->y - p->h / 2) * scale_y));
		if (n->rect) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(n->node);
			float color[4] = { n->color[0], n->color[1], n->color[2], n->color[3] * p->alpha };
			wlr_scene_rect_set_size(rect, w, h);
			wlr_scene_rect_set_color(rect, color);
		} else {
			wlr_scene_buffer_set_dest_size(wlr_scene_buffer_from_node(n->node), w, h);
		}
	}
}

/* ---------- fire, smoke, sparks and light ---------- */

static void spawn_effects(struct tw_explosion *e) {
	double small = fmin(e->width, e->height);
	double base = fmin(760, fmax(220, 0.9 * sqrt(e->width * e->height))) * e->power;
	static const enum wl_output_transform transforms[] = {
		WL_OUTPUT_TRANSFORM_NORMAL, WL_OUTPUT_TRANSFORM_90, WL_OUTPUT_TRANSFORM_180,
		WL_OUTPUT_TRANSFORM_270, WL_OUTPUT_TRANSFORM_FLIPPED, WL_OUTPUT_TRANSFORM_FLIPPED_90,
		WL_OUTPUT_TRANSFORM_FLIPPED_180, WL_OUTPUT_TRANSFORM_FLIPPED_270,
	};

	struct particle *p = sprite_particle(e, P_LIGHT, LAYER_LIGHT, SPRITE_LIGHT, false);
	if (p) {
		p->delay = CHARGE_MS * 0.3;
		p->life = 1400;
		p->size = fmax(e->width, e->height) * 3.2;
	}
	p = sprite_particle(e, P_CHARGE, LAYER_FLASH, SPRITE_GLOW, false);
	if (p) {
		p->life = CHARGE_MS + 80;
	}

	int smoke = 5 + (int)(3 * e->power);
	for (int i = 0; i < smoke; i++) {
		p = sprite_particle(e, P_SMOKE, LAYER_SMOKE, SPRITE_PUFF, true);
		if (!p) {
			continue;
		}
		double angle = range(0, TAU), radius = sqrt(rnd()) * 0.3 * small;
		p->x = e->bx + cos(angle) * radius;
		p->y = e->by + sin(angle) * radius;
		double speed = range(20, 90);
		p->vx = cos(angle) * speed;
		p->vy = sin(angle) * speed;
		p->drag = 1.2;
		p->gravity = -range(40, 110);
		p->size = base * range(0.8, 1.35);
		p->delay = CHARGE_MS + range(180, 420);
		p->life = range(1300, 1800);
	}

	int fireballs = 9 + (int)(5 * clamp01(e->width * e->height / (900.0 * 700.0)));
	for (int i = 0; i < fireballs; i++) {
		p = sprite_particle(e, P_FIREBALL, LAYER_FIRE, SPRITE_PUFF, true);
		if (!p) {
			continue;
		}
		double angle = range(0, TAU), radius = i == 0 ? 0 : sqrt(rnd()) * 0.18 * small;
		p->x = e->bx + cos(angle) * radius;
		p->y = e->by + sin(angle) * radius;
		double speed = range(40, 200) * e->power;
		p->vx = cos(angle) * speed;
		p->vy = sin(angle) * speed - range(20, 90);
		p->drag = 1.5;
		p->gravity = -range(120, 260); // hot air rises
		p->size = base * range(0.55, 1.2);
		p->delay = CHARGE_MS + (i == 0 ? 0 : range(0, 130));
		p->life = range(850, 1350);
	}

	for (int i = 0; i < e->particles->length; i++) {
		struct particle *q = e->particles->items[i];
		if (q->type == P_FIREBALL || q->type == P_SMOKE) {
			enum wl_output_transform transform = transforms[(int)range(0, 8) % 8];
			if (q->a) {
				wlr_scene_buffer_set_transform(q->a, transform);
			}
			if (q->b) {
				wlr_scene_buffer_set_transform(q->b, transform);
			}
		}
	}

	p = sprite_particle(e, P_RING, LAYER_RING, SPRITE_RING, false);
	if (p) {
		p->delay = CHARGE_MS + 15;
		p->life = 700;
	}
	p = sprite_particle(e, P_DUST_RING, LAYER_RING, SPRITE_RING, false);
	if (p) {
		p->delay = CHARGE_MS + 80;
		p->life = 1100;
	}

	int sparks = (int)fmin(150, fmax(60, e->width * e->height / 5000));
	for (int i = 0; i < sparks; i++) {
		p = sprite_particle(e, P_SPARK, LAYER_SPARKS, SPRITE_SPARK, false);
		if (!p) {
			continue;
		}
		double angle = range(0, TAU), radius = sqrt(rnd()) * 0.06 * small;
		p->x = e->bx + cos(angle) * radius;
		p->y = e->by + sin(angle) * radius;
		double speed = (350 + 1500 * pow(rnd(), 2.2)) * e->power;
		p->vx = cos(angle) * speed;
		p->vy = sin(angle) * speed - range(60, 300);
		p->drag = range(1.8, 3.0);
		p->gravity = range(700, 1200);
		p->size = range(7, 18);
		p->delay = CHARGE_MS + range(0, 60);
		p->life = range(380, 1150);
	}
	for (int i = 0; i < sparks / 3; i++) {
		p = sprite_particle(e, P_EMBER, LAYER_SPARKS, SPRITE_SPARK, false);
		if (!p) {
			continue;
		}
		double angle = range(0, TAU), radius = sqrt(rnd()) * 0.2 * small;
		p->x = e->bx + cos(angle) * radius;
		p->y = e->by + sin(angle) * radius;
		double speed = range(60, 420) * e->power;
		p->vx = cos(angle) * speed;
		p->vy = sin(angle) * speed;
		p->drag = 1.2;
		p->gravity = -range(20, 120);
		p->size = range(4, 9);
		p->delay = CHARGE_MS + range(80, 400);
		p->life = range(1100, 2000);
	}

	p = sprite_particle(e, P_CORE, LAYER_FLASH, SPRITE_GLOW, false);
	if (p) {
		p->delay = CHARGE_MS;
		p->life = 500;
	}
	p = sprite_particle(e, P_FLASH, LAYER_FLASH, SPRITE_GLOW, false);
	if (p) {
		p->delay = CHARGE_MS;
		p->life = 170;
	}
}

static void update_sprite(struct tw_explosion *e, struct particle *p, double ms) {
	double age = ms - p->delay;
	double n = p->life > 0 ? age / p->life : 1;
	if (p->type == P_CHARGE) {
		if (ms >= p->life) {
			hide_sprite(p->a);
			return;
		}
		double size, alpha;
		if (ms < CHARGE_MS) {
			double c = ms / CHARGE_MS;
			size = e->diag * (0.1 + 0.5 * c * c);
			alpha = pow(c, 1.6) * 0.7;
		} else {
			double m = (ms - CHARGE_MS) / (p->life - CHARGE_MS);
			size = e->diag * (0.6 + 0.3 * m);
			alpha = 0.7 * (1 - m);
		}
		p->alpha_a = alpha;
		show_sprite(p->a, e->bx, e->by, size, size);
		return;
	}
	if (age < 0 || n >= 1) {
		hide_sprite(p->a);
		hide_sprite(p->b);
		return;
	}
	double t = age / 1000, x, y;
	switch (p->type) {
	case P_FIREBALL:
	case P_SMOKE: {
		ballistic(p, t, &x, &y);
		bool fire = p->type == P_FIREBALL;
		double size = fire ? p->size * (0.42 + 0.78 * (1 - exp(-4.0 * n))) :
			p->size * (0.55 + 0.75 * n);
		double f = (fire ? n : 0.5 + 0.5 * n) * (PUFF_FRAMES - 1);
		int frame = (int)f;
		if (frame > PUFF_FRAMES - 2) {
			frame = PUFF_FRAMES - 2;
		}
		double frac = clamp01(f - frame);
		set_frame(p->a, &p->frame_a, frame);
		set_frame(p->b, &p->frame_b, frame + 1);
		double fade = fire ? smoothstep(0, 0.05, n) * (1 - smoothstep(0.8, 1, n)) :
			smoothstep(0, 0.25, n) * (1 - smoothstep(0.55, 1, n)) * 0.8;
		p->alpha_a = fade * (1 - frac);
		p->alpha_b = fade * frac;
		show_sprite(p->a, x, y, size, size);
		show_sprite(p->b, x, y, size, size);
		break;
	}
	case P_SPARK: {
		ballistic(p, t, &x, &y);
		double decay = exp(-p->drag * t);
		double vx = p->vx * decay, vy = p->vy * decay + p->gravity * t;
		// motion blur: stretched along the way it flies
		double stretch_x = 1 + fmin(3, fabs(vx) * 0.006);
		double stretch_y = 1 + fmin(3, fabs(vy) * 0.006);
		double size = p->size * (1 - 0.5 * n);
		p->alpha_a = pow(1 - n, 1.3) * (0.72 + 0.28 * sin(p->seed + age * 0.05));
		show_sprite(p->a, x, y, size * stretch_x, size * stretch_y);
		break;
	}
	case P_EMBER:
		ballistic(p, t, &x, &y);
		x += 10 * sin(age * 0.004 + p->seed);
		p->alpha_a = smoothstep(0, 0.1, n) * (1 - smoothstep(0.6, 1, n)) *
			(0.45 + 0.55 * fabs(sin(age * 0.011 + p->seed)));
		show_sprite(p->a, x, y, p->size, p->size);
		break;
	case P_FLASH: {
		double size = e->diag * (0.3 + 1.0 * ease_out(n, 3));
		p->alpha_a = 0.8 * pow(1 - n, 2.5);
		show_sprite(p->a, e->bx, e->by, size, size);
		break;
	}
	case P_CORE: {
		double size = e->diag * (0.25 + 0.3 * ease_out(n, 2));
		p->alpha_a = pow(1 - n, 1.6) * 0.6;
		show_sprite(p->a, e->bx, e->by, size, size);
		break;
	}
	case P_LIGHT: {
		double size = p->size * (1 + 0.1 * n);
		double flicker = 0.8 + 0.2 * sin(age * 0.047 + sin(age * 0.011) * 2);
		p->alpha_a = smoothstep(0, 0.04, n) * pow(1 - n, 1.8) * flicker;
		show_sprite(p->a, e->bx, e->by, size, size);
		break;
	}
	case P_RING: {
		double size = e->diag * (0.15 + 1.35 * ease_out(n, 2.2));
		p->alpha_a = 0.6 * pow(1 - n, 1.7);
		show_sprite(p->a, e->bx, e->by, size, size);
		break;
	}
	case P_DUST_RING: {
		double size = e->diag * (0.12 + 1.5 * ease_out(n, 2));
		p->alpha_a = 0.35 * pow(1 - n, 2);
		show_sprite(p->a, e->bx, e->by, size, size);
		break;
	}
	case P_SHARD:
	case P_CHARGE:
		break;
	}
}

bool tw_explosion_update(struct tw_explosion *e, double ms, double *shake_x, double *shake_y) {
	for (int i = 0; i < e->particles->length; i++) {
		struct particle *p = e->particles->items[i];
		if (p->type == P_SHARD) {
			update_shard(e, p, ms);
		} else {
			update_sprite(e, p, ms);
		}
	}
	double t = ms - CHARGE_MS;
	if (shake_x && shake_y && t > 0 && t < 450) {
		double amplitude = 11 * e->power * exp(-t / 110);
		*shake_x += amplitude * (sin(t * 0.083) + 0.5 * sin(t * 0.191 + 1.7)) / 1.5;
		*shake_y += amplitude * (cos(t * 0.071 + 0.4) + 0.5 * sin(t * 0.167)) / 1.5;
	}
	return ms < e->length;
}

struct tw_explosion *tw_explosion_create(struct sway_container *con,
		struct wlr_scene_tree *parent) {
	if (!con || !con->scene_tree || !images_ready()) {
		return NULL;
	}
	struct sources sources = { 0 };
	int lx, ly;
	wlr_scene_node_coords(&con->scene_tree->node, &lx, &ly);
	collect(&sources, &con->scene_tree->node, lx, ly, true);
	if (sources.count == 0) {
		free(sources.items);
		return NULL;
	}
	double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
	for (int i = 0; i < sources.count; i++) {
		struct source *s = &sources.items[i];
		x0 = fmin(x0, s->x);
		y0 = fmin(y0, s->y);
		x1 = fmax(x1, s->x + s->w);
		y1 = fmax(y1, s->y + s->h);
	}

	struct tw_explosion *e = calloc(1, sizeof(*e));
	struct cell *cells = calloc(MAX_SHARDS, sizeof(*cells));
	if (!e || !cells || !(e->tree = wlr_scene_tree_create(parent))) {
		free(e);
		free(cells);
		free(sources.items);
		return NULL;
	}
	scene_descriptor_assign(&e->tree->node, SWAY_SCENE_DESC_NON_INTERACTIVE, (void *)1);
	for (int i = 0; i < LAYER_COUNT; i++) {
		e->layers[i] = wlr_scene_tree_create(e->tree);
	}
	e->particles = create_list();
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	rng ^= (uint64_t)now.tv_nsec * 2654435761ull + (uint64_t)now.tv_sec;
	if (!rng) {
		rng = 88172645463325252ull;
	}

	e->width = x1 - x0;
	e->height = y1 - y0;
	e->diag = hypot(e->width, e->height);
	e->power = fmin(1.25, fmax(0.55, e->diag / 1100));
	// it goes off at the center, pulled towards the pointer (e.g. the close button)
	e->bx = x0 + e->width / 2 + range(-0.04, 0.04) * e->width;
	e->by = y0 + e->height / 2 + range(-0.04, 0.04) * e->height;
	struct sway_seat *seat = input_manager_current_seat();
	if (seat && seat->cursor) {
		double cx = seat->cursor->cursor->x, cy = seat->cursor->cursor->y;
		if (cx >= x0 - 10 && cx <= x1 + 10 && cy >= y0 - 10 && cy <= y1 + 10) {
			e->bx += (cx - e->bx) * 0.6;
			e->by += (cy - e->by) * 0.6;
		}
	}

	bool layers_ok = true;
	for (int i = 0; i < LAYER_COUNT; i++) {
		layers_ok = layers_ok && e->layers[i];
	}
	if (layers_ok) {
		int count = shatter(cells, (struct cell){ x0, y0, e->width, e->height },
			e->bx, e->by, e->diag);
		struct wlr_box solid = { con->current.x, con->current.y, con->current.width,
			con->current.height };
		bool opaque = con->view && !con->view->using_csd;
		for (int i = 0; i < count; i++) {
			add_shard(e, cells[i], &sources, opaque ? &solid : NULL);
		}
		spawn_effects(e);
	}
	free(cells);
	free(sources.items);
	for (int i = 0; i < e->particles->length; i++) {
		struct particle *p = e->particles->items[i];
		e->length = fmax(e->length, p->delay + p->life);
	}
	tw_explosion_update(e, 0, NULL, NULL);
	return e;
}

void tw_explosion_raise(struct tw_explosion *e) {
	wlr_scene_node_raise_to_top(&e->tree->node);
}

void tw_explosion_destroy(struct tw_explosion *e) {
	if (!e) {
		return;
	}
	wlr_scene_node_destroy(&e->tree->node);
	for (int i = 0; i < e->particles->length; i++) {
		struct particle *p = e->particles->items[i];
		if (p->nodes) {
			list_free_items_and_destroy(p->nodes);
		}
		free(p);
	}
	list_free(e->particles);
	free(e);
}
