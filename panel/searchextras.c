#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "tw_desktop.h"
#include "tw_search.h"

#define FIND_DELAY_MS 220
#define FIND_MIN_LENGTH 2
#define FILES_SHOWN 8

struct search_extras {
	struct panel *panel;
	void (*on_update)(void *data);
	void *data;
	list_t *top, *below;
	int settings; // how many of below are settings pages, the files follow
	char query[256];
	struct proc *proc;
	struct loop_timer *timer;
};

static struct tw_desktop_entry *entry_new(const char *id, const char *name, const char *detail,
		const char *icon, char *exec) {
	struct tw_desktop_entry *e = calloc(1, sizeof(*e));
	e->id = strdup(id);
	e->name = strdup(name);
	e->generic_name = detail ? strdup(detail) : NULL;
	e->icon = strdup(icon);
	e->exec = exec;
	return e;
}

static void clear_list(list_t *list, int from) {
	for (int i = list->length - 1; i >= from; i--) {
		tw_desktop_entry_free(list->items[i]);
		list_del(list, i);
	}
}

static void fill_calc(struct search_extras *x) {
	double value;
	if (!tw_calc(x->query, &value)) {
		return;
	}
	char text[64], name[80];
	tw_calc_format(value, text, sizeof(text));
	snprintf(name, sizeof(name), "= %s", text);
	list_add(x->top, entry_new("search:calc", name, "Enter copies the result",
		"accessories-calculator", format_str("wl-copy -- %s", text)));
}

static void fill_settings(struct search_extras *x) {
	x->settings = 0;
	for (int score = 3; score >= 1; score--) {
		for (size_t i = 0; i < tw_settings_page_count; i++) {
			const struct tw_settings_page *page = &tw_settings_pages[i];
			if (tw_settings_page_score(page, x->query) != score) {
				continue;
			}
			char *name = format_str("Settings: %s", page->title);
			list_add(x->below, entry_new("search:settings", name, "tileWin Settings",
				"preferences-system",
				format_str("tilewin-settings --page=%s", page->name)));
			free(name);
			x->settings++;
		}
	}
}

static const char *file_icon(const char *name, bool folder) {
	if (folder) {
		return "folder";
	}
	static const struct {
		const char *ext, *icon;
	} kinds[] = {
		{ "png", "image-x-generic" }, { "jpg", "image-x-generic" },
		{ "jpeg", "image-x-generic" }, { "svg", "image-x-generic" },
		{ "webp", "image-x-generic" }, { "gif", "image-x-generic" },
		{ "mp3", "audio-x-generic" }, { "flac", "audio-x-generic" },
		{ "ogg", "audio-x-generic" }, { "wav", "audio-x-generic" },
		{ "mp4", "video-x-generic" }, { "mkv", "video-x-generic" },
		{ "webm", "video-x-generic" }, { "pdf", "application-pdf" },
		{ "zip", "package-x-generic" }, { "gz", "package-x-generic" },
		{ "xz", "package-x-generic" }, { "zst", "package-x-generic" },
	};
	const char *dot = strrchr(name, '.');
	for (size_t i = 0; dot && i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		if (strcasecmp(dot + 1, kinds[i].ext) == 0) {
			return kinds[i].icon;
		}
	}
	return "text-x-generic";
}

/*
 * The command goes through the command parser of the compositor first, which
 * splits at a ; even between the quotes a shell wants around a ' in a name. So
 * anything but plain letters goes as octal numbers, for printf to put back.
 */
static char *open_command(const char *path) {
	char *out = malloc(4 * strlen(path) + 32);
	char *o = out + sprintf(out, "xdg-open \"$(printf '");
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if (isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-') {
			*o++ = *p;
		} else {
			o += sprintf(o, "\\%03o", *p);
		}
	}
	strcpy(o, "')\"");
	return out;
}

struct found {
	char *line; // "<kind> <path>"
	int rank;
};

static int found_cmp(const void *a, const void *b) {
	const struct found *fa = a, *fb = b;
	return fa->rank != fb->rank ? fa->rank - fb->rank : strcmp(fa->line, fb->line);
}

/* Names that begin with the query first, then those near the home folder. */
static int found_rank(const char *path, const char *query) {
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	int depth = 0;
	for (const char *p = path; *p; p++) {
		depth += *p == '/';
	}
	return depth + (strncasecmp(name, query, strlen(query)) == 0 ? 0 : 100);
}

static void find_done(void *data, const char *output) {
	struct search_extras *x = data;
	x->proc = NULL;
	char *copy = strdup(output);
	struct found found[256];
	size_t count = 0;
	char *save = NULL;
	for (char *line = strtok_r(copy, "\n", &save); line && count < 256;
			line = strtok_r(NULL, "\n", &save)) {
		if (strlen(line) > 2 && line[1] == ' ') {
			found[count].line = line;
			found[count].rank = found_rank(line + 2, x->query);
			count++;
		}
	}
	qsort(found, count, sizeof(found[0]), found_cmp);
	clear_list(x->below, x->settings);
	for (size_t i = 0; i < count && i < FILES_SHOWN; i++) {
		const char *path = found[i].line + 2;
		const char *slash = strrchr(path, '/');
		const char *name = slash ? slash + 1 : path;
		char *folder = slash ? strndup(path, slash - path) : strdup("");
		list_add(x->below, entry_new("search:file", name, folder,
			file_icon(name, found[i].line[0] == 'd'), open_command(path)));
		free(folder);
	}
	free(copy);
	if (x->on_update) {
		x->on_update(x->data);
	}
}

static void find_start(void *data) {
	struct search_extras *x = data;
	x->timer = NULL;
	// the letters of a pattern stand for themselves
	char pattern[2 * sizeof(x->query) + 3];
	char *o = pattern;
	*o++ = '*';
	for (const char *p = x->query; *p; p++) {
		if (strchr("*?[]\\", *p)) {
			*o++ = '\\';
		}
		*o++ = *p;
	}
	*o++ = '*';
	*o = '\0';
	char *quoted = tw_shell_quote(pattern);
	// hidden folders and what programs generate would drown what people look for
	char *command = format_str("exec timeout 5 find \"$HOME\" -mindepth 1 -maxdepth 6 "
		"\\( -name '.*' -o -name node_modules -o -name __pycache__ \\) -prune -o "
		"-iname %s -printf '%%y %%p\\n' 2>/dev/null", quoted);
	free(quoted);
	x->proc = proc_run(x->panel, command, false, NULL, find_done, x);
	free(command);
}

struct search_extras *search_extras_new(struct panel *panel, void (*on_update)(void *data),
		void *data) {
	struct search_extras *x = calloc(1, sizeof(*x));
	x->panel = panel;
	x->on_update = on_update;
	x->data = data;
	x->top = create_list();
	x->below = create_list();
	return x;
}

void search_extras_set_listener(struct search_extras *x, void (*on_update)(void *data),
		void *data) {
	x->on_update = on_update;
	x->data = data;
}

static void stop_finding(struct search_extras *x) {
	if (x->timer) {
		loop_remove_timer(x->panel->loop, x->timer);
		x->timer = NULL;
	}
	if (x->proc) {
		proc_cancel(x->proc);
		x->proc = NULL;
	}
}

void search_extras_free(struct search_extras *x) {
	stop_finding(x);
	clear_list(x->top, 0);
	clear_list(x->below, 0);
	list_free(x->top);
	list_free(x->below);
	free(x);
}

void search_extras_set_query(struct search_extras *x, const char *query) {
	stop_finding(x);
	snprintf(x->query, sizeof(x->query), "%s", query);
	clear_list(x->top, 0);
	clear_list(x->below, 0);
	x->settings = 0;
	if (!query[0]) {
		return;
	}
	fill_calc(x);
	fill_settings(x);
	double value;
	if (strlen(query) >= FIND_MIN_LENGTH && !tw_calc(query, &value)) {
		x->timer = loop_add_timer(x->panel->loop, FIND_DELAY_MS, find_start, x);
	}
}

const list_t *search_extras_top(struct search_extras *x) {
	return x->top;
}

const list_t *search_extras_below(struct search_extras *x) {
	return x->below;
}

struct tw_desktop_entry *search_extras_at(struct search_extras *x, int n) {
	if (n < 0) {
		return NULL;
	}
	if (n < x->top->length) {
		return x->top->items[n];
	}
	n -= x->top->length;
	return n < x->below->length ? x->below->items[n] : NULL;
}

int search_extras_index(struct search_extras *x, const struct tw_desktop_entry *e) {
	for (int i = 0; i < x->top->length; i++) {
		if (x->top->items[i] == e) {
			return i;
		}
	}
	for (int i = 0; i < x->below->length; i++) {
		if (x->below->items[i] == e) {
			return x->top->length + i;
		}
	}
	return -1;
}
