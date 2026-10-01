#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "settings.h"
#include "tw_paths.h"

/*
 * All of tileWin's settings as one file: ~/.config/tileWin packed with tar,
 * so it can be kept somewhere safe, taken to another computer, or brought back
 * after trying things. A restore replaces the settings as a whole and keeps a
 * copy of the ones it replaces first, in ~/.local/state/tileWin/backups.
 *
 * An archive is only taken when everything in it lies inside tileWin/ and is a
 * plain file or folder: no absolute paths, no "..", no links.
 */

#define TOP "tileWin"

static bool run_tar(char **argv, char **out, char **error) {
	char *err = NULL;
	int status = 0;
	GError *gerror = NULL;
	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, out, &err, &status,
			&gerror)) {
		*error = g_strdup_printf("tar could not be started: %s", gerror->message);
		g_error_free(gerror);
		g_free(err);
		return false;
	}
	if (!g_spawn_check_wait_status(status, NULL)) {
		g_strstrip(err);
		*error = g_strdup_printf("tar failed: %s", err && *err ? err : "unknown error");
		g_free(err);
		return false;
	}
	g_free(err);
	return true;
}

bool tw_backup_save(const char *file, char **error) {
	char *dir = tw_config_dir();
	if (!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		*error = g_strdup("There are no settings to back up yet.");
		free(dir);
		return false;
	}
	char *parent = g_path_get_dirname(dir);
	char *base = g_path_get_basename(dir);
	// stored as tileWin/..., whatever the folder is called here
	char *transform = g_strdup_printf("s,^%s,%s,", base, TOP);
	char *argv[] = { "tar", "-czf", (char *)file, "-C", parent, "--transform", transform,
		base, NULL };
	bool ok = run_tar(argv, NULL, error);
	g_free(transform);
	g_free(base);
	g_free(parent);
	free(dir);
	return ok;
}

/* Whether every entry of the archive is a plain file or folder inside tileWin/. */
static bool archive_safe(const char *file, char **error) {
	char *listing = NULL;
	char *argv[] = { "tar", "-tvzf", (char *)file, NULL };
	if (!run_tar(argv, &listing, error)) {
		g_free(*error);
		*error = g_strdup("This is not a tileWin backup (it cannot be read as one).");
		return false;
	}
	bool ok = true, any = false;
	char **lines = g_strsplit(listing, "\n", -1);
	for (int i = 0; ok && lines[i]; i++) {
		if (!*lines[i]) {
			continue;
		}
		// "-rw-r--r-- user/group 123 2026-10-01 12:00 tileWin/common.conf": the
		// kind first, the path after five fields (names may hold spaces)
		char kind = lines[i][0];
		const char *path = lines[i];
		for (int field = 0; field < 5 && *path; field++) {
			while (*path == ' ') {
				path++;
			}
			while (*path && *path != ' ') {
				path++;
			}
		}
		while (*path == ' ') {
			path++;
		}
		bool inside = (strcmp(path, TOP) == 0 || strcmp(path, TOP "/") == 0 ||
			g_str_has_prefix(path, TOP "/"));
		bool dots = (strstr(path, "/../") || g_str_has_suffix(path, "/.."));
		if ((kind != '-' && kind != 'd') || !inside || dots) {
			ok = false;
		}
		any = any || inside;
	}
	g_strfreev(lines);
	g_free(listing);
	if (!ok || !any) {
		*error = g_strdup("This is not a tileWin backup: it holds files outside of its "
			"tileWin folder, or links. Nothing was changed.");
		return false;
	}
	return true;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
	if (ftw->level == 0) {
		return 0; // the folder itself stays, for whoever watches it
	}
	return remove(path);
}

/* Empties the folder, then moves what is in from into it. */
static bool replace_contents(const char *dir, const char *from, char **error) {
	if (nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
		*error = g_strdup_printf("The old settings in %s could not all be removed.", dir);
		return false;
	}
	GDir *d = g_dir_open(from, 0, NULL);
	if (!d) {
		*error = g_strdup("The backup holds no settings.");
		return false;
	}
	bool ok = true;
	const char *name;
	while ((name = g_dir_read_name(d))) {
		char *src = g_build_filename(from, name, NULL);
		char *dst = g_build_filename(dir, name, NULL);
		if (rename(src, dst) != 0) {
			ok = false;
		}
		g_free(src);
		g_free(dst);
	}
	g_dir_close(d);
	if (!ok) {
		*error = g_strdup_printf("Not all of the backup could be put into %s.", dir);
	}
	return ok;
}

char *tw_backup_auto_dir(void) {
	char *state = tw_state_dir();
	char *dir = state ? g_build_filename(state, "backups", NULL) : NULL;
	free(state);
	return dir;
}

bool tw_backup_restore(const char *file, char **saved_as, char **error) {
	if (saved_as) {
		*saved_as = NULL;
	}
	if (!archive_safe(file, error)) {
		return false;
	}
	char *dir = tw_config_dir();
	char *autodir = tw_backup_auto_dir();
	if (!dir || !autodir) {
		*error = g_strdup("tileWin's folders are not known.");
		free(dir);
		g_free(autodir);
		return false;
	}
	tw_mkdir_p(dir);
	tw_mkdir_p(autodir);

	// the settings there are now, to go back to
	GDateTime *now = g_date_time_new_now_local();
	// to the microsecond: two restores in one second must not share a copy
	char *stamp = g_date_time_format(now, "%Y-%m-%d-%H%M%S-%f");
	g_date_time_unref(now);
	char *copy = g_strdup_printf("%s/before-restore-%s.tar.gz", autodir, stamp);
	g_free(stamp);
	bool ok = tw_backup_save(copy, error);

	char *tmp = NULL;
	if (ok) {
		char *template = g_build_filename(autodir, "restore-XXXXXX", NULL);
		tmp = g_mkdtemp(template) ? template : NULL;
		if (!tmp) {
			g_free(template);
			*error = g_strdup("No room to unpack the backup.");
			ok = false;
		}
	}
	if (ok) {
		char *argv[] = { "tar", "-xzf", (char *)file, "-C", tmp, "--no-same-owner",
			"--no-same-permissions", NULL };
		ok = run_tar(argv, NULL, error);
	}
	if (ok) {
		char *from = g_build_filename(tmp, TOP, NULL);
		ok = replace_contents(dir, from, error);
		g_free(from);
	}
	if (tmp) {
		char *argv[] = { "rm", "-rf", tmp, NULL };
		g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
			G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		g_free(tmp);
	}
	if (ok && saved_as) {
		*saved_as = copy;
	} else {
		g_free(copy);
	}
	free(dir);
	g_free(autodir);
	return ok;
}
