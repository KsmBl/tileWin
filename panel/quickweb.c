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
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
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
#include "tw_theme.h"
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

static struct panel *opening_panel;

/* xdg-open said nothing went wrong, or why it could not: then a notification says so. */
static void opened(void *data, const char *output) {
	char *address = data;
	const char *rc = strstr(output, "rc=");
	if (opening_panel && rc && atoi(rc + 3) != 0) {
		char *body = format_str("Nothing opens web addresses here: no browser is set to "
			"open links (xdg-open %s failed). Set one as the default browser in its own "
			"settings, or with xdg-settings set default-web-browser.", address);
		notify_local(opening_panel, "Quick web", "web-browser", "Could not open the address",
			body);
		free(body);
	}
	free(address);
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
	// run here, not by the compositor, so a failure can be told
	char *cmd = format_str("xdg-open %s >/dev/null 2>&1 & pid=$!; sleep 2; "
		"if kill -0 $pid 2>/dev/null; then echo rc=0; else wait $pid; echo \"rc=$?\"; fi",
		quoted);
	opening_panel = panel;
	proc_run(panel, cmd, false, NULL, opened, strdup(url));
	free(cmd);
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

enum { QW_HS_CLOSE = 1 };

/* The font of the theme's dialogs, a quarter bigger, for the address. */
static void field_font(struct panel *panel, char *out, size_t size) {
	PangoFontDescription *desc = pango_font_description_from_string(bar_font(panel));
	int pts = pango_font_description_get_size(desc);
	pango_font_description_set_size(desc, pts > 0 ? pts * 5 / 4 : 12 * PANGO_SCALE);
	char *text = pango_font_description_to_string(desc);
	snprintf(out, size, "%s", text);
	g_free(text);
	pango_font_description_free(desc);
}

/*
 * A dialog of the theme, as the Run dialog: its frame and title bar, the field
 * for the address, the keys that come next as the theme's buttons with their
 * words beside them, and what the keys do.
 */
static void quickweb_render(struct popup *p, cairo_t *cr) {
	struct quickweb *q = p->data;
	struct panel *panel = p->panel;
	const struct tw_theme *t = panel->theme;
	int w, h;
	double top;
	popup_dialog_chrome(p, cr, "Quick web", QW_HS_CLOSE, &w, &h, &top);
	bool console = panel_console(panel);
	uint32_t fg = tw_theme_color(t, "dialog.fg", console ? 0xd3d6ffff : 0x000000ff);
	uint32_t field_fg = tw_theme_color(t, "dialog.field_fg", 0x000000ff);
	// what is said less loudly: the theme's own, else the text a little lighter
	uint32_t dim = tw_theme_color(t, "dialog.dim_fg", (fg & 0xffffff00) | 0xb0);
	const char *font = bar_font(panel);
	char big[200];
	field_font(panel, big, sizeof(big));
	double x0 = console ? 20 : 16, cw = w - x0 - 16;
	double y = top + 12;
	// the field
	double fh = 32;
	popup_dialog_field(panel, cr, x0, y, cw, fh);
	if (q->text[0]) {
		struct text_style ts = { .font = big, .caret = true };
		text_style_colors(panel, &ts);
		ts.fg = field_fg;
		text_draw(cr, &ts, q->text, &q->tc, x0 + 7, y, cw - 14, fh);
	} else {
		pd_text(cr, big, q->any ? "Press a key for its word" : "Type an address", x0 + 7, y,
			cw - 14, fh, (field_fg & 0xffffff00) | 0x80, PD_LEFT);
		pd_rect(cr, x0 + 7, y + 7, 1, fh - 14, field_fg);
	}
	y += fh + 12;
	// the keys that come next: the key as a button of the theme, its word beside it
	double x = x0;
	int shown = 0, rows = 1;
	double bh = 22;
	for (int i = 0; q->at && i < twconf_count(q->at); i++) {
		const struct twconf_node *k = twconf_at(q->at, i);
		if (strcmp(k->name, "key") != 0 || k->argc < 2) {
			continue;
		}
		int kw = 0, ww = 0;
		pd_text_size(cr, bar_bold_font(panel), k->argv[0], &kw, NULL);
		pd_text_size(cr, font, k->argv[1], &ww, NULL);
		double bw = (kw < 12 ? 12 : kw) + 16;
		double item = bw + 6 + ww + 16;
		if (x + item > x0 + cw && x > x0) {
			if (rows == 2) {
				break; // two rows are enough
			}
			rows++;
			x = x0;
			y += bh + 6;
		}
		popup_dialog_button(panel, cr, x, y, bw, bh, k->argv[0], true);
		pd_text(cr, font, k->argv[1], x + bw + 6, y, ww + 2, bh, fg, PD_LEFT);
		x += item;
		shown++;
	}
	if (!shown) {
		pd_text(cr, font, !q->any ? "No words yet: give keys their words on the Quick web "
			"page of the settings" : q->text[0] ? "No more words here: type on" : "",
			x0, y, cw, bh, dim, PD_LEFT);
	}
	// what the keys do, shorter where the font is wide (the Atari's)
	const char *help = "Enter opens it · Shift types the letter itself · Backspace takes a "
		"word back · Esc closes";
	int hw = 0;
	pd_text_size(cr, font, help, &hw, NULL);
	if (hw > cw) {
		help = "Enter opens · Shift: the letter · Backspace: a word back · Esc";
	}
	pd_text(cr, font, help, x0, h - 30, cw, 20, dim, PD_LEFT);
}

static void quickweb_button(struct popup *p, double x, double y, uint32_t button,
		bool pressed) {
	if (pressed || button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	if (hs && hs->kind == QW_HS_CLOSE) {
		popup_close_later(p->panel);
	}
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
	.button = quickweb_button,
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
	int width = WIDTH + 2 * M, height = 176 + 2 * M;
	int x = (output->width - width) / 2;
	int y = output->height / 5;
	if (!popup_create(panel, POPUP_QUICKWEB, NULL, output, x, y, width, height,
			&quickweb_vtable, q)) {
		twconf_free(q->root);
		free(q);
	}
}
