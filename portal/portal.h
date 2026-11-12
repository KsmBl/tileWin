#ifndef _TILEWIN_PORTAL_H
#define _TILEWIN_PORTAL_H
#include <stdbool.h>

enum portal_outcome {
	PORTAL_ONLINE,     // no login page: the internet is there
	PORTAL_ACCEPTED,   // there was one, and it is done
	PORTAL_NEEDS_YOU,  // it wants something typed in
	PORTAL_FAILED,     // it could not be done
	PORTAL_NO_NETWORK, // the check got no answer at all
};

/* Whether the internet answers now, as the check URL says. */
typedef bool (*portal_online_fn)(void *data);

void portal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Opens the page in a headless Chromium and presses its way in there, for
 * pages that are made by scripts. *page gets the address of a page that
 * wants something typed in. PORTAL_FAILED also when no Chromium is there.
 */
enum portal_outcome portal_chromium(const char *browser, const char *url,
	portal_online_fn online, void *data, char **page);

/* The first Chromium (or Chrome, Brave, Edge, Vivaldi) found, NULL if none. */
char *portal_find_chromium(const char *configured);

#endif
