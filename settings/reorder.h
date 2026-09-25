#ifndef _TW_REORDER_H
#define _TW_REORDER_H
#include <stdbool.h>
#include <glib.h>

/*
 * Moves the entry at index of from to in front of the entry at before of to,
 * as a drag and drop leaves it: before counts the entries of to as they were
 * before the move, and to->len puts it at the end. from and to may be the same
 * array. Returns false, and leaves both alone, when nothing would change or
 * an index is out of range.
 */
bool reorder_move(GPtrArray *from, guint index, GPtrArray *to, guint before);

#endif
