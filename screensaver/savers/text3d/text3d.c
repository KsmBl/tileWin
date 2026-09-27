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

/* The saver, here: its settings are read with it. */
extern const struct saver saver_text3d;

/* 3D Text. */

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

TILEWIN_SAVER(.saver = &saver_text3d, .order = 50, .shot_seconds = 3);
