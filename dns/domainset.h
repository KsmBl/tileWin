#ifndef _TW_DNS_DOMAINSET_H
#define _TW_DNS_DOMAINSET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A set of names that holds a block list of a million names in little room:
 * the names one after another in one block of memory, and a table of where
 * each starts. Names can only be added; the set is built anew on a change.
 */
struct domainset {
	char *arena;
	size_t used, size;
	uint32_t *slots; // offset + 1 into the arena, 0 when free
	size_t nslots, count;
};

void domainset_init(struct domainset *set);
void domainset_finish(struct domainset *set);
/* False when it was there already (or memory ran out). */
bool domainset_add(struct domainset *set, const char *name);
bool domainset_has(const struct domainset *set, const char *name);

/*
 * The block lists: each name is kept with a mark in front, '=' for the name
 * itself (hosts files and plain lists) and '*' for it and all below it
 * (adblock "||name^" lines and the names blocked by hand).
 */
bool domainset_add_rule(struct domainset *set, const char *name, bool below_too);
/* Whether name or one above it is in the set by a rule that covers it. */
bool domainset_matches(const struct domainset *set, const char *name);
/*
 * Adds the names of one line of a block list: a hosts file line
 * ("0.0.0.0 ads.example.com"), a plain name, or an adblock rule
 * ("||ads.example.com^"). Comments and everything else are skipped.
 * Returns how many names it added.
 */
int domainset_add_list_line(struct domainset *set, char *line);

#endif
