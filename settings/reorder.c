#include "reorder.h"

bool reorder_move(GPtrArray *from, guint index, GPtrArray *to, guint before) {
	if (index >= from->len || before > to->len) {
		return false;
	}
	if (from == to) {
		// dropping it onto either side of itself leaves it where it is
		if (before == index || before == index + 1) {
			return false;
		}
		if (before > index) {
			before--; // taking it out moves everything behind it up by one
		}
	}
	// stealing keeps the free function from running on the entry that moves
	gpointer item = g_ptr_array_steal_index(from, index);
	g_ptr_array_insert(to, before, item);
	return true;
}
