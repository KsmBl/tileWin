#ifndef _TILEWIN_DM_BACKEND_H
#define _TILEWIN_DM_BACKEND_H
#include <stdbool.h>
#include <sys/types.h>

/*
 * What differs between a real login and a test: PAM, user IDs and terminals.
 * backend_pam.c goes into tilewin-display-manager. The stand-in of the tests,
 * tests/dm_fake_backend.c, accepts a known password and is only ever linked
 * into a test program that is not installed, so that the real one has no way
 * round PAM at all.
 */
enum dm_kind {
	DM_LOGIN,     // a user with a password
	DM_AUTOLOGIN, // autologin_user
	DM_GREETER,   // the login screen itself, as its own user
};

enum dm_prompt {
	DM_PROMPT_SECRET,
	DM_PROMPT_VISIBLE,
	DM_PROMPT_INFO,
	DM_PROMPT_ERROR,
};

/* Asks the greeter. For a prompt reply gets an answer of the caller's; false to stop. */
typedef bool (*dm_conversation)(void *data, enum dm_prompt kind, const char *text, char **reply);

struct dm_login {
	enum dm_kind kind;
	const char *user;
	const char *session_type;    // "wayland"
	const char *session_desktop; // XDG_SESSION_DESKTOP, may be NULL
	const char *session_class;   // "user" or "greeter"
	int vt;
	dm_conversation conversation;
	void *data;
};

struct dm_backend;

/* NULL with *error set (newly allocated) when it cannot even start. */
struct dm_backend *dm_backend_start(const struct dm_login *login, char **error);
/* Password and account. False with *error set: not this time. */
bool dm_backend_authenticate(struct dm_backend *backend, char **error);
/* Credentials and the session, in the parent of what runs in it. */
bool dm_backend_open(struct dm_backend *backend, char **error);
/* The environment the session wants; NULL at the end, the caller frees all. */
char **dm_backend_environment(struct dm_backend *backend);
/* In the child: the terminal of the session, then this user for good. */
bool dm_backend_enter(struct dm_backend *backend, char **error);
/* Closes the session and ends all of it. */
void dm_backend_finish(struct dm_backend *backend);

/* Home and shell of a user; NULL where there is none. Newly allocated. */
void dm_backend_user_info(const char *user, char **home, char **shell);
/* Only a process of this user may be the greeter. */
uid_t dm_backend_greeter_uid(void);
gid_t dm_backend_greeter_gid(void);
const char *dm_backend_greeter_user(void);
/* Puts the terminal in front. */
bool dm_backend_activate_vt(int vt);
/* The user a user is; false where it does not exist. */
bool dm_backend_is_root(const char *user);
/* Runs fn in a child as this user, for reading their files. */
void dm_backend_drop_to_user(const char *user);

#endif
