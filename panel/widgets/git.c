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
 * Many terminals (xfce4-terminal, GNOME Terminal, Konsole, kitty, foot's
 * server) run all their windows from one process, with a shell for each. Which
 * of those shells belongs to the focused window then comes from its title,
 * which shells set to where they are, whole ("/home/me/src") or shortened the
 * way fish does it ("~/s/project"): the shell whose directory the title names
 * wins, or the command it runs when the title names that; failing both, the
 * one no other window of the terminal claims.
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
#include <stdarg.h>
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

#define CANDIDATES_MAX 32

/* A process in the foreground of a terminal of the window: one per shell. */
struct candidate {
	char *cwd;
	char comm[32];
};

/*
 * Walks the children of pid, deepest first, for those holding a terminal: the
 * deepest foreground process of each branch, one per shell.
 */
static void foreground_candidates(pid_t pid, int depth, struct candidate *out, int *count) {
	if (depth > 8) {
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", (int)pid, (int)pid);
	char line[4096] = "";
	read_first_line(path, line, sizeof(line));
	for (char *save = NULL, *tok = strtok_r(line, " ", &save); tok && *count < CANDIDATES_MAX;
			tok = strtok_r(NULL, " ", &save)) {
		pid_t child = atoi(tok);
		if (child <= 0) {
			continue;
		}
		int before = *count;
		foreground_candidates(child, depth + 1, out, count);
		if (*count > before || !is_foreground(child)) {
			continue; // something deeper holds this terminal
		}
		char *dir = cwd_of(child);
		if (dir) {
			out[*count].cwd = dir;
			snprintf(path, sizeof(path), "/proc/%d/comm", (int)child);
			if (!read_first_line(path, out[*count].comm, sizeof(out[*count].comm))) {
				out[*count].comm[0] = '\0';
			}
			(*count)++;
		}
	}
}

/*
 * How well a window title names a directory: the number of path components of
 * the longest path in the title that fits it, 0 for none. A component fits
 * when it is the same, or shortened to its start the way fish does; "~" is home.
 */
static int title_names_dir(const char *title, const char *dir) {
	const char *home = getenv("HOME");
	int best = 0;
	for (const char *p = title; p && *p; p++) {
		if ((*p != '/' && *p != '~') || (p > title && !isspace((unsigned char)p[-1]) &&
				p[-1] != ':' && p[-1] != '(' && p[-1] != '[')) {
			continue; // a path starts a word
		}
		// the path in the title: up to a space, or the end
		size_t len = strcspn(p, " \t)]");
		char *path = strndup(p, len);
		char *full = path[0] == '~' && home ? format_str("%s%s", home, path + 1) : strdup(path);
		free(path);
		// component by component against the directory
		const char *a = full, *b = dir;
		int parts = 0;
		bool fits = true;
		while (*a && fits) {
			while (*a == '/') {
				a++;
			}
			while (*b == '/') {
				b++;
			}
			if (!*a) {
				break;
			}
			size_t la = strcspn(a, "/"), lb = strcspn(b, "/");
			// the same, or the start of it (shortened) when it is not the last one
			fits = lb > 0 && ((la == lb && strncmp(a, b, la) == 0) ||
				(la < lb && a[la] == '/' && strncmp(a, b, la) == 0));
			a += la;
			b += lb;
			parts += fits;
		}
		while (*b == '/') {
			b++;
		}
		if (fits && !*b && parts > best) {
			best = parts; // the whole directory, nothing left over
		}
		free(full);
	}
	return best;
}

/* How well a title fits a candidate: its directory, else the command it runs. */
static int title_score(const char *title, struct candidate *c) {
	if (!title) {
		return 0;
	}
	int score = title_names_dir(title, c->cwd) * 2;
	if (!score && c->comm[0] && strlen(c->comm) > 2 && strcasestr(title, c->comm)) {
		score = 1;
	}
	return score;
}

/*
 * Where the focused window is working: the directory of the foreground process
 * of its terminal, told apart by the title when the terminal process has more.
 */
static char *window_cwd(struct panel *panel, struct pwindow *win) {
	struct candidate cands[CANDIDATES_MAX];
	int count = 0;
	foreground_candidates(win->pid, 0, cands, &count);
	int pick = count == 1 ? 0 : -1;
	if (count > 1) {
		// the title names one of them
		int best = 0;
		bool tie = false;
		for (int i = 0; i < count; i++) {
			int score = title_score(win->title, &cands[i]);
			if (score > best) {
				best = score;
				pick = i;
				tie = false;
			} else if (score == best && score > 0 && strcmp(cands[i].cwd, cands[pick].cwd) != 0) {
				tie = true;
			}
		}
		if (tie) {
			pick = -1;
		}
		if (pick < 0) {
			// or it is the one that no other window of the same terminal claims
			int left = -1, unclaimed = 0;
			for (int i = 0; i < count; i++) {
				bool claimed = false;
				for (int k = 0; k < panel->state.windows->length && !claimed; k++) {
					struct pwindow *other = panel->state.windows->items[k];
					claimed = other != win && other->pid == win->pid &&
						title_score(other->title, &cands[i]) > 0;
				}
				if (!claimed) {
					left = i;
					unclaimed++;
				}
			}
			pick = unclaimed == 1 ? left : -1;
		}
	}
	char *dir = pick >= 0 ? strdup(cands[pick].cwd) : NULL;
	for (int i = 0; i < count; i++) {
		free(cands[i].cwd);
	}
	if (!dir && count == 0) {
		dir = cwd_of(win->pid); // no terminal: where the app itself is
	}
	return dir;
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
	if (g->pid > 0 && g->pending && strcmp(g->pending, repo) != 0) {
		// asking about a repository that is no longer wanted: that one goes
		loop_remove_fd(w->panel->loop, g->fd);
		close(g->fd);
		g->fd = -1;
		kill(g->pid, SIGTERM);
		waitpid(g->pid, NULL, 0);
		g->pid = 0;
	}
	if (g->pid > 0) {
		return; // one at a time; the next tick asks again
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
	widget_set_dirty(w);
}

/* ---------- when to look ---------- */

/* The repository of the focused window, remembered until another one appears. */
static void follow_focus(struct widget *w) {
	struct git_state *g = w->data;
	struct pwindow *win = panel_find_window(w->panel, w->panel->state.focused_window);
	if (!win || win->pid <= 0) {
		return;
	}
	char *dir = window_cwd(w->panel, win);
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
	char *before = g->last_seen ? strdup(g->last_seen) : NULL;
	follow_focus(w); // a cd in the focused window, which is no event of the compositor
	bool changed = (before == NULL) != (g->last_seen == NULL) ||
		(before && strcmp(before, g->last_seen) != 0);
	free(before);
	if (g->last_seen && !changed) {
		struct stat st;
		if (stat(g->last_seen, &st) == 0) {
			git_start(w, g->last_seen);
		} else { // the repository was removed
			free(g->last_seen);
			g->last_seen = NULL;
			free(g->repo);
			g->repo = NULL;
			widget_set_dirty(w);
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

/*
 * The reading is drawn in pieces so that what was added can be green and what
 * was removed red, the way a diff is read everywhere else. Each piece carries
 * the colour it wants; the widget measures them all and draws them in a row.
 */
#define GIT_PIECES 8

struct piece {
	char text[64];
	const char *color_key; // theme key, NULL for the widget's own colour
	uint32_t fallback;
};

static void add_piece(struct piece *pieces, int *count, const char *color_key,
		uint32_t fallback, const char *fmt, ...) {
	if (*count >= GIT_PIECES) {
		return;
	}
	struct piece *p = &pieces[(*count)++];
	va_list args;
	va_start(args, fmt);
	vsnprintf(p->text, sizeof(p->text), fmt, args);
	va_end(args);
	p->color_key = color_key;
	p->fallback = fallback;
}

/* Fills in what is to be drawn; returns how many pieces there are. */
static int git_pieces(struct widget *w, struct piece *pieces) {
	struct git_state *g = w->data;
	int count = 0;
	if (!g->repo || !g->branch) {
		return 0;
	}
	// no glyph by default: not every theme font has one, and a missing
	// glyph draws as nothing at all rather than as a branch
	const char *icon = widget_conf(w, "icon", NULL);
	if (icon && *icon) {
		add_piece(pieces, &count, NULL, 0, "%s ", icon);
	}
	add_piece(pieces, &count, NULL, 0, "%s", g->branch);
	if (g->tag && widget_conf_bool(w, "show_tag", true)) {
		add_piece(pieces, &count, "git.tag", 0, "  %s", g->tag);
	}
	if (g->ahead) {
		add_piece(pieces, &count, NULL, 0, "  ↑%d", g->ahead);
	}
	if (g->behind) {
		add_piece(pieces, &count, NULL, 0, "  ↓%d", g->behind);
	}
	if (g->added) {
		add_piece(pieces, &count, "git.added", 0x4ade80ff, "  +%ld", g->added);
	}
	if (g->removed) {
		add_piece(pieces, &count, "git.removed", 0xf87171ff, "  -%ld", g->removed);
	}
	if (g->untracked && widget_conf_bool(w, "show_untracked", true)) {
		add_piece(pieces, &count, "git.untracked", 0, "  ?%d", g->untracked);
	}
	if (g->conflicts) {
		add_piece(pieces, &count, "git.conflict", 0xf87171ff, "  !%d", g->conflicts);
	}
	return count;
}

static int git_padding(struct render_ctx *ctx) {
	return tw_theme_int(ctx->panel->theme, "panel.item_padding", 6);
}

static int git_measure(struct widget *w, struct render_ctx *ctx) {
	struct piece pieces[GIT_PIECES];
	int count = git_pieces(w, pieces);
	if (count == 0) {
		return 0; // nothing seen yet: the widget takes no room
	}
	int width = 2 * git_padding(ctx);
	for (int i = 0; i < count; i++) {
		width += render_text_width(ctx, bar_font(ctx->panel), pieces[i].text);
	}
	int max = widget_conf_int(w, "max_width", 0);
	return max > 0 && width > max ? max : width;
}

static void git_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct git_state *g = w->data;
	struct piece pieces[GIT_PIECES];
	int count = git_pieces(w, pieces);
	if (count == 0) {
		return;
	}
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	// the branch takes the colour of the state the repository is in
	uint32_t fg = widget_fg(ctx->panel, "git");
	const char *state = g->conflicts ? "git.conflict" :
		g->staged || g->unstaged || g->untracked ? "git.dirty" : "git.clean";
	fg = tw_theme_color(ctx->panel->theme, state, fg);
	int pad = git_padding(ctx);
	double x = b.x + pad;
	double end = b.x + b.width - pad;
	for (int i = 0; i < count && x < end; i++) {
		int width = render_text_width(ctx, bar_font(ctx->panel), pieces[i].text);
		uint32_t color = pieces[i].color_key ?
			tw_theme_color(ctx->panel->theme, pieces[i].color_key,
				pieces[i].fallback ? pieces[i].fallback : fg) : fg;
		double room = x + width > end ? end - x : width;
		pd_text(ctx->cairo, bar_font(ctx->panel), pieces[i].text, x, b.y, room, b.height,
			color, PD_LEFT);
		x += width;
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
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

const char *git_widget_repo(struct widget *w) {
	struct git_state *g = w->data;
	return g->repo;
}

static bool git_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct git_state *g = w->data;
	if (button != BTN_LEFT || !g->repo) {
		return false;
	}
	// what the repository is up to; a terminal there is one click further, in the flyout
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
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
