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
	GtkWidget *mode_dd, *binds_list;
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

struct binding_row {
	struct keyboard_page *p;
	int index;
	char *flags;
	GtkWidget *keys, *command;
};

static void binding_row_free(gpointer data) {
	struct binding_row *b = data;
	g_free(b->flags);
	g_free(b);
}

static void schedule_binds(struct keyboard_page *p);

static void write_binding(struct binding_row *b) {
	struct keyboard_page *p = b->p;
	const char *keys = gtk_editable_get_text(GTK_EDITABLE(b->keys));
	const char *command = gtk_editable_get_text(GTK_EDITABLE(b->command));
	if (!*keys || strchr(keys, ' ') || !*command) {
		return; // incomplete, don't write an invalid line
	}
	struct confdoc *d = binds_doc(p);
	struct cstmt *stmt = nth_binding(d, b->index);
	if (!stmt) {
		return;
	}
	char *text = g_strdup_printf("bindsym %s%s %s", b->flags, keys, command);
	confdoc_replace(d, stmt, text);
	g_free(text);
	settings_mode_changed(p->s, d);
}

static void on_binding_changed(GtkEditable *editable, gpointer data) {
	struct binding_row *b = data;
	if (!b->p->updating) {
		write_binding(b);
	}
}

static void on_binding_remove(GtkButton *button, gpointer data) {
	struct binding_row *b = data;
	struct confdoc *d = binds_doc(b->p);
	struct cstmt *stmt = nth_binding(d, b->index);
	if (stmt) {
		confdoc_remove(d, stmt);
		settings_mode_changed(b->p->s, d);
		schedule_binds(b->p);
	}
}

static void on_add_binding(GtkButton *button, gpointer data) {
	struct keyboard_page *p = data;
	struct confdoc *d = binds_doc(p);
	confdoc_append(d, d->root, "bindsym $mod+Shift+F12 nop");
	settings_mode_changed(p->s, d);
	settings_status(p->s, "Added a shortcut at the end of the list: change its keys and command");
	schedule_binds(p);
}

/* ---------- recording a shortcut ---------- */

struct recorder {
	struct binding_row *row;
	GtkWidget *window;
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
		gtk_editable_set_text(GTK_EDITABLE(r->row->keys), combo->str);
	}
	g_string_free(combo, TRUE);
	gtk_window_destroy(GTK_WINDOW(r->window));
	return TRUE;
}

static void on_record(GtkButton *button, gpointer data) {
	struct binding_row *b = data;
	struct recorder *r = g_new0(struct recorder, 1);
	r->row = b;
	r->window = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(r->window), b->p->s->window);
	gtk_window_set_modal(GTK_WINDOW(r->window), TRUE);
	gtk_window_set_title(GTK_WINDOW(r->window), "Record shortcut");
	gtk_window_set_default_size(GTK_WINDOW(r->window), 420, 160);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	GtkWidget *title = gtk_label_new("Press the new shortcut");
	gtk_widget_add_css_class(title, "tw-heading");
	gtk_box_append(GTK_BOX(box), title);
	GtkWidget *hint = gtk_label_new("Esc cancels. Shortcuts that tileWin already uses reach "
		"tileWin instead of this window; type those into the field.");
	gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(box), hint);
	gtk_window_set_child(GTK_WINDOW(r->window), box);
	GtkEventController *controller = gtk_event_controller_key_new();
	g_signal_connect(controller, "key-pressed", G_CALLBACK(on_record_key), r);
	gtk_widget_add_controller(r->window, controller);
	g_object_set_data_full(G_OBJECT(r->window), "recorder", r, g_free);
	gtk_window_present(GTK_WINDOW(r->window));
}

static void rebuild_binds(struct keyboard_page *p) {
	p->updating = true;
	gtk_list_box_remove_all(GTK_LIST_BOX(p->binds_list));
	struct confdoc *d = binds_doc(p);
	for (int i = 0;; i++) {
		struct cstmt *stmt = nth_binding(d, i);
		if (!stmt) {
			break;
		}
		char *flags, *keys, *command;
		parse_binding(d, stmt, &flags, &keys, &command);
		struct binding_row *b = g_new0(struct binding_row, 1);
		b->p = p;
		b->index = i;
		b->flags = flags;

		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_margin_start(box, 12);
		gtk_widget_set_margin_end(box, 8);
		gtk_widget_set_margin_top(box, 4);
		gtk_widget_set_margin_bottom(box, 4);
		b->keys = gtk_entry_new();
		gtk_editable_set_text(GTK_EDITABLE(b->keys), keys);
		gtk_editable_set_width_chars(GTK_EDITABLE(b->keys), 22);
		if (*flags) {
			char *tooltip = g_strdup_printf("Options: %s", flags);
			gtk_widget_set_tooltip_text(b->keys, tooltip);
			g_free(tooltip);
		}
		gtk_box_append(GTK_BOX(box), b->keys);
		GtkWidget *record = gtk_button_new_from_icon_name("input-keyboard-symbolic");
		gtk_widget_set_tooltip_text(record, "Record the keys");
		gtk_widget_add_css_class(record, "flat");
		g_signal_connect(record, "clicked", G_CALLBACK(on_record), b);
		gtk_box_append(GTK_BOX(box), record);
		b->command = gtk_entry_new();
		gtk_editable_set_text(GTK_EDITABLE(b->command), command);
		gtk_widget_set_hexpand(b->command, TRUE);
		gtk_box_append(GTK_BOX(box), b->command);
		GtkWidget *remove = gtk_button_new_from_icon_name("list-remove-symbolic");
		gtk_widget_set_tooltip_text(remove, "Remove");
		gtk_widget_add_css_class(remove, "flat");
		g_signal_connect(remove, "clicked", G_CALLBACK(on_binding_remove), b);
		gtk_box_append(GTK_BOX(box), remove);

		g_signal_connect(b->keys, "changed", G_CALLBACK(on_binding_changed), b);
		g_signal_connect(b->command, "changed", G_CALLBACK(on_binding_changed), b);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		// the row owns the binding data used by its signal handlers
		g_object_set_data_full(G_OBJECT(row), "binding", b, binding_row_free);
		gtk_list_box_append(GTK_LIST_BOX(p->binds_list), row);
		g_free(keys);
		g_free(command);
	}
	GtkWidget *add = gtk_button_new_with_label("Add shortcut");
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_binding), p);
	ui_row(p->binds_list, NULL, NULL, add);
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
		"$mod is the Super (Windows) key, Mod1 is Alt. Keys are joined with +, e.g. $mod+Shift+e or "
		"Control+Mod1+Delete. Commands are tileWin commands such as exec thunar or snap left. "
		"tileWin reloads its config after a change.");
	p->mode_dd = gtk_drop_down_new_from_strings((const char *const[]){
		"Window mode", "Tile mode", NULL });
	char *mode = settings_current_mode();
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mode_dd), strcmp(mode, "tile") == 0);
	g_free(mode);
	g_signal_connect(p->mode_dd, "notify::selected", G_CALLBACK(on_mode_selected), p);
	ui_row(binds_header, "Shortcuts of", NULL, p->mode_dd);
	p->binds_list = ui_group(content, NULL, NULL);

	s->keyboard_page = p;
	keyboard_page_refresh(s);
	return page;
}
