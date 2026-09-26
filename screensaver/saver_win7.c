#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "saver_util.h"

/*
 * The screen savers of Windows 7: Blank, Bubbles, Mystify, Ribbons, 3D Text
 * and Photos.
 */

/* ================= Blank ================= */

static void *blank_create(int width, int height, const struct saver_options *options) {
	return calloc(1, 1);
}

static void blank_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	// black, which saver_run_draw has painted already
}

const struct saver saver_blank = {
	.name = "blank",
	.title = "Blank",
	.description = "A black screen",
	.create = blank_create,
	.draw = blank_draw,
	.destroy = free,
};

/* ================= Bubbles ================= */

#define BUBBLES_MAX 32

struct bubble {
	double x, y, vx, vy, r, hue, spin;
};

static const char *const amount_values[] = { "normal", "few", "many", NULL };
static const char *const amount_labels[] = { "Normal", "Few", "Many", NULL };
static const char *const size_values[] = { "normal", "small", "large", NULL };
static const char *const size_labels[] = { "Normal", "Small", "Large", NULL };

struct bubbles {
	int count;
	struct bubble b[BUBBLES_MAX];
};

static void *bubbles_create(int width, int height, const struct saver_options *options) {
	struct bubbles *s = calloc(1, sizeof(*s));
	double u = saver_unit(width, height);
	static const double amounts[] = { 1, 0.5, 1.8 }, sizes[] = { 1, 0.65, 1.4 };
	double amount = amounts[saver_choice(options, &saver_bubbles, "count")];
	double size = sizes[saver_choice(options, &saver_bubbles, "size")];
	s->count = (int)saver_clamp(width * (double)height / (1920.0 * 1080.0) * 12 * amount, 3,
		BUBBLES_MAX);
	for (int i = 0; i < s->count; i++) {
		struct bubble *b = &s->b[i];
		b->r = u * saver_between(70, 150) * size;
		// a place of its own, so they do not start inside each other
		for (int tries = 0; tries < 50; tries++) {
			b->x = saver_between(b->r, width - b->r);
			b->y = saver_between(b->r, height - b->r);
			bool free_spot = true;
			for (int j = 0; j < i && free_spot; j++) {
				free_spot = hypot(b->x - s->b[j].x, b->y - s->b[j].y) > b->r + s->b[j].r;
			}
			if (free_spot) {
				break;
			}
		}
		double angle = saver_between(0, 2 * M_PI), speed = u * saver_between(60, 150);
		b->vx = cos(angle) * speed;
		b->vy = sin(angle) * speed;
		b->hue = saver_random();
		b->spin = saver_between(0, 2 * M_PI);
	}
	return s;
}

static void bubbles_move(struct bubbles *s, int width, int height, double dt) {
	for (int i = 0; i < s->count; i++) {
		struct bubble *b = &s->b[i];
		b->x += b->vx * dt;
		b->y += b->vy * dt;
		b->hue += dt * 0.03;
		b->spin += dt * 0.5;
		if ((b->x < b->r && b->vx < 0) || (b->x > width - b->r && b->vx > 0)) {
			b->vx = -b->vx;
		}
		if ((b->y < b->r && b->vy < 0) || (b->y > height - b->r && b->vy > 0)) {
			b->vy = -b->vy;
		}
	}
	// they bounce off each other like balls, the bigger ones heavier
	for (int i = 0; i < s->count; i++) {
		for (int j = i + 1; j < s->count; j++) {
			struct bubble *a = &s->b[i], *b = &s->b[j];
			double dx = b->x - a->x, dy = b->y - a->y, d = hypot(dx, dy);
			if (d <= 0 || d >= a->r + b->r) {
				continue;
			}
			double nx = dx / d, ny = dy / d;
			double approach = (a->vx - b->vx) * nx + (a->vy - b->vy) * ny;
			double ma = a->r * a->r, mb = b->r * b->r;
			if (approach > 0) {
				double impulse = 2 * approach / (ma + mb);
				a->vx -= impulse * mb * nx;
				a->vy -= impulse * mb * ny;
				b->vx += impulse * ma * nx;
				b->vy += impulse * ma * ny;
			}
			double overlap = (a->r + b->r - d) / 2;
			a->x -= nx * overlap;
			a->y -= ny * overlap;
			b->x += nx * overlap;
			b->y += ny * overlap;
		}
	}
}

static void bubble_draw(cairo_t *cr, const struct bubble *b) {
	double r = b->r, x = b->x, y = b->y;
	double tr, tg, tb;
	saver_hsv(b->hue, 0.45, 1, &tr, &tg, &tb);
	// the thin film: clear inside, tinted and bright towards the edge
	cairo_pattern_t *body = cairo_pattern_create_radial(x - r * 0.2, y - r * 0.25, 0, x, y, r);
	cairo_pattern_add_color_stop_rgba(body, 0, 1, 1, 1, 0.03);
	cairo_pattern_add_color_stop_rgba(body, 0.72, tr, tg, tb, 0.05);
	cairo_pattern_add_color_stop_rgba(body, 0.92, tr, tg, tb, 0.2);
	cairo_pattern_add_color_stop_rgba(body, 1, 1, 1, 1, 0.42);
	cairo_arc(cr, x, y, r, 0, 2 * M_PI);
	cairo_set_source(cr, body);
	cairo_fill(cr);
	cairo_pattern_destroy(body);
	// the rainbow sheen along the rim, turning slowly
	double cx = cos(b->spin) * r, cy = sin(b->spin) * r;
	cairo_pattern_t *sheen = cairo_pattern_create_linear(x - cx, y - cy, x + cx, y + cy);
	for (int i = 0; i <= 6; i++) {
		double sr, sg, sb;
		saver_hsv(b->hue + i / 6.0, 0.8, 1, &sr, &sg, &sb);
		cairo_pattern_add_color_stop_rgba(sheen, i / 6.0, sr, sg, sb, 0.34);
	}
	cairo_arc(cr, x, y, r * 0.955, 0, 2 * M_PI);
	cairo_set_line_width(cr, r * 0.07);
	cairo_set_source(cr, sheen);
	cairo_stroke(cr);
	cairo_pattern_destroy(sheen);
	// the window it reflects, up at the left, and a small glint below
	cairo_save(cr);
	cairo_translate(cr, x - r * 0.38, y - r * 0.42);
	cairo_rotate(cr, -0.6);
	cairo_scale(cr, 1, 0.55);
	cairo_pattern_t *glare = cairo_pattern_create_radial(0, 0, 0, 0, 0, r * 0.3);
	cairo_pattern_add_color_stop_rgba(glare, 0, 1, 1, 1, 0.85);
	cairo_pattern_add_color_stop_rgba(glare, 1, 1, 1, 1, 0);
	cairo_arc(cr, 0, 0, r * 0.3, 0, 2 * M_PI);
	cairo_set_source(cr, glare);
	cairo_fill(cr);
	cairo_pattern_destroy(glare);
	cairo_restore(cr);
	cairo_pattern_t *glint = cairo_pattern_create_radial(x + r * 0.45, y + r * 0.52, 0,
		x + r * 0.45, y + r * 0.52, r * 0.12);
	cairo_pattern_add_color_stop_rgba(glint, 0, 1, 1, 1, 0.4);
	cairo_pattern_add_color_stop_rgba(glint, 1, 1, 1, 1, 0);
	cairo_arc(cr, x + r * 0.45, y + r * 0.52, r * 0.12, 0, 2 * M_PI);
	cairo_set_source(cr, glint);
	cairo_fill(cr);
	cairo_pattern_destroy(glint);
}

static void bubbles_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct bubbles *s = state;
	bubbles_move(s, width, height, dt);
	for (int i = 0; i < s->count; i++) {
		bubble_draw(cr, &s->b[i]);
	}
}

static const struct saver_option bubbles_options[] = {
	{ "count", "Bubbles", NULL, SAVER_CHOICE, amount_values, amount_labels, false },
	{ "size", "Size", NULL, SAVER_CHOICE, size_values, size_labels, false },
	{ 0 },
};

const struct saver saver_bubbles = {
	.name = "bubbles",
	.title = "Bubbles",
	.description = "Soap bubbles drifting over the desktop and bouncing off each other",
	.transparent = true,
	.create = bubbles_create,
	.draw = bubbles_draw,
	.options = bubbles_options,
	.destroy = free,
};

/* ================= Mystify ================= */

#define MYSTIFY_CORNERS 4
#define MYSTIFY_ECHOES 28 // at most: the setting says how many

struct shape {
	double x[MYSTIFY_CORNERS], y[MYSTIFY_CORNERS], vx[MYSTIFY_CORNERS], vy[MYSTIFY_CORNERS];
	double ex[MYSTIFY_ECHOES][MYSTIFY_CORNERS], ey[MYSTIFY_ECHOES][MYSTIFY_CORNERS];
	int echoes, head;
	double hue;
};

struct mystify {
	struct shape shapes[2];
	int shape_count, trail;
	double since_echo;
	double u;
};

static void *mystify_create(int width, int height, const struct saver_options *options) {
	struct mystify *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	static const int trails[] = { 14, 5, 28 };
	s->shape_count = saver_choice(options, &saver_mystify, "shapes") == 1 ? 1 : 2;
	s->trail = trails[saver_choice(options, &saver_mystify, "trail")];
	for (int k = 0; k < s->shape_count; k++) {
		struct shape *p = &s->shapes[k];
		p->hue = k * 0.5 + saver_random() * 0.2;
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			p->x[i] = saver_between(0, width);
			p->y[i] = saver_between(0, height);
			double angle = saver_between(0, 2 * M_PI), speed = s->u * saver_between(160, 380);
			p->vx[i] = cos(angle) * speed;
			p->vy[i] = sin(angle) * speed;
		}
	}
	return s;
}

static void mystify_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct mystify *s = state;
	s->since_echo += dt;
	bool echo = s->since_echo >= 0.045;
	if (echo) {
		s->since_echo = 0;
	}
	for (int k = 0; k < s->shape_count; k++) {
		struct shape *p = &s->shapes[k];
		p->hue += dt * 0.04;
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			p->x[i] += p->vx[i] * dt;
			p->y[i] += p->vy[i] * dt;
			if ((p->x[i] < 0 && p->vx[i] < 0) || (p->x[i] > width && p->vx[i] > 0)) {
				p->vx[i] = -p->vx[i];
			}
			if ((p->y[i] < 0 && p->vy[i] < 0) || (p->y[i] > height && p->vy[i] > 0)) {
				p->vy[i] = -p->vy[i];
			}
		}
		if (echo) {
			memcpy(p->ex[p->head], p->x, sizeof(p->x));
			memcpy(p->ey[p->head], p->y, sizeof(p->y));
			p->head = (p->head + 1) % MYSTIFY_ECHOES;
			p->echoes = p->echoes < s->trail ? p->echoes + 1 : s->trail;
		}
		cairo_set_line_width(cr, fmax(1.2, 1.6 * s->u));
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		// the echoes fade behind the shape, oldest first
		for (int e = 0; e < p->echoes; e++) {
			int slot = (p->head - p->echoes + e + MYSTIFY_ECHOES) % MYSTIFY_ECHOES;
			for (int i = 0; i < MYSTIFY_CORNERS; i++) {
				if (i == 0) {
					cairo_move_to(cr, p->ex[slot][i], p->ey[slot][i]);
				} else {
					cairo_line_to(cr, p->ex[slot][i], p->ey[slot][i]);
				}
			}
			cairo_close_path(cr);
			saver_set_hsva(cr, p->hue, 1, 1, 0.15 + 0.6 * (e + 1) / s->trail);
			cairo_stroke(cr);
		}
		for (int i = 0; i < MYSTIFY_CORNERS; i++) {
			if (i == 0) {
				cairo_move_to(cr, p->x[i], p->y[i]);
			} else {
				cairo_line_to(cr, p->x[i], p->y[i]);
			}
		}
		cairo_close_path(cr);
		saver_set_hsva(cr, p->hue, 0.9, 1, 1);
		cairo_stroke(cr);
	}
}

static const char *const shapes_values[] = { "two", "one", NULL };
static const char *const shapes_labels[] = { "Two", "One", NULL };
static const char *const trail_values[] = { "normal", "short", "long", NULL };
static const char *const trail_labels[] = { "Normal", "Short", "Long", NULL };
static const struct saver_option mystify_options[] = {
	{ "shapes", "Shapes", NULL, SAVER_CHOICE, shapes_values, shapes_labels, false },
	{ "trail", "Trail", "How many echoes follow each shape", SAVER_CHOICE, trail_values,
		trail_labels, false },
	{ 0 },
};

const struct saver saver_mystify = {
	.name = "mystify",
	.title = "Mystify",
	.description = "Two shapes of lines that bounce around and leave echoes of color",
	.create = mystify_create,
	.draw = mystify_draw,
	.options = mystify_options,
	.destroy = free,
};

/* ================= Ribbons ================= */

#define RIBBONS 5
#define RIBBON_POINTS 150

struct ribbon {
	double x[RIBBON_POINTS], y[RIBBON_POINTS], z[RIBBON_POINTS], twist[RIBBON_POINTS];
	int head, count;
	double f[6], p[6]; // the frequencies and phases of its path
	double hue;
};

struct ribbons {
	struct ribbon r[RIBBONS];
	int count;
	double t, since_point, u;
};

static void *ribbons_create(int width, int height, const struct saver_options *options) {
	struct ribbons *s = calloc(1, sizeof(*s));
	s->u = saver_unit(width, height);
	static const int counts[] = { RIBBONS, 3, 1 };
	s->count = counts[saver_choice(options, &saver_ribbons, "ribbons")];
	for (int i = 0; i < s->count; i++) {
		struct ribbon *r = &s->r[i];
		for (int k = 0; k < 6; k++) {
			r->f[k] = saver_between(0.35, 0.9) * (k % 2 ? 1 : -1);
			r->p[k] = saver_between(0, 2 * M_PI);
		}
		r->hue = i / (double)RIBBONS + saver_random() * 0.1;
	}
	return s;
}

static void ribbon_head(struct ribbon *r, double t, double *x, double *y, double *z,
		double *twist) {
	*x = sin(t * r->f[0] + r->p[0]) * 0.75 + sin(t * r->f[1] * 2.3 + r->p[1]) * 0.25;
	*y = sin(t * r->f[2] + r->p[2]) * 0.7 + sin(t * r->f[3] * 1.9 + r->p[3]) * 0.3;
	*z = sin(t * r->f[4] + r->p[4]);
	*twist = t * (1 + r->f[5]) + r->p[5];
}

static void ribbons_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct ribbons *s = state;
	s->t += dt;
	s->since_point += dt;
	bool add = s->since_point >= 1 / 30.0;
	if (add) {
		s->since_point = 0;
	}
	for (int i = 0; i < s->count; i++) {
		struct ribbon *r = &s->r[i];
		r->hue += dt * 0.02;
		if (add || r->count == 0) {
			ribbon_head(r, s->t, &r->x[r->head], &r->y[r->head], &r->z[r->head],
				&r->twist[r->head]);
			r->head = (r->head + 1) % RIBBON_POINTS;
			r->count = r->count < RIBBON_POINTS ? r->count + 1 : RIBBON_POINTS;
		}
		double px[RIBBON_POINTS], py[RIBBON_POINTS], half[RIBBON_POINTS];
		for (int k = 0; k < r->count; k++) {
			int slot = (r->head - r->count + k + RIBBON_POINTS) % RIBBON_POINTS;
			double persp = 3 / (3 + r->z[slot] + 1);
			px[k] = width / 2.0 + r->x[slot] * width * 0.48 * persp;
			py[k] = height / 2.0 + r->y[slot] * height * 0.48 * persp;
			half[k] = s->u * 46 * persp * cos(r->twist[slot]);
		}
		for (int k = 0; k + 1 < r->count; k++) {
			double dx = px[k + 1] - px[k], dy = py[k + 1] - py[k];
			double len = hypot(dx, dy);
			if (len < 0.01) {
				continue;
			}
			// across the path; how wide it looks is how far it is turned towards us
			double nx = -dy / len, ny = dx / len;
			cairo_move_to(cr, px[k] + nx * half[k], py[k] + ny * half[k]);
			cairo_line_to(cr, px[k + 1] + nx * half[k + 1], py[k + 1] + ny * half[k + 1]);
			cairo_line_to(cr, px[k + 1] - nx * half[k + 1], py[k + 1] - ny * half[k + 1]);
			cairo_line_to(cr, px[k] - nx * half[k], py[k] - ny * half[k]);
			cairo_close_path(cr);
			double age = (k + 1) / (double)r->count; // 1 at the head
			double facing = fabs(half[k]) / (s->u * 46 + 0.001);
			bool back = half[k] < 0;
			// lit where it faces us, its back a deeper color; the tail fades out
			saver_set_hsva(cr, r->hue + age * 0.08, back ? 0.95 : 0.6,
				0.3 + 0.7 * facing, 0.15 + 0.85 * age);
			cairo_fill_preserve(cr);
			cairo_set_line_width(cr, 0.8); // closes the hairline between the pieces
			cairo_stroke(cr);
		}
	}
}

static const char *const ribbons_values[] = { "five", "three", "one", NULL };
static const char *const ribbons_labels[] = { "Five", "Three", "One", NULL };
static const struct saver_option ribbons_options[] = {
	{ "ribbons", "Ribbons", NULL, SAVER_CHOICE, ribbons_values, ribbons_labels, false },
	{ 0 },
};

const struct saver saver_ribbons = {
	.name = "ribbons",
	.title = "Ribbons",
	.description = "Glowing ribbons that twist and weave through the dark",
	.create = ribbons_create,
	.draw = ribbons_draw,
	.options = ribbons_options,
	.destroy = free,
};

/* ================= 3D Text ================= */

struct text3d {
	double *x, *y;   // outline points, centered around 0
	int *start;      // where each closed outline begins, count + 1 entries
	int points, outlines;
	double text_w, text_h;
	char text[128];
	bool clock;
	int minute;
	double t, cx, cy, vx, vy, hue;
	int width, height;
};

struct quad {
	double z;
	int a, b; // points of the edge
};

static void text3d_outline(struct text3d *s, const char *text) {
	free(s->x);
	free(s->y);
	free(s->start);
	s->x = s->y = NULL;
	s->start = NULL;
	s->points = s->outlines = 0;
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(scratch);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *font = pango_font_description_from_string(
		"Segoe UI Black, Noto Sans Black, Sans Bold 200px");
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_text(layout, text && *text ? text : "tileWin", -1);
	PangoRectangle ink;
	pango_layout_get_pixel_extents(layout, &ink, NULL);
	cairo_set_tolerance(cr, 1.5);
	pango_cairo_layout_path(cr, layout);
	cairo_path_t *path = cairo_copy_path_flat(cr);
	int capacity = 256, starts = 16;
	s->x = malloc(capacity * sizeof(double));
	s->y = malloc(capacity * sizeof(double));
	s->start = malloc(starts * sizeof(int));
	double ox = ink.x + ink.width / 2.0, oy = ink.y + ink.height / 2.0;
	for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
		cairo_path_data_t *d = &path->data[i];
		if (d->header.type == CAIRO_PATH_MOVE_TO) {
			if (s->outlines + 2 > starts) {
				starts *= 2;
				s->start = realloc(s->start, starts * sizeof(int));
			}
			s->start[s->outlines++] = s->points;
		}
		if (d->header.type == CAIRO_PATH_MOVE_TO || d->header.type == CAIRO_PATH_LINE_TO) {
			if (s->points + 1 > capacity) {
				capacity *= 2;
				s->x = realloc(s->x, capacity * sizeof(double));
				s->y = realloc(s->y, capacity * sizeof(double));
			}
			s->x[s->points] = d[1].point.x - ox;
			s->y[s->points] = d[1].point.y - oy;
			s->points++;
		}
	}
	s->start[s->outlines] = s->points;
	s->text_w = ink.width > 0 ? ink.width : 1;
	s->text_h = ink.height > 0 ? ink.height : 1;
	cairo_path_destroy(path);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
}

static void text3d_update_text(struct text3d *s) {
	if (!s->clock) {
		if (!s->points) {
			text3d_outline(s, s->text);
		}
		return;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (tm.tm_min != s->minute || !s->points) {
		s->minute = tm.tm_min;
		char clock[16];
		strftime(clock, sizeof(clock), "%H:%M", &tm);
		text3d_outline(s, clock);
	}
}

static void *text3d_create(int width, int height, const struct saver_options *options) {
	struct text3d *s = calloc(1, sizeof(*s));
	const char *text = options->text && *options->text ? options->text : "tileWin";
	s->clock = strcasecmp(text, "time") == 0;
	snprintf(s->text, sizeof(s->text), "%s", text);
	s->minute = -1;
	s->cx = width / 2.0;
	s->cy = height / 2.0;
	double u = saver_unit(width, height);
	s->vx = u * 45;
	s->vy = u * 30;
	s->hue = saver_random();
	return s;
}

static int quad_cmp(const void *a, const void *b) {
	double za = ((const struct quad *)a)->z, zb = ((const struct quad *)b)->z;
	return za < zb ? 1 : za > zb ? -1 : 0; // the farthest first
}

static void text3d_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct text3d *s = state;
	text3d_update_text(s);
	if (s->points < 2) {
		return;
	}
	s->t += dt;
	s->hue += dt * 0.015;
	// as big as fits, turning to and fro and drifting about
	double k = fmin(width * 0.62 / s->text_w, height * 0.34 / s->text_h);
	double yaw = sin(s->t * 0.55) * 0.8, pitch = sin(s->t * 0.41) * 0.3;
	double roll = sin(s->t * 0.27) * 0.08;
	double half_w = s->text_w * k * 0.5, half_h = s->text_h * k * 0.6;
	s->cx += s->vx * dt;
	s->cy += s->vy * dt;
	if ((s->cx < half_w && s->vx < 0) || (s->cx > width - half_w && s->vx > 0)) {
		s->vx = -s->vx;
	}
	if ((s->cy < half_h && s->vy < 0) || (s->cy > height - half_h && s->vy > 0)) {
		s->vy = -s->vy;
	}
	s->cx = saver_clamp(s->cx, fmin(half_w, width / 2.0), fmax(width - half_w, width / 2.0));
	s->cy = saver_clamp(s->cy, fmin(half_h, height / 2.0), fmax(height - half_h, height / 2.0));

	double depth = s->text_h * 0.3, camera = s->text_w * 1.8;
	double cyaw = cos(yaw), syaw = sin(yaw), cp = cos(pitch), sp = sin(pitch);
	double cr_ = cos(roll), sr = sin(roll);
	int n = s->points;
	double *sx = malloc(4 * n * sizeof(double)); // front x, front y, back x, back y
	double *zf = malloc(2 * n * sizeof(double));
	for (int layer = 0; layer < 2; layer++) {
		double z0 = layer ? depth / 2 : -depth / 2;
		for (int i = 0; i < n; i++) {
			double x = s->x[i] * cr_ - s->y[i] * sr, y = s->x[i] * sr + s->y[i] * cr_, z = z0;
			double x1 = x * cyaw + z * syaw, z1 = -x * syaw + z * cyaw;
			double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
			double persp = camera / (camera + z2);
			sx[layer * 2 * n + i] = s->cx + x1 * persp * k;
			sx[layer * 2 * n + n + i] = s->cy + y2 * persp * k;
			zf[layer * n + i] = z2;
		}
	}
	double *fx = sx, *fy = sx + n, *bx = sx + 2 * n, *by = sx + 3 * n;
	// the sides, far ones first, lit from the upper left
	struct quad *quads = malloc(n * sizeof(struct quad));
	int count = 0;
	for (int o = 0; o < s->outlines; o++) {
		for (int i = s->start[o]; i < s->start[o + 1]; i++) {
			int j = i + 1 < s->start[o + 1] ? i + 1 : s->start[o];
			quads[count++] = (struct quad){
				.z = (zf[i] + zf[j] + zf[n + i] + zf[n + j]) / 4, .a = i, .b = j,
			};
		}
	}
	qsort(quads, count, sizeof(*quads), quad_cmp);
	double br, bg, bb;
	saver_hsv(s->hue, 0.55, 1, &br, &bg, &bb);
	for (int q = 0; q < count; q++) {
		int a = quads[q].a, b = quads[q].b;
		double ex = s->x[b] - s->x[a], ey = s->y[b] - s->y[a];
		double len = hypot(ex, ey);
		if (len <= 0) {
			continue;
		}
		// the normal of the side, turned with the text
		double nx = ey / len, ny = -ex / len;
		double rx = nx * cr_ - ny * sr, ry = nx * sr + ny * cr_;
		double tx = rx * cyaw, tz = -rx * syaw;
		double ty = ry * cp - tz * sp;
		double light = fabs(tx * -0.45 + ty * -0.6 + 0.3);
		double shade = 0.2 + 0.65 * saver_clamp(light, 0, 1);
		cairo_move_to(cr, fx[a], fy[a]);
		cairo_line_to(cr, fx[b], fy[b]);
		cairo_line_to(cr, bx[b], by[b]);
		cairo_line_to(cr, bx[a], by[a]);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, br * shade, bg * shade, bb * shade);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 0.8);
		cairo_stroke(cr);
	}
	// the face, polished like chrome
	for (int o = 0; o < s->outlines; o++) {
		for (int i = s->start[o]; i < s->start[o + 1]; i++) {
			if (i == s->start[o]) {
				cairo_move_to(cr, fx[i], fy[i]);
			} else {
				cairo_line_to(cr, fx[i], fy[i]);
			}
		}
		cairo_close_path(cr);
	}
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	cairo_pattern_t *face = cairo_pattern_create_linear(0, s->cy - half_h, 0, s->cy + half_h);
	cairo_pattern_add_color_stop_rgb(face, 0, 1, 1, 1);
	cairo_pattern_add_color_stop_rgb(face, 0.45, br, bg, bb);
	cairo_pattern_add_color_stop_rgb(face, 0.55, br * 0.55, bg * 0.55, bb * 0.55);
	cairo_pattern_add_color_stop_rgb(face, 1, br * 0.9, bg * 0.9, bb * 0.9);
	cairo_set_source(cr, face);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(face);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	free(quads);
	free(sx);
	free(zf);
}

static void text3d_destroy(void *state) {
	struct text3d *s = state;
	free(s->x);
	free(s->y);
	free(s->start);
	free(s);
}

const struct saver saver_text3d = {
	.name = "text3d",
	.title = "3D Text",
	.description = "Words or the time in solid letters, turning slowly in space",
	.create = text3d_create,
	.draw = text3d_draw,
	.destroy = text3d_destroy,
};

/* ================= Photos ================= */

#define PHOTOS_MAX 2000
#define PHOTO_FADE 1.5

struct photo {
	cairo_surface_t *image;
	double z0, z1, x0, y0, x1, y1; // zoom and pan from start to end
};

struct photos {
	double t;
	char **files;
	int count, next;
	struct photo current, coming;
	double shown, seconds;
	bool coming_loaded;
	int width, height;
	char folder[512];
};

static bool is_picture(const char *name) {
	const char *dot = strrchr(name, '.');
	static const char *const types[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp",
		".tif", ".tiff", NULL };
	for (int i = 0; dot && types[i]; i++) {
		if (strcasecmp(dot, types[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void photos_scan(struct photos *s, const char *dir, int depth) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && s->count < PHOTOS_MAX) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char *path = g_build_filename(dir, e->d_name, NULL);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && depth < 4) {
			photos_scan(s, path, depth + 1);
		} else if (S_ISREG(st.st_mode) && is_picture(e->d_name)) {
			s->files = realloc(s->files, (s->count + 1) * sizeof(char *));
			s->files[s->count++] = path;
			path = NULL;
		}
		g_free(path);
	}
	if (d) {
		closedir(d);
	}
}

/* A picture as a cairo surface just big enough to cover the area as it zooms. */
static cairo_surface_t *photo_load(const char *path, int width, int height) {
	int iw = 0, ih = 0;
	if (!gdk_pixbuf_get_file_info(path, &iw, &ih) || iw <= 0 || ih <= 0) {
		return NULL;
	}
	double scale = fmax(width * 1.15 / iw, height * 1.15 / ih);
	GdkPixbuf *loaded = gdk_pixbuf_new_from_file_at_scale(path, (int)ceil(iw * scale),
		(int)ceil(ih * scale), FALSE, NULL);
	if (!loaded) {
		return NULL;
	}
	GdkPixbuf *pixbuf = gdk_pixbuf_apply_embedded_orientation(loaded);
	g_object_unref(loaded);
	int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
	int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
	const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *out = cairo_image_surface_get_data(image);
	int out_stride = cairo_image_surface_get_stride(image);
	for (int y = 0; y < h; y++) {
		const guchar *p = pixels + y * stride;
		uint32_t *o = (uint32_t *)(out + y * out_stride);
		for (int x = 0; x < w; x++, p += channels) {
			unsigned a = channels == 4 ? p[3] : 255;
			o[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(image);
	g_object_unref(pixbuf);
	return image;
}

static void photo_next(struct photos *s, struct photo *photo) {
	photo->image = NULL;
	for (int tries = 0; tries < 5 && !photo->image && s->count; tries++) {
		photo->image = photo_load(s->files[s->next], s->width, s->height);
		s->next = (s->next + 1) % s->count;
	}
	// a slow zoom in or out, and a drift towards a corner
	bool in = saver_random() < 0.5;
	photo->z0 = in ? 1.0 : 1.12;
	photo->z1 = in ? 1.12 : 1.0;
	photo->x0 = saver_between(0.35, 0.65);
	photo->y0 = saver_between(0.35, 0.65);
	photo->x1 = saver_between(0.3, 0.7);
	photo->y1 = saver_between(0.3, 0.7);
}

static void *photos_create(int width, int height, const struct saver_options *options) {
	struct photos *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->seconds = options->photo_seconds >= 2 ? options->photo_seconds : 8;
	if (options->photos && *options->photos) {
		snprintf(s->folder, sizeof(s->folder), "%s", options->photos);
	} else {
		const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
		snprintf(s->folder, sizeof(s->folder), "%s", pictures ? pictures : g_get_home_dir());
	}
	photos_scan(s, s->folder, 0);
	// in a new order every time
	for (int i = s->count - 1; i > 0; i--) {
		int j = (int)(saver_random() * (i + 1));
		char *swap = s->files[i];
		s->files[i] = s->files[j];
		s->files[j] = swap;
	}
	photo_next(s, &s->current);
	return s;
}

static void photo_paint(struct photos *s, cairo_t *cr, struct photo *photo, double progress,
		double alpha) {
	if (!photo->image) {
		return;
	}
	double iw = cairo_image_surface_get_width(photo->image);
	double ih = cairo_image_surface_get_height(photo->image);
	double zoom = photo->z0 + (photo->z1 - photo->z0) * progress;
	double scale = fmax(s->width / iw, s->height / ih) * zoom;
	double px = photo->x0 + (photo->x1 - photo->x0) * progress;
	double py = photo->y0 + (photo->y1 - photo->y0) * progress;
	double spare_x = iw * scale - s->width, spare_y = ih * scale - s->height;
	cairo_save(cr);
	cairo_translate(cr, -spare_x * px, -spare_y * py);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, photo->image, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

static void photos_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct photos *s = state;
	s->t += dt;
	if (!s->current.image) {
		char message[640];
		snprintf(message, sizeof(message), "Put pictures into %s to see them here", s->folder);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
			CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, fmax(10, saver_unit(width, height) * 26));
		cairo_text_extents_t ext;
		cairo_text_extents(cr, message, &ext);
		// drifting, so it does not burn into the screen either
		cairo_move_to(cr, (width - ext.width) / 2 + sin(s->t * 0.3) * width * 0.1,
			height / 2.0 + sin(s->t * 0.23) * height * 0.3);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		cairo_show_text(cr, message);
		return;
	}
	s->shown += dt;
	double life = s->seconds + PHOTO_FADE;
	if (!s->coming_loaded && s->shown > 0.5) {
		// loading takes a moment: in the quiet middle of a photo, not in the fade
		photo_next(s, &s->coming);
		s->coming_loaded = true;
	}
	photo_paint(s, cr, &s->current, saver_clamp(s->shown / life, 0, 1), 1);
	if (s->shown > s->seconds && s->coming_loaded) {
		double fade = saver_clamp((s->shown - s->seconds) / PHOTO_FADE, 0, 1);
		photo_paint(s, cr, &s->coming, fade * PHOTO_FADE / life, fade);
		if (fade >= 1) {
			if (s->current.image) {
				cairo_surface_destroy(s->current.image);
			}
			s->current = s->coming;
			s->coming.image = NULL;
			s->coming_loaded = false;
			s->shown = PHOTO_FADE;
		}
	}
}

static void photos_destroy(void *state) {
	struct photos *s = state;
	for (int i = 0; i < s->count; i++) {
		g_free(s->files[i]);
	}
	free(s->files);
	if (s->current.image) {
		cairo_surface_destroy(s->current.image);
	}
	if (s->coming.image) {
		cairo_surface_destroy(s->coming.image);
	}
	free(s);
}

const struct saver saver_photos = {
	.name = "photos",
	.title = "Photos",
	.description = "A slideshow of your pictures, slowly zooming and fading into each other",
	.create = photos_create,
	.draw = photos_draw,
	.destroy = photos_destroy,
};
