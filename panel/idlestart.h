#ifndef _TILEWIN_PANEL_IDLESTART_H
#define _TILEWIN_PANEL_IDLESTART_H
#include <stdbool.h>
#include "panel.h"

/*
 * Apps started in the background when the computer is idle (idlestart.c):
 * after logging in, once the processor and the disks have been quiet for a
 * while, each app of ~/.config/tileWin/idle-start is started minimized
 * ("exec --background"), one after the other, each when it is quiet again.
 */
void idlestart_init(struct panel *panel);
void idlestart_fini(void);
/* Whether the app of that desktop entry id is on the list. */
bool idlestart_listed(const char *id);
/* "panel idlestart on|off|toggle ID": puts it on the list or takes it off. */
void idlestart_command(int argc, char **argv);

#endif
