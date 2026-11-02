#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "backend.h"
#include "config.h"
#include "sessions.h"

/*
 * tilewin-display-manager: the login screen of tileWin.
 *
 * This program runs as root and stays small: no toolkit, no drawing. It starts
 * the compositor and the greeter as a user of their own, in a logind session of
 * class greeter, and talks to the greeter over a socket only that user may
 * connect to. What the greeter sends is never trusted: a user name goes to PAM,
 * a session is looked up among the session files, a password is only passed on.
 *
 * Each login attempt is a child process, the worker, which holds PAM from the
 * first prompt to the end of the session, so a stuck PAM module or a crash cannot
 * take the daemon with it:
 *
 *   greeter -> "login"   a worker starts, and talks to the greeter itself
 *   worker  -> 'A'       the password was right: the daemon ends the greeter
 *   daemon  -> 'G'       the terminal is free: the worker opens the session
 *   worker exits         the session is over: the greeter comes back
 *
 * Messages are datagrams of a sequenced socket: a word, a space, and the rest.
 *   greeter: login <user>\n<session>   answer <text>   cancel   power <what>
 *   daemon:  secret <prompt>   visible <prompt>   info <text>   error <text>
 *            failed <text>   ok   refused <text>
 */
#define MESSAGE_MAX 4096
#define GREETER_RETRY_MS 1000
#define GREETER_MAX_QUICK_DEATHS 5

#ifndef TILEWIN_COMPOSITOR
#define TILEWIN_COMPOSITOR "/usr/local/bin/tilewin"
#endif
#ifndef TILEWIN_GREETER
#define TILEWIN_GREETER "/usr/local/bin/tilewin-dm-greeter"
#endif

static struct {
	struct dm_config *config;
	const char *config_path;
	const char *state_dir, *run_dir, *compositor, *greeter;
	int vt;
	bool once;
	char socket_path[256];
	int listen_fd, greeter_fd;
	pid_t greeter_pid, worker_pid;
	int worker_ctl; // the daemon's end of the pipe pair with the worker
	char pending_user[256], pending_session[256]; // remembered once the password was right
} dm = {
	.state_dir = "/var/lib/tilewin-dm",
	.run_dir = "/run/tilewin-dm",
	.compositor = TILEWIN_COMPOSITOR,
	.greeter = TILEWIN_GREETER,
	.listen_fd = -1,
	.greeter_fd = -1,
	.worker_ctl = -1,
};

static volatile sig_atomic_t got_term;

static void on_term(int sig) {
	got_term = 1;
}

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "tilewin-display-manager: ");
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
}

static void send_message(int fd, const char *word, const char *text) {
	char buffer[MESSAGE_MAX];
	int n = snprintf(buffer, sizeof(buffer), "%s%s%s", word, text ? " " : "", text ? text : "");
	if (n < 0) {
		return;
	}
	if (n >= (int)sizeof(buffer)) {
		n = sizeof(buffer) - 1;
	}
	if (send(fd, buffer, n, MSG_NOSIGNAL) < 0 && errno != EPIPE) {
		logmsg("cannot talk to the greeter: %s", strerror(errno));
	}
}

/* ---------- files of the state folder ---------- */

/* Root writes these, in a folder only root can write to. */
static void write_state(const char *dir, const char *name, const char *text) {
	char path[512], temp[520];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(temp, sizeof(temp), "%s.new", path);
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0) {
		return;
	}
	size_t len = strlen(text);
	bool ok = write(fd, text, len) == (ssize_t)len;
	close(fd);
	if (ok) {
		rename(temp, path);
	} else {
		unlink(temp);
	}
}

static void make_dir(const char *path, mode_t mode) {
	if (mkdir(path, mode) != 0 && errno != EEXIST) {
		logmsg("cannot make %s: %s", path, strerror(errno));
	}
}

static char *shared_config_dir(void) {
	char *path = NULL;
	if (asprintf(&path, "%s/shared/config/tileWin", dm.state_dir) < 0) {
		abort();
	}
	return path;
}

/*
 * The greeter's own folders: what it may write is in home. What it only reads is
 * in shared, which root writes, so that the greeter cannot point root anywhere
 * with a link.
 */
static void prepare_folders(void) {
	char path[512];
	make_dir(dm.state_dir, 0755);
	snprintf(path, sizeof(path), "%s/shared", dm.state_dir);
	make_dir(path, 0755);
	snprintf(path, sizeof(path), "%s/shared/config", dm.state_dir);
	make_dir(path, 0755);
	snprintf(path, sizeof(path), "%s/shared/config/tileWin", dm.state_dir);
	make_dir(path, 0755);
	snprintf(path, sizeof(path), "%s/home", dm.state_dir);
	make_dir(path, 0700);
	if (geteuid() == 0 && lchown(path, dm_backend_greeter_uid(), dm_backend_greeter_gid()) != 0) {
		logmsg("cannot give %s to the greeter: %s", path, strerror(errno));
	}
	make_dir(dm.run_dir, 0755);
}

/* A name of a theme, or a word of a color scheme: nothing a path can be made of. */
static bool plain_word(const char *text) {
	if (!*text || strlen(text) > 64 || *text == '.') {
		return false;
	}
	for (const char *p = text; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
			return false;
		}
	}
	return true;
}

/*
 * The theme of who logged in, for the login screen to wear next time. Their home
 * may be closed to the greeter, and root must not follow links in it, so a child
 * that is them reads it, and only plain words are taken from what it says.
 */
static void remember_look(const char *user) {
	char *home = NULL, *shell = NULL;
	dm_backend_user_info(user, &home, &shell);
	free(shell);
	if (!home) {
		return;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		free(home);
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		dm_backend_drop_to_user(user);
		static const char *const names[] = { "current-theme", "color-scheme" };
		for (int i = 0; i < 2; i++) {
			char path[1024], line[80] = "";
			snprintf(path, sizeof(path), "%s/.config/tileWin/%s", home, names[i]);
			FILE *f = fopen(path, "r");
			if (f) {
				if (!fgets(line, sizeof(line), f)) {
					line[0] = '\0';
				}
				fclose(f);
			}
			line[strcspn(line, "\r\n \t")] = '\0';
			if (dprintf(fds[1], "%s\n", line) < 0) {
				_exit(1);
			}
		}
		_exit(0);
	}
	close(fds[1]);
	free(home);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	char buffer[256] = "";
	size_t used = 0;
	struct pollfd p = { fds[0], POLLIN, 0 };
	// a home on a dead network folder may not hold up the login
	while (used + 1 < sizeof(buffer) && poll(&p, 1, 2000) > 0) {
		ssize_t n = read(fds[0], buffer + used, sizeof(buffer) - 1 - used);
		if (n <= 0) {
			break;
		}
		used += n;
	}
	buffer[used] = '\0';
	close(fds[0]);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	char *save = NULL;
	char *theme = strtok_r(buffer, "\n", &save);
	char *scheme = theme ? strtok_r(NULL, "\n", &save) : NULL;
	char file[64], *dir = NULL;
	if (asprintf(&dir, "%s/shared", dm.state_dir) < 0) {
		return;
	}
	if (theme && plain_word(theme)) {
		snprintf(file, sizeof(file), "%s\n", theme);
		write_state(dir, "last-theme", file);
	}
	if (scheme && (strcmp(scheme, "dark") == 0 || strcmp(scheme, "light") == 0)) {
		snprintf(file, sizeof(file), "%s\n", scheme);
		write_state(dir, "last-scheme", file);
	}
	free(dir);
}

static void read_state_word(const char *name, char *out, size_t size) {
	char path[512];
	snprintf(path, sizeof(path), "%s/shared/%s", dm.state_dir, name);
	FILE *f = fopen(path, "r");
	out[0] = '\0';
	if (f) {
		if (!fgets(out, size, f)) {
			out[0] = '\0';
		}
		fclose(f);
	}
	out[strcspn(out, "\r\n")] = '\0';
}

/* What the compositor and the greeter find in their config folder. */
static void write_look(void) {
	char word[80];
	char *dir = shared_config_dir();
	const char *theme = dm_config_str(dm.config, "theme");
	if (strcmp(theme, "follow") == 0) {
		read_state_word("last-theme", word, sizeof(word));
		theme = word[0] && plain_word(word) ? word : "win10";
	}
	char line[96];
	snprintf(line, sizeof(line), "%s\n", plain_word(theme) ? theme : "win10");
	write_state(dir, "current-theme", line);
	const char *scheme = dm_config_str(dm.config, "color_scheme");
	if (strcmp(scheme, "follow") == 0) {
		read_state_word("last-scheme", word, sizeof(word));
		scheme = strcmp(word, "dark") == 0 ? "dark" : "light";
	}
	snprintf(line, sizeof(line), "%s\n", scheme);
	write_state(dir, "color-scheme", line);
	free(dir);
}

/* A value goes in the compositor's config in quotes; only what cannot break out. */
static const char *safe_for_config(const char *text) {
	for (const char *p = text; *p; p++) {
		if (!isalnum((unsigned char)*p) && !strchr(" ,:_+.-()", *p)) {
			return "";
		}
	}
	return text;
}

/* The compositor under the greeter: nothing of a desktop, just screens and keys. */
static bool write_compositor_config(char *path, size_t size) {
	snprintf(path, size, "%s/compositor.conf", dm.run_dir);
	char temp[320];
	snprintf(temp, sizeof(temp), "%s.new", path);
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		logmsg("cannot write %s: %s", temp, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	const struct dm_config *c = dm.config;
	fprintf(f, "xwayland disable\nsession_restore no\nremember_windows no\n"
		"panel_command none\nanimations off\nbattery_saver off\nbattery_warning off\n"
		"focus_follows_mouse no\npolkit_agent no\nwallpaper solid #000000\n");
	fprintf(f, "input type:keyboard {\n");
	if (*dm_config_str(c, "keyboard_layout")) {
		fprintf(f, "\txkb_layout \"%s\"\n", safe_for_config(dm_config_str(c, "keyboard_layout")));
	}
	if (*dm_config_str(c, "keyboard_variant")) {
		fprintf(f, "\txkb_variant \"%s\"\n", safe_for_config(dm_config_str(c, "keyboard_variant")));
	}
	if (*dm_config_str(c, "keyboard_options")) {
		fprintf(f, "\txkb_options \"%s\"\n", safe_for_config(dm_config_str(c, "keyboard_options")));
	}
	fprintf(f, "\txkb_numlock %s\n\trepeat_delay %d\n\trepeat_rate %d\n}\n",
		dm_config_bool(c, "numlock") ? "enabled" : "disabled",
		dm_config_int(c, "repeat_delay"), dm_config_int(c, "repeat_rate"));
	fprintf(f, "input type:touchpad {\n\ttap %s\n}\n",
		dm_config_bool(c, "tap_to_click") ? "enabled" : "disabled");
	const char *cursor = safe_for_config(dm_config_str(c, "cursor_theme"));
	if (*cursor) {
		fprintf(f, "seat seat0 xcursor_theme \"%s\" %d\n", cursor, dm_config_int(c, "cursor_size"));
	}
	if (dm_config_bool(c, "hide_cursor")) {
		fprintf(f, "seat seat0 hide_cursor 1000\n");
	}
	const char *scale = dm_config_str(c, "screen_scale");
	if (strcmp(scale, "auto") != 0 && atof(scale) >= 0.5 && atof(scale) <= 4) {
		fprintf(f, "output * scale %.3f\n", atof(scale));
	}
	fprintf(f, "exec %s --socket %s --config %s\n", dm.greeter, dm.socket_path, dm.config_path);
	bool ok = fclose(f) == 0;
	if (ok) {
		ok = rename(temp, path) == 0;
	}
	return ok;
}

/* ---------- running something in a session ---------- */

static char **environment_for(struct dm_backend *backend, const char *user, const char *home,
		const char *shell, bool greeter) {
	char **pam = dm_backend_environment(backend);
	size_t count = 0;
	while (pam && pam[count]) {
		count++;
	}
	size_t extra = dm.config->env_count + 16;
	char **env = calloc(count + extra, sizeof(char *));
	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		env[n++] = pam[i];
	}
	free(pam);
	if (asprintf(&env[n++], "HOME=%s", home) < 0 || asprintf(&env[n++], "USER=%s", user) < 0 ||
			asprintf(&env[n++], "LOGNAME=%s", user) < 0 ||
			asprintf(&env[n++], "SHELL=%s", shell) < 0 ||
			asprintf(&env[n++], "PATH=%s", dm_config_str(dm.config, "session_path")) < 0) {
		abort();
	}
	if (greeter) {
		char *shared = NULL, *own = NULL;
		if (asprintf(&shared, "%s/shared/config", dm.state_dir) < 0 ||
				asprintf(&own, "%s/home", dm.state_dir) < 0 ||
				asprintf(&env[n++], "XDG_CONFIG_HOME=%s", shared) < 0 ||
				asprintf(&env[n++], "XDG_STATE_HOME=%s/state", own) < 0 ||
				asprintf(&env[n++], "XDG_CACHE_HOME=%s/cache", own) < 0 ||
				asprintf(&env[n++], "TILEWIN_DM_STATE=%s/shared", dm.state_dir) < 0) {
			abort();
		}
		env[n++] = strdup("TILEWIN_NO_APP_TWEAKS=1");
		free(shared);
		free(own);
	}
	for (size_t i = 0; i < dm.config->env_count; i++) {
		env[n++] = strdup(dm.config->env[i]);
	}
	env[n] = NULL;
	return env;
}

static pid_t worker_child; // what runs in the session, for the signal below

static void forward_term(int sig) {
	if (worker_child > 0) {
		kill(-worker_child, SIGTERM);
	}
}

/* Starts the command in the open session and waits for its end. Runs in the worker. */
static int run_in_session(struct dm_backend *backend, const char *user, const char *command,
		bool greeter) {
	char *home = NULL, *shell = NULL;
	dm_backend_user_info(user, &home, &shell);
	if (!home) {
		home = strdup("/");
	}
	// the greeter's user has no login shell, and must not have one: nologin refuses
	if (!shell || greeter) {
		free(shell);
		shell = strdup("/bin/sh");
	}
	char **env = environment_for(backend, user, home, shell, greeter);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGTERM, forward_term);
	pid_t pid = fork();
	if (pid < 0) {
		return 1;
	}
	if (pid == 0) {
		signal(SIGTERM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		char *error = NULL;
		if (!dm_backend_enter(backend, &error)) {
			fprintf(stderr, "tilewin-display-manager: %s\n", error ? error : "cannot enter");
			_exit(127);
		}
		// where a session writes to: the file only they can read, not the terminal
		if (!greeter) {
			char log[1024];
			snprintf(log, sizeof(log), "%s/.cache/tilewin-dm-session.log", home);
			int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd >= 0) {
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
			}
		}
		// a login shell, so that the profile of the user counts as ever
		const char *base = strrchr(shell, '/');
		char *argv0 = NULL;
		if (asprintf(&argv0, "%s%s", greeter ? "" : "-", base ? base + 1 : shell) < 0) {
			_exit(127);
		}
		char *exec_line = NULL;
		if (asprintf(&exec_line, "exec %s", command) < 0) {
			_exit(127);
		}
		char *argv[] = { argv0, (char *)"-c", exec_line, NULL };
		execve(shell, argv, env);
		fprintf(stderr, "tilewin-display-manager: cannot start %s: %s\n", shell, strerror(errno));
		_exit(127);
	}
	worker_child = pid;
	setpgid(pid, pid);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	worker_child = 0;
	for (size_t i = 0; env[i]; i++) {
		free(env[i]);
	}
	free(env);
	free(home);
	free(shell);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ---------- the worker of a login ---------- */

struct conversation_state {
	int fd;
};

/* Sends a prompt to the greeter and, for a question, waits for what comes back. */
static bool talk_to_greeter(void *data, enum dm_prompt kind, const char *text, char **reply) {
	struct conversation_state *state = data;
	static const char *const words[] = { "secret", "visible", "info", "error" };
	send_message(state->fd, words[kind], text ? text : "");
	if (!reply) {
		return true;
	}
	char buffer[MESSAGE_MAX + 1];
	for (;;) {
		ssize_t n = recv(state->fd, buffer, MESSAGE_MAX, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false; // the greeter is gone
		}
		buffer[n] = '\0';
		if (strncmp(buffer, "answer ", 7) == 0) {
			*reply = strdup(buffer + 7);
			explicit_bzero(buffer, sizeof(buffer));
			return *reply != NULL;
		}
		if (strcmp(buffer, "cancel") == 0) {
			explicit_bzero(buffer, sizeof(buffer));
			return false;
		}
		// nothing else counts here, also not a second login
	}
}

/* Everything of a login, from the first prompt to the end of the session. */
static void login_worker(int greeter_fd, int ctl, const char *user, const struct dm_session *session,
		enum dm_kind kind) {
	signal(SIGCHLD, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	struct conversation_state state = { greeter_fd };
	struct dm_login login = {
		.kind = kind,
		.user = user,
		.session_type = "wayland",
		.session_desktop = session->desktop ? session->desktop : session->id,
		.session_class = "user",
		.vt = dm.vt,
		.conversation = greeter_fd >= 0 ? talk_to_greeter : NULL,
		.data = &state,
	};
	char *error = NULL;
	struct dm_backend *backend = dm_backend_start(&login, &error);
	bool ok = backend != NULL;
	if (ok) {
		ok = dm_backend_authenticate(backend, &error);
	}
	if (!ok) {
		sleep((unsigned)dm_config_int(dm.config, "fail_delay"));
		if (greeter_fd >= 0) {
			send_message(greeter_fd, "failed", error ? error : "It did not work");
		} else {
			logmsg("automatic login of %s: %s", user, error ? error : "failed");
		}
		_exit(1);
	}
	// the password was right: the greeter gives up the screen before anything starts
	char word = 'A', go = 0;
	if (greeter_fd >= 0) {
		send_message(greeter_fd, "ok", NULL);
	}
	if (write(ctl, &word, 1) != 1 || read(ctl, &go, 1) != 1 || go != 'G') {
		dm_backend_finish(backend);
		_exit(1);
	}
	if (greeter_fd >= 0) {
		close(greeter_fd);
	}
	if (!dm_backend_open(backend, &error)) {
		logmsg("session of %s: %s", user, error ? error : "failed");
		dm_backend_finish(backend);
		_exit(2);
	}
	remember_look(user);
	char *command = dm_session_command(session->exec);
	logmsg("%s logs in to %s", user, session->id);
	int code = run_in_session(backend, user, command, false);
	logmsg("the session of %s ended (%d)", user, code);
	free(command);
	remember_look(user); // the theme may have changed while it ran
	dm_backend_finish(backend);
	_exit(0);
}

static void remember_login(const char *user, const char *session) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/shared", dm.state_dir);
	if (dm_config_bool(dm.config, "remember_user")) {
		char line[300];
		snprintf(line, sizeof(line), "%s\n", user);
		write_state(dir, "last-user", line);
	}
	if (dm_config_bool(dm.config, "remember_session")) {
		char line[300];
		snprintf(line, sizeof(line), "%s\n", session);
		write_state(dir, "last-session", line);
	}
}

/* ---------- the greeter ---------- */

static void stop_process(pid_t pid) {
	if (pid <= 0) {
		return;
	}
	kill(pid, SIGTERM);
	for (int i = 0; i < 40; i++) {
		pid_t done = waitpid(pid, NULL, WNOHANG);
		if (done == pid || (done < 0 && errno != EINTR)) {
			return;
		}
		struct timespec wait = { 0, 50000000 };
		nanosleep(&wait, NULL);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

static void stop_greeter(void) {
	if (dm.greeter_fd >= 0) {
		close(dm.greeter_fd);
		dm.greeter_fd = -1;
	}
	stop_process(dm.greeter_pid);
	dm.greeter_pid = 0;
}

/* The compositor as the greeter's user, in a session of class greeter. */
static void greeter_worker(const char *config_file) {
	signal(SIGCHLD, SIG_DFL);
	close(dm.listen_fd);
	struct dm_login login = {
		.kind = DM_GREETER,
		.user = dm_backend_greeter_user(),
		.session_type = "wayland",
		.session_desktop = "tilewin-dm",
		.session_class = "greeter",
		.vt = dm.vt,
	};
	char *error = NULL;
	struct dm_backend *backend = dm_backend_start(&login, &error);
	if (!backend || !dm_backend_authenticate(backend, &error) || !dm_backend_open(backend, &error)) {
		logmsg("the session of the greeter: %s", error ? error : "failed");
		_exit(1);
	}
	char *command = NULL;
	if (asprintf(&command, "%s -c %s", dm.compositor, config_file) < 0) {
		_exit(1);
	}
	int code = run_in_session(backend, login.user, command, true);
	dm_backend_finish(backend);
	_exit(code);
}

static bool start_greeter(void) {
	write_look();
	char config_file[300];
	if (!write_compositor_config(config_file, sizeof(config_file))) {
		return false;
	}
	dm_backend_activate_vt(dm.vt);
	pid_t pid = fork();
	if (pid < 0) {
		return false;
	}
	if (pid == 0) {
		greeter_worker(config_file);
	}
	dm.greeter_pid = pid;
	return true;
}

static void make_socket(void) {
	snprintf(dm.socket_path, sizeof(dm.socket_path), "%s/greeter.sock", dm.run_dir);
	unlink(dm.socket_path);
	dm.listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", dm.socket_path);
	mode_t old = umask(0077);
	if (dm.listen_fd < 0 || bind(dm.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
			listen(dm.listen_fd, 2) != 0) {
		logmsg("cannot make %s: %s", dm.socket_path, strerror(errno));
		exit(1);
	}
	umask(old);
	if (geteuid() == 0 && chown(dm.socket_path, dm_backend_greeter_uid(),
			dm_backend_greeter_gid()) != 0) {
		logmsg("cannot give the socket to the greeter: %s", strerror(errno));
	}
}

static void accept_greeter(void) {
	int fd = accept4(dm.listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0) {
		return;
	}
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
			cred.uid != dm_backend_greeter_uid()) {
		logmsg("refused a greeter of user %d", (int)cred.uid);
		close(fd);
		return;
	}
	if (dm.greeter_fd >= 0) {
		close(dm.greeter_fd); // the newest one counts
	}
	dm.greeter_fd = fd;
}

/* ---------- what the greeter asks for ---------- */

static void do_power(const char *what) {
	static const struct {
		const char *word, *allow, *verb;
	} powers[] = {
		{ "poweroff", "allow_poweroff", "poweroff" },
		{ "reboot", "allow_reboot", "reboot" },
		{ "suspend", "allow_suspend", "suspend" },
		{ "hibernate", "allow_hibernate", "hibernate" },
	};
	for (size_t i = 0; i < sizeof(powers) / sizeof(powers[0]); i++) {
		if (strcmp(what, powers[i].word) != 0) {
			continue;
		}
		if (!dm_config_bool(dm.config, "show_power") ||
				!dm_config_bool(dm.config, powers[i].allow)) {
			send_message(dm.greeter_fd, "refused", "This is turned off");
			return;
		}
		pid_t pid = fork();
		if (pid == 0) {
			signal(SIGCHLD, SIG_DFL);
			execlp("systemctl", "systemctl", powers[i].verb, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
			}
		}
		return;
	}
}

/* Starts a worker for this user and session. Returns false if it could not. */
static bool start_login(const char *user, const char *session_id, enum dm_kind kind) {
	struct dm_session_list *sessions = dm_sessions_read();
	const struct dm_session *session = dm_session_id_ok(session_id) ?
		dm_session_find(sessions, session_id) : NULL;
	if (!session) {
		if (dm.greeter_fd >= 0) {
			send_message(dm.greeter_fd, "failed", "This session is not installed");
		}
		logmsg("no session %s", session_id);
		dm_sessions_free(sessions);
		return false;
	}
	if (dm_backend_is_root(user) && !dm_config_bool(dm.config, "allow_root")) {
		if (dm.greeter_fd >= 0) {
			send_message(dm.greeter_fd, "failed", "The user name or the password is incorrect");
		}
		dm_sessions_free(sessions);
		return false;
	}
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		dm_sessions_free(sessions);
		return false;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pair[0]);
		close(pair[1]);
		dm_sessions_free(sessions);
		return false;
	}
	if (pid == 0) {
		close(pair[0]);
		close(dm.listen_fd);
		login_worker(kind == DM_LOGIN ? dm.greeter_fd : -1, pair[1], user, session, kind);
		_exit(1);
	}
	close(pair[1]);
	dm.worker_pid = pid;
	dm.worker_ctl = pair[0];
	snprintf(dm.pending_user, sizeof(dm.pending_user), "%s", user);
	snprintf(dm.pending_session, sizeof(dm.pending_session), "%s", session_id);
	dm_sessions_free(sessions);
	return true;
}

/*
 * One message of the greeter, when no login is under way. Passwords only ever
 * pass through the worker, which reads from the same socket.
 */
static void greeter_said(const char *text) {
	if (strncmp(text, "login ", 6) == 0) {
		char user[256], session[256];
		const char *newline = strchr(text + 6, '\n');
		if (!newline || (size_t)(newline - text - 6) >= sizeof(user) ||
				strlen(newline + 1) >= sizeof(session)) {
			send_message(dm.greeter_fd, "failed", "That cannot be");
			return;
		}
		memcpy(user, text + 6, newline - text - 6);
		user[newline - text - 6] = '\0';
		snprintf(session, sizeof(session), "%s", newline + 1);
		if (!*user) {
			send_message(dm.greeter_fd, "failed", "Type a user name");
			return;
		}
		start_login(user, session, DM_LOGIN);
	} else if (strncmp(text, "power ", 6) == 0) {
		do_power(text + 6);
	}
}

/* The worker said the password was right, or it ended by itself. */
static void worker_said(void) {
	char word = 0;
	ssize_t n = read(dm.worker_ctl, &word, 1);
	if (n == 1 && word == 'A') {
		remember_login(dm.pending_user, dm.pending_session);
		stop_greeter();
		dm_backend_activate_vt(dm.vt);
		char go = 'G';
		if (write(dm.worker_ctl, &go, 1) != 1) {
			logmsg("the worker went away");
		}
	} else {
		close(dm.worker_ctl);
		dm.worker_ctl = -1;
	}
}

static void wait_for_worker(void) {
	if (dm.worker_pid > 0) {
		waitpid(dm.worker_pid, NULL, 0);
		dm.worker_pid = 0;
	}
	if (dm.worker_ctl >= 0) {
		close(dm.worker_ctl);
		dm.worker_ctl = -1;
	}
}

static void autologin_if_set(void) {
	const char *user = dm_config_str(dm.config, "autologin_user");
	if (!*user) {
		return;
	}
	const char *session = dm_config_str(dm.config, "autologin_session");
	if (!*session) {
		session = dm_config_str(dm.config, "default_session");
	}
	bool once = dm_config_bool(dm.config, "autologin_once");
	char marker[320];
	snprintf(marker, sizeof(marker), "%s/autologin-done", dm.run_dir);
	if (once && access(marker, F_OK) == 0) {
		return; // /run is empty after every start of the computer
	}
	int fd = open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) {
		close(fd);
	}
	dm_backend_activate_vt(dm.vt);
	if (!start_login(user, session, DM_AUTOLOGIN)) {
		return;
	}
	// nobody else wants the screen: 'A' is answered, and the session is waited for
	while (!got_term && dm.worker_ctl >= 0) {
		struct pollfd p = { dm.worker_ctl, POLLIN, 0 };
		if (poll(&p, 1, -1) > 0) {
			worker_said();
		}
	}
	if (got_term && dm.worker_pid > 0) {
		kill(dm.worker_pid, SIGTERM);
	}
	wait_for_worker();
}

/* Until the session of a user ended, or forever when the greeter keeps failing. */
static void login_screen_until_session_ended(void) {
	int quick_deaths = 0;
	struct timespec started;
	clock_gettime(CLOCK_MONOTONIC, &started);
	if (!start_greeter()) {
		sleep(1);
		return;
	}
	while (!got_term) {
		struct pollfd fds[3] = {
			{ dm.listen_fd, POLLIN, 0 },
			{ dm.worker_pid > 0 ? -1 : dm.greeter_fd, POLLIN, 0 },
			{ dm.worker_ctl, POLLIN, 0 },
		};
		int ready = poll(fds, 3, 500);
		if (ready < 0 && errno != EINTR) {
			break;
		}
		if (ready > 0 && (fds[0].revents & POLLIN)) {
			accept_greeter();
		}
		if (ready > 0 && fds[1].fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
			char buffer[MESSAGE_MAX + 1];
			ssize_t n = recv(dm.greeter_fd, buffer, MESSAGE_MAX, MSG_DONTWAIT);
			if (n > 0) {
				buffer[n] = '\0';
				greeter_said(buffer);
				explicit_bzero(buffer, sizeof(buffer));
			} else if (n == 0) {
				close(dm.greeter_fd);
				dm.greeter_fd = -1;
			}
		}
		if (ready > 0 && fds[2].fd >= 0 && (fds[2].revents & (POLLIN | POLLHUP))) {
			worker_said();
		}
		if (dm.worker_pid > 0) {
			pid_t done = waitpid(dm.worker_pid, NULL, WNOHANG);
			if (done == dm.worker_pid) {
				bool had_session = dm.greeter_pid == 0;
				dm.worker_pid = 0;
				if (dm.worker_ctl >= 0) {
					close(dm.worker_ctl);
					dm.worker_ctl = -1;
				}
				if (had_session) {
					return;
				}
			}
		}
		// the greeter died by itself: again, but not forever at full speed
		if (dm.greeter_pid > 0) {
			pid_t done = waitpid(dm.greeter_pid, NULL, WNOHANG);
			if (done == dm.greeter_pid) {
				dm.greeter_pid = 0;
				if (dm.greeter_fd >= 0) {
					close(dm.greeter_fd);
					dm.greeter_fd = -1;
				}
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				quick_deaths = now.tv_sec - started.tv_sec < 10 ? quick_deaths + 1 : 0;
				if (quick_deaths >= GREETER_MAX_QUICK_DEATHS) {
					logmsg("the greeter keeps failing: giving up, so that this is seen");
					exit(1);
				}
				logmsg("the greeter ended; starting it again");
				struct timespec wait = { GREETER_RETRY_MS / 1000, 0 };
				nanosleep(&wait, NULL);
				started = now;
				if (dm.worker_pid == 0 && !start_greeter()) {
					return;
				}
			}
		}
	}
}

/* ---------- main ---------- */

static void report_problem(const char *problem, void *data) {
	printf("%s\n", problem);
}

static void usage(void) {
	printf("Usage: tilewin-display-manager [options]\n"
		"  --config <file>        instead of " DM_CONFIG_PATH "\n"
		"  --vt <n>               this terminal instead of the one in the config\n"
		"  --once                 end after the first session, for a trial next to\n"
		"                         the display manager in use\n"
		"  --check-config <file>  says what is wrong with a config\n"
		"  --state <dir> --run <dir> --compositor <file> --greeter <file>\n"
		"                         other places, for tests\n");
}

int main(int argc, char **argv) {
	static const struct option options[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "vt", required_argument, NULL, 'v' },
		{ "once", no_argument, NULL, 'o' },
		{ "check-config", required_argument, NULL, 'C' },
		{ "state", required_argument, NULL, 's' },
		{ "run", required_argument, NULL, 'r' },
		{ "compositor", required_argument, NULL, 'b' },
		{ "greeter", required_argument, NULL, 'g' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	dm.config_path = DM_CONFIG_PATH;
	int vt_override = 0, c;
	while ((c = getopt_long(argc, argv, "h", options, NULL)) != -1) {
		switch (c) {
		case 'c':
			dm.config_path = optarg;
			break;
		case 'v':
			vt_override = atoi(optarg);
			break;
		case 'o':
			dm.once = true;
			break;
		case 'C':
			return dm_config_check(optarg, report_problem, NULL) ? 1 : 0;
		case 's':
			dm.state_dir = optarg;
			break;
		case 'r':
			dm.run_dir = optarg;
			break;
		case 'b':
			dm.compositor = optarg;
			break;
		case 'g':
			dm.greeter = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}
	dm.config = dm_config_load(dm.config_path);
	dm.vt = vt_override > 0 ? vt_override : dm_config_int(dm.config, "vt");
	if (dm_backend_greeter_uid() == (uid_t)-1) {
		logmsg("there is no user %s: is the package installed whole?", dm_backend_greeter_user());
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	struct sigaction term = { .sa_handler = on_term };
	sigaction(SIGTERM, &term, NULL);
	sigaction(SIGINT, &term, NULL);
	prepare_folders();
	make_socket();
	autologin_if_set();
	while (!got_term) {
		login_screen_until_session_ended();
		if (dm.once) {
			break;
		}
		dm_config_free(dm.config);
		dm.config = dm_config_load(dm.config_path);
		if (!dm_config_bool(dm.config, "autologin_once")) {
			autologin_if_set();
		}
	}
	stop_greeter();
	if (dm.worker_pid > 0) {
		kill(dm.worker_pid, SIGTERM);
		waitpid(dm.worker_pid, NULL, 0);
	}
	unlink(dm.socket_path);
	return 0;
}
