#ifndef _TW_ACCENT_H
#define _TW_ACCENT_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The accent color of the desktop. A theme writes "$accent" (or
 * "$accent_light", "$accent_dark", each with an optional "/alpha" such as
 * "$accent/d0") where Windows uses its accent color; that is the theme's own
 * "accent.color", or with "accent wallpaper" (~/.config/tileWin/accent) the
 * most striking color of the wallpaper, which the compositor works out and
 * keeps in ~/.cache/tileWin/accent.
 */

/* Whether the accent comes from the wallpaper. */
bool tw_accent_from_wallpaper(void);
bool tw_accent_set_from_wallpaper(bool enable);
/* The color worked out from the wallpaper, false if there is none yet. */
bool tw_accent_wallpaper_color(uint32_t *color);
/* Keeps the wallpaper's color; true when it changed. */
bool tw_accent_store(uint32_t color);
/* Reads the files again (another process changed them). */
void tw_accent_reload(void);

/*
 * The most striking color of a picture (premultiplied ARGB32, as cairo has
 * it): the hue most of its colorful pixels share, made dark enough for white
 * text on it. False for a picture with no color to speak of.
 */
bool tw_accent_pick(const unsigned char *data, int width, int height, int stride,
	uint32_t *color);

#endif
