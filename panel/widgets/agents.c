/*
 * The AI agent usage widget: the usage limits of the accounts of Claude Code,
 * Codex, GitHub Copilot CLI and Antigravity, the 5-hour and the weekly
 * window (Copilot: its allowance), on the taskbar and on the desktop.
 *
 *   widget agents {
 *       accounts claude:~/.claude codex:~/.codex   # which, in this order
 *                                                  # (tw_agent_accounts.h); all
 *                                                  # found by default
 *       show usage_5h reset_5h usage_7d            # what of each, in this order
 *       account_checks no                          # ask the accounts themselves
 *   }
 *
 * It replaces the claude and the codex widget of older configs: their names in a
 * layout or on the desktop make this widget, once (config.c, deskwidgets.c), and
 * the settings take them out of taskbar.conf (widgets_replace_old).
 *
 * Claude Code shows its limits to tileWin through tilewin-claude-usage, its
 * status line once the folder is connected: tileWin never touches a login of
 * Claude's. A folder that is not connected says so, and a click connects it.
 * Codex writes them into its session logs itself (tw_codex_usage.h). Copilot
 * and Antigravity, and what runs on this computer, come from agentusage.c, in
 * a thread of its own once a minute; Copilot's allowance only with
 * account_checks, which has each agent's own program asked about its account.
 *
 * What it shows of each account:
 *   usage   the share of the window used, with a bar
 *   reset   the time until the window starts over
 *   left    the time until it is used up at the pace since it started (or since
 *           it was reset by hand), or that it lasts until it resets
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "agentusage.h"
#include "agentoverlay.h"
#include "agentresume.h"
#include "draw.h"
#include "panel.h"
#include "claudecharts.h"
#include "tw_agent_accounts.h"
#include "tw_claude_usage.h"
#include "tw_codex_usage.h"
#include "tw_theme.h"

#define RELOAD_MS 5000
#define COLLECT_S 60      // what runs here, and Copilot and Antigravity: once a minute
#define HOTSPOT_ACCOUNT 1 // the part of a desktop card of one account (its id the account)
#define COLLECT_OPEN_S 15 // while the flyout is open, sooner
#define MAX_ITEMS 12
#define BAR_W 34
#define GAP 12
#define ACCOUNT_GAP 18

static const char *const short_names[TW_AGENT_COUNT] = {
	"Claude", "Codex", "Copilot", "Antigravity",
};

struct source {
	struct tw_agent_account account;
	const struct tw_usage_tool *tool; // Claude Code and Codex
	struct tw_claude_usage usage;
	bool have; // values known
	// from what runs on this computer (agentusage.c)
	int running;
	char models[160];
	bool elsewhere;
	double elsewhere_share;
	time_t elsewhere_at;
	char allowance[48];
	const char *why; // why there are no values, when known
	// Antigravity: the readings of its group of models used most (its 5-hour and its weekly
	// limit), for the charts (it has no history file), and their windows as charts draw them
	struct tw_claude_point *points;
	int point_count;
	struct tw_claude_usage chart_usage; // (only windows: nothing in it to free)
	// Antigravity: the group of models it shows ("gemini", "3p"), NULL for all of them,
	// and its names ("Antigravity Gemini", "AG Gemini")
	char *group;
	char name[64], short_name[32];
};

struct agents_state {
	const struct tw_usage_tool *legacy; // the claude or codex widget of an older config
	struct source *src;
	int count, current;
	char *accounts; // the accounts the sources were made for
	struct loop_timer *timer;
	GAsyncQueue *pending;
	struct agent_usage *collected;
	time_t collected_at;
	char *found;      // the ids of all accounts found, when none are named
	time_t found_at;  // looked for once a minute: it reads the running programs
	bool alone;       // the flyout of one account alone: opened from its part of a desktop card
};

/* ---------- the groups of Antigravity's models ---------- */

/*
 * Antigravity has a limit for each group of its models: Gemini and Claude/GPT
 * a 5-hour and a weekly one each, Argon a weekly one. Its readings name them by
 * bucket, "gemini-5h", "3p-weekly", "gemini-argon-wtu-weekly": the group is
 * what comes before the last dash. Each group the readings have is a source of
 * its own, with its values and its charts.
 */
static const struct {
	const char *group, *name, *short_name;
} agy_groups[] = {
	{ "gemini", "Gemini", "Gemini" },
	{ "3p", "Claude/GPT", "C/GPT" },
	{ "gemini-argon-wtu", "Argon", "Argon" },
};

/* The group of a limit of Antigravity, "gemini" of "gemini-weekly · weekly". */
static void limit_group(const char *name, char *out, size_t size) {
	const char *dot = strstr(name, " · ");
	size_t len = dot ? (size_t)(dot - name) : strlen(name);
	const char *dash = NULL;
	for (const char *p = name; p < name + len; p++) {
		if (*p == '-') {
			dash = p;
		}
	}
	if (dash) {
		len = (size_t)(dash - name);
	}
	snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), name);
}

/* The groups the readings of Antigravity have, in the order above: "gemini,3p" (to free). */
static char *agy_groups_known(const struct agent_usage *u) {
	GString *out = g_string_new(NULL);
	char found[16][48];
	int n = 0;
	for (guint i = 0; u && i < u->limits->len && n < 16; i++) {
		const struct agent_limit *l = u->limits->pdata[i];
		if (l->kind != AGENT_AGY || !l->name ||
				!(l->usage.five_hour.known || l->usage.seven_day.known)) {
			continue;
		}
		char group[48];
		limit_group(l->name, group, sizeof(group));
		bool seen = false;
		for (int k = 0; k < n && !seen; k++) {
			seen = strcmp(found[k], group) == 0;
		}
		if (!seen) {
			snprintf(found[n++], sizeof(found[0]), "%s", group);
		}
	}
	// the known ones first, in their order, then the others as they came
	for (size_t g = 0; g <= G_N_ELEMENTS(agy_groups); g++) {
		for (int k = 0; k < n; k++) {
			bool known = false;
			for (size_t j = 0; j < G_N_ELEMENTS(agy_groups) && !known; j++) {
				known = strcmp(found[k], agy_groups[j].group) == 0;
			}
			if (g < G_N_ELEMENTS(agy_groups) ? strcmp(found[k], agy_groups[g].group) == 0 :
					!known) {
				g_string_append_printf(out, "%s%s", out->len ? "," : "", found[k]);
			}
		}
	}
	return g_string_free(out, FALSE);
}

static void name_source(struct source *s) {
	const char *agent = tw_agent_names[s->account.agent];
	if (!s->group) {
		snprintf(s->name, sizeof(s->name), "%s", agent);
		snprintf(s->short_name, sizeof(s->short_name), "%s", short_names[s->account.agent]);
		return;
	}
	const char *name = s->group, *brief = s->group;
	for (size_t j = 0; j < G_N_ELEMENTS(agy_groups); j++) {
		if (strcmp(agy_groups[j].group, s->group) == 0) {
			name = agy_groups[j].name;
			brief = agy_groups[j].short_name;
		}
	}
	snprintf(s->name, sizeof(s->name), "%s %s", agent, name);
	snprintf(s->short_name, sizeof(s->short_name), "AG %s", brief);
}

/* ---------- which accounts ---------- */

static const char *accounts_conf(struct widget *w) {
	if (w->desk) {
		struct twconf_node *node = twconf_child(w->desk, "accounts");
		if (node && node->argc > 0) {
			static char joined[1024];
			joined[0] = '\0';
			for (int i = 0; i < node->argc; i++) {
				strncat(joined, i ? " " : "", sizeof(joined) - strlen(joined) - 1);
				strncat(joined, node->argv[i], sizeof(joined) - strlen(joined) - 1);
			}
			return joined;
		}
	}
	return widget_conf(w, "accounts", NULL);
}

/* The ids of the accounts to show, space separated (to free). */
static char *wanted_accounts(struct widget *w) {
	struct agents_state *st = w->data;
	const char *conf = accounts_conf(w);
	if (conf && *conf) {
		return strdup(conf);
	}
	char id[300];
	if (st->legacy) {
		// the claude or codex widget: the account of its "account" option
		char *dir = tw_usage_select_dir(st->legacy, widget_conf(w, "account", NULL));
		tw_agent_account_id(st->legacy == &tw_codex_tool ? TW_AGENT_CODEX : TW_AGENT_CLAUDE,
			dir, id, sizeof(id));
		free(dir);
		return strdup(id);
	}
	time_t now = time(NULL);
	if (!st->found || now < st->found_at || now - st->found_at >= COLLECT_S) {
		struct tw_agent_account *found;
		int n = tw_agent_accounts_found(&found);
		GString *all = g_string_new(NULL);
		for (int i = 0; i < n; i++) {
			g_string_append_printf(all, "%s%s", all->len ? " " : "", found[i].id);
		}
		tw_agent_accounts_free(found, n);
		if (!all->len) {
			g_string_append(all, "claude"); // none at all: the one to connect
		}
		free(st->found);
		st->found = g_string_free(all, FALSE);
		st->found_at = now;
	}
	return strdup(st->found);
}

static void free_sources(struct agents_state *st) {
	for (int i = 0; i < st->count; i++) {
		tw_agent_account_finish(&st->src[i].account);
		tw_claude_usage_finish(&st->src[i].usage);
		free(st->src[i].points);
		free(st->src[i].group);
	}
	free(st->src);
	st->src = NULL;
	st->count = 0;
}

/* The sources, made again when the accounts to show changed. */
static void resolve(struct widget *w) {
	struct agents_state *st = w->data;
	char *wanted = wanted_accounts(w);
	// (Antigravity: a source for each group of models its readings have)
	char *groups = strstr(wanted, "antigravity") ? agy_groups_known(st->collected) : NULL;
	if (groups && *groups) {
		char *both = g_strdup_printf("%s|%s", wanted, groups);
		free(wanted);
		wanted = strdup(both);
		g_free(both);
	}
	if (st->accounts && strcmp(st->accounts, wanted) == 0) {
		g_free(groups);
		free(wanted);
		return;
	}
	free_sources(st);
	free(st->accounts);
	st->accounts = wanted;
	char *copy = strdup(wanted), *save = NULL;
	char *bar = strchr(copy, '|');
	if (bar) {
		*bar = '\0'; // (the groups after it)
	}
	for (char *id = strtok_r(copy, " ,\t", &save); id; id = strtok_r(NULL, " ,\t", &save)) {
		struct tw_agent_account account;
		if (!tw_agent_account_parse(id, &account)) {
			continue;
		}
		bool twice = false;
		for (int i = 0; i < st->count && !twice; i++) {
			twice = strcmp(st->src[i].account.id, account.id) == 0;
		}
		struct source *grown = twice ? NULL : realloc(st->src, (st->count + 1) * sizeof(*grown));
		if (!grown) {
			tw_agent_account_finish(&account);
			continue;
		}
		st->src = grown;
		struct source *s = &st->src[st->count++];
		*s = (struct source){ .account = account };
		s->tool = account.agent == TW_AGENT_CLAUDE ? &tw_claude_tool :
			account.agent == TW_AGENT_CODEX ? &tw_codex_tool : NULL;
		char **each = account.agent == TW_AGENT_ANTIGRAVITY && groups && *groups ?
			g_strsplit(groups, ",", -1) : NULL;
		for (int g = 0; each && each[g]; g++) {
			if (g > 0) {
				// the next group: a source of its own for the same account
				struct tw_agent_account again;
				grown = realloc(st->src, (st->count + 1) * sizeof(*grown));
				if (!grown || !tw_agent_account_parse(account.id, &again)) {
					break;
				}
				st->src = grown;
				s = &st->src[st->count++];
				*s = (struct source){ .account = again };
			}
			s->group = strdup(each[g]);
			name_source(s);
		}
		g_strfreev(each);
		name_source(s);
	}
	free(copy);
	g_free(groups);
	if (st->current >= st->count) {
		st->current = 0;
	}
}

/* ---------- the values ---------- */

static int point_by_time(const void *a, const void *b) {
	const struct tw_claude_point *pa = a, *pb = b;
	return (pa->at > pb->at) - (pa->at < pb->at);
}

static void from_collected(struct agents_state *st, struct source *s) {
	struct agent_usage *u = st->collected;
	s->running = 0;
	s->models[0] = '\0';
	s->elsewhere = false;
	if (!u) {
		return;
	}
	int kind = (int)s->account.agent; // enum agent_kind has the same order
	for (guint i = 0; i < u->workers->len; i++) {
		const struct agent_worker *worker = u->workers->pdata[i];
		if ((int)worker->kind != kind) {
			continue;
		}
		s->running++;
		char what[96] = "";
		if (worker->model && *worker->model) {
			snprintf(what, sizeof(what), "%s%s%s", worker->model,
				worker->effort && *worker->effort ? " " : "",
				worker->effort && *worker->effort ? worker->effort : "");
		}
		if (*what && !strstr(s->models, what)) {
			size_t len = strlen(s->models);
			snprintf(s->models + len, sizeof(s->models) - len, "%s%s", len ? ", " : "", what);
		}
	}
	time_t now = time(NULL);
	struct tw_claude_usage agy = { 0 };
	const struct agent_limit *fullest = NULL, *fullest_five = NULL; // Antigravity: of its charts
	for (guint i = 0; i < u->limits->len; i++) {
		const struct agent_limit *l = u->limits->pdata[i];
		if ((int)l->kind != kind) {
			continue;
		}
		if (s->group && l->name) {
			char group[48];
			limit_group(l->name, group, sizeof(group));
			if (strcmp(group, s->group) != 0) {
				continue; // another group of Antigravity's models
			}
		}
		if (l->unattributed_at && now - l->unattributed_at < TW_CLAUDE_FIVE_HOURS &&
				(!s->account.dir || !l->usage.dir || strcmp(l->usage.dir, s->account.dir) == 0)) {
			s->elsewhere = true;
			s->elsewhere_share = fmax(s->elsewhere_share, l->unattributed);
			s->elsewhere_at = l->unattributed_at;
		}
		if (s->account.agent == TW_AGENT_ANTIGRAVITY) {
			// a limit for each group of models: the one used most of each window
			if (l->usage.five_hour.known && (!agy.five_hour.known ||
					l->usage.five_hour.used > agy.five_hour.used)) {
				agy.five_hour = l->usage.five_hour;
			}
			if (l->usage.seven_day.known && (!agy.seven_day.known ||
					l->usage.seven_day.used > agy.seven_day.used)) {
				agy.seven_day = l->usage.seven_day;
			}
			agy.updated = MAX(agy.updated, l->usage.updated);
			// the weekly limit used most (of those used alike, the one read most)
			if (l->chart.count > 0 && l->usage.seven_day.known && (!fullest ||
					l->usage.seven_day.used > fullest->usage.seven_day.used ||
					(l->usage.seven_day.used == fullest->usage.seven_day.used &&
					l->chart.count > fullest->chart.count))) {
				fullest = l;
			}
		}
	}
	if (s->account.agent == TW_AGENT_ANTIGRAVITY) {
		tw_claude_usage_finish(&s->usage);
		s->usage = agy;
		s->have = agy.updated && (agy.five_hour.known || agy.seven_day.known);
		free(s->points);
		s->points = NULL;
		s->point_count = 0;
		s->chart_usage = (struct tw_claude_usage){ 0 };
		if (fullest) {
			// the 5-hour limit of the same group of models: "gemini-5h" of "gemini-weekly"
			char fullest_group[48];
			limit_group(fullest->name, fullest_group, sizeof(fullest_group));
			for (guint i = 0; i < u->limits->len && !fullest_five; i++) {
				const struct agent_limit *l = u->limits->pdata[i];
				char its[48];
				if (l->name) {
					limit_group(l->name, its, sizeof(its));
				}
				if ((int)l->kind == kind && l->usage.five_hour.known && l->name &&
						strcmp(its, fullest_group) == 0) {
					fullest_five = l;
				}
			}
			int n = fullest->chart.count + (fullest_five ? fullest_five->chart.count : 0);
			s->points = malloc(n * sizeof(*s->points));
			if (s->points) {
				memcpy(s->points, fullest->chart.points, fullest->chart.count * sizeof(*s->points));
				if (fullest_five) {
					memcpy(s->points + fullest->chart.count, fullest_five->chart.points,
						fullest_five->chart.count * sizeof(*s->points));
				}
				qsort(s->points, n, sizeof(*s->points), point_by_time);
				s->point_count = n;
			}
			// the windows the charts draw: those of that group, one with nothing used yet
			// from now to the reset it tells
			s->chart_usage.updated = agy.updated;
			s->chart_usage.seven_day = fullest->usage.seven_day;
			if (!s->chart_usage.seven_day.resets_at) {
				s->chart_usage.seven_day.resets_at = fullest->idle_resets_at;
			}
			if (fullest_five) {
				s->chart_usage.five_hour = fullest_five->usage.five_hour;
				if (!s->chart_usage.five_hour.resets_at) {
					s->chart_usage.five_hour.resets_at = fullest_five->idle_resets_at;
				}
			}
		}
	} else if (s->account.agent == TW_AGENT_COPILOT) {
		tw_claude_usage_finish(&s->usage);
		s->usage = (struct tw_claude_usage){ .five_hour = u->copilot_billing,
			.updated = u->copilot_billing.known ? u->now : 0 };
		s->have = u->copilot_billing.known;
		snprintf(s->allowance, sizeof(s->allowance), "%s",
			u->copilot_bucket ? u->copilot_bucket : "Allowance");
		s->why = u->copilot_cli_missing ? "Copilot CLI is not installed: run copilot once" :
			u->copilot_auth_required ? "Not signed in: run copilot and /login" : NULL;
	}
}

static void reload_source(struct agents_state *st, struct source *s) {
	if (s->tool) {
		tw_claude_usage_finish(&s->usage);
		if (s->tool->backfill) {
			s->tool->backfill(s->account.dir, time(NULL)); // once: what it kept before tileWin
		}
		s->have = s->tool->read(s->account.dir, &s->usage);
		if (s->have && s->usage.updated) {
			// into the history too (only what differs from its last line goes in)
			tw_claude_history_add(s->usage.history, &s->usage, s->usage.updated);
		}
		if (!s->usage.dir) {
			s->usage.dir = strdup(s->account.dir);
		}
	}
	from_collected(st, s);
}

static gpointer collect_thread(gpointer data) {
	GAsyncQueue *queue = data;
	bool checks = GPOINTER_TO_INT(g_async_queue_pop(queue)) == 2;
	g_async_queue_push(queue, agent_usage_collect(checks));
	g_async_queue_unref(queue);
	return NULL;
}

/* What runs here and the accounts that only agentusage.c reads: in a thread of its own. */
static void collect(struct widget *w, int every) {
	struct agents_state *st = w->data;
	if (st->pending) {
		struct agent_usage *got = g_async_queue_try_pop(st->pending);
		if (!got) {
			return; // still at it
		}
		agent_usage_free(st->collected);
		st->collected = got;
		g_async_queue_unref(st->pending);
		st->pending = NULL;
		widget_set_dirty(w);
	}
	time_t now = time(NULL);
	if (st->collected_at && now >= st->collected_at && now - st->collected_at < every) {
		return;
	}
	st->collected_at = now;
	GAsyncQueue *queue = g_async_queue_new();
	g_async_queue_push(queue, GINT_TO_POINTER(widget_conf_bool(w, "account_checks", false) ? 2 : 1));
	GThread *thread = g_thread_try_new("agent-usage", collect_thread, g_async_queue_ref(queue),
		NULL);
	if (thread) {
		g_thread_unref(thread);
		st->pending = queue;
	} else {
		g_async_queue_unref(queue);
		g_async_queue_unref(queue);
	}
}

static bool overlay_style(const struct widget *w) {
	const char *style = w->desk ? twconf_value(w->desk, "style") : NULL;
	// "together", the chart of all agents before, is drawn so too
	return style && (strcmp(style, "overlay") == 0 || strcmp(style, "together") == 0);
}

static const struct tw_claude_window *overlay_agy_gate(const GPtrArray *limits,
	const struct agent_limit *limit);

static void sync_auto_resume(struct widget *w) {
	struct agents_state *st = w->data;
	struct agent_resume_config cfg = {
		.enabled = widget_conf_bool(w, "auto_resume", false),
		.delay_s = widget_conf_int(w, "resume_delay", AGENT_RESUME_DEFAULT_DELAY),
		.threshold = widget_conf_int(w, "critical", 95),
		.command = widget_conf(w, "resume_command", AGENT_RESUME_DEFAULT_COMMAND),
	};
	agent_resume_configure(w->panel, &cfg);
	time_t now = time(NULL);
	bool agy_observed = false;
	for (int i = 0; i < st->count; i++) {
		const struct source *s = &st->src[i];
		enum tw_agent agent = s->account.agent;
		if (agent == TW_AGENT_CLAUDE || agent == TW_AGENT_CODEX) {
			if (!s->have) {
				continue;
			}
			bool weekly_blocked = s->usage.seven_day.known &&
				tw_claude_window_now(&s->usage.seven_day, now) >= 100;
			struct agent_resume_window_input five = {
				.agent = agent,
				.account_id = s->account.id,
				.account_dir = s->account.dir,
				.bucket = "5h",
				.seven_day = false,
				.window = s->usage.five_hour,
				.weekly_blocked = weekly_blocked,
			};
			agent_resume_observe(w->panel, &five, st->collected, now);
			struct agent_resume_window_input week = {
				.agent = agent,
				.account_id = s->account.id,
				.account_dir = s->account.dir,
				.bucket = "7d",
				.seven_day = true,
				.window = s->usage.seven_day,
				.weekly_blocked = false,
			};
			agent_resume_observe(w->panel, &week, st->collected, now);
		} else if (agent == TW_AGENT_ANTIGRAVITY && !agy_observed &&
				st->collected && st->collected->limits) {
			agy_observed = true;
			for (guint j = 0; j < st->collected->limits->len; j++) {
				const struct agent_limit *l = st->collected->limits->pdata[j];
				if (l->kind != AGENT_AGY) {
					continue;
				}
				if (l->usage.five_hour.known) {
					const struct tw_claude_window *gate =
						overlay_agy_gate(st->collected->limits, l);
					bool blocked = gate && tw_claude_window_now(gate, now) >= 100;
					struct agent_resume_window_input in = {
						.agent = TW_AGENT_ANTIGRAVITY,
						.account_id = s->account.id,
						.account_dir = NULL,
						.bucket = l->name,
						.seven_day = false,
						.window = l->usage.five_hour,
						.weekly_blocked = blocked,
					};
					agent_resume_observe(w->panel, &in, st->collected, now);
				}
				if (l->usage.seven_day.known) {
					struct agent_resume_window_input in = {
						.agent = TW_AGENT_ANTIGRAVITY,
						.account_id = s->account.id,
						.account_dir = NULL,
						.bucket = l->name,
						.seven_day = true,
						.window = l->usage.seven_day,
						.weekly_blocked = false,
					};
					agent_resume_observe(w->panel, &in, st->collected, now);
				}
			}
		}
	}
}

static void reload(struct widget *w) {
	struct agents_state *st = w->data;
	resolve(w);
	for (int i = 0; i < st->count; i++) {
		struct source *src = &st->src[i];
		reload_source(st, src);
		if (src->tool && overlay_style(w)) {
			free(src->points);
			src->point_count = tw_claude_history_read(src->usage.history, &src->points);
		}
	}
	sync_auto_resume(w);
}

static void tick(void *data) {
	struct widget *w = data;
	struct agents_state *st = w->data;
	st->timer = NULL;
	if (!w->active) {
		return;
	}
	collect(w, COLLECT_S);
	reload(w);
	widget_set_dirty(w); // the times count down also without new values
	st->timer = loop_add_timer_lazy(w->panel->loop, RELOAD_MS, tick, w);
}

/* ---------- what to show ---------- */

static int show_list(struct widget *w, const struct tw_claude_item **items) {
	const char *show = NULL;
	char joined[512] = "";
	if (w->desk) {
		// the desktop has its own list, more by default, apart from the taskbar's
		struct twconf_node *node = twconf_child(w->desk, "show");
		for (int i = 0; node && i < node->argc; i++) {
			strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
			strncat(joined, node->argv[i], sizeof(joined) - strlen(joined) - 1);
		}
		show = *joined ? joined : TW_CLAUDE_SHOW_DESKTOP;
	} else {
		show = widget_conf(w, "show", TW_CLAUDE_SHOW_TASKBAR);
	}
	char *copy = strdup(show), *save = NULL;
	int count = 0;
	for (char *id = strtok_r(copy, " ,\t", &save); id && count < MAX_ITEMS;
			id = strtok_r(NULL, " ,\t", &save)) {
		const struct tw_claude_item *item = tw_claude_item_find(id);
		if (item) {
			items[count++] = item;
		}
	}
	free(copy);
	return count;
}

/* The items of a source: Copilot has one allowance, not a week. */
static int source_items(struct widget *w, const struct source *s,
		const struct tw_claude_item **items) {
	int count = show_list(w, items);
	if (s->group && s->have && (!s->usage.five_hour.known || !s->usage.seven_day.known)) {
		// a group of Antigravity's models with one limit (Argon: a weekly one only)
		int kept = 0;
		for (int i = 0; i < count; i++) {
			if (items[i]->seven_day ? s->usage.seven_day.known : s->usage.five_hour.known) {
				items[kept++] = items[i];
			}
		}
		return kept > 0 ? kept : count;
	}
	if (s->account.agent != TW_AGENT_COPILOT) {
		return count;
	}
	// its allowance and when it resets: no week, and no pace of a window of a known length
	int kept = 0;
	for (int i = 0; i < count; i++) {
		if (!items[i]->seven_day && items[i]->kind != TW_CLAUDE_LEFT) {
			items[kept++] = items[i];
		}
	}
	return kept;
}

static const struct tw_claude_window *window_of(const struct source *s,
		const struct tw_claude_item *item, long *length) {
	*length = item->seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	return item->seven_day ? &s->usage.seven_day : &s->usage.five_hour;
}

/* The name of an item on the taskbar: Copilot's allowance has none of its own. */
static const char *item_label(const struct source *s, const struct tw_claude_item *item) {
	return s->account.agent == TW_AGENT_COPILOT ? "" : item->label;
}

/*
 * The value of an item as text, brief for the taskbar; the share for a bar,
 * -1 when it has none.
 */
static double item_value(const struct source *s, const struct tw_claude_item *item, bool brief,
		char *out, size_t size) {
	long length;
	const struct tw_claude_window *win = window_of(s, item, &length);
	time_t now = time(NULL);
	double used = s->have ? tw_claude_window_now(win, now) : -1;
	if (used < 0) {
		snprintf(out, size, "–");
		return item->kind == TW_CLAUDE_USAGE ? 0 : -1;
	}
	char span[48];
	switch (item->kind) {
	case TW_CLAUDE_USAGE:
		snprintf(out, size, "%.0f%%", used);
		return used;
	case TW_CLAUDE_RESET:
		if (win->resets_at <= now) {
			snprintf(out, size, brief ? "reset" : "has reset");
		} else {
			tw_claude_format_span(win->resets_at - now, brief, span, sizeof(span));
			snprintf(out, size, brief ? "↻ %s" : "in %s", span);
		}
		return -1;
	case TW_CLAUDE_LEFT: {
		bool known;
		long left = s->account.agent == TW_AGENT_COPILOT ? -1 :
			tw_claude_window_left(win, length, now, &known);
		if (left < 0) {
			snprintf(out, size, brief ? "lasts" : "until it resets");
		} else if (left == 0) {
			snprintf(out, size, "used up");
		} else {
			tw_claude_format_span(left, brief, span, sizeof(span));
			snprintf(out, size, brief ? "≈%s left" : "about %s", span);
		}
		return -1;
	}
	}
	return -1;
}

static uint32_t level_color(struct widget *w, double used, uint32_t normal) {
	if (used >= widget_conf_int(w, "critical", 95)) {
		return widget_conf_color(w, "critical_fg", 0xe04040ff);
	}
	if (used >= widget_conf_int(w, "warning", 80)) {
		return widget_conf_color(w, "warning_fg", 0xf0a030ff);
	}
	return normal;
}

/* ---------- options of each place ---------- */

/*
 * The look of each place has its own options: on the desktop only those of its
 * desktop_widgets entry count, so the taskbar's bar width or names do not
 * change the card, and the other way round.
 */
static const char *place_conf(struct widget *w, const char *key, const char *fallback) {
	if (w->desk) {
		struct twconf_node *node = twconf_child(w->desk, key);
		return node && node->argc > 0 ? node->argv[0] : fallback;
	}
	return widget_conf(w, key, fallback);
}

static bool place_bool(struct widget *w, const char *key, bool fallback) {
	const char *v = place_conf(w, key, NULL);
	if (!v) {
		return fallback;
	}
	return strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

static int place_int(struct widget *w, const char *key, int fallback, int low, int high) {
	const char *v = place_conf(w, key, NULL);
	int n = v ? atoi(v) : fallback;
	return n < low ? low : n > high ? high : n;
}

/* The color of the bars: bar_color, else the accent of the theme. */
static uint32_t bar_color(struct widget *w, uint32_t accent) {
	const char *v = place_conf(w, "bar_color", NULL);
	uint32_t color;
	if (v && tw_parse_color(v, &color)) {
		return color;
	}
	return accent;
}

static void fill_rgba(cairo_t *cr, uint32_t c, double alpha) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, alpha);
}

/* The long name of an item, as the desktop and the flyout say it. */
static const char *long_name(const struct source *s, const struct tw_claude_item *item,
		bool brief) {
	if (s->account.agent == TW_AGENT_COPILOT && item->kind == TW_CLAUDE_USAGE) {
		return s->allowance[0] ? s->allowance : "Allowance";
	}
	if (s->account.agent == TW_AGENT_COPILOT && item->kind == TW_CLAUDE_RESET) {
		return "Allowance resets";
	}
	if (brief) {
		return item->label;
	}
	switch (item->kind) {
	case TW_CLAUDE_USAGE:
		return item->seven_day ? "Weekly limit" : "5-hour limit";
	case TW_CLAUDE_RESET:
		return item->seven_day ? "Weekly limit resets" : "5-hour limit resets";
	case TW_CLAUDE_LEFT:
		return item->seven_day ? "Weekly limit lasts" : "5-hour limit lasts";
	}
	return item->label;
}

/* The name of a source in the flyout and the tooltip: "Claude Code · me@example.org". */
static void source_title(const struct source *s, char *out, size_t size) {
	const char *name = s->name;
	if (s->usage.account && *s->usage.account) {
		snprintf(out, size, "%s · %s", name, s->usage.account);
	} else if (s->account.dir && s->tool == &tw_claude_tool) {
		// a portable Claude Code by its folder, the installed one by its name alone
		char *installed = tw_claude_default_dir();
		bool own = installed && strcmp(installed, s->account.dir) == 0;
		free(installed);
		const char *home = getenv("HOME");
		size_t hl = home ? strlen(home) : 0;
		const char *dir = s->account.dir;
		if (own) {
			snprintf(out, size, "%s", name);
		} else if (hl && strncmp(dir, home, hl) == 0 && dir[hl] == '/') {
			snprintf(out, size, "%s · ~%s", name, dir + hl);
		} else {
			snprintf(out, size, "%s · %s", name, dir);
		}
	} else {
		snprintf(out, size, "%s", name);
	}
}

/* ---------- the taskbar ---------- */

static const char *not_connected_text = "Claude: click to connect";

static bool show_connect(const struct source *s) {
	return s->tool && s->tool->connects && !s->have && !s->usage.connected;
}

/* The taskbar look: bars yes|no, bar_width, labels short|none. */
static int taskbar_bar_width(struct widget *w) {
	return place_bool(w, "bars", true) ? place_int(w, "bar_width", BAR_W, 8, 200) : 0;
}

static bool taskbar_labels(struct widget *w) {
	const char *v = place_conf(w, "labels", "short");
	return strcmp(v, "none") != 0 && strcmp(v, "no") != 0;
}

/* Its name before each account, when there are several (or it is not Claude Code's alone). */
static bool named(const struct agents_state *st) {
	return st->count > 1 || (st->count == 1 && !st->legacy &&
		st->src[0].account.agent == TW_AGENT_COPILOT);
}

/* The width of one item: its name, a bar for a usage, its value. */
static int item_width(struct widget *w, struct render_ctx *ctx, const struct source *s,
		const struct tw_claude_item *item) {
	const char *font = bar_font(ctx->panel);
	char value[64];
	double share = item_value(s, item, true, value, sizeof(value));
	int width = render_text_width(ctx, font, value);
	const char *label = item_label(s, item);
	if (taskbar_labels(w) && *label) {
		width += render_text_width(ctx, font, label) + 4;
	}
	int bar = taskbar_bar_width(w);
	if (share >= 0 && bar > 0) {
		width += bar + 4;
	}
	return width;
}

static int source_width(struct widget *w, struct render_ctx *ctx, const struct source *s) {
	struct agents_state *st = w->data;
	const char *font = bar_font(ctx->panel);
	if (show_connect(s)) {
		return render_text_width(ctx, font, not_connected_text);
	}
	int width = named(st) ? render_text_width(ctx, bar_bold_font(ctx->panel),
		s->short_name) + 6 : 0;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = source_items(w, s, items);
	for (int i = 0; i < count; i++) {
		width += item_width(w, ctx, s, items[i]) + (i > 0 ? GAP : 0);
	}
	return width;
}

static int agents_measure(struct widget *w, struct render_ctx *ctx) {
	struct agents_state *st = w->data;
	int width = 16;
	for (int i = 0; i < st->count; i++) {
		width += source_width(w, ctx, &st->src[i]) + (i > 0 ? ACCOUNT_GAP : 0);
	}
	return st->count > 0 ? width : 0;
}

static void agents_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct agents_state *st = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	const char *font = bar_font(ctx->panel);
	const char *bold = bar_bold_font(ctx->panel);
	uint32_t fg = widget_fg(ctx->panel, w->impl->type);
	uint32_t dim = (fg & 0xffffff00) | 0xa0;
	// the color of the bars of the flyouts (navy in Windows 95), else the taskbar's accent
	uint32_t accent = bar_color(w, tw_theme_color(ctx->panel->theme, "flyout.accent",
		tw_theme_color(ctx->panel->theme, "taskbar.indicator", 0x0078d4ff)) | 0xff);
	int bar = taskbar_bar_width(w);
	bool blocks = ctx->style == PSV_CLASSIC || ctx->style == PSV_LUNA;
	int bar_h = place_int(w, "bar_height", blocks ? 10 : 6, 2, 24); // blocks need room
	bool labels = taskbar_labels(w);
	double x = b.x + 8, end = b.x + b.width - 8;
	for (int k = 0; k < st->count && x < end; k++) {
		const struct source *s = &st->src[k];
		double from = x;
		if (show_connect(s)) {
			int tw = render_text_width(ctx, font, not_connected_text);
			pd_text(ctx->cairo, font, not_connected_text, x, b.y, fmin(tw, end - x), b.height,
				dim, PD_LEFT);
			x += tw;
		} else {
			if (named(st)) {
				const char *name = s->short_name;
				int nw = render_text_width(ctx, bold, name);
				pd_text(ctx->cairo, bold, name, x, b.y, nw, b.height, fg, PD_LEFT);
				x += nw + 6;
			}
			const struct tw_claude_item *items[MAX_ITEMS];
			int count = source_items(w, s, items);
			for (int i = 0; i < count && x < end; i++) {
				char value[64];
				double share = item_value(s, items[i], true, value, sizeof(value));
				const char *label = item_label(s, items[i]);
				if (labels && *label) {
					int lw = render_text_width(ctx, font, label);
					pd_text(ctx->cairo, font, label, x, b.y, lw, b.height, dim, PD_LEFT);
					x += lw + 4;
				}
				if (share >= 0 && bar > 0) {
					enum pd_bar_look look = ctx->style == PSV_CLASSIC ? PD_BAR_BLOCKS_SUNKEN :
						ctx->style == PSV_LUNA ? PD_BAR_BLOCKS : PD_BAR_SMOOTH;
					pd_share_bar(ctx->cairo, x, b.y + (b.height - bar_h) / 2.0, bar, bar_h, share,
						fg, 0.22, level_color(w, share, accent), 1, look, false);
					x += bar + 4;
				}
				int vw = render_text_width(ctx, font, value);
				uint32_t color = share >= 0 ? level_color(w, share, fg) : fg;
				pd_text(ctx->cairo, font, value, x, b.y, end - x < vw ? end - x : vw, b.height,
					color, PD_LEFT);
				x += vw + (i + 1 < count ? GAP : 0);
			}
		}
		// each account its own place to click: its flyout opens at it
		double to = k + 1 < st->count ? x + ACCOUNT_GAP / 2.0 : b.x + b.width;
		double left = k == 0 ? b.x : from - ACCOUNT_GAP / 2.0;
		psurface_add_hotspot(ctx->surface, (int)left, b.y, (int)(to - left), b.height, w, 0, k,
			NULL);
		x += ACCOUNT_GAP;
	}
}

/* ---------- the desktop: list, bars, rings and charts ---------- */

/*
 * The desktop look, from its desktop_widgets entry: title yes|no (the account
 * at the top), updated yes|no (how old the values are), bars yes|no,
 * bar_height (pixels at a cell of 100), bar_color, text_size (pixels at a
 * cell of 100), labels long|short. Several accounts share the height, each
 * with its title.
 */
struct desk_look {
	char font[224], bold[224], small[224];
	double sc, pad, text, bar_h;
	bool title, updated, bars, brief;
	uint32_t fill;
	enum pd_bar_look look;
};

static void desk_fonts(struct desk_look *l, const struct gadget_palette *pal) {
	snprintf(l->font, sizeof(l->font), "%s %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->bold, sizeof(l->bold), "%s Bold %dpx", pal->family, (int)(l->text + 0.5));
	snprintf(l->small, sizeof(l->small), "%s %dpx", pal->family, (int)(l->text * 0.85 + 0.5));
}

/* The text smaller than this is not made, in pixels: it could not be read. */
#define DESK_TEXT_MIN 7.0

/*
 * Too little room for what is to be shown: its text and bars f times as big (the
 * margins stay), not below DESK_TEXT_MIN; returns the factor it took. The lines
 * keep the room their text needs, so they never run into each other.
 */
static double desk_shrink(struct desk_look *l, const struct gadget_palette *pal, double f) {
	if (f >= 1 || l->text <= DESK_TEXT_MIN) {
		return 1;
	}
	f = fmax(f, DESK_TEXT_MIN / l->text);
	l->text *= f;
	l->sc *= f;
	l->bar_h *= f;
	desk_fonts(l, pal);
	return f;
}

static void desk_look(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal,
		struct desk_look *l) {
	l->sc = ctx->scale;
	l->pad = 12 * l->sc;
	int size = place_int(w, "text_size", 13, 8, 40);
	l->text = size * l->sc;
	desk_fonts(l, pal);
	l->title = place_bool(w, "title", true);
	l->updated = place_bool(w, "updated", true);
	l->bars = place_bool(w, "bars", true);
	l->bar_h = place_int(w, "bar_height", 0, 0, 60) * l->sc; // 0: from the room there is
	l->brief = strcmp(place_conf(w, "labels", "long"), "short") == 0;
	l->fill = bar_color(w, pal->accent);
	l->look = pal->look == GADGET_CLASSIC ? PD_BAR_BLOCKS_SUNKEN :
		pal->look == GADGET_LUNA ? PD_BAR_BLOCKS : PD_BAR_SMOOTH;
}

/* The account at the top of its part; returns where the rest starts. */
static double desk_title(const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, double y) {
	if (!l->title) {
		return y;
	}
	char title[300];
	source_title(s, title, sizeof(title));
	pd_text(ctx->cairo, l->bold, title, l->pad, y, ctx->width - 2 * l->pad, l->text * 1.4,
		pal->fg, PD_LEFT);
	return y + l->text * 1.8;
}

/* "As of ... ago" at the bottom of its part; returns the room it takes. */
static double desk_foot(const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, double bottom) {
	if (!l->updated || !s->have || !s->usage.updated) {
		return 0;
	}
	double h = l->text * 1.3;
	char when[64];
	long ago = (long)(time(NULL) - s->usage.updated);
	if (ago < 90) {
		snprintf(when, sizeof(when), "As of just now");
	} else {
		char span[48];
		tw_claude_format_span(ago, false, span, sizeof(span));
		snprintf(when, sizeof(when), "As of %s ago", span);
	}
	pd_text(ctx->cairo, l->small, when, l->pad, bottom - h, ctx->width - 2 * l->pad, h, pal->dim,
		PD_LEFT);
	return h;
}

/* A line for each item, a bar under each usage. */
/* The height of a bar in the lines, at least (the blocks need room to be told apart). */
static double list_bar_h(const struct desk_look *l) {
	return l->bar_h > 0 ? l->bar_h : (l->look == PD_BAR_SMOOTH ? 5 : 13) * l->sc;
}

/* The height a line needs: its text, and its bar under it with room after. */
static double list_line_h(const struct desk_look *l, bool bar) {
	return bar ? l->text * 1.9 + list_bar_h(l) : l->text * 1.6;
}

/* The height the lines of an account need. */
static double list_height(struct widget *w, const struct source *s, const struct desk_look *l) {
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = source_items(w, s, items);
	double total = 0;
	for (int i = 0; i < count; i++) {
		char value[64];
		total += list_line_h(l, item_value(s, items[i], l->brief, value, sizeof(value)) >= 0 &&
			l->bars);
	}
	return total;
}

/*
 * A line for each item, those with a share with a bar under them; each as high
 * as it needs, the room between y and bottom shared out in that measure.
 */
static void desk_list(struct widget *w, const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *look, double y, double bottom) {
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = source_items(w, s, items);
	struct desk_look smaller = *look, *l = &smaller;
	double need = list_height(w, s, l);
	// too little room: smaller lines, not lines over each other
	for (int i = 0; i < 4 && need > bottom - y && desk_shrink(l, pal, (bottom - y) / need) < 1; i++) {
		need = list_height(w, s, l);
	}
	double k = need > 0 ? fmax((bottom - y) / need, 1) : 1;
	for (int i = 0; i < count; i++) {
		char value[64];
		double share = item_value(s, items[i], l->brief, value, sizeof(value));
		bool bar = share >= 0 && l->bars;
		double row = list_line_h(l, bar) * k;
		if (y + row > bottom + 0.5) {
			break; // no room for it even with the smallest text: left out, not drawn over
		}
		// a bar grows with the room, the text above it keeps most of the rest
		double bh = bar ? (l->bar_h > 0 ? l->bar_h : fmax(list_bar_h(l), row * 0.18)) : 0;
		double line = bar ? (row - bh) * 0.8 : row;
		pd_text(cr, l->font, long_name(s, items[i], l->brief), l->pad, y, ctx->width * 0.6, line,
			pal->dim, PD_LEFT);
		uint32_t color = share >= 0 ? level_color(w, share, pal->fg) : pal->fg;
		pd_text(cr, l->bold, value, l->pad, y, ctx->width - 2 * l->pad, line, color, PD_RIGHT);
		if (bar) {
			pd_share_bar(cr, l->pad, y + line, ctx->width - 2 * l->pad, bh, share, pal->track,
				(pal->track & 0xff) / 255.0, level_color(w, share, l->fill), 2 * l->sc, l->look,
				pal->dark);
		}
		y += row;
	}
}

/* The windows with a usage ticked, for the bars and the rings: how many, which. */
static int usage_windows(const struct tw_claude_item **items, int count, bool seven[2]) {
	int windows = 0;
	for (int i = 0; i < count && windows < 2; i++) {
		if (items[i]->kind == TW_CLAUDE_USAGE) {
			seven[windows++] = items[i]->seven_day;
		}
	}
	return windows;
}

/*
 * A big bar for each window with a usage ticked, its other ticked items (when
 * it resets, how long it lasts) in a line under it.
 */
static void desk_bars(struct widget *w, const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, double y, double bottom) {
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = source_items(w, s, items);
	bool seven[2] = { false, false };
	int windows = usage_windows(items, count, seven);
	if (windows == 0) {
		desk_list(w, s, ctx, pal, l, y, bottom); // nothing to make a bar of: the lines
		return;
	}
	double block = (bottom - y) / windows;
	for (int k = 0; k < windows; k++) {
		const struct tw_claude_item *usage = tw_claude_item_find(seven[k] ? "usage_7d" : "usage_5h");
		char value[64];
		double share = item_value(s, usage, false, value, sizeof(value));
		double head = l->text * 1.4;
		pd_text(cr, l->font, long_name(s, usage, l->brief), l->pad, y, ctx->width * 0.6, head,
			pal->dim, PD_LEFT);
		pd_text(cr, l->bold, value, l->pad, y, ctx->width - 2 * l->pad, head,
			level_color(w, share, pal->fg), PD_RIGHT);
		double bh = l->bar_h > 0 ? l->bar_h : fmax(block * 0.28, 8 * l->sc);
		if (l->bars) {
			pd_share_bar(cr, l->pad, y + head + 2 * l->sc, ctx->width - 2 * l->pad, bh, share,
				pal->track, (pal->track & 0xff) / 255.0, level_color(w, share, l->fill),
				3 * l->sc, l->look, pal->dark);
		}
		// the window's other items, after one another
		GString *more = g_string_new(NULL);
		for (int i = 0; i < count; i++) {
			if (items[i]->seven_day != seven[k] || items[i]->kind == TW_CLAUDE_USAGE) {
				continue;
			}
			char v[64];
			item_value(s, items[i], l->brief, v, sizeof(v));
			g_string_append_printf(more, "%s%s %s", more->len ? " · " : "",
				items[i]->kind == TW_CLAUDE_RESET ? "resets" : "lasts", v);
		}
		if (more->len) {
			pd_text(cr, l->small, more->str, l->pad, y + head + bh + 4 * l->sc,
				ctx->width - 2 * l->pad, l->text * 1.3, pal->dim, PD_LEFT);
		}
		g_string_free(more, TRUE);
		y += block;
	}
}

/* A ring for each window with a usage ticked, side by side, its other items under it. */
static void desk_rings(struct widget *w, const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, double y, double bottom) {
	cairo_t *cr = ctx->cairo;
	const struct tw_claude_item *items[MAX_ITEMS];
	int count = source_items(w, s, items);
	bool seven[2] = { false, false };
	int windows = usage_windows(items, count, seven);
	if (windows == 0) {
		desk_list(w, s, ctx, pal, l, y, bottom);
		return;
	}
	double col = (ctx->width - 2 * l->pad) / windows;
	for (int k = 0; k < windows; k++) {
		const struct tw_claude_item *usage = tw_claude_item_find(seven[k] ? "usage_7d" : "usage_5h");
		char value[64];
		double share = item_value(s, usage, false, value, sizeof(value));
		int extras = 0;
		for (int i = 0; i < count; i++) {
			extras += items[i]->seven_day == seven[k] && items[i]->kind != TW_CLAUDE_USAGE;
		}
		double below = l->text * 1.3 * (1 + extras);
		double room = fmin(col - 8 * l->sc, bottom - y - below);
		double r = room / 2 - 2 * l->sc;
		if (r < 8 * l->sc) {
			r = 8 * l->sc;
		}
		double cx = l->pad + col * k + col / 2, cy = y + r + 2 * l->sc;
		double thick = l->bar_h > 0 ? l->bar_h : fmax(r * 0.2, 4 * l->sc);
		cairo_set_line_width(cr, thick);
		if (l->look != PD_BAR_SMOOTH) {
			// in blocks, as the ring of the other widgets in these themes
			int blocks = 20;
			double gap = 0.035;
			for (int b = 0; b < blocks; b++) {
				cairo_new_path(cr);
				cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2 + 2 * M_PI * b / blocks + gap,
					-M_PI / 2 + 2 * M_PI * (b + 1) / blocks - gap);
				bool on = l->bars && (b + 0.5) / blocks <= share / 100.0;
				fill_rgba(cr, on ? level_color(w, share, l->fill) : pal->track,
					on ? 1 : (pal->track & 0xff) / 255.0);
				cairo_stroke(cr);
			}
		} else {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, 0, 2 * M_PI);
			fill_rgba(cr, pal->track, (pal->track & 0xff) / 255.0);
			cairo_stroke(cr);
		}
		if (share > 0 && l->bars && l->look == PD_BAR_SMOOTH) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r - thick / 2, -M_PI / 2, -M_PI / 2 + 2 * M_PI * share / 100.0);
			fill_rgba(cr, level_color(w, share, l->fill), 1);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		}
		pd_text(cr, l->bold, value, cx - r, cy - l->text, 2 * r, 2 * l->text,
			level_color(w, share, pal->fg), PD_CENTER);
		double ty = cy + r + 4 * l->sc;
		pd_text(cr, l->small, long_name(s, usage, true), cx - col / 2, ty, col, l->text * 1.3,
			pal->dim, PD_CENTER);
		for (int i = 0; i < count; i++) {
			if (items[i]->seven_day != seven[k] || items[i]->kind == TW_CLAUDE_USAGE) {
				continue;
			}
			char v[64], line[96];
			item_value(s, items[i], false, v, sizeof(v)); // "in 2 d 23 h", "about 31 min"
			snprintf(line, sizeof(line), "%s %s",
				items[i]->kind == TW_CLAUDE_RESET ? "resets" : "lasts", v);
			ty += l->text * 1.3;
			pd_text(cr, l->small, line, cx - col / 2, ty, col, l->text * 1.3, pal->dim, PD_CENTER);
		}
	}
}

/*
 * The charts of the flyout on the desktop (claudecharts.c): the week ("week")
 * or the last three 5-hour windows ("windows"), with the reading under the
 * pointer, in the colors of the desktop's widgets.
 */
static void desk_chart(const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, double y, double bottom,
		bool week) {
	struct claude_charts charts = { 0 };
	if (!s->tool && s->point_count > 0) {
		claude_charts_set(&charts, s->points, s->point_count, &s->chart_usage,
			time(NULL)); // Antigravity's
	} else {
		claude_charts_load(&charts, s->have ? &s->usage : NULL, time(NULL));
	}
	if (!charts.have) {
		pd_text(ctx->cairo, l->font, "Nothing written down yet", l->pad, y,
			ctx->width - 2 * l->pad, 30 * l->sc, pal->dim, PD_LEFT);
		free(charts.points);
		return;
	}
	bool light_text = (pal->fg >> 24 & 0xff) + (pal->fg >> 16 & 0xff) + (pal->fg >> 8 & 0xff) > 384;
	struct claude_look look = {
		.fg = pal->fg, .dim = pal->dim,
		.chart_bg = claude_desktop_chart_bg(pal),
		.chart_grid = pal->grid,
		.chart_line = pal->mono ? pal->fg : pal->accent,
		.label_bg = light_text ? 0x202020ff : 0xffffffff,
		.chart_radius = pal->radius > 0 ? 4 : 0,
		.dark = light_text,
		.sunken = pal->look == GADGET_CLASSIC, .resets = true,
		.font = l->small, .bold = l->bold,
	};
	struct claude_hover hover = { ctx->pointer, ctx->px, ctx->py };
	struct pbox box = { (int)l->pad, (int)y, (int)(ctx->width - 2 * l->pad),
		(int)(bottom - y) };
	if (box.height > 40) {
		if (week) {
			claude_chart_week(&hover, &charts, ctx->cairo, &look, box);
		} else {
			claude_chart_five_hours(&hover, &charts, ctx->cairo, &look, box);
		}
	}
	free(charts.points);
}

static bool desk_has_chart(const struct source *s);
static const char *desk_missing(struct widget *w, const struct source *s);

/* Why an account shows no values yet, in a line; NULL when it has them. */
static const char *desk_missing(struct widget *w, const struct source *s) {
	if (show_connect(s)) {
		return "Not connected: click to let Claude Code show its limits here";
	}
	if (s->have) {
		return NULL;
	}
	if (s->why) {
		return s->why;
	}
	if (s->account.agent == TW_AGENT_COPILOT && !widget_conf_bool(w, "account_checks", false)) {
		return "Allowance: turn on Ask the accounts"; // (in its settings)
	}
	if (s->account.agent == TW_AGENT_ANTIGRAVITY && !widget_conf_bool(w, "account_checks", false)) {
		return "Limits: turn on Ask the accounts"; // (or its status line, docs/usage.md)
	}
	return "The limits come with its next answer";
}

/* Whether an account has a history to draw a chart of (Claude Code, Codex, Antigravity). */
static bool desk_has_chart(const struct source *s) {
	if (!s->tool) {
		return s->point_count > 0 && s->chart_usage.seven_day.resets_at > 0;
	}
	return s->usage.history && s->have && s->usage.seven_day.resets_at > 0;
}

/* One account the old way: the whole card, as with one account. */
static void desk_whole(struct widget *w, const struct source *s, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, const struct desk_look *l, const char *style) {
	double y = desk_title(s, ctx, pal, l, l->pad);
	const char *missing = desk_missing(w, s);
	if (missing) {
		pd_text(ctx->cairo, l->font, missing, l->pad, y, ctx->width - 2 * l->pad, 40 * l->sc,
			pal->dim, PD_LEFT);
		return;
	}
	double bottom = ctx->height - l->pad;
	bottom -= desk_foot(s, ctx, pal, l, bottom);
	if (strcmp(style, "week") == 0 || strcmp(style, "windows") == 0) {
		desk_chart(s, ctx, pal, l, y, bottom, strcmp(style, "week") == 0);
	} else if (strcmp(style, "list") == 0) {
		desk_list(w, s, ctx, pal, l, y, bottom);
	} else if (strcmp(style, "bar") == 0) {
		desk_bars(w, s, ctx, pal, l, y, bottom);
	} else {
		desk_rings(w, s, ctx, pal, l, y, bottom);
	}
}

/*
 * Several accounts, one under the other, each as high as what it shows needs:
 * its title, its lines (bars, rings) or why it has none yet, how old they
 * are. In the chart styles each account with a history has a chart, sharing
 * the room; in the list the room left over goes to a chart of the week under
 * each account that has one.
 */
/* How high each account's part is (need) and how many have a chart; the total. */
static double accounts_need(struct widget *w, const struct desk_look *l, bool charts, bool list,
		bool bars, double need[32], int *with_chart) {
	struct agents_state *st = w->data;
	double title = l->title ? l->text * 1.8 : 0, gap = l->text * 1.1;
	double line = l->text * 1.6, foot = l->updated ? l->text * 1.3 : 0, total = 0;
	int count = st->count < 32 ? st->count : 32;
	*with_chart = 0;
	for (int k = 0; k < count; k++) {
		const struct source *s = &st->src[k];
		const struct tw_claude_item *items[MAX_ITEMS];
		int n = source_items(w, s, items);
		bool seven[2];
		int windows = usage_windows(items, n, seven);
		need[k] = title;
		if (desk_missing(w, s)) {
			need[k] += show_connect(s) ? line * 2 : line;
		} else if (charts) {
			need[k] += desk_has_chart(s) ? 0 : line;
			need[k] += desk_has_chart(s) && s->usage.updated ? foot : 0;
		} else {
			need[k] += list ? list_height(w, s, l) : bars ? windows * l->text * 4.4 :
				l->text * (8 + 1.3 * (n - windows));
			need[k] += s->usage.updated ? foot : 0;
		}
		*with_chart += (charts || list) && !desk_missing(w, s) && desk_has_chart(s);
		total += need[k] + (k > 0 ? gap : 0);
	}
	return total;
}

static void desk_accounts(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal, struct desk_look *l, const char *style) {
	struct agents_state *st = w->data;
	bool charts = strcmp(style, "week") == 0 || strcmp(style, "windows") == 0;
	bool list = strcmp(style, "list") == 0, bars = strcmp(style, "bar") == 0;
	double need[32] = { 0 };
	int count = st->count < 32 ? st->count : 32, with_chart = 0;
	double room = ctx->height - 2 * l->pad;
	double total = accounts_need(w, l, charts, list, bars, need, &with_chart);
	// too little room for it all: smaller text, measured again (lines over each
	// other would be what squeezing them gave)
	for (int i = 0; i < 4 && total > room && desk_shrink(l, pal, room / total) < 1; i++) {
		total = accounts_need(w, l, charts, list, bars, need, &with_chart);
	}
	// still too little with the smallest text: each account its share, its title
	// kept and of its lines those that fit (desk_list leaves the others out)
	double scale = total > room && total > 0 ? room / total : 1;
	double gap = l->text * 1.1, line = l->text * 1.6;
	// what is left: the charts (the chart styles need room for them at all)
	double spare = room - total, chart = 0;
	if (with_chart > 0 && spare > 0) {
		chart = spare / with_chart;
		if (list) {
			chart = fmin(chart, l->text * 10); // a chart under the lines, not a wall
			if (chart < l->text * 5) {
				chart = 0; // too little room: the lines alone
			}
		}
	}
	double y = l->pad;
	for (int k = 0; k < count; k++) {
		const struct source *s = &st->src[k];
		double top = y, bottom = y + need[k] * scale;
		bool chart_here = chart > 0 && desk_has_chart(s) && !desk_missing(w, s);
		if (chart_here) {
			bottom += chart;
		}
		y = desk_title(s, ctx, pal, l, top);
		const char *missing = desk_missing(w, s);
		if (missing) {
			pd_text(ctx->cairo, l->font, missing, l->pad, y, ctx->width - 2 * l->pad,
				bottom - y, pal->dim, PD_LEFT);
		} else {
			double end = bottom - (s->usage.updated ? desk_foot(s, ctx, pal, l, bottom) : 0);
			if (charts) {
				if (chart_here) {
					desk_chart(s, ctx, pal, l, y, end, strcmp(style, "week") == 0);
				} else {
					pd_text(ctx->cairo, l->font, "Nothing written down yet", l->pad, y,
						ctx->width - 2 * l->pad, line, pal->dim, PD_LEFT);
				}
			} else {
				double lines = end - (chart_here ? chart : 0);
				if (list) {
					desk_list(w, s, ctx, pal, l, y, lines);
				} else if (bars) {
					desk_bars(w, s, ctx, pal, l, y, lines);
				} else {
					desk_rings(w, s, ctx, pal, l, y, lines);
				}
				if (chart_here) {
					desk_chart(s, ctx, pal, l, lines + l->text * 0.4, end, true);
				}
			}
		}
		// a click on its part of the card: the flyout of this account alone
		if (ctx->surface) {
			psurface_add_hotspot(ctx->surface, 0, (int)(top - gap * scale / 2), (int)ctx->width,
				(int)(bottom - top + gap * scale), w, HOTSPOT_ACCOUNT, k, NULL);
		}
		y = bottom + gap * scale;
	}
}

static const uint32_t overlay_colors[TW_AGENT_COUNT] = {
	[TW_AGENT_CLAUDE] = 0xff7a33ff,
	[TW_AGENT_CODEX] = 0x16d96bff,
	[TW_AGENT_COPILOT] = 0xc855ffff,
	[TW_AGENT_ANTIGRAVITY] = 0x2986ffff,
};

/* AGY records each period separately. Strip the period to pair the two windows. */
static size_t overlay_bucket_group(const char *name) {
	const char *end = g_strrstr(name, " · ");
	if (!end) {
		return 0;
	}
	const char *const suffixes[] = {
		"-weekly", "_weekly", "-7d", "_7d", "-5h", "_5h", "-5-hour",
		"_5-hour", "-five-hour", "_five_hour", "-fivehour", NULL,
	};
	size_t length = end - name;
	for (int i = 0; suffixes[i]; i++) {
		size_t n = strlen(suffixes[i]);
		if (length > n && g_ascii_strncasecmp(end - n, suffixes[i], n) == 0) {
			return length - n;
		}
	}
	return 0;
}

static const struct tw_claude_window *overlay_agy_gate(const GPtrArray *limits,
		const struct agent_limit *limit) {
	size_t group = overlay_bucket_group(limit->name);
	for (guint i = 0; group && i < limits->len; i++) {
		const struct agent_limit *other = limits->pdata[i];
		if (other->kind == AGENT_AGY && other->usage.seven_day.known &&
				group == overlay_bucket_group(other->name) &&
				g_ascii_strncasecmp(limit->name, other->name, group) == 0) {
			return &other->usage.seven_day;
		}
	}
	return NULL;
}

static void overlay_agy_series(GArray *series, const GPtrArray *limits) {
	for (guint i = 0; limits && i < limits->len; i++) {
		const struct agent_limit *l = limits->pdata[i];
		if (l->kind != AGENT_AGY) {
			continue;
		}
		const char *group = g_str_has_prefix(l->name, "3p-") ? "Claude/GPT" :
			g_str_has_prefix(l->name, "gemini-argon-") ? "Argon" :
			g_str_has_prefix(l->name, "gemini-") ? "Gemini" : NULL;
		struct agent_overlay_series item = {
			.color = g_str_has_prefix(l->name, "3p-") ? 0x00d0ffff :
				g_str_has_prefix(l->name, "gemini-argon-") ? 0x7c6effff :
				overlay_colors[TW_AGENT_ANTIGRAVITY],
			.usage = &l->usage,
			.weekly_gate = l->usage.five_hour.known ? overlay_agy_gate(limits, l) : NULL,
			.points = l->chart.points, .count = l->chart.count,
		};
		snprintf(item.name, sizeof(item.name), "Antigravity · %s", group ? group : l->name);
		// Each known group uses one legend slot for its two chart series.
		bool listed = false;
		for (guint j = 0; group && j < series->len; j++) {
			const struct agent_overlay_series *existing =
				&g_array_index(series, struct agent_overlay_series, j);
			if (strcmp(item.name, existing->name) == 0) {
				listed = true;
				break;
			}
		}
		if (!listed) {
			snprintf(item.legend, sizeof(item.legend), "AGY %s",
				group && strcmp(group, "Claude/GPT") == 0 ? "3P" : group ? group : l->name);
		}
		g_array_append_val(series, item);
	}
}

static bool desk_overlay(struct widget *w, struct gadget_ctx *ctx, const struct gadget_palette *pal) {
	struct agents_state *st = w->data;
	GArray *series = g_array_new(false, false, sizeof(struct agent_overlay_series));
	time_t now = time(NULL);
	bool agy_added = false;
	for (int i = 0; i < st->count; i++) {
		struct source *src = &st->src[i];
		enum tw_agent agent = src->account.agent;
		if (agent == TW_AGENT_ANTIGRAVITY) {
			if (agy_added) {
				continue;
			}
			agy_added = true;
			if (st->collected) {
				guint first = series->len;
				overlay_agy_series(series, st->collected->limits);
				if (series->len > first) {
					continue;
				}
			}
		}
		struct agent_overlay_series item = { .color = overlay_colors[agent] };
		source_title(src, item.name, sizeof(item.name));
		snprintf(item.legend, sizeof(item.legend), "%s", short_names[agent]);
		if (agent == TW_AGENT_COPILOT && src->have) {
			double billing = tw_claude_window_now(&src->usage.five_hour, now);
			if (billing >= 0) {
				snprintf(item.legend, sizeof(item.legend), "Copilot %.1f%% billing", billing);
			}
		}
		if (agent != TW_AGENT_COPILOT && src->have) {
			item.usage = &src->usage;
			item.weekly_gate = &src->usage.seven_day;
			item.points = src->points;
			item.count = src->point_count;
		}
		g_array_append_val(series, item);
	}
	agent_overlay_draw(ctx, pal, series);
	g_array_unref(series);
	return true;
}

static bool agents_draw_gadget(struct widget *w, struct gadget_ctx *ctx,
		const struct gadget_palette *pal) {
	const char *style = ctx->style ? ctx->style : "";
	if (strcmp(style, "overlay") == 0) {
		return desk_overlay(w, ctx, pal);
	}
	if (strcmp(style, "list") != 0 && strcmp(style, "bar") != 0 && strcmp(style, "ring") != 0 &&
			strcmp(style, "week") != 0 && strcmp(style, "windows") != 0) {
		return false; // compact and tile: the taskbar look
	}
	struct agents_state *st = w->data;
	struct desk_look l;
	desk_look(w, ctx, pal, &l);
	if (st->count == 1) {
		desk_whole(w, &st->src[0], ctx, pal, &l, style);
	} else if (st->count > 1) {
		desk_accounts(w, ctx, pal, &l, style);
	}
	return true;
}

/* ---------- tooltip, click, sample ---------- */

static void append_window(GString *t, const struct source *s, bool seven_day) {
	const struct tw_claude_window *win = seven_day ? &s->usage.seven_day : &s->usage.five_hour;
	long length = seven_day ? TW_CLAUDE_SEVEN_DAYS : TW_CLAUDE_FIVE_HOURS;
	time_t now = time(NULL);
	double used = tw_claude_window_now(win, now);
	if (used < 0) {
		return;
	}
	const char *name = s->account.agent == TW_AGENT_COPILOT ?
		(s->allowance[0] ? s->allowance : "Allowance") :
		seven_day ? "Weekly limit" : "5-hour limit";
	g_string_append_printf(t, "\n%s: %.0f%% used", name, used);
	if (win->resets_at > now) {
		char span[48], when[48];
		struct tm tm;
		localtime_r(&win->resets_at, &tm);
		strftime(when, sizeof(when), win->resets_at - now < 24 * 3600 ? "%H:%M" : "%a %H:%M", &tm);
		tw_claude_format_span(win->resets_at - now, false, span, sizeof(span));
		g_string_append_printf(t, ", resets at %s (in %s)", when, span);
	}
	if (s->account.agent == TW_AGENT_COPILOT) {
		return; // no window of a known length to tell a pace from
	}
	bool known;
	long left = tw_claude_window_left(win, length, now, &known);
	if (known && left > 0) {
		char span[48];
		tw_claude_format_span(left, false, span, sizeof(span));
		g_string_append_printf(t, "\n    at this pace used up in about %s", span);
	} else if (known && left < 0) {
		g_string_append(t, "\n    at this pace it lasts until it resets");
	}
}

/* One account in the tooltip: its name, its windows, what runs, what came from elsewhere. */
static void append_source(struct widget *w, GString *t, const struct source *s, bool checks) {
	struct agents_state *st = w->data;
	char title[300];
	source_title(s, title, sizeof(title));
	g_string_append(t, title);
	const char *name = s->name;
	if (show_connect(s)) {
		g_string_append_printf(t, "\n%s does not show its usage limits to tileWin yet: "
			"click to connect it (tileWin goes before its status line).",
			s->account.dir ? s->account.dir : "This Claude Code");
	} else if (!s->have) {
		if (s->account.agent == TW_AGENT_COPILOT && !checks) {
			g_string_append(t, "\nIts allowance shows when the accounts are asked (its settings).");
		} else {
			g_string_append_printf(t, "\nThe limits come with the next answer of %s.", name);
		}
	} else {
		append_window(t, s, false);
		if (s->account.agent != TW_AGENT_COPILOT) {
			append_window(t, s, true);
		}
	}
	if (s->running > 0) {
		g_string_append_printf(t, "\nRunning here: %d %s%s%s%s", s->running,
			s->running == 1 ? "session" : "sessions", s->models[0] ? " (" : "", s->models,
			s->models[0] ? ")" : "");
	}
	if (s->elsewhere) {
		g_string_append_printf(t, "\nUsed elsewhere too: %.0f%% more while nothing of it ran "
			"here (another computer with the same account?)", s->elsewhere_share);
	}
	struct agent_resume_status rs;
	agent_resume_account_status(w->panel, s->account.agent, s->account.id, s->account.dir,
		st->collected, &rs);
	if (rs.enabled) {
		time_t now = time(NULL);
		if (rs.deferred) {
			g_string_append(t, "\nAuto-resume: waiting for screen unlock");
		} else if (rs.scheduled && rs.fire_at > now) {
			long left = (long)(rs.fire_at - now);
			char span[48];
			if (left < 60) {
				snprintf(span, sizeof(span), "%ld s", left);
			} else {
				tw_claude_format_span(left, false, span, sizeof(span));
			}
			g_string_append_printf(t, "\nAuto-resume: scheduled in %s (%d limited %s)", span,
				rs.terminals, rs.terminals == 1 ? "terminal" : "terminals");
		} else {
			g_string_append_printf(t, "\nAuto-resume: on (%d s after reset, %d limited %s)",
				rs.delay_s, rs.terminals, rs.terminals == 1 ? "terminal" : "terminals");
		}
	}
	if (s->have && s->usage.updated) {
		long ago = (long)(time(NULL) - s->usage.updated);
		char span[48];
		tw_claude_format_span(ago, false, span, sizeof(span));
		g_string_append_printf(t, "\nAs %s showed them %s ago", name, span);
	}
}

static char *agents_tooltip(struct widget *w, struct hotspot *hs) {
	const char *style = NULL;
	struct twconf_node *node = w->desk ? twconf_child(w->desk, "style") : NULL;
	if (node && node->argc > 0) {
		style = node->argv[0];
	}
	if (style && (strcmp(style, "week") == 0 || strcmp(style, "windows") == 0)) {
		return NULL; // the chart shows the reading under the pointer instead
	}
	struct agents_state *st = w->data;
	bool checks = widget_conf_bool(w, "account_checks", false);
	GString *t = g_string_new(NULL);
	for (int i = 0; i < st->count; i++) {
		if (i > 0) {
			g_string_append(t, "\n\n");
		}
		append_source(w, t, &st->src[i], checks);
	}
	return g_string_free(t, FALSE);
}

static bool agents_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct agents_state *st = w->data;
	if (button != BTN_LEFT || st->count == 0) {
		return false;
	}
	int k = hs && hs->id >= 0 && hs->id < st->count ? hs->id : 0;
	bool alone = hs && hs->kind == HOTSPOT_ACCOUNT; // a desktop card's part of one account
	const struct source *src = &st->src[k];
	if (show_connect(src) && src->account.dir) {
		char *quoted = g_shell_quote(src->account.dir);
		char *cmd = g_strdup_printf("tilewin-claude-usage connect %s", quoted);
		proc_spawn(cmd);
		g_free(cmd);
		g_free(quoted);
		return true;
	}
	// the limits in full, in a flyout in the look of the theme, of the account clicked
	st->current = k;
	st->alone = alone;
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	struct pbox card, part;
	if (alone && deskwidget_place(s, (struct pbox){ 0, 0, s->width, s->height }, &card) &&
			deskwidget_place(s, hs->box, &part) && s->output) {
		// beside the card, at the height of the account clicked, on the side with room
		// (from its bottom upwards in the lower half of the screen)
		bool left = card.x + card.width / 2 > s->output->width / 2;
		bool above = part.y + part.height / 2 > s->output->height / 2;
		anchor = (struct popup_anchor){
			.output = s->output,
			.x = left ? card.x - 8 : card.x + card.width + 8,
			.y = above ? part.y + part.height : part.y,
			.above = above,
			.right_align = left,
		};
	}
	info_flyout_toggle(w, anchor);
	return true;
}

static bool agents_sample(struct widget *w, struct widget_sample *out) {
	struct agents_state *st = w->data;
	const struct source *s = st->count > 0 ? &st->src[0] : NULL;
	if (!s || !s->have) {
		return false;
	}
	time_t now = time(NULL);
	double five = tw_claude_window_now(&s->usage.five_hour, now);
	double seven = tw_claude_window_now(&s->usage.seven_day, now);
	out->percent = (int)(five >= 0 ? five : seven >= 0 ? seven : 0);
	snprintf(out->value, sizeof(out->value), "%.0f%%", five >= 0 ? five : 0);
	if (seven >= 0) {
		snprintf(out->detail, sizeof(out->detail), "Week %.0f%%", seven);
	}
	out->warning = out->percent >= widget_conf_int(w, "warning", 80);
	out->critical = out->percent >= widget_conf_int(w, "critical", 95);
	return true;
}

/* ---------- for the flyout ---------- */

static struct agents_state *state_of(struct widget *w) {
	return w && (w->impl == &widget_agents || w->impl == &widget_claude ||
		w->impl == &widget_codex) ? w->data : NULL;
}

int agents_widget_accounts(struct widget *w) {
	struct agents_state *st = state_of(w);
	if (!st) {
		return 0;
	}
	collect(w, COLLECT_OPEN_S); // what runs here, fresher while it is looked at
	reload(w);                  // the newest, as the flyout opens or reads again
	return st->count;
}

bool agents_widget_view(struct widget *w, int i, struct agents_view *out) {
	struct agents_state *st = state_of(w);
	if (!st || i < 0 || i >= st->count) {
		return false;
	}
	const struct source *s = &st->src[i];
	struct agent_resume_status rs;
	agent_resume_account_status(w->panel, s->account.agent, s->account.id, s->account.dir,
		st->collected, &rs);
	*out = (struct agents_view){
		.name = s->name,
		.agent = s->account.agent,
		.group = s->group,
		.tool = s->tool,
		.usage = s->have || s->usage.dir ? &s->usage : NULL,
		.connect = show_connect(s),
		.running = s->running,
		.models = s->models,
		.elsewhere = s->elsewhere,
		.elsewhere_share = s->elsewhere_share,
		.elsewhere_at = s->elsewhere_at,
		.allowance = s->account.agent == TW_AGENT_COPILOT ?
			(s->allowance[0] ? s->allowance : "Allowance") : NULL,
		.checks = widget_conf_bool(w, "account_checks", false),
		.points = s->tool ? NULL : s->points,
		.point_count = s->tool ? 0 : s->point_count,
		.chart_usage = s->tool ? NULL : &s->chart_usage,
		.resume_enabled = rs.enabled,
		.resume_scheduled = rs.scheduled,
		.resume_deferred = rs.deferred,
		.resume_fire_at = rs.fire_at,
		.resume_delay = rs.delay_s,
		.resume_terminals = rs.terminals,
	};
	if (!s->have && !s->tool) {
		out->usage = NULL;
	}
	return true;
}

bool agents_widget_alone(struct widget *w) {
	struct agents_state *st = state_of(w);
	return st && st->alone;
}

int agents_widget_current(struct widget *w) {
	struct agents_state *st = state_of(w);
	return st ? st->current : 0;
}

void agents_widget_pick(struct widget *w, int i) {
	struct agents_state *st = state_of(w);
	if (st && i >= 0 && i < st->count) {
		st->current = i;
	}
}

/* ---------- life ---------- */

static void agents_set_active(struct widget *w, bool active) {
	struct agents_state *st = w->data;
	if (active && !st->timer) {
		tick(w);
	} else if (!active && st->timer) {
		loop_remove_timer(w->panel->loop, st->timer);
		st->timer = NULL;
	}
}

static void usage_init(struct widget *w, const struct tw_usage_tool *legacy) {
	struct agents_state *st = calloc(1, sizeof(struct agents_state));
	st->legacy = legacy;
	w->data = st;
	reload(w);
}

static void agents_init(struct widget *w) {
	usage_init(w, NULL);
}

static void claude_init(struct widget *w) {
	usage_init(w, &tw_claude_tool);
}

static void codex_init(struct widget *w) {
	usage_init(w, &tw_codex_tool);
}

static void agents_destroy(struct widget *w) {
	struct agents_state *st = w->data;
	if (st->timer) {
		loop_remove_timer(w->panel->loop, st->timer);
	}
	if (st->pending) {
		// the thread pushes into it still; whoever is last frees it
		g_async_queue_unref(st->pending);
	}
	agent_usage_free(st->collected);
	free_sources(st);
	free(st->accounts);
	free(st->found);
	free(st);
}

#define AGENTS_IMPL(name, initializer) { \
	.type = name, \
	.init = initializer, \
	.destroy = agents_destroy, \
	.measure = agents_measure, \
	.render = agents_render, \
	.click = agents_click, \
	.tooltip = agents_tooltip, \
	.set_active = agents_set_active, \
	.sample = agents_sample, \
	.draw_gadget = agents_draw_gadget, \
}

const struct widget_impl widget_agents = AGENTS_IMPL("agents", agents_init);
// the widgets of older configs: the same, with the account of their "account" option
const struct widget_impl widget_claude = AGENTS_IMPL("claude", claude_init);
const struct widget_impl widget_codex = AGENTS_IMPL("codex", codex_init);
