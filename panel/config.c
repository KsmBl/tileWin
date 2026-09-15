#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "log.h"
#include "panel.h"
#include "stringop.h"
#include "tw_paths.h"

/*
 * Built-in taskbar layout used when ~/.config/tileWin/taskbar.conf is missing
 * or broken. Kept in sync with config/taskbar.conf.
 */
static const char default_config[] =
	"layout window {\n"
	"  position bottom\n"
	"  left start search taskbar\n"
	"  right tray keyboard volume network battery clock showdesktop\n"
	"}\n"
	"layout tile {\n"
	"  position top\n"
	"  height 26\n"
	"  left workspaces title\n"
	"  right tray cpu memory volume battery clock modeswitch\n"
	"}\n"
	"menu taskbar {\n"
	"  item \"Cascade windows\" arrange cascade\n"
	"  item \"Show windows stacked\" arrange vertical\n"
	"  item \"Show windows side by side\" arrange horizontal\n"
	"  item \"Arrange windows optimally\" arrange optimal\n"
	"  separator\n"
	"  item \"Show the desktop\" showdesktop\n"
	"  separator\n"
	"  item \"Switch to tile mode\" wm_mode toggle\n"
	"  item \"Task Manager\" exec xfce4-terminal -e btop\n"
	"  item \"Taskbar settings\" exec tilewin-settings --page taskbar\n"
	"}\n";

char *panel_default_config_path(void) {
	char *dir = tw_config_dir();
	if (dir) {
		char *path = format_str("%s/taskbar.conf", dir);
		free(dir);
		if (access(path, R_OK) == 0) {
			return path;
		}
		free(path);
	}
	char *path = format_str("%s/config/taskbar.conf", tw_data_dir());
	if (access(path, R_OK) == 0) {
		return path;
	}
	free(path);
	return NULL;
}

struct menu_item *menu_item_new(const char *label, const char *command) {
	struct menu_item *item = calloc(1, sizeof(*item));
	item->label = label ? strdup(label) : NULL;
	item->command = command ? strdup(command) : NULL;
	return item;
}

struct menu_item *menu_item_separator(void) {
	struct menu_item *item = calloc(1, sizeof(*item));
	item->separator = true;
	return item;
}

void menu_items_free(list_t *items) {
	if (!items) {
		return;
	}
	for (int i = 0; i < items->length; i++) {
		struct menu_item *item = items->items[i];
		free(item->label);
		free(item->command);
		free(item->icon);
		menu_items_free(item->children);
		free(item);
	}
	list_free(items);
}

/*
 *   item "Label" [icon <name>] [disabled] [checked] [bold] <command...>
 *   separator
 *   submenu "Label" [icon <name>] { ... }
 */
list_t *menu_items_parse(struct twconf_node *node) {
	list_t *items = create_list();
	for (int i = 0; i < twconf_count(node); i++) {
		struct twconf_node *child = twconf_at(node, i);
		if (strcmp(child->name, "separator") == 0) {
			list_add(items, menu_item_separator());
			continue;
		}
		bool submenu = strcmp(child->name, "submenu") == 0;
		if (!submenu && strcmp(child->name, "item") != 0) {
			sway_log(SWAY_ERROR, "Unknown menu entry '%s' (line %d)",
				child->name, child->line);
			continue;
		}
		if (child->argc < 1) {
			continue;
		}
		struct menu_item *item = menu_item_new(child->argv[0], NULL);
		int arg = 1;
		while (arg < child->argc) {
			if (strcmp(child->argv[arg], "icon") == 0 && arg + 1 < child->argc) {
				free(item->icon);
				item->icon = strdup(child->argv[arg + 1]);
				arg += 2;
			} else if (strcmp(child->argv[arg], "disabled") == 0) {
				item->disabled = true;
				arg++;
			} else if (strcmp(child->argv[arg], "checked") == 0) {
				item->checked = true;
				arg++;
			} else if (strcmp(child->argv[arg], "bold") == 0) {
				item->bold = true;
				arg++;
			} else {
				break;
			}
		}
		if (submenu) {
			item->children = child->children ? menu_items_parse(child) : create_list();
		} else if (arg < child->argc) {
			item->command = twconf_join(child, arg);
			// configs copied from older defaults opened the file in an editor
			if (strcmp(item->command, "exec xdg-open ~/.config/tileWin/taskbar.conf") == 0) {
				free(item->command);
				item->command = strdup("exec tilewin-settings --page taskbar");
			}
		}
		list_add(items, item);
	}
	return items;
}

static void add_widget_names(struct panel *panel, struct panel_config *config,
		list_t *target, struct twconf_node *node) {
	if (!node) {
		return;
	}
	for (int i = 0; i < node->argc; i++) {
		const char *name = node->argv[i];
		struct widget *found = NULL;
		for (int j = 0; j < config->widgets->length; j++) {
			struct widget *w = config->widgets->items[j];
			if (strcmp(w->name, name) == 0) {
				found = w;
				break;
			}
		}
		if (!found) {
			struct twconf_node *def = NULL;
			for (int j = 0; j < twconf_count(config->root); j++) {
				struct twconf_node *child = twconf_at(config->root, j);
				if (strcmp(child->name, "widget") == 0 && child->argc >= 1 &&
						strcmp(child->argv[0], name) == 0) {
					def = child;
				}
			}
			found = widget_create(panel, name, def);
			if (!found) {
				sway_log(SWAY_ERROR, "Unknown widget '%s' in taskbar config", name);
				continue;
			}
			list_add(config->widgets, found);
		}
		list_add(target, found);
	}
}

static void parse_layout(struct panel *panel, struct panel_config *config,
		struct layout_config *layout, struct twconf_node *node, bool default_bottom) {
	layout->left = create_list();
	layout->center = create_list();
	layout->right = create_list();
	layout->bottom = default_bottom;
	if (!node) {
		return;
	}
	const char *position = twconf_value(node, "position");
	if (position) {
		layout->bottom = strcasecmp(position, "top") != 0;
	}
	const char *height = twconf_value(node, "height");
	if (height) {
		layout->height = atoi(height);
	}
	add_widget_names(panel, config, layout->left, twconf_child(node, "left"));
	add_widget_names(panel, config, layout->center, twconf_child(node, "center"));
	add_widget_names(panel, config, layout->right, twconf_child(node, "right"));
}

struct panel_config *panel_config_load(struct panel *panel, const char *path) {
	char *error = NULL;
	struct twconf_node *root = path ? twconf_parse_file(path, &error) :
		twconf_parse_string(default_config, "<built-in>", &error);
	if (!root) {
		sway_log(SWAY_ERROR, "Taskbar config error: %s", error ? error : "?");
		free(error);
		return NULL;
	}

	struct panel_config *config = calloc(1, sizeof(*config));
	config->root = root;
	config->widgets = create_list();
	config->tooltip_delay = 600;
	const char *font = twconf_value(root, "font");
	config->font = font ? strdup(font) : NULL;
	const char *terminal = twconf_value(root, "terminal");
	config->terminal = strdup(terminal ? terminal : "xfce4-terminal -x");
	config->desktop_icons = twconf_parse_bool(twconf_value(root, "desktop_icons"), true);
	const char *delay = twconf_value(root, "tooltip_delay");
	if (delay) {
		config->tooltip_delay = atoi(delay);
	}
	struct twconf_node *outputs = twconf_child(root, "outputs");
	if (outputs && outputs->argc > 0) {
		config->outputs = create_list();
		for (int i = 0; i < outputs->argc; i++) {
			list_add(config->outputs, strdup(outputs->argv[i]));
		}
	}

	struct twconf_node *tile = NULL, *window = NULL;
	for (int i = 0; i < twconf_count(root); i++) {
		struct twconf_node *child = twconf_at(root, i);
		if (strcmp(child->name, "layout") == 0 && child->argc >= 1) {
			if (strcmp(child->argv[0], "tile") == 0) {
				tile = child;
			} else if (strcmp(child->argv[0], "window") == 0) {
				window = child;
			}
		}
	}
	if (!window && !tile) {
		// fall back to the built-in layouts but keep the user's widgets/menus
		struct twconf_node *builtin = twconf_parse_string(default_config, "<built-in>", NULL);
		for (int i = 0; builtin && i < twconf_count(builtin); i++) {
			struct twconf_node *child = twconf_at(builtin, i);
			if (strcmp(child->name, "layout") == 0 || (strcmp(child->name, "menu") == 0 &&
					!twconf_child(root, "menu"))) {
				list_add(root->children, child);
				builtin->children->items[i] = NULL;
				if (strcmp(child->argv[0], "tile") == 0) {
					tile = child;
				} else if (strcmp(child->argv[0], "window") == 0) {
					window = child;
				}
			}
		}
		if (builtin) {
			for (int i = 0; i < builtin->children->length; i++) {
				twconf_free(builtin->children->items[i]);
			}
			builtin->children->length = 0;
			twconf_free(builtin);
		}
	}
	parse_layout(panel, config, &config->layouts[LAYOUT_WINDOW], window ? window : tile, true);
	parse_layout(panel, config, &config->layouts[LAYOUT_TILE], tile ? tile : window, false);
	config->startmenu = twconf_child(root, "startmenu");
	return config;
}

void panel_config_free(struct panel_config *config) {
	if (!config) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		list_free(config->layouts[i].left);
		list_free(config->layouts[i].center);
		list_free(config->layouts[i].right);
	}
	for (int i = 0; i < config->widgets->length; i++) {
		widget_destroy(config->widgets->items[i]);
	}
	list_free(config->widgets);
	if (config->outputs) {
		list_free_items_and_destroy(config->outputs);
	}
	free(config->font);
	free(config->terminal);
	twconf_free(config->root);
	free(config);
}

/* ---------- widget registry ---------- */

static const struct widget_impl *impls[] = {
	&widget_start, &widget_taskbar, &widget_quicklaunch, &widget_workspaces,
	&widget_title, &widget_tray, &widget_clock, &widget_volume, &widget_battery,
	&widget_network, &widget_cpu, &widget_memory, &widget_brightness,
	&widget_keyboard, &widget_modeswitch, &widget_showdesktop, &widget_search,
	&widget_separator, &widget_spacer, &widget_custom,
};

const struct widget_impl *widget_impl_find(const char *type) {
	for (size_t i = 0; i < sizeof(impls) / sizeof(impls[0]); i++) {
		if (strcmp(impls[i]->type, type) == 0) {
			return impls[i];
		}
	}
	return NULL;
}

static struct twconf_node *find_menu(struct twconf_node *root, const char *name) {
	for (int i = 0; i < twconf_count(root); i++) {
		struct twconf_node *child = twconf_at(root, i);
		if (strcmp(child->name, "menu") == 0 && child->argc >= 1 &&
				strcmp(child->argv[0], name) == 0) {
			return child;
		}
	}
	return NULL;
}

struct widget *widget_create(struct panel *panel, const char *name,
		struct twconf_node *conf) {
	const char *type = conf ? twconf_value(conf, "type") : NULL;
	char *type_buf = NULL;
	if (!type) {
		const char *colon = strchr(name, ':');
		type_buf = colon ? strndup(name, colon - name) : strdup(name);
		type = type_buf;
	}
	const struct widget_impl *impl = widget_impl_find(type);
	free(type_buf);
	if (!impl) {
		return NULL;
	}
	struct widget *w = calloc(1, sizeof(*w));
	w->impl = impl;
	w->panel = panel;
	w->name = strdup(name);
	w->conf = conf;
	const char *v;
	if ((v = widget_conf(w, "on_click", NULL))) {
		w->on_click = strdup(v);
	}
	if ((v = widget_conf(w, "on_middle_click", NULL))) {
		w->on_middle_click = strdup(v);
	}
	if ((v = widget_conf(w, "on_right_click", NULL))) {
		w->on_right_click = strdup(v);
	}
	if ((v = widget_conf(w, "on_scroll_up", NULL))) {
		w->on_scroll_up = strdup(v);
	}
	if ((v = widget_conf(w, "on_scroll_down", NULL))) {
		w->on_scroll_down = strdup(v);
	}
	struct twconf_node *menu = conf ? twconf_child(conf, "menu") : NULL;
	if (!menu && panel->config) {
		menu = NULL;
	}
	if (menu && menu->children) {
		w->menu = menu_items_parse(menu);
	}
	if (impl->init) {
		impl->init(w);
	}
	return w;
}

/* Menus declared at the top level ("menu taskbar { ... }") by name. */
list_t *panel_named_menu(struct panel *panel, const char *name);
list_t *panel_named_menu(struct panel *panel, const char *name) {
	if (!panel->config) {
		return NULL;
	}
	struct twconf_node *node = find_menu(panel->config->root, name);
	return node && node->children ? menu_items_parse(node) : NULL;
}

void widget_destroy(struct widget *w) {
	if (!w) {
		return;
	}
	if (w->impl->destroy) {
		w->impl->destroy(w);
	}
	free(w->name);
	free(w->on_click);
	free(w->on_middle_click);
	free(w->on_right_click);
	free(w->on_scroll_up);
	free(w->on_scroll_down);
	menu_items_free(w->menu);
	free(w);
}

static const char *widget_conf_own(struct widget *w, const char *key, const char *fallback);

const char *widget_conf(struct widget *w, const char *key, const char *fallback) {
	const char *value = widget_conf_own(w, key, NULL);
	if (value) {
		return value;
	}
	// a theme can give widgets a format, e.g. cpu { format "CPU {usage}%" }
	if (strcmp(key, "format") == 0 && w->panel && w->panel->theme) {
		char theme_key[64];
		snprintf(theme_key, sizeof(theme_key), "%s.format", w->impl->type);
		return tw_theme_str(w->panel->theme, theme_key, fallback);
	}
	return fallback;
}

static const char *widget_conf_own(struct widget *w, const char *key, const char *fallback) {
	struct twconf_node *node = w->conf ? twconf_child(w->conf, key) : NULL;
	if (!node || node->argc == 0) {
		return fallback;
	}
	// commands and formats may consist of several words
	if (node->argc > 1) {
		static char *joined = NULL;
		free(joined);
		joined = twconf_join(node, 0);
		return joined;
	}
	return node->argv[0];
}

int widget_conf_int(struct widget *w, const char *key, int fallback) {
	const char *v = widget_conf(w, key, NULL);
	return v ? atoi(v) : fallback;
}

bool widget_conf_bool(struct widget *w, const char *key, bool fallback) {
	return twconf_parse_bool(widget_conf(w, key, NULL), fallback);
}
