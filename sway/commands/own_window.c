/*
 * own_window <criteria>: a window that names another as its owner is still a
 * window of its own (its own button on the taskbar, not kept on top of the other,
 * opened where it opens), as on Windows the copy dialog of Explorer is.
 */
#include <string.h>
#include "sway/commands.h"
#include "sway/criteria.h"
#include "list.h"
#include "log.h"

struct cmd_results *cmd_own_window(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "own_window", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	char *err_str = NULL;
	struct criteria *criteria = criteria_parse(argv[0], &err_str);
	if (!criteria) {
		error = cmd_results_new(CMD_INVALID, "%s", err_str);
		free(err_str);
		return error;
	}


	criteria->type = CT_OWN_WINDOW;

	// Check if it already exists
	if (criteria_already_exists(criteria)) {
		sway_log(SWAY_DEBUG, "own_window already exists: '%s'", criteria->raw);
		criteria_destroy(criteria);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	list_add(config->criteria, criteria);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
