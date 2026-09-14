#ifndef _TW_CONFDOC_H
#define _TW_CONFDOC_H
#include <glib.h>
#include <stdbool.h>

/*
 * Lossless editor for tileWin's sway-style config files. Statements are
 * parsed together with their source offsets, so edits only rewrite the
 * statements that changed and comments and formatting elsewhere survive.
 *
 * Every modification re-parses the document, which invalidates all
 * struct cstmt pointers obtained before it.
 *
 * Generated statement text passed to the editing functions uses one leading
 * tab per nesting level; it is converted to the file's own indentation.
 */
struct cstmt {
	char *name;
	GPtrArray *args;     // char *, unquoted
	GPtrArray *children; // struct cstmt *, NULL if the statement has no block
	struct cstmt *parent;
	size_t start, end;   // [start, end) covers the statement including its block
	size_t name_end;     // offset just after the name
	size_t close;        // offset of the closing '}' (blocks and the root)
	int depth;           // 0 for top-level statements, -1 for the root
};

struct confdoc {
	char *path;
	GString *text;
	struct cstmt *root;
	char *indent; // one indentation level as used by the file
	char *error;  // first parse error, NULL if the file is valid
};

/* Opens <config dir>/<filename>, seeded from the installed default if missing. */
struct confdoc *confdoc_open(const char *filename);
void confdoc_free(struct confdoc *doc);
bool confdoc_save(struct confdoc *doc, char **error);
/* Replaces the whole text, e.g. after the file changed on disk. */
void confdoc_set_text(struct confdoc *doc, const char *text);

int cstmt_argc(const struct cstmt *s);
const char *cstmt_arg(const struct cstmt *s, int index);
/* Arguments from index joined with spaces. Newly allocated. */
char *cstmt_join(const struct cstmt *s, int index);
/* Source text after the statement name (sway syntax values). Newly allocated. */
char *cstmt_raw_args(const struct confdoc *doc, const struct cstmt *s);

/* First child with the name and, if arg0 is not NULL, that first argument. */
struct cstmt *confdoc_child(struct cstmt *parent, const char *name, const char *arg0);
/* Top-level block `name arg0 { }`, created at the end of the file if asked. */
struct cstmt *confdoc_block(struct confdoc *doc, const char *name, const char *arg0,
		bool create);

/*
 * Sets statements `name <args>` in parent: the first matching statement is
 * replaced with one line per entry of lines (each already quoted, without the
 * name), the other matches are removed. NULL or an empty array removes all.
 */
void confdoc_set_list(struct confdoc *doc, struct cstmt *parent, const char *name,
		const char *arg0, GPtrArray *lines);
/* Single statement version of confdoc_set_list. args NULL removes it. */
void confdoc_set(struct confdoc *doc, struct cstmt *parent, const char *name,
		const char *arg0, const char *args);
void confdoc_remove(struct confdoc *doc, struct cstmt *stmt);
/* Replaces a whole statement (including its block) with text. */
void confdoc_replace(struct confdoc *doc, struct cstmt *stmt, const char *text);
/* Appends a statement at the end of parent. */
void confdoc_append(struct confdoc *doc, struct cstmt *parent, const char *text);

/* Quotes a word for the tileWin config syntax if needed. Newly allocated. */
char *conf_quote(const char *word);
/* Quotes a command: bare words when that parses back identically. */
char *conf_quote_command(const char *command);

#endif
