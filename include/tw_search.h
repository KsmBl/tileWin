#ifndef _TW_SEARCH_H
#define _TW_SEARCH_H
#include <stdbool.h>
#include <stddef.h>

/*
 * What the search of the start menu and the launcher finds besides apps: the
 * pages of the settings app, and sums. The settings app takes the titles and
 * keywords of its pages from here too, so both search the same words.
 */
struct tw_settings_page {
	const char *name, *title, *keywords, *icon;
};

extern const struct tw_settings_page tw_settings_pages[];
extern const size_t tw_settings_page_count;

/*
 * 0 for no match, higher is better. Every word of the query has to start a word
 * of the title or the keywords, so "dark mode" finds the Theme page; a query of
 * one letter finds nothing, it would find everything.
 */
int tw_settings_page_score(const struct tw_settings_page *page, const char *query);

/*
 * The value of a sum: + - * / ^, % as percent or remainder, brackets, and a
 * comma or a point in numbers. False for anything else, for a sum that has no
 * value (1/0), and for a lone number unless it begins with =.
 */
bool tw_calc(const char *text, double *result);
/* The value as people write it: no trailing zeros, no rounding noise. */
void tw_calc_format(double value, char *buf, size_t size);

/* The text in single quotes for a shell. Newly allocated. */
char *tw_shell_quote(const char *text);

/* "21:30" as minutes after midnight. */
bool tw_time_parse(const char *text, int *minutes);
/* Is it between the two now, also across midnight? The end itself is not. */
bool tw_time_between(int now, int from, int to);

#endif
