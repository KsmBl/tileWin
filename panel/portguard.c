/*
 * The guard of the ports: a program of the user that listens on a port other
 * computers could reach (any address but the loopback) has to be allowed
 * first, as Windows' firewall asks "Allow access?". Until then
 * tilewin-ports-guard (pkexec, the only part that runs as root) keeps other
 * computers from connecting to it; the program itself runs on and this
 * computer reaches it as before. Off until switched on in the settings.
 *
 * The answer is kept for the program, not for the port or the process, so a
 * program stopped and started again (on another port too) is not asked about
 * twice: its executable, and for an interpreter (python3, node, java, ...) the
 * script or module it runs, so allowing one Python server does not allow
 * every one. It is kept everywhere or for the network the computer is on (the
 * NetworkManager connection with the default route), in ports.conf
 * (tw_portrules.h). "This time only" lets the program in until it ends.
 * Closing the question without an answer blocks it until it starts again.
 *
 * It looks every two seconds, at /proc/net alone while nothing changes there,
 * and at the network every ten. What it does is written to
 * $XDG_RUNTIME_DIR/tilewin-ports-guard-$WAYLAND_DISPLAY.status for the settings ("off", "on",
 * or "broken" and why on the next line), and a notification says so once
 * when it cannot work.
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
#include "tw_portrules.h"
#include "tw_ports.h"

#ifndef TILEWIN_LIBEXECDIR
#define TILEWIN_LIBEXECDIR "/usr/local/libexec"
#endif

#define TICK_MS 2000
#define NETWORK_EVERY 5 // ticks between looks at the network

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
	struct tw_port_rules rules;
	bool guard;          // on in ports.conf
	bool applied;        // the table is there as far as we know
	bool broken;         // the helper failed: tried again only when ports.conf changes
	char *why;           // what failed
	bool told;           // the notification about it was shown
	struct timespec conf_mtime; // and its size: a change in the same second shows too
	off_t conf_size;
	bool conf_read;
	GPtrArray *dismissed; // "pid program": closed without an answer, while it runs
	GPtrArray *once;     // "pid program": allowed this time only, while it runs
	GPtrArray *queue;    // struct question *, the first one on the screen
	struct proc *asking, *applying, *networking;
	bool apply_again;
	char *sent;          // the lists last handed to the helper: "TCP UDP"
	unsigned long last_print;
	struct exposed *exposed;
	int exposed_count;
	char *net_uuid, *net_name; // the network the computer is on, NULL for none
	int ticks;
	char *status_written;
} pg;

static void question_free(void *data) {
	struct question *q = data;
	g_free(q->program);
	g_free(q->name);
	g_free(q);
}

/* ---------- what the settings see ---------- */

static void write_status(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime) {
		return;
	}
	char *text = !pg.guard ? g_strdup("off\n") : pg.broken ?
		g_strdup_printf("broken\n%s\n", pg.why ? pg.why : "") : g_strdup("on\n");
	if (pg.status_written && strcmp(pg.status_written, text) == 0) {
		g_free(text);
		return;
	}
	// one for each session, as the clipboard's socket
	const char *display = getenv("WAYLAND_DISPLAY");
	char *path = g_strdup_printf("%s/tilewin-ports-guard-%s.status", runtime,
		display && *display && !strchr(display, '/') ? display : "wayland");
	g_file_set_contents(path, text, -1, NULL);
	g_free(path);
	g_free(pg.status_written);
	pg.status_written = text;
}

/* ---------- ports.conf ---------- */

static bool read_conf(void) {
	char *path = tw_port_rules_path();
	struct stat st;
	bool there = path && stat(path, &st) == 0;
	struct timespec mtime = there ? st.st_mtim : (struct timespec){ 0 };
	off_t size = there ? st.st_size : -1;
	if (pg.conf_read && mtime.tv_sec == pg.conf_mtime.tv_sec &&
			mtime.tv_nsec == pg.conf_mtime.tv_nsec && size == pg.conf_size) {
		free(path);
		return false;
	}
	pg.conf_read = true;
	pg.conf_mtime = mtime;
	pg.conf_size = size;
	tw_port_rules_read(&pg.rules, path); // none: off, no answers
	pg.guard = pg.rules.guard;
	free(path);
	return true;
}

/* Keeps the answer for a program, everywhere or on the network it is on now. */
static void remember(const char *program, bool allow, bool this_network) {
	char *path = tw_port_rules_path();
	struct tw_port_rules r = { 0 };
	tw_port_rules_read(&r, path);
	r.guard = true; // it asks, so it is on
	const char *network = this_network ? pg.net_uuid : NULL;
	if (network) {
		tw_port_rules_name_network(&r, network, pg.net_name ? pg.net_name : network);
	}
	tw_port_rules_set(&r, program, network, allow);
	if (!tw_port_rules_write(&r, path)) {
		sway_log(SWAY_ERROR, "portguard: cannot write %s", path ? path : "ports.conf");
	}
	tw_port_rules_free(&r);
	free(path);
	pg.conf_read = false; // read again
	read_conf();
}

static bool in_list(GPtrArray *list, int pid, const char *program) {
	char *key = g_strdup_printf("%d %s", pid, program);
	bool found = false;
	for (guint i = 0; list && i < list->len && !found; i++) {
		found = strcmp(list->pdata[i], key) == 0;
	}
	g_free(key);
	return found;
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

/* Why the helper failed, in words, from what it and pkexec said. */
static char *failure(const char *output) {
	if (strstr(output, "nft is not installed")) {
		return g_strdup("nftables is not installed");
	}
	if (strstr(output, "tilewin-ports-guard: not found") || strstr(output, "No such file")) {
		return g_strdup("tilewin-ports-guard is not installed; install tileWin again");
	}
	if (strstr(output, "Not authorized") || strstr(output, "dismissed") ||
			strstr(output, "Request dismissed")) {
		return g_strdup("the administrator's password was not given");
	}
	if (strstr(output, "No authentication agent")) {
		return g_strdup("no password dialog to ask for the administrator's password");
	}
	char *line = g_strdup(output);
	g_strstrip(line);
	char *nl = strchr(line, '\n');
	if (nl) {
		*nl = '\0';
	}
	if (!*line) {
		g_free(line);
		return g_strdup("tilewin-ports-guard failed");
	}
	return line;
}

static void applied(void *data, const char *output) {
	pg.applying = NULL;
	if (!strstr(output, "tilewin-ports-guard: done")) {
		if (!pg.broken) {
			g_free(pg.why);
			pg.why = failure(output);
			sway_log(SWAY_ERROR, "portguard: tilewin-ports-guard failed: %s", pg.why);
			if (!pg.told) {
				pg.told = true;
				char *body = g_strdup_printf("Programs can be reached from other computers "
					"without being asked about: %s.", pg.why);
				notify_local(pg.panel, "Open ports", "dialog-warning",
					"Asking before a program opens a port does not work", body);
				g_free(body);
			}
		}
		pg.broken = true;
		g_free(pg.sent);
		pg.sent = NULL;
		write_status();
		return;
	}
	pg.applied = pg.guard;
	write_status();
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
	char *cmd = g_strdup_printf("%s %s 2>&1 && echo 'tilewin-ports-guard: done'", h, args);
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

/* ---------- the network the computer is on ---------- */

/* nmcli: "NAME:UUID:DEFAULT:DEFAULT6" a line for each connection that is up. */
static void networked(void *data, const char *output) {
	pg.networking = NULL;
	char *uuid = NULL, *name = NULL;
	char **lines = g_strsplit(output, "\n", -1);
	for (char **l = lines; *l && !uuid; l++) {
		// the name may hold an escaped colon (\:); the UUID and the rest have none
		char **f = g_strsplit(*l, ":", -1);
		int n = g_strv_length(f);
		if (n >= 4 && (strcmp(f[n - 2], "yes") == 0 || strcmp(f[n - 1], "yes") == 0)) {
			char *keep = f[n - 3];
			uuid = g_strdup(keep);
			f[n - 3] = NULL; // the name: what comes before the UUID
			char *joined = g_strjoinv(":", f);
			f[n - 3] = keep;
			GString *plain = g_string_new(NULL);
			for (const char *c = joined; *c; c++) {
				if (*c == '\\' && c[1]) {
					c++;
				}
				g_string_append_c(plain, *c);
			}
			name = g_string_free(plain, FALSE);
			g_free(joined);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	bool changed = g_strcmp0(uuid, pg.net_uuid) != 0;
	g_free(pg.net_uuid);
	g_free(pg.net_name);
	pg.net_uuid = uuid;
	pg.net_name = name;
	if (changed) {
		pg.last_print = 0; // the answers of another network may hold now
	}
}

static void look_at_network(void) {
	if (pg.networking) {
		return;
	}
	const char *fake = getenv("TILEWIN_PORTS_NETWORK"); // the tests: "UUID NAME"
	if (fake) {
		const char *space = strchr(fake, ' ');
		char *line = space ? g_strdup_printf("%s:%.*s:yes:no\n", space + 1, (int)(space - fake),
			fake) : g_strdup("");
		networked(NULL, line);
		g_free(line);
		return;
	}
	pg.networking = proc_run(pg.panel, "nmcli -t -f NAME,UUID,DEFAULT,DEFAULT6 connection "
		"show --active 2>/dev/null", false, NULL, networked, NULL);
}

/* ---------- the question ---------- */

static void ask_next(void);

enum { ANSWER_ALLOW = 1, ANSWER_BLOCK = 2, ANSWER_NETWORK = 3, ANSWER_ONCE = 4 };

static void answered(void *data, const char *output) {
	pg.asking = NULL;
	if (!pg.queue || pg.queue->len == 0) {
		return;
	}
	struct question *q = pg.queue->pdata[0];
	switch (atoi(output)) {
	case ANSWER_ALLOW:
		remember(q->program, true, false);
		break;
	case ANSWER_NETWORK:
		remember(q->program, true, true);
		break;
	case ANSWER_BLOCK:
		remember(q->program, false, false);
		break;
	case ANSWER_ONCE:
		g_ptr_array_add(pg.once, g_strdup_printf("%d %s", q->pid, q->program));
		break;
	default:
		// closed: blocked until it starts again
		g_ptr_array_add(pg.dismissed, g_strdup_printf("%d %s", q->pid, q->program));
		break;
	}
	g_ptr_array_remove_index(pg.queue, 0);
	pg.last_print = 0; // look again now, with the answer
	ask_next();
}

/* A value of the key file; in a list (the buttons) a ; is escaped too. */
static char *keyfile_escape(const char *text, bool list) {
	GString *s = g_string_new(NULL);
	for (const char *c = text; *c; c++) {
		if (*c == '\n') {
			g_string_append(s, "\\n");
		} else if (*c == '\\') {
			g_string_append(s, "\\\\");
		} else if (*c == ';' && list) {
			g_string_append(s, "\\;");
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
	char *t = keyfile_escape(text, false), *s = keyfile_escape(secondary, false);
	// on this network only, when the computer is on one
	char *net_label = NULL;
	if (pg.net_name && *pg.net_name) {
		char *label = g_strdup_printf("Only on “%s”", pg.net_name);
		// a _ in the name would be taken for the key of the button
		GString *plain = g_string_new(NULL);
		for (const char *c = label; *c; c++) {
			if (*c == '_') {
				g_string_append_c(plain, '_');
			}
			g_string_append_c(plain, *c);
		}
		g_free(label);
		net_label = keyfile_escape(plain->str, true);
		g_string_free(plain, TRUE);
	}
	char *keyfile = g_strdup_printf("[dialog]\ntitle=Open port\ntext=%s\nsecondary=%s\n"
		"markup=false\nsecondary_markup=false\ntype=warning\n"
		"labels=_Allow;%s%s_This time only;_Block;\nresponses=1;%s4;2;\ndefault=1\n", t, s,
		net_label ? net_label : "", net_label ? ";" : "", net_label ? "3;" : "");
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
	g_free(net_label);
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

/* Forgets what was said for processes that have ended. */
static void prune(GPtrArray *list) {
	for (guint i = 0; list && i < list->len;) {
		int pid = atoi(list->pdata[i]);
		char path[32];
		snprintf(path, sizeof(path), "/proc/%d", pid);
		if (access(path, F_OK) != 0) {
			g_ptr_array_remove_index(list, i);
		} else {
			i++;
		}
	}
}

static void look(bool force) {
	struct tw_ports p = { 0 };
	tw_ports_read(&p, false);
	unsigned long print = print_of(&p);
	if (print == pg.last_print && !force) {
		tw_ports_free(&p);
		return;
	}
	pg.last_print = print;
	prune(pg.once);
	prune(pg.dismissed);
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
		const struct tw_port_rule *r = tw_port_rules_find(&pg.rules, x->program, pg.net_uuid);
		if (in_list(pg.once, e->pid, x->program)) {
			x->verdict = PG_ALLOWED;
		} else if (r) {
			x->verdict = r->allow ? PG_ALLOWED : PG_BLOCKED;
		} else if (in_list(pg.dismissed, e->pid, x->program)) {
			x->verdict = PG_BLOCKED;
		} else {
			x->verdict = PG_ASKING;
			if (pg.guard && !pg.broken && !queued(x->program)) {
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
		pg.told = false;
		if (was_guarded != pg.guard) {
			g_free(pg.sent);
			pg.sent = NULL;
		}
	}
	if (pg.guard && pg.ticks++ % NETWORK_EVERY == 0) {
		look_at_network();
	}
	look(changed);
	write_status();
	pg.timer = loop_add_timer_lazy(pg.panel->loop, TICK_MS, tick, NULL);
}

void portguard_init(struct panel *panel) {
	const char *off = getenv("TILEWIN_NO_PORT_GUARD");
	if (off && *off) {
		return;
	}
	pg.panel = panel;
	pg.dismissed = g_ptr_array_new_with_free_func(g_free);
	pg.once = g_ptr_array_new_with_free_func(g_free);
	pg.queue = g_ptr_array_new_with_free_func(question_free);
	// on: the table at once, with no port allowed yet; the next look opens those allowed.
	// Off: nothing is asked of pkexec at all
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
	proc_cancel(pg.networking);
	clear_exposed();
	tw_port_rules_free(&pg.rules);
	g_ptr_array_free(pg.dismissed, TRUE);
	g_ptr_array_free(pg.once, TRUE);
	g_ptr_array_free(pg.queue, TRUE);
	g_free(pg.sent);
	g_free(pg.why);
	g_free(pg.net_uuid);
	g_free(pg.net_name);
	g_free(pg.status_written);
	memset(&pg, 0, sizeof(pg));
}

bool portguard_active(void) {
	return pg.panel && pg.guard && !pg.broken;
}

static const struct exposed *exposed_at(unsigned port, bool tcp) {
	for (int i = 0; i < pg.exposed_count; i++) {
		const struct exposed *e = &pg.exposed[i];
		if (e->port == port && (tcp ? e->tcp : e->udp)) {
			return e;
		}
	}
	return NULL;
}

enum portguard_verdict portguard_verdict(unsigned port, bool tcp) {
	if (!portguard_active()) {
		return PG_NONE;
	}
	const struct exposed *e = exposed_at(port, tcp);
	return e ? e->verdict : PG_NONE;
}

const char *portguard_network_name(void) {
	return pg.net_name;
}

/* "ports allow|block|once|network|forget PORT tcp|udp", from the menu of the flyout. */
void portguard_command(int argc, char **argv) {
	if (!pg.panel || argc < 3) {
		return;
	}
	const char *what = argv[0];
	const struct exposed *e = exposed_at((unsigned)atoi(argv[1]), strcmp(argv[2], "udp") != 0);
	if (!e || !e->program) {
		return;
	}
	char *program = g_strdup(e->program);
	int pid = e->pid;
	if (strcmp(what, "allow") == 0 || strcmp(what, "block") == 0) {
		// everywhere, this network too: its own answer would win
		if (pg.net_uuid) {
			char *path = tw_port_rules_path();
			struct tw_port_rules r = { 0 };
			tw_port_rules_read(&r, path);
			tw_port_rules_forget(&r, program, pg.net_uuid);
			tw_port_rules_write(&r, path);
			tw_port_rules_free(&r);
			free(path);
		}
		remember(program, what[0] == 'a', false);
	} else if (strcmp(what, "network") == 0 && pg.net_uuid) {
		remember(program, true, true);
	} else if (strcmp(what, "once") == 0) {
		g_ptr_array_add(pg.once, g_strdup_printf("%d %s", pid, program));
	} else if (strcmp(what, "forget") == 0) {
		char *path = tw_port_rules_path();
		struct tw_port_rules r = { 0 };
		tw_port_rules_read(&r, path);
		tw_port_rules_forget(&r, program, NULL);
		tw_port_rules_forget(&r, program, pg.net_uuid);
		tw_port_rules_write(&r, path);
		tw_port_rules_free(&r);
		free(path);
		pg.conf_read = false;
		read_conf();
	}
	g_free(program);
	look(true);
}
