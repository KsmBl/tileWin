/*
 * The git widget: the branch, the newest tag and the number of changed lines
 * of the repository the focused window is working in.
 *
 *     main  v1.0.7  +42 -7
 *
 * The directory comes from the kernel rather than from the shell: the focused
 * window has a process, that process has children, and one of them is the
 * foreground process of its terminal, whose /proc/<pid>/cwd is where the user
 * is. Nothing has to be sourced into a shell, so every shell behaves the same.
 *
 * The reading stays on the last repository it saw. Focusing a browser, an
 * editor or a terminal outside a repository leaves what was there on screen,
 * so a diff can be read on one screen while the outstanding work stays legible
 * on the other.
 *
 * git is asked in the background: one shell that runs three git commands and
 * writes their answers to a pipe the panel reads when it is ready. The taskbar
 * never waits for it, and nothing runs at all while the widget is off screen or
 * while the repository has not changed.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"
#include "tw_theme.h"

#define OUTPUT_MAX (256 * 1024)
#define FILES_IN_TOOLTIP 12

struct git_state {
	struct widget *widget;
	struct loop_timer *timer;
	int interval_ms;

	char *repo;      // the repository the reading belongs to, NULL when none
	char *last_seen; // the last repository a focused window was in

	// what is drawn
	char *branch, *tag;
	long added, removed;
	int staged, unstaged, untracked, conflicts, ahead, behind;
	bool detached;

	// the git that is running, if any
	pid_t pid;
	int fd;
	char *buf;
	size_t len, cap;
	char *pending; // the repository that run belongs to
};

/* ---------- where the focused window is working ---------- */

static bool read_first_line(const char *path, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(out, size, f) != NULL;
	fclose(f);
	if (ok) {
		out[strcspn(out, "\n")] = '\0';
	}
	return ok;
}

/*
 * The foreground process group of a terminal is in field 8 of its /proc stat.
 * A process whose own group is the foreground one is what the user is looking
 * at: the shell while it waits, the command while it runs.
 */
static bool is_foreground(pid_t pid) {
	char path[64], line[1024];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	if (!read_first_line(path, line, sizeof(line))) {
		return false;
	}
	char *after = strrchr(line, ')');
	if (!after) {
		return false;
	}
	int field = 2, pgrp = -1, tpgid = -2;
	for (char *save = NULL, *tok = strtok_r(after + 1, " ", &save); tok;
			tok = strtok_r(NULL, " ", &save)) {
		if (++field == 5) {
			pgrp = atoi(tok);
		} else if (field == 8) {
			tpgid = atoi(tok);
			break;
		}
	}
	return pgrp > 0 && pgrp == tpgid;
}

static char *cwd_of(pid_t pid) {
	char path[64], target[PATH_MAX];
	snprintf(path, sizeof(path), "/proc/%d/cwd", (int)pid);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0) {
		return NULL;
	}
	target[n] = '\0';
	return strdup(target);
}

/* Walks the children of pid, deepest first, for the one holding the terminal. */
static char *foreground_cwd(pid_t pid, int depth) {
	if (depth > 8) {
		return NULL;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", (int)pid, (int)pid);
	char line[4096] = "";
	read_first_line(path, line, sizeof(line));
	for (char *save = NULL, *tok = strtok_r(line, " ", &save); tok;
			tok = strtok_r(NULL, " ", &save)) {
		pid_t child = atoi(tok);
		if (child <= 0) {
			continue;
		}
		char *deeper = foreground_cwd(child, depth + 1);
		if (deeper) {
			return deeper;
		}
		if (is_foreground(child)) {
			char *dir = cwd_of(child);
			if (dir) {
				return dir;
			}
		}
	}
	return NULL;
}

/* The repository a directory is in, or NULL: the first parent holding .git. */
static char *repo_of(const char *dir) {
	if (!dir || *dir != '/') {
		return NULL;
	}
	char *path = strdup(dir);
	while (path && strlen(path) > 1) {
		char *dotgit = format_str("%s/.git", path);
		struct stat st;
		bool found = stat(dotgit, &st) == 0;
		free(dotgit);
		if (found) {
			return path;
		}
		char *slash = strrchr(path, '/');
		if (!slash) {
			break;
		}
		*slash = '\0';
		if (!*path) { // reached the root
			break;
		}
	}
	free(path);
	return NULL;
}

/* ---------- asking git ---------- */

static void git_finished(struct git_state *g, bool ok);

static void git_readable(int fd, short mask, void *data) {
	struct widget *w = data;
	struct git_state *g = w->data;
	while (true) {
		if (g->cap - g->len < 4096) {
			size_t cap = g->cap ? g->cap * 2 : 16384;
			if (cap > OUTPUT_MAX) {
				git_finished(g, false);
				return;
			}
			char *buf = realloc(g->buf, cap);
			if (!buf) {
				git_finished(g, false);
				return;
			}
			g->buf = buf;
			g->cap = cap;
		}
		ssize_t n = read(fd, g->buf + g->len, g->cap - g->len - 1);
		if (n > 0) {
			g->len += n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			return;
		} else {
			git_finished(g, n == 0);
			return;
		}
	}
}

/*
 * One shell, three git commands, each answer behind a marker. Asking in one go
 * keeps it to a single process per refresh however much is being shown.
 */
static const char *const GIT_SCRIPT =
	"cd \"$1\" 2>/dev/null || exit 1\n"
	"printf '@head\\n'; git rev-parse --abbrev-ref HEAD 2>/dev/null\n"
	"printf '@tag\\n'; git describe --tags --abbrev=0 2>/dev/null\n"
	"printf '@numstat\\n'; git diff HEAD --numstat 2>/dev/null\n"
	"printf '@status\\n'; git status --porcelain=v1 -b 2>/dev/null\n";

static void git_start(struct widget *w, const char *repo) {
	struct git_state *g = w->data;
	if (g->pid > 0) {
		return; // one at a time; the next tick picks the newest repository up
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		signal(SIGPIPE, SIG_DFL);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			close(null);
		}
		execlp("sh", "sh", "-c", GIT_SCRIPT, "sh", repo, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	g->pid = pid;
	g->fd = fds[0];
	free(g->pending);
	g->pending = strdup(repo);
	free(g->buf);
	g->buf = NULL;
	g->len = g->cap = 0;
	loop_add_fd(w->panel->loop, g->fd, POLLIN, git_readable, w);
}

static void parse_output(struct git_state *g) {
	g->branch = NULL;
	g->tag = NULL;
	g->added = g->removed = 0;
	g->staged = g->unstaged = g->untracked = g->conflicts = 0;
	g->ahead = g->behind = 0;
	g->detached = false;

	enum { NONE, HEAD, TAG, NUMSTAT, STATUS } section = NONE;
	char *save = NULL;
	for (char *line = strtok_r(g->buf, "\n", &save); line;
			line = strtok_r(NULL, "\n", &save)) {
		if (strcmp(line, "@head") == 0) {
			section = HEAD;
		} else if (strcmp(line, "@tag") == 0) {
			section = TAG;
		} else if (strcmp(line, "@numstat") == 0) {
			section = NUMSTAT;
		} else if (strcmp(line, "@status") == 0) {
			section = STATUS;
		} else if (section == HEAD && !g->branch && *line) {
			g->detached = strcmp(line, "HEAD") == 0;
			g->branch = strdup(line);
		} else if (section == TAG && !g->tag && *line) {
			g->tag = strdup(line);
		} else if (section == NUMSTAT && *line) {
			// "added<TAB>removed<TAB>path"; binary files carry "-"
			char *end = NULL;
			long a = strtol(line, &end, 10);
			if (end && *end == '\t') {
				long r = strtol(end + 1, NULL, 10);
				g->added += a > 0 ? a : 0;
				g->removed += r > 0 ? r : 0;
			}
		} else if (section == STATUS && *line) {
			if (strncmp(line, "## ", 3) == 0) {
				char *ahead = strstr(line, "[ahead ");
				char *behind = strstr(line, "[behind ");
				if (!behind) {
					behind = strstr(line, ", behind ");
				}
				g->ahead = ahead ? atoi(ahead + 7) : 0;
				g->behind = behind ? atoi(strchr(behind, ' ') + 1) : 0;
				if (g->detached && strstr(line, "no branch")) {
					free(g->branch);
					g->branch = strdup("detached");
				}
				continue;
			}
			char x = line[0], y = line[1];
			if (x == '?' && y == '?') {
				g->untracked++;
			} else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') ||
					(x == 'D' && y == 'D')) {
				g->conflicts++;
			} else {
				if (x != ' ' && x != '?') {
					g->staged++;
				}
				if (y != ' ' && y != '?') {
					g->unstaged++;
				}
			}
		}
	}
}

static void git_finished(struct git_state *g, bool ok) {
	struct widget *w = g->widget;
	loop_remove_fd(w->panel->loop, g->fd);
	close(g->fd);
	g->fd = -1;
	if (g->pid > 0) {
		waitpid(g->pid, NULL, WNOHANG);
		g->pid = 0;
	}
	if (ok && g->buf) {
		g->buf[g->len] = '\0';
		free(g->branch);
		free(g->tag);
		parse_output(g);
		free(g->repo);
		g->repo = g->pending;
		g->pending = NULL;
	}
	free(g->buf);
	g->buf = NULL;
	g->len = g->cap = 0;
	free(g->pending);
	g->pending = NULL;
	panel_set_dirty(w->panel);
}

/* ---------- when to look ---------- */

/* The repository of the focused window, remembered until another one appears. */
static void follow_focus(struct widget *w) {
	struct git_state *g = w->data;
	struct pwindow *win = panel_find_window(w->panel, w->panel->state.focused_window);
	if (!win || win->pid <= 0) {
		return;
	}
	char *dir = foreground_cwd(win->pid, 0);
	if (!dir) {
		dir = cwd_of(win->pid);
	}
	char *repo = repo_of(dir);
	free(dir);
	if (!repo) {
		return; // not a repository: what is on screen stays
	}
	if (!g->last_seen || strcmp(g->last_seen, repo) != 0) {
		free(g->last_seen);
		g->last_seen = repo;
		git_start(w, g->last_seen);
	} else {
		free(repo);
	}
}

static void git_tick(void *data) {
	struct widget *w = data;
	struct git_state *g = w->data;
	g->timer = NULL;
	if (!w->active) {
		return;
	}
	if (g->last_seen) {
		struct stat st;
		if (stat(g->last_seen, &st) == 0) {
			git_start(w, g->last_seen);
		} else { // the repository was removed
			free(g->last_seen);
			g->last_seen = NULL;
			free(g->repo);
			g->repo = NULL;
			panel_set_dirty(w->panel);
		}
	}
	g->timer = loop_add_timer(w->panel->loop, g->interval_ms, git_tick, w);
}

static void git_state_changed(struct widget *w) {
	follow_focus(w);
}

static void git_set_active(struct widget *w, bool active) {
	struct git_state *g = w->data;
	if (active && !g->timer) {
		follow_focus(w);
		git_tick(w);
	} else if (!active && g->timer) {
		loop_remove_timer(w->panel->loop, g->timer);
		g->timer = NULL;
	}
}

static void git_init(struct widget *w) {
	struct git_state *g = calloc(1, sizeof(*g));
	g->widget = w;
	g->fd = -1;
	g->interval_ms = widget_conf_int(w, "interval", 5) * 1000;
	if (g->interval_ms < 1000) {
		g->interval_ms = 1000;
	}
	w->data = g;
	follow_focus(w);
}

static void git_destroy(struct widget *w) {
	struct git_state *g = w->data;
	if (g->timer) {
		loop_remove_timer(w->panel->loop, g->timer);
	}
	if (g->fd >= 0) {
		loop_remove_fd(w->panel->loop, g->fd);
		close(g->fd);
	}
	if (g->pid > 0) {
		kill(g->pid, SIGTERM);
		waitpid(g->pid, NULL, WNOHANG);
	}
	free(g->repo);
	free(g->last_seen);
	free(g->pending);
	free(g->branch);
	free(g->tag);
	free(g->buf);
	free(g);
}

/* ---------- drawing ---------- */

static char *git_text(struct widget *w) {
	struct git_state *g = w->data;
	if (!g->repo || !g->branch) {
		return NULL;
	}
	// no glyph by default: not every theme font has one, and a missing
	// glyph draws as nothing at all rather than as a branch
	const char *icon = widget_conf(w, "icon", NULL);
	GString *out = g_string_new(NULL);
	if (icon && *icon) {
		g_string_append_printf(out, "%s ", icon);
	}
	g_string_append(out, g->branch);
	if (g->tag && widget_conf_bool(w, "show_tag", true)) {
		g_string_append_printf(out, "  %s", g->tag);
	}
	if (g->ahead) {
		g_string_append_printf(out, "  ↑%d", g->ahead);
	}
	if (g->behind) {
		g_string_append_printf(out, "  ↓%d", g->behind);
	}
	if (g->added || g->removed) {
		g_string_append_printf(out, "  +%ld -%ld", g->added, g->removed);
	}
	if (g->conflicts) {
		g_string_append_printf(out, "  !%d", g->conflicts);
	}
	return g_string_free(out, FALSE);
}

static int git_padding(struct render_ctx *ctx) {
	return tw_theme_int(ctx->panel->theme, "panel.item_padding", 6);
}

static int git_measure(struct widget *w, struct render_ctx *ctx) {
	char *text = git_text(w);
	if (!text) {
		return 0; // nothing seen yet: the widget takes no room
	}
	int width = render_text_width(ctx, bar_font(ctx->panel), text) + 2 * git_padding(ctx);
	free(text);
	int max = widget_conf_int(w, "max_width", 0);
	return max > 0 && width > max ? max : width;
}

static void git_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct git_state *g = w->data;
	char *text = git_text(w);
	if (!text) {
		return;
	}
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	uint32_t fg = widget_fg(ctx->panel, "git");
	const char *key = g->conflicts ? "git.conflict" :
		g->staged || g->unstaged || g->untracked ? "git.dirty" : "git.clean";
	fg = tw_theme_color(ctx->panel->theme, key, fg);
	int pad = git_padding(ctx);
	pd_text(ctx->cairo, bar_font(ctx->panel), text, b.x + pad, b.y, b.width - 2 * pad,
		b.height, fg, PD_CENTER);
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
	free(text);
}

static char *git_tooltip(struct widget *w, struct hotspot *hs) {
	struct git_state *g = w->data;
	if (!g->repo || !g->branch) {
		return NULL;
	}
	GString *t = g_string_new(NULL);
	g_string_append_printf(t, "%s\n%s", g->repo, g->branch);
	if (g->tag) {
		g_string_append_printf(t, " · %s", g->tag);
	}
	if (g->ahead || g->behind) {
		g_string_append(t, "\n");
		if (g->ahead) {
			g_string_append_printf(t, "%d commit%s to push", g->ahead,
				g->ahead == 1 ? "" : "s");
		}
		if (g->ahead && g->behind) {
			g_string_append(t, ", ");
		}
		if (g->behind) {
			g_string_append_printf(t, "%d to pull", g->behind);
		}
	}
	if (g->added || g->removed) {
		g_string_append_printf(t, "\n%ld line%s added, %ld removed", g->added,
			g->added == 1 ? "" : "s", g->removed);
	}
	if (g->staged || g->unstaged || g->untracked || g->conflicts) {
		g_string_append(t, "\n");
		const char *sep = "";
		if (g->staged) {
			g_string_append_printf(t, "%s%d staged", sep, g->staged);
			sep = ", ";
		}
		if (g->unstaged) {
			g_string_append_printf(t, "%s%d changed", sep, g->unstaged);
			sep = ", ";
		}
		if (g->untracked) {
			g_string_append_printf(t, "%s%d untracked", sep, g->untracked);
			sep = ", ";
		}
		if (g->conflicts) {
			g_string_append_printf(t, "%s%d conflicted", sep, g->conflicts);
		}
	} else {
		g_string_append(t, "\nnothing to commit");
	}
	return g_string_free(t, FALSE);
}

static bool git_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct git_state *g = w->data;
	if (button != BTN_LEFT || !g->repo) {
		return false;
	}
	// a terminal where the work is; the compositor knows what $term is
	char *quoted = g_shell_quote(g->repo);
	ipc_panel_commandf(w->panel, "exec cd %s && exec $term", quoted);
	g_free(quoted);
	return true;
}

const struct widget_impl widget_git = {
	.type = "git",
	.init = git_init,
	.destroy = git_destroy,
	.measure = git_measure,
	.render = git_render,
	.click = git_click,
	.tooltip = git_tooltip,
	.set_active = git_set_active,
	.state_changed = git_state_changed,
};
