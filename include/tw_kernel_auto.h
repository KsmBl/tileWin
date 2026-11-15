#ifndef _TW_KERNEL_AUTO_H
#define _TW_KERNEL_AUTO_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * The state of the automatic kernel updates (tilewin-kernel-root auto-run), as
 * the kernel widget, its flyout and the Kernel page of the settings show it.
 * Everything is read from files anyone may read:
 *
 *   /etc/tilewin/kernel-auto.conf               what is built, for whom
 *   /var/lib/tilewin/kernel-auto/status         what the run does and did
 *   /var/lib/tilewin/kernel-auto/build/kernel   how far a kernel build is
 *   /var/lib/tilewin/kernel-auto/build/fixes    how far the live fixes are
 *   /var/lib/tilewin/kernel-auto/log            what was done, newest last
 *   /run/tilewin-kernel-auto.lock               held while a run goes on
 *
 * TILEWIN_KERNEL_AUTO_ROOT puts all of them below another folder (the tests).
 */

#define TW_KERNEL_AUTO_LOG 8

struct tw_kernel_build {
	char phase[16];   // download, verify, unpack, configure, compile, modules, check,
	                  // sort, build, done, failed, cancelled, none
	char version[32];
	char text[192];
	int percent;      // -1 when not known
	time_t updated;
};

struct tw_kernel_auto {
	bool configured;  // switched on once
	bool enabled;
	char profile[16], series[8];
	char when[8], time[8]; // daily or mon..sun, HH:MM
	bool catchup;          // a time the computer was off at is made up at the next start
	bool native, battery;
	bool busy;        // a run goes on now
	char state[16];   // idle, fixes, building, applying, installing, off
	char message[192], error[256], arm_error[256];
	char running[96]; // the kernel that runs
	bool running_auto; // and it is one of the updates
	char next[96];    // the kernel set for the next start, "" when none
	char fixes[32];   // the release whose fixes are in the running kernel
	int fixes_left;   // the changed files of them that need a restart
	time_t checked, fixes_applied;
	bool on_battery;  // nothing was built: no charger
	struct tw_kernel_build kernel, fixes_build;
	int log_count;
	struct {
		time_t when;
		char text[192];
	} log[TW_KERNEL_AUTO_LOG]; // newest first
};

/* Reads it all; false when the updates were never switched on. */
bool tw_kernel_auto_read(struct tw_kernel_auto *ka);

enum tw_kernel_auto_kind {
	TW_KA_OFF,         // switched off, or never on
	TW_KA_OK,          // up to date
	TW_KA_CHECKING,    // asking kernel.org
	TW_KA_DOWNLOADING, // the source or the fixes
	TW_KA_BUILDING,    // compiling a kernel
	TW_KA_FIXING,      // making the live fixes
	TW_KA_APPLYING,    // putting fixes or a kernel on
	TW_KA_FIXED,       // fixes went in live lately
	TW_KA_RESTART,     // a newer kernel waits for the next start
	TW_KA_WAITING,     // on battery: builds wait for the charger
	TW_KA_PENDING,     // on, but no kernel of the updates yet
	TW_KA_ERROR,
};

/* What the widget shows, in short and in full. */
struct tw_kernel_auto_summary {
	enum tw_kernel_auto_kind kind;
	int percent;       // of what goes on now, -1 when not known
	char brief[64];    // "Building 7.3.6 · 69%"
	char title[96];    // "Building Linux 7.3.6"
	char detail[256];  // a sentence more
	bool attention;    // worth a look: an error, a restart that brings more
};

void tw_kernel_auto_summarize(const struct tw_kernel_auto *ka, time_t now,
	struct tw_kernel_auto_summary *out);

/* When the updates run: "every day at 03:00", "every Monday at 21:30". */
void tw_kernel_auto_schedule_words(const struct tw_kernel_auto *ka, char *out, size_t size);

/* The release of a kernel name: "7.3.6" of "7.3.6-tilewin2". */
void tw_kernel_auto_release(const char *kver, char *out, size_t size);

/*
 * A shield in the color of the state (0xRRGGBBAA) with its sign in it: a tick,
 * an arrow down, dots while busy, the arrow of a restart, "!" or a bar for
 * off; flat with a dark edge (Windows 95) or with a light top.
 */
typedef struct _cairo cairo_t;
#include <stdint.h>
void tw_kernel_auto_draw_shield(cairo_t *cr, double x, double y, double size,
	enum tw_kernel_auto_kind kind, uint32_t color, bool flat);

/* "3 min", "2 h", "5 days" ago. */
void tw_kernel_auto_ago(time_t then, time_t now, char *out, size_t size);

#endif
