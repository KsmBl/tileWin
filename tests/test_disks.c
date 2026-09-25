/*
 * Which block devices count as disks. The disk activity widget watches these
 * and the settings app offers them, so a partition or a loop device showing up
 * here would be a lamp for something that is no disk at all. The tests build
 * a small copy of /sys/class/block in a temporary folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tw_disks.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void check_str(const char *got, const char *want, const char *what) {
	if (!got || !want || strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL: %s (got \"%s\", wanted \"%s\")\n", what,
			got ? got : "(null)", want ? want : "(null)");
		failures++;
	}
}

static char root[] = "/tmp/tw-disks-XXXXXX";

static void make_dir(const char *name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, name);
	mkdir(path, 0755);
}

static void make_file(const char *name, const char *content) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(content, f);
		fclose(f);
	}
}

static void build_tree(void) {
	make_dir("nvme0n1");
	make_dir("nvme0n1/device");
	make_file("nvme0n1/device/model", "PM981a NVMe Samsung 256GB               \n");
	make_file("nvme0n1/size", "500118192\n");
	make_dir("nvme0n1p1");
	make_file("nvme0n1p1/partition", "1\n");
	make_dir("sda");
	make_file("sda/size", "0\n"); // a card reader with no card in it
	make_dir("sda1");
	make_file("sda1/partition", "1\n");
	make_dir("loop0");
	make_dir("zram0");
	make_dir("dm-0");
	make_dir("sr0");
	make_dir("md127");
	make_dir("mmcblk0");
	make_dir("mmcblk0/device");
	make_file("mmcblk0/device/model", "\n"); // says nothing
	make_file("mmcblk0/size", "3907584\n");
}

static void test_whole(void) {
	check(tw_disk_is_whole(root, "nvme0n1"), "an NVMe disk is a disk");
	check(tw_disk_is_whole(root, "sda"), "a SATA disk is a disk");
	check(!tw_disk_is_whole(root, "nvme0n1p1"), "a partition is no disk");
	check(!tw_disk_is_whole(root, "sda1"), "a SATA partition is no disk");
	check(!tw_disk_is_whole(root, "loop0"), "a loop device is no disk");
	check(!tw_disk_is_whole(root, "zram0"), "zram is no disk");
	check(!tw_disk_is_whole(root, "dm-0"), "device mapper is no disk");
	check(!tw_disk_is_whole(root, "sr0"), "an optical drive is left out");
	check(!tw_disk_is_whole(root, "md127"), "RAID is left out, its disks are counted");
	check(!tw_disk_is_whole(root, ""), "an empty name is no disk");
	check(!tw_disk_is_whole(root, NULL), "no name is no disk");
}

static void test_list(void) {
	struct tw_disk *disks;
	size_t count = tw_disks_list(root, &disks);
	check(count == 3, "three whole disks are listed");
	if (count != 3) {
		tw_disks_free(disks, count);
		return;
	}
	check_str(disks[0].name, "mmcblk0", "sorted by name, first");
	check_str(disks[1].name, "nvme0n1", "sorted by name, second");
	check_str(disks[2].name, "sda", "sorted by name, third");
	check_str(disks[1].model, "PM981a NVMe Samsung 256GB", "the model has its padding cut");
	check(disks[0].model == NULL, "an empty model counts as none");
	check(disks[2].model == NULL, "a missing model counts as none");
	check(disks[1].bytes == 500118192ULL * 512, "the size counts 512 byte sectors");

	char *label = tw_disk_label(&disks[1]);
	check_str(label, "nvme0n1 · PM981a NVMe Samsung 256GB · 256 GB", "label of a named disk");
	free(label);
	label = tw_disk_label(&disks[0]);
	check_str(label, "mmcblk0 · 2 GB", "label of a disk without a model");
	free(label);
	label = tw_disk_label(&disks[2]);
	check_str(label, "sda", "label of an empty card reader");
	free(label);
	tw_disks_free(disks, count);
}

static void test_missing_dir(void) {
	struct tw_disk *disks = (struct tw_disk *)1;
	size_t count = tw_disks_list("/nonexistent/tw-disks", &disks);
	check(count == 0 && disks == NULL, "a missing folder lists nothing");
}

static void test_big_label(void) {
	struct tw_disk disk = { .name = "sdb", .model = "Big", .bytes = 2000398934016ULL };
	char *label = tw_disk_label(&disk);
	check_str(label, "sdb · Big · 2.0 TB", "terabytes get a decimal");
	free(label);
}

int main(void) {
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	build_tree();
	test_whole();
	test_list();
	test_missing_dir();
	test_big_label();
	char command[600];
	snprintf(command, sizeof(command), "rm -rf '%s'", root);
	if (system(command) != 0) {
		fprintf(stderr, "could not remove %s\n", root);
	}
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("disks: all checks passed\n");
	return 0;
}
