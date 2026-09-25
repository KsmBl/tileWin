#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Screen savers of older Windows: the Starfield Simulation, 3D Pipes of
 * Windows 95 and the Flying Windows of Windows 3.1.
 */

/* ================= Starfield ================= */

#define STARS_MAX 900

struct star {
	double x, y, z, px, py; // px, py: where it was drawn last, for the streak
	bool drawn;
};

struct starfield {
	int count;
	struct star s[STARS_MAX];
	double u;
};

static void star_reset(struct star *s, bool anywhere) {
	s->x = saver_between(-1, 1);
	s->y = saver_between(-1, 1);
	s->z = anywhere ? saver_between(0.05, 1) : 1;
	s->drawn = false;
}

static void *starfield_create(int width, int height, const struct saver_options *options) {
	struct starfield *f = calloc(1, sizeof(*f));
	f->u = saver_unit(width, height);
	f->count = (int)saver_clamp(width * (double)height / 3000, 150, STARS_MAX);
	for (int i = 0; i < f->count; i++) {
		star_reset(&f->s[i], true);
	}
	return f;
}

static void starfield_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct starfield *f = state;
	double cx = width / 2.0, cy = height / 2.0, spread = fmax(width, height) * 0.5;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int i = 0; i < f->count; i++) {
		struct star *s = &f->s[i];
		s->z -= dt * 0.32;
		double sx = cx + s->x / s->z * spread, sy = cy + s->y / s->z * spread;
		if (s->z <= 0.02 || sx < -20 || sx > width + 20 || sy < -20 || sy > height + 20) {
			star_reset(s, false);
			continue;
		}
		double near = 1 - s->z;
		cairo_set_line_width(cr, fmax(0.8, f->u * 3.2 * near));
		cairo_move_to(cr, s->drawn ? s->px : sx, s->drawn ? s->py : sy);
		cairo_line_to(cr, sx, sy);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.25 + 0.75 * near);
		cairo_stroke(cr);
		s->px = sx;
		s->py = sy;
		s->drawn = true;
	}
}

const struct saver saver_starfield = {
	.name = "starfield",
	.title = "Starfield",
	.description = "Flying through the stars, from Windows 3.1 and 95",
	.create = starfield_create,
	.draw = starfield_draw,
	.destroy = free,
};

/* ================= 3D Pipes ================= */

#define PIPES_X 16
#define PIPES_Y 11
#define PIPES_Z 11
#define PIPE_RADIUS 0.2

static const double pipe_colors[][3] = {
	{ 0.85, 0.12, 0.1 }, { 0.1, 0.65, 0.2 }, { 0.15, 0.35, 0.9 }, { 0.9, 0.75, 0.1 },
	{ 0.1, 0.75, 0.8 }, { 0.75, 0.2, 0.75 }, { 0.85, 0.85, 0.85 }, { 0.95, 0.45, 0.1 },
};

static const int pipe_dirs[6][3] = {
	{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};

struct pipes {
	bool taken[PIPES_X][PIPES_Y][PIPES_Z];
	int x, y, z, dir, color;
	bool growing;
	int segments, stuck;
	double since_step;
	cairo_surface_t *canvas; // the pipes so far; only the new piece is drawn each time
	int width, height;
	double yaw, pitch, scale;
};

static void pipes_clear(struct pipes *p) {
	memset(p->taken, 0, sizeof(p->taken));
	p->segments = 0;
	p->growing = false;
	cairo_t *cr = cairo_create(p->canvas);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	// every new round is seen from a little different angle
	p->yaw = saver_between(-0.6, 0.6);
	p->pitch = saver_between(-0.35, 0.35);
}

static void *pipes_create(int width, int height, const struct saver_options *options) {
	struct pipes *p = calloc(1, sizeof(*p));
	p->width = width;
	p->height = height;
	p->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	p->scale = fmin(width / (double)PIPES_X, height / (double)PIPES_Y) * 0.95;
	pipes_clear(p);
	return p;
}

/* Where a point of the grid is on the screen, and how much nearer than the middle. */
static void pipes_project(struct pipes *p, double x, double y, double z, double *sx, double *sy,
		double *persp) {
	x -= (PIPES_X - 1) / 2.0;
	y -= (PIPES_Y - 1) / 2.0;
	z -= (PIPES_Z - 1) / 2.0;
	double cy = cos(p->yaw), sy_ = sin(p->yaw), cp = cos(p->pitch), sp = sin(p->pitch);
	double x1 = x * cy + z * sy_, z1 = -x * sy_ + z * cy;
	double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
	double camera = 26;
	*persp = camera / (camera + z2);
	*sx = p->width / 2.0 + x1 * *persp * p->scale;
	*sy = p->height / 2.0 + y2 * *persp * p->scale;
}

static void pipes_ball(struct pipes *p, cairo_t *cr, int x, int y, int z, double size) {
	double sx, sy, persp;
	pipes_project(p, x, y, z, &sx, &sy, &persp);
	double r = PIPE_RADIUS * size * persp * p->scale;
	const double *c = pipe_colors[p->color];
	cairo_pattern_t *ball = cairo_pattern_create_radial(sx - r * 0.35, sy - r * 0.35, r * 0.05,
		sx, sy, r);
	cairo_pattern_add_color_stop_rgb(ball, 0, fmin(1, c[0] + 0.6), fmin(1, c[1] + 0.6),
		fmin(1, c[2] + 0.6));
	cairo_pattern_add_color_stop_rgb(ball, 0.5, c[0], c[1], c[2]);
	cairo_pattern_add_color_stop_rgb(ball, 1, c[0] * 0.25, c[1] * 0.25, c[2] * 0.25);
	cairo_arc(cr, sx, sy, r, 0, 2 * M_PI);
	cairo_set_source(cr, ball);
	cairo_fill(cr);
	cairo_pattern_destroy(ball);
}

static void pipes_segment(struct pipes *p, cairo_t *cr, int x0, int y0, int z0, int x1, int y1,
		int z1) {
	double ax, ay, ap, bx, by, bp;
	pipes_project(p, x0, y0, z0, &ax, &ay, &ap);
	pipes_project(p, x1, y1, z1, &bx, &by, &bp);
	double dx = bx - ax, dy = by - ay, len = hypot(dx, dy);
	const double *c = pipe_colors[p->color];
	double ra = PIPE_RADIUS * ap * p->scale, rb = PIPE_RADIUS * bp * p->scale;
	if (len < 0.5) {
		// straight at us: the end of the pipe is all that shows
		pipes_ball(p, cr, x1, y1, z1, 1);
		return;
	}
	double nx = -dy / len, ny = dx / len;
	cairo_move_to(cr, ax + nx * ra, ay + ny * ra);
	cairo_line_to(cr, bx + nx * rb, by + ny * rb);
	cairo_line_to(cr, bx - nx * rb, by - ny * rb);
	cairo_line_to(cr, ax - nx * ra, ay - ny * ra);
	cairo_close_path(cr);
	// round like a tube: dark at the sides, a light line along it
	double r = (ra + rb) / 2, mx = (ax + bx) / 2, my = (ay + by) / 2;
	cairo_pattern_t *tube = cairo_pattern_create_linear(mx + nx * r, my + ny * r,
		mx - nx * r, my - ny * r);
	cairo_pattern_add_color_stop_rgb(tube, 0, c[0] * 0.2, c[1] * 0.2, c[2] * 0.2);
	cairo_pattern_add_color_stop_rgb(tube, 0.3, c[0], c[1], c[2]);
	cairo_pattern_add_color_stop_rgb(tube, 0.45, fmin(1, c[0] + 0.55), fmin(1, c[1] + 0.55),
		fmin(1, c[2] + 0.55));
	cairo_pattern_add_color_stop_rgb(tube, 0.62, c[0], c[1], c[2]);
	cairo_pattern_add_color_stop_rgb(tube, 1, c[0] * 0.15, c[1] * 0.15, c[2] * 0.15);
	cairo_set_source(cr, tube);
	cairo_fill(cr);
	cairo_pattern_destroy(tube);
}

static bool pipes_free(struct pipes *p, int x, int y, int z) {
	return x >= 0 && x < PIPES_X && y >= 0 && y < PIPES_Y && z >= 0 && z < PIPES_Z &&
		!p->taken[x][y][z];
}

/* A new pipe from a free cell, in a new color; false when none was found. */
static bool pipes_start(struct pipes *p, cairo_t *cr) {
	for (int tries = 0; tries < 40; tries++) {
		int x = (int)(saver_random() * PIPES_X), y = (int)(saver_random() * PIPES_Y);
		int z = (int)(saver_random() * PIPES_Z);
		if (pipes_free(p, x, y, z)) {
			p->x = x;
			p->y = y;
			p->z = z;
			p->taken[x][y][z] = true;
			p->dir = (int)(saver_random() * 6);
			p->color = (p->color + 1 + (int)(saver_random() * 7)) % 8;
			p->growing = true;
			pipes_ball(p, cr, x, y, z, 1.25);
			return true;
		}
	}
	return false;
}

static void pipes_step(struct pipes *p, cairo_t *cr) {
	if (!p->growing && !pipes_start(p, cr)) {
		pipes_clear(p);
		return;
	}
	// mostly straight on, sometimes a turn, and a turn when it has to
	int dir = p->dir;
	const int *d = pipe_dirs[dir];
	if (saver_random() < 0.25 || !pipes_free(p, p->x + d[0], p->y + d[1], p->z + d[2])) {
		int options[6], count = 0;
		for (int i = 0; i < 6; i++) {
			const int *o = pipe_dirs[i];
			if (i != dir && pipes_free(p, p->x + o[0], p->y + o[1], p->z + o[2])) {
				options[count++] = i;
			}
		}
		if (count == 0 && !pipes_free(p, p->x + d[0], p->y + d[1], p->z + d[2])) {
			pipes_ball(p, cr, p->x, p->y, p->z, 1.25); // stuck: an end cap
			p->growing = false;
			return;
		}
		if (count > 0) {
			dir = options[(int)(saver_random() * count)];
		}
	}
	d = pipe_dirs[dir];
	int nx = p->x + d[0], ny = p->y + d[1], nz = p->z + d[2];
	pipes_segment(p, cr, p->x, p->y, p->z, nx, ny, nz);
	if (dir != p->dir) {
		pipes_ball(p, cr, p->x, p->y, p->z, 1.25); // the joint of a turn
	}
	p->dir = dir;
	p->x = nx;
	p->y = ny;
	p->z = nz;
	p->taken[nx][ny][nz] = true;
	if (++p->segments > PIPES_X * PIPES_Y * PIPES_Z / 5) {
		pipes_clear(p);
	}
}

static void pipes_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct pipes *p = state;
	p->since_step += dt;
	cairo_t *canvas = cairo_create(p->canvas);
	for (int steps = 0; p->since_step >= 1 / 14.0 && steps < 8; steps++) {
		p->since_step -= 1 / 14.0;
		pipes_step(p, canvas);
	}
	cairo_destroy(canvas);
	cairo_set_source_surface(cr, p->canvas, 0, 0);
	cairo_paint(cr);
}

static void pipes_destroy(void *state) {
	struct pipes *p = state;
	cairo_surface_destroy(p->canvas);
	free(p);
}

const struct saver saver_pipes = {
	.name = "pipes",
	.title = "3D Pipes",
	.description = "Pipes growing through space and turning at shiny joints, from Windows 95",
	.create = pipes_create,
	.draw = pipes_draw,
	.destroy = pipes_destroy,
};

/* ================= Flying Windows ================= */

#define FLAGS 48

struct flag {
	double x, y, z, phase;
};

struct flying {
	struct flag f[FLAGS];
	int order[FLAGS];
	double u;
};

static void flag_reset(struct flag *f, bool anywhere) {
	f->x = saver_between(-1, 1);
	f->y = saver_between(-1, 1);
	f->z = anywhere ? saver_between(0.1, 1) : 1;
	f->phase = saver_between(0, 2 * M_PI);
}

static void *flying_create(int width, int height, const struct saver_options *options) {
	struct flying *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	for (int i = 0; i < FLAGS; i++) {
		flag_reset(&s->f[i], true);
		s->order[i] = i;
	}
	return s;
}

static struct flying *sorting;

static int flag_cmp(const void *a, const void *b) {
	double za = sorting->f[*(const int *)a].z, zb = sorting->f[*(const int *)b].z;
	return za < zb ? 1 : za > zb ? -1 : 0;
}

/* The waving four-color flag of Windows 3.1, size across, centered at x, y. */
static void flag_draw(cairo_t *cr, double x, double y, double size, double phase,
		double alpha) {
	static const double colors[4][3] = {
		{ 0.93, 0.26, 0.16 }, { 0.3, 0.69, 0.31 }, { 0.13, 0.49, 0.9 }, { 0.98, 0.75, 0.1 },
	};
	double half = size / 2, gap = size * 0.05;
	for (int pane = 0; pane < 4; pane++) {
		double x0 = pane % 2 ? gap / 2 : -half, x1 = pane % 2 ? half : -gap / 2;
		double y0 = pane < 2 ? -half : gap / 2, y1 = pane < 2 ? -gap / 2 : half;
		double w0 = sin(phase + x0 / size * 4) * size * 0.08;
		double w1 = sin(phase + x1 / size * 4) * size * 0.08;
		// the flag leans back a little as it waves, as the original did
		double lean = size * 0.12;
		cairo_move_to(cr, x + x0 + lean * (y0 / size), y + y0 + w0);
		cairo_line_to(cr, x + x1 + lean * (y0 / size), y + y0 + w1);
		cairo_line_to(cr, x + x1 + lean * (y1 / size), y + y1 + w1);
		cairo_line_to(cr, x + x0 + lean * (y1 / size), y + y1 + w0);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, colors[pane][0], colors[pane][1], colors[pane][2], alpha);
		cairo_fill(cr);
	}
}

static void flying_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct flying *s = state;
	double cx = width / 2.0, cy = height / 2.0, spread = fmax(width, height) * 0.55;
	for (int i = 0; i < FLAGS; i++) {
		struct flag *f = &s->f[i];
		f->z -= dt * 0.22;
		f->phase += dt * 5;
		double sx = cx + f->x / f->z * spread, sy = cy + f->y / f->z * spread;
		double size = s->u * 34 / f->z;
		if (f->z <= 0.04 || sx < -size || sx > width + size || sy < -size || sy > height + size) {
			flag_reset(f, false);
		}
	}
	sorting = s;
	qsort(s->order, FLAGS, sizeof(int), flag_cmp);
	for (int k = 0; k < FLAGS; k++) {
		struct flag *f = &s->f[s->order[k]];
		double sx = cx + f->x / f->z * spread, sy = cy + f->y / f->z * spread;
		double alpha = saver_clamp((1 - f->z) * 4, 0, 1); // out of the dark
		flag_draw(cr, sx, sy, s->u * 34 / f->z, f->phase, alpha);
	}
}

const struct saver saver_flying = {
	.name = "flyingwindows",
	.title = "Flying Windows",
	.description = "Waving Windows flags flying towards you, from Windows 3.1",
	.create = flying_create,
	.draw = flying_draw,
	.destroy = free,
};
