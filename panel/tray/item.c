#include <arpa/inet.h>
#include <cairo.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swaybar/tray/icon.h"
#include "tray/item.h"
#include "tray/tray.h"
#include "tw_desktop.h"

/*
 * StatusNotifierItem handling adapted from swaybar. The D-Bus side is
 * unchanged; icon loading and clicks are wired to the tileWin panel.
 */

static bool sni_ready(struct swaybar_sni *sni) {
	return sni->status && (sni->status[0] == 'N' ? // NeedsAttention
			sni->attention_icon_name || sni->attention_icon_pixmap :
			sni->icon_name || sni->icon_pixmap);
}

static void set_sni_dirty(struct swaybar_sni *sni) {
	if (sni_ready(sni)) {
		sni->icon_size = 0; // invalidate previous icon
		tray_set_dirty(sni->tray);
	}
}

static int read_pixmap(sd_bus_message *msg, struct swaybar_sni *sni,
		const char *prop, list_t **dest) {
	int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
	if (ret < 0) {
		sway_log(SWAY_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
		return ret;
	}
	if (sd_bus_message_at_end(msg, 0)) {
		return ret;
	}
	list_t *pixmaps = create_list();
	while (!sd_bus_message_at_end(msg, 0)) {
		ret = sd_bus_message_enter_container(msg, 'r', "iiay");
		if (ret < 0) {
			goto error;
		}
		int width, height;
		ret = sd_bus_message_read(msg, "ii", &width, &height);
		if (ret < 0) {
			goto error;
		}
		const void *pixels;
		size_t npixels;
		ret = sd_bus_message_read_array(msg, 'y', &pixels, &npixels);
		if (ret < 0) {
			goto error;
		}
		if (height > 0 && width == height && npixels >= (size_t)(width * height * 4)) {
			struct swaybar_pixmap *pixmap =
				malloc(sizeof(struct swaybar_pixmap) + npixels);
			pixmap->size = height;
			for (int i = 0; i < height * width; ++i) {
				((uint32_t *)pixmap->pixels)[i] = ntohl(((uint32_t *)pixels)[i]);
			}
			list_add(pixmaps, pixmap);
		}
		sd_bus_message_exit_container(msg);
	}
	if (pixmaps->length < 1) {
		goto error;
	}
	list_free_items_and_destroy(*dest);
	*dest = pixmaps;
	return ret;
error:
	list_free_items_and_destroy(pixmaps);
	return ret;
}

static int get_property_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni_slot *d = data;
	struct swaybar_sni *sni = d->sni;
	const char *prop = d->prop;
	const char *type = d->type;
	void *dest = d->dest;

	int ret;
	if (sd_bus_message_is_method_error(msg, NULL)) {
		ret = sd_bus_message_get_errno(msg);
		goto cleanup;
	}
	ret = sd_bus_message_enter_container(msg, 'v', type);
	if (ret < 0) {
		goto cleanup;
	}
	if (!type) {
		ret = read_pixmap(msg, sni, prop, dest);
		if (ret < 0) {
			goto cleanup;
		}
	} else {
		if (*type == 's' || *type == 'o') {
			free(*(char **)dest);
		}
		ret = sd_bus_message_read(msg, type, dest);
		if (ret < 0) {
			goto cleanup;
		}
		if (*type == 's' || *type == 'o') {
			char **str = dest;
			*str = strdup(*str);
		}
	}
	if (strcmp(prop, "Status") == 0 || (sni->status && (sni->status[0] == 'N' ?
				prop[0] == 'A' : has_prefix(prop, "Icon")))) {
		set_sni_dirty(sni);
	}
cleanup:
	wl_list_remove(&d->link);
	free(data);
	return ret;
}

static void sni_get_property_async(struct swaybar_sni *sni, const char *prop,
		const char *type, void *dest) {
	struct swaybar_sni_slot *data = calloc(1, sizeof(struct swaybar_sni_slot));
	data->sni = sni;
	data->prop = prop;
	data->type = type;
	data->dest = dest;
	int ret = sd_bus_call_method_async(sni->tray->bus, &data->slot, sni->service,
			sni->path, "org.freedesktop.DBus.Properties", "Get",
			get_property_callback, data, "ss", sni->interface, prop);
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &data->link);
	} else {
		free(data);
	}
}

static int sni_check_msg_sender(struct swaybar_sni *sni, sd_bus_message *msg) {
	bool has_well_known_names =
		sd_bus_creds_get_mask(sd_bus_message_get_creds(msg)) & SD_BUS_CREDS_WELL_KNOWN_NAMES;
	return sni->service[0] == ':' || has_well_known_names ? 1 : 0;
}

static int handle_new_icon(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
	if (!strcmp(sni->interface, "org.kde.StatusNotifierItem")) {
		sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
	}
	return sni_check_msg_sender(sni, msg);
}

static int handle_new_attention_icon(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
	return sni_check_msg_sender(sni, msg);
}

static int handle_new_status(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	int ret = sni_check_msg_sender(sni, msg);
	if (ret == 1) {
		char *status;
		int r = sd_bus_message_read(msg, "s", &status);
		if (r < 0) {
			ret = r;
		} else {
			free(sni->status);
			sni->status = strdup(status);
			set_sni_dirty(sni);
		}
	} else {
		sni_get_property_async(sni, "Status", "s", &sni->status);
	}
	return ret;
}

static int handle_new_title(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sni_get_property_async(sni, "Title", "s", &sni->title);
	return sni_check_msg_sender(sni, msg);
}

static void sni_match_signal_async(struct swaybar_sni *sni, char *signal,
		sd_bus_message_handler_t callback) {
	struct swaybar_sni_slot *slot = calloc(1, sizeof(struct swaybar_sni_slot));
	int ret = sd_bus_match_signal_async(sni->tray->bus, &slot->slot,
			sni->service, sni->path, sni->interface, signal, callback, NULL, sni);
	if (ret >= 0) {
		wl_list_insert(&sni->slots, &slot->link);
	} else {
		free(slot);
	}
}

struct swaybar_sni *create_sni(char *id, struct swaybar_tray *tray) {
	struct swaybar_sni *sni = calloc(1, sizeof(struct swaybar_sni));
	if (!sni) {
		return NULL;
	}
	sni->tray = tray;
	wl_list_init(&sni->slots);
	sni->watcher_id = strdup(id);
	char *path_ptr = strchr(id, '/');
	if (!path_ptr) {
		sni->service = strdup(id);
		sni->path = strdup("/StatusNotifierItem");
		sni->interface = "org.freedesktop.StatusNotifierItem";
	} else {
		sni->service = strndup(id, path_ptr - id);
		sni->path = strdup(path_ptr);
		sni->interface = "org.kde.StatusNotifierItem";
		sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
	}

	sni_get_property_async(sni, "Status", "s", &sni->status);
	sni_get_property_async(sni, "Title", "s", &sni->title);
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
	sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
	sni_get_property_async(sni, "Menu", "o", &sni->menu);

	sni_match_signal_async(sni, "NewIcon", handle_new_icon);
	sni_match_signal_async(sni, "NewAttentionIcon", handle_new_attention_icon);
	sni_match_signal_async(sni, "NewStatus", handle_new_status);
	sni_match_signal_async(sni, "NewTitle", handle_new_title);
	return sni;
}

void destroy_sni(struct swaybar_sni *sni) {
	if (!sni) {
		return;
	}
	if (sni->icon) {
		cairo_surface_destroy(sni->icon);
	}
	free(sni->watcher_id);
	free(sni->service);
	free(sni->path);
	free(sni->status);
	free(sni->icon_name);
	list_free_items_and_destroy(sni->icon_pixmap);
	free(sni->attention_icon_name);
	list_free_items_and_destroy(sni->attention_icon_pixmap);
	free(sni->menu);
	free(sni->icon_theme_path);
	free(sni->title);
	struct swaybar_sni_slot *slot, *slot_tmp;
	wl_list_for_each_safe(slot, slot_tmp, &sni->slots, link) {
		sd_bus_slot_unref(slot->slot);
		free(slot);
	}
	free(sni);
}

bool sni_visible(struct swaybar_sni *sni) {
	return sni_ready(sni) && !(sni->status && sni->status[0] == 'P');
}

cairo_surface_t *sni_icon(struct swaybar_sni *sni, int size, const char *icon_theme) {
	if (!sni_ready(sni)) {
		return NULL;
	}
	if (sni->icon && sni->icon_size == size) {
		return sni->icon;
	}
	if (sni->icon) {
		cairo_surface_destroy(sni->icon);
		sni->icon = NULL;
	}
	sni->icon_size = size;
	bool attention = sni->status && sni->status[0] == 'N';
	char *icon_name = attention ? sni->attention_icon_name : sni->icon_name;
	if (icon_name && *icon_name) {
		if (icon_name[0] == '/') {
			sni->icon = tw_icon_load_file(icon_name, size);
		} else {
			list_t *paths = create_list();
			list_cat(paths, sni->tray->basedirs);
			if (sni->icon_theme_path) {
				list_add(paths, sni->icon_theme_path);
			}
			int min_size, max_size;
			char *path = find_icon(sni->tray->themes, paths, icon_name, size,
				(char *)icon_theme, &min_size, &max_size);
			list_free(paths);
			if (path) {
				sni->icon = tw_icon_load_file(path, size);
				free(path);
			}
		}
		if (sni->icon) {
			return sni->icon;
		}
	}
	list_t *pixmaps = attention ? sni->attention_icon_pixmap : sni->icon_pixmap;
	if (pixmaps && pixmaps->length > 0) {
		struct swaybar_pixmap *best = NULL;
		int min_error = INT_MAX;
		for (int i = 0; i < pixmaps->length; ++i) {
			struct swaybar_pixmap *p = pixmaps->items[i];
			int e = abs(size - p->size);
			if (e < min_error) {
				best = p;
				min_error = e;
			}
		}
		cairo_surface_t *src = cairo_image_surface_create_for_data(best->pixels,
			CAIRO_FORMAT_ARGB32, best->size, best->size,
			cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, best->size));
		sni->icon = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
		cairo_t *cr = cairo_create(sni->icon);
		cairo_scale(cr, (double)size / best->size, (double)size / best->size);
		cairo_set_source_surface(cr, src, 0, 0);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(src);
	}
	return sni->icon;
}

void sni_click(struct swaybar_sni *sni, int x, int y, uint32_t button) {
	const char *method = button == BTN_LEFT ? "Activate" :
		button == BTN_MIDDLE ? "SecondaryActivate" :
		button == BTN_RIGHT ? "ContextMenu" : NULL;
	if (!method) {
		return;
	}
	if (sni->item_is_menu && strcmp(method, "Activate") == 0) {
		method = "ContextMenu";
	}
	sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->path,
		sni->interface, method, NULL, NULL, "ii", x, y);
}

void sni_scroll(struct swaybar_sni *sni, int direction) {
	sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->path,
		sni->interface, "Scroll", NULL, NULL, "is", direction, "vertical");
}
