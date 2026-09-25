#ifndef _TW_WIDGETS_H
#define _TW_WIDGETS_H
#include <stdbool.h>
#include <stddef.h>

/*
 * The one list of widgets. Every widget the taskbar and the desktop can show is
 * described here once: what it is called, where it can go and which options it
 * takes. The taskbar draws a widget the same way in both places, and the
 * settings app builds its lists and dialogs from this, so a widget and its
 * options are never described in two places that could drift apart.
 */

struct tw_widget_option {
	const char *key, *title, *hint;
	const char *const *choices; // NULL for free text
};

enum tw_widget_flags {
	TW_WIDGET_TASKBAR = 1 << 0, // can sit in a section of the taskbar
	TW_WIDGET_DESKTOP = 1 << 1, // can sit on the desktop
	TW_WIDGET_STATUS = 1 << 2,  // belongs to the notification area of the taskbar
	TW_WIDGET_UNLISTED = 1 << 3, // made another way, not picked from the list (scripts)
};

/*
 * A way a widget can look on the desktop. It takes columns x rows cells of the
 * grid the desktop icons sit in; 0 columns makes it as wide as it needs.
 */
struct tw_widget_style {
	const char *name, *title;
	int columns, rows;
};

struct tw_widget_info {
	const char *type, *title, *description;
	unsigned flags;
	const struct tw_widget_option *options; // ends with an entry without a key
	/* Its looks on the desktop, the default first; ends with an entry without a name. */
	const struct tw_widget_style *styles;
};

extern const struct tw_widget_info tw_widgets[];
extern const size_t tw_widget_count;

/* The mouse actions every widget takes. */
extern const struct tw_widget_option tw_widget_events[];
/* Where a widget on the desktop sits, in its block of "desktop_widgets". */
extern const struct tw_widget_option tw_widget_desktop_options[];

/* The desktop style of that name, or the default one when there is none. */
const struct tw_widget_style *tw_widget_style_find(const struct tw_widget_info *info,
	const char *name);

/* The widget of that type, or of the type in "type:name" (custom:weather). */
const struct tw_widget_info *tw_widget_find(const char *type_or_name);

#endif
