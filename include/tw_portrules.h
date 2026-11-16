#ifndef _TW_PORTRULES_H
#define _TW_PORTRULES_H
#include <stdbool.h>

/*
 * ~/.config/tileWin/ports.conf: whether tileWin asks before a program of the
 * user opens a port to the network, and the answers given, for each program,
 * everywhere or on one network only (a NetworkManager connection, by its
 * UUID; "network" lines keep its name for the settings):
 *
 *   guard yes
 *   network 0b6f...-...  Home
 *   allow /usr/bin/python3.14 -m http.server
 *   block /home/me/bin/server
 *   allow@0b6f...-... /usr/bin/syncthing
 *
 * The taskbar (portguard.c) and the Network page of the settings read and
 * write it with these; a write puts every line again, comments go.
 */

struct tw_port_rule {
	char *program;
	char *network; // the UUID of the network it holds on, NULL for everywhere
	bool allow;
};

struct tw_port_network {
	char *uuid, *name;
};

struct tw_port_rules {
	bool guard;
	struct tw_port_rule *items;
	int count, cap;
	struct tw_port_network *networks;
	int network_count, network_cap;
};

/* Reads the file; false when there is none (then the guard is off, no rules). */
bool tw_port_rules_read(struct tw_port_rules *r, const char *path);
/* Writes it, making its directory. */
bool tw_port_rules_write(const struct tw_port_rules *r, const char *path);
void tw_port_rules_free(struct tw_port_rules *r);
/* The answer for a program: the one of that network first, then the one for everywhere. */
const struct tw_port_rule *tw_port_rules_find(const struct tw_port_rules *r,
	const char *program, const char *network);
/* Sets the answer for a program on a network (NULL: everywhere), in place of one there. */
void tw_port_rules_set(struct tw_port_rules *r, const char *program, const char *network,
	bool allow);
/* Forgets the answer for a program on a network (NULL: the one for everywhere). */
void tw_port_rules_forget(struct tw_port_rules *r, const char *program, const char *network);
/* Keeps the name of a network for the settings. */
void tw_port_rules_name_network(struct tw_port_rules *r, const char *uuid, const char *name);
/* Its name, or NULL when unknown. */
const char *tw_port_rules_network_name(const struct tw_port_rules *r, const char *uuid);
/* "~/.config/tileWin/ports.conf" (to free), NULL without a home. */
char *tw_port_rules_path(void);

#endif
