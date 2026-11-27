#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "tw_load.h"

static bool letters_after(const char *name, const char *prefix) {
	size_t n = strlen(prefix);
	if (strncmp(name, prefix, n) != 0 || !name[n]) {
		return false;
	}
	for (const char *p = name + n; *p; p++) {
		if (!islower((unsigned char)*p)) {
			return false;
		}
	}
	return true;
}

bool tw_load_whole_disk(const char *name) {
	if (letters_after(name, "sd") || letters_after(name, "vd") ||
			letters_after(name, "xvd") || letters_after(name, "hd")) {
		return true;
	}
	int a, b, used = 0;
	// nvme0n1, not its partition nvme0n1p2
	if (sscanf(name, "nvme%dn%d%n", &a, &b, &used) == 2 && name[used] == '\0') {
		return true;
	}
	// mmcblk0, not mmcblk0p1 nor mmcblk0boot0
	used = 0;
	if (sscanf(name, "mmcblk%d%n", &a, &used) == 1 && name[used] == '\0') {
		return true;
	}
	return false;
}

bool tw_load_read(struct tw_load *load, const char *proc, double at) {
	memset(load, 0, sizeof(*load));
	load->at = at;
	char path[512];
	snprintf(path, sizeof(path), "%s/stat", proc);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
	fclose(f);
	if (n < 8) {
		return false;
	}
	load->cpu_idle = idle + iowait;
	load->cpu_total = user + nice + system + idle + iowait + irq + softirq + steal;

	snprintf(path, sizeof(path), "%s/diskstats", proc);
	f = fopen(path, "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f) && load->disks < TW_LOAD_DISKS) {
			char name[32];
			unsigned long long io_ms;
			// major minor name, 4 of reading, 4 of writing, in flight, the time doing I/O
			if (sscanf(line, "%*u %*u %31s %*u %*u %*u %*u %*u %*u %*u %*u %*u %llu",
					name, &io_ms) != 2 || !tw_load_whole_disk(name)) {
				continue;
			}
			snprintf(load->disk[load->disks].name, sizeof(load->disk[0].name), "%s", name);
			load->disk[load->disks++].io_ms = io_ms;
		}
		fclose(f);
	}
	load->valid = true;
	return true;
}

void tw_load_between(const struct tw_load *before, const struct tw_load *after, int *cpu,
		int *disk) {
	*cpu = *disk = -1;
	if (!before->valid || !after->valid) {
		return;
	}
	if (after->cpu_total > before->cpu_total && after->cpu_idle >= before->cpu_idle) {
		unsigned long long dt = after->cpu_total - before->cpu_total;
		unsigned long long di = after->cpu_idle - before->cpu_idle;
		*cpu = di >= dt ? 0 : (int)(100 * (dt - di) / dt);
	}
	double ms = (after->at - before->at) * 1000;
	if (ms <= 0) {
		return;
	}
	*disk = 0;
	for (int i = 0; i < after->disks; i++) {
		for (int j = 0; j < before->disks; j++) {
			if (strcmp(after->disk[i].name, before->disk[j].name) != 0 ||
					after->disk[i].io_ms < before->disk[j].io_ms) {
				continue;
			}
			int share = (int)(100 * (after->disk[i].io_ms - before->disk[j].io_ms) / ms);
			share = share > 100 ? 100 : share;
			if (share > *disk) {
				*disk = share;
			}
		}
	}
}
