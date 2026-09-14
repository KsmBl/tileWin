#ifndef _TW_THEME_H
#define _TW_THEME_H
#include <stdbool.h>
#include <stdint.h>
#include "list.h"

#define TW_DEFAULT_THEME "win10"

/*
 * A theme is a directory containing theme.conf (decoration, panel, menu and
 * wallpaper settings in twconf syntax) and tile.conf (plain sway commands
 * applied in tile mode). Nested blocks are flattened into dotted keys, e.g.
 *
 *   decoration { active { title_bg #000080 } }  ->  decoration.active.title_bg
 *
 * `inherit <theme>` at the top level loads another theme first.
 */
struct tw_theme {
	char *name;  // directory name, e.g. "win95"
	char *dir;   // absolute directory path
	char *title; // human readable name
	char *style; // renderer: win95, winxp, win7, win10, win11
	bool dark;   // loaded with the dark color scheme
	list_t *kv;  // struct tw_theme_kv *, sorted by key
};

struct tw_theme_kv {
	char *key;
	char *value;
};

struct tw_theme *tw_theme_load(const char *name, char **error);
void tw_theme_free(struct tw_theme *theme);

const char *tw_theme_str(const struct tw_theme *theme, const char *key,
		const char *fallback);
int tw_theme_int(const struct tw_theme *theme, const char *key, int fallback);
double tw_theme_double(const struct tw_theme *theme, const char *key,
		double fallback);
bool tw_theme_bool(const struct tw_theme *theme, const char *key, bool fallback);
/* Colors are 0xRRGGBBAA. */
uint32_t tw_theme_color(const struct tw_theme *theme, const char *key,
		uint32_t fallback);

bool tw_parse_color(const char *str, uint32_t *color);

/* Directory of a theme by name (user themes shadow installed ones). */
char *tw_theme_find_dir(const char *name);
/* Sorted list of available theme names (char *). */
list_t *tw_theme_list(void);
/* Name stored in ~/.config/tileWin/current-theme, or TW_DEFAULT_THEME. */
char *tw_theme_current_name(void);
bool tw_theme_save_current(const char *name);
/*
 * Light or dark color scheme, stored in ~/.config/tileWin/color-scheme. With
 * the dark scheme, the keys of a theme's "dark { ... }" block replace the
 * normal ones.
 */
bool tw_color_scheme_is_dark(void);
/* True once a scheme was chosen, so tileWin does not touch app settings before. */
bool tw_color_scheme_is_set(void);
bool tw_color_scheme_save(bool dark);
/*
 * Directory of the theme's own icons: <theme>/icons of the theme named by
 * "icons.set" (default: the theme itself), or NULL if there is none.
 */
char *tw_theme_icon_dir(const struct tw_theme *theme);
/* Path of a file inside the theme directory. Newly allocated. */
char *tw_theme_file(const struct tw_theme *theme, const char *file);

#endif
