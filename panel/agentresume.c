/*
 * Automatic resume of interactive Claude Code, Codex and Antigravity terminal
 * sessions after a full usage window resets.
 *
 * When enabled in the AI agent usage widget settings (auto_resume yes), each
 * tracked usage window that reaches the critical threshold (>= critical, 95%
 * by default) arms a timer for resets_at + resume_delay. Once that deadline
 * passes (and no corresponding weekly window still blocks usage), open
 * terminal emulator windows running an interactive foreground session of that
 * agent and account are identified through /proc and the compositor window
 * tree, focused briefly one by one, and sent resume_command followed by Enter
 * over the virtual-keyboard protocol; focus and minimized state are restored
 * afterwards. While the screen is locked, pending resumes wait until unlock.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include "agentresume.h"
#include "agentusage.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "tw_codex_usage.h"

#define FOCUS_WAIT_MS 150
#define KEY_STEP_MS 12
#define ENTER_WAIT_MS 80
#define PASTE_ENTER_WAIT_MS 260
#define AFTER_ENTER_MS 120
#define LOG_TAIL_BYTES (128 * 1024)

struct resume_slot {
	enum tw_agent agent;
	char *account_id;
	char *account_dir;
	char *bucket;
	bool seven_day;

	// Live window being observed
	time_t resets_at;
	double peak_used;
	bool armed;
	bool weekly_blocked;

	// Window that already reached reset and is counting down resume_delay
	// (kept separate so a fresh post-reset poll cannot cancel the countdown)
	bool pending;
	time_t pending_resets_at;
	time_t pending_fire_at;
	bool pending_weekly_blocked;
	bool deferred_lock;

	time_t last_fired_reset;
};

struct resume_job {
	int64_t con_id;
	bool was_minimized;
	char *command;
	char *agent_name;
};

enum type_stage {
	TYPE_IDLE,
	TYPE_FOCUSING,
	TYPE_TYPING,
	TYPE_ENTER,
	TYPE_AFTER,
};

static struct {
	struct panel *panel;
	bool enabled;
	int delay_s;
	int threshold;
	char *command;

	GPtrArray *slots; // struct resume_slot *
	struct agent_usage *collected; // ref to latest snapshot for AGY model matching
	struct loop_timer *wake_timer;

	GPtrArray *jobs; // struct resume_job *
	struct resume_job *active;
	enum type_stage stage;
	size_t char_pos;
	bool used_paste;
	int64_t saved_focus;
	bool has_saved_focus;
	struct loop_timer *step_timer;

	struct zwp_virtual_keyboard_v1 *vk;
	uint32_t shift_mask;
} ar = {
	.delay_s = AGENT_RESUME_DEFAULT_DELAY,
	.threshold = 95,
};

/* ---------- pure helpers ---------- */

bool agent_resume_is_terminal_app(const char *app_id) {
	if (!app_id || !*app_id) {
		return false;
	}
	static const char *const substrings[] = {
		"term", "kitty", "alacritty", "foot", "konsole", "wezterm", "tilix",
		"ghostty", "urxvt", "rxvt", "sakura", "guake", "tilda", "yakuake",
		"ptyxis", "blackbox", "contour", "st-256color", "org.gnome.console",
	};
	for (size_t i = 0; i < sizeof(substrings) / sizeof(substrings[0]); i++) {
		if (strcasestr(app_id, substrings[i])) {
			return true;
		}
	}
	static const char *const exact[] = { "st", "rio", "kgx" };
	for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
		if (strcasecmp(app_id, exact[i]) == 0) {
			return true;
		}
	}
	return false;
}

static const char *basename_of(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static bool is_js_runtime(const char *base) {
	return strcmp(base, "node") == 0 || strcmp(base, "nodejs") == 0 ||
		strcmp(base, "bun") == 0 || strcmp(base, "deno") == 0;
}

static bool arg_is_one_of(const char *arg, const char *const *list) {
	for (size_t i = 0; list[i]; i++) {
		if (strcmp(arg, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

int agent_resume_interactive_agent(const char *cmdline, size_t len) {
	if (!cmdline || len == 0) {
		return -1;
	}
	const char *argv[64];
	int argc = 0;
	for (size_t pos = 0; pos < len && argc < 64; ) {
		size_t n = strnlen(cmdline + pos, len - pos);
		argv[argc++] = cmdline + pos;
		pos += n + 1;
	}
	if (argc == 0 || !argv[0][0]) {
		return -1;
	}
	int agent = -1;
	int arg_start = 1;
	const char *base0 = basename_of(argv[0]);
	const char *target = argv[0];
	if (is_js_runtime(base0) && argc > 1 && argv[1][0]) {
		target = argv[1];
		arg_start = 2;
	}
	const char *base = basename_of(target);
	if (strcmp(base, "claude") == 0 || strstr(target, "@anthropic-ai/claude-code")) {
		agent = TW_AGENT_CLAUDE;
	} else if (strcmp(base, "codex") == 0 || strstr(target, "@openai/codex")) {
		agent = TW_AGENT_CODEX;
	} else if (strcmp(base, "agy") == 0 || strstr(target, "antigravity-cli")) {
		agent = TW_AGENT_ANTIGRAVITY;
	}
	if (agent < 0) {
		return -1;
	}

	if (agent == TW_AGENT_CLAUDE) {
		static const char *const non_interactive_flags[] = {
			"-p", "--print", "-v", "--version", "-h", "--help", NULL,
		};
		static const char *const non_interactive_cmds[] = {
			"doctor", "update", "mcp", "config", "setup-token", "install", NULL,
		};
		for (int i = arg_start; i < argc; i++) {
			if (strcmp(argv[i], "--") == 0) {
				break;
			}
			if (arg_is_one_of(argv[i], non_interactive_flags) ||
					strncmp(argv[i], "--print=", 8) == 0) {
				return -1;
			}
			if (i == arg_start && arg_is_one_of(argv[i], non_interactive_cmds)) {
				return -1;
			}
		}
	} else if (agent == TW_AGENT_CODEX) {
		static const char *const non_interactive_cmds[] = {
			"app-server", "app-server-daemon", "exec", "e", "mcp", "mcp-server",
			"login", "logout", "completion", "apply", "sandbox", NULL,
		};
		static const char *const non_interactive_flags[] = {
			"-V", "--version", "-h", "--help", NULL,
		};
		for (int i = arg_start; i < argc; i++) {
			if (strcmp(argv[i], "--") == 0) {
				break;
			}
			if (arg_is_one_of(argv[i], non_interactive_flags)) {
				return -1;
			}
			if (argv[i][0] != '-') {
				if (arg_is_one_of(argv[i], non_interactive_cmds)) {
					return -1;
				}
				break;
			}
		}
	} else if (agent == TW_AGENT_ANTIGRAVITY) {
		static const char *const non_interactive_flags[] = {
			"-p", "--print", "--prompt", "--settings", "-v", "--version", "-h", "--help", NULL,
		};
		static const char *const non_interactive_cmds[] = {
			"login", "logout", "mcp", "config", "daemon", "serve", NULL,
		};
		for (int i = arg_start; i < argc; i++) {
			if (strcmp(argv[i], "--") == 0) {
				break;
			}
			if (arg_is_one_of(argv[i], non_interactive_flags) ||
					strncmp(argv[i], "--print=", 8) == 0 ||
					strncmp(argv[i], "--prompt=", 9) == 0 ||
					strncmp(argv[i], "--settings=", 11) == 0) {
				return -1;
			}
			if (i == arg_start && arg_is_one_of(argv[i], non_interactive_cmds)) {
				return -1;
			}
		}
	}
	return agent;
}

bool agent_resume_agy_bucket_matches_model(const char *bucket, const char *model) {
	if (!bucket || !*bucket || !model || !*model) {
		return true;
	}
	if (g_str_has_prefix(bucket, "gemini-argon-")) {
		return strcasestr(model, "argon") != NULL;
	}
	if (g_str_has_prefix(bucket, "gemini-")) {
		return strcasestr(model, "gemini") != NULL && strcasestr(model, "argon") == NULL;
	}
	if (g_str_has_prefix(bucket, "3p-")) {
		return strcasestr(model, "gemini") == NULL;
	}
	return true;
}

bool agent_resume_ascii_key(char ch, uint32_t *keycode, bool *shift) {
	if (ch >= 'a' && ch <= 'z') {
		static const uint32_t letters[26] = {
			KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
			KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
			KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
		};
		*keycode = letters[ch - 'a'];
		*shift = false;
		return true;
	}
	if (ch >= 'A' && ch <= 'Z') {
		bool ok = agent_resume_ascii_key((char)(ch - 'A' + 'a'), keycode, shift);
		*shift = true;
		return ok;
	}
	if (ch >= '0' && ch <= '9') {
		static const uint32_t digits[10] = {
			KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
		};
		*keycode = digits[ch - '0'];
		*shift = false;
		return true;
	}
	static const struct {
		char ch;
		uint32_t code;
		bool shift;
	} punct[] = {
		{ ' ', KEY_SPACE, false },
		{ '\t', KEY_TAB, false },
		{ '-', KEY_MINUS, false },      { '_', KEY_MINUS, true },
		{ '=', KEY_EQUAL, false },      { '+', KEY_EQUAL, true },
		{ '[', KEY_LEFTBRACE, false },  { '{', KEY_LEFTBRACE, true },
		{ ']', KEY_RIGHTBRACE, false }, { '}', KEY_RIGHTBRACE, true },
		{ '\\', KEY_BACKSLASH, false }, { '|', KEY_BACKSLASH, true },
		{ ';', KEY_SEMICOLON, false },  { ':', KEY_SEMICOLON, true },
		{ '\'', KEY_APOSTROPHE, false },{ '"', KEY_APOSTROPHE, true },
		{ '`', KEY_GRAVE, false },      { '~', KEY_GRAVE, true },
		{ ',', KEY_COMMA, false },      { '<', KEY_COMMA, true },
		{ '.', KEY_DOT, false },        { '>', KEY_DOT, true },
		{ '/', KEY_SLASH, false },      { '?', KEY_SLASH, true },
		{ '!', KEY_1, true },           { '@', KEY_2, true },
		{ '#', KEY_3, true },           { '$', KEY_4, true },
		{ '%', KEY_5, true },           { '^', KEY_6, true },
		{ '&', KEY_7, true },           { '*', KEY_8, true },
		{ '(', KEY_9, true },           { ')', KEY_0, true },
	};
	for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++) {
		if (punct[i].ch == ch) {
			*keycode = punct[i].code;
			*shift = punct[i].shift;
			return true;
		}
	}
	return false;
}

static bool command_is_ascii(const char *text) {
	for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
		uint32_t code;
		bool shift;
		if (*p >= 128 || !agent_resume_ascii_key((char)*p, &code, &shift)) {
			return false;
		}
	}
	return true;
}

/* ---------- per-session usage-limit verification ---------- */

static json_object *jfield(json_object *o, const char *key) {
	json_object *v = NULL;
	if (o && json_object_is_type(o, json_type_object)) {
		json_object_object_get_ex(o, key, &v);
	}
	return v;
}

static const char *jstr(json_object *o, const char *key) {
	json_object *v = jfield(o, key);
	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}

static bool jbool(json_object *o, const char *key) {
	json_object *v = jfield(o, key);
	return json_object_is_type(v, json_type_boolean) && json_object_get_boolean(v);
}

static bool text_mentions_usage_limit(const char *s) {
	if (!s || !*s) {
		return false;
	}
	return strcasestr(s, "usage limit") != NULL ||
		strcasestr(s, "rate limit") != NULL ||
		strcasestr(s, "rate_limit") != NULL ||
		strcasestr(s, "usage_limit") != NULL ||
		strcasestr(s, "hit your") != NULL ||
		strcasestr(s, "out of extra usage") != NULL ||
		strcasestr(s, "quota reached") != NULL ||
		strcasestr(s, "quota exceeded") != NULL ||
		strcasestr(s, "out of quota") != NULL ||
		strcasestr(s, "resourceexhausted") != NULL ||
		strcasestr(s, "resource_exhausted") != NULL ||
		strcasestr(s, "too many requests") != NULL ||
		strcasestr(s, "http 429") != NULL ||
		strcasestr(s, "code 429") != NULL;
}

/* Reads up to max_bytes from the end of path; skips any partial first line when truncated. */
static char *read_file_tail(const char *path, size_t max_bytes) {
	if (!path || !*path) {
		return NULL;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	if (fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	off_t size = ftello(f);
	if (size <= 0) {
		fclose(f);
		return NULL;
	}
	bool truncated = false;
	off_t start = 0;
	if ((size_t)size > max_bytes) {
		start = size - (off_t)max_bytes;
		truncated = true;
	}
	if (fseeko(f, start, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	size_t want = (size_t)(size - start);
	char *buf = malloc(want + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, want, f);
	fclose(f);
	buf[got] = '\0';
	if (truncated) {
		char *nl = strchr(buf, '\n');
		if (nl) {
			memmove(buf, nl + 1, got - (size_t)(nl - buf));
		}
	}
	return buf;
}

static bool claude_message_mentions_limit(json_object *msg) {
	json_object *content = jfield(msg, "content");
	if (!content) {
		return false;
	}
	if (json_object_is_type(content, json_type_string)) {
		return text_mentions_usage_limit(json_object_get_string(content));
	}
	if (json_object_is_type(content, json_type_array)) {
		size_t n = json_object_array_length(content);
		for (size_t i = 0; i < n; i++) {
			json_object *part = json_object_array_get_idx(content, i);
			const char *text = jstr(part, "text");
			if (text_mentions_usage_limit(text)) {
				return true;
			}
		}
	}
	return false;
}

bool agent_resume_claude_log_hit_limit(const char *path) {
	char *tail = read_file_tail(path, LOG_TAIL_BYTES);
	if (!tail) {
		return false;
	}
	bool hit_limit = false;
	char *save = NULL;
	for (char *line = strtok_r(tail, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!*line || *line != '{') {
			continue;
		}
		json_object *o = json_tokener_parse(line);
		if (!o || !json_object_is_type(o, json_type_object)) {
			json_object_put(o);
			continue;
		}
		const char *type = jstr(o, "type");
		if (type && strcmp(type, "assistant") == 0) {
			const char *err = jstr(o, "error");
			bool api_err = jbool(o, "isApiErrorMessage");
			if ((err && strcasecmp(err, "rate_limit") == 0) ||
					(api_err && claude_message_mentions_limit(jfield(o, "message")))) {
				hit_limit = true;
			} else if (!api_err) {
				hit_limit = false;
			}
		} else if (type && strcmp(type, "system") == 0) {
			const char *content = jstr(o, "content");
			if (content && strcasestr(content, "usage limit reached")) {
				hit_limit = true;
			} else if (content && strcasestr(content, "usage limit reset")) {
				hit_limit = false;
			}
		} else if (type && strcmp(type, "user") == 0) {
			if (!jbool(o, "isMeta")) {
				hit_limit = false;
			}
		}
		json_object_put(o);
	}
	free(tail);
	return hit_limit;
}

bool agent_resume_codex_log_hit_limit(const char *path) {
	char *tail = read_file_tail(path, LOG_TAIL_BYTES);
	if (!tail) {
		return false;
	}
	bool hit_limit = false;
	char *save = NULL;
	for (char *line = strtok_r(tail, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!*line || *line != '{') {
			continue;
		}
		json_object *o = json_tokener_parse(line);
		if (!o || !json_object_is_type(o, json_type_object)) {
			json_object_put(o);
			continue;
		}
		const char *type = jstr(o, "type");
		json_object *payload = jfield(o, "payload");
		const char *ptype = jstr(payload, "type");
		if (type && strcmp(type, "event_msg") == 0 && ptype) {
			if (strcmp(ptype, "task_complete") == 0) {
				json_object *err = jfield(payload, "error");
				const char *info = jstr(err, "codex_error_info");
				const char *msg = jstr(err, "message");
				if ((info && (strcasestr(info, "usage_limit") || strcasestr(info, "rate_limit"))) ||
						text_mentions_usage_limit(msg)) {
					hit_limit = true;
				} else {
					hit_limit = false;
				}
			} else if (strcmp(ptype, "error") == 0 || strcmp(ptype, "stream_error") == 0) {
				const char *info = jstr(payload, "codex_error_info");
				const char *msg = jstr(payload, "message");
				if ((info && (strcasestr(info, "usage_limit") || strcasestr(info, "rate_limit"))) ||
						text_mentions_usage_limit(msg)) {
					hit_limit = true;
				}
			} else if (strcmp(ptype, "agent_message") == 0 ||
					strcmp(ptype, "user_message") == 0 ||
					strcmp(ptype, "task_started") == 0) {
				hit_limit = false;
			}
		}
		json_object_put(o);
	}
	free(tail);
	return hit_limit;
}

static time_t parse_iso8601(const char *s) {
	if (!s || !*s) {
		return 0;
	}
	GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
	if (!dt) {
		return 0;
	}
	time_t t = g_date_time_to_unix(dt);
	g_date_time_unref(dt);
	return t;
}

/* Parses glog prefix "E1125 20:57:25.339766 ..." into a local Unix timestamp. */
static time_t parse_glog_time(const char *line, time_t ref_now) {
	int mon = 0, day = 0, hr = 0, min = 0, sec = 0;
	if (!line || sscanf(line + 1, "%2d%2d %2d:%2d:%2d", &mon, &day, &hr, &min, &sec) != 5 ||
			mon < 1 || mon > 12 || day < 1 || day > 31) {
		return 0;
	}
	struct tm tm;
	localtime_r(&ref_now, &tm);
	tm.tm_mon = mon - 1;
	tm.tm_mday = day;
	tm.tm_hour = hr;
	tm.tm_min = min;
	tm.tm_sec = sec;
	tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	// Handle Dec -> Jan year rollover
	if (t > ref_now + 86400) {
		tm.tm_year--;
		tm.tm_isdst = -1;
		t = mktime(&tm);
	}
	return t > 0 ? t : 0;
}

static bool agy_error_matches_bucket(const char *line, const char *bucket) {
	if (!bucket || !*bucket) {
		return true;
	}
	if (strcasestr(line, "third-party quota") || strcasestr(line, "3p quota")) {
		return g_str_has_prefix(bucket, "3p-");
	}
	return true;
}

bool agent_resume_agy_session_hit_limit(const char *cli_log_path,
		const char *transcript_path, const char *bucket) {
	bool transcript_hit = false;
	time_t last_planner_at = 0;
	if (transcript_path && *transcript_path) {
		char *tail = read_file_tail(transcript_path, LOG_TAIL_BYTES);
		if (tail) {
			char *save = NULL;
			for (char *line = strtok_r(tail, "\n", &save); line;
					line = strtok_r(NULL, "\n", &save)) {
				if (!*line || *line != '{') {
					continue;
				}
				json_object *o = json_tokener_parse(line);
				if (!o || !json_object_is_type(o, json_type_object)) {
					json_object_put(o);
					continue;
				}
				const char *type = jstr(o, "type");
				time_t at = parse_iso8601(jstr(o, "created_at"));
				if (type && strcmp(type, "ERROR_MESSAGE") == 0) {
					const char *err = jstr(o, "error");
					if (!err) {
						err = jstr(o, "content");
					}
					if (!err) {
						err = jstr(o, "message");
					}
					if (text_mentions_usage_limit(err) && agy_error_matches_bucket(err, bucket)) {
						transcript_hit = true;
					}
				} else if (type && strcmp(type, "PLANNER_RESPONSE") == 0) {
					transcript_hit = false;
					if (at > last_planner_at) {
						last_planner_at = at;
					}
				} else if (type && strcmp(type, "USER_INPUT") == 0) {
					transcript_hit = false;
				}
				json_object_put(o);
			}
			free(tail);
		}
	}
	if (transcript_hit) {
		return true;
	}
	if (!cli_log_path || !*cli_log_path) {
		return false;
	}
	char *tail = read_file_tail(cli_log_path, LOG_TAIL_BYTES);
	if (!tail) {
		return false;
	}
	bool log_hit = false;
	time_t log_err_at = 0;
	time_t ref_now = time(NULL);
	char *save = NULL;
	for (char *line = strtok_r(tail, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == 'E' && strstr(line, "errorreport.go:") &&
				text_mentions_usage_limit(line) && agy_error_matches_bucket(line, bucket)) {
			log_hit = true;
			time_t t = parse_glog_time(line, ref_now);
			if (t > 0) {
				log_err_at = t;
			}
		} else if (strstr(line, "HandleUserInput called with text:")) {
			log_hit = false;
		}
	}
	free(tail);
	if (!log_hit) {
		return false;
	}
	// If the conversation produced a normal planner response after the error, it already moved on
	if (last_planner_at > 0 && log_err_at > 0 && last_planner_at > log_err_at + 2) {
		return false;
	}
	return true;
}

/* ---------- finding open terminal windows of an agent ---------- */

struct proc_entry {
	int pid, ppid, pgrp, tpgid, tty;
	char comm[64];
};

struct fg_cand {
	int pid;
	int tty;
	int agent;        // enum tw_agent or -1
	char *account_dir;
	char *model;
	char *cwd;
	char comm[64];
	int win_pid;      // ancestor pwindow->pid
};

static char *read_proc_bytes(const char *path, size_t *len) {
	*len = 0;
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap + 1);
	size_t got;
	while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
		n += got;
		if (n == cap) {
			if (cap >= (1 << 18)) {
				break;
			}
			char *grown = realloc(buf, cap * 2 + 1);
			if (!grown) {
				break;
			}
			buf = grown;
			cap *= 2;
		}
	}
	fclose(f);
	if (buf) {
		buf[n] = '\0';
	}
	*len = n;
	return buf;
}

static const char *env_lookup(const char *env, size_t len, const char *name) {
	size_t nl = strlen(name);
	for (size_t i = 0; i < len; ) {
		size_t entry_len = strnlen(env + i, len - i);
		if (entry_len > nl && strncmp(env + i, name, nl) == 0 && env[i + nl] == '=') {
			return env + i + nl + 1;
		}
		i += entry_len + 1;
	}
	return NULL;
}

static char *proc_account_dir(int pid, enum tw_agent agent) {
	if (agent != TW_AGENT_CLAUDE && agent != TW_AGENT_CODEX) {
		return NULL;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/environ", pid);
	size_t len = 0;
	char *env = read_proc_bytes(path, &len);
	if (!env) {
		return NULL;
	}
	const char *own = env_lookup(env, len,
		agent == TW_AGENT_CLAUDE ? "CLAUDE_CONFIG_DIR" : "CODEX_HOME");
	char *dir = NULL;
	if (own && *own) {
		dir = tw_claude_normalize_dir(own);
	} else {
		const char *home = env_lookup(env, len, "HOME");
		if (home && *home) {
			char *in_home = format_str("%s/%s", home,
				agent == TW_AGENT_CLAUDE ? ".claude" : ".codex");
			dir = tw_claude_normalize_dir(in_home);
			free(in_home);
		}
	}
	free(env);
	return dir;
}

static bool proc_has_tty_stdin(int pid) {
	char path[64], target[PATH_MAX];
	snprintf(path, sizeof(path), "/proc/%d/fd/0", pid);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0) {
		return false;
	}
	target[n] = '\0';
	return strncmp(target, "/dev/pts/", 9) == 0 || strncmp(target, "/dev/tty", 8) == 0;
}

static char *proc_cwd(int pid) {
	char path[64], target[PATH_MAX];
	snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0) {
		return NULL;
	}
	target[n] = '\0';
	return strdup(target);
}

static const char *worker_model_for_pid(const struct agent_usage *collected, int pid) {
	if (!collected || !collected->workers) {
		return NULL;
	}
	for (guint i = 0; i < collected->workers->len; i++) {
		const struct agent_worker *w = collected->workers->pdata[i];
		if (w->pid == pid && w->model && *w->model) {
			return w->model;
		}
	}
	return NULL;
}

static int title_names_dir(const char *title, const char *dir) {
	if (!title || !dir || !*dir) {
		return 0;
	}
	const char *home = getenv("HOME");
	int best = 0;
	for (const char *p = title; *p; p++) {
		if ((*p != '/' && *p != '~') || (p > title && !isspace((unsigned char)p[-1]) &&
				p[-1] != ':' && p[-1] != '(' && p[-1] != '[')) {
			continue;
		}
		size_t len = strcspn(p, " \t)]");
		char *path = strndup(p, len);
		char *full = path[0] == '~' && home ? format_str("%s%s", home, path + 1) : strdup(path);
		free(path);
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
			best = parts;
		}
		free(full);
	}
	return best;
}

static int cand_title_score(const char *title, const struct fg_cand *c) {
	if (!title || !*title) {
		return 0;
	}
	int score = title_names_dir(title, c->cwd) * 4;
	if (c->agent == TW_AGENT_CLAUDE && (strcasestr(title, "claude") ||
			strstr(title, "✳") || strstr(title, "⠐") || strstr(title, "⠂"))) {
		score += 2;
	} else if (c->agent == TW_AGENT_CODEX && strcasestr(title, "codex")) {
		score += 2;
	} else if (c->agent == TW_AGENT_ANTIGRAVITY && (strcasestr(title, "antigravity") ||
			strcasestr(title, "agy"))) {
		score += 2;
	} else if (!score && c->comm[0] && strlen(c->comm) > 2 && strcasestr(title, c->comm)) {
		score += 1;
	}
	return score;
}

static bool cand_matches_target(const struct fg_cand *c, enum tw_agent agent,
		const char *account_dir, const char *bucket) {
	if (c->agent != (int)agent) {
		return false;
	}
	if ((agent == TW_AGENT_CLAUDE || agent == TW_AGENT_CODEX) &&
			account_dir && *account_dir && c->account_dir && *c->account_dir &&
			strcmp(account_dir, c->account_dir) != 0) {
		return false;
	}
	if (agent == TW_AGENT_ANTIGRAVITY && bucket && *bucket &&
			!agent_resume_agy_bucket_matches_model(bucket, c->model)) {
		return false;
	}
	return true;
}

static char *cmdline_option(int pid, const char *opt) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	size_t len = 0;
	char *cmd = read_proc_bytes(path, &len);
	if (!cmd || !len) {
		free(cmd);
		return NULL;
	}
	const char *argv[64];
	int argc = 0;
	for (size_t pos = 0; pos < len && argc < 64; ) {
		size_t n = strnlen(cmd + pos, len - pos);
		argv[argc++] = cmd + pos;
		pos += n + 1;
	}
	char *res = NULL;
	size_t olen = strlen(opt);
	for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; i++) {
		if (strcmp(argv[i], opt) == 0 && i + 1 < argc && argv[i + 1][0]) {
			res = strdup(argv[i + 1]);
			break;
		}
		if (strncmp(argv[i], opt, olen) == 0 && argv[i][olen] == '=' && argv[i][olen + 1]) {
			res = strdup(argv[i] + olen + 1);
			break;
		}
	}
	free(cmd);
	return res;
}

static char *cmdline_codex_resume_id(int pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	size_t len = 0;
	char *cmd = read_proc_bytes(path, &len);
	if (!cmd || !len) {
		free(cmd);
		return NULL;
	}
	const char *argv[64];
	int argc = 0;
	for (size_t pos = 0; pos < len && argc < 64; ) {
		size_t n = strnlen(cmd + pos, len - pos);
		argv[argc++] = cmd + pos;
		pos += n + 1;
	}
	char *res = NULL;
	for (int i = 1; i + 1 < argc; i++) {
		if (strcmp(argv[i], "resume") == 0 && g_uuid_string_is_valid(argv[i + 1])) {
			res = strdup(argv[i + 1]);
			break;
		}
	}
	free(cmd);
	return res;
}

static char *find_file_recursive(const char *dir, const char *needle, const char *suffix,
		int depth) {
	if (!dir || !*dir) {
		return NULL;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return NULL;
	}
	char *found = NULL;
	struct dirent *de;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char *sub = format_str("%s/%s", dir, de->d_name);
		struct stat st;
		if (lstat(sub, &st) == 0) {
			if (S_ISDIR(st.st_mode) && depth > 0) {
				found = find_file_recursive(sub, needle, suffix, depth - 1);
			} else if (S_ISREG(st.st_mode) && (!needle || strstr(de->d_name, needle)) &&
					(!suffix || g_str_has_suffix(de->d_name, suffix))) {
				found = strdup(sub);
			}
		}
		free(sub);
	}
	closedir(d);
	return found;
}

static char *proc_fd_matching(int pid, const char *needle, const char *suffix) {
	char dir[64];
	snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
	DIR *d = opendir(dir);
	if (!d) {
		return NULL;
	}
	char *found = NULL;
	struct dirent *de;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char link_path[128], target[PATH_MAX];
		snprintf(link_path, sizeof(link_path), "%s/%s", dir, de->d_name);
		ssize_t n = readlink(link_path, target, sizeof(target) - 1);
		if (n <= 0) {
			continue;
		}
		target[n] = '\0';
		if ((!needle || strstr(target, needle)) && (!suffix || g_str_has_suffix(target, suffix))) {
			found = strdup(target);
		}
	}
	closedir(d);
	return found;
}

static char *find_claude_session_log(int pid, const char *account_dir) {
	char *dir = account_dir && *account_dir ? strdup(account_dir) : tw_claude_default_dir();
	char *session_id = NULL;
	if (dir) {
		char *meta_path = format_str("%s/sessions/%d.json", dir, pid);
		json_object *meta = json_object_from_file(meta_path);
		const char *sid = jstr(meta, "sessionId");
		if (sid && *sid) {
			session_id = strdup(sid);
		}
		json_object_put(meta);
		free(meta_path);
	}
	if (!session_id) {
		session_id = cmdline_option(pid, "--resume");
	}
	if (!session_id) {
		session_id = cmdline_option(pid, "--session-id");
	}
	char *log = NULL;
	if (dir && session_id && *session_id) {
		char *projects = format_str("%s/projects", dir);
		char *filename = format_str("%s.jsonl", session_id);
		log = find_file_recursive(projects, filename, ".jsonl", 2);
		free(filename);
		free(projects);
	}
	if (!log) {
		log = proc_fd_matching(pid, "/projects/", ".jsonl");
	}
	free(session_id);
	free(dir);
	return log;
}

static char *codex_rollout_cwd(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char *cwd = NULL;
	char *line = NULL;
	size_t cap = 0;
	for (int i = 0; i < 16 && !cwd && getline(&line, &cap, f) > 0; i++) {
		if (!strstr(line, "\"cwd\"")) {
			continue;
		}
		json_object *o = json_tokener_parse(line);
		json_object *payload = jfield(o, "payload");
		const char *s = jstr(payload, "cwd");
		if (!s) {
			json_object *state = jfield(payload, "state");
			json_object *envs = jfield(jfield(state, "environments"), "environments");
			s = jstr(jfield(envs, "local"), "cwd");
		}
		if (s && *s) {
			cwd = strdup(s);
		}
		json_object_put(o);
	}
	free(line);
	fclose(f);
	return cwd;
}

static bool codex_session_hit_limit(const struct fg_cand *c, GHashTable *procs) {
	char *dir = c->account_dir && *c->account_dir ? strdup(c->account_dir) : tw_codex_default_dir();
	char *resume_id = cmdline_codex_resume_id(c->pid);
	char *direct = NULL;
	if (dir && resume_id) {
		char *sessions = format_str("%s/sessions", dir);
		direct = find_file_recursive(sessions, resume_id, ".jsonl", 4);
		free(sessions);
	}
	free(resume_id);
	if (!direct) {
		direct = proc_fd_matching(c->pid, "/rollout-", ".jsonl");
	}
	if (direct) {
		bool hit = agent_resume_codex_log_hit_limit(direct);
		free(direct);
		free(dir);
		return hit;
	}

	// Modern Codex keeps open rollout fds in its codex app-server daemon; match by cwd.
	if (!c->cwd || !*c->cwd || !procs) {
		free(dir);
		return false;
	}
	char *prefix = dir ? format_str("%s/sessions/", dir) : NULL;
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GPtrArray *matches = g_ptr_array_new_with_free_func(free);
	GHashTableIter iter;
	gpointer val;
	g_hash_table_iter_init(&iter, procs);
	while (g_hash_table_iter_next(&iter, NULL, &val)) {
		struct proc_entry *pe = val;
		if (strcmp(pe->comm, "codex") != 0 && !g_str_has_prefix(pe->comm, "codex")) {
			continue;
		}
		char fddir[64];
		snprintf(fddir, sizeof(fddir), "/proc/%d/fd", pe->pid);
		DIR *d = opendir(fddir);
		struct dirent *de;
		while (d && (de = readdir(d))) {
			if (de->d_name[0] == '.') {
				continue;
			}
			char lp[128], target[PATH_MAX];
			snprintf(lp, sizeof(lp), "%s/%s", fddir, de->d_name);
			ssize_t n = readlink(lp, target, sizeof(target) - 1);
			if (n <= 0) {
				continue;
			}
			target[n] = '\0';
			if (!strstr(target, "/rollout-") || !g_str_has_suffix(target, ".jsonl")) {
				continue;
			}
			if (prefix && !g_str_has_prefix(target, prefix)) {
				continue;
			}
			if (g_hash_table_contains(seen, target)) {
				continue;
			}
			g_hash_table_add(seen, g_strdup(target));
			char *rcwd = codex_rollout_cwd(target);
			if (rcwd && strcmp(rcwd, c->cwd) == 0) {
				g_ptr_array_add(matches, strdup(target));
			}
			free(rcwd);
		}
		if (d) {
			closedir(d);
		}
	}
	bool hit = false;
	if (matches->len == 1) {
		hit = agent_resume_codex_log_hit_limit(matches->pdata[0]);
	} else if (matches->len > 1) {
		// Multiple open rollouts share this cwd: only resume if every one of them hit limits
		hit = true;
		for (guint i = 0; i < matches->len; i++) {
			if (!agent_resume_codex_log_hit_limit(matches->pdata[i])) {
				hit = false;
				break;
			}
		}
	}
	g_ptr_array_unref(matches);
	g_hash_table_unref(seen);
	free(prefix);
	free(dir);
	return hit;
}

static bool agy_session_hit_limit(const struct fg_cand *c, const char *bucket) {
	char fddir[64];
	snprintf(fddir, sizeof(fddir), "/proc/%d/fd", c->pid);
	DIR *d = opendir(fddir);
	if (!d) {
		return false;
	}
	char *cli_log = NULL;
	char *transcript = NULL;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char lp[128], target[PATH_MAX];
		snprintf(lp, sizeof(lp), "%s/%s", fddir, de->d_name);
		ssize_t n = readlink(lp, target, sizeof(target) - 1);
		if (n <= 0) {
			continue;
		}
		target[n] = '\0';
		if (!cli_log && strstr(target, "/log/cli-") && g_str_has_suffix(target, ".log")) {
			cli_log = strdup(target);
		}
		if (!transcript) {
			const char *conv = strstr(target, "/conversations/");
			const char *pres = strstr(target, "/presence/");
			const char *mark = conv ? conv : pres;
			const char *suf = conv ? ".db" : ".lock";
			if (mark && g_str_has_suffix(target, suf)) {
				const char *id_start = mark + (conv ? strlen("/conversations/") : strlen("/presence/"));
				size_t id_len = strlen(id_start) - strlen(suf);
				if (id_len > 0 && id_len <= 64 && !memchr(id_start, '/', id_len)) {
					char *root = strndup(target, (size_t)(mark - target));
					char *id = strndup(id_start, id_len);
					transcript = format_str("%s/brain/%s/.system_generated/logs/transcript.jsonl",
						root, id);
					free(id);
					free(root);
				}
			}
		}
	}
	closedir(d);
	bool hit = agent_resume_agy_session_hit_limit(cli_log, transcript, bucket);
	free(cli_log);
	free(transcript);
	return hit;
}

/*
 * Only sessions that explicitly recorded hitting a usage/rate limit (and have
 * not progressed past it) are eligible for auto-resume. Idle sessions are left alone.
 */
static bool cand_hit_usage_limit(const struct fg_cand *c, GHashTable *procs, const char *bucket) {
	if (c->agent == TW_AGENT_CLAUDE) {
		char *log = find_claude_session_log(c->pid, c->account_dir);
		bool hit = log && agent_resume_claude_log_hit_limit(log);
		free(log);
		return hit;
	}
	if (c->agent == TW_AGENT_CODEX) {
		return codex_session_hit_limit(c, procs);
	}
	if (c->agent == TW_AGENT_ANTIGRAVITY) {
		return agy_session_hit_limit(c, bucket);
	}
	return false;
}

static void fg_cand_free(gpointer ptr) {
	struct fg_cand *c = ptr;
	free(c->account_dir);
	free(c->model);
	free(c->cwd);
	free(c);
}

/*
 * Finds open terminal windows currently running an interactive foreground
 * session of (agent, account_dir, bucket). Appends their con_ids to out_ids.
 */
static int find_agent_windows(struct panel *panel, enum tw_agent agent,
		const char *account_dir, const char *bucket, const struct agent_usage *collected,
		GArray *out_ids) {
	if (!panel || !panel->state.windows || panel->state.windows->length == 0) {
		return 0;
	}

	// Collect all terminal window PIDs
	GHashTable *term_pids = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (int i = 0; i < panel->state.windows->length; i++) {
		struct pwindow *w = panel->state.windows->items[i];
		if (w->pid > 0 && agent_resume_is_terminal_app(w->app_id)) {
			g_hash_table_add(term_pids, GINT_TO_POINTER(w->pid));
		}
	}
	if (g_hash_table_size(term_pids) == 0) {
		g_hash_table_unref(term_pids);
		return 0;
	}

	// Snapshot /proc for this user
	GHashTable *procs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free);
	DIR *dir = opendir("/proc");
	uid_t me = getuid();
	struct dirent *de;
	while (dir && (de = readdir(dir))) {
		if (de->d_name[0] < '1' || de->d_name[0] > '9') {
			continue;
		}
		int pid = atoi(de->d_name);
		char path[64], line[1024];
		struct stat st;
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		if (stat(path, &st) != 0 || st.st_uid != me) {
			continue;
		}
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		bool ok = fgets(line, sizeof(line), f) != NULL;
		fclose(f);
		if (!ok) {
			continue;
		}
		char *start = strchr(line, '('), *end = strrchr(line, ')');
		char state = 0;
		int ppid = 0, pgrp = 0, session = 0, tty = 0, tpgid = -1;
		if (!start || !end || end <= start ||
				sscanf(end + 1, " %c %d %d %d %d %d", &state, &ppid, &pgrp, &session,
					&tty, &tpgid) != 6 ||
				state == 'Z' || state == 'X') {
			continue;
		}
		struct proc_entry *pe = calloc(1, sizeof(*pe));
		pe->pid = pid;
		pe->ppid = ppid;
		pe->pgrp = pgrp;
		pe->tpgid = tpgid;
		pe->tty = tty;
		size_t clen = (size_t)(end - start - 1);
		if (clen >= sizeof(pe->comm)) {
			clen = sizeof(pe->comm) - 1;
		}
		memcpy(pe->comm, start + 1, clen);
		pe->comm[clen] = '\0';
		g_hash_table_insert(procs, GINT_TO_POINTER(pid), pe);
	}
	if (dir) {
		closedir(dir);
	}

	// Build foreground candidates whose ancestor chain hits an open terminal window PID
	GPtrArray *cands = g_ptr_array_new_with_free_func(fg_cand_free);
	GHashTableIter iter;
	gpointer val;
	g_hash_table_iter_init(&iter, procs);
	while (g_hash_table_iter_next(&iter, NULL, &val)) {
		struct proc_entry *pe = val;
		if (pe->tty == 0 || pe->pgrp <= 0 || pe->pgrp != pe->tpgid) {
			continue;
		}
		int ancestor = 0;
		int cur = pe->ppid;
		for (int depth = 0; depth < 32 && cur > 1; depth++) {
			if (g_hash_table_contains(term_pids, GINT_TO_POINTER(cur))) {
				ancestor = cur;
				break;
			}
			struct proc_entry *parent = g_hash_table_lookup(procs, GINT_TO_POINTER(cur));
			if (!parent || parent->ppid == cur) {
				break;
			}
			cur = parent->ppid;
		}
		if (!ancestor) {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pe->pid);
		size_t clen = 0;
		char *cmd = read_proc_bytes(path, &clen);
		int ag = cmd ? agent_resume_interactive_agent(cmd, clen) : -1;
		free(cmd);
		if (ag >= 0 && !proc_has_tty_stdin(pe->pid)) {
			ag = -1;
		}
		struct fg_cand *c = calloc(1, sizeof(*c));
		c->pid = pe->pid;
		c->tty = pe->tty;
		c->agent = ag;
		c->win_pid = ancestor;
		snprintf(c->comm, sizeof(c->comm), "%s", pe->comm);
		c->cwd = proc_cwd(pe->pid);
		if (ag >= 0) {
			c->account_dir = proc_account_dir(pe->pid, (enum tw_agent)ag);
			const char *m = worker_model_for_pid(collected, pe->pid);
			c->model = m ? strdup(m) : NULL;
		}
		g_ptr_array_add(cands, c);
	}

	// Deduplicate child processes sharing the same tty and agent with an ancestor candidate
	for (guint i = cands->len; i > 0; i--) {
		struct fg_cand *child = cands->pdata[i - 1];
		struct proc_entry *pe = g_hash_table_lookup(procs, GINT_TO_POINTER(child->pid));
		bool dup = false;
		for (guint j = 0; pe && j < cands->len; j++) {
			if (j == i - 1) {
				continue;
			}
			struct fg_cand *other = cands->pdata[j];
			if (other->tty == child->tty && other->pid == pe->ppid) {
				// If a shell spawned an agent on the same tty, keep the agent (child);
				// if an agent spawned a helper or child of its own, keep the parent agent.
				if (other->agent == child->agent || (other->agent >= 0 && child->agent < 0)) {
					dup = true;
					break;
				}
			}
		}
		if (dup) {
			g_ptr_array_remove_index(cands, i - 1);
		}
	}

	int matched = 0;
	for (int i = 0; i < panel->state.windows->length; i++) {
		struct pwindow *win = panel->state.windows->items[i];
		if (win->pid <= 0 || !g_hash_table_contains(term_pids, GINT_TO_POINTER(win->pid))) {
			continue;
		}
		GPtrArray *mine = g_ptr_array_new();
		for (guint j = 0; j < cands->len; j++) {
			struct fg_cand *c = cands->pdata[j];
			if (c->win_pid == win->pid) {
				g_ptr_array_add(mine, c);
			}
		}
		const struct fg_cand *picked = NULL;
		if (mine->len == 1) {
			// Count how many open terminal windows share this PID
			int same_pid_wins = 0;
			for (int k = 0; k < panel->state.windows->length; k++) {
				struct pwindow *other = panel->state.windows->items[k];
				if (other->pid == win->pid && agent_resume_is_terminal_app(other->app_id)) {
					same_pid_wins++;
				}
			}
			if (same_pid_wins == 1 || cand_title_score(win->title, mine->pdata[0]) > 0) {
				picked = mine->pdata[0];
			}
		} else if (mine->len > 1) {
			int best = 0;
			bool tie = false;
			for (guint j = 0; j < mine->len; j++) {
				const struct fg_cand *c = mine->pdata[j];
				int score = cand_title_score(win->title, c);
				if (score > best) {
					best = score;
					picked = c;
					tie = false;
				} else if (score == best && score > 0 && picked) {
					bool both_match =
						cand_matches_target(picked, agent, account_dir, bucket) &&
						cand_matches_target(c, agent, account_dir, bucket);
					if (!both_match) {
						tie = true;
					}
				}
			}
			if (tie) {
				picked = NULL;
			}
			if (!picked) {
				// Fallback like git.c: the sole candidate not claimed by another window
				// of the same terminal server process
				const struct fg_cand *left = NULL;
				int unclaimed = 0;
				for (guint j = 0; j < mine->len; j++) {
					const struct fg_cand *c = mine->pdata[j];
					bool claimed = false;
					for (int k = 0; k < panel->state.windows->length && !claimed; k++) {
						struct pwindow *other = panel->state.windows->items[k];
						claimed = other != win && other->pid == win->pid &&
							cand_title_score(other->title, c) > 0;
					}
					if (!claimed) {
						left = c;
						unclaimed++;
					}
				}
				if (unclaimed == 1) {
					picked = left;
				}
			}
		}
		g_ptr_array_unref(mine);
		if (picked && cand_matches_target(picked, agent, account_dir, bucket) &&
				cand_hit_usage_limit(picked, procs, bucket)) {
			matched++;
			if (out_ids) {
				g_array_append_val(out_ids, win->id);
			}
		}
	}

	g_ptr_array_unref(cands);
	g_hash_table_unref(procs);
	g_hash_table_unref(term_pids);
	return matched;
}

/* ---------- virtual keyboard typing ---------- */

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void destroy_virtual_keyboard(void) {
	if (ar.vk) {
		zwp_virtual_keyboard_v1_destroy(ar.vk);
		ar.vk = NULL;
		if (ar.panel && ar.panel->display) {
			wl_display_flush(ar.panel->display);
		}
	}
}

static bool setup_virtual_keyboard(struct panel *panel) {
	if (ar.vk) {
		return true;
	}
	struct panel_seat *seat = panel ? panel_first_seat(panel) : NULL;
	if (!panel || !panel->virtual_keyboard_manager || !seat || !seat->wl_seat) {
		sway_log(SWAY_ERROR, "Agent auto-resume: compositor offers no virtual keyboard");
		return false;
	}
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *keymap = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
	char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	bool ok = false;
	if (text) {
		size_t size = strlen(text) + 1;
		int fd = memfd_create("tilewin-agent-keymap", MFD_CLOEXEC);
		if (fd >= 0 && write(fd, text, size) == (ssize_t)size) {
			ar.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
				panel->virtual_keyboard_manager, seat->wl_seat);
			zwp_virtual_keyboard_v1_keymap(ar.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
			ar.shift_mask = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
			wl_display_flush(panel->display);
			ok = true;
		}
		if (fd >= 0) {
			close(fd);
		}
		free(text);
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	return ok;
}

static void vk_send_key(uint32_t code, bool down, uint32_t mods) {
	if (!ar.vk) {
		return;
	}
	zwp_virtual_keyboard_v1_key(ar.vk, now_ms(), code,
		down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(ar.vk, mods, 0, 0, 0);
}

static void vk_tap(uint32_t code, bool shift) {
	if (!ar.vk || !ar.panel) {
		return;
	}
	uint32_t mods = shift ? ar.shift_mask : 0;
	if (shift) {
		vk_send_key(KEY_LEFTSHIFT, true, mods);
	}
	vk_send_key(code, true, mods);
	vk_send_key(code, false, mods);
	if (shift) {
		vk_send_key(KEY_LEFTSHIFT, false, 0);
	}
	wl_display_flush(ar.panel->display);
}

static void job_free(gpointer ptr) {
	struct resume_job *j = ptr;
	if (!j) {
		return;
	}
	free(j->command);
	free(j->agent_name);
	free(j);
}

static void schedule_step(int ms);
static void run_next_job(void);

static void finish_batch_if_done(void) {
	if (ar.active || (ar.jobs && ar.jobs->len > 0)) {
		return;
	}
	ar.stage = TYPE_IDLE;
	destroy_virtual_keyboard();
	if (ar.has_saved_focus && ar.panel && !ar.panel->state.locked) {
		if (ar.saved_focus > 0 && ar.saved_focus != ar.panel->state.focused_window) {
			struct pwindow *prev = panel_find_window(ar.panel, ar.saved_focus);
			if (prev && !prev->minimized) {
				ipc_panel_commandf(ar.panel, "[con_id=%lld] focus",
					(long long)ar.saved_focus);
			}
		}
	}
	ar.has_saved_focus = false;
	ar.saved_focus = 0;
}

static void step_fired(void *data) {
	ar.step_timer = NULL;
	struct panel *panel = ar.panel;
	if (!panel || !ar.active) {
		finish_batch_if_done();
		return;
	}
	if (panel->state.locked) {
		// Pause cleanly until unlocked; keep the active job at the front
		g_ptr_array_insert(ar.jobs, 0, ar.active);
		ar.active = NULL;
		ar.stage = TYPE_IDLE;
		destroy_virtual_keyboard();
		return;
	}
	struct resume_job *job = ar.active;
	if (ar.stage == TYPE_FOCUSING) {
		if (panel->state.focused_window != job->con_id) {
			ipc_panel_refresh_tree(panel);
		}
		if (panel->state.focused_window != job->con_id) {
			sway_log(SWAY_ERROR, "Agent auto-resume: window %lld did not gain focus, skipping",
				(long long)job->con_id);
			ar.stage = TYPE_AFTER;
			schedule_step(10);
			return;
		}
		if (!setup_virtual_keyboard(panel)) {
			ar.stage = TYPE_AFTER;
			schedule_step(10);
			return;
		}
		if (!job->command[0]) {
			ar.stage = TYPE_ENTER;
			schedule_step(ENTER_WAIT_MS);
			return;
		}
		if (command_is_ascii(job->command)) {
			ar.char_pos = 0;
			ar.used_paste = false;
			ar.stage = TYPE_TYPING;
			schedule_step(KEY_STEP_MS);
		} else {
			ar.used_paste = true;
			clipboard_paste_text(panel, job->command);
			ar.stage = TYPE_ENTER;
			schedule_step(PASTE_ENTER_WAIT_MS);
		}
	} else if (ar.stage == TYPE_TYPING) {
		if (panel->state.focused_window != job->con_id) {
			ar.stage = TYPE_AFTER;
			schedule_step(10);
			return;
		}
		char ch = job->command[ar.char_pos];
		if (ch) {
			uint32_t code;
			bool shift;
			if (agent_resume_ascii_key(ch, &code, &shift)) {
				vk_tap(code, shift);
			}
			ar.char_pos++;
			if (job->command[ar.char_pos]) {
				schedule_step(KEY_STEP_MS);
			} else {
				ar.stage = TYPE_ENTER;
				schedule_step(ENTER_WAIT_MS);
			}
		} else {
			ar.stage = TYPE_ENTER;
			schedule_step(ENTER_WAIT_MS);
		}
	} else if (ar.stage == TYPE_ENTER) {
		if (panel->state.focused_window == job->con_id) {
			vk_tap(KEY_ENTER, false);
		}
		ar.stage = TYPE_AFTER;
		schedule_step(AFTER_ENTER_MS);
	} else if (ar.stage == TYPE_AFTER) {
		if (job->was_minimized && panel_find_window(panel, job->con_id)) {
			ipc_panel_commandf(panel, "[con_id=%lld] minimize enable",
				(long long)job->con_id);
		}
		job_free(ar.active);
		ar.active = NULL;
		ar.stage = TYPE_IDLE;
		run_next_job();
	}
}

static void schedule_step(int ms) {
	if (!ar.panel || !ar.panel->loop) {
		return;
	}
	if (ar.step_timer) {
		loop_remove_timer(ar.panel->loop, ar.step_timer);
	}
	ar.step_timer = loop_add_timer(ar.panel->loop, ms, step_fired, NULL);
}

static void run_next_job(void) {
	struct panel *panel = ar.panel;
	if (!panel || ar.active || panel->state.locked) {
		return;
	}
	while (ar.jobs && ar.jobs->len > 0) {
		struct resume_job *job = g_ptr_array_steal_index(ar.jobs, 0);
		struct pwindow *win = panel_find_window(panel, job->con_id);
		if (!win) {
			job_free(job);
			continue;
		}
		if (!ar.has_saved_focus) {
			ar.saved_focus = panel->state.focused_window;
			ar.has_saved_focus = true;
		}
		job->was_minimized = win->minimized;
		ar.active = job;
		ar.stage = TYPE_FOCUSING;
		ipc_panel_commandf(panel, win->minimized ?
			"[con_id=%lld] minimize disable, focus" : "[con_id=%lld] focus",
			(long long)job->con_id);
		schedule_step(FOCUS_WAIT_MS);
		return;
	}
	finish_batch_if_done();
}

static bool job_already_queued(int64_t con_id) {
	if (ar.active && ar.active->con_id == con_id) {
		return true;
	}
	for (guint i = 0; ar.jobs && i < ar.jobs->len; i++) {
		struct resume_job *j = ar.jobs->pdata[i];
		if (j->con_id == con_id) {
			return true;
		}
	}
	return false;
}

/* ---------- slot tracking & scheduling ---------- */

static void slot_free(gpointer ptr) {
	struct resume_slot *s = ptr;
	free(s->account_id);
	free(s->account_dir);
	free(s->bucket);
	free(s);
}

static struct resume_slot *find_or_add_slot(enum tw_agent agent, const char *account_id,
		const char *account_dir, const char *bucket, bool seven_day) {
	if (!ar.slots) {
		ar.slots = g_ptr_array_new_with_free_func(slot_free);
	}
	for (guint i = 0; i < ar.slots->len; i++) {
		struct resume_slot *s = ar.slots->pdata[i];
		if (s->agent == agent && g_strcmp0(s->account_id, account_id) == 0 &&
				g_strcmp0(s->bucket, bucket) == 0) {
			if (account_dir && g_strcmp0(s->account_dir, account_dir) != 0) {
				free(s->account_dir);
				s->account_dir = strdup(account_dir);
			}
			return s;
		}
	}
	struct resume_slot *s = calloc(1, sizeof(*s));
	s->agent = agent;
	s->account_id = account_id ? strdup(account_id) : strdup("");
	s->account_dir = account_dir ? strdup(account_dir) : NULL;
	s->bucket = bucket ? strdup(bucket) : strdup("");
	s->seven_day = seven_day;
	g_ptr_array_add(ar.slots, s);
	return s;
}

static void reschedule_wake(time_t now);

static void fire_slot(struct resume_slot *s) {
	struct panel *panel = ar.panel;
	if (!panel || !ar.enabled) {
		return;
	}
	if (panel->state.locked) {
		s->deferred_lock = true;
		return;
	}
	s->deferred_lock = false;
	if (!s->seven_day && s->pending_weekly_blocked) {
		// Still blocked by the weekly quota: wait for the weekly window's reset instead
		s->pending = false;
		s->last_fired_reset = s->pending_resets_at;
		return;
	}
	s->pending = false;
	s->last_fired_reset = s->pending_resets_at;

	ipc_panel_refresh_tree(panel);
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
	int matched = find_agent_windows(panel, s->agent, s->account_dir,
		s->agent == TW_AGENT_ANTIGRAVITY ? s->bucket : NULL, ar.collected, ids);
	sway_log(SWAY_DEBUG, "Agent auto-resume: %s (%s) reset fired, matched %d limited terminal(s)",
		tw_agent_names[s->agent], s->bucket, matched);
	const char *cmd = ar.command && *ar.command ? ar.command : AGENT_RESUME_DEFAULT_COMMAND;
	const char *name = tw_agent_names[s->agent];
	int queued = 0;
	for (guint i = 0; i < ids->len; i++) {
		int64_t id = g_array_index(ids, int64_t, i);
		if (job_already_queued(id)) {
			continue;
		}
		if (!ar.jobs) {
			ar.jobs = g_ptr_array_new_with_free_func(job_free);
		}
		struct resume_job *job = calloc(1, sizeof(*job));
		job->con_id = id;
		job->command = strdup(cmd);
		job->agent_name = strdup(name);
		g_ptr_array_add(ar.jobs, job);
		queued++;
	}
	g_array_unref(ids);
	if (queued > 0) {
		char summary[96], body[192];
		snprintf(summary, sizeof(summary), "Resumed %s", name);
		snprintf(body, sizeof(body), "Sent \"%s\" to %d interactive terminal %s.",
			cmd, queued, queued == 1 ? "window" : "windows");
		notify_local(panel, "AI agent usage", "utilities-terminal", summary, body);
		run_next_job();
	}
}

static void check_due_slots(time_t now) {
	if (!ar.enabled || !ar.slots) {
		return;
	}
	for (guint i = 0; i < ar.slots->len; i++) {
		struct resume_slot *s = ar.slots->pdata[i];
		// Promote an armed window into the post-reset countdown as soon as resets_at passes
		if (s->armed && s->resets_at > 0 && now >= s->resets_at) {
			if (!tw_claude_same_window(s->last_fired_reset, s->resets_at) &&
					!(s->pending && tw_claude_same_window(s->pending_resets_at, s->resets_at))) {
				s->pending = true;
				s->pending_resets_at = s->resets_at;
				s->pending_fire_at = s->resets_at + ar.delay_s;
				s->pending_weekly_blocked = s->weekly_blocked;
				s->deferred_lock = false;
			}
			s->armed = false;
		}
		if (s->pending && now >= s->pending_fire_at) {
			fire_slot(s);
		}
	}
	reschedule_wake(now);
}

static void wake_fired(void *data) {
	ar.wake_timer = NULL;
	check_due_slots(time(NULL));
}

static void reschedule_wake(time_t now) {
	if (!ar.panel || !ar.panel->loop) {
		return;
	}
	if (ar.wake_timer) {
		loop_remove_timer(ar.panel->loop, ar.wake_timer);
		ar.wake_timer = NULL;
	}
	if (!ar.enabled || !ar.slots) {
		return;
	}
	time_t earliest = 0;
	for (guint i = 0; i < ar.slots->len; i++) {
		struct resume_slot *s = ar.slots->pdata[i];
		time_t target = 0;
		if (s->pending && !s->deferred_lock) {
			target = s->pending_fire_at;
		} else if (s->armed && s->resets_at > 0) {
			target = s->resets_at + ar.delay_s;
		}
		if (target > 0 && (!earliest || target < earliest)) {
			earliest = target;
		}
	}
	if (earliest > 0) {
		long delta = (long)(earliest - now);
		int ms = delta <= 0 ? 50 : delta > 3600 ? 3600 * 1000 : (int)(delta * 1000);
		ar.wake_timer = loop_add_timer(ar.panel->loop, ms, wake_fired, NULL);
	}
}

void agent_resume_init(struct panel *panel) {
	ar.panel = panel;
	if (!ar.slots) {
		ar.slots = g_ptr_array_new_with_free_func(slot_free);
	}
	if (!ar.jobs) {
		ar.jobs = g_ptr_array_new_with_free_func(job_free);
	}
}

void agent_resume_fini(void) {
	if (ar.panel && ar.panel->loop) {
		if (ar.wake_timer) {
			loop_remove_timer(ar.panel->loop, ar.wake_timer);
			ar.wake_timer = NULL;
		}
		if (ar.step_timer) {
			loop_remove_timer(ar.panel->loop, ar.step_timer);
			ar.step_timer = NULL;
		}
	}
	destroy_virtual_keyboard();
	job_free(ar.active);
	ar.active = NULL;
	ar.stage = TYPE_IDLE;
	ar.has_saved_focus = false;
	ar.saved_focus = 0;
	if (ar.jobs) {
		g_ptr_array_unref(ar.jobs);
		ar.jobs = NULL;
	}
	if (ar.slots) {
		g_ptr_array_unref(ar.slots);
		ar.slots = NULL;
	}
	agent_usage_free(ar.collected);
	ar.collected = NULL;
	free(ar.command);
	ar.command = NULL;
	ar.enabled = false;
	ar.panel = NULL;
}

void agent_resume_configure(struct panel *panel, const struct agent_resume_config *cfg) {
	if (!ar.panel) {
		agent_resume_init(panel);
	}
	bool was_enabled = ar.enabled;
	ar.enabled = cfg->enabled;
	ar.delay_s = cfg->delay_s < 0 ? 0 :
		cfg->delay_s > AGENT_RESUME_MAX_DELAY ? AGENT_RESUME_MAX_DELAY : cfg->delay_s;
	ar.threshold = cfg->threshold < 1 ? 1 : cfg->threshold > 100 ? 100 : cfg->threshold;
	const char *cmd = cfg->command && *cfg->command ? cfg->command : AGENT_RESUME_DEFAULT_COMMAND;
	if (!ar.command || strcmp(ar.command, cmd) != 0) {
		free(ar.command);
		ar.command = strdup(cmd);
	}
	if (!ar.enabled) {
		if (was_enabled && ar.slots) {
			for (guint i = 0; i < ar.slots->len; i++) {
				struct resume_slot *s = ar.slots->pdata[i];
				s->armed = false;
				s->pending = false;
				s->deferred_lock = false;
			}
		}
		reschedule_wake(time(NULL));
		return;
	}
	// Recompute pending fire times if delay changed while armed/pending
	if (ar.slots) {
		for (guint i = 0; i < ar.slots->len; i++) {
			struct resume_slot *s = ar.slots->pdata[i];
			if (s->pending) {
				s->pending_fire_at = s->pending_resets_at + ar.delay_s;
			}
		}
	}
	reschedule_wake(time(NULL));
}

void agent_resume_observe(struct panel *panel, const struct agent_resume_window_input *in,
		const struct agent_usage *collected, time_t now) {
	if (!in || (in->agent != TW_AGENT_CLAUDE && in->agent != TW_AGENT_CODEX &&
			in->agent != TW_AGENT_ANTIGRAVITY)) {
		return;
	}
	if (!ar.panel) {
		agent_resume_init(panel);
	}
	if (collected && collected != ar.collected) {
		agent_usage_free(ar.collected);
		ar.collected = agent_usage_ref(collected);
	}
	if (!in->window.known || in->window.resets_at <= 0) {
		check_due_slots(now);
		return;
	}
	struct resume_slot *s = find_or_add_slot(in->agent, in->account_id, in->account_dir,
		in->bucket, in->seven_day);
	time_t reset = in->window.resets_at;

	if (s->resets_at > 0 && tw_claude_same_window(s->resets_at, reset)) {
		// Same window (minor timestamp wobble allowed)
		s->resets_at = reset;
		if (in->window.used > s->peak_used) {
			s->peak_used = in->window.used;
		}
		s->weekly_blocked = in->weekly_blocked;
		if (s->pending && tw_claude_same_window(s->pending_resets_at, reset)) {
			s->pending_weekly_blocked = in->weekly_blocked;
		}
		if (ar.enabled && reset > now && s->peak_used >= ar.threshold &&
				!tw_claude_same_window(s->last_fired_reset, reset)) {
			s->armed = true;
		}
	} else if (reset > now) {
		// A new future window arrived. If the old armed window already expired,
		// promote it first so its delay countdown is not lost!
		if (ar.enabled && s->armed && s->resets_at > 0 && now >= s->resets_at &&
				!tw_claude_same_window(s->last_fired_reset, s->resets_at)) {
			s->pending = true;
			s->pending_resets_at = s->resets_at;
			s->pending_fire_at = s->resets_at + ar.delay_s;
			s->pending_weekly_blocked = s->weekly_blocked;
			s->deferred_lock = false;
		}
		s->resets_at = reset;
		s->peak_used = in->window.used;
		s->weekly_blocked = in->weekly_blocked;
		s->armed = ar.enabled && s->peak_used >= ar.threshold &&
			!tw_claude_same_window(s->last_fired_reset, reset);
	} else {
		// An already-expired window we never saw in the future: record as past so we
		// don't spuriously fire for stale cached usage from before startup.
		if (!s->armed && !s->pending) {
			s->resets_at = reset;
			s->peak_used = in->window.used;
			s->last_fired_reset = reset;
		}
	}

	check_due_slots(now);
}

void agent_resume_lock_changed(struct panel *panel) {
	if (!panel || panel->state.locked) {
		return;
	}
	check_due_slots(time(NULL));
	run_next_job();
}

void agent_resume_account_status(struct panel *panel, enum tw_agent agent,
		const char *account_id, const char *account_dir, const struct agent_usage *collected,
		struct agent_resume_status *out) {
	*out = (struct agent_resume_status){
		.enabled = ar.enabled,
		.delay_s = ar.delay_s,
	};
	if (agent != TW_AGENT_CLAUDE && agent != TW_AGENT_CODEX &&
			agent != TW_AGENT_ANTIGRAVITY) {
		out->enabled = false;
		return;
	}
	if (!ar.enabled) {
		return;
	}
	out->terminals = find_agent_windows(panel, agent, account_dir, NULL,
		collected ? collected : ar.collected, NULL);
	for (guint i = 0; ar.slots && i < ar.slots->len; i++) {
		struct resume_slot *s = ar.slots->pdata[i];
		if (s->agent != agent || g_strcmp0(s->account_id, account_id) != 0) {
			continue;
		}
		if (s->pending && s->deferred_lock) {
			out->deferred = true;
			out->scheduled = true;
		} else if (s->pending && !(s->pending_weekly_blocked && !s->seven_day)) {
			out->scheduled = true;
			if (!out->fire_at || s->pending_fire_at < out->fire_at) {
				out->fire_at = s->pending_fire_at;
			}
		} else if (s->armed && !(s->weekly_blocked && !s->seven_day)) {
			time_t at = s->resets_at + ar.delay_s;
			out->scheduled = true;
			if (!out->fire_at || at < out->fire_at) {
				out->fire_at = at;
			}
		}
	}
}
