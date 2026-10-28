#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "tw_netusage.h"
#include "tw_paths.h"

void tw_net_ledger_add(struct tw_net_ledger *ledger, const char *name,
		unsigned long long rx, unsigned long long tx) {
	for (int i = 0; i < ledger->count; i++) {
		if (strcmp(ledger->ifaces[i].name, name) != 0) {
			continue;
		}
		// a counter that went backwards started again from nothing
		ledger->ifaces[i].rx_total += rx >= ledger->ifaces[i].rx_seen ?
			rx - ledger->ifaces[i].rx_seen : rx;
		ledger->ifaces[i].tx_total += tx >= ledger->ifaces[i].tx_seen ?
			tx - ledger->ifaces[i].tx_seen : tx;
		ledger->ifaces[i].rx_seen = rx;
		ledger->ifaces[i].tx_seen = tx;
		return;
	}
	if (ledger->count >= TW_NET_LEDGER_MAX || strlen(name) >= sizeof(ledger->ifaces[0].name)) {
		return;
	}
	// new to the ledger: all it counted so far happened in this boot
	int i = ledger->count++;
	snprintf(ledger->ifaces[i].name, sizeof(ledger->ifaces[i].name), "%s", name);
	ledger->ifaces[i].rx_seen = ledger->ifaces[i].rx_total = rx;
	ledger->ifaces[i].tx_seen = ledger->ifaces[i].tx_total = tx;
}

static bool read_counter(const char *net_dir, const char *iface, const char *which,
		unsigned long long *out) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/statistics/%s", net_dir, iface, which);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fscanf(f, "%llu", out) == 1;
	fclose(f);
	return ok;
}

void tw_net_ledger_scan(struct tw_net_ledger *ledger, const char *net_dir) {
	DIR *dir = opendir(net_dir);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.' || strcmp(entry->d_name, "lo") == 0) {
			continue;
		}
		unsigned long long rx, tx;
		if (read_counter(net_dir, entry->d_name, "rx_bytes", &rx) &&
				read_counter(net_dir, entry->d_name, "tx_bytes", &tx)) {
			tw_net_ledger_add(ledger, entry->d_name, rx, tx);
		}
	}
	closedir(dir);
}

void tw_net_ledger_totals(const struct tw_net_ledger *ledger,
		unsigned long long *rx, unsigned long long *tx) {
	*rx = *tx = 0;
	for (int i = 0; i < ledger->count; i++) {
		*rx += ledger->ifaces[i].rx_total;
		*tx += ledger->ifaces[i].tx_total;
	}
}

bool tw_net_ledger_load(struct tw_net_ledger *ledger, const char *path, const char *boot_id) {
	memset(ledger, 0, sizeof(*ledger));
	snprintf(ledger->boot_id, sizeof(ledger->boot_id), "%s", boot_id ? boot_id : "");
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char line[256], id[64];
	bool same_boot = fgets(line, sizeof(line), f) && sscanf(line, "boot %63s", id) == 1 &&
		strcmp(id, ledger->boot_id) == 0;
	while (same_boot && ledger->count < TW_NET_LEDGER_MAX && fgets(line, sizeof(line), f)) {
		int i = ledger->count;
		if (sscanf(line, "%31s %llu %llu %llu %llu", ledger->ifaces[i].name,
				&ledger->ifaces[i].rx_seen, &ledger->ifaces[i].tx_seen,
				&ledger->ifaces[i].rx_total, &ledger->ifaces[i].tx_total) == 5) {
			ledger->count++;
		} else {
			memset(&ledger->ifaces[i], 0, sizeof(ledger->ifaces[i]));
		}
	}
	fclose(f);
	return same_boot;
}

bool tw_net_ledger_save(const struct tw_net_ledger *ledger, const char *path) {
	if (!path || !ledger->boot_id[0]) {
		return false;
	}
	size_t size = 80 + (size_t)ledger->count * 120;
	char *text = malloc(size);
	if (!text) {
		return false;
	}
	size_t used = snprintf(text, size, "boot %s\n", ledger->boot_id);
	for (int i = 0; i < ledger->count && used < size; i++) {
		used += snprintf(text + used, size - used, "%s %llu %llu %llu %llu\n",
			ledger->ifaces[i].name, ledger->ifaces[i].rx_seen, ledger->ifaces[i].tx_seen,
			ledger->ifaces[i].rx_total, ledger->ifaces[i].tx_total);
	}
	bool ok = tw_write_string(path, text);
	free(text);
	return ok;
}

char *tw_net_ledger_path(void) {
	char *dir = tw_cache_dir();
	if (!dir) {
		return NULL;
	}
	char *path = format_str("%s/net-usage", dir);
	free(dir);
	return path;
}
