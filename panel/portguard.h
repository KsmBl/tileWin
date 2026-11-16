#ifndef _TILEWIN_PANEL_PORTGUARD_H
#define _TILEWIN_PANEL_PORTGUARD_H
#include <stdbool.h>
#include "panel.h"

/*
 * The guard of the ports (portguard.c): asks before a program of the user can
 * be reached from other computers, and keeps the answer for the program.
 */
enum portguard_verdict {
	PG_NONE,    // not guarded: the guard is off, or not a port of the user's
	PG_ALLOWED, // the program was allowed
	PG_BLOCKED, // blocked: other computers cannot connect
	PG_ASKING,  // blocked while the question waits for an answer
};

void portguard_init(struct panel *panel);
void portguard_fini(void);
/* Whether the guard is on and works. */
bool portguard_active(void);
/* What the guard does to a port the user's program listens on. */
enum portguard_verdict portguard_verdict(unsigned port, bool tcp);
/* The name of the network the computer is on, NULL for none. */
const char *portguard_network_name(void);
/* "panel ports allow|block|once|network|forget PORT tcp|udp" for the program on that port. */
void portguard_command(int argc, char **argv);

#endif
