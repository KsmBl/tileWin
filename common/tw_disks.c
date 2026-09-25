#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stringop.h"
#include "tw_disks.h"
#include "tw_paths.h"

bool tw_disk_is_whole(const char *block_dir, const char *name) {
	static const char *const skip[] = { "loop", "ram", "zram", "dm-", "md", "sr", "fd" };
	if (!name || !*name || name[0] == '.') {
		return false;
	}
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strncmp(name, skip[i], strlen(skip[i])) == 0) {
			return false;
		}
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/partition", block_dir, name);
	return access(path, F_OK) != 0; // a partition has this file, a disk has not
}

static int compare_disks(const void *a, const void *b) {
	return strcmp(((const struct tw_disk *)a)->name, ((const struct tw_disk *)b)->name);
}

size_t tw_disks_list(const char *block_dir, struct tw_disk **disks) {
	*disks = NULL;
	DIR *dir = opendir(block_dir);
	if (!dir) {
		return 0;
	}
	size_t count = 0, capacity = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!tw_disk_is_whole(block_dir, entry->d_name)) {
			continue;
		}
		if (count == capacity) {
			capacity = capacity ? capacity * 2 : 8;
			struct tw_disk *grown = realloc(*disks, capacity * sizeof(**disks));
			if (!grown) {
				break;
			}
			*disks = grown;
		}
		struct tw_disk *disk = &(*disks)[count++];
		disk->name = strdup(entry->d_name);
		char *path = format_str("%s/%s/device/model", block_dir, entry->d_name);
		disk->model = tw_read_first_line(path);
		free(path);
		if (disk->model && !*disk->model) {
			free(disk->model);
			disk->model = NULL;
		}
		path = format_str("%s/%s/size", block_dir, entry->d_name);
		char *size = tw_read_first_line(path);
		free(path);
		// the kernel counts in 512 byte sectors whatever the disk's own sector size
		disk->bytes = size ? strtoull(size, NULL, 10) * 512 : 0;
		free(size);
	}
	closedir(dir);
	if (count == 0) {
		free(*disks);
		*disks = NULL;
	} else {
		qsort(*disks, count, sizeof(**disks), compare_disks);
	}
	return count;
}

void tw_disks_free(struct tw_disk *disks, size_t count) {
	for (size_t i = 0; i < count; i++) {
		free(disks[i].name);
		free(disks[i].model);
	}
	free(disks);
}

char *tw_disk_label(const struct tw_disk *disk) {
	char size[32] = "";
	if (disk->bytes >= 1000ULL * 1000 * 1000 * 1000) {
		snprintf(size, sizeof(size), "%.1f TB", disk->bytes / 1e12);
	} else if (disk->bytes >= 1000ULL * 1000 * 1000) {
		snprintf(size, sizeof(size), "%.0f GB", disk->bytes / 1e9);
	} else if (disk->bytes > 0) {
		snprintf(size, sizeof(size), "%.0f MB", disk->bytes / 1e6);
	}
	char *label = strdup(disk->name);
	const char *parts[] = { disk->model, *size ? size : NULL };
	for (size_t i = 0; i < 2; i++) {
		if (parts[i]) {
			char *longer = format_str("%s · %s", label, parts[i]);
			free(label);
			label = longer;
		}
	}
	return label;
}
