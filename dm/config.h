#ifndef _TILEWIN_DM_CONFIG_H
#define _TILEWIN_DM_CONFIG_H
#include <stdbool.h>
#include <stddef.h>

/*
 * /etc/tileWin/display-manager.conf: "key value" lines, # for comments. One table
 * says what can be set, so the daemon, the greeter, the check of a config and
 * the Login screen page of the settings all know the same options.
 *
 * Nothing in here may keep anyone from logging in: an unknown key or a value
 * that makes no sense is reported and the default is used.
 */
#define DM_CONFIG_PATH "/etc/tileWin/display-manager.conf"

enum dm_option_type {
	DM_BOOL,
	DM_INT,
	DM_CHOICE,
	DM_TEXT,
};

struct dm_option {
	const char *key;
	enum dm_option_type type;
	const char *group; // the group on the settings page
	const char *label;
	const char *help;
	const char *fallback;
	int min, max;           // DM_INT
	const char *const *choices; // DM_CHOICE, NULL at the end
};

extern const struct dm_option dm_options[];
extern const size_t dm_option_count;

struct dm_config {
	char **values; // as dm_options, never NULL
	char **env;    // "KEY=value" of the env lines, NULL at the end
	size_t env_count;
};

/* All defaults, and on top of them what the file says. A missing file is fine. */
struct dm_config *dm_config_load(const char *path);
void dm_config_free(struct dm_config *config);

const char *dm_config_str(const struct dm_config *config, const char *key);
int dm_config_int(const struct dm_config *config, const char *key);
bool dm_config_bool(const struct dm_config *config, const char *key);

/* Prints what is wrong with a file, and says how many things are. */
int dm_config_check(const char *path, void (*report)(const char *problem, void *data), void *data);

/* A name of a word in a space separated list. */
bool dm_word_in_list(const char *list, const char *word);

#endif
