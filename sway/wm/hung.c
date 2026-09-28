#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "log.h"
#include "stringop.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/ipc-server.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

/*
 * Apps that stop answering, like on Windows: an app whose window the user
 * clicks, types into, focuses or closes is asked (a Wayland ping, or the X11
 * _NET_WM_PING) whether it is still there. One that gives no answer within
 * five seconds is "(Not Responding)": its title says so, the window turns
 * pale, and closing it lets the taskbar offer to end the app. As soon as it
 * answers again all of that goes away.
 *
 * Nothing is asked while nobody does anything with a window, so an idle
 * desktop wakes no app; a hanging one is asked once a second until it answers.
 */

#define PING_TIMEOUT_MS 5000 // how long an app may take to answer
#define POKE_MS 2000         // an app is asked at most this often while it answers
#define CHECK_MS 1000        // how often answers are looked for
#define CLOSE_ASK_MS 10000   // a close this recent: offer to end the app when it hangs
static const float ghost_color[4] = { 0.62f, 0.62f, 0.62f, 0.62f }; // premultiplied white

static struct wl_event_source *timer;
static list_t *watched; // struct sway_view *: pinged or hanging

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool enabled(void) {
	return !config || config->tw_not_responding;
}

static bool ping_pending(struct sway_view *view) {
	switch (view->type) {
	case SWAY_VIEW_XDG_SHELL:
		return view->wlr_xdg_toplevel && view->wlr_xdg_toplevel->base->client->ping_serial != 0;
#if WLR_HAS_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		return view->wlr_xwayland_surface && view->wlr_xwayland_surface->pinging;
#endif
	}
	return false;
}

static void watch(struct sway_view *view) {
	if (!watched) {
		watched = create_list();
	}
	if (list_find(watched, view) < 0) {
		list_add(watched, view);
	}
	if (timer) {
		wl_event_source_timer_update(timer, CHECK_MS);
	}
}

static void unwatch(struct sway_view *view) {
	int i = watched ? list_find(watched, view) : -1;
	if (i >= 0) {
		list_del(watched, i);
	}
}

static void send_ping(struct sway_view *view) {
	if (!view->surface || tw_pause_is_paused(view)) {
		return; // a stopped app cannot answer, and that is not hanging
	}
	switch (view->type) {
	case SWAY_VIEW_XDG_SHELL:
		wlr_xdg_surface_ping(view->wlr_xdg_toplevel->base);
		break;
#if WLR_HAS_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		wlr_xwayland_surface_ping(view->wlr_xwayland_surface);
		break;
#endif
	}
	view->tw_hung.pinged = true;
	view->tw_hung.pinged_ms = now_ms();
	watch(view);
}

static void describe(struct sway_view *view, const char *change) {
	struct sway_container *con = view->container;
	if (!con) {
		return;
	}
	json_object *data = json_object_new_object();
	json_object_object_add(data, "con_id", json_object_new_int64((int64_t)con->node.id));
	const char *title = view_get_title(view);
	json_object_object_add(data, "title", json_object_new_string(title ? title : ""));
	const char *app_id = view_get_app_id(view);
	if (!app_id) {
		app_id = view_get_class(view);
	}
	json_object_object_add(data, "app_id", json_object_new_string(app_id ? app_id : ""));
	json_object_object_add(data, "pid", json_object_new_int(view->pid));
	ipc_event_tilewin(change, data);
}

void tw_hung_arrange(struct sway_view *view) {
	struct wlr_scene_rect *ghost = view->tw_hung.ghost;
	if (!ghost || !view->container) {
		return;
	}
	wlr_scene_rect_set_size(ghost, view->container->current.content_width,
		view->container->current.content_height);
	wlr_scene_node_raise_to_top(&ghost->node);
}

static void set_hung(struct sway_view *view, bool hung) {
	if (view->tw_hung.hung == hung) {
		return;
	}
	view->tw_hung.hung = hung;
	sway_log(SWAY_DEBUG, "%s %s", view_get_title(view) ? view_get_title(view) : "a window",
		hung ? "does not respond" : "responds again");
	if (hung && !view->tw_hung.ghost && view->scene_tree) {
		view->tw_hung.ghost = wlr_scene_rect_create(view->scene_tree, 0, 0, ghost_color);
	}
	if (view->tw_hung.ghost) {
		wlr_scene_node_set_enabled(&view->tw_hung.ghost->node, hung);
		tw_hung_arrange(view);
	}
	if (view->container) {
		view_update_title(view, true);
		node_set_dirty(&view->container->node); // its frame shows the new title
		transaction_commit_dirty();
	}
	if (hung) {
		watch(view);
		if (view->tw_hung.close_ms && now_ms() - view->tw_hung.close_ms < CLOSE_ASK_MS) {
			describe(view, "not_responding"); // closing it did not work: offer to end it
		}
	} else {
		view->tw_hung.close_ms = 0;
		describe(view, "responding");
	}
}

static int handle_timer(void *data) {
	if (!watched) {
		return 0;
	}
	int64_t now = now_ms();
	for (int i = watched->length - 1; i >= 0; i--) {
		struct sway_view *view = watched->items[i];
		if (!view->surface) {
			list_del(watched, i);
			continue;
		}
		if (view->tw_hung.pinged && !ping_pending(view)) {
			view->tw_hung.pinged = false; // it answered
			set_hung(view, false);
		} else if (view->tw_hung.pinged &&
				now - view->tw_hung.pinged_ms > PING_TIMEOUT_MS + 2 * CHECK_MS) {
			// asked by someone else as well, whose answer or timeout came first
			view->tw_hung.pinged = false;
		}
		if (view->tw_hung.hung && !view->tw_hung.pinged) {
			send_ping(view); // ask again, to notice it answering
		}
		if (!view->tw_hung.hung && !view->tw_hung.pinged) {
			list_del(watched, i);
		}
	}
	if (watched->length > 0) {
		wl_event_source_timer_update(timer, CHECK_MS);
	}
	return 0;
}

static void handle_ping_timeout(struct wl_listener *listener, void *data) {
	struct sway_view *view = wl_container_of(listener, view, tw_hung.ping_timeout);
	view->tw_hung.pinged = false;
	if (enabled() && !tw_pause_is_paused(view)) {
		set_hung(view, true);
	}
}

void tw_hung_view_mapped(struct sway_view *view) {
	if (!timer) {
		timer = wl_event_loop_add_timer(server.wl_event_loop, handle_timer, NULL);
		// the xdg-shell default is ten seconds; Windows says it after five
		server.xdg_shell->ping_timeout = PING_TIMEOUT_MS;
	}
	if (view->tw_hung.listening) {
		return;
	}
	view->tw_hung.ping_timeout.notify = handle_ping_timeout;
	switch (view->type) {
	case SWAY_VIEW_XDG_SHELL:
		wl_signal_add(&view->wlr_xdg_toplevel->base->events.ping_timeout,
			&view->tw_hung.ping_timeout);
		break;
#if WLR_HAS_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		wl_signal_add(&view->wlr_xwayland_surface->events.ping_timeout,
			&view->tw_hung.ping_timeout);
		break;
#endif
	}
	view->tw_hung.listening = true;
}

void tw_hung_view_unmapped(struct sway_view *view) {
	if (view->tw_hung.hung) {
		view->tw_hung.hung = false;
		describe(view, "responding"); // gone: nothing left to offer
	}
	if (view->tw_hung.listening) {
		wl_list_remove(&view->tw_hung.ping_timeout.link);
		view->tw_hung.listening = false;
	}
	if (view->tw_hung.ghost) {
		wlr_scene_node_destroy(&view->tw_hung.ghost->node);
		view->tw_hung.ghost = NULL;
	}
	view->tw_hung.pinged = false;
	view->tw_hung.close_ms = 0;
	unwatch(view);
}

void tw_hung_poke(struct sway_view *view) {
	if (!view || !view->surface || !enabled() || !timer) {
		return;
	}
	if (view->tw_hung.pinged || view->tw_hung.hung) {
		return; // an answer is awaited already
	}
	if (now_ms() - view->tw_hung.pinged_ms < POKE_MS) {
		return;
	}
	send_ping(view);
}

void tw_hung_close_requested(struct sway_view *view) {
	if (!view || !enabled()) {
		return;
	}
	view->tw_hung.close_ms = now_ms();
	if (view->tw_hung.hung) {
		describe(view, "not_responding");
	} else if (!view->tw_hung.pinged) {
		send_ping(view); // one that hangs now gets the offer once it is known
	}
}

char *tw_hung_title(struct sway_view *view, const char *title) {
	if (!title) {
		return NULL;
	}
	if (view->tw_hung.hung) {
		return format_str("%s (Not Responding)", title);
	}
	return strdup(title);
}

bool tw_end_task(struct sway_view *view, char **error) {
	pid_t pid = view ? view->pid : 0;
	if (pid <= 1 || pid == getpid() || pid == tw_panel_pid()) {
		*error = strdup("The app of this window is not known");
		return false;
	}
#if WLR_HAS_XWAYLAND
	if (server.xwayland.wlr_xwayland && server.xwayland.wlr_xwayland->server &&
			pid == server.xwayland.wlr_xwayland->server->pid) {
		*error = strdup("The app of this window is not known");
		return false;
	}
#endif
	tw_pause_wake(view);
	if (kill(pid, SIGKILL) != 0) {
		*error = format_str("Could not end the app: %s", strerror(errno));
		return false;
	}
	sway_log(SWAY_INFO, "Ended the app of '%s' (pid %d)",
		view_get_title(view) ? view_get_title(view) : "", pid);
	return true;
}

void tw_hung_config_changed(void) {
	if (enabled() || !watched) {
		return;
	}
	for (int i = watched->length - 1; i >= 0; i--) {
		struct sway_view *view = watched->items[i];
		view->tw_hung.pinged = false;
		set_hung(view, false);
	}
	watched->length = 0;
}
