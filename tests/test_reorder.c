/*
 * Where a widget ends up when its row is dragged in the Taskbar page. Dropping
 * it onto a row puts it in front of that row, or behind it on its lower half,
 * so the index it is dropped at counts the rows as they were before the move.
 */
#include <stdio.h>
#include <string.h>
#include "reorder.h"

static int failures;

static GPtrArray *names(const char *words) {
	GPtrArray *array = g_ptr_array_new_with_free_func(g_free);
	char **split = g_strsplit(words, " ", -1);
	for (char **w = split; *w; w++) {
		if (**w) {
			g_ptr_array_add(array, g_strdup(*w));
		}
	}
	g_strfreev(split);
	return array;
}

static char *joined(GPtrArray *array) {
	GString *text = g_string_new(NULL);
	for (guint i = 0; i < array->len; i++) {
		g_string_append_printf(text, "%s%s", i ? " " : "", (char *)array->pdata[i]);
	}
	return g_string_free(text, FALSE);
}

static void check_list(GPtrArray *array, const char *want, const char *what) {
	char *got = joined(array);
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL: %s (got \"%s\", wanted \"%s\")\n", what, got, want);
		failures++;
	}
	g_free(got);
}

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

/* Moves within one section and checks the result and whether it moved. */
static void within(const char *start, guint index, guint before, const char *want,
		bool moved, const char *what) {
	GPtrArray *a = names(start);
	check(reorder_move(a, index, a, before) == moved, what);
	check_list(a, want, what);
	g_ptr_array_unref(a);
}

static void test_within(void) {
	within("a b c d", 0, 4, "b c d a", true, "first to the end");
	within("a b c d", 3, 0, "d a b c", true, "last to the front");
	within("a b c d", 0, 2, "b a c d", true, "down one: in front of the one after next");
	within("a b c d", 2, 1, "a c b d", true, "up one");
	within("a b c d", 1, 3, "a c b d", true, "down one from the middle");
	within("a b c d", 1, 1, "a b c d", false, "onto its own upper half");
	within("a b c d", 1, 2, "a b c d", false, "onto its own lower half");
	within("a b c d", 4, 0, "a b c d", false, "an index past the end");
	within("a b c d", 0, 5, "a b c d", false, "a place past the end");
	within("a", 0, 0, "a", false, "the only widget");
}

static void test_between(void) {
	GPtrArray *left = names("start search taskbar");
	GPtrArray *right = names("tray clock");
	check(reorder_move(left, 1, right, 1), "into another section");
	check_list(left, "start taskbar", "the section it left");
	check_list(right, "tray search clock", "the section it went to");

	check(reorder_move(right, 0, left, left->len), "to the end of another section");
	check_list(right, "search clock", "the section it left, again");
	check_list(left, "start taskbar tray", "the section it went to, at the end");

	GPtrArray *empty = names("");
	check(reorder_move(left, 0, empty, 0), "into an empty section");
	check_list(empty, "start", "the empty section has it");
	check_list(left, "taskbar tray", "and the old one has not");
	check(!reorder_move(empty, 0, right, 3), "a place past the end of the other section");
	check_list(empty, "start", "a refused move leaves the first alone");
	check_list(right, "search clock", "a refused move leaves the second alone");

	g_ptr_array_unref(left);
	g_ptr_array_unref(right);
	g_ptr_array_unref(empty);
}

int main(void) {
	test_within();
	test_between();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("reorder: all checks passed\n");
	return 0;
}
