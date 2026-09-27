/*
 * Screen savers are directories: what is found where, and what is made of it.
 *
 *  - the savers that come with tileWin are found, Blank first, each with its
 *    screenshot;
 *  - the Lava Lamp example, a directory of a saver of one's own built from the
 *    header alone, dropped in another folder, is found too, listed after
 *    them, has its setting read and draws;
 *  - a directory that is no saver (an empty one, one whose saver.so is no
 *    module) is left out without harm, and of two savers of one name the one
 *    found first is kept;
 *  - tilewin-screensaver --list shows the example and where it is, and
 *    --screenshot draws it into a picture.
 *
 * usage: test-saver-directories <tileWin's savers> <the example folder> <tilewin-screensaver>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "saver_util.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <savers> <example folder> <tilewin-screensaver>\n", argv[0]);
		return 2;
	}
	// a folder of one's own, with a directory that is empty, one with a saver.so that is no
	// module, and a second Lava Lamp of the same name that must not win over the first
	char mine[] = "/tmp/tw-saver-dirs-XXXXXX";
	if (!mkdtemp(mine)) {
		perror("mkdtemp");
		return 2;
	}
	char path[1024], command[4096];
	snprintf(path, sizeof(path), "%s/empty", mine);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/broken", mine);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/broken/saver.so", mine);
	write_file(path, "not a module\n");
	snprintf(path, sizeof(path), "%s/lavalamp", mine);
	mkdir(path, 0700);
	snprintf(command, sizeof(command), "cp %s/lavalamp/saver.so %s/lavalamp/", argv[2], mine);
	check(system(command) == 0, "the example could not be copied");
	snprintf(command, sizeof(command), "%s:%s:%s", argv[1], argv[2], mine);
	setenv("TILEWIN_SAVERS", command, 1);

	saver_load_all();
	printf("%d savers found\n", saver_count);
	check(saver_count >= 19, "the savers of tileWin are not all found");
	check(saver_count > 0 && strcmp(savers[0]->name, "blank") == 0, "Blank is not first");
	int shots = 0;
	for (int i = 0; i < saver_count; i++) {
		shots += saver_screenshot(savers[i]) != NULL;
	}
	check(shots == saver_count, "a saver has no screenshot");
	const struct saver *lamp = saver_find("lavalamp");
	check(lamp != NULL, "the Lava Lamp, dropped in as a directory, is not found");
	if (lamp) {
		check(savers[saver_count - 1] == lamp, "a saver of one's own is not listed after tileWin's");
		check(strncmp(saver_dir(lamp), argv[2], strlen(argv[2])) == 0,
			"the Lava Lamp found first is not the one kept");
		check(lamp->options && strcmp(lamp->options[0].key, "colour") == 0,
			"the setting of the Lava Lamp is not there");
		const char *settings[] = { "lavalamp_colour", "green" };
		struct saver_options o = { .speed = 1, .photo_seconds = 8, .settings = settings,
			.setting_count = 2 };
		check(saver_choice(&o, lamp, "colour") == 2, "the setting of the Lava Lamp is not read");
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 150);
		cairo_t *cr = cairo_create(surface);
		struct saver_run *run = saver_run_new(lamp, 200, 150, &o);
		for (int i = 0; i < 30; i++) {
			saver_run_draw(run, cr, 1 / 30.0);
		}
		saver_run_free(run);
		cairo_surface_flush(surface);
		long lit = 0;
		unsigned char *data = cairo_image_surface_get_data(surface);
		for (int i = 0; i < 200 * 150; i++) {
			lit += (((uint32_t *)data)[i] & 0xffffff) > 0x202020;
		}
		check(lit > 500, "the Lava Lamp draws next to nothing");
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
	}
	check(!saver_find("empty") && !saver_find("broken"), "a directory that is no saver is listed");

	// the program: --list, and --screenshot
	snprintf(command, sizeof(command), "TILEWIN_SAVERS='%s' %s --list > %s/list 2>/dev/null",
		getenv("TILEWIN_SAVERS"), argv[3], mine);
	check(system(command) == 0, "tilewin-screensaver --list failed");
	snprintf(path, sizeof(path), "%s/list", mine);
	FILE *f = fopen(path, "r");
	char text[16384] = { 0 };
	if (f) {
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		text[n] = 0;
		fclose(f);
	}
	check(strstr(text, "lavalamp") && strstr(text, argv[2]), "--list does not show the Lava Lamp");
	snprintf(command, sizeof(command), "%s --screenshot %s/lavalamp %s/shot.png --seconds 1",
		argv[3], argv[2], mine);
	check(system(command) == 0, "tilewin-screensaver --screenshot failed");
	snprintf(path, sizeof(path), "%s/shot.png", mine);
	struct stat st;
	check(stat(path, &st) == 0 && st.st_size > 1000, "--screenshot wrote no picture");
	snprintf(command, sizeof(command), "%s --screenshot %s/broken %s/no.png 2>/dev/null", argv[3],
		mine, mine);
	check(system(command) != 0, "--screenshot of a directory that is no saver did not fail");

	snprintf(command, sizeof(command), "rm -rf %s", mine);
	system(command);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
