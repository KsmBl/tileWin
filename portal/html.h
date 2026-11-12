#ifndef _TILEWIN_PORTAL_HTML_H
#define _TILEWIN_PORTAL_HTML_H
#include <stdbool.h>

/*
 * What tilewin-portal needs of a page: its forms with their fields and
 * buttons, its links, and where it sends the browser on by itself (a meta
 * refresh, or a script setting location). Made for the login pages of public
 * Wi-Fi, so it is forgiving: unclosed tags, attributes without quotes, upper
 * case and broken entities are all taken as a browser would take them.
 */

enum html_field_kind {
	HTML_HIDDEN,
	HTML_TEXT,     // text, email, tel, number, password, url, search, date, no type ...
	HTML_CHECKBOX,
	HTML_RADIO,
	HTML_SUBMIT,   // <input type=submit>, <button> of type submit or none
	HTML_IMAGE,    // <input type=image>
	HTML_BUTTON,   // <button type=button>, <input type=button|reset>: no submit
	HTML_SELECT,
	HTML_TEXTAREA,
	HTML_OTHER,    // file and the like
};

struct html_field {
	enum html_field_kind kind;
	char *type;        // the type attribute, lower case ("" when none)
	char *name, *value, *id;
	char *label;       // the text of a button, the label of a checkbox, a placeholder
	char *formaction, *formmethod;
	bool checked, required, invisible, disabled;
};

struct html_form {
	char *action, *method, *enctype, *id;
	char *text;        // all the text inside it
	struct html_field *fields;
	int nfields;
};

struct html_link {
	char *href, *text;
};

struct html_page {
	struct html_form *forms;
	int nforms;
	struct html_link *links;
	int nlinks;
	char *refresh;     // where a meta refresh or a script sends it, NULL if nowhere
	char *title;
	char *text;        // the text of the whole page
};

struct html_page *html_parse(const char *html);
void html_free(struct html_page *page);

/* The text lower case for ASCII, with runs of space made one. Newly allocated. */
char *html_fold(const char *text);

#endif
