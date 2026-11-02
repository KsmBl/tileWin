#ifndef _TILEWIN_DM_SESSIONS_H
#define _TILEWIN_DM_SESSIONS_H
#include <stdbool.h>
#include <stddef.h>

/*
 * The sessions to log in to: the desktop entries of wayland-sessions. Read by
 * the greeter to offer them, and again by the daemon, which only ever starts what
 * such a file says and never a command from the greeter.
 */
struct dm_session {
	char *id;   // its file name without .desktop
	char *name;
	char *exec;
	char *desktop; // DesktopNames, for XDG_CURRENT_DESKTOP
};

struct dm_session_list {
	struct dm_session *items;
	size_t count;
};

/* The folders are TILEWIN_DM_SESSIONS (for the tests) or the usual ones. */
struct dm_session_list *dm_sessions_read(void);
void dm_sessions_free(struct dm_session_list *list);
const struct dm_session *dm_session_find(const struct dm_session_list *list, const char *id);

/* What Exec says as a shell command: field codes such as %U go. Newly allocated. */
char *dm_session_command(const char *exec);

/* A session id is a file name: letters, digits, and . _ - but no leading dot. */
bool dm_session_id_ok(const char *id);

#endif
