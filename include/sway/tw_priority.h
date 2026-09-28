#ifndef _TW_PRIORITY_H
#define _TW_PRIORITY_H
#include <stdbool.h>
#include <sys/types.h>

/*
 * The focused window first (focus_priority): the process of the focused
 * window runs at a lower nice value, the one before gets its own back.
 */

/* The window of process pid got the focus (0: none has); nice is the setting, 0 off. */
void tw_priority_focus(pid_t pid, int nice);

/* The process raised now, 0 for none. */
pid_t tw_priority_raised(void);

/* Gives the raised process its nice value back (at exit). */
void tw_priority_forget(void);

/* How it reaches the system, to be replaced in the tests. */
struct tw_priority_ops {
	int (*get_nice)(pid_t tid, int *nice);        // 0, or -1 with errno
	int (*set_nice)(pid_t tid, int nice);         // 0, or -1 with errno
	int (*threads)(pid_t pid, pid_t *out, int max); // how many, -1 when it is gone
	unsigned long long (*start_time)(pid_t pid);  // 0 when it is gone
};
void tw_priority_set_ops(const struct tw_priority_ops *ops);

#endif
