#ifndef _TILEWIN_PANEL_CPUTOPOLOGY_H
#define _TILEWIN_PANEL_CPUTOPOLOGY_H
#include <stdbool.h>

/*
 * Which logical CPUs are threads of one physical core, and on a hybrid
 * processor which cores are performance cores and which efficient ones
 * (Intel P- and E-cores, ARM big.LITTLE, AMD's compact cores). The CPU
 * flyout shows each core with a bar per thread, in two groups on a hybrid
 * processor.
 */

#define CPU_MAX_CORES 256
#define CPU_MAX_THREADS 4 // threads of one physical core shown
#define CPU_SYS_ROOT "/sys/devices"

enum cpu_kind {
	CPU_KIND_P,
	CPU_KIND_E,
};

struct cpu_core {
	enum cpu_kind kind;
	int threads;
	int slot[CPU_MAX_THREADS]; // indexes into the list of CPUs read
};

struct cpu_topology {
	bool hybrid;
	bool smt; // some core runs more than one thread
	struct cpu_core phys[CPU_MAX_CORES]; // performance cores first
	int phys_count;
	int kind_cores[2], kind_threads[2];
	double kind_max_mhz[2];
};

/*
 * Reads the topology of the n CPUs numbered cpu_num ("cpu12" is 12) from
 * sys_root: CPU_SYS_ROOT, or a copy of it for the tests.
 */
void cpu_topology_read(struct cpu_topology *t, const char *sys_root, const int *cpu_num, int n);

#endif
