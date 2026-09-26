#ifndef _TW_SAVERS_H
#define _TW_SAVERS_H
#include <cairo.h>
#include <stdbool.h>

/*
 * The screen savers. Each draws itself with cairo into whatever it is given:
 * a whole screen in tilewin-screensaver, or the little monitor on the Screen
 * saver page of the settings. Sizes are relative to the area, so the same
 * code serves both.
 */

/*
 * A setting of one saver, shown on the Screen saver page and kept in the
 * "screensaver" block of taskbar.conf as "<saver>_<key> <value>".
 */
enum saver_option_type {
	SAVER_CHOICE, // one of values, the first the default
	SAVER_TOGGLE, // yes or no
};

struct saver_option {
	const char *key;                 // NULL ends the list
	const char *label, *help;
	enum saver_option_type type;
	const char *const *values;       // a choice: what is written, NULL-terminated
	const char *const *labels;       // and what the settings show
	bool on;                         // a toggle: its default
};

struct saver_options {
	double speed;          // 1 is normal, 0.25 to 4
	const char *text;      // 3D Text: the words, or "time" for the clock
	const char *photos;    // Photos: the folder of the slideshow, NULL for Pictures
	int photo_seconds;     // Photos: how long each picture stays
	const char *output;    // the screen it is shown on, NULL for a preview
	cairo_surface_t *desktop; // that screen as it was before the saver covered it, or NULL
	const char *const *settings; // the savers' own settings: name, value, name, value...
	int setting_count;           // names and values together
};

struct saver {
	const char *name, *title, *description;
	bool transparent;      // drawn over the desktop, which shows through
	double resolution;     // drawn at this part of the size and scaled up, for soft ones
	bool wants_desktop;    // gets a picture of the screen from before it started
	const struct saver_option *options; // its own settings, NULL for none
	void *(*create)(int width, int height, const struct saver_options *options);
	/* Draws the next picture over black (or the desktop); dt is seconds, sped up. */
	void (*draw)(void *state, cairo_t *cr, int width, int height, double dt);
	void (*destroy)(void *state);
};

extern const struct saver *const savers[];
extern const int saver_count;

/* A choice of a saver's own settings, as the index into its values; 0 when not set. */
int saver_choice(const struct saver_options *options, const struct saver *saver, const char *key);
/* A toggle of a saver's own settings, its default when not set. */
bool saver_toggle(const struct saver_options *options, const struct saver *saver, const char *key);

/* The saver of that name; "random" picks one. NULL for none or an unknown name. */
const struct saver *saver_find(const char *name);

/* One saver running on one area, with the scaling and the clearing done for it. */
struct saver_run;
struct saver_run *saver_run_new(const struct saver *saver, int width, int height,
	const struct saver_options *options);
void saver_run_draw(struct saver_run *run, cairo_t *cr, double seconds);
void saver_run_free(struct saver_run *run);

/*
 * The "screensaver" block of taskbar.conf: which one ("name") and its options.
 * Returns the name, newly allocated, NULL when none is set; the strings of
 * options are allocated too and freed with saver_options_finish.
 */
char *saver_options_load(struct saver_options *options);
void saver_options_finish(struct saver_options *options);

#endif
