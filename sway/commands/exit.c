#include <stddef.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/server.h"
#include "sway/tilewin.h"

struct cmd_results *cmd_exit(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "exit", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	tw_session_save_now();
	sway_terminate(0);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
