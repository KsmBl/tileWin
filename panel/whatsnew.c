/*
 * After an update: once, when the taskbar first starts with a new release, a
 * notification says so with the names of what it brought (NEWS.md), and a
 * click or What's new opens the About page, which shows all of it. The release
 * seen last is kept in ~/.local/state/tileWin/release-seen; on a new install
 * there is none, and nothing is said.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panel.h"
#include "tilewin-version.h"
#include "tw_news.h"
#include "tw_paths.h"

void whatsnew_check(struct panel *panel) {
	char release[32];
	tw_news_release(SWAY_VERSION, release, sizeof(release));
	char *dir = tw_state_dir();
	if (!dir || !release[0]) {
		free(dir);
		return;
	}
	char *path = g_build_filename(dir, "release-seen", NULL);
	char *seen = NULL;
	bool known = g_file_get_contents(path, &seen, NULL, NULL);
	if (seen) {
		g_strstrip(seen);
	}
	if (!known || strcmp(seen, release) != 0) {
		tw_mkdir_p(dir);
		g_file_set_contents(path, release, -1, NULL);
	}
	char **items = known && strcmp(seen, release) != 0 ? tw_news_items(NULL, release) : NULL;
	if (items && items[0]) {
		// the names of the first few changes, a line each
		GString *body = g_string_new(NULL);
		int shown = 0, count = 0;
		for (char **i = items; *i; i++, count++) {
			char title[128];
			tw_news_title(*i, title, sizeof(title));
			if (title[0] && shown < 4) {
				g_string_append_printf(body, "%s• %s", shown ? "\n" : "", title);
				shown++;
			}
		}
		if (count > shown) {
			g_string_append_printf(body, "\nand %d more", count - shown);
		}
		char *summary = g_strdup_printf("tileWin was updated to %s", release);
		static const char *const labels[] = { "Open", "What's new", NULL };
		static const char *const commands[] = { "exec tilewin-settings --page about",
			"exec tilewin-settings --page about", NULL };
		notify_local_with_actions(panel, "tileWin", "system-software-update", summary, body->str,
			labels, commands);
		g_free(summary);
		g_string_free(body, TRUE);
	}
	tw_news_free(items);
	g_free(seen);
	g_free(path);
	free(dir);
}
