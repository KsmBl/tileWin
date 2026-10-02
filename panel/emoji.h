#ifndef _TILEWIN_PANEL_EMOJI_H
#define _TILEWIN_PANEL_EMOJI_H

/* The emoji of Unicode, in its order and groups (emoji_data.c, generated). */
struct emoji {
	const char *chars; // UTF-8
	const char *name;  // "grinning face"
};

struct emoji_group {
	const char *name;
	int first, count; // in emoji_list
};

extern const struct emoji_group emoji_groups[];
extern const int emoji_group_count;
extern const struct emoji emoji_list[];
extern const int emoji_count;

#endif
