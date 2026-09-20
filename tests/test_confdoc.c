/*
 * The editor the settings app writes common.conf and taskbar.conf with. It has
 * to change the statement it was asked about and leave everything else - the
 * comments above all - exactly as the user wrote it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include "confdoc.h"

static int failures;

static void fail(const char *what, const char *detail) {
	fprintf(stderr, "FAIL: %s\n%s\n", what, detail ? detail : "");
	failures++;
}

static void check(bool ok, const char *what) {
	if (!ok) {
		fail(what, NULL);
	}
}

static void check_str(const char *got, const char *want, const char *what) {
	if (g_strcmp0(got, want) != 0) {
		char *detail = g_strdup_printf("got:\n<<%s>>\nwanted:\n<<%s>>",
			got ? got : "(null)", want ? want : "(null)");
		fail(what, detail);
		g_free(detail);
	}
}

static struct confdoc *doc_with(const char *text) {
	struct confdoc *doc = confdoc_open("test-document.conf");
	confdoc_set_text(doc, text);
	if (doc->error) {
		fail("the document should parse", doc->error);
	}
	return doc;
}

static void test_reading(void) {
	struct confdoc *doc = doc_with(
		"font Noto Sans 10\n"
		"layout {\n"
		"\twindow {\n"
		"\t\tposition top\n"
		"\t}\n"
		"}\n");
	struct cstmt *font = confdoc_child(doc->root, "font", NULL);
	check(cstmt_argc(font) == 3, "font keeps all three words");
	check_str(cstmt_arg(font, 1), "Sans", "a single argument");
	char *joined = cstmt_join(font, 0);
	check_str(joined, "Noto Sans 10", "the arguments joined again");
	g_free(joined);
	char *raw = cstmt_raw_args(doc, font);
	check_str(raw, "Noto Sans 10", "the source text after the name");
	g_free(raw);

	struct cstmt *layout = confdoc_child(doc->root, "layout", NULL);
	struct cstmt *window = confdoc_child(layout, "window", NULL);
	check_str(cstmt_arg(confdoc_child(window, "position", NULL), 0), "top",
		"a value inside two blocks");
	check(confdoc_child(doc->root, "position", NULL) == NULL,
		"a lookup does not reach into blocks");
	check_str(doc->indent, "\t", "the file's own indentation is picked up");
	confdoc_free(doc);
}

/* The whole point of the editor: a change must not disturb the rest. */
static void test_comments_survive(void) {
	struct confdoc *doc = doc_with(
		"# the taskbar font\n"
		"font Noto Sans 10   # bigger than the theme's\n"
		"\n"
		"### Layout\n"
		"tooltip_delay 600\n");
	confdoc_set(doc, doc->root, "tooltip_delay", NULL, "250");
	check_str(doc->text->str,
		"# the taskbar font\n"
		"font Noto Sans 10   # bigger than the theme's\n"
		"\n"
		"### Layout\n"
		"tooltip_delay 250\n",
		"only the value changed");
	confdoc_free(doc);
}

static void test_set_add_and_remove(void) {
	struct confdoc *doc = doc_with("font Noto Sans 10\n");
	confdoc_set(doc, doc->root, "terminal", NULL, "foot");
	check_str(doc->text->str, "font Noto Sans 10\nterminal foot\n",
		"a statement that is not there yet is added at the end");

	confdoc_set(doc, doc->root, "font", NULL, NULL);
	check_str(doc->text->str, "terminal foot\n", "no value removes the statement");

	confdoc_set(doc, doc->root, "terminal", NULL, NULL);
	check_str(doc->text->str, "", "removing the last statement empties the file");
	confdoc_free(doc);
}

/* Several statements of the same name collapse onto the first one. */
static void test_set_collapses_duplicates(void) {
	struct confdoc *doc = doc_with(
		"height 44\n"
		"other thing\n"
		"height 32\n"
		"height 28\n");
	confdoc_set(doc, doc->root, "height", NULL, "40");
	check_str(doc->text->str, "height 40\nother thing\n",
		"the first one is changed and the rest are gone");
	confdoc_free(doc);
}

static void test_set_list(void) {
	struct confdoc *doc = doc_with("menu {\n\titem one\n}\n");
	struct cstmt *menu = confdoc_child(doc->root, "menu", NULL);
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(lines, g_strdup("\"Show the desktop\" showdesktop"));
	g_ptr_array_add(lines, g_strdup("Cascade \"arrange cascade\""));
	confdoc_set_list(doc, menu, "item", NULL, lines);
	g_ptr_array_unref(lines);
	check_str(doc->text->str,
		"menu {\n"
		"\titem \"Show the desktop\" showdesktop\n"
		"\titem Cascade \"arrange cascade\"\n"
		"}\n",
		"one line per entry, indented like the file");
	confdoc_free(doc);
}

static void test_blocks(void) {
	struct confdoc *doc = doc_with("font Noto Sans 10\n");
	check(confdoc_block(doc, "widget", "quicklaunch", false) == NULL,
		"a block that is not there is not invented");
	struct cstmt *block = confdoc_block(doc, "widget", "quicklaunch", true);
	check(block != NULL, "asking for it creates it");
	confdoc_append(doc, block, "item firefox.desktop");
	check_str(doc->text->str,
		"font Noto Sans 10\n"
		"\n"
		"widget quicklaunch {\n"
		"\titem firefox.desktop\n"
		"}\n",
		"the new block holds the statement");
	check(confdoc_block(doc, "widget", "quicklaunch", false) == block ||
		confdoc_block(doc, "widget", "quicklaunch", false) != NULL,
		"the block is found again afterwards");
	confdoc_free(doc);
}

/* A file written with four spaces has to stay a file written with four spaces. */
static void test_indent_is_taken_from_the_file(void) {
	struct confdoc *doc = doc_with("menu {\n    item one\n}\n");
	check_str(doc->indent, "    ", "four spaces are picked up");
	struct cstmt *menu = confdoc_child(doc->root, "menu", NULL);
	confdoc_append(doc, menu, "item two");
	check_str(doc->text->str, "menu {\n    item one\n    item two\n}\n",
		"the added line uses the same indentation");
	confdoc_free(doc);
}

static void test_replace(void) {
	struct confdoc *doc = doc_with("# keep me\nwidget clock {\n\tformat %H:%M\n}\n");
	struct cstmt *widget = confdoc_child(doc->root, "widget", "clock");
	confdoc_replace(doc, widget, "widget clock {\n\tformat %H:%M:%S\n}");
	check_str(doc->text->str, "# keep me\nwidget clock {\n\tformat %H:%M:%S\n}\n",
		"a whole statement can be swapped out");
	confdoc_free(doc);
}

static void test_quoting(void) {
	char *q = conf_quote("plain");
	check_str(q, "plain", "a plain word is left alone");
	g_free(q);
	q = conf_quote("two words");
	check_str(q, "\"two words\"", "a space asks for quotes");
	g_free(q);
	q = conf_quote("#008080");
	check_str(q, "\"#008080\"", "a leading '#' would start a comment");
	g_free(q);
	q = conf_quote("say \"hi\"");
	check_str(q, "\"say \\\"hi\\\"\"", "quotes inside are escaped");
	g_free(q);
	q = conf_quote("one\ttwo");
	check_str(q, "\"one\\ttwo\"", "a tab is escaped");
	g_free(q);
	q = conf_quote("");
	check_str(q, "\"\"", "an empty word is written as empty quotes");
	g_free(q);

	q = conf_quote_command("exec firefox");
	check_str(q, "exec firefox", "a simple command stays bare");
	g_free(q);
	q = conf_quote_command("exec  firefox");
	check_str(q, "\"exec  firefox\"", "two spaces would not survive being read back");
	g_free(q);
	q = conf_quote_command("exec sh -c 'a; b'");
	check_str(q, "\"exec sh -c 'a; b'\"", "a ';' would end the statement");
	g_free(q);
	q = conf_quote_command("icon");
	check_str(q, "\"icon\"", "a word the menu syntax reserves is quoted");
	g_free(q);
}

/* Reading a file back has to give the same text that was written. */
static void test_save_and_reopen(void) {
	struct confdoc *doc = doc_with("# hello\nfont Noto Sans 10\n");
	confdoc_set(doc, doc->root, "terminal", NULL, "\"xfce4-terminal -x\"");
	char *error = NULL;
	if (!confdoc_save(doc, &error)) {
		fail("the document should save", error);
		g_free(error);
	}
	char *written = g_strdup(doc->text->str);
	confdoc_free(doc);

	doc = confdoc_open("test-document.conf");
	check_str(doc->text->str, written, "the file reads back exactly as written");
	check_str(cstmt_arg(confdoc_child(doc->root, "terminal", NULL), 0),
		"xfce4-terminal -x", "and the quoted value is one argument again");
	g_free(written);
	confdoc_free(doc);
}

static void test_broken_file_is_reported(void) {
	struct confdoc *doc = confdoc_open("test-document.conf");
	confdoc_set_text(doc, "menu {\n\titem one\n");
	check(doc->error != NULL, "a block that is never closed is reported");
	confdoc_free(doc);
}

int main(void) {
	// no test may touch the real config
	char *tmp = g_dir_make_tmp("tilewin-confdoc-XXXXXX", NULL);
	if (!tmp) {
		fprintf(stderr, "could not make a temporary directory\n");
		return 77;
	}
	g_setenv("XDG_CONFIG_HOME", tmp, TRUE);
	g_setenv("TILEWIN_DATADIR", tmp, TRUE);
	char *dir = g_build_filename(tmp, "tileWin", NULL);
	g_mkdir_with_parents(dir, 0700);

	test_reading();
	test_comments_survive();
	test_set_add_and_remove();
	test_set_collapses_duplicates();
	test_set_list();
	test_blocks();
	test_indent_is_taken_from_the_file();
	test_replace();
	test_quoting();
	test_save_and_reopen();
	test_broken_file_is_reported();

	char *file = g_build_filename(dir, "test-document.conf", NULL);
	g_unlink(file);
	g_rmdir(dir);
	g_rmdir(tmp);
	g_free(file);
	g_free(dir);
	g_free(tmp);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("confdoc: all checks passed\n");
	return 0;
}
