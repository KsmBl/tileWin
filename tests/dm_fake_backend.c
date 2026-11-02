#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "backend.h"

/*
 * The stand-in for PAM, users and terminals in the test of the display manager.
 * It takes the password in TW_DM_FAKE_PASSWORD and writes down what it is asked
 * to do. It is linked into a test program only, never into what gets installed.
 *
 * TW_DM_FAKE_CODE asks for a second answer as well, as a one-time code would.
 */
struct dm_backend {
	const struct dm_login *login;
};

static void note(const char *word, const char *text) {
	const char *path = getenv("TW_DM_FAKE_LOG");
	FILE *f = path ? fopen(path, "a") : NULL;
	if (f) {
		fprintf(f, "%s %s\n", word, text ? text : "");
		fclose(f);
	}
}

struct dm_backend *dm_backend_start(const struct dm_login *login, char **error) {
	if (strcmp(login->user, "nobody-here") == 0) {
		*error = strdup("The user name or the password is incorrect");
		return NULL;
	}
	struct dm_backend *b = calloc(1, sizeof(*b));
	b->login = login;
	return b;
}

bool dm_backend_authenticate(struct dm_backend *b, char **error) {
	if (b->login->kind != DM_LOGIN) {
		return true;
	}
	char *answer = NULL;
	if (!b->login->conversation(b->login->data, DM_PROMPT_SECRET, "Password:", &answer)) {
		*error = strdup("The user name or the password is incorrect");
		return false;
	}
	const char *want = getenv("TW_DM_FAKE_PASSWORD");
	bool right = want && strcmp(answer, want) == 0;
	free(answer);
	const char *code = getenv("TW_DM_FAKE_CODE");
	if (right && code) {
		answer = NULL;
		if (!b->login->conversation(b->login->data, DM_PROMPT_VISIBLE, "One-time code:", &answer)) {
			*error = strdup("Stopped");
			return false;
		}
		right = strcmp(answer, code) == 0;
		free(answer);
	}
	if (!right) {
		*error = strdup("The user name or the password is incorrect");
		return false;
	}
	note("authenticated", b->login->user);
	return true;
}

bool dm_backend_open(struct dm_backend *b, char **error) {
	note("open", b->login->kind == DM_GREETER ? "greeter" : b->login->user);
	return true;
}

/* What is passed on, without what would send the greeter to the test's own folders. */
char **dm_backend_environment(struct dm_backend *b) {
	extern char **environ;
	size_t count = 0;
	while (environ[count]) {
		count++;
	}
	char **env = calloc(count + 8, sizeof(char *));
	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		if (strncmp(environ[i], "XDG_CONFIG_HOME=", 16) != 0 &&
				strncmp(environ[i], "XDG_STATE_HOME=", 15) != 0 &&
				strncmp(environ[i], "XDG_CACHE_HOME=", 15) != 0 &&
				strncmp(environ[i], "HOME=", 5) != 0 &&
				strncmp(environ[i], "PATH=", 5) != 0 &&
				strncmp(environ[i], "USER=", 5) != 0) {
			env[n++] = strdup(environ[i]);
		}
	}
	char *line = NULL;
	if (asprintf(&line, "XDG_SESSION_CLASS=%s", b->login->session_class) >= 0) {
		env[n++] = line;
	}
	if (asprintf(&line, "XDG_SESSION_DESKTOP=%s", b->login->session_desktop) >= 0) {
		env[n++] = line;
	}
	return env;
}

bool dm_backend_enter(struct dm_backend *b, char **error) {
	setsid();
	return true;
}

void dm_backend_finish(struct dm_backend *b) {
	note("close", b->login->kind == DM_GREETER ? "greeter" : b->login->user);
	free(b);
}

void dm_backend_user_info(const char *user, char **home, char **shell) {
	const char *fake = getenv("TW_DM_FAKE_HOME");
	*home = strdup(fake ? fake : "/");
	// as the real one: the user of the greeter may not log in, a person may
	*shell = strdup(strcmp(user, "greeter") == 0 ? "/usr/bin/nologin" : "/bin/sh");
}

uid_t dm_backend_greeter_uid(void) {
	return getuid();
}

gid_t dm_backend_greeter_gid(void) {
	return getgid();
}

const char *dm_backend_greeter_user(void) {
	return "greeter";
}

bool dm_backend_activate_vt(int vt) {
	char text[16];
	snprintf(text, sizeof(text), "%d", vt);
	note("vt", text);
	return true;
}

bool dm_backend_is_root(const char *user) {
	return strcmp(user, "root") == 0;
}

void dm_backend_drop_to_user(const char *user) {
}
