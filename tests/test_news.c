/*
 * NEWS.md as the About page and the taskbar read it (tw_news): the release of
 * a version as the build writes it, the changes of one release and no other,
 * their bold names, and the NEWS.md of the repository has the release of
 * meson.build, so a release is never made without its notes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_news.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

int main(int argc, char **argv) {
	char release[32];
	tw_news_release("1.6-61f8de8 on main (Nov 16 2026)", release, sizeof(release));
	CHECK(strcmp(release, "1.6") == 0, "the release of a build: %s", release);
	tw_news_release("1.7", release, sizeof(release));
	CHECK(strcmp(release, "1.7") == 0, "a bare version: %s", release);

	char path[] = "/tmp/tw-news-XXXXXX";
	int fd = mkstemp(path);
	FILE *f = fdopen(fd, "w");
	fputs("# What's new\n\n## 1.7\n\n- **First:** one\n- **Second thing.** two\n- plain\n\n"
		"## 1.6\n\n- **Old** one\n", f);
	fclose(f);
	char **items = tw_news_items(path, "1.7");
	int n = 0;
	for (char **i = items; i && *i; i++) {
		n++;
	}
	CHECK(n == 3, "three changes in 1.7, not %d", n);
	char title[64];
	tw_news_title(items[0], title, sizeof(title));
	CHECK(strcmp(title, "First") == 0, "the name without its colon: %s", title);
	tw_news_title(items[1], title, sizeof(title));
	CHECK(strcmp(title, "Second thing") == 0, "the name without its full stop: %s", title);
	tw_news_title(items[2], title, sizeof(title));
	CHECK(title[0] == '\0', "no name without bold");
	tw_news_free(items);
	items = tw_news_items(path, "1.6");
	CHECK(items && items[0] && !items[1], "1.6 has its one change");
	tw_news_free(items);
	CHECK(tw_news_items(path, "1.5") == NULL, "no section for 1.5");
	remove(path);

	// the repository's NEWS.md has the release meson.build names
	if (argc > 2) {
		items = tw_news_items(argv[1], argv[2]);
		CHECK(items && items[0], "NEWS.md has no changes for %s, the version of meson.build",
			argv[2]);
		tw_news_free(items);
	}

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("news: all checks passed\n");
	return 0;
}
