/*
 * tilewin-polkit: the polkit authentication agent of a tileWin session.
 *
 * When an app asks for something it needs an administrator for (mounting a
 * disk, pkexec, changing a system setting), polkit asks the agent of the
 * session to check the password. This one does it the way Windows does with
 * User Account Control: every screen darkens and a dialog on the main display
 * says what wants to make changes, who is to sign in, and takes the password.
 * Yes (or Return) signs in, No (or Escape) refuses. A wrong password is said
 * so and asked again. Requests that come while one is shown wait their turn.
 *
 * It looks like the dialogs of the theme in use: the window frame of the theme
 * (from the compositor's own renderer, with a close button only) around the
 * dialog of that Windows, from the white boxes of Windows 1 and the grey
 * bevels of Windows 95 over the dithered screen, the beige face of XP and the
 * glossy buttons of Windows 7, to the User Account Control of Windows 10 and
 * 11 without a frame. Dark themes have it dark.
 *
 * tileWin starts it with the session (polkit_agent disable turns that off).
 * It registers for the logind session it runs in and leaves when the
 * compositor goes away.
 */
#define _GNU_SOURCE
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <errno.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <polkitagent/polkitagent.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <json.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "cursor-shape-v1-client-protocol.h"
#include "ipc-client.h"
#include "ipc.h"
#include "pool-buffer.h"
#include "sway/tilewin.h"
#include "tw_theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define CONTENT_WIDTH 440 // the inside of the dialog
#define PAD 18
#define PASSWORD_MAX 512

/* Which Windows the theme looks like, as the compositor's frames tell them. */
enum era {
	ERA_WIN1,
	ERA_WIN3,
	ERA_WIN95, // also Windows 2000
	ERA_XP,
	ERA_WIN7,  // also Vista
	ERA_WIN8,
	ERA_WIN10,
	ERA_WIN11,
};

enum hit {
	HIT_NONE,
	HIT_YES,
	HIT_NO,
	HIT_USER,  // the account to sign in as, when there is a choice
	HIT_CLOSE, // the close button of the frame
};

struct agent_output {
	struct wl_output *wl_output;
	uint32_t wl_name;
	char *name;
	int scale;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	bool configured;
	struct pool_buffer buffers[2];
	struct wl_list link;
};

/* One request of polkit, shown or waiting its turn. */
struct request {
	GTask *task;
	char *action_id, *message, *cookie;
	char *program; // what asks, as far as polkit says
	GList *identities; // PolkitIdentity *
	int identity;      // the one signed in as
	GCancellable *cancellable;
	gulong cancelled_id;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_cursor_shape_manager_v1 *cursor_shape;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct wl_list outputs; // agent_output::link

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	int32_t repeat_rate, repeat_delay;
	guint repeat_source;
	xkb_keysym_t repeat_sym;
	char repeat_utf8[8];

	bool preview;     // --preview: a sample request, polkit left alone
	bool preview_yes; // what the preview was answered
	GMainLoop *loop;

	struct tw_theme *theme;
	enum era era;
	bool dark;
	bool framed; // in the theme's window frame (not Windows 10 and 11)
	bool dither; // the screens dithered, as Windows 95 and before did, not darkened
	uint32_t body, fg, instruction, dim, accent, error;
	uint32_t strip, strip_line; // the band with the buttons at the bottom (0: none)
	uint32_t field_bg, field_fg, field_line, field_focus;
	const char *font, *bold;
	const char *question, *yes_label, *no_label;
	cairo_surface_t *shield; // the shield of User Account Control, for the frame

	GQueue queue; // struct request *, waiting
	struct request *current;
	PolkitAgentSession *session;
	bool echo;      // the prompt shows what is typed (not a password)
	bool waiting;   // a prompt waits for the answer
	bool checking;  // the answer is being checked
	bool cancelling;
	char prompt[128];
	char note[256]; // what PAM says, or that the password was wrong
	bool note_error;
	char password[PASSWORD_MAX];
	size_t password_len;

	struct agent_output *main; // where the dialog is
	double px, py;
	struct wl_surface *pointer_surface;
	enum hit hover, pressed;
	struct { double x, y, w, h; } yes, no, user, close; // where they were drawn
} agent;

static void render_all(void);
static void finish_request(gboolean gained, GError *error);
static void begin_session(void);

/* ---------- drawing ---------- */

static void set_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift <= 24; shift += 8) {
		double ca = a >> shift & 0xff, cb = b >> shift & 0xff;
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void box(cairo_t *cr, double x, double y, double w, double h, double r, uint32_t c) {
	cairo_new_path(cr);
	if (r > 0) {
		rounded(cr, x, y, w, h, r);
	} else {
		cairo_rectangle(cr, x, y, w, h);
	}
	set_color(cr, c);
	cairo_fill(cr);
}

/* Windows 95's raised (or sunken) edge: light and dark lines on two sides each. */
static void bevel(cairo_t *cr, double x, double y, double w, double h, bool sunken) {
	uint32_t light = sunken ? 0x808080ff : 0xffffffff;
	uint32_t dark = sunken ? 0xffffffff : 0x404040ff;
	uint32_t light2 = sunken ? 0x404040ff : 0xdfdfdfff;
	uint32_t dark2 = sunken ? 0xdfdfdfff : 0x808080ff;
	box(cr, x, y, w, 1, 0, light);
	box(cr, x, y, 1, h, 0, light);
	box(cr, x, y + h - 1, w, 1, 0, dark);
	box(cr, x + w - 1, y, 1, h, 0, dark);
	box(cr, x + 1, y + 1, w - 2, 1, 0, light2);
	box(cr, x + 1, y + 1, 1, h - 2, 0, light2);
	box(cr, x + 1, y + h - 2, w - 2, 1, 0, dark2);
	box(cr, x + w - 2, y + 1, 1, h - 2, 0, dark2);
}

/* Text wrapped to width; returns its height. Drawn only with a color. */
static int text(cairo_t *cr, const char *font, const char *str, double x, double y,
		int width, uint32_t color, PangoAlignment align, bool draw) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, str, -1);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_alignment(layout, align);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	if (draw) {
		cairo_move_to(cr, x, y);
		set_color(cr, color);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
	return h;
}

/* The font one size up, for the question. */
static void bigger(const char *font, char *out, size_t size, int by) {
	const char *space = strrchr(font, ' ');
	int pt = space ? atoi(space + 1) : 0;
	if (pt > 0) {
		snprintf(out, size, "%.*s %d", (int)(space - font), font, pt + by);
	} else {
		snprintf(out, size, "%s %d", font, 10 + by);
	}
}

static char *identity_label(PolkitIdentity *identity) {
	if (POLKIT_IS_UNIX_USER(identity)) {
		uid_t uid = polkit_unix_user_get_uid(POLKIT_UNIX_USER(identity));
		struct passwd *pw = getpwuid(uid);
		if (pw) {
			char real[128] = "";
			if (pw->pw_gecos && pw->pw_gecos[0]) {
				snprintf(real, sizeof(real), "%.*s", (int)strcspn(pw->pw_gecos, ","),
					pw->pw_gecos);
			}
			return real[0] && strcmp(real, pw->pw_name) != 0 ?
				g_strdup_printf("%s (%s)", real, pw->pw_name) : g_strdup(pw->pw_name);
		}
		return g_strdup_printf("user %u", (unsigned)uid);
	}
	if (POLKIT_IS_UNIX_GROUP(identity)) {
		char *id = polkit_identity_to_string(identity);
		const char *group = g_str_has_prefix(id, "unix-group:") ? id + 11 : id;
		char *label = g_strdup_printf("a member of %s", group);
		g_free(id);
		return label;
	}
	return polkit_identity_to_string(identity);
}

/* The shield of User Account Control: blue and gold quarters. */
static void draw_shield(cairo_t *cr, double x, double y, double size) {
	double w = size * 0.82, h = size;
	double l = x + (size - w) / 2, r = l + w, cx = x + size / 2;
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_move_to(cr, cx, y);
	cairo_curve_to(cr, cx + w * 0.25, y + h * 0.08, r - w * 0.1, y + h * 0.1, r, y + h * 0.12);
	cairo_line_to(cr, r, y + h * 0.5);
	cairo_curve_to(cr, r, y + h * 0.78, cx + w * 0.2, y + h * 0.92, cx, y + h);
	cairo_curve_to(cr, cx - w * 0.2, y + h * 0.92, l, y + h * 0.78, l, y + h * 0.5);
	cairo_line_to(cr, l, y + h * 0.12);
	cairo_curve_to(cr, l + w * 0.1, y + h * 0.1, cx - w * 0.25, y + h * 0.08, cx, y);
	cairo_close_path(cr);
	cairo_path_t *outline = cairo_copy_path(cr);
	cairo_clip(cr);
	double my = y + h * 0.46;
	uint32_t blue = 0x2a64c8ff, gold = 0xf0c419ff;
	box(cr, l, y, cx - l, my - y, 0, blue);
	box(cr, cx, y, r - cx, my - y, 0, gold);
	box(cr, l, my, cx - l, y + h - my, 0, gold);
	box(cr, cx, my, r - cx, y + h - my, 0, blue);
	cairo_pattern_t *gloss = cairo_pattern_create_linear(0, y, 0, y + h);
	cairo_pattern_add_color_stop_rgba(gloss, 0, 1, 1, 1, 0.45);
	cairo_pattern_add_color_stop_rgba(gloss, 0.5, 1, 1, 1, 0.05);
	cairo_pattern_add_color_stop_rgba(gloss, 1, 0, 0, 0, 0.15);
	cairo_set_source(cr, gloss);
	cairo_paint(cr);
	cairo_pattern_destroy(gloss);
	cairo_reset_clip(cr);
	cairo_new_path(cr);
	cairo_append_path(cr, outline);
	cairo_path_destroy(outline);
	cairo_set_line_width(cr, size / 16);
	set_color(cr, 0x1a3a70ff);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void outline(cairo_t *cr, double x, double y, double w, double h, double r, double lw,
		uint32_t c) {
	cairo_new_path(cr);
	if (r > 0) {
		rounded(cr, x + lw / 2, y + lw / 2, w - lw, h - lw, r);
	} else {
		cairo_rectangle(cr, x + lw / 2, y + lw / 2, w - lw, h - lw);
	}
	set_color(cr, c);
	cairo_set_line_width(cr, lw);
	cairo_stroke(cr);
}

/* A vertical gradient of "offset:#color" stops, in a rounded box. */
static void gradient_box(cairo_t *cr, double x, double y, double w, double h, double r,
		const uint32_t *colors, const double *stops, int n) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(0, y, 0, y + h);
	for (int i = 0; i < n; i++) {
		uint32_t c = colors[i];
		cairo_pattern_add_color_stop_rgba(pattern, stops[i], (c >> 24 & 0xff) / 255.0,
			(c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
	}
	cairo_new_path(cr);
	if (r > 0) {
		rounded(cr, x, y, w, h, r);
	} else {
		cairo_rectangle(cr, x, y, w, h);
	}
	cairo_set_source(cr, pattern);
	cairo_fill(cr);
	cairo_pattern_destroy(pattern);
}

static void button_label(cairo_t *cr, const char *label, double x, double y, double w,
		double h, uint32_t color, double shift) {
	int th = text(cr, agent.font, label, 0, 0, w, 0, PANGO_ALIGN_CENTER, false);
	text(cr, agent.font, label, x + shift, y + (h - th) / 2.0 + shift, w, color,
		PANGO_ALIGN_CENTER, true);
}

/* A push button of the theme's Windows; the default one is marked as it was. */
static void button(cairo_t *cr, double x, double y, double w, double h, const char *label,
		bool primary, bool hover, bool pressed) {
	bool dark = agent.dark;
	switch (agent.era) {
	case ERA_WIN1:
		box(cr, x, y, w, h, 5, pressed ? 0x000000ff : 0xffffffff);
		outline(cr, x, y, w, h, 5, primary ? 2 : 1, 0x000000ff);
		button_label(cr, label, x, y, w, h, pressed ? 0xffffffff : 0x000000ff, 0);
		return;
	case ERA_WIN3:
		box(cr, x, y, w, h, 0, 0xc0c0c0ff);
		if (pressed) {
			box(cr, x + 1, y + 1, w - 2, 1, 0, 0x808080ff);
			box(cr, x + 1, y + 1, 1, h - 2, 0, 0x808080ff);
		} else {
			for (int i = 1; i <= 2; i++) { // the thick raised edge of 3.1
				box(cr, x + i, y + i, w - 2 * i, 1, 0, 0xffffffff);
				box(cr, x + i, y + i, 1, h - 2 * i, 0, 0xffffffff);
				box(cr, x + i, y + h - 1 - i, w - 2 * i, 1, 0, 0x808080ff);
				box(cr, x + w - 1 - i, y + i, 1, h - 2 * i, 0, 0x808080ff);
			}
		}
		// the black frame with its corners cut off
		box(cr, x + 1, y, w - 2, 1, 0, 0x000000ff);
		box(cr, x + 1, y + h - 1, w - 2, 1, 0, 0x000000ff);
		box(cr, x, y + 1, 1, h - 2, 0, 0x000000ff);
		box(cr, x + w - 1, y + 1, 1, h - 2, 0, 0x000000ff);
		if (primary) {
			outline(cr, x - 1, y - 1, w + 2, h + 2, 0, 1, 0x000000ff);
		}
		button_label(cr, label, x, y, w, h, 0x000000ff, pressed ? 1 : 0);
		return;
	case ERA_WIN95:
		box(cr, x, y, w, h, 0, 0xc0c0c0ff);
		if (primary) { // the default button has a dark frame round its edge
			outline(cr, x - 1, y - 1, w + 2, h + 2, 0, 1, 0x000000ff);
		}
		bevel(cr, x, y, w, h, pressed);
		button_label(cr, label, x, y, w, h, 0x000000ff, pressed ? 1 : 0);
		return;
	case ERA_XP:
		if (!dark) {
			static const double stops[] = { 0, 0.85, 1 };
			uint32_t normal[] = { 0xffffffff, 0xf0f0eaff, 0xd6d0c5ff };
			uint32_t down[] = { 0xe5e4ddff, 0xe2e2daff, 0xf0f0eaff };
			gradient_box(cr, x, y, w, h, 3, pressed ? down : normal, stops, 3);
			if (hover && !pressed) { // the orange glow inside the edge
				outline(cr, x + 1, y + 1, w - 2, h - 2, 2, 2, 0xf8b330ff);
			} else if (primary && !pressed) { // and the blue one of the default
				outline(cr, x + 1, y + 1, w - 2, h - 2, 2, 2, 0xa1bcf9ff);
			}
			outline(cr, x, y, w, h, 3, 1, 0x003c74ff);
			button_label(cr, label, x, y, w, h, 0x000000ff, pressed ? 1 : 0);
			return;
		}
		break;
	case ERA_WIN7:
		if (!dark) {
			static const double stops[] = { 0, 0.5, 0.5, 1 };
			uint32_t normal[] = { 0xf2f2f2ff, 0xebebebff, 0xddddddff, 0xcfcfcfff };
			uint32_t hot[] = { 0xeaf6fdff, 0xd9f0fcff, 0xbee6fdff, 0xa7d9f5ff };
			uint32_t down[] = { 0xe5f4fcff, 0xc4e5f6ff, 0x98d1efff, 0x68b3dbff };
			gradient_box(cr, x, y, w, h, 3, pressed ? down : hover ? hot : normal, stops, 4);
			outline(cr, x + 1, y + 1, w - 2, h - 2, 2, 1, 0xffffffb0);
			outline(cr, x, y, w, h, 3, 1, pressed ? 0x2c628bff : hover || primary ?
				0x3c7fb1ff : 0x707070ff);
			button_label(cr, label, x, y, w, h, 0x000000ff, 0);
			return;
		}
		break;
	case ERA_WIN8:
		if (!dark) {
			box(cr, x, y, w, h, 0, pressed ? 0xcce4f7ff : hover ? 0xe5f1fbff : 0xe1e1e1ff);
			outline(cr, x, y, w, h, 0, primary ? 2 : 1, pressed ? 0x005499ff :
				hover || primary ? 0x0078d7ff : 0xadadadff);
			button_label(cr, label, x, y, w, h, 0x000000ff, 0);
			return;
		}
		break;
	case ERA_WIN10:
		box(cr, x, y, w, h, 0, pressed ? (dark ? 0x666666ff : 0x999999ff) :
			dark ? 0x333333ff : 0xccccccff);
		if (hover || primary) {
			outline(cr, x, y, w, h, 0, 2, hover ? (dark ? 0x858585ff : 0x7a7a7aff) :
				agent.accent);
		}
		button_label(cr, label, x, y, w, h, agent.fg, 0);
		return;
	case ERA_WIN11:
		break;
	}
	// Windows 11, and the dark variants of the others: plain rounded buttons,
	// the default one in the accent color
	uint32_t overlay = dark ? 0xffffff00 : 0x00000000;
	uint32_t bg = primary ? agent.accent : dark ? mix(agent.body, 0xffffffff, 0.08) : 0xfbfbfbff;
	if (hover || pressed) {
		bg = primary ? mix(agent.accent, dark ? 0x000000ff : 0xffffffff, pressed ? 0.2 : 0.1) :
			mix(bg, dark ? 0xffffffff : 0x000000ff, pressed ? 0.12 : 0.05);
	}
	double radius = agent.era == ERA_WIN8 ? 0 : agent.era == ERA_XP ? 3 : 4; // 8 is square
	box(cr, x, y, w, h, radius, bg);
	if (!primary) {
		outline(cr, x, y, w, h, radius, 1, overlay | (dark ? 0x30 : 0x1c));
	}
	button_label(cr, label, x, y, w, h, primary ? 0xffffffff : agent.fg, 0);
}

/* The field the password goes into. */
static void field(cairo_t *cr, double x, double y, double w, double h) {
	bool focus = agent.waiting;
	switch (agent.era) {
	case ERA_WIN95:
		box(cr, x, y, w, h, 0, agent.field_bg);
		bevel(cr, x, y, w, h, true);
		return;
	case ERA_WIN11:
		box(cr, x, y, w, h, 4, agent.field_bg);
		outline(cr, x, y, w, h, 4, 1, agent.field_line);
		if (focus) { // the line under a field that has the keys
			box(cr, x + 1, y + h - 2, w - 2, 2, 0, agent.accent);
		}
		return;
	case ERA_WIN10:
		box(cr, x, y, w, h, 0, agent.field_bg);
		outline(cr, x, y, w, h, 0, 2, focus ? agent.field_focus : agent.field_line);
		return;
	default:
		box(cr, x, y, w, h, 0, agent.field_bg);
		outline(cr, x, y, w, h, 0, 1, focus ? agent.field_focus : agent.field_line);
		return;
	}
}

/* The text of the field: dots for a password, or what PAM asked for. */
static void field_text(cairo_t *cr, double x, double y, double w, double h) {
	char shown[PASSWORD_MAX * 3 + 4];
	if (agent.checking) {
		snprintf(shown, sizeof(shown), "Checking...");
	} else if (agent.echo) {
		snprintf(shown, sizeof(shown), "%.*s", (int)agent.password_len, agent.password);
	} else {
		size_t chars = g_utf8_strlen(agent.password, agent.password_len);
		size_t n = 0;
		for (size_t i = 0; i < chars && i < 64; i++) {
			// the old Windows showed asterisks, the newer ones dots
			n += snprintf(shown + n, sizeof(shown) - n, "%s",
				agent.era <= ERA_WIN95 ? "*" : "●");
		}
		shown[n] = '\0';
	}
	double tw = 0;
	if (shown[0]) {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *desc = pango_font_description_from_string(agent.font);
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, shown, -1);
		int lw, lh;
		pango_layout_get_pixel_size(layout, &lw, &lh);
		tw = lw;
		g_object_unref(layout);
		text(cr, agent.font, shown, x + 6, y + (h - lh) / 2.0, w - 12,
			agent.checking ? agent.dim : agent.field_fg, PANGO_ALIGN_LEFT, true);
	}
	if (agent.waiting) { // the caret
		box(cr, x + 6 + tw + 1, y + 5, 1, h - 10, 0, agent.field_fg);
	}
}

/*
 * The inside of the dialog (without a frame) with its top left corner at x, y,
 * CONTENT_WIDTH wide; returns its height. Measured only when draw is false.
 * The buttons stand in a band at the bottom where the Windows had one.
 */
static int content(cairo_t *cr, double x, double y, bool draw) {
	struct request *r = agent.current;
	const int W = CONTENT_WIDTH;
	enum era era = agent.era;
	bool unframed = !agent.framed;
	bool band = era == ERA_WIN10; // the accent band with the name on top
	int head = band ? 44 : era == ERA_WIN11 ? 36 : 0;
	bool shield = era >= ERA_WIN7;
	int icon = shield ? 36 : 0;
	int inner = W - 2 * PAD;
	int text_x = PAD + (icon ? icon + 12 : 0), text_w = W - text_x - PAD;
	char big[160];
	bigger(era == ERA_WIN10 || era == ERA_WIN11 ? agent.bold : agent.font, big, sizeof(big),
		era >= ERA_WIN10 ? 4 : era >= ERA_WIN7 ? 3 : 0);
	const char *question_font = era >= ERA_WIN7 ? big : agent.bold;
	char action[300];
	snprintf(action, sizeof(action), "Action: %s", r->action_id);
	char program[512];
	snprintf(program, sizeof(program), "Program: %s", r->program ? r->program : "");
	int strip_h = agent.strip ? 52 : 46;
	double bw = era <= ERA_WIN95 ? 80 : 96, bh = era <= ERA_WIN95 ? 24 : era == ERA_XP ? 24 : 28;
	if (era >= ERA_WIN10) {
		bw = 120;
		bh = 32;
	}

	// measured from the top: the question, what polkit says, who signs in,
	// the prompt with its field and what PAM says
	int h = head + PAD;
	int q = text(cr, question_font, agent.question, 0, 0, text_w, 0, PANGO_ALIGN_LEFT, false);
	h += (q > icon ? q : icon) + 10;
	int msg = r->message && r->message[0] ?
		text(cr, agent.font, r->message, 0, 0, text_w, 0, PANGO_ALIGN_LEFT, false) : 0;
	h += msg ? msg + 4 : 0;
	int prog = r->program ? text(cr, agent.font, program, 0, 0, text_w, 0, PANGO_ALIGN_LEFT,
		false) : 0;
	h += prog ? prog + 2 : 0;
	h += text(cr, agent.font, action, 0, 0, text_w, 0, PANGO_ALIGN_LEFT, false) + 14;
	h += 22 + 4 + 18 + 28 + 4 + 20; // signing in as, prompt, field, note
	h += strip_h;
	if (!draw) {
		return h;
	}

	// the dialog itself; Windows 10 and 11 have no frame around it
	if (unframed) {
		double radius = era == ERA_WIN11 ? 8 : 0;
		for (int i = 8; i > 0; i--) { // a soft shadow
			box(cr, x - i, y - i + 4, W + 2 * i, h + 2 * i, radius + i, 0x0000000a);
		}
		cairo_save(cr);
		cairo_new_path(cr);
		if (radius > 0) {
			rounded(cr, x, y, W, h, radius);
		} else {
			cairo_rectangle(cr, x, y, W, h);
		}
		cairo_clip(cr);
	}
	box(cr, x, y, W, h, 0, agent.body);
	if (band) {
		box(cr, x, y, W, head, 0, agent.accent);
		int th = text(cr, agent.font, "User Account Control", 0, 0, inner, 0,
			PANGO_ALIGN_LEFT, false);
		text(cr, agent.font, "User Account Control", x + PAD, y + (head - th) / 2.0, inner,
			0xffffffff, PANGO_ALIGN_LEFT, true);
	} else if (era == ERA_WIN11) {
		text(cr, agent.font, "User Account Control", x + PAD, y + PAD, inner, agent.fg,
			PANGO_ALIGN_LEFT, true);
	}
	if (agent.strip) {
		box(cr, x, y + h - strip_h, W, strip_h, 0, agent.strip);
		box(cr, x, y + h - strip_h, W, 1, 0, agent.strip_line);
	}
	if (unframed) {
		cairo_restore(cr);
		if (era == ERA_WIN11) {
			outline(cr, x, y, W, h, 8, 1, agent.dark ? 0xffffff20 : 0x00000020);
		} else {
			outline(cr, x, y, W, h, 0, 1, agent.accent);
		}
	}

	double cy = y + head + PAD;
	if (shield) {
		draw_shield(cr, x + PAD, cy, icon);
	}
	int qh = text(cr, question_font, agent.question, x + text_x, cy, text_w, agent.instruction,
		PANGO_ALIGN_LEFT, true);
	cy += (qh > icon ? qh : icon) + 10;
	if (msg) {
		cy += text(cr, agent.font, r->message, x + text_x, cy, text_w, agent.fg,
			PANGO_ALIGN_LEFT, true) + 4;
	}
	if (prog) {
		cy += text(cr, agent.font, program, x + text_x, cy, text_w, agent.dim,
			PANGO_ALIGN_LEFT, true) + 2;
	}
	cy += text(cr, agent.font, action, x + text_x, cy, text_w, agent.dim, PANGO_ALIGN_LEFT,
		true) + 14;

	// who signs in; a click (or the up and down keys) goes to the next one
	double cx = x + PAD;
	PolkitIdentity *identity = g_list_nth_data(r->identities, r->identity);
	char *who = identity ? identity_label(identity) : g_strdup("?");
	int count = g_list_length(r->identities);
	char line[300];
	snprintf(line, sizeof(line), count > 1 ? "Sign in as: %s  ▾" : "Sign in as: %s", who);
	g_free(who);
	agent.user.x = cx;
	agent.user.y = cy;
	agent.user.w = count > 1 ? inner : 0;
	agent.user.h = 22;
	if (count > 1 && agent.hover == HIT_USER) {
		box(cr, cx - 4, cy - 2, inner + 8, 22, era >= ERA_WIN11 ? 4 : 0,
			(agent.dark ? 0xffffff00 : 0x00000000) | 0x14);
	}
	text(cr, agent.bold, line, cx, cy + 1, inner, agent.fg, PANGO_ALIGN_LEFT, true);
	cy += 22 + 4;

	const char *prompt = agent.prompt[0] ? agent.prompt : "Password:";
	text(cr, agent.font, prompt, cx, cy, inner, agent.fg, PANGO_ALIGN_LEFT, true);
	cy += 18;
	field(cr, cx, cy, inner, 28);
	field_text(cr, cx, cy, inner, 28);
	cy += 28 + 4;
	if (agent.note[0]) {
		text(cr, agent.font, agent.note, cx, cy, inner,
			agent.note_error ? agent.error : agent.dim, PANGO_ALIGN_LEFT, true);
	}

	// the buttons at the bottom right
	double by = y + h - strip_h + (strip_h - bh) / 2.0;
	agent.no.x = x + W - PAD - bw;
	agent.yes.x = agent.no.x - (era <= ERA_WIN95 ? 8 : 10) - bw;
	agent.no.y = agent.yes.y = by;
	agent.no.w = agent.yes.w = bw;
	agent.no.h = agent.yes.h = bh;
	button(cr, agent.yes.x, by, bw, bh, agent.yes_label, true, agent.hover == HIT_YES,
		agent.pressed == HIT_YES && agent.hover == HIT_YES);
	button(cr, agent.no.x, by, bw, bh, agent.no_label, false, agent.hover == HIT_NO,
		agent.pressed == HIT_NO && agent.hover == HIT_NO);
	return h;
}

/* The whole dialog, its frame included; its size in *w, *h, drawn when draw. */
static void dialog(cairo_t *cr, double x, double y, bool draw, int *w, int *h) {
	int ch = content(cr, 0, 0, false);
	if (!agent.framed) {
		*w = CONTENT_WIDTH;
		*h = ch;
		agent.close.w = 0;
		if (draw) {
			content(cr, x, y, true);
		}
		return;
	}
	struct tw_insets in;
	tw_style_insets(agent.theme, false, &in, NULL);
	*w = CONTENT_WIDTH + in.left + in.right;
	*h = ch + in.top + in.bottom;
	if (!draw) {
		return;
	}
	struct tw_frame frame = {
		.width = *w,
		.height = *h,
		.focused = true,
		.title = "User Account Control",
		.icon = agent.era >= ERA_WIN7 ? agent.shield : NULL,
		.hover = agent.hover == HIT_CLOSE ? TW_HIT_CLOSE : TW_HIT_NONE,
		.pressed = agent.pressed == HIT_CLOSE ? TW_HIT_CLOSE : TW_HIT_NONE,
		.dialog = true,
	};
	cairo_save(cr);
	cairo_translate(cr, x, y);
	tw_style_draw_frame(cr, agent.theme, &frame);
	cairo_restore(cr);
	struct tw_buttons b;
	tw_style_buttons(agent.theme, *w, false, &b);
	agent.close.x = x + b.close.x;
	agent.close.y = y + b.close.y;
	agent.close.w = b.close.width;
	agent.close.h = b.close.height;
	content(cr, x + in.left, y + in.top, true);
}

/* Every other pixel black, as Windows 95 and before shaded the screen. */
static void dither(cairo_t *cr) {
	cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
	unsigned char *data = cairo_image_surface_get_data(tile);
	int stride = cairo_image_surface_get_stride(tile);
	cairo_surface_flush(tile);
	memset(data, 0, stride * 2);
	((uint32_t *)data)[0] = 0xff000000;
	((uint32_t *)(data + stride))[1] = 0xff000000;
	cairo_surface_mark_dirty(tile);
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(tile);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_set_source(cr, pattern);
	cairo_paint(cr);
	cairo_pattern_destroy(pattern);
	cairo_surface_destroy(tile);
}

static void render_output(struct agent_output *o) {
	if (!o->configured || !o->width || !o->height || !agent.current) {
		return;
	}
	int scale = o->scale > 0 ? o->scale : 1;
	struct pool_buffer *buffer = get_next_buffer(agent.shm, o->buffers,
		o->width * scale, o->height * scale);
	if (!buffer) {
		return;
	}
	cairo_t *cr = buffer->cairo;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	if (agent.dither) {
		dither(cr);
	} else {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55); // the secure desktop, darkened
		cairo_paint(cr);
	}
	cairo_restore(cr);
	cairo_save(cr);
	cairo_scale(cr, scale, scale);
	if (o == agent.main) {
		int w, h;
		dialog(cr, 0, 0, false, &w, &h);
		dialog(cr, floor((o->width - w) / 2.0), floor((o->height - h) / 2.0), true, &w, &h);
	}
	cairo_restore(cr);
	wl_surface_set_buffer_scale(o->surface, scale);
	wl_surface_attach(o->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(o->surface);
	buffer->busy = true;
}

static void render_all(void) {
	struct agent_output *o;
	wl_list_for_each(o, &agent.outputs, link) {
		render_output(o);
	}
	wl_display_flush(agent.display);
}

/* ---------- the surfaces ---------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct agent_output *o = data;
	o->width = width;
	o->height = height;
	o->configured = true;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_output(o);
}

static void destroy_surface(struct agent_output *o) {
	if (o->layer_surface) {
		zwlr_layer_surface_v1_destroy(o->layer_surface);
		o->layer_surface = NULL;
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
		o->surface = NULL;
	}
	destroy_buffer(&o->buffers[0]);
	destroy_buffer(&o->buffers[1]);
	o->configured = false;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* The name of the main display, from tileWin. */
static char *main_output_name(void) {
	char *path = get_socketpath();
	if (!path) {
		return NULL;
	}
	int fd = ipc_open_socket(path);
	free(path);
	if (fd < 0) {
		return NULL;
	}
	uint32_t len = 0;
	char *reply = ipc_single_command(fd, IPC_GET_TILEWIN, "", &len);
	close(fd);
	char *name = NULL;
	json_object *obj = reply ? json_tokener_parse(reply) : NULL;
	json_object *value;
	if (obj && json_object_object_get_ex(obj, "main_output", &value) &&
			json_object_get_string(value) && json_object_get_string(value)[0]) {
		name = strdup(json_object_get_string(value));
	}
	json_object_put(obj);
	free(reply);
	return name;
}

static void create_surface(struct agent_output *o) {
	if (o->surface || !agent.layer_shell) {
		return;
	}
	o->surface = wl_compositor_create_surface(agent.compositor);
	o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(agent.layer_shell, o->surface,
		o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-polkit");
	zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_listener, o);
	zwlr_layer_surface_v1_set_anchor(o->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
	// the keys go to the dialog, as nothing else may have them meanwhile
	zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer_surface, o == agent.main ?
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE :
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(o->surface);
}

static void show_surfaces(void) {
	char *main_name = main_output_name();
	agent.main = NULL;
	struct agent_output *o;
	wl_list_for_each(o, &agent.outputs, link) {
		if (!agent.main || (main_name && o->name && strcmp(o->name, main_name) == 0)) {
			agent.main = o;
		}
	}
	free(main_name);
	wl_list_for_each(o, &agent.outputs, link) {
		create_surface(o);
	}
	wl_display_flush(agent.display);
}

static void hide_surfaces(void) {
	struct agent_output *o;
	wl_list_for_each(o, &agent.outputs, link) {
		destroy_surface(o);
	}
	agent.main = NULL;
	agent.hover = agent.pressed = HIT_NONE;
	wl_display_flush(agent.display);
}

/* ---------- the password ---------- */

static void clear_password(void) {
	explicit_bzero(agent.password, sizeof(agent.password));
	agent.password_len = 0;
}

static void submit(void) {
	if (agent.preview && agent.waiting) {
		agent.preview_yes = true;
		finish_request(TRUE, NULL);
		return;
	}
	if (!agent.session || !agent.waiting) {
		return;
	}
	agent.waiting = false;
	agent.checking = true;
	agent.note[0] = '\0';
	polkit_agent_session_response(agent.session, agent.password);
	clear_password();
	render_all();
}

static void cancel_current(void) {
	if (!agent.current) {
		return;
	}
	agent.cancelling = true;
	if (agent.session) {
		polkit_agent_session_cancel(agent.session); // completes, not gained
	} else {
		finish_request(FALSE, g_error_new(POLKIT_ERROR, POLKIT_ERROR_CANCELLED,
			"The user said no"));
	}
}

static void next_identity(int step) {
	struct request *r = agent.current;
	int count = r ? (int)g_list_length(r->identities) : 0;
	if (count < 2 || agent.checking) {
		return;
	}
	r->identity = (r->identity + step + count) % count;
	agent.note[0] = '\0';
	clear_password();
	begin_session(); // a new session for the other account
	render_all();
}

/* ---------- polkit sessions ---------- */

static void session_request(PolkitAgentSession *session, const char *request, gboolean echo_on,
		gpointer data) {
	snprintf(agent.prompt, sizeof(agent.prompt), "%s", request ? request : "");
	g_strstrip(agent.prompt);
	if (strcasecmp(agent.prompt, "password:") == 0 ||
			strcasecmp(agent.prompt, "password: ") == 0) {
		snprintf(agent.prompt, sizeof(agent.prompt), "Password:");
	}
	agent.echo = echo_on;
	agent.waiting = true;
	agent.checking = false;
	render_all();
}

static void session_show_error(PolkitAgentSession *session, const char *text, gpointer data) {
	snprintf(agent.note, sizeof(agent.note), "%s", text ? text : "");
	agent.note_error = true;
	render_all();
}

static void session_show_info(PolkitAgentSession *session, const char *text, gpointer data) {
	snprintf(agent.note, sizeof(agent.note), "%s", text ? text : "");
	agent.note_error = false;
	render_all();
}

static void session_completed(PolkitAgentSession *session, gboolean gained, gpointer data) {
	if (session != agent.session) {
		return;
	}
	g_object_unref(agent.session);
	agent.session = NULL;
	agent.checking = agent.waiting = false;
	if (gained) {
		finish_request(TRUE, NULL);
	} else if (agent.cancelling || !agent.current) {
		finish_request(FALSE, g_error_new(POLKIT_ERROR, POLKIT_ERROR_CANCELLED,
			"The user said no"));
	} else {
		// wrong: once more, as Windows asks again
		snprintf(agent.note, sizeof(agent.note), "The password is incorrect. Try again.");
		agent.note_error = true;
		begin_session();
		render_all();
	}
}

static void begin_session(void) {
	struct request *r = agent.current;
	if (agent.preview) { // nothing to check the password: just the prompt
		agent.waiting = true;
		agent.checking = false;
		snprintf(agent.prompt, sizeof(agent.prompt), "Password:");
		return;
	}
	if (agent.session) {
		PolkitAgentSession *old = agent.session;
		agent.session = NULL; // its completion is not ours any more
		g_signal_handlers_disconnect_matched(old, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL,
			&agent);
		polkit_agent_session_cancel(old);
		g_object_unref(old);
	}
	PolkitIdentity *identity = g_list_nth_data(r->identities, r->identity);
	if (!identity) {
		finish_request(FALSE, g_error_new(POLKIT_ERROR, POLKIT_ERROR_FAILED,
			"No one to sign in as"));
		return;
	}
	agent.waiting = agent.checking = false;
	agent.prompt[0] = '\0';
	agent.session = polkit_agent_session_new(identity, r->cookie);
	g_signal_connect(agent.session, "request", G_CALLBACK(session_request), &agent);
	g_signal_connect(agent.session, "show-error", G_CALLBACK(session_show_error), &agent);
	g_signal_connect(agent.session, "show-info", G_CALLBACK(session_show_info), &agent);
	g_signal_connect(agent.session, "completed", G_CALLBACK(session_completed), &agent);
	polkit_agent_session_initiate(agent.session);
}

static void free_request(struct request *r) {
	if (r->cancellable && r->cancelled_id) {
		g_cancellable_disconnect(r->cancellable, r->cancelled_id);
	}
	g_clear_object(&r->cancellable);
	g_list_free_full(r->identities, g_object_unref);
	g_free(r->action_id);
	g_free(r->message);
	g_free(r->cookie);
	g_free(r->program);
	if (r->task) { // none in a preview
		g_object_unref(r->task);
	}
	g_free(r);
}

static void start_next(void);

static void finish_request(gboolean gained, GError *error) {
	struct request *r = agent.current;
	if (!r) {
		if (error) {
			g_error_free(error);
		}
		return;
	}
	agent.current = NULL;
	if (agent.session) {
		PolkitAgentSession *old = agent.session;
		agent.session = NULL;
		g_signal_handlers_disconnect_matched(old, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL,
			&agent);
		polkit_agent_session_cancel(old);
		g_object_unref(old);
	}
	clear_password();
	agent.waiting = agent.checking = agent.cancelling = false;
	agent.note[0] = agent.prompt[0] = '\0';
	if (agent.repeat_source) {
		g_source_remove(agent.repeat_source);
		agent.repeat_source = 0;
	}
	if (!r->task) {
		g_clear_error(&error);
	} else if (error) {
		g_task_return_error(r->task, error);
	} else {
		g_task_return_boolean(r->task, gained);
	}
	free_request(r);
	hide_surfaces();
	if (agent.preview) {
		g_main_loop_quit(agent.loop);
		return;
	}
	start_next();
}

static void start_next(void) {
	if (agent.current) {
		return;
	}
	struct request *r = g_queue_pop_head(&agent.queue);
	if (!r) {
		return;
	}
	agent.current = r;
	agent.note[0] = '\0';
	clear_password();
	show_surfaces();
	begin_session();
	render_all();
}

static void cancel_current_later(gpointer data) {
	cancel_current();
}

static void free_request_later(gpointer data) {
	free_request(data);
}

/* polkit gave up on it (the app went away): it goes without an answer. */
static void request_cancelled(GCancellable *cancellable, gpointer data) {
	struct request *r = data;
	if (r == agent.current) {
		// not from within the handler: disconnecting it there would deadlock
		g_idle_add_once(cancel_current_later, NULL);
	} else if (g_queue_remove(&agent.queue, r)) {
		g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED,
			"Cancelled by polkit");
		r->cancelled_id = 0; // disconnecting it here would deadlock
		g_idle_add_once(free_request_later, r);
	}
}

/* ---------- the listener ---------- */

#define TW_TYPE_LISTENER (tw_listener_get_type())
G_DECLARE_FINAL_TYPE(TwListener, tw_listener, TW, LISTENER, PolkitAgentListener)

struct _TwListener {
	PolkitAgentListener parent;
};

G_DEFINE_TYPE(TwListener, tw_listener, POLKIT_AGENT_TYPE_LISTENER)

static void initiate_authentication(PolkitAgentListener *listener, const gchar *action_id,
		const gchar *message, const gchar *icon_name, PolkitDetails *details,
		const gchar *cookie, GList *identities, GCancellable *cancellable,
		GAsyncReadyCallback callback, gpointer user_data) {
	struct request *r = g_new0(struct request, 1);
	r->task = g_task_new(listener, cancellable, callback, user_data);
	r->action_id = g_strdup(action_id);
	r->message = g_strdup(message);
	r->cookie = g_strdup(cookie);
	const char *program = details ? polkit_details_lookup(details, "command_line") : NULL;
	if (!program && details) {
		program = polkit_details_lookup(details, "program");
	}
	r->program = program ? g_strdup(program) : NULL;
	r->identities = g_list_copy(identities);
	g_list_foreach(r->identities, (GFunc)(void (*)(void))g_object_ref, NULL);
	// the user of the session first, when it may answer
	uid_t me = getuid();
	int index = 0;
	for (GList *l = r->identities; l; l = l->next, index++) {
		if (POLKIT_IS_UNIX_USER(l->data) &&
				polkit_unix_user_get_uid(POLKIT_UNIX_USER(l->data)) == (gint)me) {
			r->identity = index;
			break;
		}
	}
	if (cancellable) {
		r->cancellable = g_object_ref(cancellable);
		r->cancelled_id = g_cancellable_connect(cancellable, G_CALLBACK(request_cancelled),
			r, NULL);
	}
	g_queue_push_tail(&agent.queue, r);
	start_next();
}

static gboolean initiate_authentication_finish(PolkitAgentListener *listener,
		GAsyncResult *result, GError **error) {
	return g_task_propagate_boolean(G_TASK(result), error);
}

static void tw_listener_class_init(TwListenerClass *klass) {
	PolkitAgentListenerClass *listener = POLKIT_AGENT_LISTENER_CLASS(klass);
	listener->initiate_authentication = initiate_authentication;
	listener->initiate_authentication_finish = initiate_authentication_finish;
}

static void tw_listener_init(TwListener *self) {
}

/* ---------- input ---------- */

static enum hit hit_at(double x, double y) {
	if (!agent.main) {
		return HIT_NONE;
	}
	struct { double x, y, w, h; } *boxes[] = { (void *)&agent.yes, (void *)&agent.no,
		(void *)&agent.user, (void *)&agent.close };
	enum hit hits[] = { HIT_YES, HIT_NO, HIT_USER, HIT_CLOSE };
	for (size_t i = 0; i < 4; i++) {
		if (boxes[i]->w > 0 && x >= boxes[i]->x && x < boxes[i]->x + boxes[i]->w &&
				y >= boxes[i]->y && y < boxes[i]->y + boxes[i]->h) {
			return hits[i];
		}
	}
	return HIT_NONE;
}

static void handle_key(xkb_keysym_t sym, const char *utf8) {
	if (!agent.current) {
		return;
	}
	uint32_t ctrl = agent.xkb_state && xkb_state_mod_name_is_active(agent.xkb_state,
		XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		submit();
		return;
	case XKB_KEY_Escape:
		cancel_current();
		return;
	case XKB_KEY_Up:
		next_identity(-1);
		return;
	case XKB_KEY_Down:
		next_identity(1);
		return;
	case XKB_KEY_BackSpace:
		if (agent.waiting && agent.password_len > 0) {
			// a whole character, not a byte of it
			const char *prev = g_utf8_find_prev_char(agent.password,
				agent.password + agent.password_len);
			size_t keep = prev ? (size_t)(prev - agent.password) : 0;
			explicit_bzero(agent.password + keep, agent.password_len - keep);
			agent.password_len = keep;
			render_all();
		}
		return;
	default:
		break;
	}
	if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
		clear_password();
		render_all();
		return;
	}
	size_t len = strlen(utf8);
	if (agent.waiting && len > 0 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
			agent.password_len + len < PASSWORD_MAX) {
		memcpy(agent.password + agent.password_len, utf8, len);
		agent.password_len += len;
		agent.password[agent.password_len] = '\0';
		render_all();
	}
}

static gboolean repeat_again(gpointer data) {
	handle_key(agent.repeat_sym, agent.repeat_utf8);
	return G_SOURCE_CONTINUE;
}

static gboolean repeat_start(gpointer data) {
	handle_key(agent.repeat_sym, agent.repeat_utf8);
	agent.repeat_source = agent.repeat_rate > 0 ?
		g_timeout_add(1000 / agent.repeat_rate, repeat_again, NULL) : 0;
	return G_SOURCE_REMOVE;
}

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd,
		uint32_t size) {
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(agent.xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		return;
	}
	if (agent.xkb_state) {
		xkb_state_unref(agent.xkb_state);
	}
	if (agent.keymap) {
		xkb_keymap_unref(agent.keymap);
	}
	agent.keymap = keymap;
	agent.xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface) {
	if (agent.repeat_source) {
		g_source_remove(agent.repeat_source);
		agent.repeat_source = 0;
	}
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	if (!agent.xkb_state) {
		return;
	}
	if (agent.repeat_source) {
		g_source_remove(agent.repeat_source);
		agent.repeat_source = 0;
	}
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}
	xkb_keycode_t code = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(agent.xkb_state, code);
	char utf8[8] = "";
	xkb_state_key_get_utf8(agent.xkb_state, code, utf8, sizeof(utf8));
	handle_key(sym, utf8);
	if (agent.current && agent.keymap && xkb_keymap_key_repeats(agent.keymap, code) &&
			agent.repeat_rate > 0 && sym != XKB_KEY_Return && sym != XKB_KEY_KP_Enter &&
			sym != XKB_KEY_Escape) {
		agent.repeat_sym = sym;
		snprintf(agent.repeat_utf8, sizeof(agent.repeat_utf8), "%s", utf8);
		agent.repeat_source = g_timeout_add(agent.repeat_delay, repeat_start, NULL);
	}
	explicit_bzero(utf8, sizeof(utf8));
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	if (agent.xkb_state) {
		xkb_state_update_mask(agent.xkb_state, depressed, latched, locked, 0, 0, group);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
		int32_t delay) {
	agent.repeat_rate = rate;
	agent.repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void set_hover(enum hit hit) {
	if (hit != agent.hover) {
		agent.hover = hit;
		render_all();
	}
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	agent.pointer_surface = surface;
	agent.px = wl_fixed_to_double(x);
	agent.py = wl_fixed_to_double(y);
	if (agent.cursor_shape) {
		struct wp_cursor_shape_device_v1 *device =
			wp_cursor_shape_manager_v1_get_pointer(agent.cursor_shape, p);
		wp_cursor_shape_device_v1_set_shape(device, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
	}
	set_hover(agent.main && surface == agent.main->surface ? hit_at(agent.px, agent.py) :
		HIT_NONE);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface) {
	agent.pointer_surface = NULL;
	set_hover(HIT_NONE);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {
	agent.px = wl_fixed_to_double(x);
	agent.py = wl_fixed_to_double(y);
	set_hover(agent.main && agent.pointer_surface == agent.main->surface ?
		hit_at(agent.px, agent.py) : HIT_NONE);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (button != BTN_LEFT) {
		return;
	}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		agent.pressed = agent.hover;
		render_all();
		return;
	}
	enum hit pressed = agent.pressed;
	agent.pressed = HIT_NONE;
	if (pressed != HIT_NONE && pressed == agent.hover) {
		switch (pressed) {
		case HIT_YES:
			submit();
			break;
		case HIT_NO:
		case HIT_CLOSE:
			cancel_current();
			break;
		case HIT_USER:
			next_identity(1);
			break;
		default:
			break;
		}
	}
	render_all();
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
}

static void pointer_frame(void *data, struct wl_pointer *p) {
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
		int32_t discrete) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !agent.keyboard) {
		agent.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(agent.keyboard, &keyboard_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && agent.keyboard) {
		wl_keyboard_release(agent.keyboard);
		agent.keyboard = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !agent.pointer) {
		agent.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(agent.pointer, &pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && agent.pointer) {
		wl_pointer_release(agent.pointer);
		agent.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------- outputs ---------- */

static void output_geometry(void *data, struct wl_output *out, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
}

static void output_mode(void *data, struct wl_output *out, uint32_t flags, int32_t w,
		int32_t h, int32_t refresh) {
}

static void output_done(void *data, struct wl_output *out) {
	render_output(data);
}

static void output_scale(void *data, struct wl_output *out, int32_t factor) {
	struct agent_output *o = data;
	o->scale = factor;
}

static void output_name(void *data, struct wl_output *out, const char *name) {
	struct agent_output *o = data;
	free(o->name);
	o->name = strdup(name);
}

static void output_description(void *data, struct wl_output *out, const char *description) {
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		agent.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		agent.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		agent.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
			version < 4 ? version : 4);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		agent.cursor_shape = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !agent.seat) {
		agent.seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(agent.seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct agent_output *o = calloc(1, sizeof(*o));
		o->wl_name = name;
		o->scale = 1;
		o->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &output_listener, o);
		wl_list_insert(agent.outputs.prev, &o->link);
		if (agent.current) {
			create_surface(o); // a screen plugged in meanwhile darkens too
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct agent_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &agent.outputs, link) {
		if (o->wl_name != name) {
			continue;
		}
		bool was_main = o == agent.main;
		destroy_surface(o);
		wl_output_destroy(o->wl_output);
		wl_list_remove(&o->link);
		free(o->name);
		free(o);
		if (was_main && agent.current) {
			// the dialog goes to a screen that is still there
			hide_surfaces();
			show_surfaces();
			render_all();
		}
		return;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------- the Wayland connection in the GLib main loop ---------- */

struct wl_source {
	GSource source;
	gpointer fd_tag;
	bool reading;
};

static gboolean wl_source_prepare(GSource *source, gint *timeout) {
	struct wl_source *s = (struct wl_source *)source;
	*timeout = -1;
	if (s->reading) {
		return FALSE;
	}
	while (wl_display_prepare_read(agent.display) != 0) {
		if (wl_display_dispatch_pending(agent.display) < 0) {
			return TRUE;
		}
	}
	s->reading = true;
	wl_display_flush(agent.display);
	return FALSE;
}

static gboolean wl_source_check(GSource *source) {
	struct wl_source *s = (struct wl_source *)source;
	GIOCondition revents = g_source_query_unix_fd(source, s->fd_tag);
	if (s->reading) {
		s->reading = false;
		if (revents & G_IO_IN) {
			if (wl_display_read_events(agent.display) < 0) {
				return TRUE;
			}
		} else {
			wl_display_cancel_read(agent.display);
		}
	}
	return (revents & (G_IO_IN | G_IO_ERR | G_IO_HUP)) != 0;
}

static gboolean wl_source_dispatch(GSource *source, GSourceFunc callback, gpointer data) {
	struct wl_source *s = (struct wl_source *)source;
	GIOCondition revents = g_source_query_unix_fd(source, s->fd_tag);
	if ((revents & (G_IO_ERR | G_IO_HUP)) ||
			wl_display_dispatch_pending(agent.display) < 0) {
		// the compositor is gone, and with it the session the agent was for
		g_main_loop_quit(data);
		return G_SOURCE_REMOVE;
	}
	wl_display_flush(agent.display);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs wl_source_funcs = {
	.prepare = wl_source_prepare,
	.check = wl_source_check,
	.dispatch = wl_source_dispatch,
};

/* ---------- the theme ---------- */

static uint32_t dialog_color(const char *name, uint32_t fallback) {
	char key[64];
	snprintf(key, sizeof(key), "menu.%s", name);
	fallback = tw_theme_color(agent.theme, key, fallback);
	snprintf(key, sizeof(key), "flyout.%s", name);
	return tw_theme_color(agent.theme, key, fallback);
}

static enum era era_of(const struct tw_theme *t) {
	const char *s = t && t->style ? t->style : "win10";
	static const struct {
		const char *name;
		enum era era;
	} eras[] = {
		{ "win1", ERA_WIN1 }, { "win3", ERA_WIN3 }, { "win31", ERA_WIN3 },
		{ "win95", ERA_WIN95 }, { "classic", ERA_WIN95 }, { "winxp", ERA_XP },
		{ "luna", ERA_XP }, { "win7", ERA_WIN7 }, { "aero", ERA_WIN7 },
		{ "win8", ERA_WIN8 }, { "metro", ERA_WIN8 }, { "win11", ERA_WIN11 },
		{ "fluent", ERA_WIN11 },
	};
	for (size_t i = 0; i < sizeof(eras) / sizeof(eras[0]); i++) {
		if (strcasecmp(s, eras[i].name) == 0) {
			return eras[i].era;
		}
	}
	return ERA_WIN10;
}

static void load_theme(void) {
	char *name = tw_theme_current_name();
	char *error = NULL;
	agent.theme = tw_theme_load(name, &error);
	free(error);
	free(name);
	const struct tw_theme *t = agent.theme;
	enum era era = agent.era = era_of(t);
	agent.framed = era <= ERA_WIN8;
	agent.dither = era <= ERA_WIN95;
	agent.accent = tw_theme_color(t, "flyout.accent",
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff));
	agent.font = tw_theme_str(t, "menu.font", tw_theme_str(t, "panel.font", "Noto Sans 9"));
	agent.bold = tw_theme_str(t, "panel.bold_font", agent.font);
	agent.strip = agent.strip_line = 0;
	agent.error = 0xc42b1cff;
	bool old = era <= ERA_XP;
	agent.question = old ? "This program needs an administrator to continue." :
		era <= ERA_WIN8 ? "Do you want to allow the following program to make changes to "
		"this computer?" : "Do you want to allow this app to make changes to your device?";
	agent.yes_label = old ? "OK" : "Yes";
	agent.no_label = old ? "Cancel" : "No";

	switch (era) {
	case ERA_WIN1:
	case ERA_WIN3:
		agent.body = 0xffffffff;
		agent.fg = agent.instruction = 0x000000ff;
		agent.dim = 0x404040ff;
		agent.field_bg = 0xffffffff;
		agent.field_fg = 0x000000ff;
		agent.field_line = agent.field_focus = 0x000000ff;
		agent.dark = false;
		return;
	case ERA_WIN95:
		agent.body = tw_theme_color(t, "decoration.face", 0xc0c0c0ff);
		agent.fg = agent.instruction = 0x000000ff;
		agent.dim = 0x404040ff;
		agent.field_bg = 0xffffffff;
		agent.field_fg = 0x000000ff;
		agent.field_line = agent.field_focus = 0x808080ff;
		agent.dark = false;
		return;
	default:
		break;
	}

	// the newer ones are light or dark as the theme's menus are
	uint32_t menu = dialog_color("bg", 0xf2f2f2ff) | 0xff;
	int luma = (int)((menu >> 24 & 0xff) * 299 + (menu >> 16 & 0xff) * 587 +
		(menu >> 8 & 0xff) * 114) / 1000;
	bool dark = agent.dark = luma < 128;
	agent.fg = dark ? dialog_color("fg", 0xffffffff) : 0x000000ff;
	agent.dim = dark ? 0xa0a0a0ff : 0x5d5d5dff;
	agent.field_bg = dark ? 0x1f1f1fff : 0xffffffff;
	agent.field_fg = dark ? 0xffffffff : 0x000000ff;
	agent.field_focus = agent.accent;
	agent.error = dark ? 0xff99a4ff : 0xc42b1cff;
	if (dark) {
		agent.body = menu;
		agent.field_bg = mix(menu, 0x000000ff, 0.35); // a deeper shade of the dialog
		agent.instruction = era <= ERA_WIN8 ? 0x99c6ffff : agent.fg;
		agent.field_line = 0xffffff40;
		agent.strip = era == ERA_XP ? 0 : mix(menu, 0xffffffff, 0.06);
		agent.strip_line = mix(menu, 0xffffffff, 0.14);
		return;
	}
	switch (era) {
	case ERA_XP:
		agent.body = 0xece9d8ff; // the dialog face of Luna
		agent.instruction = 0x000000ff;
		agent.field_line = agent.field_focus = 0x7f9db9ff;
		break;
	case ERA_WIN7:
	case ERA_WIN8:
		// white with the main instruction in blue, the buttons in a grey band
		agent.body = 0xffffffff;
		agent.instruction = era == ERA_WIN7 ? 0x003399ff : 0x1e395bff;
		agent.field_line = 0xabadb3ff;
		agent.field_focus = era == ERA_WIN7 ? 0x3d7badff : 0x0078d7ff;
		agent.strip = 0xf0f0f0ff;
		agent.strip_line = 0xdfdfdfff;
		break;
	case ERA_WIN10:
		agent.body = 0xe6e6e6ff;
		agent.instruction = 0x000000ff;
		agent.field_line = 0x7a7a7aff;
		break;
	default: // Windows 11
		agent.body = 0xf9f9f9ff;
		agent.instruction = 0x000000ff;
		agent.field_line = 0x00000024;
		agent.strip = 0xf3f3f3ff;
		agent.strip_line = 0x0000000f;
		break;
	}
}

/* The shield, as an icon for the title bar of the frame. */
static void make_shield(void) {
	agent.shield = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
	cairo_t *cr = cairo_create(agent.shield);
	draw_shield(cr, 1, 1, 30);
	cairo_destroy(cr);
}

/* ---------- main ---------- */

static gboolean quit_on_signal(gpointer loop) {
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		printf("Usage: tilewin-polkit [--preview]\n"
			"The polkit authentication agent of the tileWin session: asks for the\n"
			"password when an app needs an administrator.\n"
			"--preview shows the dialog with a sample request in the look of the theme,\n"
			"without polkit; it exits 0 for Yes and 1 for No.\n");
		return 0;
	}
	agent.preview = argc > 1 && strcmp(argv[1], "--preview") == 0;
	setlocale(LC_ALL, "");
	mlock(agent.password, sizeof(agent.password));
	wl_list_init(&agent.outputs);
	g_queue_init(&agent.queue);
	agent.repeat_rate = 25;
	agent.repeat_delay = 600;
	agent.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	load_theme();
	make_shield();

	agent.display = wl_display_connect(NULL);
	if (!agent.display) {
		fprintf(stderr, "tilewin-polkit: cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(agent.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(agent.display);
	wl_display_roundtrip(agent.display);
	if (!agent.compositor || !agent.shm || !agent.layer_shell) {
		fprintf(stderr, "tilewin-polkit: the compositor has no layer shell\n");
		return 1;
	}

	if (agent.preview) {
		struct request *r = g_new0(struct request, 1);
		r->action_id = g_strdup("org.freedesktop.policykit.exec");
		r->message = g_strdup("Authentication is required to run a program as another user");
		r->program = g_strdup("/usr/bin/true");
		r->cookie = g_strdup("");
		r->identities = g_list_append(r->identities, polkit_unix_user_new((gint)getuid()));
		if (getuid() != 0) {
			r->identities = g_list_append(r->identities, polkit_unix_user_new(0));
		}
		agent.loop = g_main_loop_new(NULL, FALSE);
		GSource *source = g_source_new(&wl_source_funcs, sizeof(struct wl_source));
		((struct wl_source *)source)->fd_tag = g_source_add_unix_fd(source,
			wl_display_get_fd(agent.display), G_IO_IN | G_IO_ERR | G_IO_HUP);
		g_source_set_callback(source, NULL, agent.loop, NULL);
		g_source_attach(source, NULL);
		g_unix_signal_add(SIGTERM, quit_on_signal, agent.loop);
		g_unix_signal_add(SIGINT, quit_on_signal, agent.loop);
		g_queue_push_tail(&agent.queue, r);
		start_next();
		g_main_loop_run(agent.loop);
		clear_password();
		return agent.preview_yes ? 0 : 1;
	}

	GError *error = NULL;
	PolkitSubject *subject = polkit_unix_session_new_for_process_sync(getpid(), NULL, &error);
	if (!subject) {
		fprintf(stderr, "tilewin-polkit: not in a login session: %s\n",
			error ? error->message : "?");
		g_clear_error(&error);
		return 1;
	}
	PolkitAgentListener *listener = g_object_new(TW_TYPE_LISTENER, NULL);
	gpointer handle = polkit_agent_listener_register(listener,
		POLKIT_AGENT_REGISTER_FLAGS_NONE, subject, "/org/tilewin/PolicyKit1/AuthenticationAgent",
		NULL, &error);
	if (!handle) {
		// most likely another agent has the session already
		fprintf(stderr, "tilewin-polkit: cannot register: %s\n", error ? error->message : "?");
		g_clear_error(&error);
		return 1;
	}

	GMainLoop *loop = agent.loop = g_main_loop_new(NULL, FALSE);
	GSource *source = g_source_new(&wl_source_funcs, sizeof(struct wl_source));
	((struct wl_source *)source)->fd_tag = g_source_add_unix_fd(source,
		wl_display_get_fd(agent.display), G_IO_IN | G_IO_ERR | G_IO_HUP);
	g_source_set_callback(source, NULL, loop, NULL);
	g_source_attach(source, NULL);
	g_unix_signal_add(SIGTERM, quit_on_signal, loop);
	g_unix_signal_add(SIGINT, quit_on_signal, loop);
	g_main_loop_run(loop);

	if (agent.current) {
		finish_request(FALSE, g_error_new(POLKIT_ERROR, POLKIT_ERROR_CANCELLED,
			"The agent is going away"));
	}
	polkit_agent_listener_unregister(handle);
	g_object_unref(listener);
	g_object_unref(subject);
	clear_password();
	return 0;
}
