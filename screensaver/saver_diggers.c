#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saver_util.h"

/*
 * Diggers: little miners in yellow helmets run about over the desktop and dig
 * into the windows that are open on it, looking for what is inside. They
 * walk on the tops of the windows and on the bottom of the screen, fall off
 * edges, and dig like a real mine: level drifts one above the other, joined
 * by straight shafts. Now and then one strikes something (a page, a letter of the window's title, a gem,
 * a folder, a note, a picture) and carries it off in triumph. A miner done
 * digging deep in a window hammers a wooden staircase together, step by step,
 * up a narrow stairwell to its top. One of them always carries dynamite: he plants
 * a bundle against a window, runs, and the blast tears a crater into it,
 * knocks the others off their feet and now and then turns up a find. The
 * taskbar is solid ground: they walk on it, but no pick, stair or blast gets
 * through it. When the windows leave no room to walk, they climb in at the edges of the screen
 * and dig their way in from there. When the windows are dug out enough, a huge
 * drilling machine pushes in from a side and grinds all of it away, throwing
 * the miners off the screen; then helicopters fly the windows back in, slab by
 * slab, and the miners parachute in again one by one. The slabs are the real
 * windows, cut from a picture of the screen taken before the saver started.
 *
 * The windows are the real ones: tileWin tells where they are, the saver is
 * drawn over the desktop, and the tunnels are dark holes in the windows.
 * Without tileWin (the preview of the settings) it brings windows of its own.
 */

#define MEN_MAX 44
#define ITEMS_MAX 32
#define DIRT_MAX 400
#define WINDOWS_MAX 48
#define BOMBS_MAX 4
#define STAIRS_MAX 16
#define PLANKS_MAX 140
#define STAIR_ROT 45       // seconds a staircase lasts nobody walks on it
#define FALLING_MAX 60
#define TARGET_PATIENCE 30 // seconds before a miner gives up on a spot
#define STUCK_TIME 3       // seconds on one spot before a miner gets himself out
#define BLAST_TIME 1.8
#define MINE_LEVEL 2.6         // miner heights between the drifts of a mine
#define MINE_COLUMN 5.0        // and between its shafts
#define STAIRWELL 1.9          // how wide a staircase's shaft is, in miner heights
#define SLABS_MAX 256          // the pieces the helicopters bring the windows back in
#define HELIS_MAX 4
#define DRILL_TIME 7.0         // seconds the drilling machine takes across the screen

enum man_state {
	MAN_FALL,
	MAN_WALK,
	MAN_DIG,
	MAN_CHEER,
	MAN_BUILD,  // a staircase up to the top of the window
	MAN_PLANT,  // the dynamite goes down
	MAN_THROW,  // a grappling hook goes up
	MAN_CLIMB,  // and up the rope after it
};

enum item_kind {
	ITEM_PAGE,
	ITEM_LETTER,
	ITEM_GEM,
	ITEM_FOLDER,
	ITEM_NOTE,
	ITEM_PICTURE,
	ITEM_KINDS,
};

struct man {
	double x, y;       // feet
	double vy;
	int dir;           // -1 left, 1 right
	enum man_state state;
	double dx, dy;     // where the pick goes while digging
	double timer, phase, think;
	int carry;         // enum item_kind, -1 for nothing
	char letter;
	double hue;        // the color of the overalls
	double vx;         // thrown sideways by a blast
	bool tnt;          // the one with the dynamite
	double cooldown;   // until he lights the next one
	double run;        // running from a lit fuse
	double top;        // building: the top of the window it climbs to
	double left, right; // and how far the window reaches, to turn the stairs in time
	int steps;
	double fall;       // how far it has fallen so far
	double chute;      // the parachute: 0 folded, 1 open
	bool climbing;     // it went up the last step it took
	double tx, ty;     // the spot it is digging for
	bool target;
	double target_age;
	double wx;         // the foot of a staircase it walks to first, < 0 for none
	int stair;         // the staircase it builds, -1 for none
	double hx, hy;     // where the grappling hook caught, on the edge of a ledge
	double ledge_x;    // where to step off the rope onto the ledge
	bool flung;        // hit by the drilling machine: off the screen and gone
	double anchor_x, anchor_y, stuck; // where it was a while ago, and how long it stayed near
	double turned;     // when it last turned from a wall
};

/* A piece of a window the helicopters bring back: a band of it across its width. */
struct slab {
	int window;
	double x, y, w, h;
};

enum heli_phase {
	HELI_IN,     // flying in with the slab under it
	HELI_LOWER,  // letting it down into its place
	HELI_OUT,    // and away, lighter
};

struct heli {
	bool alive;
	enum heli_phase phase;
	int slab;
	int dir;           // the way it flies
	double t, time;    // into the phase, and how long it takes
	double x0, y0, x, y;
};

enum renovation {
	RENO_NONE,
	RENO_DRILL,  // the drilling machine grinds everything away
	RENO_HELIS,  // the helicopters bring the windows back
};

struct plank {
	float x0, x1, y;
};

/* A staircase: its planks, where it starts and how high it gets, and when it was used. */
struct stair {
	bool alive, building;
	double used;
	double base_x, base_y, top_y;
	int dir;           // the way its first flight goes up
	int count;
	struct plank planks[PLANKS_MAX];
};

/* A plank of a staircase that fell apart, tumbling down. */
struct falling {
	double x, y, vx, vy, angle, spin, w, life;
};

struct bomb {
	double x, y, fuse;
	bool alive;
};

struct blast {
	double x, y, age, r;
	bool alive;
};

struct item {
	double x, y, vy, life;
	int kind;
	char letter;
	bool alive;
};

struct dirt {
	double x, y, vx, vy, life;
	double shade;
};

struct diggers {
	int width, height;
	double u, h;            // unit, and how tall a miner is
	bool dynamite;          // one of them carries it
	double renew_after;     // seconds before the windows are renewed, 0 never
	int cell, gw, gh;       // the grid the soil is kept in
	unsigned char *soil;    // 1: inside a window
	unsigned char *dug;     // 1: dug out
	int soil_cells, dug_cells;
	cairo_surface_t *tunnels; // the holes, drawn as they are dug
	// what has been dug, grown by the width of the earthy edge and of the outline:
	// the tunnels are drawn from these, so where two meet they are one shape
	cairo_surface_t *mask_hole, *mask_band, *mask_line;
	cairo_pattern_t *pebbles;
	unsigned char *built;   // 1: a plank of a staircase
	unsigned char *rock;    // 1: the taskbar, walked on and never dug
	struct saver_window bars[4]; // where the taskbars are
	int bar_count, rock_cells;
	double sky;             // below the taskbars at the top: where they drop in
	cairo_surface_t *planks;
	struct bomb bombs[BOMBS_MAX];
	struct blast blasts[BOMBS_MAX];
	struct saver_window windows[WINDOWS_MAX];
	int window_count;
	bool fake;              // windows of its own, drawn by the saver
	char output[64];
	double refresh, spawn, age, found_flash;
	int found;
	struct man men[MEN_MAX];
	int men_count, men_max;
	struct item items[ITEMS_MAX];
	struct dirt dirt[DIRT_MAX];
	struct stair stairs[STAIRS_MAX];
	struct falling falling[FALLING_MAX];
	double clock;           // seconds since the start, never reset
	int dirt_next;
	enum renovation reno;
	double drill_x;         // the tip of the drilling machine
	int drill_dir;          // the way it goes
	struct slab slabs[SLABS_MAX];
	int slab_count, slab_next, slabs_placed;
	struct heli helis[HELIS_MAX];
	double heli_wait;       // until the next helicopter takes off
	cairo_surface_t *desktop; // the screen before the saver: the real pieces of the windows
	struct saver_window pictured[WINDOWS_MAX]; // the windows as they were on it
	int pictured_count;
};

struct man;
static void pick_target(struct diggers *s, struct man *m);
static int stair_nearby(struct diggers *s, struct man *m, double goal_y);

/* ---------- the windows ---------- */

/* The windows on this screen, from tileWin; false when it cannot tell. */
static bool read_windows(struct diggers *s) {
	return saver_tilewin_windows(s->output, s->windows, WINDOWS_MAX, &s->window_count,
		s->bars, &s->bar_count);
}

/* Windows of its own, for the preview: a few, overlapping, leaving room to run. */
static void make_windows(struct diggers *s) {
	static const char *const titles[] = { "Documents", "Notes - Editor", "Music",
		"Holiday photos", "Terminal" };
	s->window_count = 3 + (int)(saver_random() * 2);
	for (int i = 0; i < s->window_count; i++) {
		struct saver_window *b = &s->windows[i];
		b->w = s->width * saver_between(0.22, 0.36);
		b->h = s->height * saver_between(0.25, 0.42);
		b->x = saver_between(0.04, 0.96) * (s->width - b->w);
		b->y = saver_between(0.18, 0.9) * (s->height - b->h);
		snprintf(b->title, sizeof(b->title), "%s", titles[i % 5]);
	}
	// and a taskbar along the bottom
	double bar = fmax(8, s->height * 0.07);
	s->bars[0] = (struct saver_window){ .x = 0, .y = s->height - bar, .w = s->width, .h = bar };
	s->bar_count = 1;
}

static int cell_index(struct diggers *s, int cx, int cy) {
	return cy * s->gw + cx;
}

/* Fills the soil from the windows; digging starts over. */
static void build_soil(struct diggers *s) {
	memset(s->soil, 0, s->gw * s->gh);
	memset(s->dug, 0, s->gw * s->gh);
	s->soil_cells = s->dug_cells = 0;
	for (int i = 0; i < s->window_count; i++) {
		struct saver_window *b = &s->windows[i];
		int x0 = (int)floor(b->x / s->cell), x1 = (int)ceil((b->x + b->w) / s->cell);
		int y0 = (int)floor(b->y / s->cell), y1 = (int)ceil((b->y + b->h) / s->cell);
		for (int cy = y0 < 0 ? 0 : y0; cy < y1 && cy < s->gh; cy++) {
			for (int cx = x0 < 0 ? 0 : x0; cx < x1 && cx < s->gw; cx++) {
				unsigned char *c = &s->soil[cell_index(s, cx, cy)];
				s->soil_cells += !*c;
				*c = 1;
			}
		}
	}
	memset(s->built, 0, s->gw * s->gh);
	for (int i = 0; i < STAIRS_MAX; i++) {
		s->stairs[i].alive = false;
		s->stairs[i].count = 0;
	}
	for (int i = 0; i < s->men_count; i++) {
		s->men[i].stair = -1;
		s->men[i].target = false;
		s->men[i].wx = -1;
		if (s->men[i].state == MAN_BUILD) {
			s->men[i].state = MAN_FALL;
		}
	}
	memset(s->rock, 0, s->gw * s->gh);
	s->rock_cells = 0;
	s->sky = -s->h * 0.2;
	for (int i = 0; i < s->bar_count; i++) {
		struct saver_window *b = &s->bars[i];
		int x0 = (int)floor(b->x / s->cell), x1 = (int)ceil((b->x + b->w) / s->cell);
		int y0 = (int)floor(b->y / s->cell), y1 = (int)ceil((b->y + b->h) / s->cell);
		for (int cy = y0 < 0 ? 0 : y0; cy < y1 && cy < s->gh; cy++) {
			for (int cx = x0 < 0 ? 0 : x0; cx < x1 && cx < s->gw; cx++) {
				int k = cell_index(s, cx, cy);
				s->rock_cells += !s->rock[k];
				s->rock[k] = 1;
				s->soil_cells -= s->soil[k];
				s->soil[k] = 0; // a window under the taskbar is out of reach
			}
		}
		if (b->y <= 0 && b->w >= s->width * 0.5) {
			s->sky = fmax(s->sky, b->y + b->h + s->cell); // drop in below a taskbar at the top
		}
	}
	cairo_surface_t *layers[5] = { s->tunnels, s->planks, s->mask_hole, s->mask_band,
		s->mask_line };
	for (int i = 0; i < 5; i++) {
		cairo_t *cr = cairo_create(layers[i]);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_destroy(cr);
	}
}

/* Solid: soil not dug yet, the sides and the bottom of the screen. */
static bool solid_cell(struct diggers *s, int cx, int cy) {
	if (cx < 0 || cx >= s->gw || cy >= s->gh) {
		return true;
	}
	if (cy < 0) {
		return false;
	}
	int i = cell_index(s, cx, cy);
	return (s->soil[i] && !s->dug[i]) || s->built[i] || s->rock[i];
}

static bool solid_at(struct diggers *s, double x, double y) {
	return solid_cell(s, (int)floor(x / s->cell), (int)floor(y / s->cell));
}

static bool soil_at(struct diggers *s, double x, double y) {
	int cx = (int)floor(x / s->cell), cy = (int)floor(y / s->cell);
	return cx >= 0 && cx < s->gw && cy >= 0 && cy < s->gh && s->soil[cell_index(s, cx, cy)] &&
		!s->dug[cell_index(s, cx, cy)];
}

/* The taskbar: nothing digs into it. */
static bool rock_at(struct diggers *s, double x, double y) {
	int cx = (int)floor(x / s->cell), cy = (int)floor(y / s->cell);
	return cx >= 0 && cx < s->gw && cy >= 0 && cy < s->gh && s->rock[cell_index(s, cx, cy)];
}

/* How much of the screen is free to run about on. */
static double free_share(struct diggers *s) {
	return 1 - (double)(s->soil_cells + s->rock_cells) / (s->gw * s->gh);
}

/* ---------- digging ---------- */

static void dirt_spray(struct diggers *s, double x, double y, int count) {
	for (int i = 0; i < count; i++) {
		struct dirt *d = &s->dirt[s->dirt_next];
		s->dirt_next = (s->dirt_next + 1) % DIRT_MAX;
		d->x = x;
		d->y = y;
		d->vx = saver_between(-90, 90) * s->u;
		d->vy = saver_between(-160, -30) * s->u;
		d->life = saver_between(0.4, 0.9);
		d->shade = saver_between(0.25, 0.55);
	}
}

/*
 * Draws the tunnels over a part of the screen from the masks, in flat cartoon
 * colors: a dark outline, a band of lighter earth, and the hole with pebbles
 * in it. All of it only on the windows.
 */
static void redraw_tunnels(struct diggers *s, double x, double y, double w, double h) {
	cairo_t *cr = cairo_create(s->tunnels);
	// whole pixels: a clip edge through a pixel would leave a faint seam there
	double x0 = floor(x), y0 = floor(y);
	cairo_rectangle(cr, x0, y0, ceil(x + w) - x0, ceil(y + h) - y0);
	cairo_clip(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	for (int i = 0; i < s->window_count; i++) {
		struct saver_window *b = &s->windows[i];
		cairo_rectangle(cr, round(b->x), round(b->y), round(b->w), round(b->h));
	}
	cairo_clip(cr);
	cairo_set_source_rgb(cr, 0.16, 0.09, 0.04);
	cairo_mask_surface(cr, s->mask_line, 0, 0);
	cairo_set_source_rgb(cr, 0.8, 0.56, 0.3);
	cairo_mask_surface(cr, s->mask_band, 0, 0);
	cairo_set_source_rgb(cr, 0.4, 0.25, 0.13);
	cairo_mask_surface(cr, s->mask_hole, 0, 0);
	cairo_set_source(cr, s->pebbles);
	cairo_mask_surface(cr, s->mask_hole, 0, 0);
	cairo_destroy(cr);
}

/* Pebbles and specks in the earth, a tile that repeats over the whole screen. */
static cairo_pattern_t *make_pebbles(double u) {
	int size = (int)fmax(48, round(u * 90));
	cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(tile);
	for (int i = 0; i < 9; i++) {
		double px = saver_random() * size, py = saver_random() * size;
		double rx = u * saver_between(2.5, 5), ry = rx * saver_between(0.6, 0.9);
		// drawn at every place it shows when the tile repeats
		for (int ox = -1; ox <= 1; ox++) {
			for (int oy = -1; oy <= 1; oy++) {
				cairo_save(cr);
				cairo_translate(cr, px + ox * size, py + oy * size);
				cairo_scale(cr, rx, ry);
				cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
				cairo_restore(cr);
				cairo_set_source_rgb(cr, 0.3, 0.18, 0.09);
				cairo_fill_preserve(cr);
				cairo_set_source_rgb(cr, 0.16, 0.09, 0.04);
				cairo_set_line_width(cr, fmax(1, u * 0.9));
				cairo_stroke(cr);
				cairo_arc(cr, px + ox * size - rx * 0.35, py + oy * size - ry * 0.35, rx * 0.25,
					0, 2 * M_PI);
				cairo_set_source_rgb(cr, 0.55, 0.38, 0.22);
				cairo_fill(cr);
			}
		}
	}
	for (int i = 0; i < 14; i++) {
		cairo_arc(cr, saver_random() * size, saver_random() * size, fmax(0.8, u * 0.9), 0,
			2 * M_PI);
		cairo_set_source_rgb(cr, 0.27, 0.16, 0.08);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(tile);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_surface_destroy(tile);
	return pattern;
}

/* Digs a round hole; returns how many cells of soil it took. */
static int dig(struct diggers *s, double x, double y, double r) {
	int taken = 0;
	int x0 = (int)floor((x - r) / s->cell), x1 = (int)ceil((x + r) / s->cell);
	int y0 = (int)floor((y - r) / s->cell), y1 = (int)ceil((y + r) / s->cell);
	for (int cy = y0; cy <= y1; cy++) {
		for (int cx = x0; cx <= x1; cx++) {
			if (cx < 0 || cx >= s->gw || cy < 0 || cy >= s->gh) {
				continue;
			}
			double mx = (cx + 0.5) * s->cell - x, my = (cy + 0.5) * s->cell - y;
			int i = cell_index(s, cx, cy);
			if (mx * mx + my * my <= r * r && s->soil[i] && !s->dug[i]) {
				s->dug[i] = 1;
				taken++;
			}
		}
	}
	if (!taken) {
		return 0;
	}
	s->dug_cells += taken;
	double band = s->h * 0.12, outline = fmax(1.5, s->h * 0.06);
	cairo_surface_t *masks[3] = { s->mask_hole, s->mask_band, s->mask_line };
	double radii[3] = { r, r + band, r + band + outline };
	for (int i = 0; i < 3; i++) {
		cairo_t *mc = cairo_create(masks[i]);
		cairo_arc(mc, x, y, radii[i], 0, 2 * M_PI);
		cairo_fill(mc);
		cairo_destroy(mc);
	}
	redraw_tunnels(s, x - radii[2] - 2, y - radii[2] - 2, 2 * radii[2] + 4, 2 * radii[2] + 4);
	dirt_spray(s, x, y, taken > 3 ? 3 : taken);
	return taken;
}

/* ---------- the miners ---------- */

static void man_place(struct diggers *s, struct man *m) {
	memset(m, 0, sizeof(*m));
	m->carry = -1;
	m->hue = saver_random();
	m->phase = saver_random() * 10;
	m->think = saver_between(1, 3);
	m->dir = saver_random() < 0.5 ? -1 : 1;
	m->tnt = s->dynamite && m == &s->men[0]; // the first one in carries the dynamite
	m->stair = -1;
	m->wx = -1;
	pick_target(s, m);
	m->cooldown = saver_between(2, 5);
	if (free_share(s) < 0.1) {
		// no room: in at the edge of the screen, straight into the windows
		m->dir = saver_random() < 0.5 ? 1 : -1;
		m->x = m->dir > 0 ? s->h * 0.35 : s->width - s->h * 0.35;
		m->y = saver_between(0.2, 0.95) * s->height;
		for (int tries = 0; tries < 20 && solid_at(s, m->x, m->y - s->h * 0.5); tries++) {
			m->y = saver_between(0.2, 0.95) * s->height; // not in a taskbar at the side
		}
		dig(s, m->x, m->y - s->h * 0.5, s->h * 0.62);
		m->state = MAN_DIG;
		m->dx = m->dir;
		m->dy = 0;
		m->timer = saver_between(3, 7);
		return;
	}
	// dropping in from the top, above a free spot if one is found
	// dropping in from the top: mostly onto a window, sometimes onto free ground
	m->state = MAN_FALL;
	m->y = s->sky;
	bool onto_window = m->tnt || saver_random() < 0.7; // the blaster wants a window
	for (int tries = 0; tries < 30; tries++) {
		m->x = saver_between(s->h, s->width - s->h);
		bool window_below = false;
		for (double y = 0; y < s->height && !window_below; y += s->cell) {
			window_below = soil_at(s, m->x, y);
		}
		if (window_below == onto_window) {
			break;
		}
	}
}

static bool grounded(struct diggers *s, struct man *m) {
	return solid_at(s, m->x, m->y + 1) || m->y >= s->height - 0.5;
}

/* Whether the body of a miner would fit at x, y (feet). */
static bool body_free(struct diggers *s, double x, double y) {
	for (double part = 0.15; part <= 0.95; part += 0.2) {
		if (solid_at(s, x, y - s->h * part)) {
			return false;
		}
	}
	return true;
}

/* The windows fill the screen: the finds are reached from the sides, digging inwards. */
static bool crowded(struct diggers *s) {
	return free_share(s) < 0.1;
}

static void start_dig(struct diggers *s, struct man *m, double dx, double dy) {
	double len = hypot(dx, dy);
	m->state = MAN_DIG;
	m->dx = dx / len;
	m->dy = dy / len;
	m->timer = crowded(s) ? saver_between(4, 9) : saver_between(2, 5);
}

/*
 * The top of the window a miner is in, or right under: where a staircase
 * leads. 0 when there is none worth climbing to.
 */
static double window_top(struct diggers *s, struct man *m, double *left, double *right) {
	double top = 0;
	for (int i = 0; i < s->window_count; i++) {
		struct saver_window *b = &s->windows[i];
		if (m->x < b->x + s->h * 0.3 || m->x > b->x + b->w - s->h * 0.3) {
			continue;
		}
		// deep in it, or somewhere under it: the stairs go up to it and through it
		bool in = m->y > b->y + s->h * 3 && m->y <= b->y + b->h + s->cell;
		bool under = m->y > b->y + b->h;
		if ((in || under) && b->y > 0 && (!top || b->y < top)) {
			top = b->y;
			*left = b->x;
			*right = b->x + b->w;
		}
	}
	return top;
}

/* Up to goal_y (0: the top of the window): by stairs that are there, or by building some. */
static bool start_build(struct diggers *s, struct man *m, double goal_y) {
	double left = 0, right = 0;
	double top = window_top(s, m, &left, &right);
	if (!top) {
		return false;
	}
	if (goal_y > top) {
		top = goal_y;
	}
	int near = stair_nearby(s, m, top);
	if (near >= 0) {
		m->wx = s->stairs[near].base_x; // there are stairs that way already
		m->state = MAN_WALK;
		return true;
	}
	// up the side with more room, so the flights are long and the turns few
	double room_right = right - m->x, room_left = m->x - left;
	double needed = (m->y - top) / 0.24 * 0.34;
	double room = m->dir > 0 ? room_right : room_left;
	if (room < needed && (m->dir > 0 ? room_left : room_right) > room) {
		m->dir = -m->dir;
	}
	// the flights turn in a narrow stairwell, a straight shaft up, instead of cutting
	// across the whole window
	double well = s->h * STAIRWELL, margin = s->h * 0.45;
	if (m->dir > 0) {
		right = fmin(right, m->x + well + margin);
		left = fmax(left, right - well - 2 * margin - s->h * 0.2);
	} else {
		left = fmax(left, m->x - well - margin);
		right = fmin(right, left + well + 2 * margin + s->h * 0.2);
	}
	m->state = MAN_BUILD;
	m->top = top;
	m->left = left;
	m->right = right;
	m->steps = 0;
	m->timer = 0;
	m->stair = -1;
	return true;
}

static void draw_plank(struct diggers *s, cairo_t *cr, double left, double right, double y) {
	double thick = s->h * 0.1;
	cairo_rectangle(cr, left, y, right - left, thick);
	cairo_set_source_rgb(cr, 0.72, 0.5, 0.26);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.4, 0.26, 0.12);
	cairo_set_line_width(cr, fmax(1, s->u));
	cairo_stroke(cr);
	// a nail in the middle
	cairo_arc(cr, (left + right) / 2, y + thick / 2, fmax(0.8, s->u * 0.9), 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.25, 0.25, 0.28);
	cairo_fill(cr);
}

/* The cells a plank takes, set to value (the number of its staircase, or 0). */
static void mark_plank(struct diggers *s, const struct plank *p, unsigned char value) {
	double thick = s->h * 0.1;
	for (int cy = (int)floor(p->y / s->cell); cy * s->cell < p->y + thick && cy < s->gh; cy++) {
		for (int cx = (int)floor(p->x0 / s->cell); cx * s->cell < p->x1 && cx < s->gw; cx++) {
			if (cx >= 0 && cy >= 0) {
				s->built[cell_index(s, cx, cy)] = value;
			}
		}
	}
}

/* The planks of every staircase, drawn again after some went. */
static void redraw_planks(struct diggers *s) {
	cairo_t *cr = cairo_create(s->planks);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *st = &s->stairs[i];
		for (int k = 0; st->alive && k < st->count; k++) {
			draw_plank(s, cr, st->planks[k].x0, st->planks[k].x1, st->planks[k].y);
		}
	}
	cairo_destroy(cr);
}

/* A plank of a staircase, from x0 to x1 with its top at y. */
static void lay_plank(struct diggers *s, int stair, double x0, double x1, double y) {
	struct stair *st = &s->stairs[stair];
	if (st->count >= PLANKS_MAX) {
		return;
	}
	struct plank *p = &st->planks[st->count++];
	*p = (struct plank){ (float)fmin(x0, x1), (float)fmax(x0, x1), (float)y };
	mark_plank(s, p, (unsigned char)(stair + 1));
	st->top_y = fmin(st->top_y, y);
	st->used = s->clock;
	cairo_t *cr = cairo_create(s->planks);
	draw_plank(s, cr, p->x0, p->x1, p->y);
	cairo_destroy(cr);
}

/* A staircase nobody used for a while falls apart, plank by plank tumbling down. */
static void remove_stair(struct diggers *s, int stair, bool tumble) {
	struct stair *st = &s->stairs[stair];
	for (int k = 0; k < st->count; k++) {
		struct plank *p = &st->planks[k];
		mark_plank(s, p, 0);
		for (int f = 0; tumble && f < FALLING_MAX; f++) {
			struct falling *fl = &s->falling[f];
			if (fl->life <= 0) {
				*fl = (struct falling){ (p->x0 + p->x1) / 2, p->y, saver_between(-30, 30) * s->u,
					-saver_between(0, 80) * s->u, 0, saver_between(-6, 6), p->x1 - p->x0,
					saver_between(1.2, 2.2) };
				break;
			}
		}
	}
	// whatever was on other staircases' cells stays theirs
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *other = &s->stairs[i];
		for (int k = 0; i != stair && other->alive && k < other->count; k++) {
			mark_plank(s, &other->planks[k], (unsigned char)(i + 1));
		}
	}
	st->alive = false;
	st->count = 0;
	for (int i = 0; i < s->men_count; i++) {
		if (s->men[i].stair == stair) {
			s->men[i].stair = -1;
		}
	}
	redraw_planks(s);
}

/* A new staircase from where the miner stands; the one used longest ago gives way. */
static int new_stair(struct diggers *s, struct man *m) {
	int pick = 0;
	for (int i = 0; i < STAIRS_MAX; i++) {
		if (!s->stairs[i].alive) {
			pick = i;
			break;
		}
		if (!s->stairs[i].building && s->stairs[i].used < s->stairs[pick].used) {
			pick = i;
		}
	}
	if (s->stairs[pick].alive) {
		remove_stair(s, pick, true);
	}
	s->stairs[pick] = (struct stair){ .alive = true, .building = true, .used = s->clock,
		.base_x = m->x, .base_y = m->y, .top_y = m->y, .dir = m->dir };
	return pick;
}

/*
 * A staircase near the miner, starting on his level, that already climbs
 * about as high as he wants to go: rather than building another, he walks
 * to its foot and up it. -1 when there is none.
 */
static int stair_nearby(struct diggers *s, struct man *m, double goal_y) {
	int best = -1;
	double best_d = s->h * 6;
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *st = &s->stairs[i];
		if (!st->alive || fabs(st->base_y - m->y) > s->h * 0.6) {
			continue;
		}
		bool goes_up = st->top_y < m->y - s->h * 1.5 || st->building;
		bool high_enough = st->top_y <= goal_y + s->h * 2.5 || st->building;
		double d = fabs(st->base_x - m->x);
		if (goes_up && high_enough && d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

/* One step of a staircase: room for the body, a plank, and up onto it. */
static void build_step(struct diggers *s, struct man *m) {
	double h = s->h, step_w = h * 0.34, step_h = h * 0.24;
	double nx = m->x + m->dir * step_w;
	// the stairs turn back before they would leave the window or the screen: a switchback,
	// the next flight starting above the last so the top is reached within the window
	double margin = h * 0.45;
	bool past_window = m->right > m->left &&
		(m->dir > 0 ? nx > m->right - margin : nx < m->left + margin);
	if (past_window || nx < h * 0.35 || nx > s->width - h * 0.35) {
		m->dir = -m->dir;
		nx = m->x + m->dir * step_w;
	}
	double ny = m->y - step_h;
	if (m->right > m->left) {
		// the whole width of the stairwell, so it is one straight shaft up through the
		// window and not a zigzag of diagonal cuts
		for (double x = m->left + margin - h * 0.15; x <= m->right - margin + h * 0.15;
				x += h * 0.35) {
			dig(s, x, ny - h * 0.55, h * 0.55);
		}
	} else {
		dig(s, nx, ny - h * 0.55, h * 0.55); // through the window, where the stairs cut it
	}
	if (m->stair < 0) {
		m->stair = new_stair(s, m);
	}
	lay_plank(s, m->stair, m->x + m->dir * h * 0.12, nx + m->dir * h * 0.2, ny);
	m->x = nx;
	m->y = ny;
	m->steps++;
}

static void plant_bomb(struct diggers *s, struct man *m) {
	for (int i = 0; i < BOMBS_MAX; i++) {
		struct bomb *b = &s->bombs[i];
		if (!b->alive) {
			*b = (struct bomb){ m->x + m->dir * s->h * 0.35, m->y - s->h * 0.12, 2.4, true };
			break;
		}
	}
	// and away from it
	m->dir = -m->dir;
	m->run = 2.4;
	m->cooldown = crowded(s) ? saver_between(4, 8) : saver_between(6, 12);
	m->state = MAN_WALK;
}

static void explode(struct diggers *s, struct bomb *bomb) {
	double r = s->h * 2.3, x = bomb->x, y = bomb->y;
	bomb->alive = false;
	int taken = dig(s, x, y, r);
	// the stairs in the way go too
	for (int cy = (int)floor((y - r) / s->cell); cy <= (int)((y + r) / s->cell); cy++) {
		for (int cx = (int)floor((x - r) / s->cell); cx <= (int)((x + r) / s->cell); cx++) {
			double mx = (cx + 0.5) * s->cell - x, my = (cy + 0.5) * s->cell - y;
			if (cx >= 0 && cy >= 0 && cx < s->gw && cy < s->gh && mx * mx + my * my < r * r) {
				s->built[cell_index(s, cx, cy)] = 0;
			}
		}
	}
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *st = &s->stairs[i];
		int kept = 0;
		for (int k = 0; st->alive && k < st->count; k++) {
			struct plank *p = &st->planks[k];
			if (hypot((p->x0 + p->x1) / 2 - x, p->y - y) >= r) {
				st->planks[kept++] = *p;
			}
		}
		if (st->alive && kept < st->count) {
			// what is left goes only as high as its top plank, so nobody walks to it
			// for a climb it no longer makes
			st->count = kept;
			st->top_y = st->base_y;
			for (int k = 0; k < kept; k++) {
				st->top_y = fmin(st->top_y, st->planks[k].y);
			}
			if (!kept && !st->building) {
				st->alive = false;
			}
		}
	}
	redraw_planks(s);
	for (int i = 0; i < 60; i++) {
		struct dirt *d = &s->dirt[s->dirt_next];
		s->dirt_next = (s->dirt_next + 1) % DIRT_MAX;
		double a = saver_between(0, 2 * M_PI), v = saver_between(150, 520) * s->u;
		*d = (struct dirt){ x, y, cos(a) * v, sin(a) * v - 120 * s->u, saver_between(0.6, 1.4),
			saver_between(0.15, 0.5) };
	}
	for (int i = 0; i < BOMBS_MAX; i++) {
		if (!s->blasts[i].alive) {
			s->blasts[i] = (struct blast){ x, y, 0, r, true };
			break;
		}
	}
	// everyone near is thrown off their feet
	for (int i = 0; i < s->men_count; i++) {
		struct man *m = &s->men[i];
		double dx = m->x - x, dy = m->y - s->h * 0.5 - y, d = hypot(dx, dy);
		if (d < r * 1.5) {
			m->state = MAN_FALL;
			m->vy = -s->u * saver_between(260, 420);
			m->vx = (dx >= 0 ? 1 : -1) * s->u * saver_between(80, 200);
			m->carry = -1;
		}
	}
	// a big hole turns things up
	if (taken > 30) {
		int finds = 1 + (saver_random() < 0.4);
		for (int k = 0; k < finds; k++) {
			for (int i = 0; i < ITEMS_MAX; i++) {
				struct item *it = &s->items[i];
				if (!it->alive) {
					*it = (struct item){ x + saver_between(-r, r) * 0.4, y, -s->u * 90, 1.8,
						(int)(saver_random() * ITEM_KINDS), 'A' + (char)(saver_random() * 26),
						true };
					s->found++;
					s->found_flash = 1;
					break;
				}
			}
		}
	}
}

/*
 * A spot in a window to dig for: somewhere not dug yet, mostly not far off,
 * so a miner has somewhere to go instead of wandering.
 */
static void pick_target(struct diggers *s, struct man *m) {
	m->target = false;
	m->target_age = 0;
	m->wx = -1;
	double best = -1;
	for (int tries = 0; tries < 40 && s->window_count; tries++) {
		struct saver_window *b = &s->windows[(int)(saver_random() * s->window_count)];
		// like a mine: level drifts one above the other, joined by shafts in columns, so
		// the tunnels run straight and in parallel
		int columns = (int)((b->w - s->h * 1.4) / (s->h * MINE_COLUMN)) + 1;
		int levels = (int)((b->h - s->h * 1.6) / (s->h * MINE_LEVEL)) + 1;
		double x = b->x + s->h * 0.7 + (int)(saver_random() * columns) * s->h * MINE_COLUMN;
		double y = b->y + s->h * 1.6 + (int)(saver_random() * levels) * s->h * MINE_LEVEL -
			s->h * 0.56;
		if (x > b->x + b->w - s->h * 0.5) {
			continue; // a window too narrow for a column
		}
		if (!soil_at(s, x, y) || rock_at(s, x, y)) {
			continue;
		}
		// near is better, but now and then one heads across the screen
		double score = saver_random() / (1 + hypot(x - m->x, y - m->y) / (s->width * 0.25));
		if (score > best) {
			best = score;
			m->tx = x;
			m->ty = y;
			m->target = true;
		}
	}
}

static void find_something(struct diggers *s, struct man *m) {
	m->carry = (int)(saver_random() * ITEM_KINDS);
	m->letter = 'A' + (char)(saver_random() * 26);
	// a letter of the window it was found in
	for (int i = 0; i < s->window_count; i++) {
		struct saver_window *b = &s->windows[i];
		size_t n = strlen(b->title);
		if (n && m->x >= b->x && m->x < b->x + b->w && m->y >= b->y && m->y <= b->y + b->h + 2) {
			char c = b->title[(size_t)(saver_random() * n)];
			if (c > ' ' && (unsigned char)c < 0x80) {
				m->letter = c;
			}
		}
	}
	m->state = MAN_CHEER;
	m->timer = 0.9;
	s->found++;
	s->found_flash = 1;
}

enum ground {
	GROUND_FOUND,
	GROUND_NONE,    // air ahead: a drop
	GROUND_WALL,    // no room for the body at any height in reach
};

/*
 * Where a miner can stand at x, from up to "up" above y to "down" below it:
 * the highest spot with room for the body and something under the feet. This
 * is what lets them walk up and down stairs and slopes instead of stopping.
 */
static enum ground ground_near(struct diggers *s, double x, double y, double up, double down,
		double *out) {
	bool room = false;
	for (double dy = -up; dy <= down; dy += s->cell * 0.5) {
		double at = y + dy;
		if (at > s->height || !body_free(s, x, at)) {
			continue;
		}
		room = true;
		if (at >= s->height - 0.5 || solid_at(s, x, at + 1)) {
			// right on top of what is under the feet
			double surface = floor((at + 1) / s->cell) * s->cell - 0.01;
			*out = fmin(body_free(s, x, surface) ? surface : at, s->height);
			return GROUND_FOUND;
		}
	}
	return room ? GROUND_NONE : GROUND_WALL;
}

#define ROPE_REACH 10 // miner heights

/*
 * A ledge the grappling hook can reach: the top of the wall in front of the
 * miner, with nothing but air straight above him on the way up to it. False
 * when there is none, or when it is too low to bother or too high to reach.
 */
static bool find_ledge(struct diggers *s, struct man *m, int dir, double *hx, double *hy) {
	double h = s->h, front = m->x + dir * h * 0.32;
	if (front < h * 0.3 || front > s->width - h * 0.3) {
		return false;
	}
	bool wall = false; // passed the side of something solid on the way up
	for (double y = m->y - s->cell; y > m->y - h * ROPE_REACH && y > 0; y -= s->cell) {
		if (y < m->y - h * 0.9 && solid_at(s, m->x, y)) {
			return false; // a ceiling over the miner: the rope would not hang free
		}
		bool solid = solid_at(s, front, y);
		if (solid && rock_at(s, front, y)) {
			return false; // the taskbar at the side of the screen is no ledge
		}
		if (solid) {
			wall = true;
		} else if (wall) {
			// the top of it: something to stand on, with room above it
			double top = floor((y + s->cell) / s->cell) * s->cell - 0.01;
			double step = front + dir * h * 0.12;
			if (m->y - top < h * 1.2 || !body_free(s, step, top)) {
				return false;
			}
			*hx = front + dir * h * 0.05;
			*hy = top;
			m->ledge_x = step;
			return true;
		}
	}
	return false;
}

/* Up the wall by rope instead of stairs, when a ledge is in reach. */
static bool start_hook(struct diggers *s, struct man *m) {
	for (int k = 0; k < 2; k++) {
		int dir = k == 0 ? m->dir : -m->dir;
		// only a ledge on the way: up on the far side, the target would be further off
		if (m->target && fabs(m->tx - m->x) > s->h * 2 && (m->tx > m->x ? 1 : -1) != dir) {
			continue;
		}
		double hx, hy;
		if (find_ledge(s, m, dir, &hx, &hy)) {
			m->target_age += 6; // a climb that leads nowhere is not tried for ever
			m->dir = dir;
			m->hx = hx;
			m->hy = hy;
			m->state = MAN_THROW;
			m->timer = 0;
			return true;
		}
	}
	return false;
}

/*
 * A miner who stayed on one spot too long, walled in or turning back and
 * forth: out by whatever way there is, and after another spot.
 */
static void unstick(struct diggers *s, struct man *m) {
	double h = s->h;
	pick_target(s, m);
	m->wx = -1;
	if (!body_free(s, m->x, m->y)) {
		// in the planks of a staircase or in soil put back around him: up out of it
		for (double up = s->cell; up < h * 4; up += s->cell) {
			if (body_free(s, m->x, m->y - up)) {
				m->y -= up;
				m->state = MAN_FALL;
				m->vy = 0;
				return;
			}
		}
		dig(s, m->x, m->y - h * 0.5, h * 0.62);
	}
	// planks right beside him: the staircase goes
	for (int k = -1; k <= 1; k += 2) {
		int cx = (int)floor((m->x + k * h * 0.4) / s->cell);
		for (double part = 0.2; part < 1; part += 0.3) {
			int cy = (int)floor((m->y - h * part) / s->cell);
			if (cx >= 0 && cy >= 0 && cx < s->gw && cy < s->gh && s->built[cell_index(s, cx, cy)]) {
				remove_stair(s, s->built[cell_index(s, cx, cy)] - 1, true);
				break;
			}
		}
	}
	// then on by the pick where there is soil, the way the next spot is if it can
	bool below = soil_at(s, m->x, m->y + s->cell), above = soil_at(s, m->x, m->y - h * 1.3);
	bool side[2] = { soil_at(s, m->x - h * 0.6, m->y - h * 0.5),
		soil_at(s, m->x + h * 0.6, m->y - h * 0.5) };
	int way = m->target && m->tx > m->x ? 1 : -1;
	if (m->target && m->ty > m->y && below) {
		start_dig(s, m, 0, 1);
	} else if (side[way > 0]) {
		start_dig(s, m, way, 0);
	} else if (side[way < 0]) {
		start_dig(s, m, -way, 0);
	} else if (above) {
		start_dig(s, m, 0, -1);
	} else if (below) {
		start_dig(s, m, 0, 1);
	} else {
		// nothing to dig: a hop over whatever is in the way
		m->dir = saver_random() < 0.5 ? -1 : 1;
		m->state = MAN_FALL;
		m->vy = -s->u * 330;
		m->vx = m->dir * s->u * 110;
	}
}

static void man_step(struct diggers *s, struct man *m, double dt) {
	double h = s->h, speed = s->u * 42;
	m->phase += dt;
	m->cooldown -= dt;
	if ((m->state != MAN_WALK && m->state != MAN_DIG) || m->flung ||
			hypot(m->x - m->anchor_x, m->y - m->anchor_y) > h * 1.2) {
		m->anchor_x = m->x;
		m->anchor_y = m->y;
		m->stuck = 0;
	} else if ((m->stuck += dt) > STUCK_TIME) {
		m->stuck = 0;
		unstick(s, m);
		return;
	}
	if (m->state != MAN_FALL) {
		int cx = (int)floor(m->x / s->cell), cy = (int)floor((m->y + 1) / s->cell);
		if (cx >= 0 && cy >= 0 && cx < s->gw && cy < s->gh && s->built[cell_index(s, cx, cy)]) {
			s->stairs[s->built[cell_index(s, cx, cy)] - 1].used = s->clock;
		}
	}
	switch (m->state) {
	case MAN_FALL:
		m->vy += s->u * 700 * dt;
		if (m->flung) {
			// through the air, through everything, off the screen
			m->x += m->vx * dt;
			m->y += m->vy * dt;
			break;
		}
		if (m->vy > 0) {
			m->fall += m->vy * dt;
		}
		if (m->fall > h * 1.1) {
			// a longer fall: the parachute opens, and down it floats, swaying
			m->chute = fmin(1, m->chute + dt * 2.5);
			double slow = s->u * 70;
			if (m->chute > 0.4 && m->vy > slow) {
				m->vy = fmax(slow, m->vy - s->u * 1400 * dt);
			}
			m->vx *= 1 - fmin(1, dt * 2);
			double sway = sin(m->phase * 1.8) * s->u * 16 * m->chute;
			double sx = saver_clamp(m->x + sway * dt, h * 0.3, s->width - h * 0.3);
			if (body_free(s, sx, m->y)) {
				m->x = sx;
			}
		}
		if (fabs(m->vx) > 0.5) {
			double nx = saver_clamp(m->x + m->vx * dt, h * 0.3, s->width - h * 0.3);
			if (body_free(s, nx, m->y)) {
				m->x = nx;
			} else {
				m->vx = 0;
			}
		}
		if (m->vy < 0 && solid_at(s, m->x, m->y - h + m->vy * dt)) {
			m->vy = 0; // a ceiling
		}
		m->y += m->vy * dt;
		if (m->y >= s->height) {
			m->y = s->height;
		}
		if (grounded(s, m) && m->vy >= 0) {
			m->y = fmin(floor(m->y / s->cell) * s->cell + s->cell - 0.01, s->height);
			while (solid_at(s, m->x, m->y - 0.5) && m->y > 0) {
				m->y -= s->cell; // out of the ground it fell into
			}
			m->vy = 0;
			m->vx = 0;
			m->fall = 0;
			m->chute = 0;
			m->state = MAN_WALK;
		}
		break;
	case MAN_WALK: {
		if (!grounded(s, m)) {
			m->state = MAN_FALL;
			break;
		}
		m->target_age += dt;
		if (!m->target || !soil_at(s, m->tx, m->ty) || m->target_age > TARGET_PATIENCE) {
			pick_target(s, m); // dug out, given up on, or never had one
		}
		if (m->run <= 0 && m->wx >= 0) {
			// to the foot of a staircase, then up it the way it goes
			if (fabs(m->x - m->wx) < h * 0.25) {
				int near = -1;
				for (int i = 0; i < STAIRS_MAX; i++) {
					if (s->stairs[i].alive && fabs(s->stairs[i].base_x - m->wx) < 1) {
						near = i;
					}
				}
				m->dir = near >= 0 ? s->stairs[near].dir : m->dir;
				m->wx = -1;
				m->think = 3; // let it climb before thinking again
			} else {
				m->dir = m->wx > m->x ? 1 : -1;
			}
		} else if (m->run <= 0 && m->target && fabs(m->tx - m->x) > h * 0.4) {
			m->dir = m->tx > m->x ? 1 : -1;
		}
		double pace = speed;
		if (m->run > 0) {
			m->run -= dt;
			pace = speed * 2.3; // from the fuse
		}
		double nx = m->x + m->dir * pace * dt;
		double front = nx + m->dir * h * 0.22;
		// the ground where the feet go; and in front, only whether it is a wall
		double ground_y = m->y, front_y;
		enum ground ahead = ground_near(s, nx, m->y, h * 0.42, h * 0.42, &ground_y);
		if (ahead != GROUND_WALL && ground_near(s, front, ahead == GROUND_FOUND ? ground_y : m->y,
				h * 0.42, h * 0.42, &front_y) == GROUND_WALL) {
			ahead = GROUND_WALL;
		}
		if (ahead == GROUND_NONE && m->climbing && m->run <= 0) {
			// on the way up and at the end of a flight: the next one turns back above it
			double bx = m->x - m->dir * h * 0.3, back_y;
			if (ground_near(s, bx, m->y, h * 0.42, 0, &back_y) == GROUND_FOUND &&
					back_y < m->y - s->cell &&
					s->built[cell_index(s, (int)(bx / s->cell), (int)((back_y + 1) / s->cell))]) {
				m->dir = -m->dir;
				break;
			}
		}
		if (ahead == GROUND_FOUND) {
			// level, up a stair or a slope, or down one
			if (ground_y < m->y - s->cell * 0.5) {
				m->climbing = true;
			} else if (ground_y > m->y + s->cell * 0.5) {
				m->climbing = false;
			}
			m->x = nx;
			m->y = ground_y;
		} else if (ahead == GROUND_NONE) {
			m->x = nx; // over the edge
		} else {
			// a wall: dug into, blasted, or turned from
			if (m->tnt && m->cooldown <= 0 && m->run <= 0 &&
					soil_at(s, front, m->y - h * 0.5)) {
				m->state = MAN_PLANT; // a window in the way: blast through it
				m->timer = 0.9;
			} else if (m->run > 0) {
				m->dir = -m->dir; // cornered: the other way, quick
			} else if (m->target && m->ty < m->y - h && saver_random() < 0.4 &&
					start_hook(s, m)) {
				// the spot is up there: over the wall by rope
			} else if (soil_at(s, front, m->y - h * 0.5)) {
				// on through the wall, level like a drift of a mine; up or down to the spot
				// it goes by a shaft when it is right above or below it
				start_dig(s, m, m->dir, 0);
			} else if (s->clock - m->turned < 0.05) {
				unstick(s, m); // walls both ways and nothing to dig: out another way
			} else {
				m->dir = -m->dir;
				m->target_age += 4; // in the way: sooner another spot
				m->turned = s->clock;
			}
			break;
		}
		if (m->x < h * 0.3 || m->x > s->width - h * 0.3) {
			m->dir = m->x < h * 0.3 ? 1 : -1;
		}
		m->think -= dt;
		if (m->think <= 0 && m->wx < 0) {
			m->think = saver_between(0.3, 0.7);
			bool on_soil = soil_at(s, m->x, m->y + s->cell * 0.5);
			bool tunnel = on_soil && soil_at(s, m->x, m->y - h * 1.3);
			if (m->tnt && m->cooldown <= 0 && m->run <= 0 && (on_soil || tunnel)) {
				m->state = MAN_PLANT;
				m->timer = 0.9;
			} else if (m->target && fabs(m->tx - m->x) <= h * 0.4) {
				// right above, below or at the spot: down to it, up to it, or into it
				// below where the pick reaches level: down to it
				if (m->ty > m->y - h * 0.3 && on_soil) {
					start_dig(s, m, 0, 1);
				} else if (m->ty > m->y - h * 0.3) {
					pick_target(s, m); // below, but not through what it stands on
				} else if (m->ty < m->y - h * 1.2) {
					if (saver_random() < 0.6 && start_hook(s, m)) {
						// up the rope
					} else if (!start_build(s, m, m->ty + h * 0.9)) {
						start_dig(s, m, 0, -1); // no stairs to be had: up with the pick
					}
				} else {
					start_dig(s, m, m->dir, 0);
				}
			} else if (m->target && m->ty < m->y - h * 1.2 && saver_random() < 0.35 &&
					start_hook(s, m)) {
				// a wall on the way up to the spot: over it by rope
			} else if (m->target && m->ty < m->y - h * 1.2 && soil_at(s, m->x, m->y - h * 1.6) &&
					fabs(m->tx - m->x) < h * 6) {
				start_build(s, m, m->ty + h * 0.9); // the spot is up in the window overhead
			}
		}
		break;
	}
	case MAN_DIG: {
		// level, the hole ends at the feet, so the floor stays; down, it goes below them
		double cx = m->x + m->dx * h * 0.35;
		double cy = m->y - h * (m->dy > 0.3 ? 0.5 - m->dy * 0.55 : 0.56) + m->dy * h * 0.3;
		int taken = dig(s, cx, cy, h * 0.56);
		// the pick bites, then the miner follows into the hole
		double nx = m->x + m->dx * speed * 0.5 * dt, ny = m->y + m->dy * speed * 0.5 * dt;
		if (body_free(s, nx, ny) || (m->dy > 0 && !rock_at(s, nx, ny))) {
			m->x = saver_clamp(nx, h * 0.3, s->width - h * 0.3);
			m->y = fmin(ny, s->height);
		}
		m->timer -= dt;
		if (m->tnt && m->cooldown <= 0 && m->run <= 0 && grounded(s, m)) {
			m->state = MAN_PLANT; // the fuse is ready: dynamite does this faster
			m->timer = 0.9;
			break;
		}
		// at the spot it dug for: often something is there
		if (m->target && hypot(cx - m->tx, cy - m->ty) < h * 0.7) {
			m->target = false;
			if (saver_random() < 0.6) {
				find_something(s, m);
				break;
			}
			m->state = grounded(s, m) ? MAN_WALK : MAN_FALL;
			break;
		}
		if (taken && saver_random() < dt * 0.12) {
			find_something(s, m);
			break;
		}
		// through the window, out of the soil, or tired of it
		bool in_soil = soil_at(s, cx + m->dx * h * 0.6, cy + m->dy * h * 0.6);
		if (m->timer <= 0 || (!in_soil && !taken && m->timer < 6.5)) {
			m->state = grounded(s, m) ? MAN_WALK : MAN_FALL;
			if (m->dx != 0) {
				m->dir = m->dx > 0 ? 1 : -1;
			}
			// finished down there and the next spot is up: stairs, shared if there are some
			if (m->state == MAN_WALK && m->target && m->ty < m->y - h * 1.5) {
				start_build(s, m, m->ty + h * 0.9);
			}
		}
		break;
	}
	case MAN_BUILD:
		// hammering the next plank, then up onto it
		m->timer += dt;
		if (m->timer >= 0.4) {
			m->timer = 0;
			build_step(s, m);
			if (m->y <= m->top + s->cell * 0.5 || m->steps > 120) {
				m->state = MAN_WALK; // up there: on to what it came for
				if (m->stair >= 0) {
					s->stairs[m->stair].building = false;
				}
				m->stair = -1;
			}
		}
		break;
	case MAN_PLANT:
		m->timer -= dt;
		if (m->timer <= 0) {
			plant_bomb(s, m);
		}
		break;
	case MAN_THROW:
		// the hook flies up in an arc and catches on the edge
		m->timer += dt;
		if (m->timer >= 0.7) {
			m->state = MAN_CLIMB;
			m->timer = 0;
		}
		break;
	case MAN_CLIMB: {
		if (!solid_at(s, m->hx, m->hy + s->cell)) {
			m->state = MAN_FALL; // the ledge is gone, and the hook with it
			break;
		}
		m->timer += dt;
		double ny = m->y - h * 2.2 * dt; // hand over hand
		if (ny <= m->hy + h * 0.15 || solid_at(s, m->x, ny - h * 0.95)) {
			// up: a swing onto the ledge
			if (body_free(s, m->ledge_x, m->hy)) {
				m->x = m->ledge_x;
				m->y = m->hy;
				m->state = MAN_WALK;
				m->think = saver_between(0.3, 0.8);
			} else {
				m->state = MAN_FALL;
			}
			break;
		}
		m->y = ny;
		break;
	}
	case MAN_CHEER:
		m->timer -= dt;
		if (m->timer <= 0) {
			// the find goes up and away, and the miner goes on looking
			for (int i = 0; i < ITEMS_MAX; i++) {
				struct item *it = &s->items[i];
				if (!it->alive) {
					*it = (struct item){ m->x, m->y - h * 1.35, -s->u * 60, 1.6, m->carry,
						m->letter, true };
					break;
				}
			}
			m->carry = -1;
			m->state = grounded(s, m) ? MAN_WALK : MAN_FALL;
		}
		break;
	}
}

/* ---------- drawing ---------- */

static void draw_item(cairo_t *cr, double x, double y, double size, int kind, char letter,
		double alpha) {
	double s = size;
	switch (kind) {
	case ITEM_PAGE:
		cairo_rectangle(cr, x - s * 0.35, y - s * 0.45, s * 0.7, s * 0.9);
		cairo_set_source_rgba(cr, 1, 1, 1, alpha);
		cairo_fill(cr);
		for (int i = 0; i < 4; i++) {
			cairo_rectangle(cr, x - s * 0.25, y - s * 0.3 + i * s * 0.18, s * (i == 3 ? 0.3 : 0.5),
				s * 0.06);
			cairo_set_source_rgba(cr, 0.4, 0.5, 0.7, alpha);
			cairo_fill(cr);
		}
		break;
	case ITEM_LETTER: {
		cairo_rectangle(cr, x - s * 0.42, y - s * 0.42, s * 0.84, s * 0.84);
		cairo_set_source_rgba(cr, 0.96, 0.88, 0.62, alpha);
		cairo_fill(cr);
		char text[2] = { letter, 0 };
		cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, s * 0.7);
		cairo_text_extents_t e;
		cairo_text_extents(cr, text, &e);
		cairo_move_to(cr, x - e.width / 2 - e.x_bearing, y - e.height / 2 - e.y_bearing);
		cairo_set_source_rgba(cr, 0.2, 0.12, 0.05, alpha);
		cairo_show_text(cr, text);
		break;
	}
	case ITEM_GEM:
		cairo_move_to(cr, x, y + s * 0.45);
		cairo_line_to(cr, x - s * 0.45, y - s * 0.1);
		cairo_line_to(cr, x - s * 0.25, y - s * 0.4);
		cairo_line_to(cr, x + s * 0.25, y - s * 0.4);
		cairo_line_to(cr, x + s * 0.45, y - s * 0.1);
		cairo_close_path(cr);
		saver_set_hsva(cr, 0.5 + letter * 0.037, 0.7, 1, alpha);
		cairo_fill(cr);
		cairo_move_to(cr, x - s * 0.2, y - s * 0.25);
		cairo_line_to(cr, x, y - s * 0.3);
		cairo_set_source_rgba(cr, 1, 1, 1, alpha * 0.8);
		cairo_set_line_width(cr, s * 0.08);
		cairo_stroke(cr);
		break;
	case ITEM_FOLDER:
		cairo_rectangle(cr, x - s * 0.45, y - s * 0.3, s * 0.35, s * 0.15);
		cairo_rectangle(cr, x - s * 0.45, y - s * 0.2, s * 0.9, s * 0.6);
		cairo_set_source_rgba(cr, 0.98, 0.8, 0.3, alpha);
		cairo_fill(cr);
		break;
	case ITEM_NOTE:
		cairo_arc(cr, x - s * 0.12, y + s * 0.25, s * 0.17, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 0.35, 0.8, 1, alpha);
		cairo_fill(cr);
		cairo_rectangle(cr, x + s * 0.02, y - s * 0.4, s * 0.08, s * 0.65);
		cairo_rectangle(cr, x + s * 0.02, y - s * 0.4, s * 0.3, s * 0.1);
		cairo_fill(cr);
		break;
	default:
		cairo_rectangle(cr, x - s * 0.45, y - s * 0.35, s * 0.9, s * 0.7);
		cairo_set_source_rgba(cr, 0.55, 0.8, 1, alpha);
		cairo_fill(cr);
		cairo_move_to(cr, x - s * 0.45, y + s * 0.35);
		cairo_line_to(cr, x - s * 0.1, y - s * 0.05);
		cairo_line_to(cr, x + s * 0.15, y + s * 0.15);
		cairo_line_to(cr, x + s * 0.45, y - s * 0.1);
		cairo_line_to(cr, x + s * 0.45, y + s * 0.35);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 0.25, 0.6, 0.3, alpha);
		cairo_fill(cr);
		break;
	}
}

static void line(cairo_t *cr, double x0, double y0, double x1, double y1) {
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
	cairo_stroke(cr);
}

/* A bundle of three sticks of dynamite, with a fuse that may be burning. */
static void draw_dynamite(cairo_t *cr, double x, double y, double size, double fuse,
		double phase) {
	for (int k = -1; k <= 1; k++) {
		cairo_rectangle(cr, x + k * size * 0.2 - size * 0.09, y - size * 0.5, size * 0.18, size);
		cairo_set_source_rgb(cr, 0.82, 0.12, 0.08);
		cairo_fill(cr);
	}
	cairo_rectangle(cr, x - size * 0.32, y - size * 0.08, size * 0.64, size * 0.16);
	cairo_set_source_rgb(cr, 0.95, 0.9, 0.8);
	cairo_fill(cr);
	cairo_move_to(cr, x, y - size * 0.5);
	cairo_curve_to(cr, x + size * 0.2, y - size * 0.7, x - size * 0.1, y - size * 0.8,
		x + size * 0.15, y - size * 0.95);
	cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
	cairo_set_line_width(cr, fmax(1, size * 0.05));
	cairo_stroke(cr);
	if (fuse > 0) {
		// sparks at the end of the fuse
		for (int k = 0; k < 5; k++) {
			double a = phase * 23 + k * 1.3, r = size * (0.08 + 0.1 * fabs(sin(phase * 17 + k)));
			cairo_arc(cr, x + size * 0.15 + cos(a) * r, y - size * 0.95 + sin(a) * r,
				fmax(0.8, size * 0.035), 0, 2 * M_PI);
			cairo_set_source_rgb(cr, 1, 0.6 + 0.4 * (k % 2), 0.1);
			cairo_fill(cr);
		}
	}
}

/* A striped canopy over the miner, its lines down to his shoulders. */
static void draw_chute(struct diggers *s, cairo_t *cr, struct man *m) {
	double h = s->h, open = m->chute, x = m->x + sin(m->phase * 1.8) * h * 0.08;
	double top = m->y - h * 1.95, w = h * (0.45 + 0.8 * open);
	cairo_set_line_width(cr, fmax(0.8, s->u * 0.9));
	cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.8);
	for (int k = -2; k <= 2; k++) {
		cairo_move_to(cr, x + k * w / 2.2, top + h * 0.3);
		cairo_line_to(cr, m->x + k * h * 0.04, m->y - h * 0.68);
	}
	cairo_stroke(cr);
	// the canopy in panels of two colors
	for (int k = 0; k < 6; k++) {
		double a0 = M_PI + k * M_PI / 6, a1 = a0 + M_PI / 6;
		cairo_move_to(cr, x, top + h * 0.3);
		cairo_arc(cr, x, top + h * 0.3, w, a0, a1);
		cairo_close_path(cr);
		if (k % 2) {
			cairo_set_source_rgb(cr, 0.96, 0.96, 0.94);
		} else {
			saver_set_hsva(cr, m->tnt ? 0 : 0.05 + m->hue * 0.8, 0.75, 0.95, 1);
		}
		cairo_fill(cr);
	}
	cairo_arc(cr, x, top + h * 0.3, w, M_PI, 2 * M_PI);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
	cairo_stroke(cr);
}

/* A grappling hook: a shank and three prongs over the edge. */
static void draw_hook(struct diggers *s, cairo_t *cr, double x, double y, int dir) {
	double r = s->h * 0.12;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, fmax(1.2, s->u * 1.6));
	cairo_set_source_rgb(cr, 0.55, 0.57, 0.62);
	cairo_move_to(cr, x, y - r * 0.3);
	cairo_line_to(cr, x, y + r * 1.2);
	cairo_stroke(cr);
	for (int k = -1; k <= 1; k++) {
		cairo_arc(cr, x + k * r * 0.55, y - r * 0.3, r * 0.45, 0, M_PI);
		cairo_stroke(cr);
	}
	(void)dir;
}

/* The rope from the hook down to the miner's hands, sagging a little while it flies. */
static void draw_rope(struct diggers *s, cairo_t *cr, double x0, double y0, double x1,
		double y1, double sag) {
	cairo_set_line_width(cr, fmax(1, s->u * 1.1));
	cairo_set_source_rgb(cr, 0.78, 0.66, 0.42);
	cairo_move_to(cr, x0, y0);
	cairo_curve_to(cr, x0 + (x1 - x0) * 0.3 + sag, y0 + (y1 - y0) * 0.3,
		x0 + (x1 - x0) * 0.7 + sag, y0 + (y1 - y0) * 0.7, x1, y1);
	cairo_stroke(cr);
}

static void draw_man(struct diggers *s, cairo_t *cr, struct man *m) {
	double h = s->h, x = m->x, y = m->y, d = m->dir;
	if (m->state == MAN_THROW) {
		// the hook on its way up, in an arc, the rope paying out behind it
		double t = saver_clamp(m->timer / 0.7, 0, 1);
		double sx = x + d * h * 0.15, sy = y - h * 0.9;
		double hx = sx + (m->hx - sx) * t, hy = sy + (m->hy - sy) * t - sin(t * M_PI) * h * 1.2;
		draw_rope(s, cr, sx, sy, hx, hy, -d * h * 0.4 * (1 - t));
		draw_hook(s, cr, hx, hy, m->dir);
	} else if (m->state == MAN_CLIMB) {
		draw_rope(s, cr, m->hx, m->hy + h * 0.05, x, y + h * 0.4, 0);
		draw_hook(s, cr, m->hx, m->hy, m->dir);
	}
	if (m->state == MAN_FALL && m->chute > 0) {
		draw_chute(s, cr, m);
	}
	bool walking = m->state == MAN_WALK;
	double swing = walking ? sin(m->phase * 14) : 0;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	// legs
	cairo_set_line_width(cr, h * 0.11);
	cairo_set_source_rgb(cr, 0.16, 0.2, 0.45);
	double hip = y - h * 0.4;
	line(cr, x - h * 0.05, hip, x - h * 0.05 + swing * h * 0.14, y - h * 0.03);
	line(cr, x + h * 0.05, hip, x + h * 0.05 - swing * h * 0.14, y - h * 0.03);
	cairo_set_source_rgb(cr, 0.25, 0.16, 0.1);
	cairo_arc(cr, x - h * 0.05 + swing * h * 0.14 + d * h * 0.03, y - h * 0.03, h * 0.06, 0,
		2 * M_PI);
	cairo_arc(cr, x + h * 0.05 - swing * h * 0.14 + d * h * 0.03, y - h * 0.03, h * 0.06, 0,
		2 * M_PI);
	cairo_fill(cr);
	if (m->tnt && m->state != MAN_PLANT) {
		draw_dynamite(cr, x - d * h * 0.2, y - h * 0.58, h * 0.3, 0, 0); // on his back
	}
	// the overalls
	cairo_rectangle(cr, x - h * 0.16, y - h * 0.72, h * 0.32, h * 0.34);
	saver_set_hsva(cr, 0.6 + m->hue * 0.12, 0.7, 0.85, 1);
	cairo_fill(cr);
	// the head, the helmet and its lamp
	double hy = y - h * 0.84;
	cairo_arc(cr, x, hy, h * 0.15, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 0.8, 0.62);
	cairo_fill(cr);
	cairo_arc(cr, x + d * h * 0.07, hy, h * 0.03, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_fill(cr);
	cairo_arc(cr, x, hy - h * 0.02, h * 0.17, M_PI, 2 * M_PI);
	cairo_close_path(cr);
	if (m->tnt) {
		cairo_set_source_rgb(cr, 0.85, 0.16, 0.1); // the blaster wears red
	} else {
		cairo_set_source_rgb(cr, 1, 0.82, 0.1);
	}
	cairo_fill(cr);
	cairo_arc(cr, x + d * h * 0.13, hy - h * 0.08, h * 0.045, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 1, 0.8);
	cairo_fill(cr);
	if (m->state == MAN_DIG) {
		// a beam from the lamp into the dark
		double bx = x + d * h * 0.13, by = hy - h * 0.08;
		cairo_move_to(cr, bx, by);
		cairo_line_to(cr, bx + m->dx * h * 0.9 - m->dy * h * 0.25,
			by + m->dy * h * 0.9 + fabs(m->dx) * h * 0.25);
		cairo_line_to(cr, bx + m->dx * h * 0.9 + m->dy * h * 0.25,
			by + m->dy * h * 0.9 - fabs(m->dx) * h * 0.25);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 1, 1, 0.7, 0.18);
		cairo_fill(cr);
	}
	// arms: swinging the pick, holding a find up high, flailing in a fall
	cairo_set_line_width(cr, h * 0.09);
	double sx = x, sy = y - h * 0.66;
	if (m->state == MAN_CHEER) {
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		line(cr, sx - h * 0.1, sy, sx - h * 0.18, sy - h * 0.42);
		line(cr, sx + h * 0.1, sy, sx + h * 0.18, sy - h * 0.42);
		draw_item(cr, x, y - h * 1.35, h * 0.55, m->carry, m->letter, 1);
		return;
	}
	if (m->state == MAN_CLIMB) {
		// hand over hand on the rope
		double pull = sin(m->timer * 9) * h * 0.1;
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		line(cr, sx - h * 0.08, sy, x - h * 0.03, sy - h * 0.3 + pull);
		line(cr, sx + h * 0.08, sy, x + h * 0.03, sy - h * 0.3 - pull);
		return;
	}
	if (m->state == MAN_THROW) {
		// the throwing arm up
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		double a = -M_PI / 2 + d * (0.9 - saver_clamp(m->timer / 0.25, 0, 1) * 0.9);
		line(cr, sx, sy, sx + cos(a) * h * 0.32, sy + sin(a) * h * 0.32);
		return;
	}
	if (m->state == MAN_PLANT) {
		// down on one knee, setting the bundle against the window
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		line(cr, sx, sy, sx + d * h * 0.3, sy + h * 0.28);
		draw_dynamite(cr, x + d * h * 0.35, y - h * 0.2, h * 0.3, 0, 0);
		return;
	}
	if (m->state == MAN_FALL && m->chute > 0.4) {
		// holding on to the lines
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		line(cr, sx - h * 0.1, sy, sx - h * 0.14, sy - h * 0.3);
		line(cr, sx + h * 0.1, sy, sx + h * 0.14, sy - h * 0.3);
		return;
	}
	if (m->state == MAN_FALL || m->run > 0) {
		// arms up: thrown by a blast, or running from one
		cairo_set_source_rgb(cr, 1, 0.8, 0.62);
		double wave = sin(m->phase * 20) * h * 0.08;
		line(cr, sx - h * 0.1, sy, sx - h * 0.3, sy - h * 0.25 + wave);
		line(cr, sx + h * 0.1, sy, sx + h * 0.3, sy - h * 0.25 - wave);
		return;
	}
	bool building = m->state == MAN_BUILD;
	double angle = m->state == MAN_DIG ? -0.9 + 1.3 * (0.5 + 0.5 * sin(m->phase * 11)) :
		building ? -0.3 + 1.2 * (0.5 + 0.5 * sin(m->phase * 17)) : 0.9 + swing * 0.3;
	double ax = m->state == MAN_DIG && m->dx == 0 ? 1 : (m->state == MAN_DIG ? m->dx : d);
	double hx = sx + ax * cos(angle) * h * 0.3, hy2 = sy + sin(angle) * h * 0.3;
	cairo_set_source_rgb(cr, 1, 0.8, 0.62);
	line(cr, sx, sy, hx, hy2);
	if (building) {
		// a hammer on the next plank, and the planks under the other arm
		double px = hx + ax * cos(angle - 0.2) * h * 0.24, py = hy2 + sin(angle - 0.2) * h * 0.24;
		cairo_set_line_width(cr, h * 0.05);
		cairo_set_source_rgb(cr, 0.55, 0.36, 0.18);
		line(cr, hx, hy2, px, py);
		double nx = -(py - hy2), ny = px - hx, nl = hypot(nx, ny);
		if (nl > 0) {
			cairo_set_line_width(cr, h * 0.09);
			cairo_set_source_rgb(cr, 0.35, 0.36, 0.4);
			line(cr, px - nx / nl * h * 0.08, py - ny / nl * h * 0.08, px + nx / nl * h * 0.08,
				py + ny / nl * h * 0.08);
		}
		cairo_rectangle(cr, x - d * h * 0.3, y - h * 0.62, h * 0.3, h * 0.12);
		cairo_set_source_rgb(cr, 0.72, 0.5, 0.26);
		cairo_fill(cr);
		return;
	}
	// the pick: a handle and a curved iron head
	double px = hx + ax * cos(angle - 0.4) * h * 0.35, py = hy2 + sin(angle - 0.4) * h * 0.35;
	cairo_set_line_width(cr, h * 0.06);
	cairo_set_source_rgb(cr, 0.55, 0.36, 0.18);
	line(cr, hx, hy2, px, py);
	double nx = -(py - hy2), ny = px - hx, nl = hypot(nx, ny);
	if (nl > 0) {
		nx /= nl;
		ny /= nl;
		cairo_set_line_width(cr, h * 0.07);
		cairo_set_source_rgb(cr, 0.7, 0.72, 0.76);
		line(cr, px - nx * h * 0.17, py - ny * h * 0.17, px + nx * h * 0.17, py + ny * h * 0.17);
	}
}

static void draw_window(struct diggers *s, cairo_t *cr, int i) {
	double u = s->u;
	{
		struct saver_window *b = &s->windows[i];
		double bar = u * 26;
		cairo_rectangle(cr, b->x, b->y, b->w, b->h);
		cairo_set_source_rgb(cr, 0.96, 0.96, 0.97);
		cairo_fill(cr);
		cairo_rectangle(cr, b->x, b->y, b->w, bar);
		cairo_pattern_t *title = cairo_pattern_create_linear(0, b->y, 0, b->y + bar);
		cairo_pattern_add_color_stop_rgb(title, 0, 0.2, 0.45, 0.85);
		cairo_pattern_add_color_stop_rgb(title, 1, 0.1, 0.3, 0.7);
		cairo_set_source(cr, title);
		cairo_fill(cr);
		cairo_pattern_destroy(title);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, u * 13);
		cairo_move_to(cr, b->x + u * 8, b->y + bar * 0.7);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_show_text(cr, b->title);
		for (double ly = b->y + bar + u * 14; ly < b->y + b->h - u * 10; ly += u * 16) {
			double lw = (b->w - u * 24) * (0.4 + 0.5 * fabs(sin(ly * 0.37 + i)));
			cairo_rectangle(cr, b->x + u * 12, ly, lw, u * 5);
			cairo_set_source_rgb(cr, 0.72, 0.75, 0.8);
			cairo_fill(cr);
		}
		cairo_rectangle(cr, b->x + 0.5, b->y + 0.5, b->w - 1, b->h - 1);
		cairo_set_source_rgb(cr, 0.3, 0.35, 0.45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
}

static void draw_windows(struct diggers *s, cairo_t *cr) {
	for (int i = 0; i < s->window_count; i++) {
		draw_window(s, cr, i);
	}
}

/* ---------- the renovation: the drilling machine and the helicopters ---------- */

static double drill_length(struct diggers *s) {
	return s->h * 9; // the cutter and the body behind it
}

static void start_renovation(struct diggers *s) {
	s->reno = RENO_DRILL;
	s->drill_dir = saver_random() < 0.5 ? 1 : -1;
	s->drill_x = s->drill_dir > 0 ? 0 : s->width;
	for (int i = 0; i < BOMBS_MAX; i++) {
		s->bombs[i].alive = false;
	}
}

/* Everything behind the tip: soil ground away, stairs torn down, miners thrown off. */
static void drill_step(struct diggers *s, double dt) {
	int d = s->drill_dir;
	double from = s->drill_x;
	s->drill_x += d * (s->width + drill_length(s)) / DRILL_TIME * dt;
	double x0 = fmin(from, s->drill_x), x1 = fmax(from, s->drill_x);
	int taken = 0;
	int cx0 = (int)floor(x0 / s->cell), cx1 = (int)ceil(x1 / s->cell);
	for (int cx = cx0 < 0 ? 0 : cx0; cx < cx1 && cx < s->gw; cx++) {
		for (int cy = 0; cy < s->gh; cy++) {
			int k = cell_index(s, cx, cy);
			if (s->soil[k] && !s->dug[k]) {
				s->dug[k] = 1;
				taken++;
			}
		}
	}
	if (taken) {
		s->dug_cells += taken;
		cairo_surface_t *masks[3] = { s->mask_hole, s->mask_band, s->mask_line };
		for (int i = 0; i < 3; i++) {
			cairo_t *mc = cairo_create(masks[i]);
			cairo_rectangle(mc, floor(x0) - 1, 0, ceil(x1) - floor(x0) + 2, s->height);
			cairo_fill(mc);
			cairo_destroy(mc);
		}
		redraw_tunnels(s, floor(x0) - 2, 0, ceil(x1) - floor(x0) + 4, s->height);
		// the dirt flies from the teeth where they bite
		for (int n = 0; n < 4; n++) {
			double y = saver_random() * s->height;
			if (s->soil[cell_index(s, (int)saver_clamp(s->drill_x / s->cell, 0, s->gw - 1),
					(int)saver_clamp(y / s->cell, 0, s->gh - 1))]) {
				dirt_spray(s, s->drill_x, y, 2);
			}
		}
	}
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *st = &s->stairs[i];
		for (int k = 0; st->alive && k < st->count; k++) {
			double px = (st->planks[k].x0 + st->planks[k].x1) / 2;
			if ((px - s->drill_x) * d < 0) {
				remove_stair(s, i, true);
				break;
			}
		}
	}
	for (int i = 0; i < s->men_count; i++) {
		struct man *m = &s->men[i];
		if (!m->flung && (m->x - s->drill_x) * d < s->h * 0.3) {
			m->flung = true;
			m->state = MAN_FALL;
			m->carry = -1;
			m->chute = 0;
			m->vy = -s->u * saver_between(450, 750);
			m->vx = d * s->u * saver_between(350, 600);
		}
	}
	if ((s->drill_x - d * drill_length(s)) * d > (d > 0 ? s->width : 0) * d) {
		// through and out on the other side: the windows come back in pieces
		s->reno = RENO_HELIS;
		s->slab_count = s->slab_next = s->slabs_placed = 0;
		for (int i = 0; i < s->window_count; i++) {
			struct saver_window *b = &s->windows[i];
			int n = (int)saver_clamp(ceil(b->h / (s->h * 4)), 1, 8);
			for (int k = n - 1; k >= 0 && s->slab_count < SLABS_MAX; k--) {
				// from the bottom up, each piece resting on the last
				s->slabs[s->slab_count++] = (struct slab){ i, b->x, b->y + b->h * k / n, b->w,
					b->h / n };
			}
		}
		s->heli_wait = 0.5;
	}
}

/* A slab set into its place: that band of the window is whole again. */
static void place_slab(struct diggers *s, struct slab *sl) {
	int x0 = (int)floor(sl->x / s->cell), x1 = (int)ceil((sl->x + sl->w) / s->cell);
	int y0 = (int)floor(sl->y / s->cell), y1 = (int)ceil((sl->y + sl->h) / s->cell);
	for (int cy = y0 < 0 ? 0 : y0; cy < y1 && cy < s->gh; cy++) {
		for (int cx = x0 < 0 ? 0 : x0; cx < x1 && cx < s->gw; cx++) {
			int k = cell_index(s, cx, cy);
			if (s->soil[k] && s->dug[k]) {
				s->dug[k] = 0;
				s->dug_cells--;
			}
		}
	}
	cairo_surface_t *masks[3] = { s->mask_hole, s->mask_band, s->mask_line };
	for (int i = 0; i < 3; i++) {
		cairo_t *mc = cairo_create(masks[i]);
		cairo_set_operator(mc, CAIRO_OPERATOR_CLEAR);
		cairo_rectangle(mc, round(sl->x), round(sl->y), round(sl->w), ceil(sl->h));
		cairo_fill(mc);
		cairo_destroy(mc);
	}
	redraw_tunnels(s, sl->x - 2, sl->y - 2, sl->w + 4, sl->h + 4);
	dirt_spray(s, sl->x + sl->w * 0.2, sl->y + sl->h, 3); // a puff of dust as it sets down
	dirt_spray(s, sl->x + sl->w * 0.8, sl->y + sl->h, 3);
}

/* How far under the helicopter the slab hangs. */
static double cable(struct diggers *s) {
	return s->h * 1.6;
}

static void helis_step(struct diggers *s, double dt) {
	double speed = s->width / 3.2;
	s->heli_wait -= dt;
	for (int i = 0; i < HELIS_MAX && s->heli_wait <= 0 && s->slab_next < s->slab_count; i++) {
		struct heli *hl = &s->helis[i];
		if (hl->alive) {
			continue;
		}
		struct slab *sl = &s->slabs[s->slab_next];
		*hl = (struct heli){ .alive = true, .phase = HELI_IN, .slab = s->slab_next++ };
		double tx = sl->x + sl->w / 2, ty = sl->y - cable(s) - s->h * 2;
		// in from the nearer side, high up, and down to just above the spot
		hl->dir = tx < s->width / 2 ? 1 : -1;
		hl->x0 = hl->dir > 0 ? -sl->w / 2 - s->h * 3 : s->width + sl->w / 2 + s->h * 3;
		hl->y0 = fmin(ty, s->h * 1.5) - s->h * 2;
		hl->x = hl->x0;
		hl->y = hl->y0;
		hl->time = fmax(1.2, fabs(tx - hl->x0) / speed);
		s->heli_wait = saver_between(0.5, 0.9);
	}
	for (int i = 0; i < HELIS_MAX; i++) {
		struct heli *hl = &s->helis[i];
		if (!hl->alive) {
			continue;
		}
		struct slab *sl = &s->slabs[hl->slab];
		double tx = sl->x + sl->w / 2, ty = sl->y - cable(s);
		hl->t += dt;
		double f = saver_clamp(hl->t / hl->time, 0, 1), ease = f * f * (3 - 2 * f);
		switch (hl->phase) {
		case HELI_IN:
			hl->x = hl->x0 + (tx - hl->x0) * ease;
			hl->y = hl->y0 + (ty - s->h * 2 - hl->y0) * ease;
			if (f >= 1) {
				hl->phase = HELI_LOWER;
				hl->t = 0;
				hl->time = 0.9;
			}
			break;
		case HELI_LOWER:
			hl->y = ty - s->h * 2 * (1 - ease);
			if (f >= 1) {
				place_slab(s, sl);
				s->slabs_placed++;
				hl->phase = HELI_OUT;
				hl->t = 0;
				hl->x0 = hl->x;
				hl->y0 = hl->y;
			}
			break;
		case HELI_OUT:
			// up and away the way it was going
			hl->x = hl->x0 + hl->dir * speed * hl->t * hl->t * 0.8;
			hl->y = hl->y0 - s->h * 3 * hl->t;
			if (hl->x < -s->h * 4 || hl->x > s->width + s->h * 4) {
				hl->alive = false;
			}
			break;
		}
	}
	bool flying = false;
	for (int i = 0; i < HELIS_MAX; i++) {
		flying |= s->helis[i].alive;
	}
	if (s->slabs_placed >= s->slab_count && !flying) {
		// all whole again: a fresh start for the miners, who come back one by one
		s->reno = RENO_NONE;
		s->age = 0;
		build_soil(s);
		s->spawn = 1;
	}
}

static void draw_drill(struct diggers *s, cairo_t *cr) {
	double h = s->h, d = s->drill_dir, tip = s->drill_x, H = s->height;
	double head = h * 2.2, body = drill_length(s) - head;
	double back = tip - d * head; // where the cutter meets the body
	// the body: a big yellow machine with hazard stripes and rivets
	double bx = d > 0 ? back - body : back;
	cairo_rectangle(cr, bx, h * 0.6, body, H - h * 1.2);
	cairo_set_source_rgb(cr, 0.93, 0.72, 0.1);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.3, 0.22, 0.05);
	cairo_set_line_width(cr, fmax(1.5, s->u * 3));
	cairo_stroke(cr);
	for (int band = 0; band < 2; band++) {
		double by = band == 0 ? h * 0.9 : H - h * 1.5;
		cairo_save(cr);
		cairo_rectangle(cr, bx, by, body, h * 0.6);
		cairo_clip(cr);
		cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 0.98, 0.8, 0.1);
		for (double x = bx - h; x < bx + body + h; x += h * 0.7) {
			cairo_move_to(cr, x, by);
			cairo_line_to(cr, x + h * 0.35, by);
			cairo_line_to(cr, x + h * 0.95, by + h * 0.6);
			cairo_line_to(cr, x + h * 0.6, by + h * 0.6);
			cairo_close_path(cr);
		}
		cairo_fill(cr);
		cairo_restore(cr);
	}
	// panels with rivets, and a porthole with the driver in it
	for (double y = h * 2; y < H - h * 2; y += h * 2.5) {
		for (int k = 0; k < 3; k++) {
			cairo_arc(cr, bx + body * (0.2 + 0.3 * k), y, s->u * 3, 0, 2 * M_PI);
			cairo_set_source_rgb(cr, 0.55, 0.42, 0.08);
			cairo_fill(cr);
		}
	}
	double px = back - d * body * 0.35, py = H * 0.4;
	cairo_arc(cr, px, py, h * 0.7, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.3, 0.3, 0.32);
	cairo_fill(cr);
	cairo_arc(cr, px, py, h * 0.55, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.55, 0.8, 0.95);
	cairo_fill(cr);
	cairo_arc(cr, px, py + h * 0.1, h * 0.22, 0, 2 * M_PI); // the driver, in a helmet
	cairo_set_source_rgb(cr, 0.95, 0.78, 0.6);
	cairo_fill(cr);
	cairo_arc(cr, px, py - h * 0.05, h * 0.24, M_PI, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 0.85, 0.1);
	cairo_fill(cr);
	// the cutter: a steel drum with teeth that run round and round
	cairo_rectangle(cr, fmin(back, tip - d * h * 0.6), 0, head - h * 0.6, H);
	cairo_pattern_t *steel = cairo_pattern_create_linear(back, 0, tip, 0);
	cairo_pattern_add_color_stop_rgb(steel, 0, 0.35, 0.36, 0.4);
	cairo_pattern_add_color_stop_rgb(steel, 0.5, 0.75, 0.77, 0.8);
	cairo_pattern_add_color_stop_rgb(steel, 1, 0.45, 0.46, 0.5);
	cairo_set_source(cr, steel);
	cairo_fill(cr);
	cairo_pattern_destroy(steel);
	double pitch = h * 0.55, shift = fmod(s->clock * h * 6, pitch);
	cairo_set_line_width(cr, fmax(1, s->u * 2));
	for (double y = -pitch + shift; y < H + pitch; y += pitch) {
		// a groove across the drum, and a tooth sticking out in front
		double gx = tip - d * h * 0.6;
		cairo_move_to(cr, back, y + pitch * 0.4);
		cairo_line_to(cr, gx, y);
		cairo_set_source_rgba(cr, 0.2, 0.2, 0.24, 0.8);
		cairo_stroke(cr);
		cairo_move_to(cr, gx, y - pitch * 0.35);
		cairo_line_to(cr, tip, y);
		cairo_line_to(cr, gx, y + pitch * 0.35);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, 0.82, 0.84, 0.88);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0.25, 0.25, 0.3);
		cairo_stroke(cr);
	}
	// dust in front of it
	for (int k = 0; k < 10; k++) {
		double y = fmod(k * 0.137 * H + s->clock * h * 2 * (k % 3 + 1), H);
		double r = h * (0.4 + 0.3 * sin(s->clock * 3 + k));
		cairo_arc(cr, tip + d * h * 0.3, y, r, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, 0.6, 0.5, 0.38, 0.35);
		cairo_fill(cr);
	}
}

/* Whether the picture of the screen shows that window where it is now. */
static bool window_pictured(struct diggers *s, int i) {
	struct saver_window *b = &s->windows[i];
	for (int k = 0; s->desktop && k < s->pictured_count; k++) {
		struct saver_window *p = &s->pictured[k];
		if (p->x == b->x && p->y == b->y && p->w == b->w && p->h == b->h) {
			return true;
		}
	}
	return false;
}

static void draw_heli(struct diggers *s, cairo_t *cr, struct heli *hl) {
	double k = s->h * 1.35, x = hl->x, y = hl->y, d = hl->dir;
	struct slab *sl = &s->slabs[hl->slab];
	if (hl->phase != HELI_OUT) {
		// the slab on its cables: the piece of the window as it will be
		double sx = x - sl->w / 2, sy = y + cable(s);
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.22);
		cairo_set_line_width(cr, fmax(1, s->u * 1.5));
		cairo_move_to(cr, x, y + k * 0.62);
		cairo_line_to(cr, sx + s->u * 4, sy);
		cairo_move_to(cr, x, y + k * 0.62);
		cairo_line_to(cr, sx + sl->w - s->u * 4, sy);
		cairo_stroke(cr);
		cairo_save(cr);
		cairo_rectangle(cr, sx, sy, sl->w, sl->h);
		cairo_clip(cr);
		cairo_translate(cr, sx - sl->x, sy - sl->y);
		if (window_pictured(s, sl->window)) {
			// the real piece: just what shows there once it is set down
			cairo_scale(cr, (double)s->width / cairo_image_surface_get_width(s->desktop),
				(double)s->height / cairo_image_surface_get_height(s->desktop));
			cairo_set_source_surface(cr, s->desktop, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
		} else {
			draw_window(s, cr, sl->window); // one opened since: a drawn stand-in
		}
		cairo_restore(cr);
		cairo_rectangle(cr, sx + 0.5, sy + 0.5, sl->w - 1, sl->h - 1);
		cairo_set_source_rgb(cr, 0.3, 0.35, 0.45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	cairo_set_line_width(cr, fmax(1, s->u * 2));
	// skids
	cairo_set_source_rgb(cr, 0.2, 0.2, 0.22);
	cairo_move_to(cr, x - k * 0.6, y + k * 0.62);
	cairo_line_to(cr, x + k * 0.7, y + k * 0.62);
	cairo_move_to(cr, x - k * 0.35, y + k * 0.3);
	cairo_line_to(cr, x - k * 0.4, y + k * 0.62);
	cairo_move_to(cr, x + k * 0.35, y + k * 0.3);
	cairo_line_to(cr, x + k * 0.4, y + k * 0.62);
	cairo_stroke(cr);
	// the tail boom, its fin and the rotor on it
	double tx = x - d * k * 1.9, ty = y - k * 0.2;
	cairo_move_to(cr, x - d * k * 0.5, y - k * 0.12);
	cairo_line_to(cr, tx, ty - k * 0.06);
	cairo_line_to(cr, tx, ty + k * 0.1);
	cairo_line_to(cr, x - d * k * 0.5, y + k * 0.12);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.8, 0.18, 0.12);
	cairo_fill(cr);
	cairo_move_to(cr, tx, ty);
	cairo_line_to(cr, tx - d * k * 0.15, ty - k * 0.4);
	cairo_line_to(cr, tx + d * k * 0.15, ty);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_arc(cr, tx, ty - k * 0.05, k * 0.3, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.3);
	cairo_fill(cr);
	double a = s->clock * 50;
	cairo_move_to(cr, tx + cos(a) * k * 0.3, ty - k * 0.05 + sin(a) * k * 0.3);
	cairo_line_to(cr, tx - cos(a) * k * 0.3, ty - k * 0.05 - sin(a) * k * 0.3);
	cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
	cairo_stroke(cr);
	// the body and the cockpit
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k * 0.9, k * 0.45);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, 0.88, 0.22, 0.14);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.4, 0.08, 0.05);
	cairo_stroke(cr);
	cairo_save(cr);
	cairo_translate(cr, x + d * k * 0.42, y - k * 0.06);
	cairo_scale(cr, k * 0.38, k * 0.28);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, 0.6, 0.85, 0.98);
	cairo_fill(cr);
	// the mast and the main rotor, a blur with a blade going round
	cairo_move_to(cr, x, y - k * 0.42);
	cairo_line_to(cr, x, y - k * 0.62);
	cairo_set_source_rgb(cr, 0.2, 0.2, 0.22);
	cairo_stroke(cr);
	cairo_save(cr);
	cairo_translate(cr, x, y - k * 0.64);
	cairo_scale(cr, k * 1.7, k * 0.09);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgba(cr, 0.3, 0.3, 0.32, 0.3);
	cairo_fill(cr);
	double blade = cos(s->clock * 35) * k * 1.7;
	cairo_move_to(cr, x - blade, y - k * 0.64);
	cairo_line_to(cr, x + blade, y - k * 0.64);
	cairo_set_line_width(cr, fmax(1.5, s->u * 3));
	cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
	cairo_stroke(cr);
}

/* ---------- the saver ---------- */

static void *diggers_create(int width, int height, const struct saver_options *options) {
	struct diggers *s = calloc(1, sizeof(*s));
	s->width = width;
	s->height = height;
	s->u = saver_unit(width, height);
	s->h = fmax(9, s->u * 30);
	s->cell = (int)fmax(2, round(s->u * 3));
	s->gw = width / s->cell + 1;
	s->gh = height / s->cell + 1;
	s->soil = calloc(s->gw * s->gh, 1);
	s->dug = calloc(s->gw * s->gh, 1);
	s->built = calloc(s->gw * s->gh, 1);
	s->rock = calloc(s->gw * s->gh, 1);
	s->tunnels = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->planks = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	s->mask_hole = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
	s->mask_band = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
	s->mask_line = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
	s->pebbles = make_pebbles(s->u);
	snprintf(s->output, sizeof(s->output), "%s", options->output ? options->output : "");
	s->fake = !read_windows(s);
	if (s->fake) {
		make_windows(s);
	} else if (options->desktop) {
		s->desktop = cairo_surface_reference(options->desktop);
		memcpy(s->pictured, s->windows, sizeof(s->pictured));
		s->pictured_count = s->window_count;
	}
	build_soil(s);
	static const double crews[] = { 1, 0.5, 1.7 }, renewals[] = { 120, 300, 0 };
	double crew = crews[saver_choice(options, &saver_diggers, "miners")];
	s->men_max = (int)saver_clamp(width * (double)height / (1920.0 * 1080.0) * 22 * crew, 4,
		MEN_MAX);
	s->dynamite = saver_toggle(options, &saver_diggers, "dynamite");
	s->renew_after = renewals[saver_choice(options, &saver_diggers, "renew")];
	return s;
}

/* The windows moved, opened or closed: dig into what is there now. */
static void diggers_refresh(struct diggers *s) {
	struct saver_window before[WINDOWS_MAX];
	int count = s->window_count;
	memcpy(before, s->windows, sizeof(before));
	if (!read_windows(s)) {
		return;
	}
	bool same = count == s->window_count;
	for (int i = 0; same && i < count; i++) {
		same = before[i].x == s->windows[i].x && before[i].y == s->windows[i].y &&
			before[i].w == s->windows[i].w && before[i].h == s->windows[i].h;
	}
	if (!same) {
		build_soil(s);
		// other windows: whatever the machine and the helicopters were doing is off
		s->reno = RENO_NONE;
		memset(s->helis, 0, sizeof(s->helis));
	}
}

static void diggers_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct diggers *s = state;
	s->age += dt;
	s->clock += dt;
	for (int i = 0; i < STAIRS_MAX; i++) {
		struct stair *st = &s->stairs[i];
		if (st->alive && !st->building && s->clock - st->used > STAIR_ROT) {
			remove_stair(s, i, true);
		}
	}
	if (!s->fake) {
		s->refresh -= dt;
		if (s->refresh <= 0) {
			s->refresh = 3;
			diggers_refresh(s);
		}
	}
	// after a while (two minutes unless set otherwise), or with a third of it dug out,
	// the windows are renewed: the drilling machine clears it all away and the
	// helicopters bring them back
	if (s->reno == RENO_NONE && s->soil_cells && s->renew_after > 0 &&
			(s->dug_cells > s->soil_cells / 3 || s->age > s->renew_after)) {
		start_renovation(s);
	}
	if (s->reno == RENO_DRILL) {
		drill_step(s, dt);
	} else if (s->reno == RENO_HELIS) {
		helis_step(s, dt);
	}
	s->spawn -= dt;
	if (s->reno == RENO_NONE && s->men_count < s->men_max && s->spawn <= 0) {
		s->spawn = saver_between(0.5, 1.4);
		man_place(s, &s->men[s->men_count++]);
	}
	for (int i = 0; i < s->men_count; i++) {
		// small steps, so nobody falls through a thin floor
		double left = dt;
		while (left > 0) {
			double step = fmin(left, 1 / 60.0);
			man_step(s, &s->men[i], step);
			left -= step;
		}
	}
	// those the machine threw off the screen are gone
	for (int i = 0; i < s->men_count; i++) {
		struct man *m = &s->men[i];
		if (m->flung && (m->x < -s->h || m->x > s->width + s->h || m->y > s->height + s->h * 2)) {
			s->men[i--] = s->men[--s->men_count];
		}
	}

	if (s->fake) {
		draw_windows(s, cr);
		for (int i = 0; i < s->bar_count; i++) {
			struct saver_window *b = &s->bars[i];
			cairo_rectangle(cr, b->x, b->y, b->w, b->h);
			cairo_set_source_rgba(cr, 0.1, 0.1, 0.12, 0.92);
			cairo_fill(cr);
		}
	}
	for (int i = 0; i < BOMBS_MAX; i++) {
		struct bomb *b = &s->bombs[i];
		if (b->alive && (b->fuse -= dt) <= 0) {
			explode(s, b);
		}
	}
	cairo_set_source_surface(cr, s->tunnels, 0, 0);
	cairo_paint(cr);
	cairo_set_source_surface(cr, s->planks, 0, 0);
	cairo_paint(cr);
	for (int i = 0; i < FALLING_MAX; i++) {
		struct falling *f = &s->falling[i];
		if (f->life <= 0) {
			continue;
		}
		f->life -= dt;
		f->vy += s->u * 600 * dt;
		f->x += f->vx * dt;
		f->y += f->vy * dt;
		f->angle += f->spin * dt;
		cairo_save(cr);
		cairo_translate(cr, f->x, f->y);
		cairo_rotate(cr, f->angle);
		cairo_rectangle(cr, -f->w / 2, -s->h * 0.05, f->w, s->h * 0.1);
		cairo_set_source_rgba(cr, 0.72, 0.5, 0.26, saver_clamp(f->life, 0, 1));
		cairo_fill(cr);
		cairo_restore(cr);
	}
	for (int i = 0; i < BOMBS_MAX; i++) {
		struct bomb *b = &s->bombs[i];
		if (b->alive) {
			draw_dynamite(cr, b->x, b->y, s->h * 0.34, b->fuse, s->age);
		}
	}
	for (int i = 0; i < DIRT_MAX; i++) {
		struct dirt *d = &s->dirt[i];
		if (d->life <= 0) {
			continue;
		}
		d->life -= dt;
		d->vy += s->u * 500 * dt;
		d->x += d->vx * dt;
		d->y += d->vy * dt;
		cairo_rectangle(cr, d->x, d->y, s->u * 2.5, s->u * 2.5);
		cairo_set_source_rgba(cr, d->shade + 0.15, d->shade, d->shade * 0.6,
			saver_clamp(d->life * 2, 0, 1));
		cairo_fill(cr);
	}
	for (int i = 0; i < s->men_count; i++) {
		draw_man(s, cr, &s->men[i]);
	}
	if (s->reno == RENO_DRILL) {
		draw_drill(s, cr);
	}
	for (int i = 0; i < HELIS_MAX; i++) {
		if (s->helis[i].alive) {
			draw_heli(s, cr, &s->helis[i]);
		}
	}
	for (int i = 0; i < BOMBS_MAX; i++) {
		struct blast *b = &s->blasts[i];
		if (!b->alive) {
			continue;
		}
		b->age += dt;
		if (b->age >= BLAST_TIME) {
			b->alive = false;
			continue;
		}
		double t = b->age;
		// smoke rising and spreading
		for (int k = 0; k < 7; k++) {
			double a = k * 0.9 + 0.3, grow = saver_clamp(t / BLAST_TIME, 0, 1);
			double px = b->x + cos(a) * b->r * (0.3 + 0.6 * grow);
			double py = b->y + sin(a) * b->r * 0.4 - b->r * 0.8 * grow;
			double pr = b->r * (0.35 + 0.35 * grow);
			cairo_arc(cr, px, py, pr, 0, 2 * M_PI);
			double gray = 0.35 + 0.1 * (k % 3);
			cairo_set_source_rgba(cr, gray, gray, gray, 0.5 * (1 - grow));
			cairo_fill(cr);
		}
		// the fireball
		if (t < 0.55) {
			double f = t / 0.55, fr = b->r * (0.5 + 0.6 * f);
			cairo_pattern_t *fire = cairo_pattern_create_radial(b->x, b->y, 0, b->x, b->y, fr);
			cairo_pattern_add_color_stop_rgba(fire, 0, 1, 1, 0.85, 1 - f);
			cairo_pattern_add_color_stop_rgba(fire, 0.4, 1, 0.75, 0.2, 0.9 * (1 - f));
			cairo_pattern_add_color_stop_rgba(fire, 1, 0.9, 0.25, 0.05, 0);
			cairo_arc(cr, b->x, b->y, fr, 0, 2 * M_PI);
			cairo_set_source(cr, fire);
			cairo_fill(cr);
			cairo_pattern_destroy(fire);
		}
		// the shock wave
		if (t < 0.35) {
			cairo_arc(cr, b->x, b->y, b->r * (0.6 + t / 0.35 * 1.2), 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.6 * (1 - t / 0.35));
			cairo_set_line_width(cr, s->u * 4);
			cairo_stroke(cr);
		}
	}
	for (int i = 0; i < ITEMS_MAX; i++) {
		struct item *it = &s->items[i];
		if (!it->alive) {
			continue;
		}
		it->life -= dt;
		it->y += it->vy * dt;
		if (it->life <= 0) {
			it->alive = false;
			continue;
		}
		double alpha = saver_clamp(it->life, 0, 1);
		// a sparkle around what was found, then it floats off
		for (int k = 0; k < 4; k++) {
			double a = k * M_PI / 2 + it->life * 3, r = s->h * (0.55 + (1.6 - it->life) * 0.3);
			cairo_arc(cr, it->x + cos(a) * r, it->y + sin(a) * r, s->u * 2, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 1, 0.95, 0.5, alpha);
			cairo_fill(cr);
		}
		draw_item(cr, it->x, it->y, s->h * 0.6, it->kind, it->letter, alpha);
	}
	// what they found so far, where the taskbar clock would be proud of it
	if (s->found) {
		char text[48];
		snprintf(text, sizeof(text), "Found: %d", s->found);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		double size = fmax(9, s->u * 15);
		cairo_set_font_size(cr, size);
		cairo_text_extents_t ext;
		cairo_text_extents(cr, text, &ext);
		// in a dark pill in the middle at the top, over whatever window is there
		double px = (s->width - ext.x_advance) / 2, py = s->u * 12 + size;
		double pw = ext.x_advance + size * 1.2, ph = size * 1.6;
		double rx = px - size * 0.6, ry = py - size * 1.15, r = ph / 2;
		cairo_new_sub_path(cr);
		cairo_arc(cr, rx + pw - r, ry + r, r, -M_PI / 2, M_PI / 2);
		cairo_arc(cr, rx + r, ry + r, r, M_PI / 2, 3 * M_PI / 2);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 0.1, 0.07, 0.04, 0.75);
		cairo_fill(cr);
		cairo_move_to(cr, px, py);
		s->found_flash = fmax(0, s->found_flash - dt * 2);
		cairo_set_source_rgba(cr, 1, 0.85 + 0.15 * s->found_flash, 0.3 + 0.7 * s->found_flash,
			0.9);
		cairo_show_text(cr, text);
	}
}

static void diggers_destroy(void *state) {
	struct diggers *s = state;
	free(s->soil);
	free(s->dug);
	free(s->built);
	free(s->rock);
	cairo_surface_destroy(s->tunnels);
	cairo_surface_destroy(s->planks);
	cairo_surface_destroy(s->mask_hole);
	cairo_surface_destroy(s->mask_band);
	cairo_surface_destroy(s->mask_line);
	cairo_pattern_destroy(s->pebbles);
	if (s->desktop) {
		cairo_surface_destroy(s->desktop);
	}
	free(s);
}

static const char *const miners_values[] = { "normal", "few", "many", NULL };
static const char *const miners_labels[] = { "Normal", "Few", "Many", NULL };
static const char *const renew_values[] = { "two", "five", "never", NULL };
static const char *const renew_labels[] = { "After 2 minutes", "After 5 minutes", "Never", NULL };
static const struct saver_option diggers_options[] = {
	{ "miners", "Miners", NULL, SAVER_CHOICE, miners_values, miners_labels, false },
	{ "dynamite", "Dynamite", "One of them carries it and blasts craters", SAVER_TOGGLE, NULL,
		NULL, true },
	{ "renew", "Renew the windows", "The drill clears them and helicopters bring them back; "
		"sooner when a third is dug out", SAVER_CHOICE, renew_values, renew_labels, false },
	{ 0 },
};

const struct saver saver_diggers = {
	.name = "diggers",
	.wants_desktop = true,
	.title = "Diggers",
	.description = "Little miners running over the desktop and digging into your windows "
		"to see what is inside",
	.transparent = true,
	.create = diggers_create,
	.draw = diggers_draw,
	.options = diggers_options,
	.destroy = diggers_destroy,
};
