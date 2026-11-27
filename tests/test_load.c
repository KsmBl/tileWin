/*
 * tw_load: the shares of the processor and of the busiest whole disk between
 * two looks at a /proc of the test's own.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tw_load.h"

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } \
	else { printf("ok   %s\n", what); } } while (0)

static void write_file(const char *dir, const char *name, const char *text) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
}

int main(void) {
	CHECK(tw_load_whole_disk("sda") && tw_load_whole_disk("nvme0n1") &&
		tw_load_whole_disk("mmcblk0") && tw_load_whole_disk("vdb"), "whole disks");
	CHECK(!tw_load_whole_disk("sda1") && !tw_load_whole_disk("nvme0n1p2") &&
		!tw_load_whole_disk("mmcblk0p1") && !tw_load_whole_disk("mmcblk0boot0") &&
		!tw_load_whole_disk("loop0") && !tw_load_whole_disk("dm-0") &&
		!tw_load_whole_disk("zram0"), "partitions, loops, mappers and zram are not");

	char dir[] = "/tmp/tw-load-XXXXXX";
	if (!mkdtemp(dir)) {
		return 1;
	}
	// user nice system idle iowait irq softirq steal
	write_file(dir, "stat", "cpu 1000 0 500 8000 500 0 0 0 0 0\ncpu0 1 2 3\n");
	write_file(dir, "diskstats",
		"   8       0 sda 10 0 0 0 0 0 0 0 0 1000 0 0 0 0 0 0 0\n"
		"   8       1 sda1 10 0 0 0 0 0 0 0 0 99999 0 0 0 0 0 0 0\n"
		" 259       0 nvme0n1 10 0 0 0 0 0 0 0 0 5000 0 0 0 0 0 0 0\n"
		"   7       0 loop0 10 0 0 0 0 0 0 0 0 7000 0 0 0 0 0 0 0\n");
	struct tw_load a, b;
	CHECK(tw_load_read(&a, dir, 100.0), "a first look");
	CHECK(a.disks == 2, "two whole disks of four lines");
	// 10 seconds later: 1000 of 10000 ticks busy (10 %), sda busy 500 ms (5 %),
	// nvme0n1 3000 ms (30 %), the partition and the loop device much more
	write_file(dir, "stat", "cpu 1600 0 900 16500 1000 0 0 0 0 0\n");
	write_file(dir, "diskstats",
		"   8       0 sda 10 0 0 0 0 0 0 0 0 1500 0 0 0 0 0 0 0\n"
		"   8       1 sda1 10 0 0 0 0 0 0 0 0 999999 0 0 0 0 0 0 0\n"
		" 259       0 nvme0n1 10 0 0 0 0 0 0 0 0 8000 0 0 0 0 0 0 0\n"
		"   7       0 loop0 10 0 0 0 0 0 0 0 0 9999999 0 0 0 0 0 0 0\n");
	tw_load_read(&b, dir, 110.0);
	int cpu, disk;
	tw_load_between(&a, &b, &cpu, &disk);
	CHECK(cpu == 10, "the processor: 10 % busy");
	CHECK(disk == 30, "the busiest whole disk: 30 %");
	tw_load_between(&b, &b, &cpu, &disk);
	CHECK(cpu == -1 && disk == -1, "no time between: nothing to tell");
	write_file(dir, "diskstats", "   8       0 sda 10 0 0 0 0 0 0 0 0 99000 0 0 0 0 0 0 0\n");
	struct tw_load c;
	tw_load_read(&c, dir, 111.0);
	tw_load_between(&b, &c, &cpu, &disk);
	CHECK(disk == 100, "never more than all the time");
	struct tw_load none;
	CHECK(!tw_load_read(&none, "/nonexistent", 0) && !none.valid, "no /proc: no look");

	char path[512];
	snprintf(path, sizeof(path), "rm -rf %s", dir);
	if (system(path) != 0) {
		failures++;
	}
	return failures ? 1 : 0;
}
