#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "tw_paths.h"
#include "sway/config.h"
#include "sway/output.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

/*
 * Each app opens where its window was last closed ("remember_windows"), as
 * apps on Windows do by themselves: the place and the size, and whether it
 * was maximized or snapped. It opens on the screen it is started on (where
 * the pointer is), at the same place of that screen. That is kept per app id in
 * ~/.local/state/tileWin/window-places, the most recent last.
 *
 * Only the first window of an app goes there; a second one opening while the
 * first is still open would cover it exactly, so it gets the usual cascade.
 * Dialogs and windows of a fixed size are left alone, and so are the apps in
 * "remember_windows_except". The session restore, which knows each window,
 * comes first; window rules (for_window) come after and win.
 */

#define PLACES_MAX 200

struct place {
	char *app_id;
	char *output;
	struct wlr_box box; // from the top left corner of that screen
	bool maximized;
	enum tw_snap snap;
};

static list_t *places; // struct place *, loaded on first use

static void place_free(struct place *p) {
	free(p->app_id);
	free(p->output);
	free(p);
}

static char *places_path(void) {
	char *dir = tw_state_dir();
	if (!dir) {
		return NULL;
	}
	tw_mkdir_p(dir);
	char *path = format_str("%s/window-places", dir);
	free(dir);
	return path;
}

static void load(void) {
	if (places) {
		return;
	}
	places = create_list();
	char *path = places_path();
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	if (!f) {
		return;
	}
	char *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, f) > 0) {
		line[strcspn(line, "\n")] = 0;
		// app id, screen, x, y, width, height, maximized, snap; tab separated
		char *fields[8];
		int n = 0;
		char *save = NULL;
		for (char *tok = strtok_r(line, "\t", &save); tok && n < 8;
				tok = strtok_r(NULL, "\t", &save)) {
			fields[n++] = tok;
		}
		if (n < 8) {
			continue;
		}
		struct place *p = calloc(1, sizeof(*p));
		if (!p) {
			break;
		}
		p->app_id = strdup(fields[0]);
		p->output = strdup(fields[1]);
		p->box = (struct wlr_box){ atoi(fields[2]), atoi(fields[3]), atoi(fields[4]),
			atoi(fields[5]) };
		p->maximized = atoi(fields[6]) != 0;
		p->snap = (enum tw_snap)atoi(fields[7]);
		if (p->box.width <= 0 || p->box.height <= 0 || p->snap > TW_SNAP_BOTTOMRIGHT) {
			place_free(p);
			continue;
		}
		list_add(places, p);
	}
	free(line);
	fclose(f);
}

static void save(void) {
	char *path = places_path();
	if (!path) {
		return;
	}
	char *tmp = format_str("%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (f) {
		for (int i = 0; i < places->length; i++) {
			struct place *p = places->items[i];
			fprintf(f, "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", p->app_id, p->output,
				p->box.x, p->box.y, p->box.width, p->box.height, p->maximized ? 1 : 0,
				(int)p->snap);
		}
		fclose(f);
		rename(tmp, path);
	}
	free(tmp);
	free(path);
}

static struct place *find(const char *app_id) {
	for (int i = places->length - 1; i >= 0; i--) {
		struct place *p = places->items[i];
		if (strcmp(p->app_id, app_id) == 0) {
			return p;
		}
	}
	return NULL;
}

static const char *app_id_of(struct sway_view *view) {
	const char *app_id = view_get_app_id(view);
	return app_id ? app_id : view_get_class(view);
}

static bool wanted(struct sway_view *view, const char *app_id) {
	return config && config->tw_remember_places && app_id && *app_id && !strchr(app_id, '\t') &&
		!strchr(app_id, '\n') && !tw_app_in_list(config->tw_remember_except, app_id) &&
		!tw_view_is_dialog(view);
}

void tw_remember_view_closing(struct sway_view *view) {
	struct sway_container *con = view->container;
	const char *app_id = app_id_of(view);
	if (tw_mode != TW_MODE_WINDOW || !con || !container_is_floating(con) ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE || !wanted(view, app_id)) {
		return;
	}
	struct sway_workspace *ws = con->pending.workspace;
	struct sway_output *output = ws ? ws->output : NULL;
	if (!output || !output->wlr_output || !output->wlr_output->name) {
		return;
	}
	bool fills = con->pending.tw_maximized || con->tw.snap != TW_SNAP_NONE;
	struct wlr_box box = fills ? con->tw.restore_box : (struct wlr_box){ con->pending.x,
		con->pending.y, con->pending.width, con->pending.height };
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	struct wlr_box ob;
	wlr_output_layout_get_box(root->output_layout, output->wlr_output, &ob);
	load();
	struct place *p = find(app_id);
	if (p) {
		list_del(places, list_find(places, p));
	} else {
		p = calloc(1, sizeof(*p));
		if (!p) {
			return;
		}
		p->app_id = strdup(app_id);
	}
	free(p->output);
	p->output = strdup(output->wlr_output->name);
	p->box = (struct wlr_box){ box.x - ob.x, box.y - ob.y, box.width, box.height };
	p->maximized = con->pending.tw_maximized;
	p->snap = con->tw.snap;
	list_add(places, p); // the most recent last
	while (places->length > PLACES_MAX) {
		place_free(places->items[0]);
		list_del(places, 0);
	}
	save();
}

struct other_window {
	struct sway_container *con;
	const char *app_id;
	bool found;
};

static void look_for_other(struct sway_container *con, void *data) {
	struct other_window *o = data;
	if (con != o->con && con->view && con->view->surface) {
		const char *app_id = app_id_of(con->view);
		if (app_id && strcmp(app_id, o->app_id) == 0) {
			o->found = true;
		}
	}
}

bool tw_remember_apply(struct sway_container *con) {
	struct sway_view *view = con->view;
	const char *app_id = view ? app_id_of(view) : NULL;
	if (!view || !container_is_floating(con) || !wanted(view, app_id)) {
		return false;
	}
	load();
	struct place *p = find(app_id);
	if (!p) {
		return false;
	}
	struct other_window other = { con, app_id, false };
	root_for_each_container(look_for_other, &other);
	if (other.found) {
		return false; // it would cover the one that is open
	}
	// it opens on the screen it is started on (where the pointer is); the
	// place and size it had there are taken over to this one
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || !ws->output) {
		return false;
	}
	struct wlr_box ob;
	wlr_output_layout_get_box(root->output_layout, ws->output->wlr_output, &ob);
	struct wlr_box box = { ob.x + p->box.x, ob.y + p->box.y, p->box.width, p->box.height };
	box = tw_fit_box(box, tw_workarea(ws));
	tw_unmaximize_new(con);
	tw_set_box(con, &box);
	if (p->maximized) {
		tw_maximize(con, true);
	} else if (p->snap != TW_SNAP_NONE) {
		tw_snap_to(con, p->snap);
	} else {
		tw_maximize_if_nearly_full(con); // on a smaller screen than it was closed on
	}
	sway_log(SWAY_DEBUG, "Opened %s where it was last", app_id);
	return true;
}
