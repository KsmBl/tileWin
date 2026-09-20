#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "confdoc.h"
#include "tw_paths.h"

enum tok_type {
	T_EOF,
	T_WORD,
	T_LBRACE,
	T_RBRACE,
	T_SEP,
};

struct token {
	enum tok_type type;
	char *text;
	size_t start, end;
};

struct lexer {
	const char *s;
	size_t pos;
	bool line_has_token;
};

/* Mirrors the tokenizer in common/twconf.c. */
static struct token lex(struct lexer *l) {
	const char *s = l->s;
	for (;;) {
		char c = s[l->pos];
		if (c == ' ' || c == '\t' || c == '\r') {
			l->pos++;
		} else if (c == '\\' && s[l->pos + 1] == '\n') {
			l->pos += 2;
		} else if (c == '#' && (!l->line_has_token || strchr(" \t\n#", s[l->pos + 1]))) {
			while (s[l->pos] && s[l->pos] != '\n') {
				l->pos++;
			}
		} else {
			break;
		}
	}
	struct token t = { .start = l->pos };
	char c = s[l->pos];
	if (c == '\0') {
		t.type = T_EOF;
		t.end = l->pos;
		return t;
	}
	if (c == '\n' || c == ';') {
		if (c == '\n') {
			l->line_has_token = false;
		}
		l->pos++;
		t.type = T_SEP;
		t.end = l->pos;
		return t;
	}
	l->line_has_token = true;
	if (c == '{' || c == '}') {
		l->pos++;
		t.type = c == '{' ? T_LBRACE : T_RBRACE;
		t.end = l->pos;
		return t;
	}
	GString *buf = g_string_new(NULL);
	if (c == '"' || c == '\'') {
		char quote = c;
		l->pos++;
		while ((c = s[l->pos])) {
			l->pos++;
			if (c == quote) {
				break;
			}
			if (c == '\\' && quote == '"' && s[l->pos]) {
				char e = s[l->pos++];
				c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
			}
			g_string_append_c(buf, c);
		}
	} else {
		while ((c = s[l->pos]) && !g_ascii_isspace(c) && c != ';' && c != '{' && c != '}') {
			g_string_append_c(buf, c);
			l->pos++;
		}
	}
	t.type = T_WORD;
	t.text = g_string_free(buf, FALSE);
	t.end = l->pos;
	return t;
}

static void stmt_free(struct cstmt *s) {
	if (!s) {
		return;
	}
	g_free(s->name);
	g_ptr_array_unref(s->args);
	if (s->children) {
		g_ptr_array_unref(s->children);
	}
	g_free(s);
}

static struct cstmt *stmt_new(struct cstmt *parent) {
	struct cstmt *s = g_new0(struct cstmt, 1);
	s->args = g_ptr_array_new_with_free_func(g_free);
	s->parent = parent;
	s->depth = parent ? parent->depth + 1 : -1;
	return s;
}

static void set_error(struct confdoc *doc, size_t offset, const char *msg) {
	if (doc->error) {
		return;
	}
	int line = 1;
	for (size_t i = 0; i < offset && i < doc->text->len; i++) {
		line += doc->text->str[i] == '\n';
	}
	doc->error = g_strdup_printf("line %d: %s", line, msg);
}

static void parse_block(struct confdoc *doc, struct lexer *l, struct cstmt *block, bool top) {
	block->children = g_ptr_array_new_with_free_func((GDestroyNotify)stmt_free);
	for (;;) {
		struct token t = lex(l);
		if (t.type == T_EOF) {
			if (!top) {
				set_error(doc, t.start, "missing '}'");
			}
			block->close = t.start;
			if (top) {
				block->end = t.start;
			}
			return;
		}
		if (t.type == T_SEP) {
			continue;
		}
		if (t.type == T_RBRACE) {
			if (top) {
				set_error(doc, t.start, "unexpected '}'");
				continue;
			}
			block->close = t.start;
			block->end = t.end;
			return;
		}
		struct cstmt *st = stmt_new(block);
		st->start = t.start;
		st->end = t.end;
		st->name_end = t.end;
		if (t.type == T_LBRACE) {
			set_error(doc, t.start, "block without a name");
			st->name = g_strdup("");
			parse_block(doc, l, st, false);
			g_ptr_array_add(block->children, st);
			continue;
		}
		st->name = t.text;
		for (;;) {
			struct token a = lex(l);
			if (a.type == T_WORD) {
				g_ptr_array_add(st->args, a.text);
				st->end = a.end;
			} else if (a.type == T_LBRACE) {
				parse_block(doc, l, st, false);
				break;
			} else {
				if (a.type == T_RBRACE) {
					l->pos = a.start; // let the enclosing block see it
				} else if (a.type == T_EOF) {
					l->pos = a.start;
				}
				break;
			}
		}
		g_ptr_array_add(block->children, st);
	}
}

static void detect_indent(struct confdoc *doc) {
	g_free(doc->indent);
	doc->indent = NULL;
	const char *s = doc->text->str;
	for (const char *line = s; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
		if (*line == '\t') {
			doc->indent = g_strdup("\t");
			return;
		}
		if (*line == ' ') {
			int n = 0;
			while (line[n] == ' ') {
				n++;
			}
			if (line[n] && line[n] != '\n' && line[n] != '#') {
				doc->indent = g_strnfill(n, ' ');
				return;
			}
		}
	}
	doc->indent = g_strdup("\t");
}

static void reparse(struct confdoc *doc) {
	stmt_free(doc->root);
	g_free(doc->error);
	doc->error = NULL;
	doc->root = stmt_new(NULL);
	doc->root->name = g_strdup("");
	struct lexer l = { .s = doc->text->str };
	parse_block(doc, &l, doc->root, true);
}

void confdoc_set_text(struct confdoc *doc, const char *text) {
	g_string_assign(doc->text, text);
	detect_indent(doc);
	reparse(doc);
}

struct confdoc *confdoc_open(const char *filename) {
	struct confdoc *doc = g_new0(struct confdoc, 1);
	char *dir = tw_config_dir();
	doc->path = g_build_filename(dir, filename, NULL);
	free(dir);
	char *contents = NULL;
	if (!g_file_get_contents(doc->path, &contents, NULL, NULL)) {
		char *fallback = g_build_filename(tw_data_dir(), "config", filename, NULL);
		if (!g_file_get_contents(fallback, &contents, NULL, NULL)) {
			contents = g_strdup("");
		}
		g_free(fallback);
	}
	doc->text = g_string_new(NULL);
	confdoc_set_text(doc, contents);
	g_free(contents);
	return doc;
}

void confdoc_free(struct confdoc *doc) {
	if (!doc) {
		return;
	}
	stmt_free(doc->root);
	g_string_free(doc->text, TRUE);
	g_free(doc->path);
	g_free(doc->indent);
	g_free(doc->error);
	g_free(doc);
}

bool confdoc_save(struct confdoc *doc, char **error) {
	if (!tw_write_string(doc->path, doc->text->str)) {
		if (error) {
			*error = g_strdup_printf("Could not write %s", doc->path);
		}
		return false;
	}
	return true;
}

int cstmt_argc(const struct cstmt *s) {
	return s ? (int)s->args->len : 0;
}

const char *cstmt_arg(const struct cstmt *s, int index) {
	return s && index >= 0 && index < (int)s->args->len ? s->args->pdata[index] : NULL;
}

char *cstmt_join(const struct cstmt *s, int index) {
	if (!s || index >= (int)s->args->len) {
		return NULL;
	}
	GString *out = g_string_new(NULL);
	for (guint i = index; i < s->args->len; i++) {
		if (out->len) {
			g_string_append_c(out, ' ');
		}
		g_string_append(out, s->args->pdata[i]);
	}
	return g_string_free(out, FALSE);
}

char *cstmt_raw_args(const struct confdoc *doc, const struct cstmt *s) {
	if (!s) {
		return NULL;
	}
	char *raw = g_strndup(doc->text->str + s->name_end, s->end - s->name_end);
	return g_strstrip(raw);
}

struct cstmt *confdoc_child(struct cstmt *parent, const char *name, const char *arg0) {
	if (!parent || !parent->children) {
		return NULL;
	}
	for (guint i = 0; i < parent->children->len; i++) {
		struct cstmt *c = parent->children->pdata[i];
		if (strcmp(c->name, name) == 0 &&
				(!arg0 || g_strcmp0(cstmt_arg(c, 0), arg0) == 0)) {
			return c;
		}
	}
	return NULL;
}

/* Converts leading tabs to the file's indentation and adds depth levels. */
static char *reindent(struct confdoc *doc, const char *text, int depth, bool indent_first) {
	GString *out = g_string_new(NULL);
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (i > 0) {
			g_string_append_c(out, '\n');
		}
		const char *line = lines[i];
		if (!*line) {
			continue;
		}
		int levels = depth;
		while (*line == '\t') {
			levels++;
			line++;
		}
		if (i > 0 || indent_first) {
			for (int j = 0; j < levels; j++) {
				g_string_append(out, doc->indent);
			}
		}
		g_string_append(out, line);
	}
	g_strfreev(lines);
	return g_string_free(out, FALSE);
}

static bool is_blank(char c) {
	return c == ' ' || c == '\t';
}

/* Erases a statement, together with its line if nothing else is on it. */
static void erase_statement(GString *t, size_t start, size_t end) {
	size_t e = end;
	while (e < t->len && is_blank(t->str[e])) {
		e++;
	}
	if (e < t->len && t->str[e] == ';') {
		end = e + 1;
	}
	size_t ls = start;
	while (ls > 0 && is_blank(t->str[ls - 1])) {
		ls--;
	}
	size_t le = end;
	while (le < t->len && is_blank(t->str[le])) {
		le++;
	}
	if (le < t->len && t->str[le] == '#') {
		size_t c = le;
		while (c < t->len && t->str[c] != '\n') {
			c++;
		}
		if (ls == 0 || t->str[ls - 1] == '\n') {
			le = c; // drop the trailing comment of a removed line
		}
	}
	if ((ls == 0 || t->str[ls - 1] == '\n') && (le == t->len || t->str[le] == '\n')) {
		start = ls;
		end = le < t->len ? le + 1 : le;
		// don't leave two blank lines behind where a block was removed
		if (start >= 2 && t->str[start - 1] == '\n' && t->str[start - 2] == '\n' &&
				end < t->len && t->str[end] == '\n') {
			end++;
		}
	}
	g_string_erase(t, start, end - start);
}

static void insert_at_end(struct confdoc *doc, struct cstmt *parent, const char *text) {
	GString *t = doc->text;
	if (!parent->parent) {
		GString *ins = g_string_new(NULL);
		if (t->len > 0 && t->str[t->len - 1] != '\n') {
			g_string_append_c(ins, '\n');
		}
		if (strchr(text, '\n') && t->len > 0) {
			g_string_append_c(ins, '\n');
		}
		char *body = reindent(doc, text, 0, true);
		g_string_append(ins, body);
		g_string_append_c(ins, '\n');
		g_free(body);
		g_string_append(t, ins->str);
		g_string_free(ins, TRUE);
		return;
	}
	size_t close = parent->close;
	size_t ls = close;
	while (ls > 0 && is_blank(t->str[ls - 1])) {
		ls--;
	}
	char *body = reindent(doc, text, parent->depth + 1, true);
	if (ls == 0 || t->str[ls - 1] == '\n') {
		char *line = g_strconcat(body, "\n", NULL);
		g_string_insert(t, ls, line);
		g_free(line);
	} else {
		GString *ins = g_string_new("\n");
		g_string_append(ins, body);
		g_string_append_c(ins, '\n');
		for (int i = 0; i < parent->depth; i++) {
			g_string_append(ins, doc->indent);
		}
		g_string_insert(t, close, ins->str);
		g_string_free(ins, TRUE);
	}
	g_free(body);
}

void confdoc_set_list(struct confdoc *doc, struct cstmt *parent, const char *name,
		const char *arg0, GPtrArray *lines) {
	GPtrArray *matches = g_ptr_array_new();
	for (guint i = 0; parent->children && i < parent->children->len; i++) {
		struct cstmt *c = parent->children->pdata[i];
		if (strcmp(c->name, name) == 0 &&
				(!arg0 || g_strcmp0(cstmt_arg(c, 0), arg0) == 0)) {
			g_ptr_array_add(matches, c);
		}
	}
	bool have_lines = lines && lines->len > 0;
	// remove from the back so earlier offsets stay valid
	for (int i = (int)matches->len - 1; i >= (have_lines ? 1 : 0); i--) {
		struct cstmt *c = matches->pdata[i];
		erase_statement(doc->text, c->start, c->end);
	}
	if (have_lines) {
		GString *text = g_string_new(NULL);
		for (guint i = 0; i < lines->len; i++) {
			if (i > 0) {
				g_string_append_c(text, '\n');
			}
			g_string_append(text, name);
			const char *args = lines->pdata[i];
			if (args && *args) {
				g_string_append_c(text, ' ');
				g_string_append(text, args);
			}
		}
		if (matches->len > 0) {
			struct cstmt *first = matches->pdata[0];
			char *body = reindent(doc, text->str, first->depth, false);
			g_string_erase(doc->text, first->start, first->end - first->start);
			g_string_insert(doc->text, first->start, body);
			g_free(body);
		} else {
			insert_at_end(doc, parent, text->str);
		}
		g_string_free(text, TRUE);
	}
	g_ptr_array_unref(matches);
	reparse(doc);
}

void confdoc_set(struct confdoc *doc, struct cstmt *parent, const char *name,
		const char *arg0, const char *args) {
	if (!args) {
		confdoc_set_list(doc, parent, name, arg0, NULL);
		return;
	}
	GPtrArray *lines = g_ptr_array_new();
	g_ptr_array_add(lines, (gpointer)args);
	confdoc_set_list(doc, parent, name, arg0, lines);
	g_ptr_array_unref(lines);
}

void confdoc_remove(struct confdoc *doc, struct cstmt *stmt) {
	erase_statement(doc->text, stmt->start, stmt->end);
	reparse(doc);
}

void confdoc_replace(struct confdoc *doc, struct cstmt *stmt, const char *text) {
	char *body = reindent(doc, text, stmt->depth, false);
	g_string_erase(doc->text, stmt->start, stmt->end - stmt->start);
	g_string_insert(doc->text, stmt->start, body);
	g_free(body);
	reparse(doc);
}

void confdoc_append(struct confdoc *doc, struct cstmt *parent, const char *text) {
	insert_at_end(doc, parent, text);
	reparse(doc);
}

struct cstmt *confdoc_block(struct confdoc *doc, const char *name, const char *arg0,
		bool create) {
	struct cstmt *block = confdoc_child(doc->root, name, arg0);
	if (block && block->children) {
		return block;
	}
	if (!create) {
		return NULL;
	}
	char *qarg = arg0 ? conf_quote(arg0) : NULL;
	char *text = g_strdup_printf("%s%s%s {\n}", name, qarg ? " " : "", qarg ? qarg : "");
	confdoc_append(doc, doc->root, text);
	g_free(text);
	g_free(qarg);
	for (guint i = doc->root->children->len; i > 0; i--) {
		struct cstmt *c = doc->root->children->pdata[i - 1];
		if (strcmp(c->name, name) == 0 && c->children &&
				(!arg0 || g_strcmp0(cstmt_arg(c, 0), arg0) == 0)) {
			return c;
		}
	}
	return NULL;
}

static bool needs_quotes(const char *word) {
	if (!*word || *word == '#' || *word == '\'' || *word == '"') {
		return true;
	}
	for (const char *p = word; *p; p++) {
		if (g_ascii_isspace(*p) || strchr(";{}\"\\", *p)) {
			return true;
		}
	}
	return false;
}

char *conf_quote(const char *word) {
	if (!word) {
		return g_strdup("\"\"");
	}
	if (!needs_quotes(word)) {
		return g_strdup(word);
	}
	GString *out = g_string_new("\"");
	for (const char *p = word; *p; p++) {
		switch (*p) {
		case '"':
			g_string_append(out, "\\\"");
			break;
		case '\\':
			g_string_append(out, "\\\\");
			break;
		case '\n':
			g_string_append(out, "\\n");
			break;
		case '\t':
			g_string_append(out, "\\t");
			break;
		default:
			g_string_append_c(out, *p);
		}
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

char *conf_quote_command(const char *command) {
	if (!command || !*command) {
		return g_strdup("");
	}
	// bare words only if splitting at single spaces gives back the same string
	bool bare = !strstr(command, "  ") && !g_ascii_isspace(command[0]) &&
		!g_ascii_isspace(command[strlen(command) - 1]);
	for (const char *p = command; bare && *p; p++) {
		if (strchr(";{}\"'\\#\t\n", *p)) {
			bare = false;
		}
	}
	static const char *const reserved[] = { "icon", "disabled", "checked", "bold" };
	for (size_t i = 0; bare && i < G_N_ELEMENTS(reserved); i++) {
		size_t n = strlen(reserved[i]);
		if (strncmp(command, reserved[i], n) == 0 && (!command[n] || command[n] == ' ')) {
			bare = false;
		}
	}
	if (bare) {
		return g_strdup(command);
	}
	char *quoted = conf_quote(command);
	if (quoted[0] == '"') {
		return quoted;
	}
	// conf_quote leaves a plain word alone, but a reserved one has to be
	// quoted or the menu reads it as the keyword it looks like
	char *forced = g_strdup_printf("\"%s\"", quoted);
	g_free(quoted);
	return forced;
}
