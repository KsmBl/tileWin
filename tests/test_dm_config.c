/*
 * The config of the display manager: what is wrong in it never counts and is
 * reported, and the session list only offers what can be started.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "sessions.h"

static int failures;

static void expect(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
}

static int reports;
static void count_report(const char *problem, void *data) {
	reports++;
}

int main(void) {
	char dir[] = "/tmp/tilewin-dm-test-XXXXXX";
	if (!mkdtemp(dir)) {
		return 1;
	}
	char conf[128], sessions[128], path[256];
	snprintf(conf, sizeof(conf), "%s/conf", dir);
	snprintf(sessions, sizeof(sessions), "%s/sessions", dir);

	// every option has a default that passes its own check
	FILE *f = fopen(conf, "w");
	for (size_t i = 0; i < dm_option_count; i++) {
		fprintf(f, "%s \"%s\"\n", dm_options[i].key, dm_options[i].fallback);
		expect(dm_options[i].label && dm_options[i].group, dm_options[i].key);
	}
	fclose(f);
	reports = 0;
	expect(dm_config_check(conf, count_report, NULL) == 0, "the defaults do not pass");
	expect(dm_option_count >= 40, "fewer settings than promised");

	// what makes sense counts
	write_file(conf, "# a comment\n\n  theme   win95  \nvt 3\nshow_users no\n"
		"greeting \"  two words \"\nenv FOO=bar baz\ncard_position left\n");
	struct dm_config *c = dm_config_load(conf);
	expect(strcmp(dm_config_str(c, "theme"), "win95") == 0, "theme");
	expect(dm_config_int(c, "vt") == 3, "vt");
	expect(!dm_config_bool(c, "show_users"), "show_users");
	expect(strcmp(dm_config_str(c, "greeting"), "  two words ") == 0, "quotes");
	expect(strcmp(dm_config_str(c, "card_position"), "left") == 0, "card_position");
	expect(c->env_count == 1 && strcmp(c->env[0], "FOO=bar baz") == 0, "env");
	expect(dm_config_bool(c, "show_power"), "a default stays");
	dm_config_free(c);

	// what does not make sense is left out and reported, and cannot lock anyone out
	write_file(conf, "vt 99\nshow_users maybe\ncard_position sideways\nno_such_thing 1\n"
		"dim -5\nenv 1BAD=x\nenv NOEQUALS\n");
	c = dm_config_load(conf);
	expect(dm_config_int(c, "vt") == 1, "a terminal out of range");
	expect(dm_config_bool(c, "show_users"), "a word that is no yes or no");
	expect(strcmp(dm_config_str(c, "card_position"), "center") == 0, "a choice that is none");
	expect(dm_config_int(c, "dim") == 0, "a negative number");
	expect(c->env_count == 0, "a bad env line");
	dm_config_free(c);
	reports = 0;
	expect(dm_config_check(conf, count_report, NULL) == 7 && reports == 7, "not all were reported");

	// no file: all defaults
	c = dm_config_load("/no/such/file");
	expect(dm_config_int(c, "vt") == 1, "without a file");
	dm_config_free(c);

	expect(dm_word_in_list("sam  alex,kim", "alex"), "word in a list");
	expect(!dm_word_in_list("samuel", "sam"), "a part of a word");
	expect(!dm_word_in_list("", ""), "an empty list");

	// sessions
	mkdir(sessions, 0755);
	setenv("TILEWIN_DM_SESSIONS", sessions, 1);
	snprintf(path, sizeof(path), "%s/good.desktop", sessions);
	write_file(path, "[Desktop Entry]\nName=Good\nExec=/bin/sh -c true %U\nDesktopNames=Good;x\n");
	snprintf(path, sizeof(path), "%s/gone.desktop", sessions);
	write_file(path, "[Desktop Entry]\nName=Gone\nExec=/no/such/program\n");
	snprintf(path, sizeof(path), "%s/hidden.desktop", sessions);
	write_file(path, "[Desktop Entry]\nName=Hidden\nExec=/bin/sh\nHidden=true\n");
	snprintf(path, sizeof(path), "%s/other.txt", sessions);
	write_file(path, "[Desktop Entry]\nName=Other\nExec=/bin/sh\n");
	snprintf(path, sizeof(path), "%s/.dot.desktop", sessions);
	write_file(path, "[Desktop Entry]\nName=Dot\nExec=/bin/sh\n");
	struct dm_session_list *list = dm_sessions_read();
	expect(list->count == 1, "only what can start is a session");
	const struct dm_session *s = dm_session_find(list, "good");
	expect(s && strcmp(s->name, "Good") == 0, "the session by its file");
	expect(s && strcmp(s->desktop, "Good") == 0, "DesktopNames");
	char *command = s ? dm_session_command(s->exec) : strdup("");
	expect(strcmp(command, "/bin/sh -c true ") == 0, "field codes");
	free(command);
	expect(!dm_session_find(list, "../good"), "a path is no session");
	dm_sessions_free(list);

	expect(dm_session_id_ok("plasma-wayland.1"), "a usual id");
	expect(!dm_session_id_ok("../../bin/sh"), "a path as an id");
	expect(!dm_session_id_ok(".hidden"), "a dot first");
	expect(!dm_session_id_ok(""), "nothing");
	expect(!dm_session_id_ok("a b"), "a space");

	char cleanup[128];
	snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
	if (system(cleanup) != 0) {
		return 1;
	}
	return failures ? 1 : 0;
}
