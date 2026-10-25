/*
 * The settings window in the look of the theme: a style sheet made from the
 * theme that is on (its style says which Windows it looks like, its menus give
 * the colours, and up to XP the font) on top of GTK's own. It is made again when the
 * theme or the colour scheme changes, so the window follows at once.
 *
 *   win1, win3, win95   the grey 3D face with bevelled buttons and sunken fields
 *   winxp               Luna: the beige dialog face, round buttons that glow
 *                       orange, the blue task pane on the left
 *   win7                Aero: white, glassy grey buttons that light up blue
 *   win8, win10         flat: grey buttons, thick field borders, the accent
 *   win11               Fluent: cards on a grey ground, round corners
 */
#include <math.h>
#include <pango/pango.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "settings.h"
#include "tw_theme.h"

enum era { ERA_CLASSIC, ERA_XP, ERA_WIN7, ERA_FLAT, ERA_WIN11 };

static enum era era_of(const struct tw_theme *t) {
	const char *s = t && t->style ? t->style : "win10";
	static const struct {
		const char *name;
		enum era era;
	} eras[] = {
		{ "win1", ERA_CLASSIC }, { "win3", ERA_CLASSIC }, { "win31", ERA_CLASSIC },
		{ "win95", ERA_CLASSIC }, { "classic", ERA_CLASSIC }, { "winxp", ERA_XP },
		{ "luna", ERA_XP }, { "win7", ERA_WIN7 }, { "aero", ERA_WIN7 },
		{ "win8", ERA_FLAT }, { "metro", ERA_FLAT }, { "win10", ERA_FLAT },
		{ "win11", ERA_WIN11 }, { "fluent", ERA_WIN11 },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(eras); i++) {
		if (g_ascii_strcasecmp(s, eras[i].name) == 0) {
			return eras[i].era;
		}
	}
	return ERA_FLAT;
}

static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		double ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

static bool is_dark(uint32_t c) {
	return ((c >> 24 & 0xff) * 299 + (c >> 16 & 0xff) * 587 + (c >> 8 & 0xff) * 114) / 1000 < 128;
}

/* A colour for the style sheet; up to 128 of them can be used in one printf. */
static const char *css_color(uint32_t c) {
	static char ring[128][32];
	static int next;
	char *out = ring[next++ % 128];
	if ((c & 0xff) == 0xff) {
		snprintf(out, 32, "#%06x", c >> 8);
	} else {
		snprintf(out, 32, "rgba(%u,%u,%u,%.3f)", c >> 24 & 0xff, c >> 16 & 0xff, c >> 8 & 0xff,
			(c & 0xff) / 255.0);
	}
	return out;
}
#define C(c) css_color(c)

/* The theme's font as CSS: "font-family: ...; font-size: ...". */
static void add_font(GString *css, const struct tw_theme *t) {
	const char *spec = tw_theme_str(t, "menu.font", tw_theme_str(t, "panel.font", NULL));
	if (!spec) {
		return;
	}
	PangoFontDescription *desc = pango_font_description_from_string(spec);
	const char *family = pango_font_description_get_family(desc);
	if (family && *family) {
		g_string_append(css, "window, popover, tooltip { font-family: ");
		char **names = g_strsplit(family, ",", -1);
		for (int i = 0; names[i]; i++) {
			g_strstrip(names[i]);
			g_string_append_printf(css, "%s\"%s\"", i ? ", " : "", names[i]);
		}
		g_strfreev(names);
		// the menus of the old ones are 8 points, which the rows here are too
		// tight for: at least 9
		double size = pango_font_description_get_size(desc) / (double)PANGO_SCALE;
		if (size > 0) {
			g_string_append_printf(css, "; font-size: %.1fpt", size < 9 ? 9 : size);
		}
		g_string_append(css, "; }\n");
	}
	pango_font_description_free(desc);
}

/* What every era has: the colours of the window, the text, the selection. */
struct palette {
	uint32_t bg, fg, dim, base, base_fg, border, accent, accent_fg, hl, hl_fg, side;
	bool dark;
};

static void add_common(GString *css, const struct palette *p) {
	g_string_append_printf(css,
		"window.background, window { background-color: %s; color: %s; }\n"
		"@define-color theme_selected_bg_color %s;\n"
		"@define-color theme_selected_fg_color %s;\n"
		"@define-color accent_bg_color %s;\n"
		"@define-color accent_color %s;\n"
		"@define-color accent_fg_color %s;\n"
		".dim-label { color: %s; opacity: 1; }\n"
		".tw-side { background-color: %s; }\n"
		".tw-side .navigation-sidebar, .navigation-sidebar { background: transparent; color: %s; }\n"
		"list.tw-group { background-color: %s; color: %s; }\n"
		"list.tw-group > row { border-color: %s; }\n"
		"popover > contents, popover > arrow { background-color: %s; color: %s; }\n"
		"popover row:selected, popover row:hover, list.tw-group row:selected "
		"{ background-color: %s; color: %s; }\n"
		"separator { background-color: %s; }\n",
		C(p->bg), C(p->fg), C(p->accent), C(p->accent_fg), C(p->accent), C(p->accent),
		C(p->accent_fg), C(p->dim), C(p->side), C(p->fg), C(p->base), C(p->base_fg),
		C(p->border), C(p->base), C(p->base_fg), C(p->hl), C(p->hl_fg), C(p->border));
}

/* ---------- Windows 1 to 2000: bevels ---------- */

static void add_classic(GString *css, const struct tw_theme *t) {
	uint32_t face = tw_theme_color(t, "decoration.face", 0xc0c0c0ff) | 0xff;
	uint32_t hi = tw_theme_color(t, "decoration.highlight", 0xffffffff) | 0xff;
	uint32_t light = tw_theme_color(t, "decoration.light", mix(face, 0xffffffff, 0.5)) | 0xff;
	uint32_t shadow = tw_theme_color(t, "decoration.shadow", mix(face, 0x000000ff, 0.33)) | 0xff;
	uint32_t dark = tw_theme_color(t, "decoration.dark", 0x000000ff) | 0xff;
	uint32_t fg = tw_theme_color(t, "menu.fg", is_dark(face) ? 0xe0e0e0ff : 0x000000ff) | 0xff;
	uint32_t hl = tw_theme_color(t, "menu.hl_bg", 0x000080ff) | 0xff;
	uint32_t hl_fg = tw_theme_color(t, "menu.hl_fg", 0xffffffff) | 0xff;
	uint32_t field = tw_theme_color(t, "menu.field_bg", is_dark(face) ? 0x1f1f1fff : 0xffffffff);
	uint32_t field_fg = tw_theme_color(t, "menu.field_fg", fg);
	uint32_t dim = tw_theme_color(t, "menu.disabled_fg", mix(fg, face, 0.45));
	struct palette p = { .bg = face, .fg = fg, .dim = mix(fg, face, 0.3), .base = face,
		.base_fg = fg, .border = shadow, .accent = hl, .accent_fg = hl_fg, .hl = hl,
		.hl_fg = hl_fg, .side = face };
	add_common(css, &p);
	// raised: light top left, dark bottom right; sunken the other way round
	char raised[256], sunken[256], pressed[256];
	snprintf(raised, sizeof(raised), "border: 1px solid; border-color: %s %s %s %s; "
		"box-shadow: inset -1px -1px %s, inset 1px 1px %s;", C(hi), C(dark), C(dark), C(hi),
		C(shadow), C(light));
	snprintf(sunken, sizeof(sunken), "border: 1px solid; border-color: %s %s %s %s; "
		"box-shadow: inset 1px 1px %s, inset -1px -1px %s;", C(shadow), C(hi), C(hi), C(shadow),
		C(dark), C(light));
	snprintf(pressed, sizeof(pressed), "border: 1px solid; border-color: %s %s %s %s; "
		"box-shadow: inset 1px 1px %s;", C(dark), C(hi), C(hi), C(dark), C(shadow));
	g_string_append_printf(css,
		"window * { border-radius: 0; text-shadow: none; -gtk-icon-shadow: none; }\n"
		"list.tw-group { border: 1px solid %s; box-shadow: inset 1px 1px %s, 1px 1px %s; }\n"
		".tw-side .navigation-sidebar { background-color: %s; %s margin: 4px 8px 8px 8px; }\n"
		".navigation-sidebar > row { margin: 0; padding: 3px 6px; border-radius: 0; }\n"
		".navigation-sidebar > row:selected, .navigation-sidebar > row:selected:hover, "
		"flowboxchild:selected, flowboxchild.tw-card:selected "
		"{ background-color: %s; color: %s; }\n"
		".navigation-sidebar > row:hover { background-color: transparent; }\n"
		"button { background: %s; color: %s; %s padding: 3px 10px; min-height: 20px; }\n"
		"button:hover { background: %s; }\n"
		"button:active, button:checked { background: %s; %s }\n"
		"button.suggested-action { background: %s; color: %s; %s outline: 1px solid %s; "
		"outline-offset: 0; }\n"
		"button.flat { background: transparent; border-color: transparent; box-shadow: none; }\n"
		"button.flat:hover { background: transparent; border: 1px solid; border-color: %s %s %s %s; }\n"
		"button:disabled, button:disabled label { color: %s; text-shadow: 1px 1px %s; }\n"
		"entry, spinbutton, searchbar entry, dropdown > button.toggle "
		"{ background: %s; color: %s; %s }\n"
		"spinbutton > button { background: %s; %s min-height: 14px; padding: 0 4px; }\n"
		"spinbutton > text, entry > text { background: transparent; color: %s; }\n"
		"dropdown > button.toggle arrow { background: %s; color: %s; %s min-width: 14px; "
		"min-height: 14px; margin: -2px -8px -2px 4px; }\n"
		"switch { background: %s; %s min-width: 40px; min-height: 18px; }\n"
		"switch:checked { background: %s; }\n"
		"switch > slider { background: %s; %s min-width: 16px; margin: 0; }\n"
		"switch image { color: transparent; }\n"
		"check, radio { background: %s; color: %s; %s min-width: 13px; min-height: 13px; "
		"-gtk-icon-size: 11px; }\n"
		"radio { border-radius: 9999px; }\n"
		"scale > trough { background: %s; %s min-height: 4px; min-width: 4px; }\n"
		"scale > trough > highlight { background: transparent; border: none; box-shadow: none; }\n"
		"scale > trough > slider { background: %s; %s min-width: 11px; min-height: 21px; "
		"margin: -10px -6px; }\n"
		"progressbar > trough { background: %s; %s min-height: 16px; }\n"
		"progressbar > trough > progress { background: %s; border-radius: 0; }\n"
		"scrollbar { background-color: %s; }\n"
		"scrollbar > range > trough > slider { background: %s; %s min-width: 14px; "
		"min-height: 14px; }\n"
		"popover > contents { %s padding: 2px; box-shadow: inset -1px -1px %s, "
		"inset 1px 1px %s; }\n"
		"popover > arrow { background: transparent; border: none; }\n"
		"popover row { border-radius: 0; }\n"
		"tooltip, tooltip.background { background: %s; color: %s; border: 1px solid %s; "
		"border-radius: 0; box-shadow: none; }\n"
		"tooltip > * { color: %s; }\n",
		C(shadow), C(hi), C(hi),
		C(field), sunken,
		C(hl), C(hl_fg),
		C(face), C(fg), raised, C(face), C(face), pressed,
		C(face), C(fg), raised, C(dark),
		C(hi), C(shadow), C(shadow), C(hi),
		C(dim), C(hi),
		C(field), C(field_fg), sunken, C(face), raised, C(field_fg),
		C(face), C(fg), raised,
		C(field), sunken, C(hl), C(face), raised,
		C(field), C(field_fg), sunken,
		C(face), sunken, C(face), raised,
		C(field), sunken, C(hl),
		C(light), C(face), raised,
		raised, C(shadow), C(light),
		C(tw_theme_color(t, "tooltip.bg", 0xffffe1ff)),
		C(tw_theme_color(t, "tooltip.fg", 0x000000ff)),
		C(tw_theme_color(t, "tooltip.border", 0x000000ff)),
		C(tw_theme_color(t, "tooltip.fg", 0x000000ff)));
}

/* ---------- Windows XP: Luna ---------- */

static void add_xp(GString *css, const struct tw_theme *t, bool dark) {
	uint32_t hl = tw_theme_color(t, "menu.hl_bg", 0x316ac5ff) | 0xff;
	uint32_t bg = dark ? tw_theme_color(t, "menu.bg", 0x2b2b2bff) | 0xff : 0xece9d8ff;
	uint32_t fg = dark ? tw_theme_color(t, "menu.fg", 0xf0f0f0ff) | 0xff : 0x000000ff;
	uint32_t base = dark ? mix(bg, 0x000000ff, 0.25) : 0xffffffff;
	uint32_t field = tw_theme_color(t, "menu.field_bg", dark ? 0x1f1f1fff : 0xffffffff);
	uint32_t border = dark ? 0x555555ff : 0xd0d0bfff;
	struct palette p = { .bg = bg, .fg = fg, .dim = dark ? 0xa8a8a8ff : 0x5f5f5fff,
		.base = base, .base_fg = fg, .border = border, .accent = hl, .accent_fg = 0xffffffff,
		.hl = hl, .hl_fg = 0xffffffff, .side = 0x6f8fe0ff };
	add_common(css, &p);
	// the button face of Luna, white to its beige; dark a grey of the same shape
	const char *face = dark ? "linear-gradient(to bottom, #4c4c4c, #3e3e3e 85%, #333333)" :
		"linear-gradient(to bottom, #ffffff, #ecebe6 85%, #d6d0c5)";
	const char *face_pressed = dark ? "linear-gradient(to bottom, #2e2e2e, #3a3a3a)" :
		"linear-gradient(to bottom, #e3e2da, #f6f6f3)";
	const char *frame = dark ? "#16233f" : "#003c74";
	g_string_append_printf(css,
		// the task pane on the left: blue, with its links in a light box
		".tw-side { background-image: linear-gradient(to bottom, %s, %s); }\n"
		".tw-side .navigation-sidebar { background-color: %s; border: 1px solid %s; "
		"border-radius: 3px; margin: 4px 10px 10px 10px; padding: 4px 0; }\n"
		".navigation-sidebar > row { color: %s; border-radius: 0; margin: 0; }\n"
		".navigation-sidebar > row:hover { background: transparent; color: %s; }\n"
		".navigation-sidebar > row:hover label { text-decoration-line: underline; }\n"
		".navigation-sidebar > row:selected, .navigation-sidebar > row:selected:hover "
		"{ background-color: %s; color: #ffffff; }\n"
		".tw-title { color: %s; }\n"
		"list.tw-group { border: 1px solid %s; border-radius: 4px; }\n"
		"button { background-image: %s; background-color: transparent; color: %s; "
		"border: 1px solid %s; border-radius: 3px; box-shadow: none; text-shadow: none; "
		"padding: 3px 12px; min-height: 19px; }\n"
		// the orange glow of the button under the pointer
		"button:hover { box-shadow: inset 0 2px #ffe7a2, inset 0 -2px #e5a01a, "
		"inset 2px 0 #fbd17e, inset -2px 0 #fbd17e; }\n"
		"button:active, button:checked { background-image: %s; box-shadow: none; }\n"
		"button.suggested-action { box-shadow: inset 0 2px #cee7ff, inset 0 -2px #6982ee, "
		"inset 2px 0 #bcd4f6, inset -2px 0 #89adf0; color: %s; }\n"
		"button.suggested-action:hover { box-shadow: inset 0 2px #ffe7a2, inset 0 -2px #e5a01a, "
		"inset 2px 0 #fbd17e, inset -2px 0 #fbd17e; }\n"
		"button.flat { background-image: none; border-color: transparent; }\n"
		"button.flat:hover { border-color: %s; background-image: %s; }\n"
		"button:disabled { border-color: #c9c7ba; color: #aca899; box-shadow: none; }\n"
		"entry, spinbutton, dropdown > button.toggle { background-image: none; "
		"background-color: %s; color: %s; border: 1px solid #7f9db9; border-radius: 0; "
		"box-shadow: none; }\n"
		"dropdown > button.toggle:hover { box-shadow: none; }\n"
		"dropdown > button.toggle arrow { background-image: linear-gradient(to bottom, #c1d3fb, "
		"#a0bbf1); border: 1px solid #b9cdf8; border-radius: 2px; color: #4d6185; "
		"min-width: 15px; min-height: 17px; margin: -3px -10px -3px 4px; }\n"
		"spinbutton > button { border: none; border-radius: 0; background-image: linear-gradient("
		"to bottom, #c1d3fb, #a0bbf1); color: #4d6185; box-shadow: none; }\n"
		"spinbutton > text, entry > text { background: transparent; color: %s; }\n"
		"switch { background-image: none; background-color: %s; border: 1px solid #1c5180; "
		"border-radius: 3px; }\n"
		"switch:checked { background-color: %s; }\n"
		"switch > slider { background-image: %s; border: 1px solid %s; border-radius: 2px; "
		"box-shadow: none; }\n"
		"switch image { color: transparent; }\n"
		"check, radio { background-image: linear-gradient(135deg, #dcdcd7, #ffffff); "
		"background-color: transparent; border: 1px solid #1c5180; border-radius: 0; color: #21a121; "
		"box-shadow: none; min-width: 13px; min-height: 13px; -gtk-icon-size: 11px; }\n"
		"radio { border-radius: 9999px; }\n"
		"check:hover, radio:hover { box-shadow: inset 0 0 0 2px #fbc65e; }\n"
		"scale > trough { background-color: #ecebe6; border: 1px solid #9d9c99; border-radius: 2px; "
		"min-height: 4px; }\n"
		"scale > trough > highlight { background: transparent; border: none; }\n"
		"scale > trough > slider { background-image: linear-gradient(to bottom, #ffffff, #ecebe6); "
		"border: 1px solid #1c5180; border-bottom-color: #21a121; border-radius: 2px; "
		"min-width: 11px; min-height: 21px; margin: -10px -6px; box-shadow: none; }\n"
		"progressbar > trough { background-color: #ffffff; border: 1px solid #686868; "
		"border-radius: 3px; min-height: 13px; }\n"
		"progressbar > trough > progress { background-image: linear-gradient(to bottom, #a8f0a0, "
		"#2fb52f 50%%, #8ce28a); border-radius: 0; }\n"
		"scrollbar > range > trough > slider { background-image: linear-gradient(to right, "
		"#c8d6fb, #bacdf7); border: 1px solid #94b0f7; border-radius: 2px; min-width: 10px; }\n"
		"popover > contents { border: 1px solid #8a867a; border-radius: 0; padding: 2px; "
		"box-shadow: 2px 2px 3px rgba(0,0,0,0.25); }\n"
		"popover > arrow { background: transparent; border: none; }\n"
		"popover row { border-radius: 0; }\n"
		"flowboxchild.tw-card:selected { background-color: %s; border-radius: 3px; }\n"
		"tooltip, tooltip.background { background: #ffffe1; color: #000000; "
		"border: 1px solid #000000; border-radius: 0; }\n"
		"tooltip > * { color: #000000; }\n",
		dark ? "#1c3f8a" : "#7ba2e7", dark ? "#14295c" : "#6375d6",
		dark ? "#222c44" : "#d6dff7", dark ? "#3a4a70" : "#ffffff",
		dark ? "#c7d6f5" : "#215dc6", dark ? "#ffffff" : "#428eff", C(hl),
		dark ? "#8fb6ff" : "#003399", C(border),
		face, C(fg), frame, face_pressed, C(fg), frame, face,
		C(field), C(fg), C(fg), C(field), C(hl), face, frame,
		C((hl & 0xffffff00) | 0x60));
}

/* ---------- Windows 7 and Vista: Aero ---------- */

static void add_win7(GString *css, const struct tw_theme *t, bool dark) {
	uint32_t bg = dark ? tw_theme_color(t, "menu.bg", 0x2b2b2bff) | 0xff : 0xffffffff;
	uint32_t fg = dark ? tw_theme_color(t, "menu.fg", 0xf0f0f0ff) | 0xff : 0x000000ff;
	uint32_t field = tw_theme_color(t, "menu.field_bg", dark ? 0x1f1f1fff : 0xffffffff);
	uint32_t accent = tw_theme_color(t, "flyout.accent", 0x3399ffff) | 0xff;
	struct palette p = { .bg = bg, .fg = fg, .dim = dark ? 0xa0a0a0ff : 0x5d5d5dff,
		.base = dark ? mix(bg, 0xffffffff, 0.04) : 0xffffffff, .base_fg = fg,
		.border = dark ? 0x4a4a4aff : 0xd5dfe5ff, .accent = accent, .accent_fg = 0xffffffff,
		.hl = dark ? 0x3a5a80ff : 0xd8e6faff, .hl_fg = fg,
		.side = dark ? mix(bg, 0x1e395bff, 0.4) : 0xe8f0f9ff };
	add_common(css, &p);
	const char *face = dark ?
		"linear-gradient(to bottom, #4a4a4a, #3f3f3f 50%, #353535 50%, #2e2e2e)" :
		"linear-gradient(to bottom, #f2f2f2, #ebebeb 50%, #dddddd 50%, #cfcfcf)";
	g_string_append_printf(css,
		".tw-side { background-image: linear-gradient(to bottom, %s, %s); }\n"
		".navigation-sidebar > row { color: %s; border-radius: 3px; border: 1px solid transparent; }\n"
		".navigation-sidebar > row:hover { background-image: linear-gradient(to bottom, %s, %s); "
		"border-color: %s; }\n"
		".navigation-sidebar > row:selected, .navigation-sidebar > row:selected:hover, "
		"flowboxchild.tw-card:selected { background-image: linear-gradient(to bottom, %s, %s); "
		"border: 1px solid %s; color: %s; }\n"
		".tw-title { color: %s; font-weight: normal; }\n"
		"list.tw-group { border: 1px solid %s; border-radius: 3px; }\n"
		"button { background-image: %s; background-color: transparent; color: %s; "
		"border: 1px solid %s; border-radius: 3px; box-shadow: inset 0 0 0 1px %s; "
		"text-shadow: none; padding: 3px 12px; min-height: 19px; }\n"
		"button:hover { background-image: linear-gradient(to bottom, #eaf6fd, #d9f0fc 50%%, "
		"#bee6fd 50%%, #a7d9f5); border-color: #3c7fb1; color: #000000; }\n"
		"button:active, button:checked { background-image: linear-gradient(to bottom, #e5f4fc, "
		"#c4e5f6 50%%, #98d1ef 50%%, #68b3db); border-color: #2c628b; color: #000000; "
		"box-shadow: none; }\n"
		"button.suggested-action { border-color: #3399ff; box-shadow: inset 0 0 0 1px #48d8fb; }\n"
		"button.flat { background-image: none; border-color: transparent; box-shadow: none; }\n"
		"button:disabled, button:disabled:hover { opacity: 0.5; }\n"
		"entry, spinbutton, dropdown > button.toggle { background-color: %s; color: %s; "
		"border: 1px solid %s; border-top-color: %s; border-radius: 2px; box-shadow: none; }\n"
		"dropdown > button.toggle { background-image: %s; }\n"
		"entry:focus-within, spinbutton:focus-within { border-color: #3d7bad; "
		"border-top-color: #3d7bad; }\n"
		"spinbutton > text, entry > text { background: transparent; color: %s; }\n"
		"spinbutton > button { border: none; border-radius: 0; box-shadow: none; }\n"
		"switch { background-image: none; background-color: %s; border: 1px solid #8f8f8f; "
		"border-radius: 3px; }\n"
		"switch:checked { background-color: %s; border-color: #2c628b; }\n"
		"switch > slider { background-image: %s; border: 1px solid #707070; border-radius: 2px; "
		"box-shadow: none; }\n"
		"switch image { color: transparent; }\n"
		"check, radio { background-image: linear-gradient(135deg, #dcdcdc, #fbfbfb); "
		"border: 1px solid #8f8f8f; border-radius: 0; color: #4a5f97; box-shadow: none; "
		"min-width: 13px; min-height: 13px; -gtk-icon-size: 11px; }\n"
		"radio { border-radius: 9999px; }\n"
		"check:hover, radio:hover { border-color: #3c7fb1; }\n"
		"scale > trough { background-color: #e7eaea; border: 1px solid #b0b0b0; border-radius: 2px; "
		"min-height: 4px; }\n"
		"scale > trough > highlight { background: transparent; border: none; }\n"
		"scale > trough > slider { background-image: %s; border: 1px solid #707070; "
		"border-radius: 2px; min-width: 11px; min-height: 19px; margin: -9px -6px; "
		"box-shadow: none; }\n"
		"progressbar > trough { background-color: #e6e6e6; border: 1px solid #bcbcbc; "
		"border-radius: 2px; min-height: 14px; }\n"
		"progressbar > trough > progress { background-image: linear-gradient(to bottom, #37d643, "
		"#06b025); border-radius: 0; }\n"
		"popover > contents { border: 1px solid #979797; border-radius: 0; padding: 2px; "
		"box-shadow: 3px 3px 4px rgba(0,0,0,0.2); }\n"
		"popover > arrow { background: transparent; border: none; }\n"
		"popover row { border-radius: 3px; }\n"
		"tooltip, tooltip.background { background-image: linear-gradient(to bottom, #ffffff, "
		"#e4e5f0); color: #575757; border: 1px solid #767676; border-radius: 2px; }\n"
		"tooltip > * { color: #575757; }\n",
		dark ? "#2a3446" : "#e3edf8", dark ? "#222a38" : "#d5e3f2",
		dark ? "#cfe0f7" : "#1e395b",
		dark ? "#344866" : "#fafbfd", dark ? "#2b3c56" : "#ebf3fd", dark ? "#4a6894" : "#b8d6fb",
		dark ? "#3c5a84" : "#f1f7fe", dark ? "#30486a" : "#dcebfc", dark ? "#5a80b8" : "#7da2ce",
		C(fg),
		dark ? "#99c6ff" : "#1e395b", dark ? "#4a4a4a" : "#d5dfe5",
		face, C(fg), dark ? "#1f1f1f" : "#707070", dark ? "rgba(255,255,255,0.08)" :
		"rgba(255,255,255,0.75)",
		C(field), C(fg), dark ? "#555555" : "#e3e9ef", dark ? "#555555" : "#abadb3", face, C(fg),
		C(field), C(accent), face, face);
}

/* ---------- Windows 8 and 10: flat ---------- */

static void add_flat(GString *css, const struct tw_theme *t, bool dark, uint32_t accent) {
	uint32_t menu = tw_theme_color(t, "menu.bg", dark ? 0x2b2b2bff : 0xf2f2f2ff) | 0xff;
	uint32_t bg = dark ? 0x000000ff : 0xffffffff;
	uint32_t fg = dark ? 0xffffffff : 0x000000ff;
	uint32_t field = dark ? 0x000000ff : 0xffffffff;
	uint32_t line = dark ? 0x999999ff : 0x7a7a7aff;
	struct palette p = { .bg = bg, .fg = fg, .dim = dark ? 0x9a9a9aff : 0x5d5d5dff,
		.base = bg, .base_fg = fg, .border = dark ? 0x333333ff : 0xe5e5e5ff, .accent = accent,
		.accent_fg = 0xffffffff, .hl = dark ? 0x333333ff : 0xe5e5e5ff, .hl_fg = fg,
		.side = dark ? mix(menu, 0x000000ff, 0.3) : menu };
	add_common(css, &p);
	g_string_append_printf(css,
		"window * { text-shadow: none; }\n"
		// the sizes stay those of GTK: thick lines are drawn inside, as shadows
		".navigation-sidebar > row { border-radius: 0; }\n"
		".navigation-sidebar > row:hover { background-color: %s; }\n"
		".navigation-sidebar > row:selected, .navigation-sidebar > row:selected:hover "
		"{ background-color: %s; color: %s; box-shadow: inset 4px 0 %s; }\n"
		".tw-title { font-weight: 300; }\n"
		"list.tw-group { border: 1px solid %s; border-radius: 0; }\n"
		"button { background-image: none; background-color: %s; color: %s; "
		"border-color: transparent; border-radius: 0; box-shadow: none; }\n"
		"button:hover { box-shadow: inset 0 0 0 2px %s; }\n"
		"button:active, button:checked { background-color: %s; box-shadow: inset 0 0 0 2px %s; }\n"
		"button.suggested-action { background-color: %s; color: #ffffff; }\n"
		"button.suggested-action:hover { box-shadow: inset 0 0 0 2px %s; }\n"
		"button.flat { background-color: transparent; }\n"
		"button.flat:hover { background-color: %s; box-shadow: none; }\n"
		"button:disabled, button:disabled:hover { opacity: 0.45; box-shadow: none; }\n"
		"entry, spinbutton, dropdown > button.toggle { background-color: %s; color: %s; "
		"border-color: transparent; border-radius: 0; box-shadow: inset 0 0 0 2px %s; }\n"
		"dropdown > button.toggle { background-color: %s; box-shadow: none; }\n"
		"dropdown > button.toggle:hover { box-shadow: inset 0 0 0 2px %s; }\n"
		"entry:focus-within, spinbutton:focus-within { box-shadow: inset 0 0 0 2px %s; "
		"outline: none; }\n"
		"spinbutton > text, entry > text { background: transparent; color: %s; }\n"
		"spinbutton > button { border: none; border-radius: 0; }\n"
		// the toggle switch of Windows 10: an outlined pill with a dot
		// (the knob is a ring of the ground's colour around a dot, keeping GTK's size)
		"switch { background-color: transparent; border-color: %s; border-radius: 9999px; "
		"box-shadow: inset 0 0 0 1px %s; }\n"
		"switch:checked { background-color: %s; border-color: %s; box-shadow: none; }\n"
		"switch > slider { background: none; border-color: transparent; border-radius: 9999px; "
		"box-shadow: inset 0 0 0 5px %s, inset 0 0 0 20px %s; }\n"
		"switch:checked > slider { box-shadow: inset 0 0 0 5px %s, inset 0 0 0 20px #ffffff; }\n"
		"switch image { color: transparent; }\n"
		"check, radio { background-color: transparent; border-color: %s; border-radius: 0; "
		"color: %s; box-shadow: inset 0 0 0 1px %s; }\n"
		"check:checked { background-color: %s; border-color: %s; color: #ffffff; box-shadow: none; }\n"
		"radio { border-radius: 9999px; }\n"
		"radio:checked { background-color: transparent; color: %s; border-color: %s; }\n"
		"scale > trough { background-color: %s; border-color: transparent; border-radius: 0; }\n"
		"scale > trough > highlight { background-color: %s; border-color: transparent; "
		"border-radius: 0; }\n"
		"scale > trough > slider { background: %s; border-color: %s; border-radius: 4px; "
		"box-shadow: none; }\n"
		"scale > trough > slider:hover { background: %s; }\n"
		"progressbar > trough { background-color: %s; border-radius: 0; }\n"
		"progressbar > trough > progress { background-color: %s; border-radius: 0; }\n"
		"popover > contents { border: 1px solid %s; border-radius: 0; "
		"box-shadow: 0 2px 8px rgba(0,0,0,0.3); }\n"
		"popover > arrow { background: transparent; border: none; }\n"
		"popover row { border-radius: 0; }\n"
		"flowboxchild.tw-card { border-radius: 0; }\n"
		"flowboxchild.tw-card:selected { background-color: transparent; "
		"box-shadow: inset 0 0 0 2px %s; }\n"
		"tooltip, tooltip.background { background: %s; color: %s; border: 1px solid %s; "
		"border-radius: 0; }\n"
		"tooltip > * { color: %s; }\n",
		dark ? "#2b2b2b" : "#e6e6e6",
		dark ? "#333333" : "#d9d9d9", C(fg), C(accent),
		dark ? "#333333" : "#e5e5e5",
		dark ? "#333333" : "#cccccc", C(fg), dark ? "#858585" : "#7a7a7a",
		dark ? "#666666" : "#999999", dark ? "#666666" : "#999999",
		C(accent), C(mix(accent, 0x000000ff, 0.35)),
		dark ? "#333333" : "#e6e6e6",
		C(field), C(fg), dark ? "#666666" : "#999999",
		dark ? "#333333" : "#cccccc", dark ? "#858585" : "#7a7a7a",
		C(accent), C(fg),
		C(line), C(line), C(accent), C(accent), C(bg), C(fg), C(accent),
		C(line), C(fg), C(line), C(accent), C(accent), C(accent), C(line),
		dark ? "#666666" : "#999999", C(accent), C(accent), C(accent), C(fg),
		dark ? "#333333" : "#cccccc", C(accent),
		dark ? "#767676" : "#cccccc",
		C(accent),
		dark ? "#2b2b2b" : "#f2f2f2", C(fg), dark ? "#767676" : "#cccccc", C(fg));
}

/* ---------- Windows 11: Fluent ---------- */

static void add_win11(GString *css, const struct tw_theme *t, bool dark, uint32_t accent) {
	uint32_t bg = dark ? 0x202020ff : 0xf3f3f3ff;
	uint32_t fg = dark ? 0xffffffff : 0x1b1b1bff;
	uint32_t card = dark ? 0x2b2b2bff : 0xfbfbfbff;
	// on dark the accent is a lighter shade of itself, as Windows 11 does
	uint32_t on = dark ? mix(accent, 0xffffffff, 0.45) : accent;
	struct palette p = { .bg = bg, .fg = fg, .dim = dark ? 0xc5c5c5ff : 0x5f5f5fff,
		.base = card, .base_fg = fg, .border = dark ? 0x1d1d1dff : 0xe5e5e5ff, .accent = on,
		.accent_fg = dark ? 0x000000ff : 0xffffffff, .hl = dark ? 0x383838ff : 0xeaeaeaff,
		.hl_fg = fg, .side = bg };
	add_common(css, &p);
	g_string_append_printf(css,
		"window * { text-shadow: none; }\n"
		".navigation-sidebar > row { border-radius: 5px; }\n"
		".navigation-sidebar > row:hover { background-color: %s; }\n"
		".navigation-sidebar > row:selected, .navigation-sidebar > row:selected:hover "
		"{ background-color: %s; color: %s; box-shadow: inset 3px 0 %s; }\n"
		".tw-title { font-weight: 600; }\n"
		"list.tw-group { border: 1px solid %s; border-radius: 7px; }\n"
		"button { background-image: none; background-color: %s; color: %s; "
		"border: 1px solid %s; border-bottom-color: %s; border-radius: 4px; box-shadow: none; }\n"
		"button:hover { background-color: %s; }\n"
		"button:active, button:checked { background-color: %s; color: %s; }\n"
		"button.suggested-action { background-color: %s; color: %s; border-color: %s; }\n"
		"button.suggested-action:hover { background-color: %s; }\n"
		"button.flat { background-color: transparent; border-color: transparent; }\n"
		"button.flat:hover { background-color: %s; }\n"
		"button:disabled, button:disabled:hover { opacity: 0.45; }\n"
		"entry, spinbutton { background-color: %s; color: %s; border: 1px solid %s; "
		"border-bottom-color: %s; border-radius: 4px; box-shadow: none; }\n"
		"entry:focus-within, spinbutton:focus-within { background-color: %s; "
		"box-shadow: inset 0 -1px %s; border-bottom-color: transparent; outline: none; }\n"
		"spinbutton > text, entry > text { background: transparent; color: %s; }\n"
		"spinbutton > button { border: none; background-color: transparent; }\n"
		"switch { background-color: transparent; border-color: %s; border-radius: 9999px; "
		"box-shadow: none; }\n"
		"switch:checked { background-color: %s; border-color: %s; }\n"
		"switch > slider { background: none; border-color: transparent; border-radius: 9999px; "
		"box-shadow: inset 0 0 0 5px %s, inset 0 0 0 20px %s; }\n"
		"switch:checked > slider { box-shadow: inset 0 0 0 5px %s, inset 0 0 0 20px %s; }\n"
		"switch image { color: transparent; }\n"
		"check, radio { background-color: %s; border-color: %s; border-radius: 4px; "
		"box-shadow: none; }\n"
		"check:checked, radio:checked { background-color: %s; border-color: %s; color: %s; }\n"
		"radio { border-radius: 9999px; }\n"
		"scale > trough { background-color: %s; border-color: transparent; border-radius: 2px; }\n"
		"scale > trough > highlight { background-color: %s; border-color: transparent; "
		"border-radius: 2px; }\n"
		// a dot of the accent in a ring
		"scale > trough > slider { background: %s; border-color: %s; border-radius: 9999px; "
		"box-shadow: inset 0 0 0 5px %s; }\n"
		"progressbar > trough { background-color: %s; border-radius: 2px; }\n"
		"progressbar > trough > progress { background-color: %s; border-radius: 2px; }\n"
		"popover > contents { border: 1px solid %s; border-radius: 8px; "
		"box-shadow: 0 8px 16px rgba(0,0,0,0.14); }\n"
		"popover > arrow { background: transparent; border: none; }\n"
		"popover row { border-radius: 4px; }\n"
		"flowboxchild.tw-card:selected { background-color: %s; border-radius: 7px; }\n"
		"tooltip, tooltip.background { background: %s; color: %s; border: 1px solid %s; "
		"border-radius: 4px; }\n"
		"tooltip > * { color: %s; }\n",
		dark ? "#2d2d2d" : "#eaeaea",
		dark ? "#2d2d2d" : "#eaeaea", C(fg), C(on),
		dark ? "#1d1d1d" : "#e5e5e5",
		dark ? "#2d2d2d" : "#fbfbfb", C(fg), dark ? "#353535" : "#e5e5e5",
		dark ? "#2a2a2a" : "#cccccc",
		dark ? "#323232" : "#f6f6f6", dark ? "#272727" : "#f5f5f5", dark ? "#cecece" : "#5d5d5d",
		C(on), dark ? "#000000" : "#ffffff", C(on), C(mix(on, dark ? 0x000000ff : 0xffffffff, 0.1)),
		dark ? "#2d2d2d" : "#eaeaea",
		dark ? "#2d2d2d" : "#ffffff", C(fg), dark ? "#303030" : "#e5e5e5",
		dark ? "#9a9a9a" : "#868686", dark ? "#1f1f1f" : "#ffffff", C(on), C(fg),
		dark ? "#9a9a9a" : "#8a8a8a", C(on), C(on), C(card), dark ? "#cecece" : "#5d5d5d",
		C(on), dark ? "#000000" : "#ffffff",
		dark ? "#272727" : "#f5f5f5", dark ? "#9a9a9a" : "#8a8a8a", C(on), C(on),
		dark ? "#000000" : "#ffffff",
		dark ? "#9a9a9a" : "#8a8a8a", C(on), C(on),
		dark ? "rgba(0,0,0,0.2)" : "rgba(0,0,0,0.1)", dark ? "#454545" : "#ffffff",
		dark ? "#9a9a9a" : "#8a8a8a", C(on),
		dark ? "#1d1d1d" : "#e5e5e5",
		C((on & 0xffffff00) | 0x40),
		dark ? "#2c2c2c" : "#f9f9f9", C(fg), dark ? "#1d1d1d" : "#e5e5e5", C(fg));
}

char *settings_theme_css(const struct tw_theme *t) {
	GString *css = g_string_new(NULL);
	if (!t) {
		return g_string_free(css, FALSE);
	}
	enum era era = era_of(t);
	// the newer ones are light or dark as the theme's menus are
	bool dark = is_dark(tw_theme_color(t, "menu.bg", t->dark ? 0x2b2b2bff : 0xf2f2f2ff) | 0xff);
	uint32_t accent = tw_theme_color(t, "flyout.accent", tw_theme_color(t, "accent.color",
		tw_theme_color(t, "taskbar.indicator", 0x0078d4ff))) | 0xff;
	// the Settings of Windows 10 and 11 write larger than their menus, in a font
	// of the system (Segoe UI) that is rarely there: those keep the font of GTK
	if (era != ERA_FLAT && era != ERA_WIN11) {
		add_font(css, t);
	}
	switch (era) {
	case ERA_CLASSIC:
		add_classic(css, t);
		break;
	case ERA_XP:
		add_xp(css, t, dark);
		break;
	case ERA_WIN7:
		add_win7(css, t, dark);
		break;
	case ERA_FLAT:
		add_flat(css, t, dark, accent);
		break;
	case ERA_WIN11:
		add_win11(css, t, dark, accent);
		break;
	}
	return g_string_free(css, FALSE);
}

static GtkCssProvider *provider;

void settings_apply_theme(void) {
	char *name = settings_current_theme();
	char *error = NULL;
	struct tw_theme *t = name ? tw_theme_load(name, &error) : NULL;
	free(error);
	g_free(name);
	char *css = settings_theme_css(t);
	if (t) {
		settings_apply_color_scheme(t->dark);
	}
	tw_theme_free(t);
	GdkDisplay *display = gdk_display_get_default();
	if (!display) {
		g_free(css);
		return;
	}
	if (!provider) {
		provider = gtk_css_provider_new();
		// above the window's own style sheet, which is APPLICATION
		gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	gtk_css_provider_load_from_string(provider, css);
	g_free(css);
}
