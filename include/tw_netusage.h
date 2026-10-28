#ifndef _TW_NETUSAGE_H
#define _TW_NETUSAGE_H
#include <stdbool.h>
#include <stddef.h>

/*
 * How much went in and out over the network since the machine was switched
 * on. The kernel counts bytes per interface, but starts again from nothing
 * when an interface goes away and comes back (Wi-Fi switched off and on, a
 * USB adapter pulled), so the counters alone only tell what went over the
 * connection as it is now. The ledger remembers every interface it has seen
 * with what its counter last said, adds what the counter grew by since, and
 * when the counter went backwards, takes that as a fresh start.
 *
 * It is kept in a file with the boot id beside it, so a restarted panel goes
 * on counting, and a ledger of an earlier boot is thrown away.
 */

#define TW_NET_DIR "/sys/class/net"
#define TW_BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"
#define TW_NET_LEDGER_MAX 32

struct tw_net_ledger {
	char boot_id[64];
	int count;
	struct {
		char name[32];
		unsigned long long rx_seen, tx_seen;   // what the counters last said
		unsigned long long rx_total, tx_total; // since boot
	} ifaces[TW_NET_LEDGER_MAX];
};

/* Takes in what the counters of one interface say now. */
void tw_net_ledger_add(struct tw_net_ledger *ledger, const char *name,
	unsigned long long rx, unsigned long long tx);

/* Reads the counters of every interface under net_dir (TW_NET_DIR, or a copy
 * of it for the tests) but the loopback one. */
void tw_net_ledger_scan(struct tw_net_ledger *ledger, const char *net_dir);

/* The sums over every interface since boot. */
void tw_net_ledger_totals(const struct tw_net_ledger *ledger,
	unsigned long long *rx, unsigned long long *tx);

/*
 * Fills the ledger from the file at path when it was written in this boot
 * (boot_id), and starts it empty otherwise. Returns whether it was loaded.
 */
bool tw_net_ledger_load(struct tw_net_ledger *ledger, const char *path, const char *boot_id);

bool tw_net_ledger_save(const struct tw_net_ledger *ledger, const char *path);

/* $XDG_CACHE_HOME/tileWin/net-usage. Newly allocated. */
char *tw_net_ledger_path(void);

#endif
