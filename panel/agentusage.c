#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>
#include <json.h>
#include <poll.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "agentusage.h"
#include "tw_agent_accounts.h"
#include "tw_codex_usage.h"
#include "tw_paths.h"

#define DAY 86400
#define MAX_LINE (16 * 1024 * 1024)

const char *const agent_names[AGENT_COUNT] = { "Claude", "Codex", "Copilot", "Antigravity" };
static const char *const commands[] = { "claude", "codex", "copilot", "agy" };

struct session {
	char *id, *model, *effort;
	time_t at, mtime;
	long mtime_ns;
	dev_t device;
	ino_t inode;
	off_t offset;
	double previous_tokens;
	bool modern_codex;
};

struct collect {
	struct agent_usage *usage;
	json_object *events;
	GHashTable *sessions[AGENT_COUNT], *logs;
	bool events_changed;
};

static json_object *field(json_object *o, const char *key) {
	json_object *value = NULL;
	json_object_object_get_ex(o, key, &value);
	return value;
}

static const char *str(json_object *o, const char *key) {
	json_object *v = field(o, key);
	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}

static double number(json_object *o, const char *key) {
	json_object *v = field(o, key);
	return json_object_is_type(v, json_type_double) || json_object_is_type(v, json_type_int)
		? json_object_get_double(v) : 0;
}

static double tokens(json_object *o, const char *key) {
	return CLAMP(number(o, key), 0, 1e12);
}

static time_t timestamp(json_object *o, const char *key) {
	const char *s = str(o, key);
	if (!s) {
		double value = number(o, key);
		return value > 0 && value < 253402300800.0 ? (time_t)value : 0;
	}
	GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
	time_t result = dt ? g_date_time_to_unix(dt) : 0;
	if (dt) {
		g_date_time_unref(dt);
	}
	return result;
}

static bool equal(const char *a, const char *b) {
	return g_strcmp0(a, b) == 0;
}

static char *label(const char *s) {
	if (!s || !*s) {
		return NULL;
	}
	char *prefix = g_strndup(s, 95);
	char *valid = g_utf8_make_valid(prefix, -1);
	g_free(prefix);
	return valid;
}

static void replace(char **dest, const char *value) {
	if (value && *value) {
		g_free(*dest);
		*dest = label(value);
	}
}

static void session_free(gpointer ptr) {
	struct session *s = ptr;
	g_free(s->id);
	g_free(s->model);
	g_free(s->effort);
	g_free(s);
}

static void worker_free(gpointer ptr) {
	struct agent_worker *w = ptr;
	g_free(w->session);
	g_free(w->model);
	g_free(w->effort);
	g_free(w->tty);
	g_free(w);
}

static void model_free(gpointer ptr) {
	struct agent_model *m = ptr;
	g_free(m->name);
	g_free(m);
}

static void limit_free(gpointer ptr) {
	struct agent_limit *limit = ptr;
	g_free(limit->name);
	free(limit->chart.points);
	tw_claude_usage_finish(&limit->usage);
	g_free(limit);
}

struct agent_usage *agent_usage_ref(const struct agent_usage *usage) {
	struct agent_usage *owned = (struct agent_usage *)usage;
	if (owned) {
		g_atomic_ref_count_inc(&owned->refs);
	}
	return owned;
}

void agent_usage_free(struct agent_usage *usage) {
	if (usage && g_atomic_ref_count_dec(&usage->refs)) {
		g_ptr_array_unref(usage->workers);
		g_ptr_array_unref(usage->models);
		g_ptr_array_unref(usage->limits);
		g_free(usage->copilot_bucket);
		g_free(usage);
	}
}

static void remember_event(struct collect *c, enum agent_kind kind, const char *id,
		const char *model, time_t at, double count, bool evidence_only) {
	if (!id || !*id || at <= c->usage->now - AGENT_DAYS * DAY || at > time(NULL) + 300 ||
			(!evidence_only && (!model || !*model || !(count > 0) || count > 1e13))) {
		return;
	}
	char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
	char *key = g_strdup_printf("%d:%s", kind, hash);
	json_object *event = json_object_new_object();
	char *name = label(model);
	json_object_object_add(event, "model", name ? json_object_new_string(name) : NULL);
	json_object_object_add(event, "at", json_object_new_int64(at));
	json_object_object_add(event, "tokens", json_object_new_double(count));
	if (json_object_equal(field(c->events, key), event)) {
		json_object_put(event);
	} else {
		json_object_object_add(c->events, key, event);
		c->events_changed = true;
	}
	g_free(name);
	g_free(key);
	g_free(hash);
}

static void update_session(struct session *s, const char *model, const char *effort, time_t at) {
	if (model && !equal(model, s->model)) {
		g_clear_pointer(&s->effort, g_free);
	}
	replace(&s->model, model);
	replace(&s->effort, effort);
	s->at = MAX(at, s->at);
}

static void claude_event(struct collect *c, struct session *s, json_object *o, time_t at) {
	const char *type = str(o, "type");
	if (equal(type, "api-request")) {
		json_object *params = field(o, "params");
		update_session(s, str(params, "model"), str(field(params, "output_config"), "effort"), at);
		return;
	}
	if (!equal(type, "assistant")) {
		return;
	}
	json_object *m = field(o, "message"), *u = field(m, "usage");
	const char *model = str(m, "model");
	const char *id = str(m, "id");
	if (!id) {
		id = str(o, "uuid");
	}
	update_session(s, model, str(m, "effort"), at);
	double count = tokens(u, "input_tokens") + tokens(u, "output_tokens") +
		tokens(u, "cache_creation_input_tokens") + tokens(u, "cache_read_input_tokens");
	remember_event(c, AGENT_CLAUDE, id, model, at, count, false);
}

static void copilot_event(struct collect *c, struct session *s, json_object *o, time_t at) {
	const char *type = str(o, "type");
	json_object *p = field(o, "data");
	if (equal(type, "session.model_change")) {
		update_session(s, str(p, "newModel"), str(p, "reasoningEffort"), at);
	} else if (equal(type, "session.auto_mode_resolved")) {
		update_session(s, str(p, "chosenModel"), NULL, at);
	} else if (equal(type, "session.usage_record")) {
		json_object *u = field(p, "usage");
		const char *model = str(u, "model"), *id = str(u, "apiCallId");
		const char *effort = str(u, "reasoningEffort");
		update_session(s, model, effort, at);
		remember_event(c, AGENT_COPILOT, id && *id ? id : str(o, "id"), model, at,
			tokens(u, "inputTokens") + tokens(u, "outputTokens"), false);
	}
}

static void codex_event(struct collect *c, struct session *s, json_object *o, time_t at) {
	const char *type = str(o, "type");
	json_object *p = field(o, "payload");
	if (equal(type, "session_meta")) {
		replace(&s->id, str(p, "id"));
	} else if (equal(type, "turn_context")) {
		update_session(s, str(p, "model"), str(p, "effort"), at);
	} else if (equal(type, "token_usage_record")) {
		s->modern_codex = true;
		const char *id = str(p, "response_id");
		double count = tokens(field(p, "usage"), "total_tokens");
		remember_event(c, AGENT_CODEX, id, s->model, at, count, count > 0 && !s->model);
	} else if (!s->modern_codex && equal(type, "event_msg") && equal(str(p, "type"), "token_count")) {
		json_object *info = field(p, "info");
		double total = tokens(field(info, "total_token_usage"), "total_tokens");
		if (total > 0 && total != s->previous_tokens) {
			double count = tokens(field(info, "last_token_usage"), "total_tokens");
			if (!count && total > s->previous_tokens) {
				count = total - s->previous_tokens;
			}
			char *id = g_strdup_printf("%s:%s:%.0f", s->id, str(o, "timestamp"), total);
			remember_event(c, AGENT_CODEX, id, s->model, at, count, count > 0 && !s->model);
			g_free(id);
			s->previous_tokens = total;
		}
	}
}

static void read_log(struct collect *c, const char *path, const char *id, enum agent_kind kind,
		const struct stat *st) {
	struct session *s = g_hash_table_lookup(c->logs, path);
	if (s && (s->device != st->st_dev || s->inode != st->st_ino || s->offset > st->st_size ||
			(s->offset == st->st_size &&
				(s->mtime != st->st_mtime || s->mtime_ns != st->st_mtim.tv_nsec)))) {
		g_hash_table_remove(c->logs, path);
		s = NULL;
	}
	if (s && s->offset == st->st_size) {
		g_hash_table_insert(c->sessions[kind], g_strdup(path), s);
		return;
	}
	FILE *file = fopen(path, "re");
	if (!file) {
		return;
	}
	if (!s) {
		s = g_new0(struct session, 1);
		s->id = g_strdup(id);
		s->device = st->st_dev;
		s->inode = st->st_ino;
		g_hash_table_insert(c->logs, g_strdup(path), s);
	}
	if (fseeko(file, s->offset, SEEK_SET) != 0) {
		fclose(file);
		return;
	}
	s->mtime = st->st_mtime;
	s->mtime_ns = st->st_mtim.tv_nsec;
	char *line = NULL;
	size_t capacity = 0;
	ssize_t len;
	while ((len = getline(&line, &capacity, file)) >= 0) {
		if (line[len - 1] != '\n') {
			break; // revisit the unfinished last line after the writer extends it
		}
		s->offset += len;
		if (len > MAX_LINE) {
			continue;
		}
		const char *const markers[] = { "\"assistant\"", "\"token_", "\"session.", NULL };
		if (!strstr(line, markers[kind]) && !(kind == AGENT_CODEX &&
				(strstr(line, "\"turn_context\"") || strstr(line, "\"session_meta\""))) &&
				!(kind == AGENT_CLAUDE && strstr(line, "\"api-request\""))) {
			continue;
		}
		json_object *o = json_tokener_parse(line);
		time_t at = timestamp(o, "timestamp");
		if (kind == AGENT_CLAUDE) {
			claude_event(c, s, o, at);
		} else if (kind == AGENT_CODEX) {
			codex_event(c, s, o, at);
		} else {
			copilot_event(c, s, o, at);
		}
		json_object_put(o);
	}
	free(line);
	fclose(file);
	g_hash_table_insert(c->sessions[kind], g_strdup(path), s);
}

static void scan_logs(struct collect *c, const char *dir, enum agent_kind kind, int depth) {
	GDir *d = g_dir_open(dir, 0, NULL);
	if (!d) {
		return;
	}
	const char *name;
	while ((name = g_dir_read_name(d))) {
		char *path = g_build_filename(dir, name, NULL);
		struct stat st;
		if (lstat(path, &st) == 0) {
			if (S_ISDIR(st.st_mode) && depth && name[0] != '.') {
				scan_logs(c, path, kind, depth - 1);
			} else if (S_ISREG(st.st_mode) && st.st_mtime > c->usage->now - AGENT_DAYS * DAY &&
					(kind == AGENT_COPILOT ? equal(name, "events.jsonl") :
					g_str_has_suffix(name, ".jsonl"))) {
				char *id = kind == AGENT_COPILOT ? g_path_get_basename(dir) :
					g_strndup(name, strlen(name) - strlen(".jsonl"));
				read_log(c, path, id, kind, &st);
				g_free(id);
			}
		}
		g_free(path);
	}
	g_dir_close(d);
}

static int kind_of(const char *name) {
	for (int i = 0; i < AGENT_COUNT; i++) {
		if (equal(commands[i], name)) {
			return i;
		}
	}
	return -1;
}

static bool proc_stat(int pid, int *parent, int *tty, time_t *started, char name[256]) {
	char path[64], *content = NULL;
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	struct stat st;
	if (stat(path, &st) != 0 || st.st_uid != getuid() ||
			!g_file_get_contents(path, &content, NULL, NULL)) {
		return false;
	}
	char *start = strchr(content, '('), *end = strrchr(content, ')');
	int group, session, offset = 0;
	char state;
	bool ok = start && end && end > start && end - start < 256 &&
		sscanf(end + 1, " %c %d %d %d %d%n", &state, parent, &group, &session, tty,
			&offset) == 5 &&
		state != 'Z' && state != 'X';
	if (ok) {
		memcpy(name, start + 1, end - start - 1);
		name[end - start - 1] = '\0';
		const char *p = end + 1 + offset;
		for (int field = 8; field < 22; field++) {
			p += strspn(p, " ");
			p += strcspn(p, " ");
		}
		unsigned long long start_ticks = strtoull(p, NULL, 10);
		long tick_rate = sysconf(_SC_CLK_TCK);
		struct timespec boot;
#ifdef CLOCK_BOOTTIME
		clockid_t uptime_clock = CLOCK_BOOTTIME;
#else
		clockid_t uptime_clock = CLOCK_MONOTONIC;
#endif
		if (start_ticks && tick_rate > 0 && clock_gettime(uptime_clock, &boot) == 0) {
			*started = time(NULL) - boot.tv_sec + start_ticks / tick_rate;
		}
	}
	g_free(content);
	return ok;
}

static const char *option_value(char **args, const char *option) {
	size_t len = strlen(option);
	for (size_t i = 1; args[i] && !equal(args[i], "--"); i++) {
		if (equal(args[i], option)) {
			return args[i + 1];
		}
		if (g_str_has_prefix(args[i], option) && args[i][len] == '=') {
			return args[i] + len + 1;
		}
	}
	return NULL;
}

static void find_worker(struct collect *c, int pid) {
	char name[256], path[128];
	int parent, tty;
	time_t started = 0;
	if (!proc_stat(pid, &parent, &tty, &started, name)) {
		return;
	}
	int kind = kind_of(name);
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	char *command = NULL;
	size_t len;
	if (!g_file_get_contents(path, &command, &len, NULL) || !len) {
		g_free(command);
		return;
	}
	GPtrArray *args = g_ptr_array_new();
	for (size_t pos = 0; pos < len; pos += strlen(command + pos) + 1) {
		g_ptr_array_add(args, command + pos);
	}
	g_ptr_array_add(args, NULL);
	char **argv = (char **)args->pdata;
	if (kind < 0) {
		const char *program = equal(name, "node") && argv[1] ? argv[1] : argv[0];
		const char *base = strrchr(program, '/');
		kind = kind_of(base ? base + 1 : program);
	}
	if (kind >= 0) {
		struct agent_worker *w = g_new0(struct agent_worker, 1);
		w->pid = pid;
		w->parent = parent;
		w->started = started;
		w->kind = kind;
		w->terminal = tty != 0;
		const char *resume = option_value(argv, "--resume");
		if (!resume && kind == AGENT_CODEX && equal(argv[1], "resume") && args->len > 3 &&
				g_uuid_string_is_valid(argv[2])) {
			resume = argv[2];
		}
		w->session = label(resume ? resume : option_value(argv, "--session-id"));
		w->model = label(option_value(argv, "--model"));
		w->effort = label(option_value(argv, "--effort"));
		if (!w->effort) {
			w->effort = label(option_value(argv, "--reasoning-effort"));
		}
		snprintf(path, sizeof(path), "/proc/%d/fd/0", pid);
		w->tty = g_file_read_link(path, NULL);
		if (w->tty && !g_str_has_prefix(w->tty, "/dev/pts/") &&
				!g_str_has_prefix(w->tty, "/dev/tty")) {
			g_clear_pointer(&w->tty, g_free);
		}
		// A Codex app server can hold the rollout fd on behalf of the TUI.
		if (kind == AGENT_CODEX && (equal(argv[1], "app-server") ||
				equal(argv[1], "app-server-daemon"))) {
			w->terminal = false;
		}
		g_ptr_array_add(c->usage->workers, w);
	}
	g_ptr_array_unref(args);
	g_free(command);
}

static struct session *session_by_id(struct collect *c, enum agent_kind kind, const char *id) {
	if (!id) {
		return NULL;
	}
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, c->sessions[kind]);
	struct session *newest = NULL;
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		struct session *s = value;
		if (equal(s->id, id) && (!newest || s->at > newest->at)) {
			newest = s;
		}
	}
	return newest;
}

static struct session *session_by_fd(struct collect *c, struct agent_worker *w) {
	char dir[64];
	snprintf(dir, sizeof(dir), "/proc/%d/fd", w->pid);
	GDir *d = g_dir_open(dir, 0, NULL);
	if (!d) {
		return NULL;
	}
	struct session *only = NULL;
	const char *fd;
	while ((fd = g_dir_read_name(d))) {
		char *path = g_build_filename(dir, fd, NULL);
		char *target = g_file_read_link(path, NULL);
		struct session *s = target ? g_hash_table_lookup(c->sessions[w->kind], target) : NULL;
		g_free(target);
		g_free(path);
		if (s && only && s != only) {
			only = NULL;
			break;
		}
		if (s) {
			only = s;
		}
	}
	g_dir_close(d);
	return only;
}

static void find_workers(struct collect *c, const char *claude_dir) {
	GDir *d = g_dir_open("/proc", 0, NULL);
	if (!d) {
		return;
	}
	const char *entry;
	while ((entry = g_dir_read_name(d))) {
		if (g_ascii_isdigit(entry[0])) {
			find_worker(c, atoi(entry));
		}
	}
	g_dir_close(d);
	GPtrArray *workers = c->usage->workers;
	for (guint i = 0; i < workers->len; i++) {
		struct agent_worker *w = workers->pdata[i];
		if (w->kind == AGENT_CLAUDE) {
			char *path = g_strdup_printf("%s/sessions/%d.json", claude_dir, w->pid);
			json_object *meta = json_object_from_file(path);
			replace(&w->session, str(meta, "sessionId"));
			replace(&w->effort, str(meta, "effort"));
			json_object_put(meta);
			g_free(path);
		}
		if (w->kind == AGENT_AGY) {
			char *id = g_strdup_printf("pid:%d", w->pid);
			struct session *s = g_hash_table_lookup(c->sessions[AGENT_AGY], id);
			if (s && w->started && s->at + 2 >= w->started) {
				replace(&w->model, s->model);
				replace(&w->effort, s->effort);
			}
			g_free(id);
		}
		struct session *s = session_by_id(c, w->kind, w->session);
		if (!s) {
			s = session_by_fd(c, w);
		}
		if (s) {
			replace(&w->session, s->id);
			replace(&w->model, s->model);
			replace(&w->effort, s->effort);
		}
	}
	for (guint i = 0; i < workers->len; i++) {
		struct agent_worker *w = workers->pdata[i];
		for (guint j = 0; j < workers->len; j++) {
			struct agent_worker *parent = workers->pdata[j];
			if (w->parent == parent->pid && w->kind == parent->kind &&
					(!w->tty || equal(w->tty, parent->tty))) {
				if (w->model) {
					replace(&parent->model, w->model);
					replace(&parent->effort, w->effort);
				}
				w->terminal = false;
				break;
			}
		}
	}
	for (guint i = workers->len; i > 0; i--) {
		struct agent_worker *w = workers->pdata[i - 1];
		if (!w->terminal) {
			g_ptr_array_remove_index(workers, i - 1);
		}
	}
}

static struct agent_limit *new_limit(struct collect *c, enum agent_kind kind, const char *name) {
	struct agent_limit *limit = g_new0(struct agent_limit, 1);
	limit->kind = kind;
	limit->name = label(name);
	g_ptr_array_add(c->usage->limits, limit);
	return limit;
}

static struct agent_limit *read_account(struct collect *c, enum agent_kind kind, const struct tw_usage_tool *tool,
		const char *dir) {
	struct agent_limit *limit = new_limit(c, kind, tool->name);
	if (!dir) {
		return limit;
	}
	if (tool->backfill) {
		tool->backfill(dir, c->usage->now);
	}
	tool->read(dir, &limit->usage);
	if (kind == AGENT_CODEX && limit->usage.updated) {
		struct tw_claude_point *points = NULL;
		int n = tw_claude_history_read(limit->usage.history, &points);
		if (!n || limit->usage.updated > points[n - 1].at) {
			tw_claude_history_add(limit->usage.history, &limit->usage, limit->usage.updated);
		}
		free(points);
	}
	return limit;
}

/* Query only the account quota. The CLI handles authentication and no thread is started. */
static json_object *codex_rpc(void) {
	// (a portable Codex: where it was found running, tw_agent_command)
	char *program = tw_agent_command(TW_AGENT_CODEX);
	GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
		program ? program : "codex", "app-server", "--listen", "stdio://", NULL);
	free(program);
	if (!proc) {
		return NULL;
	}
	const char *init = "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{"
		"\"name\":\"tilewin\",\"title\":\"tileWin\",\"version\":\"1\"}}}\n";
	const char *request = "{\"method\":\"initialized\"}\n"
		"{\"id\":2,\"method\":\"account/rateLimits/read\"}\n";
	GOutputStream *input = g_subprocess_get_stdin_pipe(proc);
	GInputStream *output = g_subprocess_get_stdout_pipe(proc);
	int fd = g_file_descriptor_based_get_fd(G_FILE_DESCRIPTOR_BASED(output));
	struct pollfd poller = { .fd = fd, .events = POLLIN };
	gint64 deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
	bool ok = g_output_stream_write_all(input, init, strlen(init), NULL, NULL, NULL);
	GString *line = g_string_new(NULL);
	json_object *result = NULL;
	while (ok && !result) {
		gint64 left = deadline - g_get_monotonic_time();
		if (left <= 0) {
			break;
		}
		int ready = poll(&poller, 1, (left + 999) / 1000);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0 || !(poller.revents & POLLIN)) {
			break;
		}
		char bytes[4096];
		ssize_t len = read(fd, bytes, sizeof(bytes));
		if (len < 0 && errno == EINTR) {
			continue;
		}
		if (len <= 0) {
			break;
		}
		for (ssize_t i = 0; i < len && ok && !result; i++) {
			if (bytes[i] != '\n') {
				if (line->len >= 1024 * 1024) {
					ok = false;
					break;
				}
				g_string_append_c(line, bytes[i]);
				continue;
			}
			json_object *message = json_tokener_parse(line->str);
			double id = number(message, "id");
			if (id == 1) {
				ok = field(message, "result") &&
					g_output_stream_write_all(input, request, strlen(request), NULL, NULL, NULL);
			} else if (id == 2) {
				result = json_object_get(field(message, "result"));
				ok = false;
			}
			json_object_put(message);
			g_string_truncate(line, 0);
		}
	}
	g_string_free(line, true);
	g_subprocess_force_exit(proc);
	g_subprocess_wait(proc, NULL, NULL);
	g_object_unref(proc);
	return result;
}

static json_object *copilot_quota_query(void) {
	static const char request[] =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account.getQuota\",\"params\":{}}";
	// (also the Copilot that comes with VS Code, tw_agent_command)
	char *program = tw_agent_command(TW_AGENT_COPILOT);
	GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
		program ? program : "copilot", "--headless", "--stdio", "--no-auto-update", NULL);
	free(program);
	if (!proc) {
		return NULL;
	}
	char *wire = g_strdup_printf("Content-Length: %zu\r\n\r\n%s", strlen(request), request);
	bool sent = g_output_stream_write_all(g_subprocess_get_stdin_pipe(proc), wire,
		strlen(wire), NULL, NULL, NULL);
	g_free(wire);
	GInputStream *output = g_subprocess_get_stdout_pipe(proc);
	int fd = g_file_descriptor_based_get_fd(G_FILE_DESCRIPTOR_BASED(output));
	struct pollfd poller = { .fd = fd, .events = POLLIN };
	gint64 deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
	GString *response = g_string_new(NULL);
	json_object *message = NULL;
	while (sent && response->len < 256 * 1024) {
		gint64 left = deadline - g_get_monotonic_time();
		if (left <= 0) {
			break;
		}
		int ready = poll(&poller, 1, (left + 999) / 1000);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0 || !(poller.revents & POLLIN)) {
			break;
		}
		char chunk[4096];
		ssize_t size = read(fd, chunk, sizeof(chunk));
		if (size <= 0) {
			break;
		}
		g_string_append_len(response, chunk, size);
		char *header = strstr(response->str, "Content-Length:");
		char *body = header ? strstr(header, "\r\n\r\n") : NULL;
		if (!body) {
			continue;
		}
		int length = atoi(header + strlen("Content-Length:"));
		body += 4;
		if (length <= 0 || length > 256 * 1024) {
			break;
		}
		if ((size_t)length <= response->len - (body - response->str)) {
			char *payload = g_strndup(body, length);
			message = json_tokener_parse(payload);
			g_free(payload);
			break;
		}
	}
	g_string_free(response, true);
	g_subprocess_force_exit(proc);
	g_subprocess_wait(proc, NULL, NULL);
	g_object_unref(proc);
	return message;
}

static void copilot_account(struct agent_usage *usage) {
	static time_t last_try;
	static struct tw_claude_window quota;
	static const char *bucket;
	static bool auth_required;
	time_t now = time(NULL);
	if (!last_try || now < last_try || now - last_try >= 300) {
		last_try = now;
		json_object *answer = copilot_quota_query();
		const char *error = str(field(answer, "error"), "message");
		auth_required = error && strstr(error, "Not authenticated");
		json_object *result = field(answer, "result");
		json_object *snapshots = field(result, "quotaSnapshots");
		if (!snapshots) {
			snapshots = field(result, "quota_snapshots");
		}
		const char *const ids[] = { "ai_credits", "premium_interactions", "chat" };
		const char *const names[] = { "AI credits", "Premium interactions", "Chat allowance" };
		if (snapshots || auth_required) {
			quota = (struct tw_claude_window){ 0 };
			bucket = NULL;
		}
		for (size_t i = 0; i < G_N_ELEMENTS(ids); i++) {
			json_object *item = field(snapshots, ids[i]);
			const char *key = field(item, "remainingPercentage") ? "remainingPercentage" :
				"remaining_percentage";
			if (json_object_get_boolean(field(item, "isUnlimitedEntitlement"))) {
				continue;
			}
			json_object *value = field(item, key);
			double remaining = number(item, key);
			if ((!json_object_is_type(value, json_type_int) &&
					!json_object_is_type(value, json_type_double)) ||
					!(remaining >= 0 && remaining <= 100)) {
				continue;
			}
			time_t reset = timestamp(item, "resetDate");
			if (!reset) {
				reset = timestamp(item, "reset_date");
			}
			// Some runtimes send the fetch timestamp in the reset field.
			quota = (struct tw_claude_window){ .known = true, .used = 100 - remaining,
				.resets_at = reset > now ? reset : 0 };
			bucket = json_object_get_boolean(field(item, "tokenBasedBilling")) ?
				"AI credits" : names[i];
			break;
		}
		json_object_put(answer);
	}
	usage->copilot_billing = quota;
	usage->copilot_bucket = g_strdup(bucket);
	usage->copilot_auth_required = auth_required;
}

static void codex_poll_window(struct tw_claude_usage *u, json_object *w) {
	double length = number(w, "windowDurationMins");
	json_object *used = field(w, "usedPercent");
	double share = number(w, "usedPercent");
	if ((!json_object_is_type(used, json_type_int) && !json_object_is_type(used, json_type_double)) ||
			!(share >= 0 && share <= 100)) {
		return;
	}
	struct tw_claude_window win = { .known = true, .used = share,
		.resets_at = timestamp(w, "resetsAt") };
	if (length == TW_CLAUDE_FIVE_HOURS / 60) {
		u->five_hour = win;
	} else if (length == TW_CLAUDE_SEVEN_DAYS / 60) {
		u->seven_day = win;
	}
}

static void codex_account(struct collect *c, const char *dir) {
	static time_t last_try;
	static struct tw_claude_usage polled;
	static bool blocked;
	time_t now = time(NULL);
	if (!polled.dir || !equal(polled.dir, dir) || now < last_try || now - last_try >= 300) {
		bool same_dir = equal(polled.dir, dir);
		last_try = now;
		json_object *answer = codex_rpc();
		json_object *rates = field(answer, "rateLimits");
		struct tw_claude_usage sample = { 0 };
		codex_poll_window(&sample, field(rates, "primary"));
		codex_poll_window(&sample, field(rates, "secondary"));
		if (!same_dir || sample.five_hour.known || sample.seven_day.known) {
			tw_claude_usage_finish(&polled);
			polled = sample;
			polled.dir = strdup(dir);
			if (sample.five_hour.known || sample.seven_day.known) {
				polled.updated = time(NULL);
			}
			json_object *allowed = field(answer, "ordinaryUsageAllowed");
			blocked = json_object_is_type(allowed, json_type_boolean) &&
				!json_object_get_boolean(allowed);
		}
		json_object_put(answer);
	}
	if (polled.updated) {
		struct agent_limit *live = new_limit(c, AGENT_CODEX, "Codex · live account quota");
		live->live_account = true;
		live->account_blocked = blocked;
		live->usage.updated = polled.updated;
		live->usage.five_hour = polled.five_hour;
		live->usage.seven_day = polled.seven_day;
		live->usage.history = tw_usage_history_path("agent-codex-account-v2", dir);
		tw_claude_history_add(live->usage.history, &live->usage, polled.updated);
	}
}

static struct agent_limit *agy_limit(struct collect *c, const char *bucket, bool weekly) {
	char *name = g_strdup_printf("%s · %s", bucket, weekly ? "weekly" : "5h");
	for (guint i = 0; i < c->usage->limits->len; i++) {
		struct agent_limit *limit = c->usage->limits->pdata[i];
		if (limit->kind == AGENT_AGY && equal(limit->name, name)) {
			g_free(name);
			return limit;
		}
	}
	struct agent_limit *limit = new_limit(c, AGENT_AGY, name);
	g_free(name);
	return limit;
}

static void agy_quotas(struct collect *c, json_object *quota, time_t at) {
	if (!json_object_is_type(quota, json_type_object) || !at || at > c->usage->now) {
		return;
	}
	json_object_object_foreach(quota, bucket, values) {
		json_object *fraction = field(values, "remaining_fraction");
		double left = number(values, "remaining_fraction");
		time_t reset = timestamp(values, "reset_time");
		if ((!json_object_is_type(fraction, json_type_int) &&
				!json_object_is_type(fraction, json_type_double)) ||
				!(left >= 0 && left <= 1) || !reset ||
				json_object_get_boolean(field(values, "disabled"))) {
			continue;
		}
		char *lower = g_ascii_strdown(bucket, -1);
		bool weekly = strstr(lower, "weekly") || strstr(lower, "7d");
		bool five = strstr(lower, "5h") || strstr(lower, "5-hour") ||
			strstr(lower, "five-hour") || strstr(lower, "fivehour") || strstr(lower, "five_hour");
		g_free(lower);
		if ((!weekly && !five) || strlen(bucket) > 64) {
			continue;
		}
		struct agent_limit *l = agy_limit(c, bucket, weekly);
		struct tw_claude_window win = { .known = true, .used = 100 * (1 - left),
			.resets_at = reset };
		struct tw_claude_point point = { .at = at };
		if (weekly) {
			point.seven_day = win;
		} else {
			point.five_hour = win;
		}
		if (at >= l->usage.updated) {
			l->usage.updated = at;
			l->usage.five_hour = point.five_hour;
			l->usage.seven_day = point.seven_day;
		}
		struct claude_charts *chart = &l->chart;
		struct tw_claude_point *points = realloc(chart->points,
			(chart->count + 1) * sizeof(*points));
		if (points) {
			chart->points = points;
			chart->points[chart->count++] = point;
		}
	}
}

static int point_order(const void *a, const void *b) {
	const struct tw_claude_point *pa = a, *pb = b;
	return (pa->at > pb->at) - (pa->at < pb->at);
}

static void agy_account_poll(const char *state) {
	static time_t last_attempt;
	time_t now = time(NULL);
	if (last_attempt && now >= last_attempt && now - last_attempt < 300) {
		return;
	}
	char *dir = g_build_filename(state, "agy", NULL);
	GDir *entries = g_dir_open(dir, 0, NULL);
	const char *entry;
	bool fresh = false;
	while (entries && (entry = g_dir_read_name(entries))) {
		if (!g_str_has_suffix(entry, ".jsonl")) {
			continue;
		}
		char *path = g_build_filename(dir, entry, NULL);
		struct stat st;
		fresh = stat(path, &st) == 0 && st.st_size > 0 && st.st_mtime > now - 300;
		g_free(path);
		if (fresh) {
			break;
		}
	}
	if (entries) {
		g_dir_close(entries);
	}
	g_free(dir);
	if (!fresh) {
		last_attempt = now;
		char *argv[] = { "tilewin-claude-usage", "agy-quota", NULL };
		g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
			G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	}
}

/* Idle AGY buckets may return a new full-window deadline at every refresh.
 * A zero reading only has an established deadline if we saw usage in it. */
static void agy_normalize_deadlines(struct agent_limit *limit) {
	bool weekly = limit->usage.seven_day.known;
	struct tw_claude_window *current = weekly ? &limit->usage.seven_day : &limit->usage.five_hour;
	time_t active = 0;
	for (int i = 0; i < limit->chart.count; i++) {
		struct tw_claude_point *point = &limit->chart.points[i];
		struct tw_claude_window *win = weekly ? &point->seven_day : &point->five_hour;
		if (!win->known) {
			continue;
		}
		if (win->used > 0 && win->resets_at > point->at) {
			active = win->resets_at;
		} else if (win->used == 0 && (!active || !tw_claude_same_window(active, win->resets_at))) {
			win->resets_at = 0;
		}
	}
	if (current->used == 0 && (!active || !tw_claude_same_window(active, current->resets_at))) {
		current->resets_at = 0;
	}
}

static void read_agy(struct collect *c, const char *state) {
	char *dir = g_build_filename(state, "agy", NULL);
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *entry;
	while (d && (entry = g_dir_read_name(d))) {
		if (!g_str_has_suffix(entry, ".jsonl")) {
			continue;
		}
		char *path = g_build_filename(dir, entry, NULL);
		FILE *file = fopen(path, "re");
		g_free(path);
		if (!file) {
			continue;
		}
		char *line = NULL, *previous_model = NULL;
		size_t capacity = 0;
		ssize_t len;
		double previous = 0;
		unsigned row = 0;
		while ((len = getline(&line, &capacity, file)) >= 0) {
			row++;
			if (len > MAX_LINE || line[len - 1] != '\n') {
				continue;
			}
			json_object *o = json_tokener_parse(line);
			json_object *p = field(o, "data"), *m = field(p, "model"), *t = field(p, "totals");
			time_t at = timestamp(o, "at");
			const char *model = str(m, "id");
			double total = tokens(t, "total_input_tokens") + tokens(t, "total_output_tokens");
			if (model && at && at <= c->usage->now) {
				// A context counter can fall on compaction or change meaning on a model switch.
				bool same_model = equal(previous_model, model);
				double delta = same_model && total > previous ? total - previous : 0;
				char *id = g_strdup_printf("%s:%u", entry, row);
				remember_event(c, AGENT_AGY, id, model, at, delta, total != previous);
				g_free(id);
				replace(&previous_model, model);
				previous = total;
			}
			double pid = number(p, "pid");
			if (model && pid > 0 && pid < G_MAXINT && at && at <= c->usage->now) {
				char *key = g_strdup_printf("pid:%d", (int)pid);
				struct session *session = g_hash_table_lookup(c->sessions[AGENT_AGY], key);
				if (!session) {
					session = g_new0(struct session, 1);
					g_hash_table_insert(c->sessions[AGENT_AGY], g_strdup(key), session);
				}
				if (at >= session->at) {
					update_session(session, model, str(m, "effort"), at);
				}
				g_free(key);
			}
			if (at > c->usage->now - 8 * DAY) {
				agy_quotas(c, field(p, "quota"), at);
			}
			json_object_put(o);
		}
		g_free(previous_model);
		free(line);
		fclose(file);
	}
	if (d) {
		g_dir_close(d);
	}
	g_free(dir);
	for (guint i = 0; i < c->usage->limits->len; i++) {
		struct agent_limit *l = c->usage->limits->pdata[i];
		if (l->kind == AGENT_AGY && l->chart.count) {
			qsort(l->chart.points, l->chart.count, sizeof(*l->chart.points), point_order);
			agy_normalize_deadlines(l);
			l->chart.now = c->usage->now;
			l->chart.have = true;
			l->chart.five_hour = l->usage.five_hour;
			l->chart.seven_day = l->usage.seven_day;
		}
	}
}

static struct agent_model *get_model(struct collect *c, enum agent_kind kind, const char *name) {
	for (guint i = 0; i < c->usage->models->len; i++) {
		struct agent_model *m = c->usage->models->pdata[i];
		if (m->kind == kind && equal(m->name, name)) {
			return m;
		}
	}
	struct agent_model *m = g_new0(struct agent_model, 1);
	m->kind = kind;
	m->name = label(name);
	g_ptr_array_add(c->usage->models, m);
	return m;
}

static int time_order(const void *a, const void *b) {
	time_t x = *(const time_t *)a, y = *(const time_t *)b;
	return (x > y) - (x < y);
}

static bool local_evidence(GArray *times, time_t from, time_t to) {
	guint low = 0, high = times->len;
	while (low < high) {
		guint mid = low + (high - low) / 2;
		if (g_array_index(times, time_t, mid) <= from) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	return low < times->len && g_array_index(times, time_t, low) < to;
}

static struct tw_claude_window point_window(struct tw_claude_point p, bool weekly) {
	return weekly ? p.seven_day : p.five_hour;
}

static void unattributed(struct agent_limit *limit, GArray *times, bool weekly) {
	struct claude_charts *chart = &limit->chart;
	for (int i = 1; i < chart->count; i++) {
		struct tw_claude_point p = chart->points[i];
		struct tw_claude_window b = point_window(p, weekly);
		if (!b.known || !b.resets_at || p.at < chart->now - 8 * DAY) {
			continue;
		}
		// An account sampled every five minutes can rise across several polls.
		// Find the nearest reading in the same window at least 15 minutes back.
		for (int j = i - 1; j >= 0; j--) {
			struct tw_claude_point prev = chart->points[j];
			struct tw_claude_window a = point_window(prev, weekly);
			if (!a.known) {
				continue;
			}
			if (!a.resets_at || !tw_claude_same_window(a.resets_at, b.resets_at)) {
				break;
			}
			if (p.at - prev.at < 15 * 60) {
				continue;
			}
			if (b.used - a.used >= 3 &&
					!local_evidence(times, prev.at + 30, p.at - 30) &&
					(p.at > limit->unattributed_at ||
						(p.at == limit->unattributed_at && b.used - a.used > limit->unattributed))) {
				limit->unattributed_at = p.at;
				limit->unattributed_from = prev.at;
				limit->unattributed = b.used - a.used;
			}
			break;
		}
	}
}

static void add_codex_log_history(struct agent_limit *limit, time_t now) {
	static char *directory;
	static struct tw_claude_point *recent;
	static int count;
	static time_t imported;
	const char *dir = limit->usage.dir;
	if (!dir) {
		return;
	}
	if (!equal(directory, dir) || now < imported || now - imported >= 300) {
		g_free(directory);
		directory = g_strdup(dir);
		free(recent);
		count = tw_codex_history_points(dir, now, &recent);
		imported = now;
	}
	struct claude_charts *chart = &limit->chart;
	if (!count) {
		return;
	}
	struct tw_claude_point *merged = malloc((count + chart->count) * sizeof(*merged));
	if (!merged) {
		return;
	}
	int n = 0, log = 0, saved = 0;
	while (log < count || saved < chart->count) {
		struct tw_claude_point p;
		if (saved < chart->count &&
				(log == count || chart->points[saved].at <= recent[log].at)) {
			p = chart->points[saved++];
		} else {
			p = recent[log++];
		}
		if (n && p.at == merged[n - 1].at) {
			continue;
		}
		if (n) {
			struct tw_claude_point *prev = &merged[n - 1];
			if (tw_claude_window_older(&p.five_hour, &prev->five_hour)) {
				p.five_hour = prev->five_hour;
			}
			if (tw_claude_window_older(&p.seven_day, &prev->seven_day)) {
				p.seven_day = prev->seven_day;
			}
		}
		merged[n++] = p;
	}
	free(chart->points);
	chart->points = merged;
	chart->count = n;
}

static void summarize(struct collect *c) {
	GArray *activity[AGENT_COUNT];
	for (int i = 0; i < AGENT_COUNT; i++) {
		activity[i] = g_array_new(false, false, sizeof(time_t));
	}
	json_object_iter iter;
	json_object_object_foreachC(c->events, iter) {
		int kind = iter.key[0] - '0';
		time_t at = timestamp(iter.val, "at");
		if (kind < 0 || kind >= AGENT_COUNT || iter.key[1] != ':' ||
				at <= c->usage->now - AGENT_DAYS * DAY || at > c->usage->now + 300) {
			json_object_object_del(c->events, iter.key);
			c->events_changed = true;
			continue;
		}
		if (at > c->usage->now) {
			continue;
		}
		int day = c->usage->now / DAY - at / DAY;
		const char *model = str(iter.val, "model");
		double count = number(iter.val, "tokens");
		g_array_append_val(activity[kind], at);
		if (model && *model && count > 0 && count <= 1e13 && day < AGENT_DAYS) {
			get_model(c, kind, model)->tokens[day] += count;
		}
	}
	for (int i = 0; i < AGENT_COUNT; i++) {
		g_array_sort(activity[i], time_order);
	}
	for (guint i = 0; i < c->usage->limits->len; i++) {
		struct agent_limit *l = c->usage->limits->pdata[i];
		if (l->kind != AGENT_AGY) {
			claude_charts_load(&l->chart, &l->usage, c->usage->now);
		}
		if (l->kind == AGENT_CODEX && !l->live_account) {
			add_codex_log_history(l, c->usage->now);
		}
		unattributed(l, activity[l->kind], false);
		unattributed(l, activity[l->kind], true);
	}
	for (int i = 0; i < AGENT_COUNT; i++) {
		g_array_unref(activity[i]);
	}
}

struct agent_usage *agent_usage_collect(bool account_checks) {
	static GMutex collector_lock;
	g_mutex_lock(&collector_lock);
	struct agent_usage *usage = g_new0(struct agent_usage, 1);
	g_atomic_ref_count_init(&usage->refs);
	usage->now = time(NULL);
	usage->workers = g_ptr_array_new_with_free_func(worker_free);
	usage->models = g_ptr_array_new_with_free_func(model_free);
	usage->limits = g_ptr_array_new_with_free_func(limit_free);
	struct collect c = { .usage = usage };
	for (int i = 0; i < AGENT_COUNT; i++) {
		// on $PATH, or where it was found running (enum agent_kind: tw_agent's order)
		char *command = tw_agent_command((enum tw_agent)i);
		usage->installed[i] = command != NULL;
		free(command);
		c.sessions[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			i == AGENT_AGY ? session_free : NULL);
	}
	char *root = tw_state_dir();
	char *state = root ? g_build_filename(root, "agent-usage", NULL) : NULL;
	free(root);
	char *path = state ? g_build_filename(state, "events.json", NULL) : NULL;
	char *lock = state ? g_build_filename(state, "events.lock", NULL) : NULL;
	int fd = -1;
	if (state && g_mkdir_with_parents(state, 0700) == 0) {
		fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	}
	bool locked = fd >= 0 && flock(fd, LOCK_EX) == 0;
	c.events = locked ? json_object_from_file(path) : NULL;
	static GHashTable *log_cache;
	static char *cache_path;
	if (!log_cache) {
		log_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, session_free);
	}
	if (!equal(cache_path, path) || !json_object_is_type(c.events, json_type_object)) {
		g_free(cache_path);
		cache_path = g_strdup(path);
		g_hash_table_remove_all(log_cache);
	}
	c.logs = log_cache;
	if (!json_object_is_type(c.events, json_type_object)) {
		json_object_put(c.events);
		c.events = json_object_new_object();
		c.events_changed = true;
	}
	GHashTableIter files;
	gpointer value;
	g_hash_table_iter_init(&files, c.logs);
	while (g_hash_table_iter_next(&files, NULL, &value)) {
		struct session *log = value;
		if (log->mtime < usage->now - AGENT_DAYS * DAY) {
			g_hash_table_iter_remove(&files);
		}
	}
	char *claude = tw_usage_select_dir(&tw_claude_tool, NULL);
	char *codex = tw_usage_select_dir(&tw_codex_tool, NULL);
	const char *copilot = g_getenv("COPILOT_HOME");
	char *logs = g_build_filename(claude ? claude : "", "projects", NULL);
	scan_logs(&c, logs, AGENT_CLAUDE, 4);
	g_free(logs);
	if (codex) {
		logs = g_build_filename(codex, "sessions", NULL);
		scan_logs(&c, logs, AGENT_CODEX, 4);
		g_free(logs);
		logs = g_build_filename(codex, "archived_sessions", NULL);
		scan_logs(&c, logs, AGENT_CODEX, 0);
		g_free(logs);
	}
	logs = copilot && *copilot ? g_build_filename(copilot, "session-state", NULL) :
		g_build_filename(g_get_home_dir(), ".copilot", "session-state", NULL);
	scan_logs(&c, logs, AGENT_COPILOT, 1);
	g_free(logs);
	read_account(&c, AGENT_CLAUDE, &tw_claude_tool, claude);
	read_account(&c, AGENT_CODEX, &tw_codex_tool, codex);
	// asking the signed-in agents themselves (each starts its program, which asks its
	// account online): only when that is turned on, as it uses their logins
	if (account_checks && codex && usage->installed[AGENT_CODEX]) {
		codex_account(&c, codex);
	}
	if (account_checks && usage->installed[AGENT_COPILOT]) {
		copilot_account(usage);
	}
	if (state) {
		if (account_checks && usage->installed[AGENT_AGY]) {
			agy_account_poll(state);
		}
		usage->now = time(NULL);
		read_agy(&c, state);
	}
	find_workers(&c, claude ? claude : "");
	usage->now = time(NULL);
	summarize(&c);
	if (!locked || (c.events_changed && !g_file_set_contents_full(path,
			json_object_to_json_string_ext(c.events, JSON_C_TO_STRING_PLAIN), -1,
			G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))) {
		g_hash_table_remove_all(log_cache);
	}
	if (fd >= 0) {
		close(fd);
	}
	for (int i = 0; i < AGENT_COUNT; i++) {
		g_hash_table_unref(c.sessions[i]);
	}
	json_object_put(c.events);
	free(claude);
	free(codex);
	g_free(state);
	g_free(path);
	g_free(lock);
	g_mutex_unlock(&collector_lock);
	return usage;
}
