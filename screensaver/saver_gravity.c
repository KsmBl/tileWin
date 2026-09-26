#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Gravity: a screen saver for privacy. A moment after it starts, the windows
 * come loose, one after another from the top of the stack: each hangs for a
 * breath from one corner of its title bar, swinging, then drops, tumbling,
 * and falls behind the taskbar and off the screen. What is left is the empty
 * desktop, the wallpaper where the windows were, and nothing on it to read.
 * The taskbar can go after them.
 *
 * The windows are the real ones, cut from the picture of the screen taken
 * before the saver started. Where one lay under another, what was hidden is
 * filled with the colour of the window around it, so no window carries a
 * piece of another away with it.
 */

#define WINDOWS_MAX 48
#define HANG_TIME 0.9
#define DUST_MAX 120

enum pane_state {
	PANE_REST,
	PANE_HANGING, // from one top corner, swinging
	PANE_FALLING,
	PANE_GONE,
};

struct pane {
	cairo_surface_t *img;
	double x, y, w, h;   // where it was
	double cx, cy;       // its middle, falling
	double vx, vy, angle, spin;
	double start;        // when it comes loose
	double t;            // in its state
	int hinge;           // the corner it hangs from: -1 left, 1 right
	enum pane_state state;
};

struct dust {
	float x, y, vx, vy, life, age;
};

struct gravity {
	int width, height;
	double u, t, gravity;
	cairo_surface_t *empty;   // the desktop with no windows on it
	cairo_surface_t *desktop; // as it was, for the taskbar in front of the falling
	struct saver_window wins[WINDOWS_MAX], bars[4];
	int count, bar_count;
	struct pane panes[WINDOWS_MAX];
	struct pane bar_panes[4];
	bool bar_falls;
	struct dust dust[DUST_MAX];
	int dust_next;
};

/* ---------- the pieces ---------- */

/* The average colour of what shows of a window, for what another one hid of it. */
static void window_colour(struct gravity *s, int i, double rgb[3]) {
	cairo_surface_flush(s->desktop);
	const uint32_t *px = (const uint32_t *)cairo_image_surface_get_data(s->desktop);
	int stride = cairo_image_surface_get_stride(s->desktop) / 4;
	struct saver_window *b = &s->wins[i];
	double sum[3] = { 0 };
	int n = 0;
	int step = (int)fmax(4, fmin(b->w, b->h) / 24);
	for (int y = (int)(b->y + b->title_h + 2); y < b->y + b->h - 2; y += step) {
		for (int x = (int)b->x + 2; x < b->x + b->w - 2; x += step) {
			if (x < 0 || y < 0 || x >= s->width || y >= s->height) {
				continue;
			}
			bool hidden = false;
			for (int j = i + 1; j < s->count && !hidden; j++) {
				struct saver_window *o = &s->wins[j];
				hidden = x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h;
			}
			if (hidden) {
				continue;
			}
			uint32_t p = px[y * stride + x];
			sum[0] += (p >> 16) & 255;
			sum[1] += (p >> 8) & 255;
			sum[2] += p & 255;
			n++;
		}
	}
	for (int k = 0; k < 3; k++) {
		rgb[k] = n ? sum[k] / n / 255.0 : 0.9;
	}
}

/* A window cut from the picture, what lay over it filled with its own colour. */
static cairo_surface_t *cut_window(struct gravity *s, int i) {
	struct saver_window *b = &s->wins[i];
	int w = (int)ceil(b->w), h = (int)ceil(b->h);
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w > 0 ? w : 1,
		h > 0 ? h : 1);
	cairo_t *cr = cairo_create(img);
	cairo_set_source_surface(cr, s->desktop, -b->x, -b->y);
	cairo_paint(cr);
	double rgb[3];
	window_colour(s, i, rgb);
	for (int j = i + 1; j < s->count; j++) {
		struct saver_window *o = &s->wins[j];
		cairo_rectangle(cr, o->x - b->x, o->y - b->y, o->w, o->h);
	}
	// a title bar stays a title bar: cover only below it
	cairo_rectangle(cr, 0, b->title_h, w, h - b->title_h);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	cairo_clip(cr);
	cairo_new_path(cr);
	for (int j = i + 1; j < s->count; j++) {
		struct saver_window *o = &s->wins[j];
		cairo_rectangle(cr, o->x - b->x, o->y - b->y, o->w, o->h);
	}
	cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
	cairo_fill(cr);
	cairo_destroy(cr);
	return img;
}

/* How far the soft shadow a theme draws around a window reaches. */
static double shadow_margin(struct gravity *s) {
	return fmax(8, s->u * 14);
}

/* Whether a point is on no window and no taskbar: desktop to be seen. */
static bool bare(struct gravity *s, int x, int y) {
	for (int i = 0; i < s->count; i++) {
		struct saver_window *b = &s->wins[i];
		double m = shadow_margin(s);
		if (x >= b->x - m && x < b->x + b->w + m && y >= b->y - m && y < b->y + b->h + m) {
			return false;
		}
	}
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			return false;
		}
	}
	return true;
}

/*
 * Whether a wallpaper is the one on the screen: compared with the picture where
 * the desktop shows. Another program may draw its own over tileWin's (swaybg).
 */
static bool wallpaper_matches(struct gravity *s, cairo_surface_t *wall) {
	cairo_surface_flush(s->desktop);
	cairo_surface_flush(wall);
	const uint32_t *a = (const uint32_t *)cairo_image_surface_get_data(s->desktop);
	const uint32_t *b = (const uint32_t *)cairo_image_surface_get_data(wall);
	int sa = cairo_image_surface_get_stride(s->desktop) / 4, sb = cairo_image_surface_get_stride(wall) / 4;
	double diff = 0;
	int n = 0;
	for (int k = 0; k < 4000 && n < 400; k++) {
		int x = (int)(saver_random() * s->width), y = (int)(saver_random() * s->height);
		if (!bare(s, x, y)) {
			continue;
		}
		uint32_t p = a[y * sa + x], q = b[y * sb + x];
		diff += abs((int)((p >> 16) & 255) - (int)((q >> 16) & 255)) +
			abs((int)((p >> 8) & 255) - (int)((q >> 8) & 255)) + abs((int)(p & 255) - (int)(q & 255));
		n++;
	}
	return n < 20 || diff / n / 3 < 18; // desktop icons and the like differ a little
}

/* The colour of the desktop just around a window, for where it stood. */
static void surrounding_colour(struct gravity *s, struct saver_window *b, double rgb[3]) {
	const uint32_t *px = (const uint32_t *)cairo_image_surface_get_data(s->desktop);
	int stride = cairo_image_surface_get_stride(s->desktop) / 4;
	double sum[3] = { 0 };
	int n = 0;
	for (int k = 0; k < 400; k++) {
		// a point on a ring outside the window and its shadow
		double f = k / 400.0, ring = shadow_margin(s) + 4;
		int x, y;
		if (f < 0.25) {
			x = (int)(b->x + b->w * f * 4); y = (int)(b->y - ring);
		} else if (f < 0.5) {
			x = (int)(b->x + b->w + ring); y = (int)(b->y + b->h * (f - 0.25) * 4);
		} else if (f < 0.75) {
			x = (int)(b->x + b->w * (f - 0.5) * 4); y = (int)(b->y + b->h + ring);
		} else {
			x = (int)(b->x - ring); y = (int)(b->y + b->h * (f - 0.75) * 4);
		}
		if (x < 0 || y < 0 || x >= s->width || y >= s->height || !bare(s, x, y)) {
			continue;
		}
		uint32_t p = px[y * stride + x];
		sum[0] += (p >> 16) & 255;
		sum[1] += (p >> 8) & 255;
		sum[2] += p & 255;
		n++;
	}
	for (int k = 0; k < 3; k++) {
		rgb[k] = n ? sum[k] / n / 255.0 : 0.12;
	}
}

/* The desktop without the windows: as it was around them, the wallpaper where they stood. */
static void make_empty(struct gravity *s, const struct saver_options *options, bool real) {
	s->empty = cairo_image_surface_create(CAIRO_FORMAT_RGB24, s->width, s->height);
	cairo_t *cr = cairo_create(s->empty);
	cairo_set_source_surface(cr, s->desktop, 0, 0);
	cairo_paint(cr);
	// where the windows stood, with the shadows around them
	double m = shadow_margin(s);
	for (int i = 0; i < s->count; i++) {
		cairo_rectangle(cr, s->wins[i].x - m, s->wins[i].y - m, s->wins[i].w + 2 * m,
			s->wins[i].h + 2 * m);
	}
	cairo_clip(cr);
	cairo_surface_t *wall = real ? saver_wallpaper(options->output, s->width, s->height) : NULL;
	if (wall && !wallpaper_matches(s, wall)) {
		cairo_surface_destroy(wall); // not the one on the screen
		wall = NULL;
	}
	if (wall) {
		cairo_set_source_surface(cr, wall, 0, 0);
		cairo_paint(cr);
		cairo_surface_destroy(wall);
	} else if (!real) {
		saver_fake_wallpaper(cr, s->width, s->height);
	} else {
		// no wallpaper known: each place in the colour of the desktop around it
		cairo_reset_clip(cr);
		for (int i = 0; i < s->count; i++) {
			double rgb[3];
			surrounding_colour(s, &s->wins[i], rgb);
			cairo_rectangle(cr, s->wins[i].x - m, s->wins[i].y - m, s->wins[i].w + 2 * m,
				s->wins[i].h + 2 * m);
			cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
			cairo_fill(cr);
		}
	}
	cairo_destroy(cr);
}

/* ---------- falling ---------- */

static void puff(struct gravity *s, double x, double y, int n) {
	for (int k = 0; k < n; k++) {
		struct dust *d = &s->dust[s->dust_next];
		s->dust_next = (s->dust_next + 1) % DUST_MAX;
		double a = saver_between(-M_PI, 0), v = saver_between(20, 110) * s->u;
		*d = (struct dust){ (float)x, (float)y, (float)(cos(a) * v), (float)(sin(a) * v),
			(float)saver_between(0.4, 0.9), 0 };
	}
}

/* Where the middle of a hanging window is: swung about its corner by its angle. */
static void hanging_centre(struct pane *p, double *cx, double *cy) {
	double px = p->hinge < 0 ? p->x : p->x + p->w, py = p->y;
	double dx = p->x + p->w / 2 - px, dy = p->y + p->h / 2 - py;
	*cx = px + dx * cos(p->angle) - dy * sin(p->angle);
	*cy = py + dx * sin(p->angle) + dy * cos(p->angle);
}

static void pane_step(struct gravity *s, struct pane *p, double dt) {
	double u = s->u;
	switch (p->state) {
	case PANE_REST:
		if (s->t >= p->start) {
			p->state = PANE_HANGING;
			p->t = 0;
			// the other corner gives first: a crack of dust where it tore loose
			puff(s, p->hinge < 0 ? p->x + p->w : p->x, p->y, 10);
		}
		break;
	case PANE_HANGING: {
		p->t += dt;
		// dropping on the loose side, swinging back a little, overshooting
		double k = p->t / HANG_TIME;
		double swing = 1 - exp(-k * 4) * cos(k * 9);
		double old = p->angle;
		p->angle = p->hinge * -0.16 * swing;
		if (p->t >= HANG_TIME) {
			// the last corner lets go: it falls from the way it was swinging
			hanging_centre(p, &p->cx, &p->cy);
			p->spin = (p->angle - old) / dt * 0.6 + p->hinge * -0.25;
			p->vx = -p->hinge * saver_between(20, 70) * u;
			p->vy = saver_between(-40, 10) * u;
			p->state = PANE_FALLING;
			p->t = 0;
			puff(s, p->hinge < 0 ? p->x : p->x + p->w, p->y, 8);
		}
		break;
	}
	case PANE_FALLING:
		p->t += dt;
		p->vy += s->gravity * dt;
		p->cx += p->vx * dt;
		p->cy += p->vy * dt;
		p->angle += p->spin * dt;
		if (p->cy - hypot(p->w, p->h) / 2 > s->height) {
			p->state = PANE_GONE;
		}
		break;
	case PANE_GONE:
		break;
	}
}

static void draw_pane(struct gravity *s, cairo_t *cr, struct pane *p) {
	if (p->state == PANE_GONE) {
		return;
	}
	if (p->state == PANE_REST) {
		cairo_set_source_surface(cr, p->img, round(p->x), round(p->y));
		cairo_paint(cr);
		return;
	}
	double cx, cy;
	if (p->state == PANE_HANGING) {
		hanging_centre(p, &cx, &cy);
	} else {
		cx = p->cx;
		cy = p->cy;
	}
	double u = s->u;
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_rotate(cr, p->angle);
	// its shadow on the desktop, further off the further it has fallen
	double lift = p->state == PANE_FALLING ? fmin(1, p->t * 1.5) : 0.3;
	cairo_rectangle(cr, -p->w / 2 + u * 8 * (1 + lift), -p->h / 2 + u * 12 * (1 + lift), p->w,
		p->h);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.3 - 0.12 * lift);
	cairo_fill(cr);
	cairo_set_source_surface(cr, p->img, -p->w / 2, -p->h / 2);
	// while it moves, the quick filter: nobody sees the edges of a falling window
	cairo_pattern_set_filter(cairo_get_source(cr), p->state == PANE_FALLING ?
		CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* ---------- the saver ---------- */

static void *gravity_create(int width, int height, const struct saver_options *options) {
	struct gravity *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->u = saver_unit(width, height);
	// the real windows, or in the preview ones of its own
	bool real = saver_tilewin_windows(options->output, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	s->desktop = saver_desktop(options, width, height, s->wins, WINDOWS_MAX, &s->count, s->bars,
		&s->bar_count);
	// each with its border and the line of shade around it, so nothing of it stays behind
	for (int i = 0; i < s->count; i++) {
		struct saver_window *b = &s->wins[i];
		double edge = fmax(1, b->border) + 1;
		b->x -= edge;
		b->y -= edge;
		b->w += 2 * edge;
		b->h += 2 * edge;
		b->title_h += edge;
	}
	static const double gravities[] = { 1800, 900, 3200 }, spacings[] = { 0.55, 1.1, 0.3 };
	int pace = saver_choice(options, &saver_gravity, "speed");
	s->gravity = gravities[pace] * s->u;
	s->bar_falls = saver_toggle(options, &saver_gravity, "taskbar");
	make_empty(s, options, real);
	// they come loose from the top of the stack down
	double at = 1.2;
	for (int i = s->count - 1; i >= 0; i--) {
		struct pane *p = &s->panes[i];
		struct saver_window *b = &s->wins[i];
		*p = (struct pane){ .img = cut_window(s, i), .x = b->x, .y = b->y, .w = b->w, .h = b->h,
			.start = at, .hinge = saver_random() < 0.5 ? -1 : 1 };
		at += spacings[pace] * saver_between(0.7, 1.3);
	}
	for (int i = 0; i < s->bar_count; i++) {
		struct pane *p = &s->bar_panes[i];
		struct saver_window *b = &s->bars[i];
		cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			(int)fmax(1, ceil(b->w)), (int)fmax(1, ceil(b->h)));
		cairo_t *cr = cairo_create(img);
		cairo_set_source_surface(cr, s->desktop, -b->x, -b->y);
		cairo_paint(cr);
		cairo_destroy(cr);
		*p = (struct pane){ .img = img, .x = b->x, .y = b->y, .w = b->w, .h = b->h,
			.start = s->bar_falls ? at + 1.5 : 1e9, .hinge = saver_random() < 0.5 ? -1 : 1 };
	}
	return s;
}

static void gravity_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct gravity *s = state;
	s->t += dt;
	for (int i = 0; i < s->count; i++) {
		pane_step(s, &s->panes[i], dt);
	}
	for (int i = 0; i < s->bar_count; i++) {
		pane_step(s, &s->bar_panes[i], dt);
	}
	cairo_set_source_surface(cr, s->empty, 0, 0);
	cairo_paint(cr);
	// those still in their place in their order, then the loose ones over them
	for (int i = 0; i < s->count; i++) {
		if (s->panes[i].state == PANE_REST) {
			draw_pane(s, cr, &s->panes[i]);
		}
	}
	for (int i = 0; i < s->count; i++) {
		if (s->panes[i].state != PANE_REST) {
			draw_pane(s, cr, &s->panes[i]);
		}
	}
	// the dust where they tore loose
	for (int i = 0; i < DUST_MAX; i++) {
		struct dust *d = &s->dust[i];
		if (d->age >= d->life) {
			continue;
		}
		d->age += (float)dt;
		d->vy += (float)(s->u * 200 * dt);
		d->x += d->vx * (float)dt;
		d->y += d->vy * (float)dt;
		double k = 1 - d->age / d->life;
		cairo_arc(cr, d->x, d->y, s->u * (1.5 + 3 * (1 - k)), 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 0.75, 0.75, 0.72, 0.5 * k);
		cairo_fill(cr);
	}
	// the taskbar in front: they fall behind it; if it falls itself, the empty desktop
	for (int i = 0; i < s->bar_count; i++) {
		draw_pane(s, cr, &s->bar_panes[i]);
	}
}

static void gravity_destroy(void *state) {
	struct gravity *s = state;
	for (int i = 0; i < s->count; i++) {
		cairo_surface_destroy(s->panes[i].img);
	}
	for (int i = 0; i < s->bar_count; i++) {
		cairo_surface_destroy(s->bar_panes[i].img);
	}
	cairo_surface_destroy(s->empty);
	cairo_surface_destroy(s->desktop);
	free(s);
}

static const char *const speed_values[] = { "normal", "slow", "fast", NULL };
static const char *const speed_labels[] = { "Normal", "Slow", "Fast", NULL };
static const struct saver_option gravity_options[] = {
	{ "speed", "Falling", "How fast the windows come loose and fall", SAVER_CHOICE, speed_values,
		speed_labels, false },
	{ "taskbar", "The taskbar falls too", "Last, so nothing of the windows shows on its buttons "
		"either", SAVER_TOGGLE, NULL, NULL, false },
	{ 0 },
};

const struct saver saver_gravity = {
	.name = "gravity",
	.title = "Gravity",
	.description = "For privacy: the windows come loose and fall behind the taskbar, off the "
		"screen",
	.wants_desktop = true,
	.options = gravity_options,
	.create = gravity_create,
	.draw = gravity_draw,
	.destroy = gravity_destroy,
};
