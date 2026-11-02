#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "config.h"
#include "pool-buffer.h"
#include "sessions.h"
#include "tw_desktop.h"
#include "tw_paths.h"
#include "tw_theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/*
 * tilewin-dm-greeter: the login screen itself, in the look of the theme. It runs
 * as a user of its own and can do nothing but draw and ask the daemon; see
 * main.c. With --preview it runs in a session of yours, checks nothing, and Esc
 * ends it, which is what the Login screen page of the settings shows.
 *
 * The first frame has the colors of the theme; the wallpaper, which can take a
 * while to load, comes right after it, so the screen is up at once.
 */
#define MAX_USERS 64
#define USER_ROWS 4
#define ROW_H 46
#define FIELD_H 38
#define PAD 20
#define TITLE_H 30

enum area {
	AREA_NONE,
	AREA_USER,
	AREA_NAME,
	AREA_PASSWORD,
	AREA_REVEAL,
	AREA_SESSION_PREV,
	AREA_SESSION_NEXT,
	AREA_LAYOUT,
	AREA_LOGIN,
	AREA_POWER,
	AREA_CANCEL,
};

enum look {
	LOOK_CARD, // a card, as card_style says, on any theme
	LOOK_WIN10,
	LOOK_WIN11,
	LOOK_WIN8,
	LOOK_AERO,
	LOOK_XP,
	LOOK_DIALOG,
	LOOK_OLD,
	LOOK_SWAY,
};

enum stage {
	STAGE_IDLE,
	STAGE_CHECKING,
	STAGE_PROMPT, // a further question of PAM: a code, or a new password
	STAGE_DONE,
};

struct hit {
	double x, y, w, h;
	enum area area;
	int index;
};

struct output {
	struct wl_output *wl_output;
	uint32_t global_name;
	int scale;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	int width, height;
	bool configured, first;
	struct pool_buffer buffers[2];
	cairo_surface_t *background;
	int bg_w, bg_h;
	bool bg_full;
	struct hit hits[MAX_USERS + 16];
	int hit_count;
	struct wl_list link;
};

struct user {
	char *name, *full;
	cairo_surface_t *avatar;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_list outputs;
	struct output *pointer_output;
	double px, py;
	bool dirty, running, preview;

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	uint32_t group;

	struct dm_config *config;
	struct tw_theme *theme;
	uint32_t accent, card_bg, fg, field_bg, field_fg, title_bg, title_fg;
	const char *font, *bold;
	const char *gradient;
	bool classic, gradient_sideways;
	enum look look;
	double radius;

	struct user users[MAX_USERS];
	int user_count, user_sel; // user_sel == user_count: a name is typed
	struct dm_session_list *sessions;
	int session_sel;

	char name[256];
	size_t name_len;
	char password[512];
	size_t password_len;
	bool reveal;
	enum area focus;

	enum stage stage;
	bool answer_pending; // the first prompt gets what is typed already
	char prompt[256];
	bool prompt_secret;
	char message[512];
	bool message_error;

	int fd;
	char clock_text[64];
	struct timespec last_input;
	bool screen_off;
	char *hostname;
} g = { .fd = -1 };

static void touch_input(void) {
	clock_gettime(CLOCK_MONOTONIC, &g.last_input);
}

static long idle_seconds(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec - g.last_input.tv_sec;
}

static void clear_secret(void) {
	explicit_bzero(g.password, sizeof(g.password));
	g.password_len = 0;
	g.reveal = false;
}

static void set_message(const char *text, bool error) {
	snprintf(g.message, sizeof(g.message), "%s", text ? text : "");
	g.message_error = error;
	g.dirty = true;
}

static void color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

/* ---------- who and what ---------- */

static bool user_listed(const struct passwd *pw) {
	int min = dm_config_int(g.config, "min_uid"), max = dm_config_int(g.config, "max_uid");
	if ((int)pw->pw_uid < min || (int)pw->pw_uid > max) {
		return false;
	}
	if (dm_word_in_list(dm_config_str(g.config, "hide_users"), pw->pw_name)) {
		return false;
	}
	const char *shell = pw->pw_shell ? pw->pw_shell : "";
	return !strstr(shell, "nologin") && !strstr(shell, "/false");
}

static cairo_surface_t *load_avatar(const struct passwd *pw) {
	if (!dm_config_bool(g.config, "show_avatars")) {
		return NULL;
	}
	char path[1024];
	snprintf(path, sizeof(path), "/var/lib/AccountsService/icons/%s", pw->pw_name);
	if (access(path, R_OK) == 0) {
		return tw_image_load(path);
	}
	snprintf(path, sizeof(path), "%s/.face", pw->pw_dir);
	return access(path, R_OK) == 0 ? tw_image_load(path) : NULL;
}

static int user_cmp(const void *a, const void *b) {
	return strcasecmp(((const struct user *)a)->name, ((const struct user *)b)->name);
}

static void read_state_word(const char *name, char *out, size_t size) {
	out[0] = '\0';
	const char *dir = getenv("TILEWIN_DM_STATE");
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir ? dir : "/var/lib/tilewin-dm/shared", name);
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(out, size, f)) {
			out[0] = '\0';
		}
		fclose(f);
	}
	out[strcspn(out, "\r\n")] = '\0';
}

static void read_users(void) {
	if (dm_config_bool(g.config, "show_users")) {
		setpwent();
		struct passwd *pw;
		while ((pw = getpwent()) && g.user_count < MAX_USERS) {
			if (user_listed(pw)) {
				struct user *u = &g.users[g.user_count++];
				u->name = strdup(pw->pw_name);
				u->full = pw->pw_gecos && *pw->pw_gecos && dm_config_bool(g.config, "show_full_names") ?
					strndup(pw->pw_gecos, strcspn(pw->pw_gecos, ",")) : NULL;
				u->avatar = load_avatar(pw);
			}
		}
		endpwent();
		qsort(g.users, g.user_count, sizeof(g.users[0]), user_cmp);
	}
	char wanted[256] = "";
	if (dm_config_bool(g.config, "remember_user")) {
		read_state_word("last-user", wanted, sizeof(wanted));
	}
	if (!wanted[0]) {
		snprintf(wanted, sizeof(wanted), "%s", dm_config_str(g.config, "default_user"));
	}
	g.user_sel = g.user_count > 0 ? 0 : g.user_count;
	bool found = false;
	for (int i = 0; i < g.user_count; i++) {
		if (strcmp(g.users[i].name, wanted) == 0) {
			g.user_sel = i;
			found = true;
		}
	}
	// someone typed last time, or nothing is listed: the name is ready in the field
	if (!found && wanted[0] && g.user_count == 0) {
		snprintf(g.name, sizeof(g.name), "%s", wanted);
		g.name_len = strlen(g.name);
	}
	g.focus = g.user_sel < g.user_count || g.name_len > 0 ? AREA_PASSWORD : AREA_NAME;
}

static void read_sessions(void) {
	g.sessions = dm_sessions_read();
	char wanted[256] = "";
	if (dm_config_bool(g.config, "remember_session")) {
		read_state_word("last-session", wanted, sizeof(wanted));
	}
	const char *fallback = dm_config_str(g.config, "default_session");
	g.session_sel = 0;
	for (size_t i = 0; i < g.sessions->count; i++) {
		if (strcmp(g.sessions->items[i].id, fallback) == 0) {
			g.session_sel = (int)i;
		}
	}
	for (size_t i = 0; i < g.sessions->count; i++) {
		if (strcmp(g.sessions->items[i].id, wanted) == 0) {
			g.session_sel = (int)i;
		}
	}
}

static void read_theme(void) {
	char *name = tw_theme_current_name();
	char *error = NULL;
	g.theme = tw_theme_load(name, &error);
	free(error);
	free(name);
	if (!g.theme) {
		error = NULL;
		g.theme = tw_theme_load(TW_DEFAULT_THEME, &error);
		free(error);
	}
	const struct tw_theme *t = g.theme;
	const char *style = t && t->style ? t->style : "";
	g.classic = strcmp(style, "win95") == 0 || strcmp(style, "win2000") == 0 ||
		strcmp(style, "win1") == 0 || strcmp(style, "win3") == 0;
	g.radius = strcmp(style, "win11") == 0 || strcmp(style, "sway") == 0 ? 8 :
		strcmp(style, "win7") == 0 || strcmp(style, "vista") == 0 ? 6 :
		strcmp(style, "winxp") == 0 ? 4 : 0;
	// what the theme highlights with: its accent, or else the selection of its menus
	uint32_t highlight = tw_theme_color(t, "menu.hl_bg", 0);
	g.accent = tw_theme_color(t, "accent.color", (highlight & 0xff) == 0xff ? highlight :
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff));
	const char *accent = dm_config_str(g.config, "accent");
	if (strcmp(accent, "theme") != 0) {
		tw_parse_color(accent, &g.accent);
	}
	g.card_bg = tw_theme_color(t, "menu.bg", 0xf2f2f2ff);
	g.fg = tw_theme_color(t, "menu.fg", 0x000000ff);
	g.field_bg = tw_theme_color(t, "menu.field_bg", 0xffffffff);
	g.field_fg = tw_theme_color(t, "menu.field_fg", 0x000000ff);
	g.title_bg = tw_theme_color(t, "decoration.active.title_bg", g.accent);
	g.title_fg = tw_theme_color(t, "decoration.active.title_fg", 0xffffffff);
	g.gradient = tw_theme_str(t, "decoration.active.title_gradient", NULL);
	g.gradient_sideways = g.gradient && strchr(g.gradient, ' ') &&
		!strchr(strchr(g.gradient, ' ') + 1, ' ');
	const char *font = dm_config_str(g.config, "font");
	g.font = strcmp(font, "theme") != 0 ? font :
		tw_theme_str(t, "menu.font", tw_theme_str(t, "panel.font", "Noto Sans 10"));
	g.bold = tw_theme_str(t, "panel.bold_font", g.font);

	// as that Windows looked when it asked who you are, unless asked for a card
	const char *theme_name = t && t->name ? t->name : "";
	g.look = strcmp(dm_config_str(g.config, "card_style"), "theme") != 0 ? LOOK_CARD :
		strcmp(theme_name, "sway") == 0 ? LOOK_SWAY :
		strcmp(style, "win11") == 0 ? LOOK_WIN11 : strcmp(style, "win10") == 0 ? LOOK_WIN10 :
		strcmp(style, "win8") == 0 ? LOOK_WIN8 : strcmp(style, "win7") == 0 ? LOOK_AERO :
		strcmp(style, "winxp") == 0 ? LOOK_XP :
		strcmp(style, "win1") == 0 || strcmp(style, "win3") == 0 ? LOOK_OLD :
		strcmp(style, "win95") == 0 ? LOOK_DIALOG : LOOK_CARD;
	// white fields on all that have them, whatever the menus of the theme say
	if (g.look != LOOK_CARD && g.look != LOOK_DIALOG && g.look != LOOK_SWAY) {
		g.field_bg = 0xffffffff;
		g.field_fg = 0x000000ff;
	}
	if (g.look == LOOK_OLD) {
		g.card_bg = 0xffffffff;
		g.fg = 0x000000ff;
		g.accent = 0x000000ff;
		g.field_bg = 0xffffffff;
		g.field_fg = 0x000000ff;
	}
	if (g.look == LOOK_SWAY) {
		g.field_bg = 0x313244ff;
		g.field_fg = 0xcdd6f4ff;
	}
}

/* ---------- wallpaper ---------- */

/* full: also a picture, which may take a while; else only the colors. */
static cairo_surface_t *render_background(int w, int h, bool full) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(out);
	const struct tw_theme *t = g.theme;
	uint32_t c1 = tw_theme_color(t, "wallpaper.color", 0x1c3a6eff);
	uint32_t c2 = tw_theme_color(t, "wallpaper.color2", c1);
	const char *setting = dm_config_str(g.config, "wallpaper");
	char *image = NULL;
	if (setting[0] == '#') {
		tw_parse_color(setting, &c1);
		c2 = c1;
	} else if (strcmp(setting, "theme") != 0) {
		image = strdup(setting);
	} else if (t) {
		const char *name = tw_theme_str(t, "wallpaper.image", NULL);
		if (name && strcasecmp(tw_theme_str(t, "wallpaper.type", ""), "image") == 0 && name[0] != '~') {
			image = name[0] == '/' ? strdup(name) : tw_theme_file(t, name);
		}
	}
	bool vertical = strcasecmp(tw_theme_str(t, "wallpaper.direction", "vertical"), "horizontal") != 0;
	cairo_pattern_t *p = vertical ? cairo_pattern_create_linear(0, 0, 0, h) :
		cairo_pattern_create_linear(0, 0, w, 0);
	uint32_t cs[2] = { c1, c2 };
	for (int i = 0; i < 2; i++) {
		cairo_pattern_add_color_stop_rgb(p, i, (cs[i] >> 24 & 0xff) / 255.0,
			(cs[i] >> 16 & 0xff) / 255.0, (cs[i] >> 8 & 0xff) / 255.0);
	}
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	if (full && image) {
		cairo_surface_t *picture = tw_image_render_cover(image, w, h);
		if (picture) {
			cairo_set_source_surface(cr, picture, 0, 0);
			cairo_paint(cr);
			cairo_surface_destroy(picture);
		}
	}
	free(image);
	int dim = dm_config_int(g.config, "dim");
	cairo_set_source_rgba(cr, 0, 0, 0, dim / 100.0);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

/* ---------- drawing ---------- */

static void add_hit(struct output *o, double x, double y, double w, double h, enum area area,
		int index) {
	if (o->hit_count < (int)(sizeof(o->hits) / sizeof(o->hits[0]))) {
		o->hits[o->hit_count++] = (struct hit){ x, y, w, h, area, index };
	}
}

static PangoLayout *make_layout(cairo_t *cr, const char *font, const char *str) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, str, -1);
	return layout;
}

enum { LEFT, MIDDLE, RIGHT };

/* Text in a box, up and down in the middle of it. */
static void text_in(cairo_t *cr, const char *font, const char *str, double x, double y, double w,
		double h, uint32_t c, int align) {
	PangoLayout *layout = make_layout(cr, font, str);
	pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, align == MIDDLE ? PANGO_ALIGN_CENTER :
		align == RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	color(cr, c);
	cairo_move_to(cr, x, y + (h - th) / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

/* White with a soft shadow, on whatever the wallpaper is. */
static void text_on_wallpaper(cairo_t *cr, const char *font, const char *str, double x, double y,
		double w, double h, int align) {
	text_in(cr, font, str, x + 1, y + 2, w, h, 0x00000060, align);
	text_in(cr, font, str, x, y, w, h, 0xffffffff, align);
}

static void box_path(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_path(cr);
	if (r <= 0) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path(cr);
}

/* The raised or sunken edge of the old Windows looks. */
static void bevel(cairo_t *cr, double x, double y, double w, double h, bool sunken) {
	uint32_t light = 0xffffffff, dark = 0x808080ff;
	color(cr, sunken ? dark : light);
	cairo_rectangle(cr, x, y, w, 1);
	cairo_rectangle(cr, x, y, 1, h);
	cairo_fill(cr);
	color(cr, sunken ? light : dark);
	cairo_rectangle(cr, x, y + h - 1, w, 1);
	cairo_rectangle(cr, x + w - 1, y, 1, h);
	cairo_fill(cr);
}

static void field_frame(cairo_t *cr, double x, double y, double w, double h, bool focused) {
	double r = g.radius > 4 ? 4 : g.radius;
	box_path(cr, x, y, w, h, r);
	color(cr, g.field_bg);
	cairo_fill(cr);
	if (g.classic) {
		bevel(cr, x, y, w, h, true);
	}
	box_path(cr, x + 0.5, y + 0.5, w - 1, h - 1, r);
	color(cr, focused ? g.accent : (g.fg & 0xffffff00) | 0x40);
	cairo_set_line_width(cr, focused ? 2 : 1);
	cairo_stroke(cr);
}

static void draw_avatar(cairo_t *cr, const struct user *u, double cx, double cy, double r) {
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_clip(cr);
	if (u && u->avatar) {
		double iw = cairo_image_surface_get_width(u->avatar);
		double ih = cairo_image_surface_get_height(u->avatar);
		double s = 2 * r / (iw < ih ? iw : ih);
		cairo_translate(cr, cx - iw * s / 2, cy - ih * s / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, u->avatar, 0, 0);
		cairo_paint(cr);
	} else {
		color(cr, g.accent);
		cairo_paint(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy - r * 0.18, r * 0.36, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy + r * 0.95, r * 0.72, M_PI, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* Dots, stars or nothing, or the text itself with the reveal button or a question. */
static void draw_secret(cairo_t *cr, double x, double y, double w, double h) {
	const char *style = dm_config_str(g.config, "password_style");
	size_t chars = 0;
	for (size_t i = 0; i < g.password_len; i++) {
		chars += ((unsigned char)g.password[i] & 0xc0) != 0x80;
	}
	bool plain = g.reveal || (g.stage == STAGE_PROMPT && !g.prompt_secret);
	if (plain) {
		text_in(cr, g.font, g.password, x, y, w, h, g.field_fg, LEFT);
	} else if (strcmp(style, "hidden") == 0) {
		return;
	} else if (strcmp(style, "stars") == 0) {
		char stars[64];
		size_t n = chars < sizeof(stars) - 1 ? chars : sizeof(stars) - 1;
		memset(stars, '*', n);
		stars[n] = '\0';
		text_in(cr, g.font, stars, x, y, w, h, g.field_fg, LEFT);
	} else {
		color(cr, g.field_fg);
		double dx = x + 4;
		for (size_t i = 0; i < chars && dx < x + w - 4; i++, dx += 13) {
			cairo_new_path(cr);
			cairo_arc(cr, dx, y + h / 2, 3.6, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
}

static void draw_eye(cairo_t *cr, double cx, double cy, uint32_t c, bool crossed) {
	color(cr, c);
	cairo_set_line_width(cr, 1.5);
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 8, cy);
	cairo_curve_to(cr, cx - 4, cy - 6, cx + 4, cy - 6, cx + 8, cy);
	cairo_curve_to(cr, cx + 4, cy + 6, cx - 4, cy + 6, cx - 8, cy);
	cairo_stroke(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 2.2, 0, 2 * M_PI);
	cairo_fill(cr);
	if (crossed) {
		cairo_move_to(cr, cx - 7, cy + 7);
		cairo_line_to(cr, cx + 7, cy - 7);
		cairo_stroke(cr);
	}
}

static int card_rows(void) {
	return g.user_count > 0 ? (g.user_count + 1 < USER_ROWS ? g.user_count + 1 : USER_ROWS) : 0;
}

static bool typing_name(void) {
	return g.user_sel >= g.user_count;
}

static bool has_layouts(void) {
	return g.keymap && dm_config_bool(g.config, "show_layout") &&
		xkb_keymap_num_layouts(g.keymap) > 1;
}

static const char *card_title(char *buffer, size_t size) {
	const char *format = dm_config_str(g.config, "card_title");
	size_t o = 0;
	for (const char *p = format; *p && o + 1 < size; p++) {
		if (p[0] == '%' && p[1] == 'h') {
			o += snprintf(buffer + o, size - o, "%s", g.hostname);
			if (o >= size) {
				o = size - 1;
			}
			p++;
		} else {
			buffer[o++] = *p;
		}
	}
	buffer[o] = '\0';
	return buffer;
}

static void draw_card(struct output *o, cairo_t *cr, double W, double H) {
	double cw = dm_config_int(g.config, "card_width");
	if (cw > W - 20) {
		cw = W - 20;
	}
	bool window = strcmp(dm_config_str(g.config, "card_style"), "window") == 0;
	const char *greeting = dm_config_str(g.config, "greeting");
	bool sessions = dm_config_bool(g.config, "show_sessions") && g.sessions->count > 1;
	bool layouts = has_layouts();
	double inner = (window ? TITLE_H : 0) + PAD;
	double list_h = card_rows() * ROW_H;
	double name_h = typing_name() ? FIELD_H + 10 : 0;
	double session_h = sessions ? 34 : 0;
	double layout_h = layouts ? 30 : 0;
	double greet_h = *greeting ? 26 : 0;
	double ch = inner + greet_h + list_h + name_h + FIELD_H + 10 + session_h + layout_h + 38 + 30 + PAD;
	const char *place = dm_config_str(g.config, "card_position");
	double cx = strcmp(place, "left") == 0 ? W * 0.07 :
		strcmp(place, "right") == 0 ? W * 0.93 - cw : (W - cw) / 2;
	double cy = (H - ch) / 2;
	if (cy < 8) {
		cy = 8;
	}

	// the card, in the frame of the theme
	cairo_save(cr);
	box_path(cr, cx + 2, cy + 3, cw, ch, g.radius);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
	cairo_fill(cr);
	box_path(cr, cx, cy, cw, ch, g.radius);
	color(cr, g.card_bg);
	cairo_fill_preserve(cr);
	cairo_clip(cr);
	if (window) {
		if (g.gradient) {
			cairo_pattern_t *p = g.gradient_sideways ?
				cairo_pattern_create_linear(cx, 0, cx + cw, 0) :
				cairo_pattern_create_linear(0, cy, 0, cy + TITLE_H);
			char *end;
			for (const char *s = g.gradient; *s;) {
				double at = strtod(s, &end);
				if (end == s || *end != ':') {
					break;
				}
				uint32_t c = 0x000000ff;
				char word[16];
				size_t n = strcspn(end + 1, " ");
				snprintf(word, sizeof(word), "%.*s", (int)n, end + 1);
				tw_parse_color(word, &c);
				cairo_pattern_add_color_stop_rgb(p, at, (c >> 24 & 0xff) / 255.0,
					(c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0);
				s = end + 1 + n;
				s += strspn(s, " ");
			}
			cairo_set_source(cr, p);
			cairo_rectangle(cr, cx, cy, cw, TITLE_H);
			cairo_fill(cr);
			cairo_pattern_destroy(p);
		} else {
			color(cr, g.title_bg);
			cairo_rectangle(cr, cx, cy, cw, TITLE_H);
			cairo_fill(cr);
		}
	}
	cairo_restore(cr);
	if (g.classic) {
		bevel(cr, cx, cy, cw, ch, false);
	} else {
		box_path(cr, cx + 0.5, cy + 0.5, cw - 1, ch - 1, g.radius);
		color(cr, (g.fg & 0xffffff00) | 0x30);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	char title[256];
	card_title(title, sizeof(title));
	if (window) {
		text_in(cr, g.bold, title, cx + 10, cy, cw - 20, TITLE_H, g.title_fg, LEFT);
	}

	double x = cx + PAD, w = cw - 2 * PAD, y = cy + inner;
	if (*greeting) {
		text_in(cr, g.font, greeting, x, y - 4, w, 22, (g.fg & 0xffffff00) | 0xc0, LEFT);
		y += greet_h;
	}

	// who
	if (g.user_count > 0) {
		int rows = card_rows();
		int total = g.user_count + 1;
		int first = g.user_sel - rows / 2;
		first = first > total - rows ? total - rows : first;
		first = first < 0 ? 0 : first;
		for (int i = first; i < first + rows && i < total; i++) {
			double ry = y + (i - first) * ROW_H;
			bool sel = i == g.user_sel;
			if (sel) {
				box_path(cr, x - 6, ry + 1, w + 12, ROW_H - 2, g.radius > 4 ? 4 : g.radius);
				color(cr, (g.accent & 0xffffff00) | 0x38);
				cairo_fill(cr);
			}
			const struct user *u = i < g.user_count ? &g.users[i] : NULL;
			draw_avatar(cr, u, x + 18, ry + ROW_H / 2.0, 16);
			const char *shown = !u ? "Another user" : u->full && *u->full ? u->full : u->name;
			text_in(cr, sel ? g.bold : g.font, shown, x + 46, ry, w - 46, ROW_H, g.fg, LEFT);
			add_hit(o, x - 6, ry, w + 12, ROW_H, AREA_USER, i);
		}
		y += list_h;
	}
	if (typing_name()) {
		field_frame(cr, x, y, w, FIELD_H, g.focus == AREA_NAME);
		if (g.name_len) {
			text_in(cr, g.font, g.name, x + 10, y, w - 20, FIELD_H, g.field_fg, LEFT);
		} else {
			text_in(cr, g.font, "User name", x + 10, y, w - 20, FIELD_H,
				(g.field_fg & 0xffffff00) | 0x80, LEFT);
		}
		add_hit(o, x, y, w, FIELD_H, AREA_NAME, 0);
		y += FIELD_H + 10;
	}

	// the password, or what PAM asks further
	bool reveal_button = dm_config_bool(g.config, "password_reveal") &&
		(g.stage != STAGE_PROMPT || g.prompt_secret);
	double field_w = w;
	field_frame(cr, x, y, field_w, FIELD_H, g.focus == AREA_PASSWORD);
	if (g.password_len == 0) {
		const char *hint = g.stage == STAGE_PROMPT ? g.prompt : "Password";
		text_in(cr, g.font, hint, x + 10, y, w - 44, FIELD_H, (g.field_fg & 0xffffff00) | 0x80, LEFT);
	} else {
		draw_secret(cr, x + 10, y, w - 48, FIELD_H);
	}
	add_hit(o, x, y, w, FIELD_H, AREA_PASSWORD, 0);
	if (reveal_button) {
		draw_eye(cr, x + w - 20, y + FIELD_H / 2.0, (g.field_fg & 0xffffff00) | 0xa0, g.reveal);
		add_hit(o, x + w - 38, y, 38, FIELD_H, AREA_REVEAL, 0);
	}
	y += FIELD_H + 10;

	// which session
	if (sessions) {
		const char *name = g.sessions->items[g.session_sel].name;
		text_in(cr, g.bold, "\xe2\x80\xb9", x, y, 24, 26, g.fg, MIDDLE);
		text_in(cr, g.font, name, x + 24, y, w - 48, 26, g.fg, MIDDLE);
		text_in(cr, g.bold, "\xe2\x80\xba", x + w - 24, y, 24, 26, g.fg, MIDDLE);
		if (g.focus == AREA_SESSION_NEXT) {
			box_path(cr, x + 0.5, y + 0.5, w - 1, 25, 3);
			color(cr, g.accent);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		add_hit(o, x, y, w / 2, 26, AREA_SESSION_PREV, 0);
		add_hit(o, x + w / 2, y, w / 2, 26, AREA_SESSION_NEXT, 0);
		y += session_h;
	}
	if (layouts) {
		const char *name = xkb_keymap_layout_get_name(g.keymap, g.group);
		char text[160];
		snprintf(text, sizeof(text), "Keyboard: %s", name ? name : "?");
		text_in(cr, g.font, text, x, y, w, 24, g.focus == AREA_LAYOUT ? g.accent : g.fg, MIDDLE);
		add_hit(o, x, y, w, 24, AREA_LAYOUT, 0);
		y += layout_h;
	}

	// the button, like the default button of the theme
	box_path(cr, x, y, w, 36, g.radius > 4 ? 4 : g.radius);
	bool busy = g.stage == STAGE_CHECKING || g.stage == STAGE_DONE;
	color(cr, busy ? (g.accent & 0xffffff00) | 0x80 : g.accent);
	cairo_fill(cr);
	if (g.classic) {
		bevel(cr, x, y, w, 36, false);
	}
	uint32_t on_accent = 0xffffffff;
	const char *label = g.stage == STAGE_CHECKING ? "Checking..." :
		g.stage == STAGE_DONE ? "Starting..." : g.stage == STAGE_PROMPT ? "Go on" : "Log in";
	text_in(cr, g.bold, label, x, y, w, 36, on_accent, MIDDLE);
	if (g.focus == AREA_LOGIN) {
		box_path(cr, x + 2.5, y + 2.5, w - 5, 31, 2);
		color(cr, on_accent);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	add_hit(o, x, y, w, 36, AREA_LOGIN, 0);
	y += 38;

	// what happened
	const char *line = g.message;
	if (!*line && dm_config_bool(g.config, "show_caps_lock") && g.state &&
			xkb_state_mod_name_is_active(g.state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0) {
		line = "Caps Lock is on";
	}
	if (*line) {
		text_in(cr, g.font, line, x, y, w, 28, g.message_error ? 0xc42b1cff : g.fg, MIDDLE);
	}
}

static void draw_power(struct output *o, cairo_t *cr, double W, double H) {
	if (!dm_config_bool(g.config, "show_power") || g.stage == STAGE_DONE) {
		return;
	}
	static const struct {
		const char *allow, *label;
	} buttons[] = {
		{ "allow_hibernate", "Hibernate" },
		{ "allow_suspend", "Sleep" },
		{ "allow_reboot", "Restart" },
		{ "allow_poweroff", "Shut down" },
	};
	double x = W - 16;
	for (int i = 3; i >= 0; i--) {
		if (!dm_config_bool(g.config, buttons[i].allow)) {
			continue;
		}
		PangoLayout *layout = make_layout(cr, g.font, buttons[i].label);
		int tw, th;
		pango_layout_get_pixel_size(layout, &tw, &th);
		g_object_unref(layout);
		double bw = tw + 24, by = H - 44;
		x -= bw;
		box_path(cr, x, by, bw, 30, g.radius > 4 ? 4 : g.radius);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
		cairo_fill(cr);
		text_in(cr, g.font, buttons[i].label, x, by, bw, 30, 0xffffffff, MIDDLE);
		add_hit(o, x, by, bw, 30, AREA_POWER, i);
		x -= 8;
	}
}

static void draw_clock(cairo_t *cr, double W, double H) {
	const char *place = dm_config_str(g.config, "clock");
	if (strcmp(place, "theme") == 0) {
		// the real ones only had a clock before asking; Windows 11 has it on top
		place = g.look == LOOK_CARD || g.look == LOOK_WIN11 || g.look == LOOK_SWAY ? "top" : "none";
	}
	if (strcmp(place, "none") == 0) {
		return;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char clock_text[64] = "", date_text[128] = "";
	strftime(clock_text, sizeof(clock_text), dm_config_str(g.config, "clock_format"), &tm);
	strftime(date_text, sizeof(date_text), dm_config_str(g.config, "date_format"), &tm);
	bool top = strcmp(place, "top") == 0;
	int align = top ? MIDDLE : strcmp(place, "bottom-right") == 0 ? RIGHT : LEFT;
	double size = H / 12 < 28 ? 28 : H / 12;
	char font[96];
	snprintf(font, sizeof(font), "Segoe UI Light, Noto Sans Light, Sans %d", (int)size);
	double y = top ? H * 0.06 : H - size * 3.4;
	double x = align == LEFT ? W * 0.03 : 0;
	double w = align == MIDDLE ? W : W * 0.97 - x;
	if (strcmp(place, "bottom-right") == 0) {
		y -= 44; // above the power buttons
	}
	text_on_wallpaper(cr, font, clock_text, x, y, w, size * 1.7, align);
	snprintf(font, sizeof(font), "Segoe UI, Noto Sans, Sans %d", (int)(size / 3.2));
	text_on_wallpaper(cr, font, date_text, x, y + size * 1.7, w, size * 0.7, align);
}

#include "looks.inc"

/* What is on a screen, in pixels of a scale of 1. */
static void draw_scene(struct output *o, cairo_t *cr, double W, double H) {
	o->hit_count = 0;
	// these lay white words on any picture, and the real ones dim it for that
	if (g.look == LOOK_WIN10 || g.look == LOOK_WIN11 || g.look == LOOK_WIN8) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
		cairo_paint(cr);
	}
	draw_clock(cr, W, H);
	if (o->first || dm_config_bool(g.config, "card_on_all_screens")) {
		draw_login(o, cr, W, H);
	}
}

/* One frame into a picture, without a screen: what the settings show. */
static int render_to_file(const char *path, int w, int h) {
	struct output o = { .first = true };
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(surface);
	cairo_surface_t *background = render_background(w, h, true);
	cairo_set_source_surface(cr, background, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(background);
	double virtual_w = 1280, scale = w / virtual_w;
	cairo_scale(cr, scale, scale);
	snprintf(g.password, sizeof(g.password), "secret");
	g.password_len = 6;
	g.focus = AREA_PASSWORD;
	draw_scene(&o, cr, virtual_w, h / scale);
	cairo_destroy(cr);
	cairo_status_t status = cairo_surface_write_to_png(surface, path);
	cairo_surface_destroy(surface);
	clear_secret();
	return status == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

static void render_output(struct output *o) {
	if (!o->configured || o->width <= 0 || o->height <= 0) {
		return;
	}
	int pw = o->width * o->scale, ph = o->height * o->scale;
	struct pool_buffer *buffer = get_next_buffer(g.shm, o->buffers, pw, ph);
	if (!buffer) {
		g.dirty = true;
		return;
	}
	if (!o->background || o->bg_w != pw || o->bg_h != ph) {
		if (o->background) {
			cairo_surface_destroy(o->background);
		}
		o->background = render_background(pw, ph, false);
		o->bg_w = pw;
		o->bg_h = ph;
		o->bg_full = false;
	}
	cairo_t *cr = buffer->cairo;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, o->background, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, o->scale, o->scale);
	draw_scene(o, cr, o->width, o->height);
	cairo_restore(cr);
	wl_surface_set_buffer_scale(o->surface, o->scale);
	wl_surface_attach(o->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(o->surface);
}

static void render_all(void) {
	g.dirty = false;
	struct output *o;
	wl_list_for_each(o, &g.outputs, link) {
		render_output(o);
	}
}

/* ---------- talking to the daemon ---------- */

static void send_text(const char *text) {
	if (g.fd >= 0 && send(g.fd, text, strlen(text), MSG_NOSIGNAL) < 0) {
		set_message("The login service does not answer", true);
	}
}

/* The answer to the first prompt goes out as soon as it is asked for. */
static void send_answer(void) {
	char buffer[sizeof(g.password) + 8];
	int n = snprintf(buffer, sizeof(buffer), "answer %s", g.password);
	if (n > 0) {
		send_text(buffer);
	}
	explicit_bzero(buffer, sizeof(buffer));
	clear_secret();
}

static const char *chosen_user(void) {
	return typing_name() ? g.name : g.users[g.user_sel].name;
}

static void submit(void) {
	if (g.stage == STAGE_PROMPT) {
		g.stage = STAGE_CHECKING;
		g.prompt[0] = '\0';
		send_answer();
		g.dirty = true;
		return;
	}
	if (g.stage != STAGE_IDLE) {
		return;
	}
	if (g.preview) {
		clear_secret();
		set_message("A preview: nothing is checked. Esc ends it.", false);
		return;
	}
	if (!*chosen_user()) {
		g.focus = AREA_NAME;
		set_message("Type a user name", true);
		return;
	}
	if (g.sessions->count == 0) {
		set_message("No session is installed", true);
		return;
	}
	char buffer[600];
	snprintf(buffer, sizeof(buffer), "login %s\n%s", chosen_user(),
		g.sessions->items[g.session_sel].id);
	g.stage = STAGE_CHECKING;
	g.answer_pending = true;
	g.message[0] = '\0';
	send_text(buffer);
	g.dirty = true;
}

static void handle_daemon(void) {
	char buffer[4097];
	ssize_t n = recv(g.fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
	if (n == 0) {
		if (g.stage == STAGE_DONE) {
			g.running = false; // on our way to the session
			return;
		}
		g.fd = -1;
		g.stage = STAGE_IDLE;
		set_message("The login service went away", true);
		return;
	}
	if (n < 0) {
		return;
	}
	buffer[n] = '\0';
	const char *rest = strchr(buffer, ' ');
	rest = rest ? rest + 1 : "";
	if (strncmp(buffer, "secret", 6) == 0 || strncmp(buffer, "visible", 7) == 0) {
		bool secret = buffer[0] == 's';
		if (g.answer_pending && secret) {
			g.answer_pending = false;
			send_answer();
			return;
		}
		g.answer_pending = false;
		g.stage = STAGE_PROMPT;
		g.prompt_secret = secret;
		snprintf(g.prompt, sizeof(g.prompt), "%s", rest);
		clear_secret();
		g.focus = AREA_PASSWORD;
	} else if (strncmp(buffer, "info", 4) == 0) {
		set_message(rest, false);
	} else if (strncmp(buffer, "error", 5) == 0) {
		set_message(rest, true);
	} else if (strncmp(buffer, "failed", 6) == 0) {
		g.stage = STAGE_IDLE;
		g.answer_pending = false;
		clear_secret();
		g.focus = AREA_PASSWORD;
		set_message(rest, true);
	} else if (strcmp(buffer, "ok") == 0) {
		g.stage = STAGE_DONE;
		set_message("", false);
	} else if (strncmp(buffer, "refused", 7) == 0) {
		set_message(rest, true);
	}
	g.dirty = true;
}

static bool connect_daemon(const char *path) {
	g.fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (g.fd < 0 || connect(g.fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("tilewin-dm-greeter: cannot reach the daemon");
		return false;
	}
	return true;
}

/* ---------- input ---------- */

/* Asks the compositor under this program. */
static void compositor_command(const char *a, const char *b, const char *c, const char *d) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp("tilewinmsg", "tilewinmsg", a, b, c, d, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

static void wake_up(void) {
	touch_input();
	if (g.screen_off) {
		g.screen_off = false;
		compositor_command("output", "*", "power", "on");
	}
}

static void next_user(int by) {
	if (g.user_count == 0 || g.stage != STAGE_IDLE) {
		return;
	}
	int total = g.user_count + 1;
	g.user_sel = (g.user_sel + by + total) % total;
	clear_secret();
	g.focus = typing_name() ? AREA_NAME : AREA_PASSWORD;
	g.message[0] = '\0';
	g.dirty = true;
}

static void next_session(int by) {
	if (g.sessions->count > 0) {
		g.session_sel = (g.session_sel + by + (int)g.sessions->count) % (int)g.sessions->count;
		g.dirty = true;
	}
}

static void next_focus(int by) {
	enum area order[6];
	int count = 0;
	if (typing_name() && g.stage == STAGE_IDLE) {
		order[count++] = AREA_NAME;
	}
	order[count++] = AREA_PASSWORD;
	if (dm_config_bool(g.config, "show_sessions") && g.sessions->count > 1) {
		order[count++] = AREA_SESSION_NEXT;
	}
	if (has_layouts()) {
		order[count++] = AREA_LAYOUT;
	}
	order[count++] = AREA_LOGIN;
	int at = 0;
	for (int i = 0; i < count; i++) {
		if (order[i] == g.focus) {
			at = i;
		}
	}
	g.focus = order[(at + by + count) % count];
	g.dirty = true;
}

static void switch_layout(void) {
	compositor_command("input", "type:keyboard", "xkb_switch_layout", "next");
}

static void power_button(int index) {
	static const char *const words[] = { "hibernate", "suspend", "reboot", "poweroff" };
	if (g.preview) {
		set_message("A preview: the computer stays on.", false);
		return;
	}
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "power %s", words[index]);
	send_text(buffer);
}

static void click(struct output *o, double x, double y) {
	for (int i = o->hit_count - 1; i >= 0; i--) {
		const struct hit *h = &o->hits[i];
		if (x < h->x || x >= h->x + h->w || y < h->y || y >= h->y + h->h) {
			continue;
		}
		switch (h->area) {
		case AREA_USER:
			if (g.stage == STAGE_IDLE && h->index != g.user_sel) {
				g.user_sel = h->index;
				clear_secret();
				g.message[0] = '\0';
			}
			g.focus = typing_name() ? AREA_NAME : AREA_PASSWORD;
			break;
		case AREA_NAME:
		case AREA_PASSWORD:
			g.focus = h->area;
			break;
		case AREA_REVEAL:
			g.reveal = !g.reveal;
			g.focus = AREA_PASSWORD;
			break;
		case AREA_SESSION_PREV:
			next_session(-1);
			break;
		case AREA_SESSION_NEXT:
			next_session(1);
			break;
		case AREA_LAYOUT:
			switch_layout();
			break;
		case AREA_LOGIN:
			submit();
			break;
		case AREA_POWER:
			power_button(h->index);
			break;
		case AREA_CANCEL:
			if (g.stage == STAGE_PROMPT) {
				send_text("cancel");
				g.stage = STAGE_CHECKING;
			}
			clear_secret();
			g.message[0] = '\0';
			break;
		case AREA_NONE:
			break;
		}
		g.dirty = true;
		return;
	}
}

static void type_text(const char *utf8) {
	if (g.focus != AREA_NAME && g.focus != AREA_PASSWORD) {
		g.focus = AREA_PASSWORD;
	}
	bool name = g.focus == AREA_NAME && typing_name() && g.stage == STAGE_IDLE;
	char *buffer = name ? g.name : g.password;
	size_t *len = name ? &g.name_len : &g.password_len;
	size_t size = name ? sizeof(g.name) : sizeof(g.password);
	size_t add = strlen(utf8);
	if (add > 0 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && *len + add < size - 1) {
		memcpy(buffer + *len, utf8, add);
		*len += add;
		buffer[*len] = '\0';
		g.message[0] = '\0';
		g.dirty = true;
	}
}

static void erase_char(void) {
	bool name = g.focus == AREA_NAME && typing_name();
	char *buffer = name ? g.name : g.password;
	size_t *len = name ? &g.name_len : &g.password_len;
	while (*len > 0) {
		unsigned char c = buffer[--*len];
		buffer[*len] = '\0';
		if ((c & 0xc0) != 0x80) {
			break;
		}
	}
	g.dirty = true;
}

static void handle_key(xkb_keysym_t sym, const char *utf8) {
	wake_up();
	switch (sym) {
	case XKB_KEY_Escape:
		if (g.preview) {
			g.running = false;
		} else if (g.stage == STAGE_PROMPT) {
			send_text("cancel");
			g.stage = STAGE_CHECKING;
		} else {
			clear_secret();
		}
		g.dirty = true;
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (g.focus == AREA_NAME && g.stage == STAGE_IDLE && g.password_len == 0) {
			g.focus = AREA_PASSWORD; // on to the password first
			g.dirty = true;
		} else if (g.focus == AREA_LAYOUT) {
			switch_layout();
		} else if (g.focus == AREA_SESSION_NEXT) {
			next_session(1);
		} else {
			submit();
		}
		return;
	case XKB_KEY_Tab:
		next_focus(1);
		return;
	case XKB_KEY_ISO_Left_Tab:
		next_focus(-1);
		return;
	case XKB_KEY_Up:
		next_user(-1);
		return;
	case XKB_KEY_Down:
		next_user(1);
		return;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		if (g.focus == AREA_SESSION_NEXT) {
			next_session(sym == XKB_KEY_Left ? -1 : 1);
		} else if (g.focus == AREA_LAYOUT) {
			switch_layout();
		}
		return;
	case XKB_KEY_BackSpace:
		erase_char();
		return;
	}
	if (g.stage == STAGE_CHECKING || g.stage == STAGE_DONE) {
		return;
	}
	if (xkb_state_mod_name_is_active(g.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) {
		if (sym == XKB_KEY_u || sym == XKB_KEY_U) {
			clear_secret();
			g.dirty = true;
		}
		return;
	}
	type_text(utf8);
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
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(g.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		return;
	}
	if (g.state) {
		xkb_state_unref(g.state);
	}
	if (g.keymap) {
		xkb_keymap_unref(g.keymap);
	}
	g.keymap = keymap;
	g.state = xkb_state_new(keymap);
	g.dirty = true;
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface) {
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !g.state) {
		return;
	}
	xkb_keycode_t code = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(g.state, code);
	char utf8[16] = { 0 };
	xkb_state_key_get_utf8(g.state, code, utf8, sizeof(utf8));
	handle_key(sym, utf8);
	explicit_bzero(utf8, sizeof(utf8));
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	if (g.state) {
		xkb_state_update_mask(g.state, depressed, latched, locked, 0, 0, group);
		g.group = group;
		g.dirty = true; // Caps Lock, the layout
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static struct output *output_of_surface(struct wl_surface *surface) {
	struct output *o;
	wl_list_for_each(o, &g.outputs, link) {
		if (o->surface == surface) {
			return o;
		}
	}
	return NULL;
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	g.pointer_output = output_of_surface(surface);
	g.px = wl_fixed_to_double(x);
	g.py = wl_fixed_to_double(y);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surface) {
	g.pointer_output = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {
	g.px = wl_fixed_to_double(x);
	g.py = wl_fixed_to_double(y);
	wake_up();
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT && g.pointer_output) {
		wake_up();
		click(g.pointer_output, g.px, g.py);
	}
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && wl_fixed_to_double(value) != 0) {
		next_user(wl_fixed_to_double(value) > 0 ? 1 : -1);
	}
}

static void pointer_frame(void *data, struct wl_pointer *p) {
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
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

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
		struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct output *o = output_of_surface(surface);
	wake_up();
	if (o) {
		click(o, wl_fixed_to_double(x), wl_fixed_to_double(y));
	}
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x,
		wl_fixed_t y) {
}

static void touch_frame(void *data, struct wl_touch *t) {
}

static void touch_cancel(void *data, struct wl_touch *t) {
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	bool keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	bool pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	bool touch = caps & WL_SEAT_CAPABILITY_TOUCH;
	if (keyboard && !g.keyboard) {
		g.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(g.keyboard, &keyboard_listener, NULL);
	} else if (!keyboard && g.keyboard) {
		wl_keyboard_release(g.keyboard);
		g.keyboard = NULL;
	}
	if (pointer && !g.pointer) {
		g.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(g.pointer, &pointer_listener, NULL);
	} else if (!pointer && g.pointer) {
		wl_pointer_release(g.pointer);
		g.pointer = NULL;
		g.pointer_output = NULL;
	}
	if (touch && !g.touch) {
		g.touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(g.touch, &touch_listener, NULL);
	} else if (!touch && g.touch) {
		wl_touch_release(g.touch);
		g.touch = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------- screens ---------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct output *o = data;
	o->width = width;
	o->height = height;
	o->configured = true;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	render_output(o);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	struct output *o = data;
	o->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void make_surface(struct output *o) {
	if (o->surface || !g.layer_shell || !g.compositor) {
		return;
	}
	o->surface = wl_compositor_create_surface(g.compositor);
	o->layer = zwlr_layer_shell_v1_get_layer_surface(g.layer_shell, o->surface, o->wl_output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-dm-greeter");
	zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
	zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
	// on one screen only, or the keyboard goes to whichever surface was made last
	zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, o->first ?
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE :
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(o->surface);
}

static void output_geometry(void *data, struct wl_output *out, int32_t x, int32_t y, int32_t pw,
		int32_t ph, int32_t subpixel, const char *make, const char *model, int32_t transform) {
}

static void output_mode(void *data, struct wl_output *out, uint32_t flags, int32_t w, int32_t h,
		int32_t refresh) {
}

static void output_done(void *data, struct wl_output *out) {
	g.dirty = true;
}

static void output_scale(void *data, struct wl_output *out, int32_t factor) {
	struct output *o = data;
	o->scale = factor > 0 ? factor : 1;
	g.dirty = true;
}

static void output_name(void *data, struct wl_output *out, const char *name) {
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
		g.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		g.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !g.seat) {
		g.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(g.seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		g.layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
			version < 4 ? version : 4);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		o->global_name = name;
		o->scale = 1;
		o->first = wl_list_empty(&g.outputs);
		o->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &output_listener, o);
		wl_list_insert(g.outputs.prev, &o->link);
		make_surface(o); // a screen plugged in while the login screen is up
	}
}

static void destroy_output(struct output *o) {
	bool first = o->first;
	wl_list_remove(&o->link);
	if (g.pointer_output == o) {
		g.pointer_output = NULL;
	}
	if (o->layer) {
		zwlr_layer_surface_v1_destroy(o->layer);
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
	}
	destroy_buffer(&o->buffers[0]);
	destroy_buffer(&o->buffers[1]);
	if (o->background) {
		cairo_surface_destroy(o->background);
	}
	wl_output_destroy(o->wl_output);
	free(o);
	// with the first gone another has to take the keyboard, or nobody can type
	if (first && !wl_list_empty(&g.outputs)) {
		struct output *next = wl_container_of(g.outputs.next, next, link);
		next->first = true;
		zwlr_layer_surface_v1_set_keyboard_interactivity(next->layer,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		wl_surface_commit(next->surface);
		g.dirty = true;
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &g.outputs, link) {
		if (o->global_name == name) {
			destroy_output(o);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------- main ---------- */

/* Pictures come after the first frame, one screen at a time. */
static bool load_next_wallpaper(void) {
	struct output *o;
	wl_list_for_each(o, &g.outputs, link) {
		if (o->configured && o->background && !o->bg_full) {
			cairo_surface_t *full = render_background(o->bg_w, o->bg_h, true);
			cairo_surface_destroy(o->background);
			o->background = full;
			o->bg_full = true;
			g.dirty = true;
			return true;
		}
	}
	return false;
}

static void tick(void) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char text[64] = "";
	strftime(text, sizeof(text), dm_config_str(g.config, "clock_format"), &tm);
	if (strcmp(text, g.clock_text) != 0) {
		snprintf(g.clock_text, sizeof(g.clock_text), "%s", text);
		g.dirty = true;
	}
	long idle = idle_seconds();
	int forget = dm_config_int(g.config, "clear_password_after");
	if (forget > 0 && idle >= forget && g.password_len > 0 && g.stage == STAGE_IDLE) {
		clear_secret();
		g.dirty = true;
	}
	int off = dm_config_int(g.config, "screen_off_after");
	if (off > 0 && idle >= off && !g.screen_off && !g.preview) {
		g.screen_off = true;
		compositor_command("output", "*", "power", "off");
	}
}

int main(int argc, char **argv) {
	static const struct option options[] = {
		{ "socket", required_argument, NULL, 's' },
		{ "config", required_argument, NULL, 'c' },
		{ "preview", no_argument, NULL, 'p' },
		{ "render", required_argument, NULL, 'r' },
		{ "size", required_argument, NULL, 'S' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	const char *socket_path = NULL, *config_path = DM_CONFIG_PATH, *render_path = NULL;
	int render_w = 1280, render_h = 720;
	int c;
	while ((c = getopt_long(argc, argv, "h", options, NULL)) != -1) {
		switch (c) {
		case 's':
			socket_path = optarg;
			break;
		case 'c':
			config_path = optarg;
			break;
		case 'p':
			g.preview = true;
			break;
		case 'r':
			render_path = optarg;
			g.preview = true;
			break;
		case 'S':
			sscanf(optarg, "%dx%d", &render_w, &render_h);
			break;
		case 'h':
		default:
			printf("Usage: tilewin-dm-greeter --socket <file> [--config <file>]\n"
				"       tilewin-dm-greeter --preview [--config <file>]\n"
				"       tilewin-dm-greeter --render <png> [--size <w>x<h>] [--config <file>]\n");
			return c == 'h' ? 0 : 2;
		}
	}
	if (!g.preview && !socket_path) {
		fprintf(stderr, "tilewin-dm-greeter: the display manager starts this; "
			"--preview shows how it looks\n");
		return 2;
	}
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);
	mlock(g.password, sizeof(g.password));
	g.config = dm_config_load(config_path);
	char host[256] = "";
	gethostname(host, sizeof(host) - 1);
	g.hostname = strdup(host);
	read_theme();
	read_sessions();
	read_users();
	if (render_path) {
		return render_to_file(render_path, render_w, render_h);
	}
	if (!g.preview && !connect_daemon(socket_path)) {
		return 1;
	}
	wl_list_init(&g.outputs);
	g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	g.display = wl_display_connect(NULL);
	if (!g.display) {
		fprintf(stderr, "tilewin-dm-greeter: cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(g.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(g.display);
	if (!g.compositor || !g.shm || !g.layer_shell) {
		fprintf(stderr, "tilewin-dm-greeter: the compositor lacks what this needs\n");
		return 1;
	}
	struct output *o;
	wl_list_for_each(o, &g.outputs, link) {
		make_surface(o);
	}
	wl_display_roundtrip(g.display);

	g.running = true;
	touch_input();
	struct timespec preview_started = g.last_input;
	while (g.running) {
		tick();
		if (g.dirty) {
			render_all();
		}
		bool more = load_next_wallpaper();
		while (wl_display_prepare_read(g.display) != 0) {
			wl_display_dispatch_pending(g.display);
		}
		if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(g.display);
			break;
		}
		struct pollfd fds[2] = {
			{ wl_display_get_fd(g.display), POLLIN, 0 },
			{ g.fd, POLLIN, 0 },
		};
		int ready = poll(fds, 2, more || g.dirty ? 0 : 1000);
		if (ready > 0 && (fds[0].revents & POLLIN)) {
			if (wl_display_read_events(g.display) < 0) {
				break;
			}
		} else {
			wl_display_cancel_read(g.display);
		}
		if (ready > 0 && (fds[0].revents & (POLLERR | POLLHUP))) {
			break;
		}
		if (wl_display_dispatch_pending(g.display) < 0) {
			break;
		}
		if (ready > 0 && g.fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
			handle_daemon();
		}
		// a preview covers everything: it never stays up unattended
		if (g.preview) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec - g.last_input.tv_sec > 60 && now.tv_sec - preview_started.tv_sec > 60) {
				g.running = false;
			}
		}
	}
	clear_secret();
	return 0;
}
