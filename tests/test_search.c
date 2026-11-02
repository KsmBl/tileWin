/*
 * What the search finds besides apps: the settings pages by the words people
 * look for, and the value of sums, with a comma or a point.
 */
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_search.h"

static int failures;

static void check_sum(const char *text, double want) {
	double got;
	if (!tw_calc(text, &got) || fabs(got - want) > 1e-9) {
		fprintf(stderr, "FAIL: %s is not %g\n", text, want);
		failures++;
	}
}

static void check_no_sum(const char *text) {
	double got;
	if (tw_calc(text, &got)) {
		fprintf(stderr, "FAIL: %s was taken for a sum (%g)\n", text, got);
		failures++;
	}
}

static const char *best_page(const char *query) {
	const char *best = NULL;
	int top = 0;
	for (size_t i = 0; i < tw_settings_page_count; i++) {
		int score = tw_settings_page_score(&tw_settings_pages[i], query);
		if (score > top) {
			top = score;
			best = tw_settings_pages[i].name;
		}
	}
	return best;
}

static void check_page(const char *query, const char *want) {
	const char *got = best_page(query);
	if ((want == NULL) != (got == NULL) || (want && strcmp(got, want) != 0)) {
		fprintf(stderr, "FAIL: \"%s\" finds %s, not %s\n", query, got ? got : "nothing",
			want ? want : "nothing");
		failures++;
	}
}

static void check_format(double value, const char *want) {
	char buf[64];
	tw_calc_format(value, buf, sizeof(buf));
	if (strcmp(buf, want) != 0) {
		fprintf(stderr, "FAIL: %g reads %s, not %s\n", value, buf, want);
		failures++;
	}
}

int main(void) {
	setlocale(LC_ALL, "");
	check_sum("1+2*3", 7);
	check_sum("(1+2)*3", 9);
	check_sum("2^3^2", 512);
	check_sum("10/4", 2.5);
	check_sum("1,5+1.5", 3);
	check_sum("-3*-2", 6);
	check_sum("200*10%", 20);
	check_sum("7 % 3", 1);
	check_sum("= 42", 42);
	check_sum("  3 - 1 ", 2);
	check_no_sum("42");
	check_no_sum("-42");
	check_no_sum("1/0");
	check_no_sum("7 % 0");
	check_no_sum("(1+2");
	check_no_sum("1+");
	check_no_sum("firefox");
	check_no_sum("a+b");
	check_no_sum("");
	check_no_sum("10^1000");

	check_format(0.1 + 0.2, "0.3");
	check_format(42, "42");
	check_format(-0.0, "0");

	check_page("dark mode", "theme");
	check_page("wifi", "network");
	check_page("wallp", "wallpaper");
	check_page("Sound", "sound");
	check_page("resolution", "screen");
	check_page("x", NULL);
	check_page("no such setting", NULL);

	for (size_t i = 0; i < tw_settings_page_count; i++) {
		const struct tw_settings_page *page = &tw_settings_pages[i];
		if (tw_settings_page_score(page, page->title) != 3) {
			fprintf(stderr, "FAIL: the page %s is not found by its title\n", page->name);
			failures++;
		}
	}

	char *quoted = tw_shell_quote("it's");
	if (strcmp(quoted, "'it'\\''s'") != 0) {
		fprintf(stderr, "FAIL: it's quoted as %s\n", quoted);
		failures++;
	}
	free(quoted);
	return failures ? 1 : 0;
}
