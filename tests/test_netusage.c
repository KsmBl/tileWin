/*
 * The network usage since the machine came up. The kernel's counters start
 * again from nothing when an interface comes back, so the ledger has to add
 * up what each one counted before; and it has to survive a restarted panel,
 * but not a reboot. The tests build a small copy of /sys/class/net in a
 * temporary folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tw_netusage.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void check_totals(const struct tw_net_ledger *ledger, unsigned long long want_rx,
		unsigned long long want_tx, const char *what) {
	unsigned long long rx, tx;
	tw_net_ledger_totals(ledger, &rx, &tx);
	if (rx != want_rx || tx != want_tx) {
		fprintf(stderr, "FAIL: %s (got %llu in, %llu out, wanted %llu in, %llu out)\n",
			what, rx, tx, want_rx, want_tx);
		failures++;
	}
}

static char root[] = "/tmp/tw-netusage-XXXXXX";

static void set_counters(const char *iface, unsigned long long rx, unsigned long long tx) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, iface);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s/statistics", root, iface);
	mkdir(path, 0755);
	const char *names[] = { "rx_bytes", "tx_bytes" };
	unsigned long long values[] = { rx, tx };
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%s/statistics/%s", root, iface, names[i]);
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "%llu\n", values[i]);
			fclose(f);
		}
	}
}

int main(void) {
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}

	struct tw_net_ledger ledger;
	char path[512];
	snprintf(path, sizeof(path), "%s/net-usage", root);
	check(!tw_net_ledger_load(&ledger, path, "boot-a"), "no file is no ledger");

	// what the counters held when it first looks happened in this boot
	set_counters("lo", 999999, 999999);
	set_counters("wlan0", 1000, 100);
	set_counters("eth0", 50, 5);
	tw_net_ledger_scan(&ledger, root);
	check_totals(&ledger, 1050, 105, "first look counts what the counters hold, not lo");

	set_counters("wlan0", 1500, 300);
	tw_net_ledger_scan(&ledger, root);
	check_totals(&ledger, 1550, 305, "growth is added");

	// Wi-Fi switched off and on: its counters start again
	set_counters("wlan0", 200, 20);
	tw_net_ledger_scan(&ledger, root);
	check_totals(&ledger, 1750, 325, "a counter that starts again keeps what came before");

	set_counters("wlan0", 700, 70);
	tw_net_ledger_scan(&ledger, root);
	check_totals(&ledger, 2250, 375, "and goes on counting after");

	// a USB adapter pulled out: its interface is gone, its traffic is not
	char gone[600];
	snprintf(gone, sizeof(gone), "rm -r %s/eth0", root);
	check(system(gone) == 0, "removing eth0");
	tw_net_ledger_scan(&ledger, root);
	check_totals(&ledger, 2250, 375, "an interface that went away still counts");

	// a restarted panel picks up where it was
	snprintf(ledger.boot_id, sizeof(ledger.boot_id), "boot-a");
	check(tw_net_ledger_save(&ledger, path), "saving");
	struct tw_net_ledger again;
	check(tw_net_ledger_load(&again, path, "boot-a"), "loading in the same boot");
	check_totals(&again, 2250, 375, "the loaded ledger has the same totals");
	set_counters("wlan0", 800, 80);
	tw_net_ledger_scan(&again, root);
	check_totals(&again, 2350, 385, "after loading only the growth is added");

	// after a reboot the old ledger is thrown away
	struct tw_net_ledger fresh;
	check(!tw_net_ledger_load(&fresh, path, "boot-b"), "a ledger of another boot is not loaded");
	check_totals(&fresh, 0, 0, "a new boot starts empty");
	tw_net_ledger_scan(&fresh, root);
	check_totals(&fresh, 800, 80, "and counts from the counters");

	snprintf(gone, sizeof(gone), "rm -r %s", root);
	if (system(gone) != 0) {
		fprintf(stderr, "could not remove %s\n", root);
	}
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all network usage tests passed\n");
	return 0;
}
