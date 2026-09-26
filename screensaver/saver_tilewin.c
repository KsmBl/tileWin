/*
 * The windows tileWin shows on a screen, for the savers drawn over them
 * (Diggers, Hellfire): where each is, its title bar and border, and where the
 * taskbars are. Without tileWin (the preview of the settings) there are none.
 */
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "ipc-client.h"
#include "ipc.h"
#include "saver_util.h"

static int rect_int(json_object *rect, const char *key) {
	json_object *v;
	return rect && json_object_object_get_ex(rect, key, &v) ? json_object_get_int(v) : 0;
}

static void collect_windows(json_object *node, int ox, int oy, struct saver_window *wins,
		int max, int *count) {
	json_object *v;
	bool view = json_object_object_get_ex(node, "pid", &v) && json_object_get_int(v) > 0;
	if (view) {
		bool visible = json_object_object_get_ex(node, "visible", &v) &&
			json_object_get_boolean(v);
		bool minimized = json_object_object_get_ex(node, "minimized", &v) &&
			json_object_get_boolean(v);
		json_object *rect = NULL, *deco = NULL;
		json_object_object_get_ex(node, "rect", &rect);
		json_object_object_get_ex(node, "deco_rect", &deco);
		if (visible && !minimized && *count < max) {
			// the rect is what the app draws; the title bar sits on top of it
			int title = rect_int(deco, "height");
			struct saver_window *b = &wins[(*count)++];
			b->x = rect_int(rect, "x") - ox;
			b->y = rect_int(rect, "y") - title - oy;
			b->w = rect_int(rect, "width");
			b->h = rect_int(rect, "height") + title;
			b->title_h = title;
			b->border = json_object_object_get_ex(node, "current_border_width", &v) ?
				json_object_get_int(v) : 0;
			const char *name = json_object_object_get_ex(node, "name", &v) ?
				json_object_get_string(v) : NULL;
			snprintf(b->title, sizeof(b->title), "%s", name ? name : "");
		}
	}
	static const char *const children[] = { "nodes", "floating_nodes" };
	for (int k = 0; k < 2; k++) {
		json_object *list;
		if (!json_object_object_get_ex(node, children[k], &list)) {
			continue;
		}
		for (size_t i = 0; i < json_object_array_length(list); i++) {
			collect_windows(json_object_array_get_idx(list, i), ox, oy, wins, max, count);
		}
	}
}

/* The socket of tileWin, or -1; unlike ipc_open_socket, not being able to is no end. */
static int connect_tilewin(void) {
	const char *path = getenv("TILEWINSOCK");
	path = path ? path : getenv("SWAYSOCK");
	if (!path) {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

/*
 * The taskbars of the screen: what its workspaces leave free of it, at the
 * top, the bottom or a side. tileWin lists no taskbar, but a workspace is as
 * big as the screen less the taskbars.
 */
static void find_bars(json_object *output, int ox, int oy, int ow, int oh,
		struct saver_window *bars, int *bar_count) {
	*bar_count = 0;
	json_object *workspaces, *v, *rect;
	if (!json_object_object_get_ex(output, "nodes", &workspaces)) {
		return;
	}
	for (size_t i = 0; i < json_object_array_length(workspaces); i++) {
		json_object *ws = json_object_array_get_idx(workspaces, i);
		if (!json_object_object_get_ex(ws, "type", &v) ||
				strcmp(json_object_get_string(v), "workspace") != 0 ||
				!json_object_object_get_ex(ws, "rect", &rect)) {
			continue;
		}
		double x = rect_int(rect, "x") - ox, y = rect_int(rect, "y") - oy;
		double w = rect_int(rect, "width"), h = rect_int(rect, "height");
		if (w <= 0 || h <= 0) {
			continue;
		}
		struct saver_window strips[4] = {
			{ .x = 0, .y = 0, .w = ow, .h = y },                 // above
			{ .x = 0, .y = y + h, .w = ow, .h = oh - (y + h) },  // below
			{ .x = 0, .y = 0, .w = x, .h = oh },                 // left
			{ .x = x + w, .y = 0, .w = ow - (x + w), .h = oh },  // right
		};
		for (int k = 0; k < 4; k++) {
			if (strips[k].w > 0 && strips[k].h > 0) {
				bars[(*bar_count)++] = strips[k];
			}
		}
		return; // every workspace of a screen has the same size
	}
}

bool saver_tilewin_windows(const char *output, struct saver_window *wins, int max,
		int *count, struct saver_window *bars, int *bar_count) {
	if (!output || !*output) {
		return false;
	}
	int fd = connect_tilewin();
	if (fd < 0) {
		return false;
	}
	uint32_t len = 0;
	char *reply = ipc_single_command(fd, IPC_GET_TREE, NULL, &len);
	close(fd);
	json_object *tree = reply ? json_tokener_parse(reply) : NULL;
	free(reply);
	if (!tree) {
		return false;
	}
	*count = 0;
	bool found = false;
	json_object *outputs;
	if (json_object_object_get_ex(tree, "nodes", &outputs)) {
		for (size_t i = 0; i < json_object_array_length(outputs); i++) {
			json_object *o = json_object_array_get_idx(outputs, i), *name, *rect;
			if (json_object_object_get_ex(o, "name", &name) &&
					strcmp(json_object_get_string(name), output) == 0 &&
					json_object_object_get_ex(o, "rect", &rect)) {
				int ox = rect_int(rect, "x"), oy = rect_int(rect, "y");
				collect_windows(o, ox, oy, wins, max, count);
				if (bars) {
					find_bars(o, ox, oy, rect_int(rect, "width"), rect_int(rect, "height"),
						bars, bar_count);
				}
				found = true;
			}
		}
	}
	json_object_put(tree);
	return found;
}
