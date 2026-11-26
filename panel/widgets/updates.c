/*
 * The updates widget: the system updates the package manager has, on the
 * taskbar and on the desktop. A round arrow in the color of the state (the
 * accent: updates are there, amber: installed, waiting for a restart, red:
 * asking or installing failed, grey: up to date) and how many there are.
 *
 *   widget updates {
 *       text full|short|none   # "12 updates", "12", the arrow alone
 *       hide_none yes|no       # nothing on the taskbar while all is up to date
 *       interval 6             # hours between checks
 *       notify yes|no          # a notification of updates not seen before
 *   }
 *
 * tilewin-updates does the work and writes its state to
 * ~/.local/state/tileWin/updates, which the widget reads every few seconds: it
 * only starts "tilewin-updates check" when the last check is older than the
 * interval, the first time a minute and a half after the taskbar started, so
 * that logging in is not slowed down. A click opens a flyout with the updates,
 * Install updates (through pkexec: the password is asked), Check now and
 * Clean up (the package cache), and the weekly clean-up.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "panel.h"
#include "stringop.h"
#include "tw_kernel_auto.h"
#include "tw_paths.h"
#include "tw_theme.h"

#define READ_MS 3000
#define FIRST_CHECK_S 90

struct updates_state {
	struct updates_view view;
	struct loop_timer *timer;
	time_t started;
	pid_t pid;          // a check this widget started, until it ends
	struct timespec mtime; // of the status file read last
	struct timespec list_mtime;
	char boot_id[64];
};

static char *state_file(const char *name) {
	char *dir = tw_state_dir();
	char *path = dir ? format_str("%s/updates/%s", dir, name) : NULL;
	free(dir);
	return path;
}

static void read_boot_id(char *out, size_t size) {
	out[0] = '\0';
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (f) {
		if (fgets(out, (int)size, f)) {
			out[strcspn(out, "\n")] = '\0';
		}
		fclose(f);
	}
}

static void free_list(struct updates_view *v) {
	for (int i = 0; i < v->count_listed; i++) {
		free(v->list[i].name);
		free(v->list[i].old);
		free(v->list[i].new);
	}
	free(v->list);
	v->list = NULL;
	v->count_listed = 0;
}

static void read_list(struct updates_state *u) {
	char *path = state_file("list");
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	free_list(&u->view);
	if (!f) {
		return;
	}
	char *line = NULL;
	size_t cap = 0;
	int size = 0;
	while (getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\n")] = '\0';
		if (!*line) {
			continue;
		}
		if (u->view.count_listed == size) {
			size = size ? size * 2 : 32;
			u->view.list = realloc(u->view.list, size * sizeof(*u->view.list));
		}
		// name<TAB>now<TAB>new; dnf does not tell the version now
		char *tab1 = strchr(line, '\t');
		char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
		struct updates_item *it = &u->view.list[u->view.count_listed++];
		it->name = strndup(line, tab1 ? (size_t)(tab1 - line) : strlen(line));
		it->old = tab1 ? strndup(tab1 + 1, tab2 ? (size_t)(tab2 - tab1 - 1) : strlen(tab1 + 1))
			: strdup("");
		it->new = tab2 ? strdup(tab2 + 1) : strdup("");
	}
	free(line);
	fclose(f);
}

static bool changed(const char *name, struct timespec *seen) {
	char *path = state_file(name);
	struct stat st;
	bool there = path && stat(path, &st) == 0;
	free(path);
	struct timespec now = there ? st.st_mtim : (struct timespec){ 0 };
	if (now.tv_sec == seen->tv_sec && now.tv_nsec == seen->tv_nsec) {
		return false;
	}
	*seen = now;
	return true;
}

static void read_status(struct updates_state *u) {
	struct updates_view *v = &u->view;
	char *path = state_file("status");
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	char restart_boot[64] = "";
	v->state = UPDATES_UNKNOWN;
	v->count = v->done = v->total = 0;
	v->checked = v->installed = 0;
	v->cache = -1;
	v->weekly = 0;
	v->restart = false;
	v->downloading = false;
	v->manager[0] = v->current[0] = v->message[0] = '\0';
	if (!f) {
		return;
	}
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *key = line, *value = eq + 1;
		if (strcmp(key, "state") == 0) {
			v->state = strcmp(value, "idle") == 0 ? UPDATES_IDLE :
				strcmp(value, "checking") == 0 ? UPDATES_CHECKING :
				strcmp(value, "installing") == 0 ? UPDATES_INSTALLING :
				strcmp(value, "cleaning") == 0 ? UPDATES_CLEANING :
				strcmp(value, "error") == 0 ? UPDATES_ERROR : UPDATES_UNKNOWN;
		} else if (strcmp(key, "count") == 0) {
			v->count = atoi(value);
		} else if (strcmp(key, "done") == 0) {
			v->done = atoi(value);
		} else if (strcmp(key, "total") == 0) {
			v->total = atoi(value);
		} else if (strcmp(key, "checked") == 0) {
			v->checked = strtoll(value, NULL, 10);
		} else if (strcmp(key, "installed") == 0) {
			v->installed = strtoll(value, NULL, 10);
		} else if (strcmp(key, "cache") == 0) {
			v->cache = strtoll(value, NULL, 10);
		} else if (strcmp(key, "weekly") == 0) {
			v->weekly = strcmp(value, "on") == 0 ? 1 : strcmp(value, "off") == 0 ? -1 : 0;
		} else if (strcmp(key, "restart_boot") == 0) {
			snprintf(restart_boot, sizeof(restart_boot), "%s", value);
		} else if (strcmp(key, "phase") == 0) {
			v->downloading = strcmp(value, "downloading") == 0;
		} else if (strcmp(key, "manager") == 0) {
			snprintf(v->manager, sizeof(v->manager), "%s", value);
		} else if (strcmp(key, "current") == 0) {
			snprintf(v->current, sizeof(v->current), "%s", value);
		} else if (strcmp(key, "message") == 0) {
			snprintf(v->message, sizeof(v->message), "%s", value);
		}
	}
	free(line);
	fclose(f);
	// an installation that wants a restart: until the computer has restarted
	v->restart = restart_boot[0] && strcmp(restart_boot, u->boot_id) == 0;
}

static void reload(struct widget *w, bool force) {
	struct updates_state *u = w->data;
	if (changed("status", &u->mtime) || force) {
		read_status(u);
	}
	if (changed("list", &u->list_mtime) || force) {
		read_list(u);
	}
}

static bool busy(const struct updates_view *v) {
	return v->state == UPDATES_CHECKING || v->state == UPDATES_INSTALLING ||
		v->state == UPDATES_CLEANING;
}

/* "tilewin-updates check" in the background; it writes what it finds. */
static void start_check(struct widget *w) {
	struct updates_state *u = w->data;
	if (u->pid > 0) {
		return;
	}
	bool quiet = strcmp(widget_conf(w, "notify", "yes"), "no") == 0;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
			close(null);
		}
		signal(SIGPIPE, SIG_DFL);
		execlp("tilewin-updates", "tilewin-updates", "check", quiet ? "--quiet" : NULL,
			(char *)NULL);
		_exit(127);
	}
	if (pid > 0) {
		u->pid = pid;
	}
}

static void tick(void *data) {
	struct widget *w = data;
	struct updates_state *u = w->data;
	u->timer = NULL;
	if (!w->active) {
		return;
	}
	if (u->pid > 0 && waitpid(u->pid, NULL, WNOHANG) != 0) {
		u->pid = 0;
	}
	struct updates_view before = u->view;
	reload(w, false);
	const struct updates_view *v = &u->view;
	time_t now = time(NULL);
	int hours = atoi(widget_conf(w, "interval", "6"));
	hours = hours < 1 ? 1 : hours > 168 ? 168 : hours;
	// tests start the taskbar with a state of their own: no check of this computer
	bool tests = getenv("TILEWIN_NO_APP_TWEAKS") != NULL;
	if (!tests && !busy(v) && now - u->started >= FIRST_CHECK_S &&
			(v->checked == 0 || now - v->checked >= hours * 3600L)) {
		start_check(w);
	}
	if (before.state != v->state || before.count != v->count || before.done != v->done ||
			before.restart != v->restart) {
		widget_set_dirty(w);
	}
	u->timer = loop_add_timer_lazy(w->panel->loop, READ_MS, tick, w);
}

/* ---------- looks ---------- */

static uint32_t accent_of(struct panel *panel) {
	return tw_theme_color(panel->theme, "flyout.accent",
		tw_theme_color(panel->theme, "taskbar.indicator", 0x0078d4ff)) | 0xff;
}

static uint32_t state_color(struct widget *w, uint32_t accent) {
	struct updates_state *u = w->data;
	const struct updates_view *v = &u->view;
	if (panel_atari(w->panel)) {
		return tw_theme_color(w->panel->theme, "menu.fg", 0x000000ff);
	}
	if (v->state == UPDATES_ERROR) {
		return widget_conf_color(w, "critical_fg", 0xd83b3bff);
	}
	if (v->restart && v->count == 0 && !busy(v)) {
		return widget_conf_color(w, "warning_fg", 0xe8952aff);
	}
	if (v->count > 0 || busy(v)) {
		return accent;
	}
	return widget_conf_color(w, "ok_fg", 0x8a8a8aff);
}

/* A round sign with an arrow down (updates), a turning arrow (restart) or a cross. */
static void draw_sign(cairo_t *cr, double x, double y, double s, uint32_t color,
		const struct updates_view *v) {
	double cx = x + s / 2, cy = y + s / 2, r = s / 2;
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	pd_color(cr, color);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_set_line_width(cr, fmax(s * 0.11, 1.5));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	if (v->state == UPDATES_ERROR) {
		double d = r * 0.38;
		cairo_move_to(cr, cx - d, cy - d);
		cairo_line_to(cr, cx + d, cy + d);
		cairo_move_to(cr, cx + d, cy - d);
		cairo_line_to(cr, cx - d, cy + d);
		cairo_stroke(cr);
	} else if (v->restart && v->count == 0 && !busy(v)) {
		// a turning arrow: restart
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r * 0.48, -M_PI * 0.35, M_PI * 1.45);
		cairo_stroke(cr);
		double ax = cx + r * 0.48 * cos(-M_PI * 0.35), ay = cy + r * 0.48 * sin(-M_PI * 0.35);
		cairo_move_to(cr, ax - r * 0.30, ay - r * 0.05);
		cairo_line_to(cr, ax, ay);
		cairo_line_to(cr, ax + r * 0.02, ay - r * 0.32);
		cairo_stroke(cr);
	} else {
		// an arrow down onto a line
		cairo_move_to(cr, cx, cy - r * 0.50);
		cairo_line_to(cr, cx, cy + r * 0.22);
		cairo_move_to(cr, cx - r * 0.32, cy - r * 0.08);
		cairo_line_to(cr, cx, cy + r * 0.24);
		cairo_line_to(cr, cx + r * 0.32, cy - r * 0.08);
		cairo_move_to(cr, cx - r * 0.40, cy + r * 0.50);
		cairo_line_to(cr, cx + r * 0.40, cy + r * 0.50);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

static void taskbar_text(struct widget *w, char *out, size_t size) {
	struct updates_state *u = w->data;
	const struct updates_view *v = &u->view;
	const char *how = widget_conf(w, "text", "full");
	bool full = strcmp(how, "full") == 0;
	if (strcmp(how, "none") == 0) {
		out[0] = '\0';
	} else if (v->state == UPDATES_INSTALLING && v->total > 0) {
		snprintf(out, size, "%s%d/%d", full ? "Installing " : "", v->done, v->total);
	} else if (v->state == UPDATES_INSTALLING) {
		snprintf(out, size, "%s", full ? (v->downloading ? "Downloading…" : "Installing…") : "…");
	} else if (v->state == UPDATES_CLEANING) {
		snprintf(out, size, "%s", full ? "Cleaning up…" : "…");
	} else if (v->state == UPDATES_ERROR) {
		snprintf(out, size, "%s", full ? "Updates failed" : "!");
	} else if (v->count > 0) {
		snprintf(out, size, "%d%s", v->count, !full ? "" : v->count == 1 ? " update" : " updates");
	} else if (v->restart) {
		snprintf(out, size, "%s", full ? "Restart" : "");
	} else {
		out[0] = '\0';
	}
}

static bool hidden(struct widget *w) {
	struct updates_state *u = w->data;
	const struct updates_view *v = &u->view;
	// a system tileWin cannot update (no pacman, apt or dnf): nothing to show
	if (strcmp(v->manager, "none") == 0) {
		return true;
	}
	return strcmp(widget_conf(w, "hide_none", "yes"), "yes") == 0 && v->count == 0 &&
		!busy(v) && !v->restart && v->state != UPDATES_ERROR;
}

/* ---------- the taskbar ---------- */

static int updates_measure(struct widget *w, struct render_ctx *ctx) {
	if (hidden(w)) {
		return 0;
	}
	char text[64];
	taskbar_text(w, text, sizeof(text));
	int icon = (int)(ctx->height * 0.55);
	int width = 16 + icon;
	if (*text) {
		width += 6 + render_text_width(ctx, bar_font(ctx->panel), text);
	}
	return width;
}

static void updates_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct updates_state *u = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	uint32_t fg = widget_fg(ctx->panel, "updates");
	uint32_t color = state_color(w, accent_of(ctx->panel));
	double icon = floor(b.height * 0.55);
	double x = b.x + 8;
	draw_sign(ctx->cairo, x, b.y + (b.height - icon) / 2, icon, color, &u->view);
	x += icon + 6;
	char text[64];
	taskbar_text(w, text, sizeof(text));
	if (*text) {
		const char *font = bar_font(ctx->panel);
		int tw = render_text_width(ctx, font, text);
		pd_text(ctx->cairo, font, text, x, b.y, tw, b.height,
			u->view.state == UPDATES_ERROR ? color : fg, PD_LEFT);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

/* ---------- tooltip, click, sample ---------- */

void updates_widget_words(const struct updates_view *v, char *title, size_t title_size,
		char *sub, size_t sub_size) {
	char ago[64] = "";
	if (v->checked) {
		tw_kernel_auto_ago(v->checked, time(NULL), ago, sizeof(ago));
	}
	switch (v->state) {
	case UPDATES_CHECKING:
		snprintf(title, title_size, "Looking for updates…");
		snprintf(sub, sub_size, "Asking the package manager");
		return;
	case UPDATES_INSTALLING:
		if (v->total > 0) {
			snprintf(title, title_size, "Installing %d of %d", v->done, v->total);
		} else {
			snprintf(title, title_size, "%s", v->downloading ? "Downloading the updates…" :
				"Installing the updates…");
		}
		snprintf(sub, sub_size, "%s", v->current[0] ? v->current : "Programs keep running");
		return;
	case UPDATES_CLEANING:
		snprintf(title, title_size, "Cleaning up the package cache…");
		snprintf(sub, sub_size, "Older versions of packages are removed");
		return;
	case UPDATES_ERROR:
		snprintf(title, title_size, "Updates failed");
		snprintf(sub, sub_size, "%s", v->message[0] ? v->message : "See the log");
		return;
	default:
		break;
	}
	if (v->count > 0) {
		if (v->count == 1) {
			snprintf(title, title_size, "An update is available");
		} else {
			snprintf(title, title_size, "%d updates are available", v->count);
		}
	} else if (v->restart) {
		snprintf(title, title_size, "Restart to finish the updates");
	} else if (v->checked) {
		snprintf(title, title_size, "Up to date");
	} else {
		snprintf(title, title_size, "Not checked yet");
	}
	if (v->message[0]) {
		snprintf(sub, sub_size, "%s", v->message);
	} else if (v->restart) {
		snprintf(sub, sub_size, "A new kernel or system parts start with it");
	} else if (v->checked) {
		snprintf(sub, sub_size, "Checked %s", ago);
	} else {
		snprintf(sub, sub_size, "The first check is soon after logging in");
	}
}

static char *updates_tooltip(struct widget *w, struct hotspot *hs) {
	struct updates_state *u = w->data;
	reload(w, false);
	char title[128], sub[256];
	updates_widget_words(&u->view, title, sizeof(title), sub, sizeof(sub));
	return format_str("%s\n%s", title, sub);
}

static bool updates_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
	return true;
}

static bool updates_sample(struct widget *w, struct widget_sample *out) {
	struct updates_state *u = w->data;
	const struct updates_view *v = &u->view;
	out->percent = v->state == UPDATES_INSTALLING && v->total > 0 ?
		v->done * 100 / v->total : -1;
	char title[128], sub[256];
	updates_widget_words(v, title, sizeof(title), sub, sizeof(sub));
	if (v->count > 0 && !busy(v)) {
		snprintf(out->value, sizeof(out->value), "%d", v->count);
	} else {
		snprintf(out->value, sizeof(out->value), "%.*s", (int)sizeof(out->value) - 1,
			v->checked ? (v->restart ? "Restart" : "Up to date") : "–");
	}
	snprintf(out->detail, sizeof(out->detail), "%.*s", (int)sizeof(out->detail) - 1, title);
	out->warning = v->restart;
	out->critical = v->state == UPDATES_ERROR;
	return true;
}

/* What the widget shows, for its flyout: read again now. */
const struct updates_view *updates_widget_state(struct widget *w) {
	struct updates_state *u = w && w->impl == &widget_updates ? w->data : NULL;
	if (!u) {
		return NULL;
	}
	reload(w, false);
	return &u->view;
}

/* ---------- life ---------- */

static void updates_set_active(struct widget *w, bool active) {
	struct updates_state *u = w->data;
	if (active && !u->timer) {
		tick(w);
	} else if (!active && u->timer) {
		loop_remove_timer(w->panel->loop, u->timer);
		u->timer = NULL;
	}
}

static void updates_init(struct widget *w) {
	struct updates_state *u = calloc(1, sizeof(struct updates_state));
	w->data = u;
	u->started = time(NULL);
	read_boot_id(u->boot_id, sizeof(u->boot_id));
	reload(w, true);
}

static void updates_destroy(struct widget *w) {
	struct updates_state *u = w->data;
	if (u->timer) {
		loop_remove_timer(w->panel->loop, u->timer);
	}
	if (u->pid > 0) {
		waitpid(u->pid, NULL, WNOHANG);
	}
	free_list(&u->view);
	free(u);
}

const struct widget_impl widget_updates = {
	.type = "updates",
	.init = updates_init,
	.destroy = updates_destroy,
	.measure = updates_measure,
	.render = updates_render,
	.click = updates_click,
	.tooltip = updates_tooltip,
	.set_active = updates_set_active,
	.sample = updates_sample,
};
