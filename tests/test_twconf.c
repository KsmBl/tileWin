/*
 * The parser behind theme.conf and taskbar.conf. Everything the taskbar and
 * the settings app read goes through it, so the syntax it accepts is part of
 * the config format and is pinned down here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "twconf.h"

static int failures;

static void fail(const char *what, const char *detail) {
	fprintf(stderr, "FAIL: %s (%s)\n", what, detail ? detail : "");
	failures++;
}

static void check(bool ok, const char *what) {
	if (!ok) {
		fail(what, NULL);
	}
}

static void check_str(const char *got, const char *want, const char *what) {
	if (!got || !want || strcmp(got, want) != 0) {
		char detail[256];
		snprintf(detail, sizeof(detail), "got \"%s\", wanted \"%s\"",
			got ? got : "(null)", want ? want : "(null)");
		fail(what, detail);
	}
}

static void check_int(int got, int want, const char *what) {
	if (got != want) {
		char detail[64];
		snprintf(detail, sizeof(detail), "got %d, wanted %d", got, want);
		fail(what, detail);
	}
}

static struct twconf_node *parse(const char *text) {
	char *error = NULL;
	struct twconf_node *root = twconf_parse_string(text, "test", &error);
	if (!root) {
		fail("parse", error);
	}
	free(error);
	return root;
}

static void test_words_and_blocks(void) {
	struct twconf_node *root = parse(
		"font Noto Sans 10\n"
		"layout {\n"
		"    window {\n"
		"        position top\n"
		"        height 44\n"
		"    }\n"
		"}\n");
	if (!root) {
		return;
	}
	check_int(twconf_count(root), 2, "two statements at the top level");
	struct twconf_node *font = twconf_child(root, "font");
	check_int(font ? font->argc : -1, 3, "font keeps all three words");
	char *joined = twconf_join(font, 0);
	check_str(joined, "Noto Sans 10", "join puts the words back together");
	free(joined);
	check(font && !font->children, "a statement without a block has no children");

	struct twconf_node *layout = twconf_child(root, "layout");
	struct twconf_node *window = layout ? twconf_child(layout, "window") : NULL;
	check_str(twconf_value(window, "position"), "top", "nested value");
	check_str(twconf_value(window, "height"), "44", "second nested value");
	check(twconf_child(root, "position") == NULL, "lookups do not cross into blocks");
	check(twconf_value(root, "nothing") == NULL, "a missing key reads as nothing");
	check(twconf_arg(font, 9) == NULL, "an argument past the end reads as nothing");
	twconf_free(root);
}

static void test_quotes(void) {
	struct twconf_node *root = parse(
		"item \"Show the desktop\" showdesktop\n"
		"format \"%H:%M\\nlater\"\n"
		"single 'as written \\n'\n");
	if (!root) {
		return;
	}
	struct twconf_node *item = twconf_child(root, "item");
	check_str(twconf_arg(item, 0), "Show the desktop", "a quoted argument keeps its spaces");
	check_str(twconf_arg(item, 1), "showdesktop", "the word after it is its own argument");
	check_str(twconf_value(root, "format"), "%H:%M\nlater", "\\n in double quotes is a newline");
	check_str(twconf_value(root, "single"), "as written \\n",
		"single quotes take the text as it stands");
	twconf_free(root);
}

/* '#' starts a comment, but colors like #008080 have to survive. */
static void test_comments(void) {
	struct twconf_node *root = parse(
		"# a whole line\n"
		"   # an indented line\n"
		"fg #008080\n"
		"bg #123456 # and a trailing comment\n"
		"height 44\t# after a tab\n");
	if (!root) {
		return;
	}
	check_int(twconf_count(root), 3, "the comment lines are gone");
	check_str(twconf_value(root, "fg"), "#008080", "a color is not a comment");
	check_str(twconf_value(root, "bg"), "#123456", "a trailing comment is cut off");
	check_int(twconf_child(root, "bg")->argc, 1, "and leaves no extra arguments");
	check_str(twconf_value(root, "height"), "44", "a comment after a tab is cut off too");
	twconf_free(root);
}

static void test_separators(void) {
	struct twconf_node *root = parse(
		"menu { item one; item two }\n"
		"long one \\\n"
		"     two\n"
		"block\n"
		"{\n"
		"    inside yes\n"
		"}\n");
	if (!root) {
		return;
	}
	struct twconf_node *menu = twconf_child(root, "menu");
	check_int(menu && menu->children ? menu->children->length : -1, 2,
		"';' ends a statement");
	char *joined = twconf_join(twconf_child(root, "long"), 0);
	check_str(joined, "one two", "a line ending in '\\' carries on");
	free(joined);
	struct twconf_node *block = twconf_child(root, "block");
	check_str(block ? twconf_value(block, "inside") : NULL, "yes",
		"the opening brace may sit on the next line");
	twconf_free(root);
}

static void test_variables(void) {
	struct twconf_node *root = parse(
		"set $term xfce4-terminal\n"
		"set $termtab $term --tab\n"
		"item Terminal exec $term\n"
		"other exec $termtab\n");
	if (!root) {
		return;
	}
	check_int(twconf_count(root), 2, "set statements are not kept as statements");
	char *joined = twconf_join(twconf_child(root, "item"), 1);
	check_str(joined, "exec xfce4-terminal", "a variable is put in place");
	free(joined);
	joined = twconf_join(twconf_child(root, "other"), 0);
	check_str(joined, "exec xfce4-terminal --tab", "a variable may use another one");
	free(joined);
	twconf_free(root);
}

/* The longer name has to win, or $termtab would come out as "<$term>tab". */
static void test_variable_prefixes(void) {
	struct twconf_node *root = parse(
		"set $a one\n"
		"set $ab two\n"
		"value $ab\n");
	if (!root) {
		return;
	}
	check_str(twconf_value(root, "value"), "two", "the longest matching name wins");
	twconf_free(root);
}

static void test_bools(void) {
	check(twconf_parse_bool("yes", false), "yes is true");
	check(twconf_parse_bool("true", false), "true is true");
	check(twconf_parse_bool("on", false), "on is true");
	check(twconf_parse_bool("1", false), "1 is true");
	check(!twconf_parse_bool("no", true), "no is false");
	check(!twconf_parse_bool("false", true), "false is false");
	check(!twconf_parse_bool("off", true), "off is false");
	check(!twconf_parse_bool("0", true), "0 is false");
	check(twconf_parse_bool(NULL, true), "nothing falls back");
	check(!twconf_parse_bool(NULL, false), "nothing falls back the other way too");
}

static void check_error(const char *text, const char *what) {
	char *error = NULL;
	struct twconf_node *root = twconf_parse_string(text, "test", &error);
	if (root || !error) {
		fail(what, "the parser accepted it");
	}
	twconf_free(root);
	free(error);
}

static void test_errors(void) {
	check_error("block {\n  key value\n", "a block that is never closed is an error");
	check_error("key value\n}\n", "a stray '}' is an error");
	check_error("key \"never ends\n", "a quote that is never closed is an error");
}

int main(void) {
	test_words_and_blocks();
	test_quotes();
	test_comments();
	test_separators();
	test_variables();
	test_variable_prefixes();
	test_bools();
	test_errors();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("twconf: all checks passed\n");
	return 0;
}
