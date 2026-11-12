#ifndef _TW_BRIGHTNESS_H
#define _TW_BRIGHTNESS_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Brightness over 100 %: with the backlight at full, tilewin-nightlight lifts
 * the dark and middle tones by a gamma curve, in the same gamma tables as the
 * night light's color temperature (only one program can hold those of a
 * screen). The boost, 100 (none) to TW_BOOST_MAX, is kept in
 * ~/.local/state/tileWin/brightness-boost, which the panel, the settings app
 * and tilewin-media write and the night light watches.
 */

#define TW_BOOST_MAX 130
/* The exponent at TW_BOOST_MAX: out = in ^ (1 / gamma). */
#define TW_BOOST_GAMMA 1.6

/* The boosted level of a channel level, both 0-1. 100 or less leaves it alone. */
double tw_boost_curve(double v, int boost);

/*
 * Fills a gamma table of size entries per channel (red, then green, then
 * blue): the boost curve times the channel factors of the color temperature.
 */
void tw_gamma_ramp(uint16_t *table, uint32_t size, const double rgb[3], int boost);

/* ~/.local/state/tileWin/brightness-boost. Newly allocated. */
char *tw_boost_path(void);

/* The boost of the state file, 100 when there is none. */
int tw_boost_read(void);

/* Writes the boost (clamped to 100..TW_BOOST_MAX) when it differs. */
bool tw_boost_write(int boost);

/* True while tilewin-nightlight runs, so a boost has an effect. */
bool tw_boost_available(void);

/*
 * The brightness to show for a backlight of that percentage: the boost
 * while the backlight is at full and the night light applies it.
 */
int tw_brightness_shown(int backlight);

/*
 * Sets a brightness of 0..TW_BOOST_MAX: writes the boost (100 for anything
 * up to 100, and when the night light does not run) and returns the
 * percentage the backlight is to be set to.
 */
int tw_brightness_split(int percent);

#endif
