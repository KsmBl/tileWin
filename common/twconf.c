#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "stringop.h"
#include "tw_paths.h"
#include "twconf.h"

enum token_type {
	TOK_EOF,
	TOK_WORD,
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_SEP,
	TOK_ERROR,
};

struct token {
	enum token_type type;
	char *text;
	int line;
};

struct var {
	char *name;
	char *value;
};

struct parser {
	const char *src;
	size_t pos;
	int line;
	bool line_has_token;
	const char *origin;
	char *error;
	list_t *vars; // struct var *, shared with included files
	int include_depth;
	bool has_peek;
	struct token peek;
};

static void set_error(struct parser *p, int line, const char *msg) {
	if (!p->error) {
		p->error = format_str("%s:%d: %s", p->origin ? p->origin : "<string>",
			line, msg);
	}
}

static struct token read_token(struct parser *p) {
	const char *s = p->src;
	for (;;) {
		char c = s[p->pos];
		if (c == ' ' || c == '\t' || c == '\r') {
			p->pos++;
		} else if (c == '\\' && s[p->pos + 1] == '\n') {
			p->pos += 2;
			p->line++;
		} else if (c == '#' && (!p->line_has_token || s[p->pos + 1] == ' ' ||
				s[p->pos + 1] == '\t' || s[p->pos + 1] == '\n' ||
				s[p->pos + 1] == '#' || s[p->pos + 1] == '\0')) {
			while (s[p->pos] && s[p->pos] != '\n') {
				p->pos++;
			}
		} else {
			break;
		}
	}

	struct token t = { .line = p->line };
	char c = s[p->pos];
	if (c == '\0') {
		t.type = TOK_EOF;
		return t;
	}
	if (c == '\n' || c == ';') {
		if (c == '\n') {
			p->line++;
			p->line_has_token = false;
		}
		p->pos++;
		t.type = TOK_SEP;
		return t;
	}
	p->line_has_token = true;
	if (c == '{') {
		p->pos++;
		t.type = TOK_LBRACE;
		return t;
	}
	if (c == '}') {
		p->pos++;
		t.type = TOK_RBRACE;
		return t;
	}

	size_t cap = 32, len = 0;
	char *buf = malloc(cap);
	if (c == '"' || c == '\'') {
		char quote = c;
		p->pos++;
		for (;;) {
			c = s[p->pos];
			if (c == '\0') {
				free(buf);
				set_error(p, t.line, "unterminated quoted string");
				t.type = TOK_ERROR;
				return t;
			}
			p->pos++;
			if (c == quote) {
				break;
			}
			if (c == '\n') {
				p->line++;
			}
			if (c == '\\' && quote == '"' && s[p->pos]) {
				char e = s[p->pos++];
				c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
			}
			if (len + 2 > cap) {
				cap *= 2;
				buf = realloc(buf, cap);
			}
			buf[len++] = c;
		}
	} else {
		while ((c = s[p->pos]) && !isspace((unsigned char)c) &&
				c != ';' && c != '{' && c != '}') {
			if (len + 2 > cap) {
				cap *= 2;
				buf = realloc(buf, cap);
			}
			buf[len++] = c;
			p->pos++;
		}
	}
	buf[len] = '\0';
	t.type = TOK_WORD;
	t.text = buf;
	return t;
}

static struct token next_token(struct parser *p) {
	if (p->has_peek) {
		p->has_peek = false;
		return p->peek;
	}
	return read_token(p);
}

static void push_back(struct parser *p, struct token t) {
	p->peek = t;
	p->has_peek = true;
}

static int var_length_cmp(const void *a, const void *b) {
	const struct var *va = *(struct var **)a;
	const struct var *vb = *(struct var **)b;
	return (int)strlen(vb->name) - (int)strlen(va->name);
}

static char *expand_vars(struct parser *p, char *word) {
	if (!strchr(word, '$') || p->vars->length == 0) {
		return word;
	}
	for (int i = 0; i < p->vars->length; i++) {
		struct var *v = p->vars->items[i];
		size_t nlen = strlen(v->name);
		char *found;
		while ((found = strstr(word, v->name))) {
			char *next = format_str("%.*s%s%s", (int)(found - word), word,
				v->value, found + nlen);
			free(word);
			word = next;
			// avoid infinite loops for self-referencing values
			if (strstr(v->value, v->name)) {
				break;
			}
		}
	}
	return word;
}

static void free_node_list(list_t *list) {
	if (!list) {
		return;
	}
	for (int i = 0; i < list->length; i++) {
		twconf_free(list->items[i]);
	}
	list_free(list);
}

static bool parse_block(struct parser *p, list_t *children, bool top);

static bool parse_include(struct parser *p, list_t *children, const char *arg,
		int line) {
	if (p->include_depth > 8) {
		set_error(p, line, "include nesting too deep");
		return false;
	}
	char *expanded = tw_expand_home(arg);
	char *path = expanded;
	if (expanded[0] != '/' && p->origin && strchr(p->origin, '/')) {
		const char *slash = strrchr(p->origin, '/');
		path = format_str("%.*s/%s", (int)(slash - p->origin), p->origin, expanded);
		free(expanded);
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		char *msg = format_str("cannot include %s", path);
		set_error(p, line, msg);
		free(msg);
		free(path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = calloc(1, size + 1);
	size_t nread = fread(text, 1, size, f);
	text[nread] = '\0';
	fclose(f);

	struct parser sub = {
		.src = text,
		.line = 1,
		.origin = path,
		.vars = p->vars,
		.include_depth = p->include_depth + 1,
	};
	bool ok = parse_block(&sub, children, true);
	if (!ok) {
		p->error = sub.error;
	}
	free(text);
	free(path);
	return ok;
}

static bool parse_block(struct parser *p, list_t *children, bool top) {
	for (;;) {
		struct token t = next_token(p);
		switch (t.type) {
		case TOK_ERROR:
			return false;
		case TOK_EOF:
			if (!top) {
				set_error(p, t.line, "missing '}'");
				return false;
			}
			return true;
		case TOK_SEP:
			continue;
		case TOK_RBRACE:
			if (top) {
				set_error(p, t.line, "unexpected '}'");
				return false;
			}
			return true;
		case TOK_LBRACE:
			set_error(p, t.line, "unexpected '{'");
			return false;
		case TOK_WORD:
			break;
		}

		struct twconf_node *node = calloc(1, sizeof(*node));
		node->name = expand_vars(p, t.text);
		node->line = t.line;
		list_t *args = create_list();
		bool ok = true;
		for (;;) {
			struct token a = next_token(p);
			if (a.type == TOK_WORD) {
				// the name a "set" gives is the one word that is not looked
				// up, or "set $termtab $term --tab" would name its variable
				// after the value of $term
				bool names_a_variable = args->length == 0 &&
					strcmp(node->name, "set") == 0;
				list_add(args, names_a_variable ? a.text : expand_vars(p, a.text));
			} else if (a.type == TOK_LBRACE) {
				node->children = create_list();
				ok = parse_block(p, node->children, false);
				break;
			} else if (a.type == TOK_SEP) {
				// allow the opening brace on the following line
				struct token n;
				do {
					n = next_token(p);
				} while (n.type == TOK_SEP);
				if (n.type == TOK_LBRACE) {
					node->children = create_list();
					ok = parse_block(p, node->children, false);
				} else {
					push_back(p, n);
				}
				break;
			} else if (a.type == TOK_ERROR) {
				ok = false;
				break;
			} else {
				push_back(p, a);
				break;
			}
		}

		node->argc = args->length;
		node->argv = calloc(args->length + 1, sizeof(char *));
		for (int i = 0; i < args->length; i++) {
			node->argv[i] = args->items[i];
		}
		list_free(args);

		if (!ok) {
			twconf_free(node);
			return false;
		}

		if (strcmp(node->name, "set") == 0 && node->argc >= 2 && !node->children) {
			struct var *v = calloc(1, sizeof(*v));
			v->name = strdup(node->argv[0]);
			v->value = twconf_join(node, 1);
			for (int i = 0; i < p->vars->length; i++) {
				struct var *old = p->vars->items[i];
				if (strcmp(old->name, v->name) == 0) {
					free(old->name);
					free(old->value);
					free(old);
					list_del(p->vars, i);
					break;
				}
			}
			list_add(p->vars, v);
			list_qsort(p->vars, var_length_cmp);
			twconf_free(node);
		} else if (strcmp(node->name, "include") == 0 && node->argc == 1 &&
				!node->children) {
			ok = parse_include(p, children, node->argv[0], node->line);
			twconf_free(node);
			if (!ok) {
				return false;
			}
		} else {
			list_add(children, node);
		}
	}
}

static void free_vars(list_t *vars) {
	for (int i = 0; i < vars->length; i++) {
		struct var *v = vars->items[i];
		free(v->name);
		free(v->value);
		free(v);
	}
	list_free(vars);
}

struct twconf_node *twconf_parse_string(const char *text, const char *origin,
		char **error) {
	struct parser p = {
		.src = text,
		.line = 1,
		.origin = origin,
		.vars = create_list(),
	};
	struct twconf_node *root = calloc(1, sizeof(*root));
	root->name = strdup("");
	root->argv = calloc(1, sizeof(char *));
	root->children = create_list();
	bool ok = parse_block(&p, root->children, true);
	free_vars(p.vars);
	if (p.has_peek) {
		free(p.peek.text);
	}
	if (!ok) {
		if (error) {
			*error = p.error ? p.error : strdup("parse error");
		} else {
			free(p.error);
		}
		twconf_free(root);
		return NULL;
	}
	return root;
}

struct twconf_node *twconf_parse_file(const char *path, char **error) {
	FILE *f = fopen(path, "r");
	if (!f) {
		if (error) {
			*error = format_str("cannot open %s", path);
		}
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = calloc(1, size + 1);
	size_t nread = fread(text, 1, size, f);
	text[nread] = '\0';
	fclose(f);
	struct twconf_node *root = twconf_parse_string(text, path, error);
	free(text);
	return root;
}

void twconf_free(struct twconf_node *node) {
	if (!node) {
		return;
	}
	free(node->name);
	for (int i = 0; i < node->argc; i++) {
		free(node->argv[i]);
	}
	free(node->argv);
	free_node_list(node->children);
	free(node);
}

int twconf_count(const struct twconf_node *node) {
	return node && node->children ? node->children->length : 0;
}

struct twconf_node *twconf_at(const struct twconf_node *node, int index) {
	if (index < 0 || index >= twconf_count(node)) {
		return NULL;
	}
	return node->children->items[index];
}

struct twconf_node *twconf_child(const struct twconf_node *node, const char *name) {
	for (int i = 0; i < twconf_count(node); i++) {
		struct twconf_node *child = node->children->items[i];
		if (strcmp(child->name, name) == 0) {
			return child;
		}
	}
	return NULL;
}

const char *twconf_arg(const struct twconf_node *node, int index) {
	if (!node || index < 0 || index >= node->argc) {
		return NULL;
	}
	return node->argv[index];
}

const char *twconf_value(const struct twconf_node *node, const char *name) {
	return twconf_arg(twconf_child(node, name), 0);
}

char *twconf_join(const struct twconf_node *node, int start) {
	if (!node || start >= node->argc) {
		return strdup("");
	}
	return join_args(node->argv + start, node->argc - start);
}

bool twconf_parse_bool(const char *value, bool fallback) {
	if (!value) {
		return fallback;
	}
	if (strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0 ||
			strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0 ||
			strcasecmp(value, "enable") == 0 || strcasecmp(value, "enabled") == 0) {
		return true;
	}
	if (strcasecmp(value, "no") == 0 || strcasecmp(value, "false") == 0 ||
			strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0 ||
			strcasecmp(value, "disable") == 0 || strcasecmp(value, "disabled") == 0) {
		return false;
	}
	return fallback;
}
