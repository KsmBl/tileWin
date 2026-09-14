#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "draw.h"
#include "log.h"
#include "panel.h"

#if HAVE_TRAY
#include "swaybar/tray/icon.h"
#include "tray/host.h"
#include "tray/item.h"
#include "tray/tray.h"
#include "tray/watcher.h"

static int handle_lost_watcher(sd_bus_message *msg, void *data, sd_bus_error *error) {
	char *service, *old_owner, *new_owner;
	int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
	if (ret < 0) {
		return ret;
	}
	if (!*new_owner) {
		struct swaybar_tray *tray = data;
		if (strcmp(service, "org.freedesktop.StatusNotifierWatcher") == 0) {
			tray->watcher_xdg = create_watcher("freedesktop", tray->bus);
		} else if (strcmp(service, "org.kde.StatusNotifierWatcher") == 0) {
			tray->watcher_kde = create_watcher("kde", tray->bus);
		}
	}
	return 0;
}

struct swaybar_tray *create_tray(struct panel *panel) {
	sd_bus *bus;
	int ret = sd_bus_open_user(&bus);
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Tray: failed to connect to user bus: %s", strerror(-ret));
		return NULL;
	}
	struct swaybar_tray *tray = calloc(1, sizeof(*tray));
	tray->panel = panel;
	tray->bus = bus;
	tray->fd = sd_bus_get_fd(bus);
	tray->watcher_xdg = create_watcher("freedesktop", bus);
	tray->watcher_kde = create_watcher("kde", bus);
	sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", handle_lost_watcher, tray);
	tray->items = create_list();
	init_host(&tray->host_xdg, "freedesktop", tray);
	init_host(&tray->host_kde, "kde", tray);
	init_themes(&tray->themes, &tray->basedirs);
	return tray;
}

void destroy_tray(struct swaybar_tray *tray) {
	if (!tray) {
		return;
	}
	finish_host(&tray->host_xdg);
	finish_host(&tray->host_kde);
	for (int i = 0; i < tray->items->length; ++i) {
		destroy_sni(tray->items->items[i]);
	}
	list_free(tray->items);
	destroy_watcher(tray->watcher_xdg);
	destroy_watcher(tray->watcher_kde);
	sd_bus_flush_close_unref(tray->bus);
	finish_themes(tray->themes, tray->basedirs);
	free(tray);
}

void tray_process(struct swaybar_tray *tray) {
	int ret;
	while ((ret = sd_bus_process(tray->bus, NULL)) > 0) {
		// keep processing
	}
	if (ret < 0) {
		sway_log(SWAY_ERROR, "Tray: failed to process bus: %s", strerror(-ret));
	}
}

void tray_set_dirty(struct swaybar_tray *tray) {
	panel_set_dirty(tray->panel);
}

static void tray_in(int fd, short mask, void *data) {
	tray_process(data);
}

static void tray_widget_set_active(struct widget *w, bool active) {
	if (active && !w->data) {
		struct swaybar_tray *tray = create_tray(w->panel);
		if (tray) {
			w->data = tray;
			loop_add_fd(w->panel->loop, tray->fd, POLLIN, tray_in, tray);
			tray_process(tray);
		}
	}
}

static void tray_widget_destroy(struct widget *w) {
	struct swaybar_tray *tray = w->data;
	if (tray) {
		loop_remove_fd(w->panel->loop, tray->fd);
		destroy_tray(tray);
	}
}

static int tray_icon_size(struct render_ctx *ctx) {
	return ctx->height >= 36 ? 18 : 16;
}

static int tray_measure(struct widget *w, struct render_ctx *ctx) {
	struct swaybar_tray *tray = w->data;
	if (!tray) {
		return 0;
	}
	int count = 0;
	for (int i = 0; i < tray->items->length; i++) {
		count += sni_visible(tray->items->items[i]);
	}
	return count ? count * (tray_icon_size(ctx) + 8) + 4 : 0;
}

static void tray_render(struct widget *w, struct render_ctx *ctx, struct pbox box) {
	struct swaybar_tray *tray = w->data;
	if (!tray) {
		return;
	}
	int size = tray_icon_size(ctx);
	int x = box.x + 2;
	const char *theme = tw_theme_str(ctx->panel->theme, "icons.theme", "hicolor");
	for (int i = 0; i < tray->items->length; i++) {
		struct swaybar_sni *sni = tray->items->items[i];
		if (!sni_visible(sni)) {
			continue;
		}
		struct pbox b = { x, box.y, size + 8, box.height };
		if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
			render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
		}
		cairo_surface_t *icon = sni_icon(sni, size * ctx->surface->scale, theme);
		pd_icon(ctx->cairo, icon, b.x + 4, b.y + (b.height - size) / 2.0, size);
		psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, i,
			sni->watcher_id);
		x += size + 8;
	}
}

static struct swaybar_sni *find_sni(struct swaybar_tray *tray, const char *id) {
	for (int i = 0; tray && id && i < tray->items->length; i++) {
		struct swaybar_sni *sni = tray->items->items[i];
		if (strcmp(sni->watcher_id, id) == 0) {
			return sni;
		}
	}
	return NULL;
}

static bool tray_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	struct swaybar_sni *sni = find_sni(w->data, hs->str);
	if (!sni) {
		return false;
	}
	int gx = (s->output ? s->output->x : 0) + (int)x;
	int gy = (s->output ? s->output->y : 0) + (int)y;
	sni_click(sni, gx, gy, button);
	return true;
}

static bool tray_scroll(struct widget *w, struct psurface *s, struct hotspot *hs,
		int direction) {
	struct swaybar_sni *sni = find_sni(w->data, hs->str);
	if (sni) {
		sni_scroll(sni, direction);
	}
	return sni != NULL;
}

static char *tray_tooltip(struct widget *w, struct hotspot *hs) {
	struct swaybar_sni *sni = find_sni(w->data, hs->str);
	return sni && sni->title && *sni->title ? strdup(sni->title) : NULL;
}

const struct widget_impl widget_tray = {
	.type = "tray",
	.destroy = tray_widget_destroy,
	.measure = tray_measure,
	.render = tray_render,
	.click = tray_click,
	.scroll = tray_scroll,
	.tooltip = tray_tooltip,
	.set_active = tray_widget_set_active,
};

#else

static int tray_measure(struct widget *w, struct render_ctx *ctx) {
	return 0;
}

const struct widget_impl widget_tray = {
	.type = "tray",
	.measure = tray_measure,
};

#endif
