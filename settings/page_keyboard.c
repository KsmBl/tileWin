#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/*
 * Keyboard page: layouts, xkb options and key repeat live in the
 * "input type:keyboard" block of common.conf; shortcuts are the bindsym lines
 * of windowmode.conf and tilemode.conf.
 */

#define EVDEV_LST "/usr/share/X11/xkb/rules/evdev.lst"
#define KEYBOARD_BLOCK "type:keyboard"

struct xkb_entry {
	char *layout; // for variants: the layout they belong to
	char *name;
	char *description;
};

struct option_choice {
	const char *option;
	const char *title;
};

static const struct option_choice switch_choices[] = {
	{ NULL, "No shortcut" },
	{ "grp:win_space_toggle", "Super+Space" },
	{ "grp:alt_shift_toggle", "Alt+Shift" },
	{ "grp:ctrl_shift_toggle", "Ctrl+Shift" },
	{ "grp:caps_toggle", "Caps Lock" },
	{ "grp:toggle", "Right Alt" },
	{ 0 },
};

static const struct option_choice caps_choices[] = {
	{ NULL, "Caps Lock" },
	{ "ctrl:nocaps", "Ctrl" },
	{ "caps:escape", "Escape" },
	{ "caps:swapescape", "Swap with Escape" },
	{ "caps:none", "Disabled" },
	{ 0 },
};

static const struct option_choice compose_choices[] = {
	{ NULL, "No compose key" },
	{ "compose:ralt", "Right Alt" },
	{ "compose:rctrl", "Right Ctrl" },
	{ "compose:menu", "Menu key" },
	{ "compose:caps", "Caps Lock" },
	{ 0 },
};

struct keyboard_page {
	struct settings *s;
	bool updating;
	GPtrArray *layouts;  // struct xkb_entry *, sorted by description
	GPtrArray *variants; // struct xkb_entry *
	GtkStringList *layout_model;
	GtkWidget *layouts_list;
	GPtrArray *cur_layouts, *cur_variants; // char *, same length
	GtkWidget *switch_dd, *caps_dd, *compose_dd, *numlock_switch;
	GtkWidget *delay_spin, *rate_spin;
	GtkWidget *mode_dd, *binds_box, *search;
	guint layout_rebuild_id, binds_rebuild_id;
};

static struct confdoc *common(struct keyboard_page *p) {
	return p->s->common;
}

/* ---------- xkb data ---------- */

static void xkb_entry_free(gpointer data) {
	struct xkb_entry *e = data;
	g_free(e->layout);
	g_free(e->name);
	g_free(e->description);
	g_free(e);
}

static int xkb_entry_cmp(gconstpointer a, gconstpointer b) {
	const struct xkb_entry *ea = *(struct xkb_entry **)a;
	const struct xkb_entry *eb = *(struct xkb_entry **)b;
	return g_utf8_collate(ea->description, eb->description);
}

static void load_xkb(struct keyboard_page *p) {
	p->layouts = g_ptr_array_new_with_free_func(xkb_entry_free);
	p->variants = g_ptr_array_new_with_free_func(xkb_entry_free);
	FILE *f = fopen(EVDEV_LST, "r");
	if (!f) {
		return;
	}
	char line[512];
	int section = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '!') {
			section = g_str_has_prefix(line, "! layout") ? 1 :
				g_str_has_prefix(line, "! variant") ? 2 : 0;
			continue;
		}
		g_strstrip(line);
		if (!section || !*line) {
			continue;
		}
		char *desc = line;
		while (*desc && !g_ascii_isspace(*desc)) {
			desc++;
		}
		if (!*desc) {
			continue;
		}
		*desc++ = '\0';
		while (g_ascii_isspace(*desc)) {
			desc++;
		}
		struct xkb_entry *e = g_new0(struct xkb_entry, 1);
		e->name = g_strdup(line);
		if (section == 2) {
			char *colon = strstr(desc, ": ");
			if (!colon) {
				xkb_entry_free(e);
				continue;
			}
			e->layout = g_strndup(desc, colon - desc);
			e->description = g_strdup(colon + 2);
			g_ptr_array_add(p->variants, e);
		} else {
			e->description = g_strdup(desc);
			g_ptr_array_add(p->layouts, e);
		}
	}
	fclose(f);
	g_ptr_array_sort(p->layouts, xkb_entry_cmp);
	p->layout_model = gtk_string_list_new(NULL);
	for (guint i = 0; i < p->layouts->len; i++) {
		struct xkb_entry *e = p->layouts->pdata[i];
		char *label = g_strdup_printf("%s (%s)", e->description, e->name);
		gtk_string_list_append(p->layout_model, label);
		g_free(label);
	}
}

static int layout_index(struct keyboard_page *p, const char *name) {
	for (guint i = 0; i < p->layouts->len; i++) {
		struct xkb_entry *e = p->layouts->pdata[i];
		if (strcmp(e->name, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* ---------- reading and writing the keyboard block ---------- */

static char *keyboard_value(struct keyboard_page *p, const char *key) {
	struct cstmt *block = confdoc_block(common(p), "input", KEYBOARD_BLOCK, false);
	return cstmt_raw_args(common(p), confdoc_child(block, key, NULL));
}

static void set_keyboard_value(struct keyboard_page *p, const char *key, const char *value) {
	struct confdoc *d = common(p);
	struct cstmt *block = confdoc_block(d, "input", KEYBOARD_BLOCK, value != NULL);
	if (block) {
		confdoc_set(d, block, key, NULL, value);
	}
}

static GPtrArray *split_list(const char *value) {
	GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
	if (value && *value) {
		char **parts = g_strsplit(value, ",", -1);
		for (int i = 0; parts[i]; i++) {
			g_ptr_array_add(items, g_strstrip(g_strdup(parts[i])));
		}
		g_strfreev(parts);
	}
	return items;
}

static char *join_list(GPtrArray *items) {
	GString *out = g_string_new(NULL);
	for (guint i = 0; i < items->len; i++) {
		if (i > 0) {
			g_string_append_c(out, ',');
		}
		g_string_append(out, items->pdata[i]);
	}
	return g_string_free(out, FALSE);
}

static void read_layouts(struct keyboard_page *p) {
	if (p->cur_layouts) {
		g_ptr_array_unref(p->cur_layouts);
		g_ptr_array_unref(p->cur_variants);
	}
	char *layouts = keyboard_value(p, "xkb_layout");
	char *variants = keyboard_value(p, "xkb_variant");
	p->cur_layouts = split_list(layouts);
	p->cur_variants = split_list(variants);
	if (p->cur_layouts->len == 0) {
		g_ptr_array_add(p->cur_layouts, g_strdup("us"));
	}
	while (p->cur_variants->len < p->cur_layouts->len) {
		g_ptr_array_add(p->cur_variants, g_strdup(""));
	}
	g_ptr_array_set_size(p->cur_variants, p->cur_layouts->len);
	g_free(layouts);
	g_free(variants);
}

static void write_layouts(struct keyboard_page *p) {
	char *layouts = join_list(p->cur_layouts);
	bool any_variant = false;
	for (guint i = 0; i < p->cur_variants->len; i++) {
		any_variant |= ((char *)p->cur_variants->pdata[i])[0] != '\0';
	}
	char *variants = any_variant ? join_list(p->cur_variants) : NULL;
	set_keyboard_value(p, "xkb_layout", layouts);
	set_keyboard_value(p, "xkb_variant", variants);
	// layout and variant lists must change together, so reload instead of
	// sending the commands one by one
	settings_common_changed(p->s, true);
	settings_status(p->s, "Keyboard layout: %s%s%s", layouts, variants ? " / " : "",
		variants ? variants : "");
	g_free(layouts);
	g_free(variants);
}

/* ---------- layouts list ---------- */

static void rebuild_layouts(struct keyboard_page *p);

static gboolean rebuild_layouts_idle(gpointer data) {
	struct keyboard_page *p = data;
	p->layout_rebuild_id = 0;
	rebuild_layouts(p);
	return G_SOURCE_REMOVE;
}

static void schedule_layouts(struct keyboard_page *p) {
	if (!p->layout_rebuild_id) {
		p->layout_rebuild_id = g_idle_add(rebuild_layouts_idle, p);
	}
}

enum {
	LAYOUT_UP,
	LAYOUT_DOWN,
	LAYOUT_REMOVE,
};

struct layout_action {
	struct keyboard_page *p;
	guint index;
	int op;
};

static void swap_items(GPtrArray *array, guint a, guint b) {
	gpointer tmp = array->pdata[a];
	array->pdata[a] = array->pdata[b];
	array->pdata[b] = tmp;
}

static void on_layout_action(GtkButton *button, gpointer data) {
	struct layout_action *a = data;
	struct keyboard_page *p = a->p;
	guint i = a->index;
	if (i >= p->cur_layouts->len) {
		return;
	}
	if (a->op == LAYOUT_UP && i > 0) {
		swap_items(p->cur_layouts, i, i - 1);
		swap_items(p->cur_variants, i, i - 1);
	} else if (a->op == LAYOUT_DOWN && i + 1 < p->cur_layouts->len) {
		swap_items(p->cur_layouts, i, i + 1);
		swap_items(p->cur_variants, i, i + 1);
	} else if (a->op == LAYOUT_REMOVE && p->cur_layouts->len > 1) {
		g_ptr_array_remove_index(p->cur_layouts, i);
		g_ptr_array_remove_index(p->cur_variants, i);
	} else {
		return;
	}
	write_layouts(p);
	schedule_layouts(p);
}

static void on_layout_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct keyboard_page *p = data;
	if (p->updating) {
		return;
	}
	guint row = GPOINTER_TO_UINT(g_object_get_data(dropdown, "row"));
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (row >= p->cur_layouts->len || sel >= p->layouts->len) {
		return;
	}
	struct xkb_entry *e = p->layouts->pdata[sel];
	g_free(p->cur_layouts->pdata[row]);
	p->cur_layouts->pdata[row] = g_strdup(e->name);
	g_free(p->cur_variants->pdata[row]);
	p->cur_variants->pdata[row] = g_strdup("");
	write_layouts(p);
	schedule_layouts(p); // new variant list
}

static void on_variant_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct keyboard_page *p = data;
	if (p->updating) {
		return;
	}
	guint row = GPOINTER_TO_UINT(g_object_get_data(dropdown, "row"));
	GPtrArray *names = g_object_get_data(dropdown, "variants");
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
	if (row >= p->cur_variants->len || !names || sel >= names->len) {
		return;
	}
	g_free(p->cur_variants->pdata[row]);
	p->cur_variants->pdata[row] = g_strdup(names->pdata[sel]);
	write_layouts(p);
}

static void on_add_layout(GtkButton *button, gpointer data) {
	struct keyboard_page *p = data;
	g_ptr_array_add(p->cur_layouts, g_strdup("us"));
	g_ptr_array_add(p->cur_variants, g_strdup(""));
	write_layouts(p);
	schedule_layouts(p);
}

static void add_layout_button(struct keyboard_page *p, GtkWidget *box, const char *icon,
		const char *tooltip, bool sensitive, guint index, int op) {
	struct layout_action *a = g_new0(struct layout_action, 1);
	a->p = p;
	a->index = index;
	a->op = op;
	gtk_box_append(GTK_BOX(box), ui_icon_button(icon, tooltip, sensitive,
		G_CALLBACK(on_layout_action), a));
}

static void rebuild_layouts(struct keyboard_page *p) {
	p->updating = true;
	gtk_list_box_remove_all(GTK_LIST_BOX(p->layouts_list));
	GtkExpression *expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
	for (guint i = 0; i < p->cur_layouts->len; i++) {
		const char *layout = p->cur_layouts->pdata[i];
		const char *variant = p->cur_variants->pdata[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(box, 12);
		gtk_widget_set_margin_end(box, 8);
		gtk_widget_set_margin_top(box, 6);
		gtk_widget_set_margin_bottom(box, 6);

		GtkWidget *layout_dd = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(p->layout_model)),
			gtk_expression_ref(expression));
		gtk_drop_down_set_enable_search(GTK_DROP_DOWN(layout_dd), TRUE);
		gtk_widget_set_hexpand(layout_dd, TRUE);
		int index = layout_index(p, layout);
		if (index >= 0) {
			gtk_drop_down_set_selected(GTK_DROP_DOWN(layout_dd), index);
		}
		g_object_set_data(G_OBJECT(layout_dd), "row", GUINT_TO_POINTER(i));
		g_signal_connect(layout_dd, "notify::selected", G_CALLBACK(on_layout_selected), p);
		gtk_box_append(GTK_BOX(box), layout_dd);

		GtkStringList *variant_model = gtk_string_list_new(NULL);
		GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
		gtk_string_list_append(variant_model, "Default");
		g_ptr_array_add(names, g_strdup(""));
		guint selected = 0;
		for (guint v = 0; v < p->variants->len; v++) {
			struct xkb_entry *e = p->variants->pdata[v];
			if (strcmp(e->layout, layout) != 0) {
				continue;
			}
			gtk_string_list_append(variant_model, e->description);
			g_ptr_array_add(names, g_strdup(e->name));
			if (strcmp(e->name, variant) == 0) {
				selected = names->len - 1;
			}
		}
		GtkWidget *variant_dd = gtk_drop_down_new(G_LIST_MODEL(variant_model),
			gtk_expression_ref(expression));
		gtk_drop_down_set_enable_search(GTK_DROP_DOWN(variant_dd), names->len > 12);
		gtk_widget_set_size_request(variant_dd, 220, -1);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(variant_dd), selected);
		g_object_set_data(G_OBJECT(variant_dd), "row", GUINT_TO_POINTER(i));
		g_object_set_data_full(G_OBJECT(variant_dd), "variants", names,
			(GDestroyNotify)g_ptr_array_unref);
		g_signal_connect(variant_dd, "notify::selected", G_CALLBACK(on_variant_selected), p);
		gtk_box_append(GTK_BOX(box), variant_dd);

		add_layout_button(p, box, "go-up-symbolic", "Move up", i > 0, i, LAYOUT_UP);
		add_layout_button(p, box, "go-down-symbolic", "Move down", i + 1 < p->cur_layouts->len,
			i, LAYOUT_DOWN);
		add_layout_button(p, box, "list-remove-symbolic", "Remove", p->cur_layouts->len > 1, i,
			LAYOUT_REMOVE);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(p->layouts_list), row);
	}
	gtk_expression_unref(expression);
	GtkWidget *add = gtk_button_new_with_label("Add layout");
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_layout), p);
	ui_row(p->layouts_list, NULL, p->cur_layouts->len > 1 ?
		"The first layout is used at login." : NULL, add);
	p->updating = false;
}

/* ---------- xkb options and repeat ---------- */

static bool option_in(const char *option, const struct option_choice *choices) {
	for (int i = 1; choices[i].title; i++) {
		if (strcmp(option, choices[i].option) == 0) {
			return true;
		}
	}
	return false;
}

static guint choice_index(GPtrArray *options, const struct option_choice *choices) {
	for (guint i = 0; i < options->len; i++) {
		for (int c = 1; choices[c].title; c++) {
			if (strcmp(options->pdata[i], choices[c].option) == 0) {
				return (guint)c;
			}
		}
	}
	return 0;
}

static void write_options(struct keyboard_page *p) {
	char *value = keyboard_value(p, "xkb_options");
	GPtrArray *options = split_list(value);
	g_free(value);
	GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < options->len; i++) {
		const char *option = options->pdata[i];
		if (*option && !option_in(option, switch_choices) && !option_in(option, caps_choices) &&
				!option_in(option, compose_choices)) {
			g_ptr_array_add(result, g_strdup(option)); // keep options set by hand
		}
	}
	const struct {
		GtkWidget *dd;
		const struct option_choice *choices;
	} groups[] = {
		{ p->switch_dd, switch_choices },
		{ p->caps_dd, caps_choices },
		{ p->compose_dd, compose_choices },
	};
	for (size_t g = 0; g < G_N_ELEMENTS(groups); g++) {
		guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(groups[g].dd));
		if (sel > 0 && groups[g].choices[sel].option) {
			g_ptr_array_add(result, g_strdup(groups[g].choices[sel].option));
		}
	}
	char *joined = result->len ? join_list(result) : NULL;
	set_keyboard_value(p, "xkb_options", joined);
	if (joined) {
		settings_command(p->s, "input " KEYBOARD_BLOCK " xkb_options %s", joined);
	} else {
		settings_common_changed(p->s, true);
	}
	settings_common_changed(p->s, false);
	g_free(joined);
	g_ptr_array_unref(result);
	g_ptr_array_unref(options);
}

static void on_option_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	struct keyboard_page *p = data;
	if (!p->updating) {
		write_options(p);
	}
}

static gboolean on_numlock(GtkSwitch *widget, gboolean active, gpointer data) {
	struct keyboard_page *p = data;
	if (!p->updating) {
		const char *value = active ? "enabled" : "disabled";
		set_keyboard_value(p, "xkb_numlock", value);
		settings_common_changed(p->s, false);
		settings_command(p->s, "input " KEYBOARD_BLOCK " xkb_numlock %s", value);
	}
	return FALSE;
}

static void on_repeat_changed(GtkSpinButton *spin, gpointer data) {
	struct keyboard_page *p = data;
	if (p->updating) {
		return;
	}
	const char *key = g_object_get_data(G_OBJECT(spin), "key");
	char value[16];
	snprintf(value, sizeof(value), "%d", gtk_spin_button_get_value_as_int(spin));
	set_keyboard_value(p, key, value);
	settings_common_changed(p->s, false);
	settings_command(p->s, "input " KEYBOARD_BLOCK " %s %s", key, value);
}

static GtkWidget *choice_dropdown(struct keyboard_page *p, const struct option_choice *choices) {
	GtkStringList *model = gtk_string_list_new(NULL);
	for (int i = 0; choices[i].title; i++) {
		gtk_string_list_append(model, choices[i].title);
	}
	GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_option_selected), p);
	return dd;
}

/* ---------- shortcuts ---------- */

static struct confdoc *binds_doc(struct keyboard_page *p) {
	return gtk_drop_down_get_selected(GTK_DROP_DOWN(p->mode_dd)) == 1 ?
		p->s->tilemode : p->s->windowmode;
}

static struct cstmt *nth_binding(struct confdoc *d, int n) {
	int count = 0;
	for (guint i = 0; i < d->root->children->len; i++) {
		struct cstmt *c = d->root->children->pdata[i];
		if (strcmp(c->name, "bindsym") == 0 && c->args->len >= 2) {
			if (count++ == n) {
				return c;
			}
		}
	}
	return NULL;
}

/* Splits "bindsym [--flags] <keys> <command>" into its parts. */
static void parse_binding(struct confdoc *d, struct cstmt *stmt, char **flags, char **keys,
		char **command) {
	char *raw = cstmt_raw_args(d, stmt);
	GString *flag_text = g_string_new(NULL);
	char *pos = raw;
	while (g_str_has_prefix(pos, "--")) {
		char *end = pos;
		while (*end && !g_ascii_isspace(*end)) {
			end++;
		}
		g_string_append_len(flag_text, pos, end - pos);
		g_string_append_c(flag_text, ' ');
		pos = end;
		while (g_ascii_isspace(*pos)) {
			pos++;
		}
	}
	char *end = pos;
	while (*end && !g_ascii_isspace(*end)) {
		end++;
	}
	*keys = g_strndup(pos, end - pos);
	while (g_ascii_isspace(*end)) {
		end++;
	}
	*command = g_strdup(end);
	*flags = g_string_free(flag_text, FALSE);
	g_free(raw);
}

/* Writes a binding: replaces the index-th one, or appends one for index < 0. */
static void set_binding(struct keyboard_page *p, int index, const char *flags, const char *keys,
		const char *command) {
	struct confdoc *d = binds_doc(p);
	char *text = g_strdup_printf("bindsym %s%s %s", flags ? flags : "", keys, command);
	struct cstmt *stmt = index >= 0 ? nth_binding(d, index) : NULL;
	if (stmt) {
		confdoc_replace(d, stmt, text);
	} else {
		confdoc_append(d, d->root, text);
	}
	g_free(text);
	settings_mode_changed(p->s, d);
}

static void schedule_binds(struct keyboard_page *p);

/* ---------- names for people ---------- */

static const struct {
	const char *from, *to;
} key_names[] = {
	{ "$mod", "Win" }, { "Mod4", "Win" }, { "Super", "Win" }, { "Super_L", "Win" },
	{ "Super_R", "Right Win" }, { "Mod1", "Alt" }, { "Alt", "Alt" }, { "Control", "Ctrl" },
	{ "Ctrl", "Ctrl" }, { "Shift", "Shift" }, { "Return", "Enter" }, { "KP_Enter", "Enter" },
	{ "Prior", "Page Up" }, { "Page_Up", "Page Up" }, { "Next", "Page Down" },
	{ "Page_Down", "Page Down" }, { "Escape", "Esc" }, { "space", "Space" },
	{ "Print", "Print Screen" }, { "BackSpace", "Backspace" }, { "minus", "-" },
	{ "plus", "+" }, { "equal", "=" }, { "comma", "," }, { "period", "." },
	{ "slash", "/" }, { "grave", "`" }, { "Tab", "Tab" }, { "Delete", "Delete" },
	{ "XF86AudioRaiseVolume", "Volume Up" }, { "XF86AudioLowerVolume", "Volume Down" },
	{ "XF86AudioMute", "Mute" }, { "XF86AudioMicMute", "Microphone Mute" },
	{ "XF86MonBrightnessUp", "Brightness Up" }, { "XF86MonBrightnessDown", "Brightness Down" },
	{ "XF86AudioPlay", "Play/Pause" }, { "XF86AudioPause", "Pause" },
	{ "XF86AudioNext", "Next Track" }, { "XF86AudioPrev", "Previous Track" },
	{ "XF86AudioStop", "Stop" },
};

/* $mod+Shift+Return -> Win + Shift + Enter */
static char *display_keys(const char *keys) {
	gchar **parts = g_strsplit(keys, "+", -1);
	GString *out = g_string_new(NULL);
	for (int i = 0; parts[i]; i++) {
		const char *name = parts[i];
		for (size_t k = 0; k < G_N_ELEMENTS(key_names); k++) {
			if (g_ascii_strcasecmp(parts[i], key_names[k].from) == 0) {
				name = key_names[k].to;
				break;
			}
		}
		if (out->len) {
			g_string_append(out, " + ");
		}
		if (strlen(name) == 1) {
			g_string_append_c(out, g_ascii_toupper(name[0]));
		} else {
			g_string_append(out, name);
		}
	}
	g_strfreev(parts);
	return g_string_free(out, FALSE);
}

/* Keys in a comparable form: modifiers sorted, aliases resolved, lower case. */
static char *normalize_keys(const char *keys) {
	gchar **parts = g_strsplit(keys, "+", -1);
	bool super = false, ctrl = false, alt = false, shift = false;
	GString *rest = g_string_new(NULL);
	for (int i = 0; parts[i]; i++) {
		char *lower = g_ascii_strdown(parts[i], -1);
		if (!strcmp(lower, "$mod") || !strcmp(lower, "mod4") || !strcmp(lower, "super")) {
			super = true;
		} else if (!strcmp(lower, "control") || !strcmp(lower, "ctrl")) {
			ctrl = true;
		} else if (!strcmp(lower, "mod1") || !strcmp(lower, "alt")) {
			alt = true;
		} else if (!strcmp(lower, "shift")) {
			shift = true;
		} else {
			if (!strcmp(lower, "page_up")) {
				g_free(lower);
				lower = g_strdup("prior");
			} else if (!strcmp(lower, "page_down")) {
				g_free(lower);
				lower = g_strdup("next");
			}
			g_string_append(rest, lower);
		}
		g_free(lower);
	}
	g_strfreev(parts);
	char *out = g_strdup_printf("%s%s%s%s%s", super ? "super+" : "", ctrl ? "ctrl+" : "",
		alt ? "alt+" : "", shift ? "shift+" : "", rest->str);
	g_string_free(rest, TRUE);
	return out;
}

enum shortcut_group {
	GROUP_APPS,
	GROUP_WINDOWS,
	GROUP_DESKTOPS,
	GROUP_TASKBAR,
	GROUP_MEDIA,
	GROUP_SYSTEM,
	GROUP_OTHER,
	GROUP_COUNT,
};

static const char *const group_titles[GROUP_COUNT] = {
	"Apps", "Windows", "Desktops", "Taskbar", "Sound & screen", "tileWin & system", "Other",
};

struct action {
	const char *command, *label;
	enum shortcut_group group;
};

/* The actions offered when adding or changing a shortcut. */
static const struct action common_actions[] = {
	{ "exec $term", "Open a terminal", GROUP_APPS },
	{ "exec $filemanager", "Open the file manager", GROUP_APPS },
	{ "exec $taskmanager", "Open the task manager", GROUP_APPS },
	{ "exec tilewin-settings", "Open tileWin Settings", GROUP_APPS },
	{ "launcher", "Search apps (launcher)", GROUP_APPS },
	{ "panel startmenu toggle", "Open the start menu", GROUP_APPS },
	{ "panel run", "Open the Run dialog", GROUP_APPS },
	{ "kill", "Close the window", GROUP_WINDOWS },
	{ "maximize enable", "Maximize the window", GROUP_WINDOWS },
	{ "maximize toggle", "Maximize or restore the window", GROUP_WINDOWS },
	{ "minimize enable", "Minimize the window", GROUP_WINDOWS },
	{ "snap left", "Snap the window to the left", GROUP_WINDOWS },
	{ "snap right", "Snap the window to the right", GROUP_WINDOWS },
	{ "snap up", "Maximize the window (snap up)", GROUP_WINDOWS },
	{ "snap down", "Restore or minimize the window", GROUP_WINDOWS },
	{ "fullscreen", "Full screen", GROUP_WINDOWS },
	{ "floating toggle", "Float or tile the window", GROUP_WINDOWS },
	{ "alttab next", "Switch to the next window", GROUP_WINDOWS },
	{ "alttab prev", "Switch to the previous window", GROUP_WINDOWS },
	{ "panel window_menu", "Open the window menu", GROUP_WINDOWS },
	{ "move output left", "Move the window to the screen on the left", GROUP_WINDOWS },
	{ "move output right", "Move the window to the screen on the right", GROUP_WINDOWS },
	{ "taskview", "Task view", GROUP_DESKTOPS },
	{ "showdesktop", "Show the desktop", GROUP_DESKTOPS },
	{ "desktop new", "New desktop", GROUP_DESKTOPS },
	{ "desktop close", "Close the desktop", GROUP_DESKTOPS },
	{ "workspace prev_on_output", "Previous desktop", GROUP_DESKTOPS },
	{ "workspace next_on_output", "Next desktop", GROUP_DESKTOPS },
	{ "exec tilewin-media volume-up", "Volume up", GROUP_MEDIA },
	{ "exec tilewin-media volume-down", "Volume down", GROUP_MEDIA },
	{ "exec tilewin-media mute", "Mute", GROUP_MEDIA },
	{ "exec tilewin-media mic-mute", "Mute the microphone", GROUP_MEDIA },
	{ "exec tilewin-media brightness-up", "Brightness up", GROUP_MEDIA },
	{ "exec tilewin-media brightness-down", "Brightness down", GROUP_MEDIA },
	{ "exec tilewin-media play-pause", "Play or pause music", GROUP_MEDIA },
	{ "exec tilewin-media next", "Next track", GROUP_MEDIA },
	{ "exec tilewin-media previous", "Previous track", GROUP_MEDIA },
	{ "exec $screenshot", "Take a screenshot", GROUP_MEDIA },
	{ "exec $locker", "Lock the screen", GROUP_SYSTEM },
	{ "panel shutdown", "Shut down, restart or sign out", GROUP_SYSTEM },
	{ "wm_mode toggle", "Switch between window and tile mode", GROUP_SYSTEM },
	{ "reload", "Reload the configuration", GROUP_SYSTEM },
	{ "restart", "Restart tileWin", GROUP_SYSTEM },
	{ "restart panel", "Restart the taskbar", GROUP_SYSTEM },
};

static const char *direction_word(const char *dir) {
	return !strcmp(dir, "left") ? "to the left" : !strcmp(dir, "right") ? "to the right" :
		!strcmp(dir, "up") ? "above" : !strcmp(dir, "down") ? "below" : NULL;
}

/* A description of a command, NULL if there is none. */
static char *describe_command(const char *command, enum shortcut_group *group) {
	*group = GROUP_OTHER;
	for (size_t i = 0; i < G_N_ELEMENTS(common_actions); i++) {
		if (strcmp(command, common_actions[i].command) == 0) {
			*group = common_actions[i].group;
			return g_strdup(common_actions[i].label);
		}
	}
	char word[64];
	int n;
	if (sscanf(command, "panel activate %d", &n) == 1) {
		*group = GROUP_TASKBAR;
		return g_strdup_printf("Open the app %d on the taskbar", n);
	}
	if (sscanf(command, "move container to workspace number %d", &n) == 1) {
		*group = GROUP_DESKTOPS;
		return g_strdup_printf("Move the window to desktop %d", n);
	}
	if (sscanf(command, "workspace number %d", &n) == 1) {
		*group = GROUP_DESKTOPS;
		return g_strdup_printf("Go to desktop %d", n);
	}
	if (g_str_has_prefix(command, "move container to workspace prev_on_output")) {
		*group = GROUP_DESKTOPS;
		return g_strdup("Move the window to the previous desktop");
	}
	if (g_str_has_prefix(command, "move container to workspace next_on_output")) {
		*group = GROUP_DESKTOPS;
		return g_strdup("Move the window to the next desktop");
	}
	if (sscanf(command, "focus %63s", word) == 1 && direction_word(word)) {
		*group = GROUP_WINDOWS;
		return g_strdup_printf("Focus the window %s", direction_word(word));
	}
	if (sscanf(command, "move %63s", word) == 1 && direction_word(word) &&
			!strchr(command + 5, ' ')) {
		*group = GROUP_WINDOWS;
		return g_strdup_printf("Move the window %s", direction_word(word));
	}
	static const struct action tile_actions[] = {
		{ "focus mode_toggle", "Switch focus between tiled and floating windows", GROUP_WINDOWS },
		{ "focus parent", "Focus the parent container", GROUP_WINDOWS },
		{ "splith", "Split the next window side by side", GROUP_WINDOWS },
		{ "splitv", "Split the next window below", GROUP_WINDOWS },
		{ "layout stacking", "Stacked layout", GROUP_WINDOWS },
		{ "layout tabbed", "Tabbed layout", GROUP_WINDOWS },
		{ "layout toggle split", "Switch the split direction", GROUP_WINDOWS },
		{ "move scratchpad", "Move the window to the scratchpad", GROUP_WINDOWS },
		{ "scratchpad show", "Show the scratchpad", GROUP_WINDOWS },
		{ "mode \"resize\"", "Resize mode (arrows resize, Esc ends)", GROUP_WINDOWS },
		{ "mode resize", "Resize mode (arrows resize, Esc ends)", GROUP_WINDOWS },
		{ "snap restore", "Restore the window", GROUP_WINDOWS },
	};
	for (size_t i = 0; i < G_N_ELEMENTS(tile_actions); i++) {
		if (strcmp(command, tile_actions[i].command) == 0) {
			*group = tile_actions[i].group;
			return g_strdup(tile_actions[i].label);
		}
	}
	if (g_str_has_prefix(command, "panel startmenu")) {
		*group = GROUP_APPS;
		return g_strdup("Open the start menu");
	}
	if (g_str_has_prefix(command, "exec tilewin-nag") && strstr(command, "exit")) {
		*group = GROUP_SYSTEM;
		return g_strdup("Exit tileWin (asks first)");
	}
	if (g_str_has_prefix(command, "exec ")) {
		const char *program = command + 5;
		while (g_str_has_prefix(program, "--")) {
			program += strcspn(program, " ");
			while (*program == ' ') {
				program++;
			}
		}
		size_t len = strcspn(program, " ");
		char *name = g_strndup(program, len);
		char *base = g_path_get_basename(name);
		char *label = g_strdup_printf("Run %s", base);
		g_free(base);
		g_free(name);
		*group = GROUP_APPS;
		return label;
	}
	return NULL;
}

/* ---------- recording keys ---------- */

typedef void (*record_done)(const char *keys, gpointer data);

struct recorder {
	GtkWidget *window;
	record_done done;
	gpointer data;
};

static gboolean on_record_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	struct recorder *r = data;
	switch (keyval) {
	case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
	case GDK_KEY_Control_L: case GDK_KEY_Control_R:
	case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
	case GDK_KEY_Super_L: case GDK_KEY_Super_R:
	case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
	case GDK_KEY_ISO_Level3_Shift:
		return TRUE; // wait for the actual key
	case GDK_KEY_Escape:
		if (!(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
			gtk_window_destroy(GTK_WINDOW(r->window));
			return TRUE;
		}
		break;
	}
	GString *combo = g_string_new(NULL);
	if (state & GDK_SUPER_MASK) {
		g_string_append(combo, "$mod+");
	}
	if (state & GDK_CONTROL_MASK) {
		g_string_append(combo, "Control+");
	}
	if (state & GDK_ALT_MASK) {
		g_string_append(combo, "Mod1+");
	}
	if (state & GDK_SHIFT_MASK) {
		g_string_append(combo, "Shift+");
	}
	const char *name = gdk_keyval_name(gdk_keyval_to_lower(keyval));
	if (name) {
		g_string_append(combo, name);
		r->done(combo->str, r->data);
	}
	g_string_free(combo, TRUE);
	gtk_window_destroy(GTK_WINDOW(r->window));
	return TRUE;
}

/* While recording, tileWin's own shortcuts reach this window too. */
static void on_record_map(GtkWidget *window, gpointer data) {
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (surface && GDK_IS_TOPLEVEL(surface)) {
		gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(surface), NULL);
	}
}

static void record_keys(GtkWindow *parent, record_done done, gpointer data) {
	struct recorder *r = g_new0(struct recorder, 1);
	r->done = done;
	r->data = data;
	r->window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(r->window), parent);
	gtk_window_set_modal(GTK_WINDOW(r->window), TRUE);
	gtk_window_set_title(GTK_WINDOW(r->window), "Record shortcut");
	gtk_window_set_default_size(GTK_WINDOW(r->window), 420, 170);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	GtkWidget *title = gtk_label_new("Press the new shortcut");
	gtk_widget_add_css_class(title, "tw-heading");
	gtk_box_append(GTK_BOX(box), title);
	GtkWidget *hint = gtk_label_new("For example Win+E or Ctrl+Alt+T. Esc cancels.");
	gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(box), hint);
	gtk_window_set_child(GTK_WINDOW(r->window), box);
	GtkEventController *controller = gtk_event_controller_key_new();
	g_signal_connect(controller, "key-pressed", G_CALLBACK(on_record_key), r);
	gtk_widget_add_controller(r->window, controller);
	g_signal_connect(r->window, "map", G_CALLBACK(on_record_map), NULL);
	g_object_set_data_full(G_OBJECT(r->window), "recorder", r, g_free);
	gtk_window_present(GTK_WINDOW(r->window));
}

/* The description of another binding with the same keys, NULL if there is none. */
static char *conflict(struct keyboard_page *p, const char *keys, int exclude) {
	struct confdoc *d = binds_doc(p);
	char *wanted = normalize_keys(keys);
	char *found = NULL;
	for (int i = 0; !found; i++) {
		struct cstmt *stmt = nth_binding(d, i);
		if (!stmt) {
			break;
		}
		if (i == exclude) {
			continue;
		}
		char *flags, *other_keys, *command;
		parse_binding(d, stmt, &flags, &other_keys, &command);
		char *norm = normalize_keys(other_keys);
		if (strcmp(norm, wanted) == 0 && !strstr(flags, "--release")) {
			enum shortcut_group group;
			found = describe_command(command, &group);
			if (!found) {
				found = g_strdup(command);
			}
		}
		g_free(norm);
		g_free(flags);
		g_free(other_keys);
		g_free(command);
	}
	g_free(wanted);
	return found;
}

/* ---------- add / change dialog ---------- */

struct shortcut_dialog {
	struct keyboard_page *p;
	int index; // binding being changed, -1 for a new one
	char *flags, *keys;
	GtkWidget *window, *keys_button, *action_dd, *custom_entry, *custom_row, *warning, *save;
};

static void shortcut_dialog_free(gpointer data) {
	struct shortcut_dialog *d = data;
	g_free(d->flags);
	g_free(d->keys);
	g_free(d);
}

static const char *dialog_command(struct shortcut_dialog *d) {
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->action_dd));
	if (sel < G_N_ELEMENTS(common_actions)) {
		return common_actions[sel].command;
	}
	return gtk_editable_get_text(GTK_EDITABLE(d->custom_entry));
}

static void dialog_update(struct shortcut_dialog *d) {
	char *shown = d->keys && *d->keys ? display_keys(d->keys) : g_strdup("Click to record");
	gtk_button_set_label(GTK_BUTTON(d->keys_button), shown);
	g_free(shown);
	bool custom = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->action_dd)) >=
		G_N_ELEMENTS(common_actions);
	gtk_widget_set_visible(d->custom_row, custom);
	char *other = d->keys && *d->keys ? conflict(d->p, d->keys, d->index) : NULL;
	if (other) {
		char *text = g_strdup_printf("These keys are also used for \"%s\". The shortcut saved "
			"last wins.", other);
		gtk_label_set_text(GTK_LABEL(d->warning), text);
		g_free(text);
	}
	gtk_widget_set_visible(d->warning, other != NULL);
	g_free(other);
	const char *command = dialog_command(d);
	gtk_widget_set_sensitive(d->save, d->keys && *d->keys && command && *command);
}

static void on_dialog_keys_recorded(const char *keys, gpointer data) {
	struct shortcut_dialog *d = data;
	g_free(d->keys);
	d->keys = g_strdup(keys);
	dialog_update(d);
}

static void on_dialog_record(GtkButton *button, gpointer data) {
	struct shortcut_dialog *d = data;
	record_keys(GTK_WINDOW(d->window), on_dialog_keys_recorded, d);
}

static void on_dialog_action(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	dialog_update(data);
}

static void on_dialog_custom(GtkEditable *editable, gpointer data) {
	dialog_update(data);
}

static void on_dialog_save(GtkButton *button, gpointer data) {
	struct shortcut_dialog *d = data;
	const char *command = dialog_command(d);
	if (!d->keys || !*d->keys || strchr(d->keys, ' ') || !command || !*command) {
		return;
	}
	set_binding(d->p, d->index, d->flags, d->keys, command);
	settings_status(d->p->s, d->index < 0 ? "Added the shortcut" : "Changed the shortcut");
	schedule_binds(d->p);
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static void on_dialog_cancel(GtkButton *button, gpointer data) {
	struct shortcut_dialog *d = data;
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static void open_shortcut_dialog(struct keyboard_page *p, int index) {
	struct shortcut_dialog *d = g_new0(struct shortcut_dialog, 1);
	d->p = p;
	d->index = index;
	char *command = NULL;
	if (index >= 0) {
		struct confdoc *doc = binds_doc(p);
		struct cstmt *stmt = nth_binding(doc, index);
		if (!stmt) {
			g_free(d);
			return;
		}
		parse_binding(doc, stmt, &d->flags, &d->keys, &command);
	} else {
		d->flags = g_strdup("");
	}

	d->window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(d->window), p->s->window);
	gtk_window_set_modal(GTK_WINDOW(d->window), TRUE);
	gtk_window_set_title(GTK_WINDOW(d->window), index < 0 ? "Add shortcut" : "Change shortcut");
	gtk_window_set_default_size(GTK_WINDOW(d->window), 520, -1);
	g_object_set_data_full(G_OBJECT(d->window), "dialog", d, shortcut_dialog_free);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(box, 20);
	gtk_widget_set_margin_end(box, 20);
	gtk_widget_set_margin_top(box, 20);
	gtk_widget_set_margin_bottom(box, 20);

	GtkWidget *keys_label = gtk_label_new("Keys");
	gtk_label_set_xalign(GTK_LABEL(keys_label), 0);
	gtk_widget_add_css_class(keys_label, "tw-heading");
	gtk_box_append(GTK_BOX(box), keys_label);
	d->keys_button = gtk_button_new_with_label("");
	gtk_widget_set_tooltip_text(d->keys_button, "Click, then press the keys");
	g_signal_connect(d->keys_button, "clicked", G_CALLBACK(on_dialog_record), d);
	gtk_box_append(GTK_BOX(box), d->keys_button);

	GtkWidget *action_label = gtk_label_new("Action");
	gtk_label_set_xalign(GTK_LABEL(action_label), 0);
	gtk_widget_add_css_class(action_label, "tw-heading");
	gtk_box_append(GTK_BOX(box), action_label);
	GtkStringList *model = gtk_string_list_new(NULL);
	guint selected = G_N_ELEMENTS(common_actions);
	for (size_t i = 0; i < G_N_ELEMENTS(common_actions); i++) {
		char *label = g_strdup_printf("%s \xe2\x80\x94 %s", group_titles[common_actions[i].group],
			common_actions[i].label);
		gtk_string_list_append(model, label);
		g_free(label);
		if (command && strcmp(command, common_actions[i].command) == 0) {
			selected = i;
		}
	}
	gtk_string_list_append(model, "Other command...");
	d->action_dd = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(d->action_dd), TRUE);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d->action_dd), index < 0 ? 0 : selected);
	gtk_box_append(GTK_BOX(box), d->action_dd);

	d->custom_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	d->custom_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(d->custom_entry), "e.g. exec firefox or snap left");
	if (command && selected == G_N_ELEMENTS(common_actions)) {
		gtk_editable_set_text(GTK_EDITABLE(d->custom_entry), command);
	}
	gtk_box_append(GTK_BOX(d->custom_row), d->custom_entry);
	GtkWidget *custom_hint = gtk_label_new("A tileWin command: \"exec <program>\" starts a "
		"program, other commands are listed in the README.");
	gtk_label_set_wrap(GTK_LABEL(custom_hint), TRUE);
	gtk_label_set_xalign(GTK_LABEL(custom_hint), 0);
	gtk_widget_add_css_class(custom_hint, "dim-label");
	gtk_box_append(GTK_BOX(d->custom_row), custom_hint);
	gtk_box_append(GTK_BOX(box), d->custom_row);

	d->warning = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(d->warning), TRUE);
	gtk_label_set_xalign(GTK_LABEL(d->warning), 0);
	gtk_widget_add_css_class(d->warning, "warning");
	gtk_box_append(GTK_BOX(box), d->warning);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	GtkWidget *cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(on_dialog_cancel), d);
	gtk_box_append(GTK_BOX(buttons), cancel);
	d->save = gtk_button_new_with_label(index < 0 ? "Add" : "Save");
	gtk_widget_add_css_class(d->save, "suggested-action");
	g_signal_connect(d->save, "clicked", G_CALLBACK(on_dialog_save), d);
	gtk_box_append(GTK_BOX(buttons), d->save);
	gtk_box_append(GTK_BOX(box), buttons);

	g_signal_connect(d->action_dd, "notify::selected", G_CALLBACK(on_dialog_action), d);
	g_signal_connect(d->custom_entry, "changed", G_CALLBACK(on_dialog_custom), d);
	gtk_window_set_child(GTK_WINDOW(d->window), box);
	dialog_update(d);
	g_free(command);
	gtk_window_present(GTK_WINDOW(d->window));
	if (index < 0) {
		record_keys(GTK_WINDOW(d->window), on_dialog_keys_recorded, d);
	}
}

/* ---------- list ---------- */

struct row_keys {
	struct keyboard_page *p;
	int index;
};

static void on_row_keys_recorded(const char *keys, gpointer data) {
	struct row_keys *r = data;
	struct confdoc *d = binds_doc(r->p);
	struct cstmt *stmt = nth_binding(d, r->index);
	if (stmt) {
		char *flags, *old_keys, *command;
		parse_binding(d, stmt, &flags, &old_keys, &command);
		set_binding(r->p, r->index, flags, keys, command);
		char *shown = display_keys(keys);
		settings_status(r->p->s, "The shortcut is now %s", shown);
		g_free(shown);
		g_free(flags);
		g_free(old_keys);
		g_free(command);
		schedule_binds(r->p);
	}
}

static void on_row_keys(GtkButton *button, gpointer data) {
	struct keyboard_page *p = data;
	struct row_keys *r = g_new0(struct row_keys, 1);
	r->p = p;
	r->index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"));
	// freed with the button's row when the list is rebuilt after the change
	g_object_set_data_full(G_OBJECT(button), "record", r, g_free);
	record_keys(p->s->window, on_row_keys_recorded, r);
}

static void on_row_edit(GtkButton *button, gpointer data) {
	open_shortcut_dialog(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index")));
}

static void on_row_remove(GtkButton *button, gpointer data) {
	struct keyboard_page *p = data;
	struct confdoc *d = binds_doc(p);
	struct cstmt *stmt = nth_binding(d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
		"index")));
	if (stmt) {
		confdoc_remove(d, stmt);
		settings_mode_changed(p->s, d);
		settings_status(p->s, "Removed the shortcut");
		schedule_binds(p);
	}
}

static void on_add_binding(GtkButton *button, gpointer data) {
	open_shortcut_dialog(data, -1);
}

static bool matches_search(const char *search, const char *a, const char *b, const char *c) {
	if (!search || !*search) {
		return true;
	}
	const char *fields[] = { a, b, c };
	for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) {
		if (!fields[i]) {
			continue;
		}
		char *hay = g_utf8_casefold(fields[i], -1);
		char *needle = g_utf8_casefold(search, -1);
		bool found = strstr(hay, needle) != NULL;
		g_free(hay);
		g_free(needle);
		if (found) {
			return true;
		}
	}
	return false;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip, int index,
		GCallback callback, struct keyboard_page *p) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(button), "index", GINT_TO_POINTER(index));
	g_signal_connect(button, "clicked", callback, p);
	return button;
}

static void rebuild_binds(struct keyboard_page *p) {
	p->updating = true;
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->binds_box))) {
		gtk_box_remove(GTK_BOX(p->binds_box), child);
	}
	const char *search = gtk_editable_get_text(GTK_EDITABLE(p->search));
	struct confdoc *d = binds_doc(p);
	GtkWidget *groups[GROUP_COUNT] = { 0 };
	// create the groups in their order, empty ones are removed afterwards
	for (int g = 0; g < GROUP_COUNT; g++) {
		groups[g] = ui_group(p->binds_box, group_titles[g], NULL);
	}
	int shown = 0;
	for (int i = 0;; i++) {
		struct cstmt *stmt = nth_binding(d, i);
		if (!stmt) {
			break;
		}
		char *flags, *keys, *command;
		parse_binding(d, stmt, &flags, &keys, &command);
		enum shortcut_group group;
		char *label = describe_command(command, &group);
		char *pretty = display_keys(keys);
		bool release = strstr(flags, "--release") != NULL;
		if (matches_search(search, label, command, pretty)) {
			shown++;
			char *other = release ? NULL : conflict(p, keys, i);
			char *subtitle = other ?
				g_strdup_printf("%s \xe2\x80\x94 also used for \"%s\"", command, other) :
				g_strdup(command);
			GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
			char *key_text = release ? g_strdup_printf("%s (tap)", pretty) : g_strdup(pretty);
			GtkWidget *keys_button = gtk_button_new_with_label(key_text);
			g_free(key_text);
			gtk_widget_set_tooltip_text(keys_button, "Change the keys: click, then press them");
			gtk_widget_set_valign(keys_button, GTK_ALIGN_CENTER);
			g_object_set_data(G_OBJECT(keys_button), "index", GINT_TO_POINTER(i));
			g_signal_connect(keys_button, "clicked", G_CALLBACK(on_row_keys), p);
			gtk_box_append(GTK_BOX(controls), keys_button);
			gtk_box_append(GTK_BOX(controls), icon_button("document-edit-symbolic",
				"Change the action", i, G_CALLBACK(on_row_edit), p));
			gtk_box_append(GTK_BOX(controls), icon_button("user-trash-symbolic",
				"Remove the shortcut", i, G_CALLBACK(on_row_remove), p));
			GtkWidget *row = ui_row(groups[group], label ? label : command,
				label || other ? subtitle : NULL, controls);
			if (other) {
				gtk_widget_add_css_class(row, "tw-conflict");
			}
			g_free(subtitle);
			g_free(other);
		}
		g_free(label);
		g_free(pretty);
		g_free(flags);
		g_free(keys);
		g_free(command);
	}
	for (int g = 0; g < GROUP_COUNT; g++) {
		// ui_group returns the list; its parent is the group box
		if (!gtk_widget_get_first_child(groups[g])) {
			gtk_box_remove(GTK_BOX(p->binds_box), gtk_widget_get_parent(groups[g]));
		}
	}
	if (shown == 0) {
		GtkWidget *empty = gtk_label_new(search && *search ? "No shortcut matches the search." :
			"No shortcuts yet.");
		gtk_widget_add_css_class(empty, "dim-label");
		gtk_box_append(GTK_BOX(p->binds_box), empty);
	}
	p->updating = false;
}

static gboolean rebuild_binds_idle(gpointer data) {
	struct keyboard_page *p = data;
	p->binds_rebuild_id = 0;
	rebuild_binds(p);
	return G_SOURCE_REMOVE;
}

static void schedule_binds(struct keyboard_page *p) {
	if (!p->binds_rebuild_id) {
		p->binds_rebuild_id = g_idle_add(rebuild_binds_idle, p);
	}
}

static void on_mode_selected(GObject *dropdown, GParamSpec *pspec, gpointer data) {
	schedule_binds(data);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
	schedule_binds(data);
}

/* ---------- page ---------- */

void keyboard_page_refresh(struct settings *s) {
	struct keyboard_page *p = s->keyboard_page;
	if (!p) {
		return;
	}
	p->updating = true;
	read_layouts(p);
	char *value = keyboard_value(p, "xkb_options");
	GPtrArray *options = split_list(value);
	g_free(value);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->switch_dd), choice_index(options, switch_choices));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->caps_dd), choice_index(options, caps_choices));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->compose_dd), choice_index(options, compose_choices));
	g_ptr_array_unref(options);
	value = keyboard_value(p, "xkb_numlock");
	gtk_switch_set_active(GTK_SWITCH(p->numlock_switch), value && strcmp(value, "enabled") == 0);
	g_free(value);
	value = keyboard_value(p, "repeat_delay");
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->delay_spin), value ? atoi(value) : 600);
	g_free(value);
	value = keyboard_value(p, "repeat_rate");
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->rate_spin), value ? atoi(value) : 25);
	g_free(value);
	p->updating = false;
	rebuild_layouts(p);
	rebuild_binds(p);
}

GtkWidget *keyboard_page_new(struct settings *s) {
	struct keyboard_page *p = g_new0(struct keyboard_page, 1);
	p->s = s;
	load_xkb(p);
	GtkWidget *content;
	GtkWidget *page = ui_page("Keyboard",
		"Keyboard layouts, special keys, key repeat and the keyboard shortcuts of both modes.",
		&content);

	p->layouts_list = ui_group(content, "Layouts",
		"Type in a list to search it. With several layouts, switch between them with the shortcut below.");

	GtkWidget *options = ui_group(content, "Keys", NULL);
	p->switch_dd = choice_dropdown(p, switch_choices);
	ui_row(options, "Switch layouts with", NULL, p->switch_dd);
	p->caps_dd = choice_dropdown(p, caps_choices);
	ui_row(options, "Caps Lock key acts as", NULL, p->caps_dd);
	p->compose_dd = choice_dropdown(p, compose_choices);
	ui_row(options, "Compose key", "Type special characters, e.g. Compose, o, \" for \xc3\xb6",
		p->compose_dd);
	p->numlock_switch = gtk_switch_new();
	g_signal_connect(p->numlock_switch, "state-set", G_CALLBACK(on_numlock), p);
	ui_row(options, "Num Lock on at login", NULL, p->numlock_switch);

	GtkWidget *typing = ui_group(content, "Typing", NULL);
	p->delay_spin = gtk_spin_button_new_with_range(100, 2000, 25);
	g_object_set_data(G_OBJECT(p->delay_spin), "key", "repeat_delay");
	g_signal_connect(p->delay_spin, "value-changed", G_CALLBACK(on_repeat_changed), p);
	ui_row(typing, "Repeat delay", "Milliseconds before a held key repeats", p->delay_spin);
	p->rate_spin = gtk_spin_button_new_with_range(5, 100, 1);
	g_object_set_data(G_OBJECT(p->rate_spin), "key", "repeat_rate");
	g_signal_connect(p->rate_spin, "value-changed", G_CALLBACK(on_repeat_changed), p);
	ui_row(typing, "Repeat rate", "Characters per second", p->rate_spin);
	GtkWidget *test = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(test), "Type here to try the settings");
	gtk_widget_set_size_request(test, 300, -1);
	ui_row(typing, "Test", NULL, test);

	GtkWidget *binds_header = ui_group(content, "Shortcuts",
		"Click the keys of a shortcut to change them, the pencil to choose another action. "
		"Win is the Windows key. tileWin applies changes right away.");
	p->mode_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Window mode", "Tile mode", NULL });
	char *mode = settings_current_mode();
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), strcmp(mode, "tile") == 0);
	g_free(mode);
	g_signal_connect(p->mode_dd, "notify::selected", G_CALLBACK(on_mode_selected), p);
	ui_row(binds_header, "Shortcuts of", NULL, p->mode_dd);
	p->search = gtk_search_entry_new();
	gtk_widget_set_size_request(p->search, 300, -1);
	g_signal_connect(p->search, "search-changed", G_CALLBACK(on_search_changed), p);
	ui_row(binds_header, "Search", "By action, command or keys", p->search);
	GtkWidget *add = gtk_button_new_with_label("Add shortcut");
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_binding), p);
	ui_row(binds_header, "New shortcut", "Press the keys, then choose what they do", add);
	p->binds_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(content), p->binds_box);

	s->keyboard_page = p;
	keyboard_page_refresh(s);
	return page;
}
