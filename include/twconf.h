#ifndef _TWCONF_H
#define _TWCONF_H
#include <stdbool.h>
#include "list.h"

/*
 * Parser for the sway-style block syntax used by tileWin themes and the
 * taskbar config:
 *
 *   name arg "quoted arg" {
 *       child arg; other_child
 *   }
 *
 * Statements end at a newline or ';'. '#' starts a comment at the start of a
 * line or when followed by whitespace (so colors like #008080 work).
 * `set $var value` defines variables and `include <path>` splices in a file.
 */
struct twconf_node {
	char *name;
	int argc;
	char **argv;
	list_t *children; // struct twconf_node *, NULL if the statement has no block
	int line;
};

struct twconf_node *twconf_parse_file(const char *path, char **error);
struct twconf_node *twconf_parse_string(const char *text, const char *origin,
		char **error);
void twconf_free(struct twconf_node *node);

int twconf_count(const struct twconf_node *node);
struct twconf_node *twconf_at(const struct twconf_node *node, int index);
/* First child with the given name, or NULL. */
struct twconf_node *twconf_child(const struct twconf_node *node, const char *name);
/* Argument at index, or NULL. */
const char *twconf_arg(const struct twconf_node *node, int index);
/* First argument of the first child with the given name, or NULL. */
const char *twconf_value(const struct twconf_node *node, const char *name);
/* Joins the arguments starting at index with spaces. Newly allocated. */
char *twconf_join(const struct twconf_node *node, int start);

bool twconf_parse_bool(const char *value, bool fallback);

#endif
