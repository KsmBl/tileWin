/*
 * The guard of the ports: a program of the user that listens on a port other
 * computers could reach (any address but the loopback) has to be allowed
 * first, as Windows' firewall asks "Allow access?". Until then
 * tilewin-ports-guard (pkexec, the only part that runs as root) keeps other
 * computers from connecting to it; the program itself runs on and this
 * computer reaches it as before.
 *
 * The answer is kept for the program, not for the port or the process, so a
 * program stopped and started again (on another port too) is not asked about
 * twice: its executable, and for an interpreter (python3, node, java, ...) the
 * script or module it runs, so allowing one Python server does not allow
 * every one. ~/.config/tileWin/ports.conf:
 *
 *   guard yes|no
 *   allow /usr/bin/python3 -m http.server
 *   block /home/me/bin/server
 *
 * Closing the question without an answer blocks the program until it starts
 * again, when it is asked once more. It looks every two seconds, at /proc/net
 * alone while nothing changes there.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "log.h"
#include "panel.h"
#include "portguard.h"
#include "tw_paths.h"
#include "tw_ports.h"

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

#define TICK_MS 2000

struct rule {
	char *program;
	bool allow;
};

struct question {
	char *program, *name;
	int pid;
	unsigned port;
	bool tcp, udp;
	char address[48];
	bool any;
};

/* A port of the user's open to the network, and whose it is. */
struct exposed {
	unsigned port;
	bool tcp, udp;
	char *program; // NULL when its process is gone
	int pid;
	enum portguard_verdict verdict;
};

static struct {
	struct panel *panel;
	struct loop_timer *timer;
	bool guard;          // on in ports.conf
	bool applied;        // the table is there as far as we know
	bool broken;         // the helper failed: tried again only when ports.conf changes
	time_t conf_mtime;
	GPtrArray *rules;    // struct rule *
	GPtrArray *dismissed; // "pid program": closed without an answer, while it runs
	GPtrArray *queue;    // struct question *, the first one on the screen
	struct proc *asking, *applying;
	bool apply_again;
	char *sent;          // the lists last handed to the helper: "TCP UDP"
	unsigned long last_print;
	struct exposed *exposed;
	int exposed_count;
} pg;

static char *conf_path(void) {
	char *dir = tw_config_dir();
	char *path = dir ? g_strdup_printf("%s/ports.conf", dir) : NULL;
	free(dir);
	return path;
}

static void rule_free(void *data) {
	struct rule *r = data;
	g_free(r->program);
	g_free(r);
}

static void question_free(void *data) {
	struct question *q = data;
	g_free(q->program);
	g_free(q->name);
	g_free(q);
}

/* ---------- ports.conf ---------- */

static bool read_conf(void) {
	char *path = conf_path();
	struct stat st;
	time_t mtime = path && stat(path, &st) == 0 ? st.st_mtime : 0;
	if (pg.rules && mtime == pg.conf_mtime) {
		g_free(path);
		return false;
	}
	pg.conf_mtime = mtime;
	if (pg.rules) {
		g_ptr_array_free(pg.rules, TRUE);
	}
	pg.rules = g_ptr_array_new_with_free_func(rule_free);
	pg.guard = true; // on unless switched off
	char *text = NULL;
	if (path && g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l; l++) {
			char *line = g_strstrip(*l);
			if (g_str_has_prefix(line, "guard ")) {
				char *v = g_strstrip(line + 6);
				pg.guard = strcmp(v, "no") != 0 && strcmp(v, "off") != 0 &&
					strcmp(v, "false") != 0;
			} else if (g_str_has_prefix(line, "allow ") || g_str_has_prefix(line, "block ")) {
				struct rule *r = g_new0(struct rule, 1);
				r->allow = line[0] == 'a';
				r->program = g_strdup(g_strstrip(line + 6));
				g_ptr_array_add(pg.rules, r);
			}
		}
		g_strfreev(lines);
	}
	g_free(text);
	g_free(path);
	return true;
}

/* Keeps the answer for a program in ports.conf, in place of an older one. */
static void remember(const char *program, bool allow) {
	char *path = conf_path();
	if (!path) {
		return;
	}
	char *text = NULL;
	GString *out = g_string_new(NULL);
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l; l++) {
			char *copy = g_strdup(*l);
			char *line = g_strstrip(copy);
			bool same = (g_str_has_prefix(line, "allow ") || g_str_has_prefix(line, "block ")) &&
				strcmp(g_strstrip(line + 6), program) == 0;
			if (!same && (**l || l[1])) {
				g_string_append_printf(out, "%s\n", *l);
			}
			g_free(copy);
		}
		g_strfreev(lines);
	} else {
		g_string_append(out, "# Which programs other computers may reach: the answers to tileWin's\n"
			"# question, for each program (\"guard no\" switches the questions off)\n");
	}
	g_string_append_printf(out, "%s %s\n", allow ? "allow" : "block", program);
	char *dir = g_path_get_dirname(path);
	tw_mkdir_p(dir);
	g_free(dir);
	if (!g_file_set_contents(path, out->str, out->len, NULL)) {
		sway_log(SWAY_ERROR, "portguard: cannot write %s", path);
	}
	g_string_free(out, TRUE);
	g_free(text);
	g_free(path);
	pg.conf_mtime = -1; // read again
}

static const struct rule *rule_for(const char *program) {
	for (guint i = 0; pg.rules && i < pg.rules->len; i++) {
		const struct rule *r = pg.rules->pdata[i];
		if (strcmp(r->program, program) == 0) {
			return r;
		}
	}
	return NULL;
}

/* ---------- which program ---------- */

static bool interpreter(const char *exe) {
	const char *base = strrchr(exe, '/');
	base = base ? base + 1 : exe;
	static const char *const names[] = { "python", "node", "nodejs", "ruby", "perl", "php", "java",
		"bash", "sh", "dash", "zsh", "fish", "lua", "luajit", "deno", "bun", "tclsh", "Rscript",
		"pypy", "pypy3", "dotnet", "mono", NULL };
	for (int i = 0; names[i]; i++) {
		size_t n = strlen(names[i]);
		// python3, python3.13, lua5.4 count as python and lua
		if (strncmp(base, names[i], n) == 0 &&
				(base[n] == '\0' || isdigit((unsigned char)base[n]))) {
			return true;
		}
	}
	return false;
}

/*
 * What the answer is kept for: the executable, and for an interpreter what it
 * runs ("-m http.server", "-jar /opt/x.jar", the script as a full path).
 */
static char *program_of(int pid) {
	char link[64], exe[PATH_MAX];
	snprintf(link, sizeof(link), "/proc/%d/exe", pid);
	ssize_t n = readlink(link, exe, sizeof(exe) - 1);
	if (n <= 0) {
		return NULL;
	}
	exe[n] = '\0';
	char *deleted = strstr(exe, " (deleted)"); // updated while it ran
	if (deleted && deleted[10] == '\0') {
		*deleted = '\0';
	}
	if (!interpreter(exe)) {
		return g_strdup(exe);
	}
	char path[64], *args = NULL;
	gsize len = 0;
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if (!g_file_get_contents(path, &args, &len, NULL) || len == 0) {
		g_free(args);
		return g_strdup(exe);
	}
	GString *key = g_string_new(exe);
	const char *end = args + len;
	const char *arg = args + strlen(args) + 1; // past argv[0]
	for (; arg < end; arg += strlen(arg) + 1) {
		if (strcmp(arg, "-m") == 0 || strcmp(arg, "-jar") == 0) {
			const char *what = arg + strlen(arg) + 1;
			if (what < end) {
				g_string_append_printf(key, " %s %s", arg, what);
			}
			break;
		}
		if (arg[0] == '-') {
			continue;
		}
		if (arg[0] == '/') {
			g_string_append_printf(key, " %s", arg);
		} else {
			char cwd_link[64], cwd[PATH_MAX];
			snprintf(cwd_link, sizeof(cwd_link), "/proc/%d/cwd", pid);
			ssize_t c = readlink(cwd_link, cwd, sizeof(cwd) - 1);
			cwd[c > 0 ? c : 0] = '\0';
			char *full = g_build_filename(c > 0 ? cwd : "/", arg, NULL);
			char *canon = g_canonicalize_filename(full, NULL);
			g_string_append_printf(key, " %s", canon);
			g_free(canon);
			g_free(full);
		}
		break;
	}
	g_free(args);
	return g_string_free(key, FALSE);
}

/* ---------- the helper ---------- */

static void apply_ports(void);

static void applied(void *data, const char *output) {
	pg.applying = NULL;
	if (!strstr(output, "tilewin-ports-guard: done")) {
		if (!pg.broken) {
			sway_log(SWAY_ERROR, "portguard: tilewin-ports-guard failed; programs on public "
				"ports are not asked about until ports.conf changes");
		}
		pg.broken = true;
		g_free(pg.sent);
		pg.sent = NULL;
		return;
	}
	pg.applied = pg.guard;
	if (pg.apply_again) {
		pg.apply_again = false;
		apply_ports();
	}
}

static char *helper(void) {
	const char *env = getenv("TILEWIN_PORTS_GUARD"); // the tests: without pkexec
	if (env && *env) {
		return g_strdup(env);
	}
	return g_strdup("pkexec " TILEWIN_LIBEXECDIR "/tilewin-ports-guard");
}

static void run_helper(const char *args) {
	char *h = helper();
	char *cmd = g_strdup_printf("%s %s && echo 'tilewin-ports-guard: done'", h, args);
	pg.applying = proc_run(pg.panel, cmd, false, NULL, applied, NULL);
	g_free(cmd);
	g_free(h);
}

/* Hands the ports of the programs allowed to the helper, when they changed. */
static void apply_ports(void) {
	if (pg.broken) {
		return;
	}
	if (pg.applying) {
		pg.apply_again = true;
		return;
	}
	if (!pg.guard) {
		if (pg.applied) {
			g_free(pg.sent);
			pg.sent = NULL;
			pg.applied = false;
			run_helper("stop");
		}
		return;
	}
	GString *tcp = g_string_new(NULL), *udp = g_string_new(NULL);
	for (int i = 0; i < pg.exposed_count; i++) {
		const struct exposed *e = &pg.exposed[i];
		if (e->verdict != PG_ALLOWED) {
			continue;
		}
		if (e->tcp) {
			g_string_append_printf(tcp, "%s%u", tcp->len ? "," : "", e->port);
		}
		if (e->udp) {
			g_string_append_printf(udp, "%s%u", udp->len ? "," : "", e->port);
		}
	}
	char *lists = g_strdup_printf("%s %s", tcp->len ? tcp->str : "-", udp->len ? udp->str : "-");
	g_string_free(tcp, TRUE);
	g_string_free(udp, TRUE);
	if (pg.sent && strcmp(pg.sent, lists) == 0) {
		g_free(lists);
		return;
	}
	g_free(pg.sent);
	pg.sent = lists;
	char *args = g_strdup_printf("apply %s", lists);
	run_helper(args);
	g_free(args);
}

/* ---------- the question ---------- */

static void ask_next(void);

static void answered(void *data, const char *output) {
	pg.asking = NULL;
	if (!pg.queue || pg.queue->len == 0) {
		return;
	}
	struct question *q = pg.queue->pdata[0];
	int response = atoi(output);
	if (response == 1 || response == 2) {
		remember(q->program, response == 1);
		read_conf();
	} else {
		// closed: blocked until it starts again
		g_ptr_array_add(pg.dismissed, g_strdup_printf("%d %s", q->pid, q->program));
	}
	g_ptr_array_remove_index(pg.queue, 0);
	pg.last_print = 0; // look again now, with the answer
	ask_next();
}

static char *keyfile_escape(const char *text) {
	GString *s = g_string_new(NULL);
	for (const char *c = text; *c; c++) {
		if (*c == '\n') {
			g_string_append(s, "\\n");
		} else if (*c == '\\') {
			g_string_append(s, "\\\\");
		} else {
			g_string_append_c(s, *c);
		}
	}
	return g_string_free(s, FALSE);
}

static void ask_next(void) {
	if (pg.asking || !pg.queue || pg.queue->len == 0) {
		return;
	}
	struct question *q = pg.queue->pdata[0];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char *file = g_strdup_printf("%s/tilewin-ports-question-%d", runtime && *runtime ? runtime :
		"/tmp", getpid());
	char *text = g_strdup_printf("Allow %s to be reached from other computers?", q->name);
	char *where = q->any ? g_strdup("on all addresses: every network this computer is on") :
		g_strdup_printf("on %s", q->address);
	char *secondary = g_strdup_printf("%s listens on port %u (%s) %s.\n\n"
		"Until you allow it, other computers cannot connect to it; this computer still can. "
		"The answer is kept for this program, also when it is started again.",
		q->program, q->port, q->tcp && q->udp ? "TCP and UDP" : q->udp ? "UDP" : "TCP", where);
	char *t = keyfile_escape(text), *s = keyfile_escape(secondary);
	char *keyfile = g_strdup_printf("[dialog]\ntitle=Open port\ntext=%s\nsecondary=%s\n"
		"markup=false\nsecondary_markup=false\ntype=warning\nlabels=_Allow;_Block;\n"
		"responses=1;2;\ndefault=1\n", t, s);
	bool ok = g_file_set_contents(file, keyfile, -1, NULL);
	if (ok) {
		chmod(file, 0600);
		const char *dialog = getenv("TILEWIN_DIALOG");
		char *quoted = g_shell_quote(file);
		char *cmd = g_strdup_printf("%s < %s; rm -f %s", dialog && *dialog ? dialog :
			"tilewin-dialog", quoted, quoted);
		pg.asking = proc_run(pg.panel, cmd, false, NULL, answered, NULL);
		g_free(cmd);
		g_free(quoted);
	}
	g_free(keyfile);
	g_free(t);
	g_free(s);
	g_free(secondary);
	g_free(where);
	g_free(text);
	g_free(file);
	if (!pg.asking) {
		g_ptr_array_remove_index(pg.queue, 0);
	}
}

static bool queued(const char *program) {
	for (guint i = 0; pg.queue && i < pg.queue->len; i++) {
		const struct question *q = pg.queue->pdata[i];
		if (strcmp(q->program, program) == 0) {
			return true;
		}
	}
	return false;
}

static bool dismissed(int pid, const char *program) {
	char *key = g_strdup_printf("%d %s", pid, program);
	bool found = false;
	for (guint i = 0; pg.dismissed && i < pg.dismissed->len && !found; i++) {
		found = strcmp(pg.dismissed->pdata[i], key) == 0;
	}
	g_free(key);
	return found;
}

/* ---------- looking ---------- */

static void clear_exposed(void) {
	for (int i = 0; i < pg.exposed_count; i++) {
		g_free(pg.exposed[i].program);
	}
	g_free(pg.exposed);
	pg.exposed = NULL;
	pg.exposed_count = 0;
}

/* What of the ports of the user's own can be reached from elsewhere. */
static unsigned long print_of(const struct tw_ports *p) {
	unsigned long h = 5381;
	uid_t me = getuid();
	for (int i = 0; i < p->count; i++) {
		const struct tw_port *e = &p->items[i];
		if (e->scope != TW_PORT_LOCAL && e->uid == me) {
			h = h * 33 + e->port * 4 + e->tcp * 2 + e->udp;
		}
	}
	return h | 1; // never 0, which means "not looked yet"
}

static void look(bool conf_changed) {
	struct tw_ports p = { 0 };
	tw_ports_read(&p, false);
	unsigned long print = print_of(&p);
	if (print == pg.last_print && !conf_changed) {
		tw_ports_free(&p);
		return;
	}
	pg.last_print = print;
	tw_ports_read(&p, true); // now with the programs
	clear_exposed();
	uid_t me = getuid();
	pg.exposed = g_new0(struct exposed, p.count ? p.count : 1);
	for (int i = 0; i < p.count; i++) {
		const struct tw_port *e = &p.items[i];
		if (e->scope == TW_PORT_LOCAL || e->uid != me) {
			continue;
		}
		struct exposed *x = &pg.exposed[pg.exposed_count++];
		x->port = e->port;
		x->tcp = e->tcp;
		x->udp = e->udp;
		x->pid = e->pid;
		x->program = e->pid ? program_of(e->pid) : NULL;
		if (!x->program) {
			x->verdict = PG_BLOCKED;
			continue;
		}
		const struct rule *r = rule_for(x->program);
		if (r) {
			x->verdict = r->allow ? PG_ALLOWED : PG_BLOCKED;
			continue;
		}
		x->verdict = PG_ASKING;
		if (dismissed(e->pid, x->program)) {
			x->verdict = PG_BLOCKED;
		} else if (pg.guard && !pg.broken && !queued(x->program)) {
			struct question *q = g_new0(struct question, 1);
			q->program = g_strdup(x->program);
			char name[64];
			q->name = g_strdup(tw_port_name(e, name, sizeof(name)));
			q->pid = e->pid;
			q->port = e->port;
			q->tcp = e->tcp;
			q->udp = e->udp;
			q->any = e->any;
			snprintf(q->address, sizeof(q->address), "%s", e->address);
			g_ptr_array_add(pg.queue, q);
		}
	}
	tw_ports_free(&p);
	apply_ports();
	ask_next();
}

static void tick(void *data) {
	pg.timer = NULL;
	bool was_guarded = pg.guard;
	bool changed = read_conf();
	if (changed) {
		pg.broken = false; // ports.conf changed: try the helper again
		if (was_guarded != pg.guard) {
			g_free(pg.sent);
			pg.sent = NULL;
		}
	}
	look(changed);
	pg.timer = loop_add_timer_lazy(pg.panel->loop, TICK_MS, tick, NULL);
}

void portguard_init(struct panel *panel) {
	const char *off = getenv("TILEWIN_NO_PORT_GUARD");
	if (off && *off) {
		return;
	}
	pg.panel = panel;
	pg.dismissed = g_ptr_array_new_with_free_func(g_free);
	pg.queue = g_ptr_array_new_with_free_func(question_free);
	read_conf();
	// the table at once, with no port allowed yet; the next look opens those allowed
	pg.applied = !pg.guard; // off: one stop, in case it was on before
	tick(NULL);
}

void portguard_fini(void) {
	if (!pg.panel) {
		return;
	}
	if (pg.timer) {
		loop_remove_timer(pg.panel->loop, pg.timer);
	}
	proc_cancel(pg.asking);
	proc_cancel(pg.applying);
	clear_exposed();
	if (pg.rules) {
		g_ptr_array_free(pg.rules, TRUE);
	}
	g_ptr_array_free(pg.dismissed, TRUE);
	g_ptr_array_free(pg.queue, TRUE);
	g_free(pg.sent);
	memset(&pg, 0, sizeof(pg));
}

bool portguard_active(void) {
	return pg.panel && pg.guard && !pg.broken;
}

enum portguard_verdict portguard_verdict(unsigned port, bool tcp) {
	if (!portguard_active()) {
		return PG_NONE;
	}
	for (int i = 0; i < pg.exposed_count; i++) {
		const struct exposed *e = &pg.exposed[i];
		if (e->port == port && (tcp ? e->tcp : e->udp)) {
			return e->verdict;
		}
	}
	return PG_NONE;
}
