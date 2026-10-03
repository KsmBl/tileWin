#include <json.h>
#include <string.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include "log.h"
#include "sway/ipc-server.h"
#include "sway/tilewin.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/commands.h"

static void close_container_iterator(struct sway_container *con, void *data) {
	if (con->view) {
		view_close(con->view);
	}
}

struct cmd_results *cmd_kill(int argc, char **argv) {
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID,
				"Can't run this command while there's no outputs connected.");
	}
	struct sway_container *con = config->handler_context.container;
	struct sway_workspace *ws = config->handler_context.workspace;

	// Window mode, Alt+F4 with the desktop in front (its own surface has the
	// keyboard, or no window has the focus): as on Windows, the shut down
	// dialog, not every window of the desktop closed at once
	struct sway_seat *seat = config->handler_context.seat;
	struct wlr_layer_surface_v1 *layer = seat ? seat->focused_layer : NULL;
	bool desktop = layer && layer->namespace &&
		strncmp(layer->namespace, "tilewin-desktop", 15) == 0;
	if (tw_is_window_mode() && argc == 0 && !config->handler_context.node_overridden &&
			(desktop || (!con && !layer))) {
		json_object *data = json_object_new_object();
		json_object_object_add(data, "args", json_object_new_string("shutdown"));
		ipc_event_tilewin("panel", data);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	if (con) {
		close_container_iterator(con, NULL);
		container_for_each_child(con, close_container_iterator, NULL);
	} else {
		workspace_for_each_container(ws, close_container_iterator, NULL);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
