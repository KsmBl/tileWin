#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "backend.h"

#define GREETER_USER "tilewin-dm"

struct dm_backend {
	const struct dm_login *login;
	pam_handle_t *pam;
	struct pam_conv conv;
	char *user, *home;
	uid_t uid;
	gid_t gid;
	bool credentials, session;
	int last;
};

static char *error_text(const char *what, const struct dm_backend *b, int ret) {
	char *text = NULL;
	if (asprintf(&text, "%s: %s", what, pam_strerror(b->pam, ret)) < 0) {
		return strdup(what);
	}
	return text;
}

static int conversation(int count, const struct pam_message **messages,
		struct pam_response **responses, void *data) {
	struct dm_backend *b = data;
	struct pam_response *replies = calloc(count, sizeof(*replies));
	if (!replies) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < count; i++) {
		enum dm_prompt kind;
		switch (messages[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			kind = DM_PROMPT_SECRET;
			break;
		case PAM_PROMPT_ECHO_ON:
			kind = DM_PROMPT_VISIBLE;
			break;
		case PAM_ERROR_MSG:
			kind = DM_PROMPT_ERROR;
			break;
		default:
			kind = DM_PROMPT_INFO;
			break;
		}
		char *reply = NULL;
		bool asked = kind == DM_PROMPT_SECRET || kind == DM_PROMPT_VISIBLE;
		if (!b->login->conversation || !b->login->conversation(b->login->data, kind,
				messages[i]->msg, asked ? &reply : NULL)) {
			for (int j = 0; j < i; j++) {
				if (replies[j].resp) {
					explicit_bzero(replies[j].resp, strlen(replies[j].resp));
					free(replies[j].resp);
				}
			}
			free(replies);
			return PAM_CONV_ERR;
		}
		replies[i].resp = reply;
	}
	*responses = replies;
	return PAM_SUCCESS;
}

static void put_env(pam_handle_t *pam, const char *key, const char *value) {
	char *item = NULL;
	if (value && asprintf(&item, "%s=%s", key, value) >= 0) {
		pam_putenv(pam, item);
		free(item);
	}
}

struct dm_backend *dm_backend_start(const struct dm_login *login, char **error) {
	struct passwd *pw = getpwnam(login->user);
	if (!pw) {
		// the same words as a wrong password: this does not tell who exists
		*error = strdup("The user name or the password is incorrect");
		return NULL;
	}
	struct dm_backend *b = calloc(1, sizeof(*b));
	b->login = login;
	b->user = strdup(pw->pw_name);
	b->home = strdup(pw->pw_dir);
	b->uid = pw->pw_uid;
	b->gid = pw->pw_gid;
	b->conv.conv = conversation;
	b->conv.appdata_ptr = b;
	const char *service = login->kind == DM_GREETER ? "tilewin-dm-greeter" :
		login->kind == DM_AUTOLOGIN ? "tilewin-dm-autologin" : "tilewin-dm";
	int ret = pam_start(service, b->user, &b->conv, &b->pam);
	if (ret != PAM_SUCCESS) {
		*error = strdup("PAM could not start");
		free(b->user);
		free(b->home);
		free(b);
		return NULL;
	}
	char tty[32], vt[16];
	snprintf(tty, sizeof(tty), "/dev/tty%d", login->vt);
	snprintf(vt, sizeof(vt), "%d", login->vt);
	pam_set_item(b->pam, PAM_TTY, tty);
	// what pam_systemd makes a logind session on this seat and terminal out of
	put_env(b->pam, "XDG_SEAT", "seat0");
	put_env(b->pam, "XDG_VTNR", vt);
	put_env(b->pam, "XDG_SESSION_TYPE", login->session_type);
	put_env(b->pam, "XDG_SESSION_CLASS", login->session_class);
	put_env(b->pam, "XDG_SESSION_DESKTOP", login->session_desktop);
	put_env(b->pam, "XDG_CURRENT_DESKTOP", login->session_desktop);
	return b;
}

bool dm_backend_authenticate(struct dm_backend *b, char **error) {
	int ret = pam_authenticate(b->pam, 0);
	if (ret == PAM_SUCCESS) {
		ret = pam_acct_mgmt(b->pam, 0);
		if (ret == PAM_NEW_AUTHTOK_REQD) {
			ret = pam_chauthtok(b->pam, PAM_CHANGE_EXPIRED_AUTHTOK);
		}
	}
	b->last = ret;
	if (ret != PAM_SUCCESS) {
		bool plain = ret == PAM_AUTH_ERR || ret == PAM_USER_UNKNOWN || ret == PAM_CONV_ERR;
		*error = plain ? strdup("The user name or the password is incorrect") :
			strdup(pam_strerror(b->pam, ret));
		return false;
	}
	return true;
}

bool dm_backend_open(struct dm_backend *b, char **error) {
	// pam_setcred and pam_open_session of modules that look at groups need them
	if (initgroups(b->user, b->gid) != 0) {
		*error = strdup("Could not set the groups of the user");
		return false;
	}
	int ret = pam_setcred(b->pam, PAM_ESTABLISH_CRED);
	if (ret != PAM_SUCCESS) {
		*error = error_text("Could not set the credentials", b, ret);
		return false;
	}
	b->credentials = true;
	ret = pam_open_session(b->pam, 0);
	if (ret != PAM_SUCCESS) {
		*error = error_text("Could not open the session", b, ret);
		return false;
	}
	b->session = true;
	return true;
}

char **dm_backend_environment(struct dm_backend *b) {
	return pam_getenvlist(b->pam);
}

bool dm_backend_enter(struct dm_backend *b, char **error) {
	setsid();
	char tty[32];
	snprintf(tty, sizeof(tty), "/dev/tty%d", b->login->vt);
	int fd = open(tty, O_RDWR | O_NOCTTY);
	if (fd >= 0) {
		// a controlling terminal, as a program on a terminal has
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, STDIN_FILENO);
		if (fd > STDERR_FILENO) {
			close(fd);
		}
	}
	if (initgroups(b->user, b->gid) != 0 || setgid(b->gid) != 0 || setuid(b->uid) != 0) {
		*error = strdup("Could not become the user");
		return false;
	}
	if (setuid(0) == 0) { // must not be possible any more
		*error = strdup("Could not drop root");
		return false;
	}
	if (chdir(b->home) != 0 && chdir("/") != 0) {
		*error = strdup("Could not change the folder");
		return false;
	}
	return true;
}

void dm_backend_finish(struct dm_backend *b) {
	if (b->session) {
		pam_close_session(b->pam, 0);
	}
	if (b->credentials) {
		pam_setcred(b->pam, PAM_DELETE_CRED);
	}
	pam_end(b->pam, b->last);
	free(b->user);
	free(b->home);
	free(b);
}

void dm_backend_user_info(const char *user, char **home, char **shell) {
	struct passwd *pw = getpwnam(user);
	*home = pw && pw->pw_dir ? strdup(pw->pw_dir) : NULL;
	*shell = pw && pw->pw_shell && *pw->pw_shell ? strdup(pw->pw_shell) : NULL;
}

uid_t dm_backend_greeter_uid(void) {
	struct passwd *pw = getpwnam(GREETER_USER);
	return pw ? pw->pw_uid : (uid_t)-1;
}

gid_t dm_backend_greeter_gid(void) {
	struct passwd *pw = getpwnam(GREETER_USER);
	return pw ? pw->pw_gid : (gid_t)-1;
}

const char *dm_backend_greeter_user(void) {
	return GREETER_USER;
}

bool dm_backend_activate_vt(int vt) {
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		return false;
	}
	bool ok = ioctl(fd, VT_ACTIVATE, vt) == 0 && ioctl(fd, VT_WAITACTIVE, vt) == 0;
	close(fd);
	return ok;
}

bool dm_backend_is_root(const char *user) {
	struct passwd *pw = getpwnam(user);
	return pw && pw->pw_uid == 0;
}

void dm_backend_drop_to_user(const char *user) {
	struct passwd *pw = getpwnam(user);
	if (!pw || initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
			setuid(pw->pw_uid) != 0 || setuid(0) == 0) {
		_exit(1);
	}
}
