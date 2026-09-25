#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * 3D Maze, like the one of Windows 95: walking through a maze of brick walls
 * over a carpet and under a wooden ceiling, keeping a hand on the wall on the
 * right. Rats scurry through the corridors, grey stones spin in the air and
 * turn the world upside down when walked through, and a smiley face waits at
 * the way out; reaching it starts a new maze. The start is marked by a sign
 * with the Windows flag.
 *
 * It is ray cast, a column at a time, into a small picture that is scaled up
 * (as the original looked, a little coarse), and the things in the maze are
 * drawn over it, cut off where a wall is in front of them.
 */

#define CELLS_W 9
#define CELLS_H 9
#define MAP_W (CELLS_W * 2 + 1)
#define MAP_H (CELLS_H * 2 + 1)
#define TEX 64
#define THINGS_MAX 12
#define RENDER_SCALE 0.4

enum thing_kind {
	THING_SIGN,
	THING_SMILEY,
	THING_RAT,
	THING_STONE,
};

struct thing {
	enum thing_kind kind;
	double x, y;     // in map blocks
	double tx, ty;   // where a rat is going
	double spin;
	bool alive;
};

struct maze {
	unsigned char map[MAP_H][MAP_W]; // 1: wall
	uint32_t wall[TEX * TEX], floor[TEX * TEX], ceiling[TEX * TEX];
	int bw, bh;            // the picture it is cast into
	uint32_t *pixels;
	cairo_surface_t *picture;
	double *depth;         // distance of the wall of each column
	// the walker: from one cell to the next, turning on the way
	double x, y, angle;
	int cx, cy, dir;       // the cell it is in and the way it faces
	int tx, ty;            // the cell it walks to
	double turn_to;
	bool turning, moving;
	int exit_x, exit_y;
	double roll, roll_to;  // upside down after a stone
	double fade, done;
	struct thing things[THINGS_MAX];
	int width, height;
};

static const int step_x[4] = { 1, 0, -1, 0 };
static const int step_y[4] = { 0, 1, 0, -1 };

/* ---------- textures ---------- */

static uint32_t rgb(double r, double g, double b) {
	return 0xff000000u | (uint32_t)(saver_clamp(r, 0, 1) * 255) << 16 |
		(uint32_t)(saver_clamp(g, 0, 1) * 255) << 8 | (uint32_t)(saver_clamp(b, 0, 1) * 255);
}

static void make_textures(struct maze *m) {
	for (int y = 0; y < TEX; y++) {
		for (int x = 0; x < TEX; x++) {
			double noise = saver_random() * 0.12;
			// bricks: rows of eight, every other row shifted by half a brick, grey mortar
			int row = y / 8, bx = (x + (row % 2) * 8) % 16;
			bool mortar = y % 8 == 7 || bx == 15;
			m->wall[y * TEX + x] = mortar ? rgb(0.62 + noise, 0.6 + noise, 0.55 + noise) :
				rgb(0.62 + noise + (row * 7 % 5) * 0.03, 0.22 + noise * 0.5, 0.14 + noise * 0.4);
			// a speckled carpet
			double c = 0.35 + noise * 1.5 + ((x * 7 + y * 13) % 17 == 0 ? 0.15 : 0);
			m->floor[y * TEX + x] = rgb(c * 0.55, c * 0.62, c * 0.75);
			// wooden planks with grain
			double grain = 0.5 + 0.1 * sin(x * 0.9 + sin(y * 0.2) * 2) + noise * 0.5;
			bool seam = x % 16 == 0;
			m->ceiling[y * TEX + x] = seam ? rgb(0.3, 0.2, 0.1) :
				rgb(grain * 1.25, grain * 0.9, grain * 0.55);
		}
	}
}

/* ---------- the maze ---------- */

static void carve(struct maze *m, int cx, int cy) {
	int order[4] = { 0, 1, 2, 3 };
	for (int i = 3; i > 0; i--) {
		int j = (int)(saver_random() * (i + 1)), t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	for (int i = 0; i < 4; i++) {
		int d = order[i], nx = cx + step_x[d], ny = cy + step_y[d];
		if (nx < 0 || ny < 0 || nx >= CELLS_W || ny >= CELLS_H ||
				!m->map[ny * 2 + 1][nx * 2 + 1]) {
			continue;
		}
		m->map[cy * 2 + 1 + step_y[d]][cx * 2 + 1 + step_x[d]] = 0;
		m->map[ny * 2 + 1][nx * 2 + 1] = 0;
		carve(m, nx, ny);
	}
}

static bool open_way(struct maze *m, int cx, int cy, int dir) {
	return !m->map[cy * 2 + 1 + step_y[dir]][cx * 2 + 1 + step_x[dir]];
}

static void thing_add(struct maze *m, enum thing_kind kind, int cx, int cy) {
	for (int i = 0; i < THINGS_MAX; i++) {
		if (!m->things[i].alive) {
			m->things[i] = (struct thing){ kind, cx * 2 + 1.5, cy * 2 + 1.5, cx * 2 + 1.5,
				cy * 2 + 1.5, saver_random() * 6, true };
			return;
		}
	}
}

static void new_maze(struct maze *m) {
	memset(m->map, 1, sizeof(m->map));
	m->map[1][1] = 0;
	carve(m, 0, 0);
	memset(m->things, 0, sizeof(m->things));
	m->cx = m->cy = 0;
	m->dir = open_way(m, 0, 0, 0) ? 0 : 1;
	m->x = 1.5;
	m->y = 1.5;
	m->angle = m->dir * M_PI / 2;
	m->turning = m->moving = false;
	m->exit_x = CELLS_W - 1;
	m->exit_y = CELLS_H - 1;
	m->roll = m->roll_to = 0;
	m->fade = 1;
	m->done = 0;
	// the sign behind the start, the smiley at the way out, rats and stones between
	thing_add(m, THING_SIGN, 0, 0);
	m->things[0].x = 1.5 - step_x[m->dir] * 0.45;
	m->things[0].y = 1.5 - step_y[m->dir] * 0.45;
	thing_add(m, THING_SMILEY, m->exit_x, m->exit_y);
	for (int i = 0; i < 3; i++) {
		thing_add(m, THING_RAT, 2 + (int)(saver_random() * (CELLS_W - 2)),
			(int)(saver_random() * CELLS_H));
	}
	for (int i = 0; i < 3; i++) {
		thing_add(m, THING_STONE, 1 + (int)(saver_random() * (CELLS_W - 1)),
			1 + (int)(saver_random() * (CELLS_H - 1)));
	}
}

/* The next step: right if it can, else straight on, else left, else back. */
static void choose_way(struct maze *m) {
	static const int prefer[4] = { 1, 0, 3, 2 }; // right, ahead, left, back
	for (int i = 0; i < 4; i++) {
		int d = (m->dir + prefer[i]) % 4;
		if (open_way(m, m->cx, m->cy, d)) {
			if (d != m->dir) {
				// the shorter way round
				double target = d * M_PI / 2;
				while (target - m->angle > M_PI) {
					target -= 2 * M_PI;
				}
				while (target - m->angle < -M_PI) {
					target += 2 * M_PI;
				}
				m->turn_to = target;
				m->turning = true;
			}
			m->dir = d;
			m->tx = m->cx + step_x[d];
			m->ty = m->cy + step_y[d];
			m->moving = true;
			return;
		}
	}
}

static void walk(struct maze *m, double dt) {
	if (m->done > 0) {
		m->done -= dt;
		if (m->done <= 0) {
			new_maze(m);
		}
		return;
	}
	if (!m->moving) {
		choose_way(m);
	}
	if (m->turning) {
		double diff = m->turn_to - m->angle, step = dt * 2.6;
		if (fabs(diff) <= step) {
			m->angle = m->turn_to;
			m->turning = false;
		} else {
			m->angle += diff > 0 ? step : -step;
		}
		return;
	}
	double gx = m->tx * 2 + 1.5, gy = m->ty * 2 + 1.5;
	double dx = gx - m->x, dy = gy - m->y, left = hypot(dx, dy), step = dt * 1.8;
	if (left <= step) {
		m->x = gx;
		m->y = gy;
		m->cx = m->tx;
		m->cy = m->ty;
		m->moving = false;
		if (m->cx == m->exit_x && m->cy == m->exit_y) {
			m->done = 1.2; // the smiley is reached: a moment of it, then a new maze
		}
	} else {
		m->x += dx / left * step;
		m->y += dy / left * step;
	}
	// a stone walked through turns the world over
	for (int i = 0; i < THINGS_MAX; i++) {
		struct thing *t = &m->things[i];
		if (t->alive && t->kind == THING_STONE && hypot(t->x - m->x, t->y - m->y) < 0.5) {
			t->alive = false;
			m->roll_to += M_PI;
		}
	}
}

static void move_rats(struct maze *m, double dt) {
	for (int i = 0; i < THINGS_MAX; i++) {
		struct thing *t = &m->things[i];
		t->spin += dt * 2.5;
		if (!t->alive || t->kind != THING_RAT) {
			continue;
		}
		double dx = t->tx - t->x, dy = t->ty - t->y, left = hypot(dx, dy), step = dt * 2.4;
		if (left <= step) {
			t->x = t->tx;
			t->y = t->ty;
			int cx = (int)(t->x / 2), cy = (int)(t->y / 2);
			int d = (int)(saver_random() * 4);
			for (int k = 0; k < 4 && !open_way(m, cx, cy, d); k++) {
				d = (d + 1) % 4;
			}
			t->tx = (cx + step_x[d]) * 2 + 1.5;
			t->ty = (cy + step_y[d]) * 2 + 1.5;
		} else {
			t->x += dx / left * step;
			t->y += dy / left * step;
		}
	}
}

/* ---------- casting ---------- */

static void cast(struct maze *m) {
	int w = m->bw, h = m->bh;
	double dirx = cos(m->angle), diry = sin(m->angle);
	double planex = -diry * 0.72, planey = dirx * 0.72; // about 70 degrees of view
	// floor and ceiling, a row at a time
	for (int y = h / 2 + 1; y < h; y++) {
		double row = (double)h / (2.0 * y - h);
		double fx = m->x + row * (dirx - planex), fy = m->y + row * (diry - planey);
		double sx = row * 2 * planex / w, sy = row * 2 * planey / w;
		double shade = saver_clamp(1.4 / (row + 0.6), 0.25, 1);
		for (int x = 0; x < w; x++, fx += sx, fy += sy) {
			int tx = (int)(fx * TEX) & (TEX - 1), ty = (int)(fy * TEX) & (TEX - 1);
			uint32_t f = m->floor[ty * TEX + tx], c = m->ceiling[ty * TEX + tx];
			uint32_t fs = rgb((f >> 16 & 255) / 255.0 * shade, (f >> 8 & 255) / 255.0 * shade,
				(f & 255) / 255.0 * shade);
			uint32_t cs = rgb((c >> 16 & 255) / 255.0 * shade, (c >> 8 & 255) / 255.0 * shade,
				(c & 255) / 255.0 * shade);
			m->pixels[y * w + x] = fs;
			m->pixels[(h - 1 - y) * w + x] = cs;
		}
	}
	// the walls, a column at a time
	for (int x = 0; x < w; x++) {
		double camera = 2.0 * x / w - 1;
		double rx = dirx + planex * camera, ry = diry + planey * camera;
		int mx = (int)m->x, my = (int)m->y;
		double ddx = fabs(1 / rx), ddy = fabs(1 / ry), sdx, sdy;
		int stx = rx < 0 ? -1 : 1, sty = ry < 0 ? -1 : 1;
		sdx = rx < 0 ? (m->x - mx) * ddx : (mx + 1 - m->x) * ddx;
		sdy = ry < 0 ? (m->y - my) * ddy : (my + 1 - m->y) * ddy;
		int side = 0;
		for (int guard = 0; guard < 64; guard++) {
			if (sdx < sdy) {
				sdx += ddx;
				mx += stx;
				side = 0;
			} else {
				sdy += ddy;
				my += sty;
				side = 1;
			}
			if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H || m->map[my][mx]) {
				break;
			}
		}
		double dist = side == 0 ? sdx - ddx : sdy - ddy;
		dist = dist < 0.05 ? 0.05 : dist;
		m->depth[x] = dist;
		double hit = side == 0 ? m->y + dist * ry : m->x + dist * rx;
		int tx = (int)((hit - floor(hit)) * TEX) & (TEX - 1);
		int line = (int)(h / dist), top = (h - line) / 2;
		double shade = saver_clamp(1.4 / (dist + 0.6), 0.25, 1) * (side ? 0.78 : 1);
		for (int y = top < 0 ? 0 : top; y < top + line && y < h; y++) {
			int ty = (int)((y - top) * (double)TEX / line) & (TEX - 1);
			uint32_t c = m->wall[ty * TEX + tx];
			m->pixels[y * w + x] = rgb((c >> 16 & 255) / 255.0 * shade,
				(c >> 8 & 255) / 255.0 * shade, (c & 255) / 255.0 * shade);
		}
	}
}

/* ---------- the things in it ---------- */

static void draw_thing(cairo_t *cr, const struct thing *t, double x, double y, double size) {
	switch (t->kind) {
	case THING_SMILEY:
		cairo_arc(cr, x, y, size * 0.4, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 1, 0.9, 0.1);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0.3, 0.2, 0);
		cairo_set_line_width(cr, size * 0.03);
		cairo_stroke(cr);
		cairo_arc(cr, x - size * 0.13, y - size * 0.1, size * 0.05, 0, 2 * M_PI);
		cairo_arc(cr, x + size * 0.13, y - size * 0.1, size * 0.05, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, x, y + size * 0.02, size * 0.22, 0.15 * M_PI, 0.85 * M_PI);
		cairo_set_line_width(cr, size * 0.05);
		cairo_stroke(cr);
		break;
	case THING_RAT: {
		double by = y + size * 0.36;
		cairo_save(cr);
		cairo_translate(cr, x, by);
		cairo_scale(cr, 1, 0.55);
		cairo_arc(cr, 0, 0, size * 0.16, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_set_source_rgb(cr, 0.45, 0.43, 0.42);
		cairo_fill(cr);
		cairo_arc(cr, x + size * 0.14, by - size * 0.03, size * 0.07, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, x + size * 0.1, by - size * 0.1, size * 0.035, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 0.9, 0.6, 0.65);
		cairo_fill(cr);
		cairo_move_to(cr, x - size * 0.15, by);
		cairo_curve_to(cr, x - size * 0.3, by - size * 0.1, x - size * 0.3, by + size * 0.08,
			x - size * 0.42, by - size * 0.02);
		cairo_set_line_width(cr, size * 0.02);
		cairo_stroke(cr);
		break;
	}
	case THING_STONE: {
		// a turning twelve-sided stone, shaded face by face
		double r = size * 0.22, cy = y + sin(t->spin * 1.3) * size * 0.05;
		for (int k = 0; k < 6; k++) {
			double a0 = t->spin + k * M_PI / 3, a1 = a0 + M_PI / 3;
			cairo_move_to(cr, x, cy);
			cairo_line_to(cr, x + cos(a0) * r, cy + sin(a0) * r * 0.9);
			cairo_line_to(cr, x + cos(a1) * r, cy + sin(a1) * r * 0.9);
			cairo_close_path(cr);
			double light = 0.45 + 0.35 * cos(a0 + 0.5 - M_PI * 0.75);
			cairo_set_source_rgb(cr, light, light, light * 1.05);
			cairo_fill(cr);
		}
		break;
	}
	case THING_SIGN: {
		// a board on a post with the four panes of the Windows flag
		cairo_rectangle(cr, x - size * 0.03, y, size * 0.06, size * 0.5);
		cairo_set_source_rgb(cr, 0.4, 0.28, 0.15);
		cairo_fill(cr);
		cairo_rectangle(cr, x - size * 0.3, y - size * 0.3, size * 0.6, size * 0.36);
		cairo_set_source_rgb(cr, 0.95, 0.95, 0.9);
		cairo_fill(cr);
		static const double panes[4][3] = {
			{ 0.93, 0.26, 0.16 }, { 0.3, 0.69, 0.31 }, { 0.13, 0.49, 0.9 }, { 0.98, 0.75, 0.1 },
		};
		for (int k = 0; k < 4; k++) {
			cairo_rectangle(cr, x - size * 0.1 + (k % 2) * size * 0.105,
				y - size * 0.24 + (k / 2) * size * 0.125, size * 0.095, size * 0.115);
			cairo_set_source_rgb(cr, panes[k][0], panes[k][1], panes[k][2]);
			cairo_fill(cr);
		}
		break;
	}
	}
}

static int thing_cmp_distance(const void *a, const void *b) {
	double da = ((const double *)a)[0], db = ((const double *)b)[0];
	return da < db ? 1 : da > db ? -1 : 0;
}

static void draw_things(struct maze *m, cairo_t *cr) {
	double dirx = cos(m->angle), diry = sin(m->angle);
	double planex = -diry * 0.72, planey = dirx * 0.72;
	double inv = 1 / (planex * diry - dirx * planey);
	double order[THINGS_MAX][2];
	int count = 0;
	for (int i = 0; i < THINGS_MAX; i++) {
		struct thing *t = &m->things[i];
		if (t->alive) {
			order[count][0] = hypot(t->x - m->x, t->y - m->y);
			order[count][1] = i;
			count++;
		}
	}
	qsort(order, count, sizeof(order[0]), thing_cmp_distance);
	for (int k = 0; k < count; k++) {
		struct thing *t = &m->things[(int)order[k][1]];
		double rx = t->x - m->x, ry = t->y - m->y;
		double tx = inv * (diry * rx - dirx * ry), depth = inv * (-planey * rx + planex * ry);
		if (depth < 0.15) {
			continue;
		}
		double sx = m->bw / 2.0 * (1 + tx / depth), size = m->bh / depth;
		int x0 = (int)(sx - size / 2), x1 = (int)(sx + size / 2);
		// only the columns where no wall is nearer
		cairo_save(cr);
		cairo_new_path(cr);
		int run = -1;
		for (int x = x0 < 0 ? 0 : x0; x <= x1 + 1 && x <= m->bw; x++) {
			bool visible = x < m->bw && x <= x1 && m->depth[x] > depth;
			if (visible && run < 0) {
				run = x;
			} else if (!visible && run >= 0) {
				cairo_rectangle(cr, run, 0, x - run, m->bh);
				run = -1;
			}
		}
		cairo_clip(cr);
		draw_thing(cr, t, sx, m->bh / 2.0, size);
		cairo_restore(cr);
	}
}

/* ---------- the saver ---------- */

static void *maze_create(int width, int height, const struct saver_options *options) {
	struct maze *m = calloc(1, sizeof(*m));
	m->width = width;
	m->height = height;
	m->bw = (int)fmax(64, width * RENDER_SCALE);
	m->bh = (int)fmax(40, height * RENDER_SCALE);
	m->picture = cairo_image_surface_create(CAIRO_FORMAT_RGB24, m->bw, m->bh);
	m->pixels = (uint32_t *)cairo_image_surface_get_data(m->picture);
	m->depth = calloc(m->bw, sizeof(double));
	make_textures(m);
	new_maze(m);
	return m;
}

static void maze_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct maze *m = state;
	walk(m, dt);
	move_rats(m, dt);
	// the world turns over slowly, as it did
	double diff = m->roll_to - m->roll;
	m->roll += fabs(diff) < dt * 2.2 ? diff : (diff > 0 ? 1 : -1) * dt * 2.2;
	cairo_surface_flush(m->picture);
	if (cairo_image_surface_get_stride(m->picture) != m->bw * 4) {
		return; // never for RGB24, whose rows are whole pixels
	}
	cast(m);
	cairo_surface_mark_dirty(m->picture);
	cairo_t *pc = cairo_create(m->picture);
	draw_things(m, pc);
	cairo_destroy(pc);

	cairo_save(cr);
	cairo_translate(cr, width / 2.0, height / 2.0);
	cairo_rotate(cr, m->roll);
	cairo_scale(cr, (double)width / m->bw, (double)height / m->bh);
	cairo_translate(cr, -m->bw / 2.0, -m->bh / 2.0);
	cairo_set_source_surface(cr, m->picture, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_restore(cr);
	// out of black at the start of a maze, into it at the end
	m->fade = fmax(0, m->fade - dt * 1.5);
	double black = fmax(m->fade, m->done > 0 ? 1 - m->done / 1.2 : 0);
	if (black > 0) {
		cairo_set_source_rgba(cr, 0, 0, 0, black);
		cairo_paint(cr);
	}
}

static void maze_destroy(void *state) {
	struct maze *m = state;
	cairo_surface_destroy(m->picture);
	free(m->depth);
	free(m);
}

const struct saver saver_maze = {
	.name = "maze",
	.title = "3D Maze",
	.description = "Walking through a brick maze with rats, stones that turn the world "
		"upside down and a smiley at the way out, as in Windows 95",
	.create = maze_create,
	.draw = maze_draw,
	.destroy = maze_destroy,
};
