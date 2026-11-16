#ifndef _TW_PORTS_H
#define _TW_PORTS_H
#include <stdbool.h>
#include <stddef.h>

/*
 * The ports this computer listens on, for the Ports widget: every TCP socket
 * that listens and every UDP socket bound and waiting, read from /proc/net,
 * one entry for each port (its IPv4 and IPv6 sockets, TCP and UDP, together),
 * and how far it can be reached:
 *
 *   local   only from this computer (127.0.0.0/8, ::1)
 *   lan     an address of a private network (10/8, 172.16/12, 192.168/16,
 *           100.64/10, 169.254/16, fc00::/7, fe80::/10): the computers there
 *   public  all addresses (0.0.0.0, ::) or a public one: every network the
 *           computer is on, the internet too when nothing stands in between
 *
 * UDP sockets on a port of the range the kernel hands out for outgoing
 * connections (ip_local_port_range) are the answers to a program's own
 * requests (DNS lookups, for one), not a service: they are only counted.
 */

enum tw_port_scope {
	TW_PORT_LOCAL,
	TW_PORT_LAN,
	TW_PORT_PUBLIC,
};

struct tw_port {
	unsigned port;
	bool tcp, udp;
	enum tw_port_scope scope;
	char address[48]; // where it listens, as written ("0.0.0.0", "::", "127.0.0.1")
	bool any;         // on all addresses
	char program[32]; // the process, "" when it is not ours to see (another user's)
	int pid;          // 0 when unknown
	unsigned uid;
	char user[32];
	char service[32]; // the name in /etc/services, "" for none
};

struct tw_ports {
	struct tw_port *items; // public first, then lan, then local, each by port
	int count, cap;
	int local, lan, public_; // ports of each scope
	int temporary;           // outgoing UDP sockets left out
	bool programs;           // the programs were looked up
};

/* Reads them from /proc; with programs, finds the process of each one it may see. */
void tw_ports_read(struct tw_ports *p, bool programs);
/* The same from another root (tests): <root>/net/tcp, <root>/sys/net/ipv4/... */
void tw_ports_read_from(struct tw_ports *p, const char *proc, bool programs);
void tw_ports_free(struct tw_ports *p);

/*
 * The scope of an address as /proc/net writes it: 8 hex digits for IPv4,
 * 32 for IPv6. Sets any for 0.0.0.0 and ::, and writes it readably to out.
 */
enum tw_port_scope tw_port_scope_of(const char *hex, bool *any, char *out, size_t size);

/* "Local", "LAN", "Public". */
const char *tw_port_scope_name(enum tw_port_scope scope);
/* The name to show: the program, else the service, else "port N". */
const char *tw_port_name(const struct tw_port *port, char *buf, size_t size);
/* "TCP", "UDP" or "TCP+UDP". */
const char *tw_port_protocols(const struct tw_port *port);

#endif
