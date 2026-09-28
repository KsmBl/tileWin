#include <pango/pangocairo.h>
#include <stdlib.h>
#include "saver_util.h"
#include "matrix_source.h" // this file itself, made into matrix_source when built

/* The saver, here: its settings are read with it. */
extern const struct saver saver_matrix;

/*
 * Matrix: green code raining down the screen, as in the film. Columns of
 * characters (half-width katakana, mirrored as they were there, digits and a
 * few signs) fall at their own speed, each led by a bright white one, and
 * leave a trail that fades out; now and then a character in a trail changes.
 *
 * With "characters code" the rain is this very file: each column reads the
 * source of the screen saver downwards, character by character, from a place
 * of its own, so the code that draws it runs down the screen.
 *
 * It is cheap: every character is drawn once into an atlas, a frame draws only
 * the new heads and the few characters that change, and the trails fade on
 * their own in a picture kept between frames.
 */

#define GLYPHS_MAX 96
#define FADE 1.5 // how quickly a trail goes dark

struct drop {
	double y;      // the row of the head, may be above the screen
	double speed;  // rows a second
	int last;      // the row drawn last
	size_t pos;    // code: the character of the source at row "start"
	int start;
};

struct matrix {
	int width, height, cols, rows;
	double cw, ch;            // the size of a character cell
	cairo_surface_t *atlas;   // one cell per character, white on nothing
	int glyphs;
	cairo_surface_t *canvas;
	struct drop *drops;
	int frames;
	double color[3];         // of the trails; the heads are nearly white
	bool code;               // the saver's own source instead of the film's characters
	char *text;              // that source, blanks run together, ASCII only
	size_t text_len;
	signed char glyph_of[128]; // atlas cell of each character, -1 for none (blank)
};

/* The source as it rains: tabs, line ends and runs of blanks become one blank. */
static void load_code(struct matrix *m) {
	size_t n = sizeof(matrix_source);
	m->text = malloc(n + 1);
	size_t len = 0;
	bool blank = true;
	for (size_t i = 0; i < n && matrix_source[i]; i++) {
		unsigned char c = (unsigned char)matrix_source[i];
		if (c >= 0x80) {
			continue; // the katakana above: not in a monospace font of plain code
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (!blank) {
				m->text[len++] = ' ';
			}
			blank = true;
			continue;
		}
		if (c < 0x20) {
			continue;
		}
		m->text[len++] = (char)c;
		blank = false;
	}
	m->text[len] = '\0';
	m->text_len = len;
}

/* The characters, and whether a font has them: katakana only where one does. */
static void make_atlas(struct matrix *m) {
	static const char *const set[] = {
		"ｱ", "ｲ", "ｳ", "ｴ", "ｵ", "ｶ", "ｷ", "ｸ", "ｹ", "ｺ", "ｻ", "ｼ", "ｽ", "ｾ", "ｿ", "ﾀ",
		"ﾁ", "ﾂ", "ﾃ", "ﾄ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾉ", "ﾊ", "ﾋ", "ﾌ", "ﾍ", "ﾎ", "ﾏ", "ﾐ",
		"ﾑ", "ﾒ", "ﾓ", "ﾔ", "ﾕ", "ﾖ", "ﾗ", "ﾘ", "ﾙ", "ﾚ", "ﾛ", "ﾜ", "ｦ", "ﾝ",
		"0", "1", "2", "3", "4", "5", "7", "8", "9", "Z", ":", ".", "=", "*", "+", "-",
		"<", ">", "|", "\"", "¦", "ç",
	};
	// the code: every printable character of ASCII
	char ascii[95][2];
	const char *code_set[95];
	for (int c = 33; c < 127; c++) {
		ascii[c - 33][0] = (char)c;
		ascii[c - 33][1] = '\0';
		code_set[c - 33] = ascii[c - 33];
	}
	const char *const *chars = m->code ? code_set : set;
	int count = m->code ? 94 : (int)(sizeof(set) / sizeof(set[0]));
	memset(m->glyph_of, -1, sizeof(m->glyph_of));
	m->atlas = cairo_image_surface_create(CAIRO_FORMAT_A8, (int)ceil(m->cw) * count,
		(int)ceil(m->ch));
	cairo_t *cr = cairo_create(m->atlas);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	char font[128];
	snprintf(font, sizeof(font), m->code ? "DejaVu Sans Mono, Noto Sans Mono, monospace %dpx" :
		"Noto Sans Mono CJK JP, Noto Sans CJK JP, IPAGothic, DejaVu Sans Mono, monospace %dpx",
		(int)(m->ch * 0.78));
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	m->glyphs = 0;
	for (int i = 0; i < count && m->glyphs < GLYPHS_MAX; i++) {
		pango_layout_set_text(layout, chars[i], -1);
		if (pango_layout_get_unknown_glyphs_count(layout) > 0) {
			continue; // no font for it: no boxes on the screen
		}
		int tw, th;
		pango_layout_get_pixel_size(layout, &tw, &th);
		double cx = m->glyphs * ceil(m->cw) + ceil(m->cw) / 2;
		cairo_save(cr);
		cairo_rectangle(cr, m->glyphs * ceil(m->cw), 0, ceil(m->cw), ceil(m->ch));
		cairo_clip(cr);
		cairo_translate(cr, cx, 0);
		if (i < 46 && !m->code) {
			cairo_scale(cr, -1, 1); // the katakana ran mirrored
		}
		if (m->code) {
			m->glyph_of[(unsigned char)chars[i][0]] = (signed char)m->glyphs;
		}
		cairo_move_to(cr, -tw / 2.0, (m->ch - th) / 2);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		pango_cairo_show_layout(cr, layout);
		cairo_restore(cr);
		m->glyphs++;
	}
	g_object_unref(layout);
	cairo_destroy(cr);
}

static void drop_reset(struct matrix *m, struct drop *d, bool anywhere) {
	d->y = anywhere ? saver_between(-m->rows, m->rows) : -saver_between(0, m->rows * 0.6);
	d->speed = saver_between(7, 22);
	d->last = (int)floor(d->y);
	d->start = d->last;
	d->pos = m->text_len ? (size_t)(saver_random() * m->text_len) : 0;
}

/* The character a drop shows in a row: from its place in the code, or any. */
static int glyph_at(struct matrix *m, struct drop *d, int row) {
	if (!m->code) {
		return m->glyphs ? (int)(saver_random() * m->glyphs) : -1;
	}
	if (!m->text_len) {
		return -1;
	}
	long offset = (long)d->pos + (row - d->start);
	offset %= (long)m->text_len;
	if (offset < 0) {
		offset += (long)m->text_len;
	}
	return m->glyph_of[(unsigned char)m->text[offset] & 0x7f];
}

static void *matrix_create(int width, int height, const struct saver_options *options) {
	struct matrix *m = calloc(1, sizeof(*m));
	m->width = width;
	m->height = height;
	static const double colors[][3] = { { 0.35, 1, 0.5 }, { 0.35, 0.7, 1 }, { 1, 0.3, 0.3 },
		{ 1, 0.72, 0.2 }, { 0.9, 0.9, 0.95 } };
	memcpy(m->color, colors[saver_choice(options, &saver_matrix, "color")], sizeof(m->color));
	m->code = saver_choice(options, &saver_matrix, "characters") == 1;
	if (m->code) {
		load_code(m);
	}
	double u = saver_unit(width, height);
	m->ch = fmax(8, round(u * 23));
	m->cw = round(m->ch * 0.72);
	m->cols = (int)(width / m->cw) + 1;
	m->rows = (int)(height / m->ch) + 1;
	make_atlas(m);
	m->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	m->drops = calloc(m->cols, sizeof(struct drop));
	for (int i = 0; i < m->cols; i++) {
		drop_reset(m, &m->drops[i], true);
	}
	return m;
}

/* One character in its cell, over whatever was there. */
static void put_glyph(struct matrix *m, cairo_t *cr, int col, int row, double r, double g,
		double b) {
	if (m->glyphs == 0 || row < 0 || row >= m->rows) {
		return;
	}
	double x = col * m->cw, y = row * m->ch;
	int glyph = glyph_at(m, &m->drops[col], row);
	cairo_save(cr);
	cairo_rectangle(cr, x, y, m->cw, m->ch);
	cairo_clip(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	if (glyph >= 0) { // (a blank of the code stays dark)
		cairo_set_source_rgb(cr, r, g, b);
		cairo_mask_surface(cr, m->atlas, x - glyph * ceil(m->cw), y);
	}
	cairo_restore(cr);
}

/* Fading by a fraction leaves the darkest greens behind for ever: those go. */
static void clear_residue(struct matrix *m) {
	cairo_surface_flush(m->canvas);
	unsigned char *data = cairo_image_surface_get_data(m->canvas);
	int stride = cairo_image_surface_get_stride(m->canvas);
	for (int y = 0; y < m->height; y++) {
		uint32_t *row = (uint32_t *)(data + y * stride);
		for (int x = 0; x < m->width; x++) {
			if (((row[x] >> 8) & 0xff) < 14) {
				row[x] = 0;
			}
		}
	}
	cairo_surface_mark_dirty(m->canvas);
}

static void matrix_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	struct matrix *m = state;
	cairo_t *c = cairo_create(m->canvas);
	cairo_set_source_rgba(c, 0, 0, 0, 1 - exp(-dt * FADE));
	cairo_paint(c);
	for (int i = 0; i < m->cols; i++) {
		struct drop *d = &m->drops[i];
		d->y += d->speed * dt;
		int head = (int)floor(d->y);
		for (int row = d->last + 1; row <= head; row++) {
			// the head moves on: the cell it left turns green, the new one is white
			put_glyph(m, c, i, row - 1, m->color[0], m->color[1], m->color[2]);
			put_glyph(m, c, i, row, 0.75 + 0.25 * m->color[0], 0.75 + 0.25 * m->color[1],
				0.75 + 0.25 * m->color[2]);
		}
		d->last = head;
		if (d->y - d->speed / FADE * 2 > m->rows) {
			drop_reset(m, d, false);
		}
	}
	// a few characters of the trails change, as bright as the trail is there by now
	int changes = m->cols / 3;
	for (int k = 0; k < changes; k++) {
		int col = (int)(saver_random() * m->cols);
		struct drop *d = &m->drops[col];
		int back = 2 + (int)(saver_random() * d->speed * 1.2);
		double light = exp(-FADE * back / d->speed);
		put_glyph(m, c, col, (int)floor(d->y) - back, 0.7 * m->color[0] * light,
			0.9 * m->color[1] * light, 0.7 * m->color[2] * light);
	}
	cairo_destroy(c);
	if (++m->frames % 12 == 0) {
		clear_residue(m);
	}
	cairo_set_source_surface(cr, m->canvas, 0, 0);
	cairo_paint(cr);
}

static void matrix_destroy(void *state) {
	struct matrix *m = state;
	cairo_surface_destroy(m->atlas);
	cairo_surface_destroy(m->canvas);
	free(m->drops);
	free(m->text);
	free(m);
}

static const char *const matrix_values[] = { "green", "blue", "red", "amber", "white", NULL };
static const char *const matrix_labels[] = { "Green", "Blue", "Red", "Amber", "White", NULL };
static const char *const characters_values[] = { "film", "code", NULL };
static const char *const characters_labels[] = { "As in the film", "Its own code", NULL };
static const struct saver_option matrix_options[] = {
	{ "color", "Colour", "Green, as in the film, or another", SAVER_CHOICE, matrix_values,
		matrix_labels, false, NULL },
	{ "characters", "Characters", "The film's katakana and digits, or the source code of this "
		"screen saver itself, read downwards", SAVER_CHOICE, characters_values,
		characters_labels, false, NULL },
	{ 0 },
};

const struct saver saver_matrix = {
	.name = "matrix",
	.title = "Matrix",
	.description = "Green code raining down the screen, as in the film",
	.create = matrix_create,
	.draw = matrix_draw,
	.options = matrix_options,
	.destroy = matrix_destroy,
};

TILEWIN_SAVER(.saver = &saver_matrix, .order = 150, .shot_seconds = 6);
