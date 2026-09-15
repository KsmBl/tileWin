#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"
#include "log.h"
#include "stringop.h"
#include "util.h"

static struct cmd_results *result_from_error(char *error) {
	struct cmd_results *res = cmd_results_new(CMD_FAILURE, "%s",
		error ? error : "Unknown error");
	free(error);
	return res;
}

static struct sway_container *target_window(void) {
	struct sway_container *con = config->handler_context.container;
	return con && con->view ? con : NULL;
}

struct cmd_results *cmd_wm_mode(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "wm_mode", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	enum tw_mode mode;
	if (strcasecmp(argv[0], "toggle") == 0) {
		mode = tw_mode == TW_MODE_WINDOW ? TW_MODE_TILE : TW_MODE_WINDOW;
	} else if (!tw_parse_mode(argv[0], &mode)) {
		return cmd_results_new(CMD_INVALID, "Expected 'wm_mode tile|window|toggle'");
	}
	char *err = NULL;
	if (!tw_request_mode(mode, &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_theme(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "theme", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	// theme <name> [tile|window]
	enum tw_mode mode = tw_mode;
	int count = argc;
	if (argc >= 2 && tw_parse_mode(argv[argc - 1], &mode)) {
		count--;
	}
	char *name = join_args(argv, count);
	char *err = NULL;
	bool ok = tw_set_mode_theme(mode, name, &err);
	free(name);
	if (!ok) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_arrange(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "arrange", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *err = NULL;
	if (!tw_arrange_workspace(config->handler_context.workspace, argv[0], &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_snap(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "snap", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *err = NULL;
	if (!tw_snap(target_window(), argv[0], &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_maximize(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "maximize", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	struct sway_container *con = target_window();
	if (!con) {
		return cmd_results_new(CMD_FAILURE, "No window to maximize");
	}
	if (!container_is_floating(con)) {
		return cmd_results_new(CMD_FAILURE,
			"maximize only works on floating windows (use fullscreen in tile mode)");
	}
	bool current = con->pending.tw_maximized;
	bool enable = argc ? parse_boolean(argv[0], current) : !current;
	tw_maximize(con, enable);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_minimize(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "minimize", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	struct sway_container *con = target_window();
	if (!con) {
		return cmd_results_new(CMD_FAILURE, "No window to minimize");
	}
	bool current = con->pending.tw_minimized;
	bool enable = argc ? parse_boolean(argv[0], current) : !current;
	tw_minimize(con, enable);
	if (!enable) {
		struct sway_seat *seat = config->handler_context.seat ?
			config->handler_context.seat : input_manager_current_seat();
		seat_set_focus_container(seat, con);
		container_raise_floating(con);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_showdesktop(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "showdesktop", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	if (tw_mode != TW_MODE_WINDOW) {
		return cmd_results_new(CMD_FAILURE, "showdesktop is only available in window mode");
	}
	char *err = NULL;
	if (!tw_show_desktop(config->handler_context.workspace, &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_alttab(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "alttab", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	struct sway_seat *seat = config->handler_context.seat ?
		config->handler_context.seat : input_manager_current_seat();
	const char *action = argc ? argv[0] : "next";
	if (strcasecmp(action, "next") == 0) {
		tw_alttab_step(seat, 1);
	} else if (strcasecmp(action, "prev") == 0) {
		tw_alttab_step(seat, -1);
	} else if (strcasecmp(action, "commit") == 0) {
		tw_alttab_commit();
	} else if (strcasecmp(action, "cancel") == 0) {
		tw_alttab_cancel();
	} else {
		return cmd_results_new(CMD_INVALID, "Expected 'alttab next|prev|commit|cancel'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_taskview(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "taskview", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	if (config->reading) {
		return cmd_results_new(CMD_FAILURE, "taskview can't be used in the config file");
	}
	struct sway_seat *seat = config->handler_context.seat ?
		config->handler_context.seat : input_manager_current_seat();
	const char *action = argc ? argv[0] : "toggle";
	if (strcasecmp(action, "toggle") == 0) {
		tw_taskview_toggle(seat);
	} else if (strcasecmp(action, "open") == 0) {
		tw_taskview_open(seat);
	} else if (strcasecmp(action, "close") == 0) {
		tw_taskview_close();
	} else {
		return cmd_results_new(CMD_INVALID, "Expected 'taskview [toggle|open|close]'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_desktop(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "desktop", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (config->reading) {
		return cmd_results_new(CMD_FAILURE, "desktop can't be used in the config file");
	}
	struct sway_seat *seat = config->handler_context.seat ?
		config->handler_context.seat : input_manager_current_seat();
	struct sway_workspace *ws = seat_get_focused_workspace(seat);
	if (strcasecmp(argv[0], "new") == 0) {
		struct sway_workspace *created = tw_desktop_new(ws ? ws->output : NULL);
		if (!created) {
			return cmd_results_new(CMD_FAILURE, "Cannot create a desktop");
		}
		workspace_switch(created);
	} else if (strcasecmp(argv[0], "close") == 0) {
		if (!ws || !tw_desktop_close(ws)) {
			return cmd_results_new(CMD_FAILURE, "The last desktop cannot be closed");
		}
	} else {
		return cmd_results_new(CMD_INVALID, "Expected 'desktop new|close'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_restart(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "restart", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	if (argc == 1 && strcasecmp(argv[0], "panel") == 0) {
		tw_panel_restart();
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	bool relaunch = argc == 1 && (strcmp(argv[0], "--relaunch-apps") == 0 ||
		strcmp(argv[0], "relaunch-apps") == 0);
	if (argc == 1 && !relaunch) {
		return cmd_results_new(CMD_INVALID, "Expected 'restart [panel|relaunch-apps]'");
	}
	char *err = NULL;
	if (!tw_restart(relaunch, &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_panel(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "panel", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *args = join_args(argv, argc);
	tw_panel_command(args);
	free(args);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_panel_command(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "panel_command", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	free(config->tw_panel_command);
	config->tw_panel_command = join_args(argv, argc);
	if (config->active && !config->reading) {
		tw_panel_config_reloaded();
		tw_panel_start();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_wallpaper(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "wallpaper", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	const char *type = argv[0];
	if (strcasecmp(type, "theme") != 0 && strcasecmp(type, "none") != 0 &&
			strcasecmp(type, "solid") != 0 && strcasecmp(type, "gradient") != 0 &&
			strcasecmp(type, "image") != 0) {
		return cmd_results_new(CMD_INVALID, "Expected 'wallpaper theme|none|"
			"solid <color>|gradient <color> <color> [vertical|horizontal]|"
			"image <path> [fill|fit|stretch|center] [background color]'");
	}
	if (strcasecmp(type, "image") == 0 && argc < 2) {
		return cmd_results_new(CMD_INVALID, "wallpaper image needs a path");
	}
	free(config->tw_wallpaper);
	config->tw_wallpaper = join_args(argv, argc);
	if (config->active && !config->reading) {
		tw_wallpaper_invalidate();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_launcher_command(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "launcher_command", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	free(config->tw_launcher_command);
	config->tw_launcher_command = join_args(argv, argc);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_color_scheme(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "color_scheme", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	bool dark;
	if (strcasecmp(argv[0], "toggle") == 0) {
		dark = !tw_color_scheme_is_dark();
	} else if (strcasecmp(argv[0], "dark") == 0) {
		dark = true;
	} else if (strcasecmp(argv[0], "light") == 0) {
		dark = false;
	} else {
		return cmd_results_new(CMD_INVALID, "Expected 'color_scheme light|dark|toggle'");
	}
	char *err = NULL;
	if (!tw_set_color_scheme(dark, &err)) {
		return result_from_error(err);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_session_restore(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "session_restore", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->tw_session_restore = parse_boolean(argv[0], config->tw_session_restore);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_launcher(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "launcher", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	const char *cmd = config->tw_launcher_command;
	if (!cmd || !*cmd || strcasecmp(cmd, "builtin") == 0) {
		json_object *data = json_object_new_object();
		json_object_object_add(data, "args", json_object_new_string("launcher"));
		ipc_event_tilewin("panel", data);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	int cmd_argc = 0;
	char **cmd_argv = split_args(cmd, &cmd_argc);
	struct cmd_results *res = cmd_exec_process(cmd_argc, cmd_argv);
	free_argv(cmd_argc, cmd_argv);
	return res;
}

struct cmd_results *cmd_idle_timeout(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "idle_timeout", EXPECTED_EQUAL_TO, 2))) {
		return error;
	}
	enum tw_idle_stage stage;
	char *end = NULL;
	long seconds = strcasecmp(argv[1], "never") == 0 ? 0 : strtol(argv[1], &end, 10);
	if (!tw_idle_stage_parse(argv[0], &stage) || (end && (end == argv[1] || *end)) ||
			seconds < 0) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'idle_timeout dim|screen_off|lock|sleep <seconds>|never'");
	}
	config->tw_idle_timeout[stage] = seconds > (1L << 30) ? (1 << 30) : (int)seconds;
	if (config->active && !config->reading) {
		tw_power_config_changed();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_lid_action(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "lid_action", EXPECTED_EQUAL_TO, 2))) {
		return error;
	}
	enum tw_lid_action action;
	int which = strcasecmp(argv[0], "closed") == 0 ? 0 :
		strcasecmp(argv[0], "docked") == 0 ? 1 : -1;
	if (which < 0 || !tw_lid_action_parse(argv[1], &action)) {
		return cmd_results_new(CMD_INVALID, "Expected 'lid_action closed|docked "
			"default|nothing|sleep|hibernate|lock|screen_off|shutdown'");
	}
	config->tw_lid_action[which] = action;
	if (config->active && !config->reading) {
		tw_power_config_changed();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_animations(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "animations", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->tw_animations = parse_boolean(argv[0], config->tw_animations);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_animation_speed(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "animation_speed", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *end = NULL;
	double speed = strtod(argv[0], &end);
	if (!end || *end || speed <= 0) {
		return cmd_results_new(CMD_INVALID,
			"animation_speed needs a factor above 0, e.g. 2 for twice as fast");
	}
	config->tw_animation_speed = speed;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_xdg_autostart(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "xdg_autostart", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->tw_xdg_autostart = parse_boolean(argv[0], config->tw_xdg_autostart);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_lock_command(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "lock_command", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	free(config->tw_lock_command);
	config->tw_lock_command = join_args(argv, argc);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
