/*
 * Quick web: a field for a web address where a single key types a whole word.
 * ~/.config/tileWin/quickweb.conf (the Quick web page of the settings) gives
 * the words as a tree:
 *
 *   key g "github.com/" {
 *       key k "ksmbl/"
 *   }
 *   key y "youtube.com"
 *
 * g types "github.com/" at once, and the next key is looked for among its own
 * keys: k then types "ksmbl/". A key with no word of its own is typed as it is,
 * and from there on the keys are letters again; Shift types the letter itself
 * anyway. Backspace takes back a whole word, Enter opens the address in the
 * browser (https:// when it has no scheme), Escape closes. Under the field the
 * keys that come next are shown with their words.
 *
 * "panel quickweb" opens it, on the screen with the focus.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include "draw.h"
#include "flyout.h"
#include "panel.h"
#include "popup.h"
#include "stringop.h"
#include "textfield.h"
#include "tw_paths.h"
#include "twconf.h"

#define WIDTH 620
#define MAX_STEPS 64

struct quickweb {
	struct twconf_node *root;  // the file; NULL when there is none
	const struct twconf_node *at; // whose keys come next; NULL: none any more (letters)
	char text[1024];
	struct text_cursor tc;
	// the words typed, to take back: the length of the text before each, and where it was
	struct {
		size_t before, end;             // the text before the word, and with it
		const struct twconf_node *at;   // whose keys came before it
		const struct twconf_node *word; // its own keys come after it
	} steps[MAX_STEPS];
	int step_count;
	bool any; // the file has words at all
};

static struct twconf_node *load(void) {
	char *dir = tw_config_dir();
	char *path = dir ? format_str("%s/quickweb.conf", dir) : NULL;
	free(dir);
	struct twconf_node *root = path ? twconf_parse_file(path, NULL) : NULL;
	free(path);
	return root;
}

/* The word of a key among the keys of node, NULL for none. */
static const struct twconf_node *find_key(const struct twconf_node *node, const char *key) {
	for (int i = 0; node && i < twconf_count(node); i++) {
		const struct twconf_node *n = twconf_at(node, i);
		if (strcmp(n->name, "key") == 0 && n->argc >= 2 && strcmp(n->argv[0], key) == 0) {
			return n;
		}
	}
	return NULL;
}

static int key_count(const struct twconf_node *node) {
	int n = 0;
	for (int i = 0; node && i < twconf_count(node); i++) {
		const struct twconf_node *k = twconf_at(node, i);
		n += strcmp(k->name, "key") == 0 && k->argc >= 2;
	}
	return n;
}

static void append(struct quickweb *q, const char *s) {
	size_t len = strlen(q->text);
	snprintf(q->text + len, sizeof(q->text) - len, "%s", s);
	text_cursor_end(q->text, &q->tc, false);
}

static void open_address(struct panel *panel, const char *text) {
	const char *t = text;
	while (*t == ' ') {
		t++;
	}
	if (!*t) {
		return;
	}
	char *url = strstr(t, "://") ? strdup(t) : format_str("https://%s", t);
	char *quoted = g_shell_quote(url);
	ipc_panel_commandf(panel, "exec xdg-open %s", quoted);
	g_free(quoted);
	free(url);
}

static void quickweb_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct quickweb *q = p->data;
	if (sym == XKB_KEY_Escape) {
		popup_close_later(p->panel);
		return;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		open_address(p->panel, q->text);
		popup_close_later(p->panel);
		return;
	}
	if (sym == XKB_KEY_BackSpace && !(mods & TEXT_MOD_CTRL)) {
		size_t len = strlen(q->text);
		if (q->step_count > 0 && len == q->steps[q->step_count - 1].end) {
			// a word typed by its key goes as a whole, and its key may be pressed again
			q->step_count--;
			q->text[q->steps[q->step_count].before] = '\0';
			q->at = q->steps[q->step_count].at;
		} else if (len > 0) {
			text_cursor_end(q->text, &q->tc, false);
			text_key(q->text, sizeof(q->text), &q->tc, sym, utf8, mods);
			// back at the end of a word: its keys come next again
			len = strlen(q->text);
			if (q->step_count > 0 && len == q->steps[q->step_count - 1].end) {
				q->at = q->steps[q->step_count - 1].word;
			}
		}
		if (!q->text[0]) {
			q->at = q->root;
			q->step_count = 0;
		}
		text_cursor_end(q->text, &q->tc, false);
		popup_set_dirty(p);
		return;
	}
	bool typed = utf8 && utf8[0] && (unsigned char)utf8[0] >= 0x20 && !(mods & TEXT_MOD_CTRL);
	if (typed) {
		const struct twconf_node *word = !(mods & TEXT_MOD_SHIFT) ? find_key(q->at, utf8) : NULL;
		if (word && q->step_count < MAX_STEPS) {
			q->steps[q->step_count].before = strlen(q->text);
			q->steps[q->step_count].at = q->at;
			q->steps[q->step_count].word = word;
			append(q, word->argv[1]);
			q->steps[q->step_count].end = strlen(q->text);
			q->step_count++;
			q->at = word; // its own keys come next
		} else {
			append(q, utf8);
			q->at = NULL; // a letter of its own: from here on the keys are letters
		}
		popup_set_dirty(p);
		return;
	}
	// Ctrl+A, Ctrl+U and the like as in any field
	if (text_key(q->text, sizeof(q->text), &q->tc, sym, utf8, mods) == TEXT_KEY_CHANGED) {
		if (!q->text[0]) {
			q->at = q->root;
			q->step_count = 0;
		} else {
			q->at = NULL;
		}
	}
	popup_set_dirty(p);
}

static void quickweb_render(struct popup *p, cairo_t *cr) {
	struct quickweb *q = p->data;
	struct fly_style st;
	fly_style_init(&st, p->panel);
	fly_draw_frame(p, cr, &st);
	int M = popup_shadow_margin(p->panel);
	int W = p->surface->width, x0 = M + 18, cw = W - 2 * M - 36;
	int y = M + 14;
	pd_text(cr, st.font, "Quick web", x0, y, cw, 18, st.dim, PD_LEFT);
	y += 22;
	// the field, in a large font
	char big[200];
	const char *base = st.font;
	const char *space = strrchr(base, ' ');
	snprintf(big, sizeof(big), "%.*s 17", space ? (int)(space - base) : (int)strlen(base), base);
	pd_rounded(cr, x0, y, cw, 40, 6);
	pd_color(cr, st.field_bg);
	cairo_fill(cr);
	pd_rect(cr, x0, y + 38, cw, 2, st.accent);
	if (q->text[0]) {
		struct text_style ts = { .font = big, .caret = true };
		text_style_colors(p->panel, &ts);
		ts.fg = st.field_fg;
		text_draw(cr, &ts, q->text, &q->tc, x0 + 12, y, cw - 24, 40);
	} else {
		pd_text(cr, big, q->any ? "Press a key for its word" : "Type an address", x0 + 12, y,
			cw - 24, 40, (st.field_fg & 0xffffff00) | 0x70, PD_LEFT);
		pd_rect(cr, x0 + 12, y + 10, 1.5, 20, st.field_fg);
	}
	y += 52;
	// the keys that come next, as chips
	int x = x0;
	int shown = 0;
	for (int i = 0; q->at && i < twconf_count(q->at); i++) {
		const struct twconf_node *k = twconf_at(q->at, i);
		if (strcmp(k->name, "key") != 0 || k->argc < 2) {
			continue;
		}
		int kw = 0, ww = 0;
		pd_text_size(cr, st.bold, k->argv[0], &kw, NULL);
		pd_text_size(cr, st.font, k->argv[1], &ww, NULL);
		int chip = 8 + (kw < 14 ? 14 : kw) + 8 + ww + 10;
		if (x + chip > x0 + cw) {
			if (y > M + 88 + 30) {
				break; // two rows are enough
			}
			x = x0;
			y += 30;
		}
		pd_rounded(cr, x, y, chip, 24, 12);
		pd_color(cr, (st.fg & 0xffffff00) | 0x14);
		cairo_fill(cr);
		pd_rounded(cr, x + 3, y + 3, (kw < 14 ? 14 : kw) + 10, 18, 9);
		pd_color(cr, st.accent);
		cairo_fill(cr);
		pd_text(cr, st.bold, k->argv[0], x + 3, y + 3, (kw < 14 ? 14 : kw) + 10, 18,
			0xffffffff, PD_CENTER);
		pd_text(cr, st.font, k->argv[1], x + 8 + (kw < 14 ? 14 : kw) + 8, y, ww + 4, 24, st.fg,
			PD_LEFT);
		x += chip + 6;
		shown++;
	}
	if (!shown) {
		pd_text(cr, st.font, !q->any ? "No words yet: give keys their words on the Quick web "
			"page of the settings" : q->text[0] ? "No more words here: type on" : "",
			x0, y, cw, 24, st.dim, PD_LEFT);
	}
	int fy = p->surface->height - M - 30;
	pd_text(cr, st.font, "Enter opens it · Shift types a letter itself · Backspace takes a "
		"word back · Esc closes", x0, fy, cw, 22, st.dim, PD_LEFT);
}

static void quickweb_destroy(struct popup *p) {
	struct quickweb *q = p->data;
	twconf_free(q->root);
	memset(q->text, 0, sizeof(q->text));
	free(q);
}

static bool quickweb_typing(struct popup *p) {
	return true;
}

static const struct popup_vtable quickweb_vtable = {
	.render = quickweb_render,
	.key = quickweb_key,
	.destroy = quickweb_destroy,
	.typing = quickweb_typing,
};

void quickweb_toggle(struct panel *panel, struct panel_output *output) {
	if (popup_is_open(panel, POPUP_QUICKWEB)) {
		popup_close_all(panel);
		return;
	}
	if (!output) {
		return;
	}
	struct quickweb *q = calloc(1, sizeof(*q));
	q->root = load();
	q->at = q->root;
	q->any = key_count(q->root) > 0;
	int M = popup_shadow_margin(panel);
	int width = WIDTH + 2 * M, height = 172 + 2 * M;
	int x = (output->width - width) / 2;
	int y = output->height / 5;
	if (!popup_create(panel, POPUP_QUICKWEB, NULL, output, x, y, width, height,
			&quickweb_vtable, q)) {
		twconf_free(q->root);
		free(q);
	}
}
