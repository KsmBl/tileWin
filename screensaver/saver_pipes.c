#include <float.h>
#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * 3D Pipes, the way Windows 95 to XP drew them with OpenGL: pipes of one
 * color each grow through a box seen straight on, one piece at a time, turn
 * in smooth elbows (now and then in a ball joint, and very rarely in a
 * teapot), pass in front of and behind each other, and shine with a white
 * highlight. When the box is full the screen goes black and it starts over.
 *
 * Each new piece is ray-cast into a picture that is kept between frames,
 * together with how far away every pixel of it is, so a pipe behind another
 * one really is behind it. Only the new pieces cost anything.
 */

#define GRID_Y 10
#define GRID_Z 10
#define GRID_X_MAX 32
#define PIPES_AT_ONCE 3 // at most: the setting says how many
#define RADIUS 0.2
#define BEND 0.36      // how far before a joint the elbow begins
#define BALL 0.33
#define STEP 1 / 9.0   // seconds per piece

static const double colors[][3] = {
	{ 0.85, 0.08, 0.06 }, { 0.1, 0.72, 0.12 }, { 0.12, 0.25, 0.9 }, { 0.92, 0.82, 0.1 },
	{ 0.1, 0.78, 0.82 }, { 0.8, 0.12, 0.8 }, { 0.78, 0.78, 0.78 },
};

static const int dirs[6][3] = {
	{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};

struct pipe {
	bool alive;
	int x, y, z, dir, color;
	double drawn;      // how far along the current straight run it is drawn
	bool first;        // the run starts the pipe: it gets an end cap
};

struct pipes {
	int width, height, gx;
	unsigned char *taken; // gx * GRID_Y * GRID_Z
	cairo_surface_t *canvas;
	float *depth;
	struct pipe pipe[PIPES_AT_ONCE];
	double since, fade, focal, eye;
	int pieces, color_next;
	int pipes;             // growing at once
	double ball, teapot;   // how likely a joint is a ball, or the teapot
};

/* ---------- the camera ---------- */

/* Grid coordinates to the world, which is centered in front of the eye. */
static void world(struct pipes *p, double gx, double gy, double gz, double out[3]) {
	out[0] = gx - (p->gx - 1) / 2.0;
	out[1] = (GRID_Y - 1) / 2.0 - gy;
	out[2] = gz;
}

static void project(struct pipes *p, const double w[3], double *sx, double *sy) {
	double z = w[2] + p->eye;
	*sx = p->width / 2.0 + p->focal * w[0] / z;
	*sy = p->height / 2.0 - p->focal * w[1] / z;
}

/* ---------- shapes, found by the ray of each pixel ---------- */

enum shape_kind {
	SHAPE_SPHERE,
	SHAPE_CYLINDER,
	SHAPE_ELLIPSOID,
};

struct shape {
	enum shape_kind kind;
	double c[3];      // center; for a cylinder a point of its axis
	double r[3];      // radius; for an ellipsoid one per axis
	int axis;         // cylinder: 0, 1 or 2
	double lo, hi;    // cylinder: its extent along the axis
	bool cap_lo, cap_hi;
};

/* The nearest hit of the ray o + t d, t > 0; sets the normal. */
static bool hit(const struct shape *s, const double o[3], const double d[3], double *t,
		double n[3]) {
	double best = DBL_MAX;
	switch (s->kind) {
	case SHAPE_SPHERE:
	case SHAPE_ELLIPSOID: {
		// an ellipsoid is a sphere in a stretched space
		double oc[3], dd[3];
		for (int i = 0; i < 3; i++) {
			double r = s->kind == SHAPE_SPHERE ? s->r[0] : s->r[i];
			oc[i] = (o[i] - s->c[i]) / r;
			dd[i] = d[i] / r;
		}
		double a = dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2];
		double b = oc[0] * dd[0] + oc[1] * dd[1] + oc[2] * dd[2];
		double c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - 1;
		double disc = b * b - a * c;
		if (disc < 0) {
			return false;
		}
		double root = (-b - sqrt(disc)) / a;
		if (root <= 0) {
			return false;
		}
		best = root;
		for (int i = 0; i < 3; i++) {
			double r = s->kind == SHAPE_SPHERE ? s->r[0] : s->r[i];
			n[i] = (oc[i] + dd[i] * root) / r;
		}
		break;
	}
	case SHAPE_CYLINDER: {
		int a = s->axis, u = (a + 1) % 3, v = (a + 2) % 3;
		double ou = o[u] - s->c[u], ov = o[v] - s->c[v], r = s->r[0];
		double qa = d[u] * d[u] + d[v] * d[v];
		double qb = ou * d[u] + ov * d[v];
		double qc = ou * ou + ov * ov - r * r;
		double disc = qb * qb - qa * qc;
		if (qa > 1e-12 && disc >= 0) {
			double root = (-qb - sqrt(disc)) / qa;
			double along = o[a] + d[a] * root;
			if (root > 0 && along >= s->lo && along <= s->hi) {
				best = root;
				n[a] = 0;
				n[u] = (ou + d[u] * root) / r;
				n[v] = (ov + d[v] * root) / r;
			}
		}
		// the flat ends of a pipe that starts or stops
		for (int end = 0; end < 2; end++) {
			if (!(end ? s->cap_hi : s->cap_lo) || fabs(d[a]) < 1e-12) {
				continue;
			}
			double plane = end ? s->hi : s->lo;
			double root = (plane - o[a]) / d[a];
			double pu = ou + d[u] * root, pv = ov + d[v] * root;
			if (root > 0 && root < best && pu * pu + pv * pv <= r * r) {
				best = root;
				n[0] = n[1] = n[2] = 0;
				n[a] = end ? 1 : -1;
			}
		}
		if (best == DBL_MAX) {
			return false;
		}
		break;
	}
	}
	double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	for (int i = 0; i < 3; i++) {
		n[i] /= len;
	}
	*t = best;
	return true;
}

/* Where on the screen a shape can show: the corners of its box, projected. */
static void shape_bounds(struct pipes *p, const struct shape *s, int *x0, int *y0, int *x1,
		int *y1) {
	double lo[3], hi[3];
	for (int i = 0; i < 3; i++) {
		double r = s->kind == SHAPE_ELLIPSOID ? s->r[i] : s->r[0];
		lo[i] = s->c[i] - r;
		hi[i] = s->c[i] + r;
	}
	if (s->kind == SHAPE_CYLINDER) {
		lo[s->axis] = s->lo;
		hi[s->axis] = s->hi;
	}
	double minx = DBL_MAX, miny = DBL_MAX, maxx = -DBL_MAX, maxy = -DBL_MAX;
	for (int k = 0; k < 8; k++) {
		double corner[3] = { k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2] };
		double sx, sy;
		project(p, corner, &sx, &sy);
		minx = fmin(minx, sx);
		maxx = fmax(maxx, sx);
		miny = fmin(miny, sy);
		maxy = fmax(maxy, sy);
	}
	*x0 = (int)saver_clamp(floor(minx) - 1, 0, p->width);
	*y0 = (int)saver_clamp(floor(miny) - 1, 0, p->height);
	*x1 = (int)saver_clamp(ceil(maxx) + 1, 0, p->width);
	*y1 = (int)saver_clamp(ceil(maxy) + 1, 0, p->height);
}

/* Draws a shape lit the way OpenGL lit it: a little ambient light, the lamp, and a highlight. */
static void render(struct pipes *p, const struct shape *s, const double color[3]) {
	int x0, y0, x1, y1;
	shape_bounds(p, s, &x0, &y0, &x1, &y1);
	cairo_surface_flush(p->canvas);
	unsigned char *data = cairo_image_surface_get_data(p->canvas);
	int stride = cairo_image_surface_get_stride(p->canvas);
	double o[3] = { 0, 0, -p->eye };
	static const double light[3] = { -0.42, 0.5, -0.76 }; // from the upper left, behind the eye
	for (int y = y0; y < y1; y++) {
		uint32_t *row = (uint32_t *)(data + y * stride);
		for (int x = x0; x < x1; x++) {
			double d[3] = { (x + 0.5 - p->width / 2.0) / p->focal,
				-(y + 0.5 - p->height / 2.0) / p->focal, 1 };
			double t, n[3];
			if (!hit(s, o, d, &t, n) || t >= p->depth[y * p->width + x]) {
				continue;
			}
			p->depth[y * p->width + x] = (float)t;
			double dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			double v[3] = { -d[0] / dl, -d[1] / dl, -d[2] / dl };
			double lm = sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
			double l[3] = { light[0] / lm, light[1] / lm, light[2] / lm };
			double nl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
			double diffuse = nl > 0 ? nl : 0;
			double rv = 2 * nl * (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) -
				(l[0] * v[0] + l[1] * v[1] + l[2] * v[2]);
			double spec = rv > 0 && nl > 0 ? pow(rv, 36) : 0;
			unsigned c[3];
			for (int i = 0; i < 3; i++) {
				double value = color[i] * (0.18 + 0.82 * diffuse) + 0.85 * spec;
				c[i] = (unsigned)(saver_clamp(value, 0, 1) * 255 + 0.5);
			}
			row[x] = 0xff000000u | c[0] << 16 | c[1] << 8 | c[2];
		}
	}
	cairo_surface_mark_dirty_rectangle(p->canvas, x0, y0, x1 - x0, y1 - y0);
}

static void render_sphere(struct pipes *p, const double c[3], double r, const double color[3]) {
	struct shape s = { .kind = SHAPE_SPHERE, .c = { c[0], c[1], c[2] }, .r = { r } };
	render(p, &s, color);
}

/* A straight piece between two points on one axis. */
static void render_run(struct pipes *p, const double a[3], const double b[3], bool cap_a,
		bool cap_b, const double color[3]) {
	int axis = a[0] != b[0] ? 0 : a[1] != b[1] ? 1 : 2;
	struct shape s = { .kind = SHAPE_CYLINDER, .axis = axis, .r = { RADIUS } };
	memcpy(s.c, a, sizeof(s.c));
	bool forward = a[axis] < b[axis];
	s.lo = forward ? a[axis] : b[axis];
	s.hi = forward ? b[axis] : a[axis];
	s.cap_lo = forward ? cap_a : cap_b;
	s.cap_hi = forward ? cap_b : cap_a;
	if (s.hi - s.lo > 1e-6) {
		render(p, &s, color);
	}
}

/* The bend of a turn: a quarter circle of spheres close enough to be one smooth tube. */
static void render_elbow(struct pipes *p, const double at[3], const int in[3], const int out[3],
		const double color[3]) {
	double center[3];
	for (int i = 0; i < 3; i++) {
		center[i] = at[i] + (out[i] - in[i]) * BEND;
	}
	for (int k = 0; k <= 14; k++) {
		double angle = k * M_PI / 28;
		double c[3];
		for (int i = 0; i < 3; i++) {
			c[i] = center[i] - out[i] * BEND * cos(angle) + in[i] * BEND * sin(angle);
		}
		render_sphere(p, c, RADIUS, color);
	}
}

/* The teapot that hid in the joints of the Windows pipes. */
static void render_teapot(struct pipes *p, const double at[3], const double color[3]) {
	struct shape body = { .kind = SHAPE_ELLIPSOID, .c = { at[0], at[1], at[2] },
		.r = { 0.42, 0.3, 0.42 } };
	render(p, &body, color);
	double lid[3] = { at[0], at[1] + 0.3, at[2] };
	render_sphere(p, lid, 0.07, color);
	struct shape top = { .kind = SHAPE_ELLIPSOID, .c = { at[0], at[1] + 0.22, at[2] },
		.r = { 0.24, 0.09, 0.24 } };
	render(p, &top, color);
	for (int k = 0; k <= 10; k++) {
		// the spout rises to the right, the handle curls on the left
		double t = k / 10.0;
		double spout[3] = { at[0] + 0.38 + t * 0.3, at[1] - 0.05 + t * t * 0.3, at[2] };
		render_sphere(p, spout, 0.07 - t * 0.02, color);
		double a = -M_PI / 2 + t * M_PI;
		double handle[3] = { at[0] - 0.4 - cos(a) * 0.18, at[1] + sin(a) * 0.17, at[2] };
		render_sphere(p, handle, 0.045, color);
	}
}

/* ---------- growing ---------- */

static unsigned char *cell(struct pipes *p, int x, int y, int z) {
	return &p->taken[(z * GRID_Y + y) * p->gx + x];
}

static bool free_cell(struct pipes *p, int x, int y, int z) {
	return x >= 0 && x < p->gx && y >= 0 && y < GRID_Y && z >= 0 && z < GRID_Z &&
		!*cell(p, x, y, z);
}

static void clear(struct pipes *p) {
	memset(p->taken, 0, p->gx * GRID_Y * GRID_Z);
	for (int i = 0; i < p->width * p->height; i++) {
		p->depth[i] = FLT_MAX;
	}
	cairo_t *cr = cairo_create(p->canvas);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	memset(p->pipe, 0, sizeof(p->pipe));
	p->pieces = 0;
}

static void grid_point(struct pipes *p, int x, int y, int z, double out[3]) {
	world(p, x, y, z, out);
}

/* The world direction of a grid direction: y of the grid points down. */
static void world_dir(int dir, int out[3]) {
	out[0] = dirs[dir][0];
	out[1] = -dirs[dir][1];
	out[2] = dirs[dir][2];
}

static bool start_pipe(struct pipes *p, struct pipe *pipe) {
	for (int tries = 0; tries < 60; tries++) {
		int x = (int)(saver_random() * p->gx), y = (int)(saver_random() * GRID_Y);
		int z = (int)(saver_random() * GRID_Z);
		if (!free_cell(p, x, y, z)) {
			continue;
		}
		*cell(p, x, y, z) = 1;
		*pipe = (struct pipe){ .alive = true, .x = x, .y = y, .z = z,
			.dir = (int)(saver_random() * 6), .color = p->color_next, .first = true };
		p->color_next = (p->color_next + 1 + (int)(saver_random() * 5)) %
			(int)(sizeof(colors) / sizeof(colors[0]));
		return true;
	}
	return false;
}

/* The point a run is drawn from: its start, moved on by what is already drawn. */
static void run_point(struct pipes *p, struct pipe *pipe, double back, double out[3]) {
	int w[3];
	world_dir(pipe->dir, w);
	grid_point(p, pipe->x, pipe->y, pipe->z, out);
	for (int i = 0; i < 3; i++) {
		out[i] -= w[i] * back;
	}
}

/*
 * One piece of a pipe. The last stretch before the head is left undrawn until
 * the next piece says whether it goes straight on or turns, so an elbow can
 * take its place.
 */
static void grow(struct pipes *p, struct pipe *pipe) {
	const double *color = colors[pipe->color];
	const int *d = dirs[pipe->dir];
	int dir = pipe->dir;
	bool ahead = free_cell(p, pipe->x + d[0], pipe->y + d[1], pipe->z + d[2]);
	bool starting = pipe->first && pipe->drawn == 0; // a pipe starts with a straight piece
	if ((saver_random() < 0.25 && !starting) || !ahead) {
		int options[6], count = 0;
		for (int i = 0; i < 6; i++) {
			if (i != pipe->dir && i != (pipe->dir ^ 1) &&
					free_cell(p, pipe->x + dirs[i][0], pipe->y + dirs[i][1], pipe->z + dirs[i][2])) {
				options[count++] = i;
			}
		}
		if (count) {
			dir = options[(int)(saver_random() * count)];
		} else if (!ahead) {
			// stuck: the run is finished up to the head and closed
			double from[3], to[3];
			run_point(p, pipe, pipe->drawn, from);
			grid_point(p, pipe->x, pipe->y, pipe->z, to);
			render_run(p, from, to, pipe->first && pipe->drawn == 0, true, color);
			pipe->alive = false;
			return;
		}
	}
	double head[3];
	grid_point(p, pipe->x, pipe->y, pipe->z, head);
	if (dir != pipe->dir) {
		int in[3], out[3];
		world_dir(pipe->dir, in);
		world_dir(dir, out);
		double r = saver_random();
		bool ball = r < p->ball, teapot = r > 1 - p->teapot;
		double from[3], to[3];
		run_point(p, pipe, pipe->drawn, from);
		for (int i = 0; i < 3; i++) {
			to[i] = head[i] - (ball || teapot ? 0 : in[i] * BEND);
		}
		render_run(p, from, to, pipe->first && pipe->drawn == 0, false, color);
		if (teapot) {
			render_teapot(p, head, color);
		} else if (ball) {
			render_sphere(p, head, BALL, color);
		} else {
			render_elbow(p, head, in, out, color);
		}
		pipe->dir = dir;
		pipe->first = false;
		pipe->drawn = ball || teapot ? 0 : -BEND; // the new run starts after the bend
	}
	// one cell on, drawn up to where a bend could begin
	d = dirs[pipe->dir];
	pipe->x += d[0];
	pipe->y += d[1];
	pipe->z += d[2];
	*cell(p, pipe->x, pipe->y, pipe->z) = 1;
	double from[3], to[3];
	int w[3];
	world_dir(pipe->dir, w);
	run_point(p, pipe, 1 + pipe->drawn, from);
	grid_point(p, pipe->x, pipe->y, pipe->z, to);
	for (int i = 0; i < 3; i++) {
		to[i] -= w[i] * BEND;
	}
	render_run(p, from, to, pipe->first && pipe->drawn == 0, false, color);
	pipe->drawn = BEND;
	p->pieces++;
}

static void *pipes_create(int width, int height, const struct saver_options *options) {
	struct pipes *p = calloc(1, sizeof(*p));
	p->width = width;
	p->height = height;
	p->gx = (int)saver_clamp(round(GRID_Y * (double)width / height), 4, GRID_X_MAX);
	p->taken = calloc(p->gx * GRID_Y * GRID_Z, 1);
	p->depth = malloc(sizeof(float) * width * height);
	p->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	// the front of the box fills the screen, its back is a good deal smaller
	p->eye = GRID_Z * 1.35;
	p->focal = fmin(width / (double)p->gx, height / (double)GRID_Y) * p->eye * 0.98;
	p->color_next = (int)(saver_random() * 7);
	static const double balls[] = { 0.14, 0, 1 }, teapots[] = { 0.0035, 0.12 };
	static const int counts[] = { 2, 1, 3 };
	int joints = saver_choice(options, &saver_pipes, "joints");
	p->ball = balls[joints];
	p->teapot = teapots[saver_choice(options, &saver_pipes, "teapot")];
	if (joints == 2) {
		p->ball = 1 - p->teapot; // all balls, but for the teapots
	}
	p->pipes = counts[saver_choice(options, &saver_pipes, "pipes")];
	clear(p);
	return p;
}

static void pipes_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct pipes *p = state;
	if (p->fade > 0) {
		// full: black for a moment, then from the start
		p->fade -= dt;
		if (p->fade <= 0) {
			clear(p);
		}
		return;
	}
	p->since += dt;
	int steps = 0;
	while (p->since >= STEP && steps++ < 6) {
		p->since -= STEP;
		int alive = 0;
		for (int i = 0; i < p->pipes; i++) {
			struct pipe *pipe = &p->pipe[i];
			if (!pipe->alive && !start_pipe(p, pipe)) {
				continue;
			}
			grow(p, pipe);
			alive += pipe->alive;
		}
		if (p->pieces > p->gx * GRID_Y * GRID_Z / 3 || (!alive && p->pieces > 10)) {
			p->fade = 0.6;
			break;
		}
	}
	cairo_set_source_surface(cr, p->canvas, 0, 0);
	cairo_paint(cr);
}

static void pipes_destroy(void *state) {
	struct pipes *p = state;
	cairo_surface_destroy(p->canvas);
	free(p->depth);
	free(p->taken);
	free(p);
}

static const char *const joints_values[] = { "mixed", "elbows", "balls", NULL };
static const char *const joints_labels[] = { "Mixed", "Elbows", "Ball joints", NULL };
static const char *const pipes_values[] = { "two", "one", "three", NULL };
static const char *const pipes_labels[] = { "Two", "One", "Three", NULL };
static const char *const teapot_values[] = { "rarely", "often", NULL };
static const char *const teapot_labels[] = { "Very rarely", "Often", NULL };
static const struct saver_option pipes_options[] = {
	{ "joints", "Joint type", NULL, SAVER_CHOICE, joints_values, joints_labels, false },
	{ "pipes", "Pipes at once", NULL, SAVER_CHOICE, pipes_values, pipes_labels, false },
	{ "teapot", "Teapots", "In place of a joint, as in the Windows pipes", SAVER_CHOICE,
		teapot_values, teapot_labels, false },
	{ 0 },
};

const struct saver saver_pipes = {
	.name = "pipes",
	.title = "3D Pipes",
	.description = "Shiny pipes growing through space, turning in elbows and ball joints "
		"(and once in a while a teapot), as in Windows 95 to XP",
	.create = pipes_create,
	.draw = pipes_draw,
	.options = pipes_options,
	.destroy = pipes_destroy,
};
