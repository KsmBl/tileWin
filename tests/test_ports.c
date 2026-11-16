/*
 * What the ports widget makes of /proc/net (tw_ports): the TCP sockets that
 * listen and the UDP sockets that wait, one entry for each port with its IPv4
 * and IPv6 sockets together, how far each reaches (local, LAN, public),
 * public first, the program of a socket, and the outgoing UDP sockets only
 * counted.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tw_ports.h"

static int failures;
static char root[256];

#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static void put(const char *path, const char *text) {
	char full[512], cmd[600];
	snprintf(full, sizeof(full), "%s%s", root, path);
	snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", full);
	if (system(cmd) != 0) {
		return;
	}
	FILE *f = fopen(full, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static const struct tw_port *find(const struct tw_ports *p, unsigned port) {
	for (int i = 0; i < p->count; i++) {
		if (p->items[i].port == port) {
			return &p->items[i];
		}
	}
	return NULL;
}

#define HEAD "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when " \
	"retrnsmt   uid  timeout inode\n"
#define REST " 00000000:00000000 00:00000000 00000000"

int main(void) {
	// /proc/net writes the addresses as numbers in the order of this machine
	int one = 1;
	if (*(const unsigned char *)&one != 1) {
		printf("ports: skipped on a big-endian machine\n");
		return 0;
	}
	snprintf(root, sizeof(root), "/tmp/tw-ports-XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}

	char addr[64];
	bool any;
	CHECK(tw_port_scope_of("0100007F", &any, addr, sizeof(addr)) == TW_PORT_LOCAL && !any &&
		strcmp(addr, "127.0.0.1") == 0, "127.0.0.1 is local (%s)", addr);
	CHECK(tw_port_scope_of("00000000", &any, addr, sizeof(addr)) == TW_PORT_PUBLIC && any,
		"0.0.0.0 is public on all addresses");
	CHECK(tw_port_scope_of("0101A8C0", &any, NULL, 0) == TW_PORT_LAN, "192.168.1.1 is LAN");
	CHECK(tw_port_scope_of("0100110A", &any, NULL, 0) == TW_PORT_LAN, "10.17.0.1 is LAN");
	CHECK(tw_port_scope_of("010011AC", &any, NULL, 0) == TW_PORT_LAN, "172.17.0.1 is LAN");
	CHECK(tw_port_scope_of("010020AC", &any, NULL, 0) == TW_PORT_PUBLIC, "172.32.0.1 is public");
	CHECK(tw_port_scope_of("08080808", &any, NULL, 0) == TW_PORT_PUBLIC, "8.8.8.8 is public");
	CHECK(tw_port_scope_of("00000000000000000000000001000000", &any, addr, sizeof(addr)) ==
		TW_PORT_LOCAL && strcmp(addr, "::1") == 0, "::1 is local (%s)", addr);
	CHECK(tw_port_scope_of("00000000000000000000000000000000", &any, NULL, 0) ==
		TW_PORT_PUBLIC && any, ":: is public on all addresses");
	CHECK(tw_port_scope_of("000080FE000000000000000001000000", &any, NULL, 0) == TW_PORT_LAN,
		"fe80::1 is LAN");
	CHECK(tw_port_scope_of("0000000000000000FFFF00000100007F", &any, addr, sizeof(addr)) ==
		TW_PORT_LOCAL && strcmp(addr, "127.0.0.1") == 0, "::ffff:127.0.0.1 is local (%s)", addr);

	put("/sys/net/ipv4/ip_local_port_range", "32768\t60999\n");
	put("/net/tcp", HEAD
		// ssh on all addresses
		"   0: 00000000:0016 00000000:0000 0A" REST "     0        0 101 1\n"
		// postgres on 127.0.0.1
		"   1: 0100007F:1538 00000000:0000 0A" REST "   970        0 102 1\n"
		// a connection, not a listener
		"   2: 0101A8C0:D431 08080808:01BB 01" REST "  1000        0 103 1\n"
		// a web server on a LAN address
		"   3: 0101A8C0:1F90 00000000:0000 0A" REST "  1000        0 104 1\n"
		// DNS on a loopback address, TCP and UDP
		"   4: 9900007F:0035 00000000:0000 0A" REST "   977        0 105 1\n");
	put("/net/tcp6", HEAD
		// ssh again on ::, postgres on ::1
		"   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A"
		REST "     0        0 201 1\n"
		"   1: 00000000000000000000000001000000:1538 00000000000000000000000000000000:0000 0A"
		REST "   970        0 202 1\n");
	put("/net/udp", HEAD
		"   0: 9900007F:0035 00000000:0000 07" REST "   977        0 301 2 0\n"
		// mDNS on all addresses
		"   1: 00000000:14E9 00000000:0000 07" REST "  1000        0 302 2 0\n"
		// an outgoing socket on a port of the ephemeral range
		"   2: 00000000:ADDC 00000000:0000 07" REST "  1000        0 303 2 0\n"
		// WireGuard: the kernel's socket on a port of that range, without an inode
		"   3: 00000000:CA6C 00000000:0000 07" REST "     0        0 0 2 0\n"
		// a connected UDP socket
		"   4: 0101A8C0:9C40 08080808:0035 01" REST "  1000        0 305 2 0\n");
	// a process with the web server's socket open
	put("/4242/comm", "python3\n");
	char fd[512], cmd[700];
	snprintf(fd, sizeof(fd), "%s/4242/fd", root);
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && ln -s 'socket:[104]' '%s/7' && "
		"ln -s /dev/null '%s/0'", fd, fd, fd);
	CHECK(system(cmd) == 0, "the fake process is made");

	struct tw_ports p = { 0 };
	tw_ports_read_from(&p, root, true);
	CHECK(p.count == 6, "six ports (22, 53, 5353, 5432, 8080, 51820), not %d", p.count);
	CHECK(p.temporary == 1, "one outgoing UDP socket counted, not %d", p.temporary);
	CHECK(p.public_ == 3 && p.lan == 1 && p.local == 2,
		"3 public, 1 LAN, 2 local, not %d, %d, %d", p.public_, p.lan, p.local);

	const struct tw_port *ssh = find(&p, 22);
	CHECK(ssh && ssh->scope == TW_PORT_PUBLIC && ssh->any && ssh->tcp && !ssh->udp,
		"ssh is public on all addresses, TCP");
	const struct tw_port *dns = find(&p, 53);
	CHECK(dns && dns->scope == TW_PORT_LOCAL && dns->tcp && dns->udp &&
		strcmp(dns->address, "127.0.0.153") == 0, "DNS is local, TCP and UDP, on 127.0.0.153");
	CHECK(dns && strcmp(tw_port_protocols(dns), "TCP+UDP") == 0, "DNS is TCP+UDP");
	const struct tw_port *pg = find(&p, 5432);
	CHECK(pg && pg->scope == TW_PORT_LOCAL && !pg->any, "postgres is local on both");
	const struct tw_port *web = find(&p, 8080);
	CHECK(web && web->scope == TW_PORT_LAN && strcmp(web->address, "192.168.1.1") == 0,
		"the web server is LAN on 192.168.1.1");
	CHECK(web && web->pid == 4242 && strcmp(web->program, "python3") == 0,
		"the web server's program is python3 (%s)", web ? web->program : "-");
	char name[64];
	CHECK(web && strcmp(tw_port_name(web, name, sizeof(name)), "python3") == 0,
		"named after its program");
	CHECK(find(&p, 51820) && find(&p, 51820)->udp, "WireGuard's port is kept");
	CHECK(!find(&p, 44508), "the outgoing UDP socket is left out");
	CHECK(!find(&p, 54321), "the connection is left out");

	// public first, then LAN, then local, each by port
	for (int i = 1; i < p.count; i++) {
		const struct tw_port *a = &p.items[i - 1], *b = &p.items[i];
		CHECK(a->scope > b->scope || (a->scope == b->scope && a->port < b->port),
			"in order: %u before %u", a->port, b->port);
	}

	// without the programs nothing is looked up
	tw_ports_read_from(&p, root, false);
	web = find(&p, 8080);
	CHECK(web && web->pid == 0 && !web->program[0], "no program without asking for them");
	tw_ports_free(&p);

	// nothing there at all
	struct tw_ports none = { 0 };
	tw_ports_read_from(&none, "/nonexistent", true);
	CHECK(none.count == 0 && none.temporary == 0, "no ports without /proc/net");
	tw_ports_free(&none);

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0) {
		failures++;
	}
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ports: all checks passed\n");
	return 0;
}
