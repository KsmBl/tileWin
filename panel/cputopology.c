#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cputopology.h"

static bool read_sys(const char *path, char *buf, size_t size) {
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	size_t n = fread(buf, 1, size - 1, file);
	fclose(file);
	buf[n] = '\0';
	while (n > 0 && isspace((unsigned char)buf[n - 1])) {
		buf[--n] = '\0';
	}
	return true;
}

/* A CPU list as the kernel writes it ("0-15,20") into a bitmap of CPU numbers. */
static bool parse_list(const char *path, bool *set, int size) {
	char buf[512];
	if (!read_sys(path, buf, sizeof(buf)) || !buf[0]) {
		return false;
	}
	bool any = false;
	char *save = NULL;
	for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		int a, b;
		int n = sscanf(tok, "%d-%d", &a, &b);
		if (n < 1) {
			continue;
		}
		if (n == 1) {
			b = a;
		}
		for (int i = a; i <= b && i < size; i++) {
			if (i >= 0) {
				set[i] = true;
				any = true;
			}
		}
	}
	return any;
}

static long sys_long(const char *root, int cpu, const char *file) {
	char path[512], buf[64];
	snprintf(path, sizeof(path), "%s/system/cpu/cpu%d/%s", root, cpu, file);
	return read_sys(path, buf, sizeof(buf)) ? atol(buf) : -1;
}

/*
 * Intel lists its kinds as two PMUs; elsewhere the kernel's capacity of each
 * CPU tells them apart, or else a much lower top clock. Returns whether there
 * are two kinds.
 */
static bool read_kinds(const char *root, const int *cpu_num, int n, const long *max_freq,
		enum cpu_kind *kind) {
	static bool atom[4096], big[4096];
	memset(atom, 0, sizeof(atom));
	memset(big, 0, sizeof(big));
	char atom_path[512], big_path[512];
	snprintf(atom_path, sizeof(atom_path), "%s/cpu_atom/cpus", root);
	snprintf(big_path, sizeof(big_path), "%s/cpu_core/cpus", root);
	if (parse_list(atom_path, atom, 4096) && parse_list(big_path, big, 4096)) {
		for (int i = 0; i < n; i++) {
			int c = cpu_num[i];
			kind[i] = c >= 0 && c < 4096 && atom[c] ? CPU_KIND_E : CPU_KIND_P;
		}
		return true;
	}
	long cap[CPU_MAX_CORES], cap_max = 0, cap_min = -1;
	long freq_max = 0, freq_min = -1;
	for (int i = 0; i < n; i++) {
		cap[i] = sys_long(root, cpu_num[i], "cpu_capacity");
		if (cap[i] > 0) {
			cap_max = cap[i] > cap_max ? cap[i] : cap_max;
			cap_min = cap_min < 0 || cap[i] < cap_min ? cap[i] : cap_min;
		}
		if (max_freq[i] > 0) {
			freq_max = max_freq[i] > freq_max ? max_freq[i] : freq_max;
			freq_min = freq_min < 0 || max_freq[i] < freq_min ? max_freq[i] : freq_min;
		}
	}
	// a few percent apart are the favored cores of one kind, not two kinds
	if (cap_max > 0 && cap_min > 0 && cap_min < cap_max * 8 / 10) {
		for (int i = 0; i < n; i++) {
			kind[i] = cap[i] >= cap_max * 6 / 10 ? CPU_KIND_P : CPU_KIND_E;
		}
		return true;
	}
	if (freq_max > 0 && freq_min > 0 && freq_min < freq_max * 85 / 100) {
		for (int i = 0; i < n; i++) {
			kind[i] = max_freq[i] >= freq_max * 85 / 100 ? CPU_KIND_P : CPU_KIND_E;
		}
		return true;
	}
	return false;
}

void cpu_topology_read(struct cpu_topology *t, const char *root, const int *cpu_num, int n) {
	memset(t, 0, sizeof(*t));
	if (n > CPU_MAX_CORES) {
		n = CPU_MAX_CORES;
	}
	enum cpu_kind kind[CPU_MAX_CORES] = { 0 };
	long max_freq[CPU_MAX_CORES];
	for (int i = 0; i < n; i++) {
		max_freq[i] = sys_long(root, cpu_num[i], "cpufreq/cpuinfo_max_freq");
	}
	t->hybrid = read_kinds(root, cpu_num, n, max_freq, kind);

	// the threads of one core share its package, die and core id
	long long key[CPU_MAX_CORES];
	for (int i = 0; i < n; i++) {
		int c = cpu_num[i];
		long pkg = sys_long(root, c, "topology/physical_package_id");
		long die = sys_long(root, c, "topology/die_id");
		long core = sys_long(root, c, "topology/core_id");
		key[i] = core < 0 ? -1 - (long long)c :
			((long long)(pkg & 0xfffff) << 40) | ((long long)(die & 0xfffff) << 20) |
			(core & 0xfffff);
	}
	for (int k = CPU_KIND_P; k <= CPU_KIND_E; k++) {
		for (int i = 0; i < n; i++) {
			if (kind[i] != (enum cpu_kind)k) {
				continue;
			}
			t->kind_threads[k]++;
			if (max_freq[i] / 1000.0 > t->kind_max_mhz[k]) {
				t->kind_max_mhz[k] = max_freq[i] / 1000.0;
			}
			struct cpu_core *pc = NULL;
			for (int j = 0; j < t->phys_count; j++) {
				struct cpu_core *other = &t->phys[j];
				if (other->kind == (enum cpu_kind)k && key[other->slot[0]] == key[i]) {
					pc = other;
					break;
				}
			}
			if (!pc) {
				pc = &t->phys[t->phys_count++];
				pc->kind = k;
				pc->threads = 0;
				t->kind_cores[k]++;
			}
			if (pc->threads < CPU_MAX_THREADS) {
				pc->slot[pc->threads++] = i;
			}
			if (pc->threads > 1) {
				t->smt = true;
			}
		}
	}
	// one kind only after all (an offline group): no groups to show
	if (t->kind_cores[CPU_KIND_P] == 0 || t->kind_cores[CPU_KIND_E] == 0) {
		t->hybrid = false;
	}
}
