#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "html.h"

/* ---------- a growing string ---------- */

struct buf {
	char *s;
	size_t len, cap;
};

static void buf_addn(struct buf *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 64;
		while (cap < b->len + n + 1) {
			cap *= 2;
		}
		b->s = realloc(b->s, cap);
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void buf_add(struct buf *b, const char *s) {
	buf_addn(b, s, strlen(s));
}

static char *buf_take(struct buf *b) {
	char *s = b->s ? b->s : strdup("");
	b->s = NULL;
	b->len = b->cap = 0;
	return s;
}

/* ---------- entities ---------- */

static void put_utf8(struct buf *b, unsigned long c) {
	char out[4];
	size_t n;
	if (c == 0 || c > 0x10FFFF) {
		c = 0xFFFD;
	}
	if (c < 0x80) {
		out[0] = (char)c;
		n = 1;
	} else if (c < 0x800) {
		out[0] = (char)(0xC0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 0x3F));
		n = 2;
	} else if (c < 0x10000) {
		out[0] = (char)(0xE0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		out[2] = (char)(0x80 | (c & 0x3F));
		n = 3;
	} else {
		out[0] = (char)(0xF0 | (c >> 18));
		out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
		out[3] = (char)(0x80 | (c & 0x3F));
		n = 4;
	}
	buf_addn(b, out, n);
}

static const struct {
	const char *name;
	unsigned long code;
} entities[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
	{ "nbsp", ' ' }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC },
	{ "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF },
	{ "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 }, { "ccedil", 0xE7 },
	{ "ntilde", 0xF1 }, { "oacute", 0xF3 }, { "aacute", 0xE1 }, { "iacute", 0xED },
	{ "uacute", 0xFA }, { "ecirc", 0xEA }, { "copy", 0xA9 }, { "reg", 0xAE },
	{ "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D }, { "ldquo", 0x201C },
	{ "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 },
};

/* Appends text with its entities decoded. */
static void add_decoded(struct buf *b, const char *s, size_t n) {
	size_t i = 0;
	while (i < n) {
		if (s[i] != '&') {
			size_t start = i;
			while (i < n && s[i] != '&') {
				i++;
			}
			buf_addn(b, s + start, i - start);
			continue;
		}
		size_t j = i + 1;
		while (j < n && j - i < 12 && (isalnum((unsigned char)s[j]) || s[j] == '#')) {
			j++;
		}
		bool done = false;
		if (j > i + 1) {
			char name[16];
			snprintf(name, sizeof(name), "%.*s", (int)(j - i - 1), s + i + 1);
			if (name[0] == '#') {
				char *end;
				unsigned long c = (name[1] == 'x' || name[1] == 'X') ?
					strtoul(name + 2, &end, 16) : strtoul(name + 1, &end, 10);
				if (*end == '\0' && end != name + 1) {
					put_utf8(b, c);
					done = true;
				}
			} else {
				for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
					if (strcmp(entities[e].name, name) == 0) {
						put_utf8(b, entities[e].code);
						done = true;
						break;
					}
				}
			}
		}
		if (done) {
			i = j < n && s[j] == ';' ? j + 1 : j;
		} else {
			buf_addn(b, "&", 1);
			i++;
		}
	}
}

static char *decoded(const char *s, size_t n) {
	struct buf b = { 0 };
	add_decoded(&b, s, n);
	return buf_take(&b);
}

char *html_fold(const char *text) {
	struct buf b = { 0 };
	bool space = true;
	for (const char *p = text ? text : ""; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (isspace(c)) {
			if (!space) {
				buf_addn(&b, " ", 1);
				space = true;
			}
			continue;
		}
		char lower = (char)(c < 0x80 ? tolower(c) : c);
		buf_addn(&b, &lower, 1);
		space = false;
	}
	char *s = buf_take(&b);
	size_t len = strlen(s);
	if (len && s[len - 1] == ' ') {
		s[len - 1] = '\0';
	}
	return s;
}

/* ---------- tags ---------- */

#define MAX_ATTRS 32

struct tag {
	char name[16];
	bool end, self_closing;
	int nattrs;
	char *attr_name[MAX_ATTRS], *attr_value[MAX_ATTRS];
};

static void tag_clear(struct tag *t) {
	for (int i = 0; i < t->nattrs; i++) {
		free(t->attr_name[i]);
		free(t->attr_value[i]);
	}
	t->nattrs = 0;
}

static const char *attr(const struct tag *t, const char *name) {
	for (int i = 0; i < t->nattrs; i++) {
		if (strcmp(t->attr_name[i], name) == 0) {
			return t->attr_value[i];
		}
	}
	return NULL;
}

static char *attr_dup(const struct tag *t, const char *name) {
	const char *v = attr(t, name);
	return v ? strdup(v) : NULL;
}

/* Reads a tag starting at '<'; returns where it ends, or NULL if it is no tag. */
static const char *read_tag(const char *p, struct tag *t) {
	memset(t, 0, sizeof(*t));
	p++;
	if (*p == '/') {
		t->end = true;
		p++;
	}
	if (!isalpha((unsigned char)*p)) {
		return NULL;
	}
	size_t n = 0;
	while (isalnum((unsigned char)*p) || *p == '-' || *p == ':') {
		if (n < sizeof(t->name) - 1) {
			t->name[n++] = (char)tolower((unsigned char)*p);
		}
		p++;
	}
	while (*p && *p != '>') {
		if (isspace((unsigned char)*p)) {
			p++;
			continue;
		}
		if (*p == '/') {
			t->self_closing = p[1] == '>';
			p++;
			continue;
		}
		const char *ns = p;
		while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' &&
				!(*p == '/' && p[1] == '>')) {
			p++;
		}
		if (p == ns) {
			p++;
			continue;
		}
		size_t nlen = (size_t)(p - ns);
		while (isspace((unsigned char)*p)) {
			p++;
		}
		const char *vs = NULL;
		size_t vlen = 0;
		if (*p == '=') {
			p++;
			while (isspace((unsigned char)*p)) {
				p++;
			}
			if (*p == '"' || *p == '\'') {
				char q = *p++;
				vs = p;
				while (*p && *p != q) {
					p++;
				}
				vlen = (size_t)(p - vs);
				if (*p) {
					p++;
				}
			} else {
				vs = p;
				while (*p && !isspace((unsigned char)*p) && *p != '>') {
					p++;
				}
				vlen = (size_t)(p - vs);
			}
		}
		if (t->nattrs < MAX_ATTRS) {
			char *name = strndup(ns, nlen);
			for (char *c = name; *c; c++) {
				*c = (char)tolower((unsigned char)*c);
			}
			t->attr_name[t->nattrs] = name;
			t->attr_value[t->nattrs] = vs ? decoded(vs, vlen) : strdup("");
			t->nattrs++;
		}
	}
	return *p == '>' ? p + 1 : p;
}

/* ---------- the page ---------- */

struct parser {
	struct html_page *page;
	struct html_form *form;       // the form being read, NULL outside one
	struct html_form loose;       // fields outside any form
	struct buf text, form_text;
	// text of what is open now: a button, a link, a label, an option, a title
	struct buf button_text, link_text, label_text, option_text, title_text;
	struct html_field *button;    // index-free: re-found by position below
	int button_form, button_index;
	bool in_button, in_link, in_label, in_option, in_select, in_title;
	char *link_href;
	char *label_for;
	int label_field_form, label_field_index; // a field inside the label
	int select_form, select_index;
	char *option_value;
	bool option_selected, select_has_value;
};

static struct html_form *form_at(struct parser *ps, int index) {
	return index < 0 ? &ps->loose : &ps->page->forms[index];
}

static int current_form_index(struct parser *ps) {
	return ps->form ? (int)(ps->form - ps->page->forms) : -1;
}

static struct html_field *add_field(struct parser *ps, int *index) {
	struct html_form *f = ps->form ? ps->form : &ps->loose;
	f->fields = realloc(f->fields, sizeof(*f->fields) * (size_t)(f->nfields + 1));
	struct html_field *field = &f->fields[f->nfields];
	memset(field, 0, sizeof(*field));
	*index = f->nfields++;
	return field;
}

static void add_text(struct parser *ps, const char *s, size_t n) {
	struct buf tmp = { 0 };
	add_decoded(&tmp, s, n);
	if (!tmp.s) {
		return;
	}
	buf_add(&ps->text, tmp.s);
	buf_add(&ps->text, " ");
	if (ps->form) {
		buf_add(&ps->form_text, tmp.s);
		buf_add(&ps->form_text, " ");
	}
	if (ps->in_button) {
		buf_add(&ps->button_text, tmp.s);
	}
	if (ps->in_link) {
		buf_add(&ps->link_text, tmp.s);
	}
	if (ps->in_label) {
		buf_add(&ps->label_text, tmp.s);
		buf_add(&ps->label_text, " ");
	}
	if (ps->in_option) {
		buf_add(&ps->option_text, tmp.s);
	}
	if (ps->in_title) {
		buf_add(&ps->title_text, tmp.s);
	}
	free(tmp.s);
}

static bool style_hides(const char *style) {
	if (!style) {
		return false;
	}
	char *folded = html_fold(style);
	char *squeezed = folded;
	char *w = folded;
	for (char *r = folded; *r; r++) {
		if (*r != ' ') {
			*w++ = *r;
		}
	}
	*w = '\0';
	bool hides = strstr(squeezed, "display:none") || strstr(squeezed, "visibility:hidden");
	free(folded);
	return hides;
}

static enum html_field_kind input_kind(const char *type) {
	static const char *const text[] = { "", "text", "email", "tel", "number", "password",
		"url", "search", "date", "datetime-local", "month", "week", "time" };
	for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
		if (strcmp(type, text[i]) == 0) {
			return HTML_TEXT;
		}
	}
	if (strcmp(type, "hidden") == 0) {
		return HTML_HIDDEN;
	}
	if (strcmp(type, "checkbox") == 0) {
		return HTML_CHECKBOX;
	}
	if (strcmp(type, "radio") == 0) {
		return HTML_RADIO;
	}
	if (strcmp(type, "submit") == 0) {
		return HTML_SUBMIT;
	}
	if (strcmp(type, "image") == 0) {
		return HTML_IMAGE;
	}
	if (strcmp(type, "button") == 0 || strcmp(type, "reset") == 0) {
		return HTML_BUTTON;
	}
	return HTML_OTHER;
}

static void fill_common(struct html_field *f, const struct tag *t) {
	const char *type = attr(t, "type");
	f->type = strdup(type ? type : "");
	for (char *c = f->type; *c; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	f->name = attr_dup(t, "name");
	f->id = attr_dup(t, "id");
	f->formaction = attr_dup(t, "formaction");
	f->formmethod = attr_dup(t, "formmethod");
	f->required = attr(t, "required") != NULL || (attr(t, "aria-required") &&
		strcasecmp(attr(t, "aria-required"), "true") == 0);
	f->disabled = attr(t, "disabled") != NULL;
	f->invisible = attr(t, "hidden") != NULL || style_hides(attr(t, "style"));
	const char *placeholder = attr(t, "placeholder");
	const char *aria = attr(t, "aria-label");
	const char *title = attr(t, "title");
	f->label = strdup(aria ? aria : placeholder ? placeholder : title ? title : "");
}

static void end_label(struct parser *ps) {
	if (!ps->in_label) {
		return;
	}
	ps->in_label = false;
	char *text = buf_take(&ps->label_text);
	struct html_field *target = NULL;
	if (ps->label_field_index >= 0) {
		struct html_form *f = form_at(ps, ps->label_field_form);
		if (ps->label_field_index < f->nfields) {
			target = &f->fields[ps->label_field_index];
		}
	}
	if (target && !target->label[0]) {
		free(target->label);
		target->label = strdup(text);
		free(text);
	} else if (ps->label_for) {
		// <label for=id>: given to the field once the page is read
		struct html_field *field;
		int index;
		struct html_form *keep = ps->form;
		ps->form = NULL;
		field = add_field(ps, &index); // a placeholder in the loose list
		ps->form = keep;
		field->kind = HTML_OTHER;
		field->type = strdup("label");
		field->id = strdup(ps->label_for);
		field->label = text;
		field->invisible = true;
	} else {
		free(text);
	}
	free(ps->label_for);
	ps->label_for = NULL;
}

static void end_button(struct parser *ps) {
	if (!ps->in_button) {
		return;
	}
	ps->in_button = false;
	char *text = buf_take(&ps->button_text);
	struct html_form *f = form_at(ps, ps->button_form);
	if (ps->button_index < f->nfields) {
		struct html_field *b = &f->fields[ps->button_index];
		char *folded = html_fold(text);
		if (folded[0]) {
			free(b->label);
			b->label = folded;
		} else {
			free(folded);
		}
	}
	free(text);
}

static void end_option(struct parser *ps) {
	if (!ps->in_option) {
		return;
	}
	ps->in_option = false;
	char *text = buf_take(&ps->option_text);
	struct html_form *f = form_at(ps, ps->select_form);
	if (ps->in_select && ps->select_index < f->nfields) {
		struct html_field *s = &f->fields[ps->select_index];
		char *value = ps->option_value ? ps->option_value : html_fold(text);
		ps->option_value = NULL;
		if (!ps->select_has_value || ps->option_selected) {
			free(s->value);
			s->value = value;
			ps->select_has_value = true;
		} else {
			free(value);
		}
	}
	free(ps->option_value);
	ps->option_value = NULL;
	free(text);
}

static void end_link(struct parser *ps) {
	if (!ps->in_link) {
		return;
	}
	ps->in_link = false;
	struct html_page *pg = ps->page;
	pg->links = realloc(pg->links, sizeof(*pg->links) * (size_t)(pg->nlinks + 1));
	char *text = buf_take(&ps->link_text);
	pg->links[pg->nlinks].href = ps->link_href;
	pg->links[pg->nlinks].text = html_fold(text);
	pg->nlinks++;
	ps->link_href = NULL;
	free(text);
}

static void end_form(struct parser *ps) {
	if (!ps->form) {
		return;
	}
	end_button(ps);
	char *text = buf_take(&ps->form_text);
	ps->form->text = html_fold(text);
	free(text);
	ps->form = NULL;
}

/* The URL of a meta refresh: "5; url=/login". */
static char *refresh_url(const char *content) {
	const char *p = content;
	while (*p && *p != ';' && *p != ',') {
		p++;
	}
	if (!*p) {
		return NULL;
	}
	p++;
	while (isspace((unsigned char)*p)) {
		p++;
	}
	if (strncasecmp(p, "url", 3) == 0) {
		const char *q = p + 3;
		while (isspace((unsigned char)*q)) {
			q++;
		}
		if (*q == '=') {
			p = q + 1;
		}
	}
	while (isspace((unsigned char)*p) || *p == '\'' || *p == '"') {
		p++;
	}
	size_t n = strlen(p);
	while (n && (isspace((unsigned char)p[n - 1]) || p[n - 1] == '\'' || p[n - 1] == '"')) {
		n--;
	}
	return n ? strndup(p, n) : NULL;
}

/*
 * Where a script sends the page: location = "...", location.href = '...',
 * location.replace("..."), location.assign(...), of window, document, top or
 * self. Only plain strings count.
 */
static char *script_redirect(const char *s, size_t n) {
	const char *end = s + n;
	for (const char *p = s; p + 8 <= end; p++) {
		if (strncmp(p, "location", 8) != 0) {
			continue;
		}
		if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
			continue;
		}
		const char *q = p + 8;
		if (q + 5 <= end && strncmp(q, ".href", 5) == 0) {
			q += 5;
		}
		while (q < end && isspace((unsigned char)*q)) {
			q++;
		}
		if (q < end && *q == '=' && (q + 1 >= end || q[1] != '=')) {
			q++;
		} else if (q + 9 <= end && strncmp(q, ".replace(", 9) == 0) {
			q += 9;
		} else if (q + 8 <= end && strncmp(q, ".assign(", 8) == 0) {
			q += 8;
		} else {
			continue;
		}
		while (q < end && isspace((unsigned char)*q)) {
			q++;
		}
		if (q >= end || (*q != '"' && *q != '\'')) {
			continue;
		}
		char quote = *q++;
		const char *u = q;
		while (q < end && *q != quote && *q != '\n') {
			q++;
		}
		if (q < end && *q == quote && q > u) {
			return strndup(u, (size_t)(q - u));
		}
	}
	return NULL;
}

static void start_tag(struct parser *ps, const struct tag *t) {
	const char *n = t->name;
	if (strcmp(n, "form") == 0) {
		end_form(ps);
		struct html_page *pg = ps->page;
		pg->forms = realloc(pg->forms, sizeof(*pg->forms) * (size_t)(pg->nforms + 1));
		ps->form = &pg->forms[pg->nforms++];
		memset(ps->form, 0, sizeof(*ps->form));
		ps->form->action = attr_dup(t, "action");
		const char *method = attr(t, "method");
		ps->form->method = strdup(method && strcasecmp(method, "post") == 0 ? "POST" : "GET");
		ps->form->enctype = attr_dup(t, "enctype");
		ps->form->id = attr_dup(t, "id");
	} else if (strcmp(n, "input") == 0) {
		int index;
		struct html_field *f = add_field(ps, &index);
		fill_common(f, t);
		f->kind = input_kind(f->type);
		const char *value = attr(t, "value");
		if (f->kind == HTML_CHECKBOX || f->kind == HTML_RADIO) {
			f->value = strdup(value ? value : "on");
		} else {
			f->value = strdup(value ? value : "");
		}
		f->checked = attr(t, "checked") != NULL;
		if ((f->kind == HTML_SUBMIT || f->kind == HTML_BUTTON) && value) {
			char *folded = html_fold(value);
			if (folded[0]) {
				free(f->label);
				f->label = folded;
			} else {
				free(folded);
			}
		} else if (f->kind == HTML_IMAGE && attr(t, "alt") && !f->label[0]) {
			free(f->label);
			f->label = html_fold(attr(t, "alt"));
		}
		if (ps->in_label && ps->label_field_index < 0) {
			ps->label_field_form = current_form_index(ps);
			ps->label_field_index = index;
		}
	} else if (strcmp(n, "button") == 0) {
		end_button(ps);
		int index;
		struct html_field *f = add_field(ps, &index);
		fill_common(f, t);
		const char *type = f->type;
		f->kind = (!type[0] || strcmp(type, "submit") == 0) ? HTML_SUBMIT : HTML_BUTTON;
		f->value = strdup(attr(t, "value") ? attr(t, "value") : "");
		ps->in_button = true;
		ps->button_form = current_form_index(ps);
		ps->button_index = index;
	} else if (strcmp(n, "select") == 0) {
		int index;
		struct html_field *f = add_field(ps, &index);
		fill_common(f, t);
		f->kind = HTML_SELECT;
		f->value = strdup("");
		ps->in_select = true;
		ps->select_has_value = false;
		ps->select_form = current_form_index(ps);
		ps->select_index = index;
	} else if (strcmp(n, "option") == 0 && ps->in_select) {
		end_option(ps);
		ps->in_option = true;
		free(ps->option_value);
		ps->option_value = attr_dup(t, "value");
		ps->option_selected = attr(t, "selected") != NULL;
	} else if (strcmp(n, "a") == 0) {
		end_link(ps);
		const char *href = attr(t, "href");
		if (href) {
			ps->in_link = true;
			ps->link_href = strdup(href);
			const char *aria = attr(t, "aria-label");
			if (aria) {
				buf_add(&ps->link_text, aria);
				buf_add(&ps->link_text, " ");
			}
		}
	} else if (strcmp(n, "label") == 0) {
		end_label(ps);
		ps->in_label = true;
		ps->label_for = attr_dup(t, "for");
		ps->label_field_index = -1;
	} else if (strcmp(n, "meta") == 0) {
		const char *equiv = attr(t, "http-equiv");
		const char *content = attr(t, "content");
		if (equiv && content && strcasecmp(equiv, "refresh") == 0 && !ps->page->refresh) {
			ps->page->refresh = refresh_url(content);
		}
	} else if (strcmp(n, "title") == 0) {
		ps->in_title = true;
	} else if (strcmp(n, "img") == 0) {
		const char *alt = attr(t, "alt");
		if (alt && (ps->in_button || ps->in_link)) {
			add_text(ps, alt, strlen(alt));
		}
	}
}

static void end_tag(struct parser *ps, const char *n) {
	if (strcmp(n, "form") == 0) {
		end_form(ps);
	} else if (strcmp(n, "button") == 0) {
		end_button(ps);
	} else if (strcmp(n, "a") == 0) {
		end_link(ps);
	} else if (strcmp(n, "label") == 0) {
		end_label(ps);
	} else if (strcmp(n, "option") == 0) {
		end_option(ps);
	} else if (strcmp(n, "select") == 0) {
		end_option(ps);
		ps->in_select = false;
	} else if (strcmp(n, "title") == 0) {
		ps->in_title = false;
	}
}

/* Labels with for= give their text to the field with that id. */
static void join_labels(struct parser *ps) {
	struct html_form *loose = &ps->loose;
	for (int l = 0; l < loose->nfields; l++) {
		struct html_field *label = &loose->fields[l];
		if (!label->type || strcmp(label->type, "label") != 0) {
			continue;
		}
		for (int fi = -1; fi < ps->page->nforms; fi++) {
			struct html_form *f = form_at(ps, fi);
			for (int i = 0; i < f->nfields; i++) {
				struct html_field *field = &f->fields[i];
				if (field != label && field->id && strcmp(field->id, label->id) == 0 &&
						(!field->label[0] || field->kind == HTML_CHECKBOX ||
						field->kind == HTML_RADIO)) {
					free(field->label);
					field->label = html_fold(label->label);
				}
			}
		}
	}
}

static void free_field(struct html_field *f) {
	free(f->type);
	free(f->name);
	free(f->value);
	free(f->id);
	free(f->label);
	free(f->formaction);
	free(f->formmethod);
}

static void free_form(struct html_form *f) {
	for (int i = 0; i < f->nfields; i++) {
		free_field(&f->fields[i]);
	}
	free(f->fields);
	free(f->action);
	free(f->method);
	free(f->enctype);
	free(f->id);
	free(f->text);
}

/*
 * Fields outside any form that a script would send (inputs and buttons next
 * to each other, no <form>): they become a form of their own at the end, sent
 * to the page itself, when there are any buttons or checkboxes among them.
 */
static void loose_form(struct parser *ps) {
	struct html_form *loose = &ps->loose;
	bool useful = false;
	for (int i = 0; i < loose->nfields; i++) {
		enum html_field_kind k = loose->fields[i].kind;
		bool is_label = loose->fields[i].type && strcmp(loose->fields[i].type, "label") == 0;
		useful |= !is_label && (k == HTML_SUBMIT || k == HTML_CHECKBOX || k == HTML_IMAGE);
	}
	if (!useful) {
		return;
	}
	struct html_page *pg = ps->page;
	pg->forms = realloc(pg->forms, sizeof(*pg->forms) * (size_t)(pg->nforms + 1));
	struct html_form *f = &pg->forms[pg->nforms++];
	memset(f, 0, sizeof(*f));
	f->method = strdup("GET");
	f->id = strdup("tilewin-loose");
	for (int i = 0; i < loose->nfields; i++) {
		struct html_field *src = &loose->fields[i];
		if (src->type && strcmp(src->type, "label") == 0) {
			continue;
		}
		f->fields = realloc(f->fields, sizeof(*f->fields) * (size_t)(f->nfields + 1));
		f->fields[f->nfields] = *src;
		f->fields[f->nfields].type = strdup(src->type ? src->type : "");
		f->fields[f->nfields].name = src->name ? strdup(src->name) : NULL;
		f->fields[f->nfields].value = src->value ? strdup(src->value) : NULL;
		f->fields[f->nfields].id = src->id ? strdup(src->id) : NULL;
		f->fields[f->nfields].label = strdup(src->label ? src->label : "");
		f->fields[f->nfields].formaction = src->formaction ? strdup(src->formaction) : NULL;
		f->fields[f->nfields].formmethod = src->formmethod ? strdup(src->formmethod) : NULL;
		f->nfields++;
	}
	f->text = html_fold(ps->text.s);
}

struct html_page *html_parse(const char *html) {
	struct html_page *page = calloc(1, sizeof(*page));
	struct parser ps = { .page = page, .label_field_index = -1 };
	const char *p = html ? html : "";
	while (*p) {
		if (*p != '<') {
			const char *start = p;
			while (*p && *p != '<') {
				p++;
			}
			add_text(&ps, start, (size_t)(p - start));
			continue;
		}
		if (strncmp(p, "<!--", 4) == 0) {
			const char *end = strstr(p + 4, "-->");
			p = end ? end + 3 : p + strlen(p);
			continue;
		}
		if (p[1] == '!' || p[1] == '?') {
			const char *end = strchr(p, '>');
			p = end ? end + 1 : p + strlen(p);
			continue;
		}
		struct tag t;
		const char *after = read_tag(p, &t);
		if (!after) {
			add_text(&ps, p, 1);
			p++;
			continue;
		}
		p = after;
		if (t.end) {
			end_tag(&ps, t.name);
			tag_clear(&t);
			continue;
		}
		start_tag(&ps, &t);
		// the raw text of these runs to their end tag
		bool script = strcmp(t.name, "script") == 0;
		bool raw = script || strcmp(t.name, "style") == 0 || strcmp(t.name, "textarea") == 0 ||
			strcmp(t.name, "noscript") == 0;
		if (raw && !t.self_closing) {
			char close[24];
			snprintf(close, sizeof(close), "</%s", t.name);
			const char *end = strcasestr(p, close);
			size_t n = end ? (size_t)(end - p) : strlen(p);
			if (script && !page->refresh && !attr(&t, "src")) {
				page->refresh = script_redirect(p, n);
			} else if (strcmp(t.name, "textarea") == 0) {
				int index;
				struct html_field *f = add_field(&ps, &index);
				fill_common(f, &t);
				f->kind = HTML_TEXTAREA;
				f->value = decoded(p, n);
			}
			p += n;
			if (end) {
				const char *gt = strchr(p, '>');
				p = gt ? gt + 1 : p + strlen(p);
			}
		}
		tag_clear(&t);
	}
	end_button(&ps);
	end_link(&ps);
	end_label(&ps);
	end_option(&ps);
	end_form(&ps);
	join_labels(&ps);
	loose_form(&ps);
	char *title = buf_take(&ps.title_text);
	page->title = html_fold(title);
	free(title);
	char *text = buf_take(&ps.text);
	page->text = html_fold(text);
	free(text);
	free_form(&ps.loose);
	free(ps.form_text.s);
	free(ps.button_text.s);
	free(ps.link_text.s);
	free(ps.label_text.s);
	free(ps.option_text.s);
	free(ps.link_href);
	free(ps.label_for);
	free(ps.option_value);
	return page;
}

void html_free(struct html_page *page) {
	if (!page) {
		return;
	}
	for (int i = 0; i < page->nforms; i++) {
		free_form(&page->forms[i]);
	}
	free(page->forms);
	for (int i = 0; i < page->nlinks; i++) {
		free(page->links[i].href);
		free(page->links[i].text);
	}
	free(page->links);
	free(page->refresh);
	free(page->title);
	free(page->text);
	free(page);
}
