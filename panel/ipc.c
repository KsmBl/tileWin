#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "ipc-client.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"

static void free_window(struct pwindow *w) {
	free(w->app_id);
	free(w->title);
	free(w->workspace);
	free(w->output);
	free(w->identifier);
	free(w);
}

static void free_workspace(struct pworkspace *ws) {
	free(ws->name);
	free(ws->output);
	free(ws);
}

static const char *jstr(json_object *obj, const char *key) {
	json_object *v;
	if (obj && json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)) {
		return json_object_get_string(v);
	}
	return NULL;
}

static bool jbool(json_object *obj, const char *key) {
	json_object *v;
	return obj && json_object_object_get_ex(obj, key, &v) && json_object_get_boolean(v);
}

static int64_t jint(json_object *obj, const char *key) {
	json_object *v;
	return obj && json_object_object_get_ex(obj, key, &v) ? json_object_get_int64(v) : 0;
}

struct pwindow *panel_find_window(struct panel *panel, int64_t id) {
	list_t *windows = panel->state.windows;
	for (int i = 0; windows && i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (w->id == id) {
			return w;
		}
	}
	return NULL;
}

static void update_window_from_json(struct pwindow *w, json_object *con) {
	const char *title = jstr(con, "name");
	free(w->title);
	w->title = strdup(title ? title : "");
	const char *app_id = jstr(con, "app_id");
	if (!app_id) {
		json_object *props;
		if (json_object_object_get_ex(con, "window_properties", &props)) {
			app_id = jstr(props, "class");
		}
	}
	free(w->app_id);
	w->app_id = strdup(app_id ? app_id : "");
	w->focused = jbool(con, "focused");
	w->urgent = jbool(con, "urgent");
	w->minimized = jbool(con, "minimized");
	w->maximized = jbool(con, "maximized");
	w->above = jbool(con, "above");
	w->pid = (int)jint(con, "pid");
	const char *identifier = jstr(con, "foreign_toplevel_identifier");
	free(w->identifier);
	w->identifier = identifier ? strdup(identifier) : NULL;
	const char *type = jstr(con, "type");
	w->floating = type && strcmp(type, "floating_con") == 0;
}

struct walk_ctx {
	list_t *windows;
	const char *output;
	const char *workspace;
	int order;
};

static void walk_tree(json_object *node, struct walk_ctx *ctx) {
	const char *type = jstr(node, "type");
	const char *name = jstr(node, "name");
	const char *saved_output = ctx->output, *saved_ws = ctx->workspace;
	if (type && strcmp(type, "output") == 0) {
		ctx->output = name;
	} else if (type && strcmp(type, "workspace") == 0) {
		ctx->workspace = name;
	}
	json_object *pid;
	bool is_view = json_object_object_get_ex(node, "pid", &pid) &&
		(type && (strcmp(type, "con") == 0 || strcmp(type, "floating_con") == 0));
	if (is_view && ctx->workspace && strcmp(ctx->workspace, "__i3_scratch") != 0) {
		struct pwindow *w = calloc(1, sizeof(*w));
		w->id = jint(node, "id");
		update_window_from_json(w, node);
		w->workspace = strdup(ctx->workspace);
		w->output = strdup(ctx->output ? ctx->output : "");
		w->order = ctx->order++;
		list_add(ctx->windows, w);
	}
	const char *children[] = { "nodes", "floating_nodes" };
	for (size_t c = 0; c < 2; c++) {
		json_object *arr;
		if (json_object_object_get_ex(node, children[c], &arr)) {
			size_t len = json_object_array_length(arr);
			for (size_t i = 0; i < len; i++) {
				walk_tree(json_object_array_get_idx(arr, i), ctx);
			}
		}
	}
	ctx->output = saved_output;
	ctx->workspace = saved_ws;
}

static json_object *request(struct panel *panel, uint32_t type, const char *payload) {
	uint32_t len = payload ? strlen(payload) : 0;
	char *resp = ipc_single_command(panel->ipc_cmd_fd, type, payload ? payload : "", &len);
	if (!resp) {
		return NULL;
	}
	json_object *obj = json_tokener_parse(resp);
	free(resp);
	return obj;
}

static void notify_widgets(struct panel *panel) {
	if (!panel->config) {
		return;
	}
	for (int i = 0; i < panel->config->widgets->length; i++) {
		struct widget *w = panel->config->widgets->items[i];
		if (w->impl->state_changed) {
			w->impl->state_changed(w);
		}
	}
	panel_set_dirty(panel);
}

void ipc_panel_refresh_tree(struct panel *panel) {
	json_object *tree = request(panel, IPC_GET_TREE, NULL);
	if (!tree) {
		return;
	}
	list_t *windows = create_list();
	struct walk_ctx ctx = { .windows = windows };
	walk_tree(tree, &ctx);
	json_object_put(tree);

	// keep the previous ordering so taskbar buttons don't jump around
	list_t *old = panel->state.windows;
	for (int i = 0; i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		w->order = 1000000 + i;
		for (int j = 0; old && j < old->length; j++) {
			struct pwindow *o = old->items[j];
			if (o->id == w->id) {
				w->order = o->order;
				break;
			}
		}
	}
	int next_order = 0;
	for (int j = 0; old && j < old->length; j++) {
		struct pwindow *o = old->items[j];
		if (o->order >= next_order) {
			next_order = o->order + 1;
		}
	}
	for (int i = 0; i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (w->order >= 1000000) {
			w->order = next_order++;
		}
	}
	// stable insertion sort by order
	for (int i = 1; i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		int j = i - 1;
		while (j >= 0 && ((struct pwindow *)windows->items[j])->order > w->order) {
			windows->items[j + 1] = windows->items[j];
			j--;
		}
		windows->items[j + 1] = w;
	}

	panel->state.focused_window = 0;
	for (int i = 0; i < windows->length; i++) {
		struct pwindow *w = windows->items[i];
		if (w->focused) {
			panel->state.focused_window = w->id;
		}
	}
	if (old) {
		for (int i = 0; i < old->length; i++) {
			free_window(old->items[i]);
		}
		list_free(old);
	}
	panel->state.windows = windows;
	notify_widgets(panel);
}

void ipc_panel_refresh_workspaces(struct panel *panel) {
	json_object *arr = request(panel, IPC_GET_WORKSPACES, NULL);
	if (!arr || !json_object_is_type(arr, json_type_array)) {
		json_object_put(arr);
		return;
	}
	list_t *list = create_list();
	size_t len = json_object_array_length(arr);
	for (size_t i = 0; i < len; i++) {
		json_object *obj = json_object_array_get_idx(arr, i);
		struct pworkspace *ws = calloc(1, sizeof(*ws));
		const char *name = jstr(obj, "name");
		const char *output = jstr(obj, "output");
		ws->name = strdup(name ? name : "");
		ws->output = strdup(output ? output : "");
		ws->num = (int)jint(obj, "num");
		ws->focused = jbool(obj, "focused");
		ws->visible = jbool(obj, "visible");
		ws->urgent = jbool(obj, "urgent");
		if (ws->focused) {
			free(panel->state.focused_output);
			panel->state.focused_output = strdup(ws->output);
			free(panel->state.focused_workspace);
			panel->state.focused_workspace = strdup(ws->name);
		}
		list_add(list, ws);
	}
	json_object_put(arr);
	if (panel->state.workspaces) {
		for (int i = 0; i < panel->state.workspaces->length; i++) {
			free_workspace(panel->state.workspaces->items[i]);
		}
		list_free(panel->state.workspaces);
	}
	panel->state.workspaces = list;
	notify_widgets(panel);
}

void ipc_panel_refresh_inputs(struct panel *panel) {
	json_object *arr = request(panel, IPC_GET_INPUTS, NULL);
	if (!arr || !json_object_is_type(arr, json_type_array)) {
		json_object_put(arr);
		return;
	}
	size_t len = json_object_array_length(arr);
	for (size_t i = 0; i < len; i++) {
		json_object *obj = json_object_array_get_idx(arr, i);
		const char *type = jstr(obj, "type");
		const char *layout = jstr(obj, "xkb_active_layout_name");
		if (type && strcmp(type, "keyboard") == 0 && layout) {
			free(panel->state.keyboard_layout);
			panel->state.keyboard_layout = strdup(layout);
			break;
		}
	}
	json_object_put(arr);
	notify_widgets(panel);
}

static void set_main_output(struct panel *panel, const char *name) {
	if (!name || !*name) {
		name = NULL;
	}
	if ((!name && !panel->state.main_output) || (name && panel->state.main_output &&
			strcmp(name, panel->state.main_output) == 0)) {
		return;
	}
	free(panel->state.main_output);
	panel->state.main_output = name ? strdup(name) : NULL;
	if (panel->config) { // not while starting, before the screens are known
		desktop_main_output_changed(panel);
		notify_widgets(panel);
	}
}

static void refresh_mode(struct panel *panel) {
	json_object *state = request(panel, IPC_GET_TILEWIN, NULL);
	const char *mode = jstr(state, "mode");
	if (mode) {
		free(panel->state.mode);
		panel->state.mode = strdup(mode);
		panel->layout = strcmp(mode, "tile") == 0 ? LAYOUT_TILE : LAYOUT_WINDOW;
	}
	int64_t ms = jint(state, "double_click_time");
	panel->state.double_click_ms = ms > 0 ? (int)ms : 0;
	set_main_output(panel, jstr(state, "main_output"));
	json_object_put(state);
}

static void tree_timer_fired(void *data) {
	struct panel *panel = data;
	panel->tree_timer = NULL;
	ipc_panel_refresh_tree(panel);
}

static void schedule_tree_refresh(struct panel *panel) {
	if (!panel->tree_timer) {
		panel->tree_timer = loop_add_timer(panel->loop, 30, tree_timer_fired, panel);
	}
}

bool ipc_panel_init(struct panel *panel) {
	panel->ipc_cmd_fd = ipc_open_socket(panel->socket_path);
	panel->ipc_event_fd = ipc_open_socket(panel->socket_path);
	if (panel->ipc_cmd_fd < 0 || panel->ipc_event_fd < 0) {
		return false;
	}
	struct timeval tv = { .tv_sec = 2 };
	ipc_set_recv_timeout(panel->ipc_cmd_fd, tv);

	const char *subscribe = "[\"window\", \"workspace\", \"tilewin\", \"input\", \"output\", \"shutdown\"]";
	uint32_t len = strlen(subscribe);
	char *resp = ipc_single_command(panel->ipc_event_fd, IPC_SUBSCRIBE, subscribe, &len);
	bool ok = resp && strstr(resp, "true");
	free(resp);
	if (!ok) {
		return false;
	}
	panel->state.windows = create_list();
	panel->state.workspaces = create_list();
	refresh_mode(panel);
	ipc_panel_refresh_workspaces(panel);
	ipc_panel_refresh_tree(panel);
	ipc_panel_refresh_inputs(panel);
	return true;
}

void ipc_panel_fini(struct panel *panel) {
	if (panel->state.windows) {
		for (int i = 0; i < panel->state.windows->length; i++) {
			free_window(panel->state.windows->items[i]);
		}
		list_free(panel->state.windows);
	}
	if (panel->state.workspaces) {
		for (int i = 0; i < panel->state.workspaces->length; i++) {
			free_workspace(panel->state.workspaces->items[i]);
		}
		list_free(panel->state.workspaces);
	}
	free(panel->state.mode);
	free(panel->state.focused_output);
	free(panel->state.main_output);
	free(panel->state.focused_workspace);
	free(panel->state.keyboard_layout);
}

bool ipc_panel_command(struct panel *panel, const char *command) {
	json_object *res = request(panel, IPC_COMMAND, command);
	bool ok = false;
	if (res && json_object_is_type(res, json_type_array) &&
			json_object_array_length(res) > 0) {
		json_object *first = json_object_array_get_idx(res, 0);
		ok = jbool(first, "success");
		if (!ok) {
			const char *error = jstr(first, "error");
			sway_log(SWAY_ERROR, "Command '%s' failed: %s", command, error ? error : "?");
		}
	}
	json_object_put(res);
	return ok;
}

bool ipc_panel_commandf(struct panel *panel, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *command = vformat_str(fmt, args);
	va_end(args);
	bool ok = command && ipc_panel_command(panel, command);
	free(command);
	return ok;
}

static void handle_window_event(struct panel *panel, json_object *event) {
	const char *change = jstr(event, "change");
	json_object *con;
	if (!change || !json_object_object_get_ex(event, "container", &con)) {
		return;
	}
	if (strcmp(change, "new") == 0 || strcmp(change, "close") == 0 ||
			strcmp(change, "move") == 0 || strcmp(change, "floating") == 0) {
		schedule_tree_refresh(panel);
		return;
	}
	struct pwindow *w = panel_find_window(panel, jint(con, "id"));
	if (!w) {
		schedule_tree_refresh(panel);
		return;
	}
	update_window_from_json(w, con);
	if (strcmp(change, "focus") == 0) {
		for (int i = 0; i < panel->state.windows->length; i++) {
			struct pwindow *o = panel->state.windows->items[i];
			o->focused = o == w;
		}
		panel->state.focused_window = w->id;
		// the focused window may live on another workspace/output now
		schedule_tree_refresh(panel);
	}
	notify_widgets(panel);
}

static void handle_tilewin_event(struct panel *panel, json_object *event) {
	const char *change = jstr(event, "change");
	if (!change) {
		return;
	}
	if (strcmp(change, "mode") == 0) {
		const char *mode = jstr(event, "mode");
		free(panel->state.mode);
		panel->state.mode = strdup(mode ? mode : "window");
		enum layout_index layout = mode && strcmp(mode, "tile") == 0 ?
			LAYOUT_TILE : LAYOUT_WINDOW;
		popup_close_all(panel);
		if (layout != panel->layout) {
			panel->layout = layout;
			panel_outputs_update_bars(panel);
		}
		schedule_tree_refresh(panel);
	} else if (strcmp(change, "settings") == 0) {
		int64_t ms = jint(event, "double_click_time");
		panel->state.double_click_ms = ms > 0 ? (int)ms : 0;
	} else if (strcmp(change, "main_output") == 0) {
		set_main_output(panel, jstr(event, "main_output"));
	} else if (strcmp(change, "theme") == 0) {
		panel_request_reload(panel);
	} else if (strcmp(change, "panel") == 0) {
		const char *args = jstr(event, "args");
		if (args) {
			bar_handle_panel_command(panel, args);
		}
	} else if (strcmp(change, "window_menu") == 0) {
		const char *output_name = jstr(event, "output");
		struct panel_output *output = NULL, *iter;
		wl_list_for_each(iter, &panel->outputs, link) {
			if (output_name && iter->name && strcmp(iter->name, output_name) == 0) {
				output = iter;
			}
		}
		if (!output) {
			output = panel_focused_output(panel);
		}
		char args[96];
		snprintf(args, sizeof(args), "window_menu %lld %d %d",
			(long long)jint(event, "con_id"), (int)jint(event, "x"), (int)jint(event, "y"));
		free(panel->state.focused_output);
		panel->state.focused_output = output && output->name ? strdup(output->name) : NULL;
		bar_handle_panel_command(panel, args);
	}
}

void ipc_panel_readable(struct panel *panel) {
	struct ipc_response *resp = ipc_recv_response(panel->ipc_event_fd);
	if (!resp) {
		panel->running = false;
		return;
	}
	json_object *event = json_tokener_parse(resp->payload);
	switch (resp->type) {
	case IPC_EVENT_WINDOW:
		handle_window_event(panel, event);
		break;
	case IPC_EVENT_WORKSPACE:
		ipc_panel_refresh_workspaces(panel);
		schedule_tree_refresh(panel);
		break;
	case IPC_EVENT_OUTPUT:
		ipc_panel_refresh_workspaces(panel);
		break;
	case IPC_EVENT_INPUT:
		ipc_panel_refresh_inputs(panel);
		break;
	case IPC_EVENT_TILEWIN:
		handle_tilewin_event(panel, event);
		break;
	case IPC_EVENT_SHUTDOWN:
		panel->running = false;
		break;
	default:
		break;
	}
	json_object_put(event);
	free_ipc_response(resp);
}
