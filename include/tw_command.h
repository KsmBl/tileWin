#ifndef _TW_COMMAND_H
#define _TW_COMMAND_H
#include <stdbool.h>

/*
 * The ranking of the Command Center (modernCC): how well a name matches what
 * was typed, and how often and how lately each thing was used, kept between
 * sessions in ~/.local/state/tileWin/command-center.
 */

/*
 * How well a name (or failing that, its keywords) matches what was typed,
 * ignoring case: 100 the name starts with it, 85 a word of the name starts with
 * it, 75 the initials of the name ("ts" for "Task Switcher"), 55 it is in the
 * name, 35 it starts a keyword, 15 its letters come in that order in the name
 * (four letters or more),
 * 0 not at all. Nothing typed: 1.
 */
int tw_cmd_match(const char *name, const char *keywords, const char *query);

struct tw_cmd_use {
	char *key;
	int count;
	long long last; // seconds since the epoch
};

struct tw_cmd_history {
	struct tw_cmd_use *items;
	int count, cap;
};

#define TW_CMD_HISTORY_MAX 300 // the most kept: those used last

/* "count last key" a line each; false when there is no file. */
bool tw_cmd_history_load(struct tw_cmd_history *h, const char *path);
bool tw_cmd_history_save(const struct tw_cmd_history *h, const char *path);
void tw_cmd_history_free(struct tw_cmd_history *h);
/* One more use of key, now. */
void tw_cmd_history_note(struct tw_cmd_history *h, const char *key, long long now);
/* More for what was used often and lately, 0 for what never was. */
double tw_cmd_frecency(const struct tw_cmd_history *h, const char *key, long long now);
/* ~/.local/state/tileWin/command-center, to free; NULL without a home. */
char *tw_cmd_history_path(void);

#endif
