#define _GNU_SOURCE
#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "agentresume.h"
#include "agentusage.h"
#include "panel.h"

/* ---------- lightweight stubs for panel symbols ---------- */

static int ipc_cmd_count;
static int notify_count;
static char last_notify_summary[128];

struct pwindow *panel_find_window(struct panel *panel, int64_t id) {
	for (int i = 0; panel && panel->state.windows && i < panel->state.windows->length; i++) {
		struct pwindow *w = panel->state.windows->items[i];
		if (w->id == id) {
			return w;
		}
	}
	return NULL;
}

bool ipc_panel_commandf(struct panel *panel, const char *fmt, ...) {
	ipc_cmd_count++;
	return true;
}

void ipc_panel_refresh_tree(struct panel *panel) {}

struct panel_seat *panel_first_seat(struct panel *panel) {
	return NULL;
}

void notify_local(struct panel *panel, const char *app_name, const char *icon,
		const char *summary, const char *body) {
	notify_count++;
	snprintf(last_notify_summary, sizeof(last_notify_summary), "%s", summary ? summary : "");
}

void clipboard_paste_text(struct panel *panel, const char *text) {}

struct agent_usage *agent_usage_ref(const struct agent_usage *usage) {
	return (struct agent_usage *)usage;
}

void agent_usage_free(struct agent_usage *usage) {}

/* ---------- test harness ---------- */

static int failures;

static void check(bool ok, const char *msg) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", msg);
	if (!ok) {
		failures++;
	}
}

static size_t make_cmdline(char *buf, size_t size, const char *const *argv) {
	size_t pos = 0;
	for (size_t i = 0; argv[i]; i++) {
		size_t n = strlen(argv[i]) + 1;
		if (pos + n > size) {
			break;
		}
		memcpy(buf + pos, argv[i], n);
		pos += n;
	}
	return pos;
}

static void test_terminal_apps(void) {
	check(agent_resume_is_terminal_app("xfce4-terminal"), "xfce4-terminal is a terminal");
	check(agent_resume_is_terminal_app("foot"), "foot is a terminal");
	check(agent_resume_is_terminal_app("Alacritty"), "Alacritty is a terminal");
	check(agent_resume_is_terminal_app("kitty"), "kitty is a terminal");
	check(agent_resume_is_terminal_app("com.mitchellh.ghostty"), "ghostty is a terminal");
	check(agent_resume_is_terminal_app("org.kde.konsole"), "konsole is a terminal");
	check(agent_resume_is_terminal_app("org.wezfurlong.wezterm"), "wezterm is a terminal");
	check(agent_resume_is_terminal_app("org.gnome.Console"), "GNOME Console is a terminal");
	check(agent_resume_is_terminal_app("st"), "st exact match is a terminal");
	check(!agent_resume_is_terminal_app("code"), "VS Code is not a terminal window");
	check(!agent_resume_is_terminal_app("firefox"), "Firefox is not a terminal window");
	check(!agent_resume_is_terminal_app("steam"), "Steam does not match st");
	check(!agent_resume_is_terminal_app("chromium"), "Chromium is not a terminal window");
	check(!agent_resume_is_terminal_app(NULL), "NULL app_id is not a terminal");
}

static void test_interactive_cmdline(void) {
	char buf[512];
	size_t len;

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "claude", "--continue", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_CLAUDE,
		"claude --continue is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "/usr/bin/node",
		"/usr/lib/node_modules/@anthropic-ai/claude-code/cli.js", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_CLAUDE,
		"node @anthropic-ai/claude-code is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "claude", "-p", "hello", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"claude -p is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "claude", "--print", "hi", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"claude --print is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "codex", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_CODEX,
		"codex is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "codex", "resume", "abc", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_CODEX,
		"codex resume is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "codex", "app-server",
		"--listen", "stdio://", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"codex app-server is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "codex", "exec", "test", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"codex exec is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "agy", "--continue", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_ANTIGRAVITY,
		"agy --continue is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "agy",
		"--dangerously-skip-permissions", NULL });
	check(agent_resume_interactive_agent(buf, len) == TW_AGENT_ANTIGRAVITY,
		"agy --dangerously-skip-permissions is interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "agy", "--settings", "{}", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"agy --settings probe is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "agy", "-p", "prompt", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"agy -p is non-interactive");

	len = make_cmdline(buf, sizeof(buf), (const char *[]){ "fish", NULL });
	check(agent_resume_interactive_agent(buf, len) == -1,
		"fish is not an agent");
}

static void test_agy_bucket_matching(void) {
	check(agent_resume_agy_bucket_matches_model("gemini-5h · 5h", "gemini-2.5-pro"),
		"gemini bucket matches gemini model");
	check(!agent_resume_agy_bucket_matches_model("gemini-5h · 5h", "claude-sonnet-4"),
		"gemini bucket does not match claude model");
	check(agent_resume_agy_bucket_matches_model("3p-5h · 5h", "claude-sonnet-4"),
		"3p bucket matches claude model");
	check(!agent_resume_agy_bucket_matches_model("3p-5h · 5h", "gemini-2.5-pro"),
		"3p bucket does not match gemini model");
	check(agent_resume_agy_bucket_matches_model("gemini-argon-wtu-weekly · weekly", "gemini-argon-1"),
		"argon bucket matches argon model");
	check(!agent_resume_agy_bucket_matches_model("gemini-5h · 5h", "gemini-argon-1"),
		"regular gemini bucket does not match argon model");
	check(agent_resume_agy_bucket_matches_model("3p-5h · 5h", NULL),
		"unknown worker model matches any bucket");
}

static void test_ascii_keys(void) {
	uint32_t code = 0;
	bool shift = false;
	check(agent_resume_ascii_key('c', &code, &shift) && code == KEY_C && !shift,
		"lowercase 'c' maps to KEY_C without shift");
	check(agent_resume_ascii_key('C', &code, &shift) && code == KEY_C && shift,
		"uppercase 'C' maps to KEY_C with shift");
	check(agent_resume_ascii_key('/', &code, &shift) && code == KEY_SLASH && !shift,
		"'/' maps to KEY_SLASH without shift");
	check(agent_resume_ascii_key('?', &code, &shift) && code == KEY_SLASH && shift,
		"'?' maps to KEY_SLASH with shift");
	check(agent_resume_ascii_key(' ', &code, &shift) && code == KEY_SPACE && !shift,
		"' ' maps to KEY_SPACE without shift");
	check(!agent_resume_ascii_key('\n', &code, &shift),
		"newline is not mapped as a printable key");
}

static void test_state_machine(void) {
	struct panel panel = { 0 };
	panel.state.windows = create_list();
	agent_resume_init(&panel);

	struct agent_resume_config cfg = {
		.enabled = true,
		.delay_s = 30,
		.threshold = 95,
		.command = "continue",
	};
	agent_resume_configure(&panel, &cfg);

	time_t t0 = 1700000000;
	struct agent_resume_status st;

	// Below threshold: must NOT arm
	struct agent_resume_window_input low = {
		.agent = TW_AGENT_CLAUDE,
		.account_id = "claude:~/.claude",
		.account_dir = "/tmp/claude",
		.bucket = "5h",
		.window = { .known = true, .used = 80, .resets_at = t0 + 600 },
	};
	agent_resume_observe(&panel, &low, NULL, t0);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(st.enabled && !st.scheduled, "80% usage below 95% threshold does not arm");

	// Reaches critical (>= 95%): arms for resets_at + delay
	low.window.used = 98;
	agent_resume_observe(&panel, &low, NULL, t0 + 60);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(st.scheduled && st.fire_at == t0 + 630,
		"98% usage arms for resets_at + 30s");

	// Weekly blocked: status reports not scheduled for 5h when weekly is 100%
	low.weekly_blocked = true;
	agent_resume_observe(&panel, &low, NULL, t0 + 120);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(!st.scheduled, "weekly-blocked 5h window is not scheduled");

	// Unblock weekly; then a fresh post-reset poll arrives during the 30s delay window
	low.weekly_blocked = false;
	agent_resume_observe(&panel, &low, NULL, t0 + 500);
	struct agent_resume_window_input next_win = low;
	next_win.window.used = 0;
	next_win.window.resets_at = t0 + 600 + 18000;
	agent_resume_observe(&panel, &next_win, NULL, t0 + 610);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(st.scheduled && st.fire_at == t0 + 630,
		"fresh post-reset window during delay countdown preserves pending fire_at");

	// While locked at fire_at: deferred until unlock
	panel.state.locked = true;
	agent_resume_observe(&panel, &next_win, NULL, t0 + 635);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(st.deferred && st.scheduled, "due resume defers while screen is locked");

	// Unlock clears deferred state and consumes the pending slot
	panel.state.locked = false;
	agent_resume_observe(&panel, &next_win, NULL, t0 + 640);
	agent_resume_account_status(&panel, TW_AGENT_CLAUDE, "claude:~/.claude", "/tmp/claude",
		NULL, &st);
	check(!st.deferred && !st.scheduled, "after unlock pending resume fires and clears");

	// Already-expired stale window at startup never arms
	struct agent_resume_window_input stale = {
		.agent = TW_AGENT_CODEX,
		.account_id = "codex:~/.codex",
		.account_dir = "/tmp/codex",
		.bucket = "5h",
		.window = { .known = true, .used = 100, .resets_at = t0 - 10 },
	};
	agent_resume_observe(&panel, &stale, NULL, t0 + 640);
	agent_resume_account_status(&panel, TW_AGENT_CODEX, "codex:~/.codex", "/tmp/codex",
		NULL, &st);
	check(!st.scheduled && !st.deferred, "stale already-expired window does not fire");

	// Turning auto_resume off disarms pending slots
	struct agent_resume_window_input armed2 = {
		.agent = TW_AGENT_ANTIGRAVITY,
		.account_id = "antigravity",
		.bucket = "gemini-5h · 5h",
		.window = { .known = true, .used = 100, .resets_at = t0 + 1200 },
	};
	agent_resume_observe(&panel, &armed2, NULL, t0 + 640);
	agent_resume_account_status(&panel, TW_AGENT_ANTIGRAVITY, "antigravity", NULL, NULL, &st);
	check(st.scheduled && st.fire_at == t0 + 1230, "Antigravity bucket arms on 100%");
	cfg.enabled = false;
	agent_resume_configure(&panel, &cfg);
	agent_resume_account_status(&panel, TW_AGENT_ANTIGRAVITY, "antigravity", NULL, NULL, &st);
	check(!st.enabled && !st.scheduled, "disabling auto_resume clears armed windows");

	agent_resume_fini();
	list_free(panel.state.windows);
}

static char *write_temp_file(const char *dir, const char *name, const char *content) {
	char *path = g_build_filename(dir, name, NULL);
	g_file_set_contents(path, content, -1, NULL);
	return path;
}

static void test_session_limit_logs(void) {
	char tmpl[] = "/tmp/tilewin-agent-resume-test-XXXXXX";
	char *dir = mkdtemp(tmpl);
	check(dir != NULL, "created temp dir for session log tests");
	if (!dir) {
		return;
	}

	// Claude: hit 5-hour limit
	char *p = write_temp_file(dir, "claude-limited.jsonl",
		"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"fix bugs\"}}\n"
		"{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"error\":\"rate_limit\","
		"\"message\":{\"model\":\"<synthetic>\",\"content\":[{\"type\":\"text\","
		"\"text\":\"You've hit your 5-hour limit · resets 2:00pm\"}]}}\n");
	check(agent_resume_claude_log_hit_limit(p), "claude log ending in rate_limit is detected");
	unlink(p);
	g_free(p);

	// Claude: idle after normal response
	p = write_temp_file(dir, "claude-idle.jsonl",
		"{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"error\":\"rate_limit\","
		"\"message\":{\"model\":\"<synthetic>\",\"content\":\"You've hit your 5-hour limit\"}}\n"
		"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"continue\"}}\n"
		"{\"type\":\"assistant\",\"message\":{\"model\":\"claude-opus\",\"content\":[{\"type\":\"text\",\"text\":\"All done!\"}]}}\n");
	check(!agent_resume_claude_log_hit_limit(p), "claude idle log after normal response is skipped");
	unlink(p);
	g_free(p);

	// Claude: already auto-resumed by Claude itself
	p = write_temp_file(dir, "claude-resumed.jsonl",
		"{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"error\":\"rate_limit\",\"message\":{\"content\":\"You've hit your 5-hour limit\"}}\n"
		"{\"type\":\"system\",\"subtype\":\"informational\",\"content\":\"Usage limit reached · continuing automatically at 2:00pm\"}\n"
		"{\"type\":\"system\",\"subtype\":\"informational\",\"content\":\"Usage limit reset · Continuing automatically\"}\n");
	check(!agent_resume_claude_log_hit_limit(p), "claude log that already auto-resumed is skipped");
	unlink(p);
	g_free(p);

	// Claude: unrelated server_error is NOT a usage limit
	p = write_temp_file(dir, "claude-neterr.jsonl",
		"{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"error\":\"server_error\","
		"\"message\":{\"model\":\"<synthetic>\",\"content\":[{\"type\":\"text\",\"text\":\"API Error: Connection lost\"}]}}\n");
	check(!agent_resume_claude_log_hit_limit(p), "claude server_error is not treated as usage limit");
	unlink(p);
	g_free(p);

	// Codex: task_complete with usage_limit_exceeded
	p = write_temp_file(dir, "codex-limited.jsonl",
		"{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\"}}\n"
		"{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"last_agent_message\":null,"
		"\"error\":{\"message\":\"You've hit your usage limit.\",\"codex_error_info\":\"usage_limit_exceeded\"}}}\n");
	check(agent_resume_codex_log_hit_limit(p), "codex rollout ending in usage_limit_exceeded is detected");
	unlink(p);
	g_free(p);

	// Codex: idle after normal task_complete
	p = write_temp_file(dir, "codex-idle.jsonl",
		"{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"error\":{\"codex_error_info\":\"usage_limit_exceeded\"}}}\n"
		"{\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\"}}\n"
		"{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"last_agent_message\":\"Done.\"}}\n");
	check(!agent_resume_codex_log_hit_limit(p), "codex idle rollout after normal completion is skipped");
	unlink(p);
	g_free(p);

	// AGY: cli log with ResourceExhausted Individual quota reached
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char agy_line[512];
	snprintf(agy_line, sizeof(agy_line),
		"I%02d%02d %02d:%02d:%02d.000000 100 input_loop.go:104] HandleUserInput called with text: \"go\"\n"
		"E%02d%02d %02d:%02d:%02d.100000 100 errorreport.go:282] agent executor error: transport error "
		"(ResourceExhausted, HTTP 429): Individual quota reached. Resets in 1h.: RESOURCE_EXHAUSTED (code 429)\n",
		tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	char *cli_log = write_temp_file(dir, "cli-test.log", agy_line);
	check(agent_resume_agy_session_hit_limit(cli_log, NULL, "gemini-5h · 5h"),
		"agy cli log ending in ResourceExhausted quota reached is detected");

	// AGY: newer PLANNER_RESPONSE in transcript means session already recovered
	time_t later = now + 120;
	struct tm gmt;
	gmtime_r(&later, &gmt);
	char tr_line[256];
	snprintf(tr_line, sizeof(tr_line),
		"{\"type\":\"PLANNER_RESPONSE\",\"status\":\"DONE\",\"created_at\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"}\n",
		gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
	char *tr = write_temp_file(dir, "transcript.jsonl", tr_line);
	check(!agent_resume_agy_session_hit_limit(cli_log, tr, "gemini-5h · 5h"),
		"agy session with newer PLANNER_RESPONSE after quota error is skipped");
	unlink(cli_log);
	unlink(tr);
	g_free(cli_log);
	g_free(tr);

	// AGY: Third-party quota reached matches 3p bucket, not gemini bucket
	snprintf(agy_line, sizeof(agy_line),
		"E%02d%02d %02d:%02d:%02d.100000 100 errorreport.go:282] agent executor error: "
		"(ResourceExhausted, HTTP 429): Third-party quota reached.: RESOURCE_EXHAUSTED (code 429)\n",
		tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	cli_log = write_temp_file(dir, "cli-3p.log", agy_line);
	check(agent_resume_agy_session_hit_limit(cli_log, NULL, "3p-5h · 5h"),
		"agy third-party quota error matches 3p bucket");
	check(!agent_resume_agy_session_hit_limit(cli_log, NULL, "gemini-5h · 5h"),
		"agy third-party quota error does not match gemini bucket");
	unlink(cli_log);
	g_free(cli_log);

	// AGY: idle cli log with no quota error
	cli_log = write_temp_file(dir, "cli-idle.log",
		"I1126 19:00:00.000000 100 input_loop.go:104] HandleUserInput called with text: \"quota limit\"\n");
	check(!agent_resume_agy_session_hit_limit(cli_log, NULL, "gemini-5h · 5h"),
		"agy idle cli log mentioning quota in user text is not a false positive");
	unlink(cli_log);
	g_free(cli_log);

	rmdir(dir);
}

int main(void) {
	test_terminal_apps();
	test_interactive_cmdline();
	test_agy_bucket_matching();
	test_ascii_keys();
	test_state_machine();
	test_session_limit_logs();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
