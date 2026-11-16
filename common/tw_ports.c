#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tw_ports.h"

/* One socket as /proc/net writes it, before the sockets of a port go together. */
struct sock {
	unsigned port;
	bool tcp;
	enum tw_port_scope scope;
	bool any;
	char address[48];
	unsigned uid;
	unsigned long inode;
};

struct socks {
	struct sock *items;
	int count, cap;
};

static void add_sock(struct socks *s, const struct sock *sock) {
	if (s->count == s->cap) {
		int cap = s->cap ? s->cap * 2 : 32;
		struct sock *items = realloc(s->items, cap * sizeof(*items));
		if (!items) {
			return;
		}
		s->items = items;
		s->cap = cap;
	}
	s->items[s->count++] = *sock;
}

static bool hex_bytes(const char *hex, unsigned char *out, int words) {
	// each word of 4 bytes is written as the number it is in memory, so the
	// bytes come back in order by storing the number as this machine does
	for (int i = 0; i < words; i++) {
		char word[9];
		memcpy(word, hex + 8 * i, 8);
		word[8] = '\0';
		for (int k = 0; k < 8; k++) {
			if (!isxdigit((unsigned char)word[k])) {
				return false;
			}
		}
		uint32_t v = (uint32_t)strtoul(word, NULL, 16);
		memcpy(out + 4 * i, &v, 4);
	}
	return true;
}

static enum tw_port_scope scope_v4(const unsigned char *a, bool *any) {
	*any = a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0;
	if (*any) {
		return TW_PORT_PUBLIC;
	}
	if (a[0] == 127) {
		return TW_PORT_LOCAL;
	}
	if (a[0] == 10 || (a[0] == 172 && (a[1] & 0xf0) == 16) || (a[0] == 192 && a[1] == 168) ||
			(a[0] == 169 && a[1] == 254) || (a[0] == 100 && (a[1] & 0xc0) == 64)) {
		return TW_PORT_LAN;
	}
	return TW_PORT_PUBLIC;
}

enum tw_port_scope tw_port_scope_of(const char *hex, bool *any, char *out, size_t size) {
	unsigned char a[16];
	bool dummy;
	any = any ? any : &dummy;
	*any = false;
	size_t len = strspn(hex, "0123456789abcdefABCDEF");
	char text[INET6_ADDRSTRLEN] = "";
	enum tw_port_scope scope = TW_PORT_PUBLIC;
	if (len == 8 && hex_bytes(hex, a, 1)) {
		inet_ntop(AF_INET, a, text, sizeof(text));
		scope = scope_v4(a, any);
	} else if (len == 32 && hex_bytes(hex, a, 4)) {
		static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
		static const unsigned char zero[16] = { 0 };
		if (memcmp(a, mapped, 12) == 0) {
			// an IPv4 address in an IPv6 socket
			inet_ntop(AF_INET, a + 12, text, sizeof(text));
			scope = scope_v4(a + 12, any);
		} else {
			inet_ntop(AF_INET6, a, text, sizeof(text));
			if (memcmp(a, zero, 16) == 0) {
				*any = true;
				scope = TW_PORT_PUBLIC;
			} else if (memcmp(a, zero, 15) == 0 && a[15] == 1) {
				scope = TW_PORT_LOCAL;
			} else if ((a[0] & 0xfe) == 0xfc || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)) {
				scope = TW_PORT_LAN;
			}
		}
	}
	if (out && size) {
		snprintf(out, size, "%s", text);
	}
	return scope;
}

static void read_range(const char *proc, unsigned *low, unsigned *high) {
	*low = 32768;
	*high = 60999;
	char path[512];
	snprintf(path, sizeof(path), "%s/sys/net/ipv4/ip_local_port_range", proc);
	FILE *f = fopen(path, "r");
	if (f) {
		unsigned l, h;
		if (fscanf(f, "%u %u", &l, &h) == 2 && l <= h) {
			*low = l;
			*high = h;
		}
		fclose(f);
	}
}

/*
 * A table of /proc/net: "sl local rem st tx:rx tr:when retr uid timeout inode".
 * TCP sockets that listen (0A); UDP sockets bound and not connected (07, no
 * remote end), apart from those on a port handed out for outgoing traffic.
 */
static void read_table(const char *proc, const char *name, bool tcp, struct socks *out,
		unsigned low, unsigned high, int *temporary) {
	char path[512];
	snprintf(path, sizeof(path), "%s/net/%s", proc, name);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[512];
	bool header = true;
	while (fgets(line, sizeof(line), f)) {
		if (header) {
			header = false;
			continue;
		}
		char local[64], remote[64];
		unsigned state, uid;
		unsigned long inode;
		if (sscanf(line, " %*[^:]: %63s %63s %x %*s %*s %*s %u %*s %lu", local, remote, &state,
				&uid, &inode) != 5) {
			continue;
		}
		char *colon = strchr(local, ':'), *rcolon = strchr(remote, ':');
		if (!colon || !rcolon) {
			continue;
		}
		*colon = '\0';
		unsigned port = (unsigned)strtoul(colon + 1, NULL, 16);
		if (tcp) {
			if (state != 0x0a) {
				continue;
			}
		} else {
			bool unconnected = state == 0x07 && strtoul(rcolon + 1, NULL, 16) == 0;
			if (!unconnected) {
				continue;
			}
			// an outgoing socket; those of the kernel (WireGuard) have no inode
			if (port >= low && port <= high && inode != 0) {
				(*temporary)++;
				continue;
			}
		}
		struct sock s = { .port = port, .tcp = tcp, .uid = uid, .inode = inode };
		s.scope = tw_port_scope_of(local, &s.any, s.address, sizeof(s.address));
		add_sock(out, &s);
	}
	fclose(f);
}

static int cmp_inode(const void *a, const void *b) {
	unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;
	return x < y ? -1 : x > y;
}

struct owner {
	unsigned long inode;
	int pid;
	char comm[32];
};

/* Who has each socket open: the processes we may look into (our own, all as root). */
static struct owner *find_owners(const char *proc, unsigned long *inodes, int n, int *found) {
	*found = 0;
	if (n == 0) {
		return NULL;
	}
	qsort(inodes, n, sizeof(*inodes), cmp_inode);
	struct owner *owners = calloc(n, sizeof(*owners));
	DIR *dir = opendir(proc);
	if (!owners || !dir) {
		free(owners);
		if (dir) {
			closedir(dir);
		}
		return NULL;
	}
	struct dirent *de;
	while ((de = readdir(dir)) && *found < n) {
		if (!isdigit((unsigned char)de->d_name[0])) {
			continue;
		}
		char path[512];
		snprintf(path, sizeof(path), "%s/%s/fd", proc, de->d_name);
		DIR *fds = opendir(path);
		if (!fds) {
			continue;
		}
		struct dirent *fe;
		while ((fe = readdir(fds))) {
			if (fe->d_name[0] == '.') {
				continue;
			}
			char link[512], target[64];
			snprintf(link, sizeof(link), "%s/%s", path, fe->d_name);
			ssize_t len = readlink(link, target, sizeof(target) - 1);
			if (len <= 0) {
				continue;
			}
			target[len] = '\0';
			unsigned long inode;
			if (sscanf(target, "socket:[%lu]", &inode) != 1) {
				continue;
			}
			if (!bsearch(&inode, inodes, n, sizeof(*inodes), cmp_inode)) {
				continue;
			}
			bool known = false;
			for (int i = 0; i < *found && !known; i++) {
				known = owners[i].inode == inode;
			}
			if (known) {
				continue;
			}
			struct owner *o = &owners[(*found)++];
			o->inode = inode;
			o->pid = atoi(de->d_name);
			char comm[512];
			snprintf(comm, sizeof(comm), "%s/%s/comm", proc, de->d_name);
			FILE *f = fopen(comm, "r");
			if (f) {
				if (fgets(o->comm, sizeof(o->comm), f)) {
					o->comm[strcspn(o->comm, "\n")] = '\0';
				}
				fclose(f);
			}
			if (*found == n) {
				break;
			}
		}
		closedir(fds);
	}
	closedir(dir);
	return owners;
}

static int cmp_port(const void *a, const void *b) {
	const struct tw_port *x = a, *y = b;
	if (x->scope != y->scope) {
		return x->scope > y->scope ? -1 : 1; // public first
	}
	return x->port < y->port ? -1 : x->port > y->port;
}

static struct tw_port *port_entry(struct tw_ports *p, unsigned port) {
	for (int i = 0; i < p->count; i++) {
		if (p->items[i].port == port) {
			return &p->items[i];
		}
	}
	if (p->count == p->cap) {
		int cap = p->cap ? p->cap * 2 : 16;
		struct tw_port *items = realloc(p->items, cap * sizeof(*items));
		if (!items) {
			return NULL;
		}
		p->items = items;
		p->cap = cap;
	}
	struct tw_port *e = &p->items[p->count++];
	memset(e, 0, sizeof(*e));
	e->port = port;
	e->scope = TW_PORT_LOCAL;
	e->address[0] = '\0';
	return e;
}

void tw_ports_read_from(struct tw_ports *p, const char *proc, bool programs) {
	tw_ports_free(p);
	unsigned low, high;
	read_range(proc, &low, &high);
	struct socks socks = { 0 };
	read_table(proc, "tcp", true, &socks, low, high, &p->temporary);
	read_table(proc, "tcp6", true, &socks, low, high, &p->temporary);
	read_table(proc, "udp", false, &socks, low, high, &p->temporary);
	read_table(proc, "udp6", false, &socks, low, high, &p->temporary);

	struct owner *owners = NULL;
	int owner_count = 0;
	if (programs && socks.count > 0) {
		unsigned long *inodes = malloc(socks.count * sizeof(*inodes));
		int n = 0;
		for (int i = 0; inodes && i < socks.count; i++) {
			if (socks.items[i].inode) {
				inodes[n++] = socks.items[i].inode;
			}
		}
		owners = inodes ? find_owners(proc, inodes, n, &owner_count) : NULL;
		free(inodes);
	}
	p->programs = programs;

	for (int i = 0; i < socks.count; i++) {
		const struct sock *s = &socks.items[i];
		bool fresh = false;
		for (int k = 0; k < p->count && !fresh; k++) {
			fresh = p->items[k].port == s->port;
		}
		fresh = !fresh;
		struct tw_port *e = port_entry(p, s->port);
		if (!e) {
			break;
		}
		if (s->tcp) {
			e->tcp = true;
		} else {
			e->udp = true;
		}
		// the widest of its sockets says how far it can be reached
		if (fresh || s->scope > e->scope || (s->scope == e->scope && s->any && !e->any)) {
			e->scope = s->scope;
			e->any = s->any;
			snprintf(e->address, sizeof(e->address), "%s", s->address);
		}
		if (fresh) {
			e->uid = s->uid;
		}
		for (int k = 0; k < owner_count && !e->pid; k++) {
			if (owners[k].inode == s->inode) {
				e->pid = owners[k].pid;
				snprintf(e->program, sizeof(e->program), "%s", owners[k].comm);
			}
		}
	}
	free(owners);
	free(socks.items);

	for (int i = 0; i < p->count; i++) {
		struct tw_port *e = &p->items[i];
		struct servent *se = getservbyport(htons((uint16_t)e->port), e->tcp ? "tcp" : "udp");
		if (se && se->s_name) {
			snprintf(e->service, sizeof(e->service), "%s", se->s_name);
		}
		struct passwd *pw = getpwuid(e->uid);
		if (pw && pw->pw_name) {
			snprintf(e->user, sizeof(e->user), "%s", pw->pw_name);
		} else {
			snprintf(e->user, sizeof(e->user), "%u", e->uid);
		}
		switch (e->scope) {
		case TW_PORT_LOCAL: p->local++; break;
		case TW_PORT_LAN: p->lan++; break;
		case TW_PORT_PUBLIC: p->public_++; break;
		}
	}
	endservent();
	if (p->count > 1) {
		qsort(p->items, p->count, sizeof(*p->items), cmp_port);
	}
}

void tw_ports_read(struct tw_ports *p, bool programs) {
	tw_ports_read_from(p, "/proc", programs);
}

void tw_ports_free(struct tw_ports *p) {
	free(p->items);
	memset(p, 0, sizeof(*p));
}

const char *tw_port_scope_name(enum tw_port_scope scope) {
	switch (scope) {
	case TW_PORT_LOCAL: return "Local";
	case TW_PORT_LAN: return "LAN";
	case TW_PORT_PUBLIC: return "Public";
	}
	return "";
}

const char *tw_port_name(const struct tw_port *port, char *buf, size_t size) {
	if (port->program[0]) {
		snprintf(buf, size, "%s", port->program);
	} else if (port->service[0]) {
		snprintf(buf, size, "%s", port->service);
	} else {
		snprintf(buf, size, "port %u", port->port);
	}
	return buf;
}

const char *tw_port_protocols(const struct tw_port *port) {
	return port->tcp && port->udp ? "TCP+UDP" : port->udp ? "UDP" : "TCP";
}
