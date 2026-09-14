#ifndef _TW_PATHS_H
#define _TW_PATHS_H
#include <stdbool.h>

/* Expands a leading "~" to $HOME. Returns a newly allocated string. */
char *tw_expand_home(const char *path);

/* $XDG_CONFIG_HOME/tileWin (default ~/.config/tileWin). Newly allocated. */
char *tw_config_dir(void);

/* $XDG_STATE_HOME/tileWin (default ~/.local/state/tileWin). Newly allocated. */
char *tw_state_dir(void);

/* Installed data directory, overridable with $TILEWIN_DATADIR. */
const char *tw_data_dir(void);

bool tw_mkdir_p(const char *path);

/* Reads the first line of a file with surrounding whitespace stripped. */
char *tw_read_first_line(const char *path);

/* Atomically replaces a file with the given content, creating parents. */
bool tw_write_string(const char *path, const char *content);

/*
 * Looks up a relative path first in the user config dir, then in the data
 * dir. Returns a newly allocated absolute path or NULL.
 */
char *tw_find_data_file(const char *relpath);

#endif
