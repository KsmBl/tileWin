#ifndef _TW_SAVERS_H
#define _TW_SAVERS_H
#include <cairo.h>
#include <stdbool.h>

/*
 * The screen savers. Each draws itself with cairo into whatever it is given:
 * a whole screen in tilewin-screensaver, or the little monitor on the Screen
 * saver page of the settings. Sizes are relative to the area, so the same
 * code serves both.
 *
 * Each screen saver is a directory of its own: in it saver.so, a module that
 * draws it, and screenshot.png, a picture of it for the settings. The
 * directories are looked for in $TILEWIN_SAVERS (folders separated by
 * colons), in ~/.local/share/tileWin/screensavers and in the screensavers
 * folder of tileWin's data; the first of a name found is the one. How to make
 * one of your own: screensaver/README.md.
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
	const char *when;                // shown only while another choice of the saver is
	                                 // this, "<key>=<value>"; NULL for always
};

struct saver_options {
	double speed;          // 1 is normal, 0.25 to 4
	const char *text;      // 3D Text: the words, or "time" for the clock
	const char *photos;    // Photos: the folder of the slideshow, NULL for Pictures
	int photo_seconds;     // Photos: how long each picture stays
	const char *output;    // the screen it is shown on, NULL for a preview
	cairo_surface_t *desktop; // that screen as it was before the saver covered it, or NULL
	bool show_stats;       // shows frames a second and the processor's use in a corner
	const char *const *settings; // the savers' own settings: name, value, name, value...
	int setting_count;           // names and values together
};

struct saver {
	const char *name, *title, *description;
	bool transparent;      // drawn over the desktop, which shows through
	double resolution;     // drawn at this part of the size and scaled up, for soft ones
	bool wants_desktop;    // gets a picture of the screen from before it started
	bool covers;           // paints every pixel of every frame itself: no clearing first
	const struct saver_option *options; // its own settings, NULL for none
	void *(*create)(int width, int height, const struct saver_options *options);
	/* Draws the next picture over black (or the desktop); dt is seconds, sped up. */
	void (*draw)(void *state, cairo_t *cr, int width, int height, double dt);
	void (*destroy)(void *state);
};

/*
 * What the module of a screen saver gives, as tilewin_saver_module:
 *
 *     TILEWIN_SAVER(.saver = &my_saver, .order = 0);
 */
#define TILEWIN_SAVER_API 1

struct saver_module {
	int api;                           // TILEWIN_SAVER_API, as it was built
	const struct saver *saver;         // the saver
	int order;                         // its place in the list, lower first; 0: after the rest
	double shot_seconds;               // how long it runs before its screenshot; 0: 5
	const struct saver *const *hidden; // savers it runs that are found by name but not listed,
	                                   // NULL-terminated; or NULL
};

#define TILEWIN_SAVER(...) \
	const struct saver_module tilewin_saver_module = { .api = TILEWIN_SAVER_API, __VA_ARGS__ }

/* The screen savers found, in the order of the list: loaded at the first need. */
extern const struct saver **savers;
extern int saver_count;
void saver_load_all(void);

/* The directory a saver came from, and its screenshot there; NULL when it has none. */
const char *saver_dir(const struct saver *saver);
const char *saver_screenshot(const struct saver *saver);

/*
 * Loads the saver of one directory alone (not into the list); NULL, with the
 * reason in *why, when it is not one.
 */
const struct saver_module *saver_load_dir(const char *dir, char **why);

/* A choice of a saver's own settings, as the index into its values; 0 when not set. */
int saver_choice(const struct saver_options *options, const struct saver *saver, const char *key);
/* A toggle of a saver's own settings, its default when not set. */
bool saver_toggle(const struct saver_options *options, const struct saver *saver, const char *key);

/*
 * The saver of that name; "random" picks one. NULL for none or an unknown name.
 * The kinds of Doomsday are found by their own names too (hellfire,
 * thunderstorm, blizzard), which is what they were called before.
 */
const struct saver *saver_find(const char *name);

/* The saver of the list that shows the one of that name: itself, or Doomsday for its kinds. */
const struct saver *saver_listed(const char *name);

/* Whether an option of a saver is shown with these settings, going by its "when". */
bool saver_option_shown(const struct saver_options *options, const struct saver *saver,
	const struct saver_option *option);

/*
 * One saver running on one area, with the scaling and the clearing done for
 * it, and with show_stats the frames a second, the time a frame takes and the
 * use of the processor (all of it, and of that this program's share) in its
 * top left corner, for any saver.
 */
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
