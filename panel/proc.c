#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "panel.h"

/*
 * Child processes for custom widgets and system helpers. Output is read
 * without blocking through the panel's event loop. The loop does not allow
 * removing an fd from inside its own callback safely, so finished processes
 * are cleaned up from a zero-delay timer.
 */
struct proc {
	struct panel *panel;
	pid_t pid;
	int fd;
	bool listen;
	bool finished;
	char *buf;
	size_t len, cap;
	proc_line_fn on_line;
	proc_done_fn on_done;
	void *data;
	bool cancelled;
};

static void proc_free(void *data) {
	struct proc *proc = data;
	if (proc->fd >= 0) {
		close(proc->fd);
	}
	free(proc->buf);
	free(proc);
}

static void finish(struct proc *proc) {
	if (proc->finished) {
		return;
	}
	proc->finished = true;
	loop_remove_fd(proc->panel->loop, proc->fd);
	if (!proc->cancelled) {
		if (proc->listen && proc->len > 0 && proc->on_line) {
			proc->buf[proc->len] = '\0';
			proc->on_line(proc->data, proc->buf);
		}
		if (proc->on_done) {
			if (proc->buf) {
				proc->buf[proc->len] = '\0';
			}
			proc->on_done(proc->data, proc->buf ? proc->buf : "");
		}
	}
	loop_add_timer(proc->panel->loop, 0, proc_free, proc);
}

static void proc_readable(int fd, short mask, void *data) {
	struct proc *proc = data;
	if (proc->finished) {
		return;
	}
	for (;;) {
		if (proc->cap - proc->len < 1024) {
			proc->cap = proc->cap ? proc->cap * 2 : 4096;
			if (proc->cap > (1 << 20)) {
				// runaway output: stop reading
				kill(proc->pid, SIGTERM);
				finish(proc);
				return;
			}
			proc->buf = realloc(proc->buf, proc->cap);
		}
		ssize_t n = read(fd, proc->buf + proc->len, proc->cap - proc->len - 1);
		if (n > 0) {
			proc->len += n;
			if (proc->listen) {
				char *nl;
				while ((nl = memchr(proc->buf, '\n', proc->len))) {
					*nl = '\0';
					if (!proc->cancelled && proc->on_line) {
						proc->on_line(proc->data, proc->buf);
					}
					size_t consumed = nl - proc->buf + 1;
					memmove(proc->buf, nl + 1, proc->len - consumed);
					proc->len -= consumed;
				}
			}
			continue;
		}
		if (n == 0) {
			finish(proc);
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (mask & (POLLHUP | POLLERR)) {
				finish(proc);
			}
			return;
		}
		if (errno == EINTR) {
			continue;
		}
		finish(proc);
		return;
	}
}

struct proc *proc_run(struct panel *panel, const char *command, bool listen,
		proc_line_fn on_line, proc_done_fn on_done, void *data) {
	int fds[2];
	if (pipe(fds) != 0) {
		return NULL;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}
	if (pid == 0) {
		setsid();
		dup2(fds[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
		}
		close(fds[0]);
		close(fds[1]);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);

	struct proc *proc = calloc(1, sizeof(*proc));
	proc->panel = panel;
	proc->pid = pid;
	proc->fd = fds[0];
	proc->listen = listen;
	proc->on_line = on_line;
	proc->on_done = on_done;
	proc->data = data;
	loop_add_fd(panel->loop, proc->fd, POLLIN, proc_readable, proc);
	return proc;
}

void proc_cancel(struct proc *proc) {
	if (!proc || proc->finished) {
		return;
	}
	proc->cancelled = true;
	kill(-proc->pid, SIGTERM);
	finish(proc);
}

void proc_spawn(const char *command) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
		}
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
		_exit(127);
	}
}
