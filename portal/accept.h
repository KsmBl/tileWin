#ifndef _TILEWIN_PORTAL_ACCEPT_H
#define _TILEWIN_PORTAL_ACCEPT_H
#include <stdbool.h>
#include "html.h"

/*
 * What to do on a login page: which form to send with which button, which
 * link to follow, or that it wants something only the user can give (an
 * email, a room number, a voucher), so it is opened for them instead.
 */

enum portal_step {
	PORTAL_NOTHING,     // nothing on it looks like a way in
	PORTAL_SUBMIT,      // send a form
	PORTAL_LINK,        // follow a link
	PORTAL_REDIRECT,    // the page sends itself on (meta refresh, script)
	PORTAL_NEEDS_INPUT, // its way in wants something typed in
};

struct portal_action {
	enum portal_step step;
	char *url;          // where to: the form's action, the link, the redirect
	bool post, multipart;
	int npairs;         // what the form sends, in order
	char **names, **values;
	char *what;         // for the log: "the button 'I agree'", "the link 'Continue'"
	int score;
};

/* The way in of a page that was fetched from base_url. */
struct portal_action portal_decide(const struct html_page *page, const char *base_url);
void portal_action_clear(struct portal_action *action);

/*
 * How much the text of a button or link says "let me in" (accept, agree,
 * connect, continue ... in English, German, French, Spanish, Italian, Dutch,
 * Portuguese, Polish, Turkish and Swedish); below zero for the ways out
 * (decline, cancel, back, more information ...).
 */
int portal_text_score(const char *text);

/* Whether a checkbox is one for newsletters and offers, which stays unticked. */
bool portal_is_marketing(const char *text);

/* An absolute URL for href found on the page at base. Newly allocated, NULL if none. */
char *portal_resolve(const char *base, const char *href);

#endif
