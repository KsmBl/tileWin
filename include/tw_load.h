#ifndef _TW_LOAD_H
#define _TW_LOAD_H
#include <stdbool.h>

/*
 * How busy the computer is: the processor from /proc/stat and the disks from
 * /proc/diskstats (the time each whole disk spent doing anything). Two looks
 * some seconds apart give the shares in between, as "Start in the background
 * when idle" asks them.
 */
struct tw_load {
	bool valid;
	unsigned long long cpu_total, cpu_idle;
#define TW_LOAD_DISKS 16
	int disks;
	struct {
		char name[32];
		unsigned long long io_ms;
	} disk[TW_LOAD_DISKS];
	double at; // seconds, of the clock the caller gives
};

/* A look now, from proc ("/proc", or a test's), at the time `at` (seconds). */
bool tw_load_read(struct tw_load *load, const char *proc, double at);

/*
 * The processor's share busy (0 to 100) between two looks, and the busiest
 * disk's; -1 where a share cannot be told.
 */
void tw_load_between(const struct tw_load *before, const struct tw_load *after, int *cpu,
	int *disk);

/* Whether a name of /proc/diskstats is a whole disk (sda, nvme0n1, mmcblk0, vda). */
bool tw_load_whole_disk(const char *name);

#endif
