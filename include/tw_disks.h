#ifndef _TW_DISKS_H
#define _TW_DISKS_H
#include <stdbool.h>
#include <stddef.h>

/*
 * The disks of the machine as the kernel names them in /sys/class/block and
 * /proc/diskstats. Both the disk activity widget and its settings ask here,
 * so the lamp watches exactly the disks the settings app offers.
 */

#define TW_BLOCK_DIR "/sys/class/block"

struct tw_disk {
	char *name;  // e.g. nvme0n1
	char *model; // what the drive calls itself, NULL if it does not say
	unsigned long long bytes;
};

/*
 * Whether a block device is a whole disk worth watching: no partition, and
 * none of the loop, ram, zram, device mapper, RAID, optical or floppy devices.
 * block_dir is TW_BLOCK_DIR, or a copy of it for the tests.
 */
bool tw_disk_is_whole(const char *block_dir, const char *name);

/*
 * The whole disks under block_dir, sorted by name. Returns the number found
 * and stores a newly allocated array in *disks, to be freed with
 * tw_disks_free. Returns 0 and sets *disks to NULL when there are none.
 */
size_t tw_disks_list(const char *block_dir, struct tw_disk **disks);

void tw_disks_free(struct tw_disk *disks, size_t count);

/*
 * A line to pick a disk by, e.g. "nvme0n1 · Samsung SSD 980 · 1.0 TB".
 * Newly allocated.
 */
char *tw_disk_label(const struct tw_disk *disk);

#endif
