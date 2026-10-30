/*
 * The cores of the CPU flyout: which CPUs are threads of one core, so they
 * share a cell, and on a hybrid processor which cores are performance and
 * which efficient ones. The tests build small copies of /sys/devices in a
 * temporary folder: a processor of one kind with and without two threads a
 * core, an Intel hybrid one, and one whose favored cores are a little faster.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cputopology.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static char base[] = "/tmp/tw-cputopo-XXXXXX";
static char root[600];

static void write_file(const char *rel, const char *content) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	for (char *p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(path, 0755);
			*p = '/';
		}
	}
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", content);
		fclose(f);
	}
}

static void write_cpu(int cpu, int core, long max_khz, long capacity) {
	char rel[128], value[32];
	snprintf(rel, sizeof(rel), "system/cpu/cpu%d/topology/core_id", cpu);
	snprintf(value, sizeof(value), "%d", core);
	write_file(rel, value);
	snprintf(rel, sizeof(rel), "system/cpu/cpu%d/topology/physical_package_id", cpu);
	write_file(rel, "0");
	snprintf(rel, sizeof(rel), "system/cpu/cpu%d/topology/die_id", cpu);
	write_file(rel, "0");
	snprintf(rel, sizeof(rel), "system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
	snprintf(value, sizeof(value), "%ld", max_khz);
	write_file(rel, value);
	if (capacity > 0) {
		snprintf(rel, sizeof(rel), "system/cpu/cpu%d/cpu_capacity", cpu);
		snprintf(value, sizeof(value), "%ld", capacity);
		write_file(rel, value);
	}
}

static void new_root(const char *name) {
	snprintf(root, sizeof(root), "%s/%s", base, name);
	mkdir(root, 0755);
}

static int cpus[CPU_MAX_CORES];

int main(void) {
	if (!mkdtemp(base)) {
		perror("mkdtemp");
		return 1;
	}
	for (int i = 0; i < CPU_MAX_CORES; i++) {
		cpus[i] = i;
	}
	struct cpu_topology t;

	// an older Intel: 4 cores, 8 threads, the second thread of each core numbered after the first
	new_root("smt");
	for (int i = 0; i < 8; i++) {
		write_cpu(i, i % 4, 4200000, 0);
	}
	cpu_topology_read(&t, root, cpus, 8);
	check(!t.hybrid, "one kind of core is no hybrid");
	check(t.smt, "two threads a core are seen");
	check(t.phys_count == 4, "8 threads are 4 cores");
	bool paired = t.phys_count == 4;
	for (int c = 0; paired && c < 4; c++) {
		paired = t.phys[c].threads == 2 && t.phys[c].slot[0] == c && t.phys[c].slot[1] == c + 4;
	}
	check(paired, "cpu0 and cpu4 share core 0, cpu1 and cpu5 core 1 and so on");

	// AMD: the threads of a core numbered side by side
	new_root("smt-amd");
	for (int i = 0; i < 6; i++) {
		write_cpu(i, i / 2, 4600000, 0);
	}
	cpu_topology_read(&t, root, cpus, 6);
	check(t.smt && t.phys_count == 3, "6 threads numbered in pairs are 3 cores");
	check(t.phys_count == 3 && t.phys[1].threads == 2 && t.phys[1].slot[0] == 2 &&
		t.phys[1].slot[1] == 3, "cpu2 and cpu3 share core 1");

	// one thread a core: a cell for each, as before
	new_root("plain");
	for (int i = 0; i < 4; i++) {
		write_cpu(i, i, 3000000, 0);
	}
	cpu_topology_read(&t, root, cpus, 4);
	check(!t.hybrid && !t.smt && t.phys_count == 4, "one thread a core is no smt");

	// Intel hybrid: 2 P-cores with two threads each, 4 E-cores
	new_root("hybrid");
	for (int i = 0; i < 4; i++) {
		write_cpu(i, i / 2, 5000000, 0);
	}
	for (int i = 4; i < 8; i++) {
		write_cpu(i, 8 + i, 3800000, 0);
	}
	write_file("cpu_core/cpus", "0-3");
	write_file("cpu_atom/cpus", "4-7");
	cpu_topology_read(&t, root, cpus, 8);
	check(t.hybrid, "Intel's two PMUs are a hybrid");
	check(t.kind_cores[CPU_KIND_P] == 2 && t.kind_threads[CPU_KIND_P] == 4, "2 P-cores, 4 threads");
	check(t.kind_cores[CPU_KIND_E] == 4 && t.kind_threads[CPU_KIND_E] == 4, "4 E-cores");
	check(t.phys_count == 6 && t.phys[0].kind == CPU_KIND_P && t.phys[5].kind == CPU_KIND_E,
		"performance cores come first");

	// favored cores a few percent faster are still one kind
	new_root("favored");
	for (int i = 0; i < 8; i++) {
		write_cpu(i, i / 2, i < 2 ? 5100000 : 4900000, i < 2 ? 1024 : 990);
	}
	cpu_topology_read(&t, root, cpus, 8);
	check(!t.hybrid && t.smt && t.phys_count == 4, "favored cores are no second kind");

	char cmd[700];
	snprintf(cmd, sizeof(cmd), "rm -r %s", base);
	if (system(cmd) != 0) {
		fprintf(stderr, "could not remove %s\n", base);
	}
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all CPU topology tests passed\n");
	return 0;
}
