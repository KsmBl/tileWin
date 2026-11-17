/*
 * The Quick web page: the words of the keys of Quick web (panel/quickweb.c) as
 * a tree, a key with its word and the keys that come after it, and the
 * shortcut that opens it. The words are ~/.config/tileWin/quickweb.conf,
 *
 *   key g "github.com/" {
 *       key k "ksmbl/"
 *   }
 *
 * and the shortcut a line at the end of both mode files, as the Popups page
 * writes them: "bindsym $mod+w panel quickweb". Nothing by default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "confdoc.h"
#include "settings.h"
#include "tw_paths.h"
#include "twconf.h"

#define COMMAND "panel quickweb"

struct qw_node {
	char *key, *word;
	GPtrArray *children; // struct qw_node *
	struct qw_node *parent;
};

struct quickweb_page {
	struct settings *s;
	struct qw_node *root;
	GtkWidget *list, *keys_button, *warning;
	char *keys;
	guint save_timer;
};

static struct quickweb_page *current;

static void node_free(gpointer data) {
	struct qw_node *n = data;
	g_free(n->key);
	g_free(n->word);
	g_ptr_array_free(n->children, TRUE);
	g_free(n);
}

static struct qw_node *node_new(struct qw_node *parent, const char *key, const char *word) {
	struct qw_node *n = g_new0(struct qw_node, 1);
	n->key = g_strdup(key ? key : "");
	n->word = g_strdup(word ? word : "");
	n->children = g_ptr_array_new_with_free_func(node_free);
	n->parent = parent;
	if (parent) {
		g_ptr_array_add(parent->children, n);
	}
	return n;
}

static char *conf_path(void) {
	char *dir = tw_config_dir();
	char *path = g_build_filename(dir, "quickweb.conf", NULL);
	free(dir);
	return path;
}

/* ---------- the file ---------- */

static void read_keys(struct qw_node *into, const struct twconf_node *from) {
	for (int i = 0; from && i < twconf_count(from); i++) {
		const struct twconf_node *k = twconf_at(from, i);
		if (strcmp(k->name, "key") != 0 || k->argc < 2) {
			continue;
		}
		struct qw_node *n = node_new(into, k->argv[0], k->argv[1]);
		read_keys(n, k);
	}
}

static void load(struct quickweb_page *p) {
	if (p->root) {
		node_free(p->root);
	}
	p->root = node_new(NULL, NULL, NULL);
	char *path = conf_path();
	struct twconf_node *conf = twconf_parse_file(path, NULL);
	g_free(path);
	read_keys(p->root, conf);
	twconf_free(conf);
}

static void write_keys(GString *out, const struct qw_node *n, int depth) {
	for (guint i = 0; i < n->children->len; i++) {
		const struct qw_node *c = n->children->pdata[i];
		if (!c->key[0] || !c->word[0]) {
			continue; // not filled in yet
		}
		char *key = conf_quote(c->key), *word = conf_quote(c->word);
		for (int d = 0; d < depth; d++) {
			g_string_append_c(out, '\t');
		}
		g_string_append_printf(out, "key %s %s", key, word);
		g_free(key);
		g_free(word);
		bool any = false;
		for (guint j = 0; j < c->children->len && !any; j++) {
			const struct qw_node *g = c->children->pdata[j];
			any = g->key[0] && g->word[0];
		}
		if (any) {
			g_string_append(out, " {\n");
			write_keys(out, c, depth + 1);
			for (int d = 0; d < depth; d++) {
				g_string_append_c(out, '\t');
			}
			g_string_append(out, "}");
		}
		g_string_append_c(out, '\n');
	}
}

static gboolean save_now(gpointer data) {
	struct quickweb_page *p = data;
	p->save_timer = 0;
	GString *out = g_string_new("# Quick web: a key types its word, the keys in its braces come next.\n"
		"# Made by the Quick web page of the settings.\n");
	write_keys(out, p->root, 0);
	char *path = conf_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	GError *error = NULL;
	if (!g_file_set_contents(path, out->str, -1, &error)) {
		problem_text(p->s, "Could not save %s: %s", path, error->message);
		g_error_free(error);
	}
	g_free(path);
	g_string_free(out, TRUE);
	return G_SOURCE_REMOVE;
}

static void save_soon(struct quickweb_page *p) {
	if (p->save_timer) {
		g_source_remove(p->save_timer);
	}
	p->save_timer = g_timeout_add(400, save_now, p);
}

/* ---------- the shortcut, in both mode files ---------- */

static struct cstmt *find_binding(struct confdoc *d, char **keys) {
	for (guint i = 0; d && i < d->root->children->len; i++) {
		struct cstmt *c = d->root->children->pdata[i];
		if (strcmp(c->name, "bindsym") != 0) {
			continue;
		}
		char *raw = cstmt_raw_args(d, c);
		char *pos = raw;
		while (g_str_has_prefix(pos, "--")) {
			pos += strcspn(pos, " \t");
			pos += strspn(pos, " \t");
		}
		size_t klen = strcspn(pos, " \t");
		char *command = g_strstrip(g_strdup(pos + klen));
		bool match = strcmp(command, COMMAND) == 0;
		if (match && keys) {
			*keys = g_strndup(pos, klen);
		}
		g_free(command);
		g_free(raw);
		if (match) {
			return c;
		}
	}
	return NULL;
}

static void set_binding(struct settings *s, const char *keys) {
	struct confdoc *docs[] = { s->windowmode, s->tilemode };
	for (size_t k = 0; k < G_N_ELEMENTS(docs); k++) {
		struct confdoc *d = docs[k];
		if (!d) {
			continue;
		}
		struct cstmt *old = find_binding(d, NULL);
		if (!keys || !*keys) {
			if (!old) {
				continue;
			}
			confdoc_remove(d, old);
		} else {
			char *text = g_strdup_printf("bindsym %s " COMMAND, keys);
			if (old) {
				confdoc_replace(d, old, text);
			} else {
				confdoc_append(d, d->root, text);
			}
			g_free(text);
		}
		settings_mode_changed(s, d);
	}
}

static void show_keys(struct quickweb_page *p) {
	char *shown = p->keys && *p->keys ? keyboard_display_keys(p->keys) :
		g_strdup("Click to record");
	gtk_button_set_label(GTK_BUTTON(p->keys_button), shown);
	g_free(shown);
}

static void on_keys_recorded(const char *keys, gpointer data) {
	struct quickweb_page *p = data;
	g_free(p->keys);
	p->keys = g_strdup(keys);
	set_binding(p->s, p->keys);
	show_keys(p);
	settings_status(p->s, "The shortcut of Quick web is set");
}

static void on_record(GtkButton *button, gpointer data) {
	struct quickweb_page *p = data;
	keyboard_record_keys(p->s->window, on_keys_recorded, p);
}

static void on_clear_keys(GtkButton *button, gpointer data) {
	struct quickweb_page *p = data;
	g_free(p->keys);
	p->keys = NULL;
	set_binding(p->s, NULL);
	show_keys(p);
}

static void on_try(GtkButton *button, gpointer data) {
	struct quickweb_page *p = data;
	save_now(p);
	settings_command(p->s, COMMAND);
}

/* ---------- the tree ---------- */

static void rebuild(struct quickweb_page *p);

static void on_key_changed(GtkEditable *editable, gpointer data) {
	struct qw_node *n = data;
	g_free(n->key);
	n->key = g_strdup(gtk_editable_get_text(editable));
	if (current) {
		save_soon(current);
	}
}

static void on_word_changed(GtkEditable *editable, gpointer data) {
	struct qw_node *n = data;
	g_free(n->word);
	n->word = g_strdup(gtk_editable_get_text(editable));
	if (current) {
		save_soon(current);
	}
}

static void on_add_after(GtkButton *button, gpointer data) {
	struct qw_node *n = data;
	node_new(n, NULL, NULL);
	if (current) {
		rebuild(current);
	}
}

static void on_remove(GtkButton *button, gpointer data) {
	struct qw_node *n = data;
	if (!n->parent || !current) {
		return;
	}
	g_ptr_array_remove(n->parent->children, n); // frees it and what comes after it
	save_soon(current);
	rebuild(current);
}

static void on_add_top(GtkButton *button, gpointer data) {
	struct quickweb_page *p = data;
	node_new(p->root, NULL, NULL);
	rebuild(p);
}

static void add_rows(struct quickweb_page *p, struct qw_node *n, int depth) {
	for (guint i = 0; i < n->children->len; i++) {
		struct qw_node *c = n->children->pdata[i];
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(box, 12 + depth * 28);
		gtk_widget_set_margin_end(box, 8);
		gtk_widget_set_margin_top(box, 6);
		gtk_widget_set_margin_bottom(box, 6);
		if (depth > 0) {
			GtkWidget *arrow = gtk_label_new("↳");
			gtk_widget_add_css_class(arrow, "dim-label");
			gtk_box_append(GTK_BOX(box), arrow);
		}
		GtkWidget *key = gtk_entry_new();
		gtk_entry_set_max_length(GTK_ENTRY(key), 1);
		gtk_editable_set_width_chars(GTK_EDITABLE(key), 2);
		gtk_editable_set_max_width_chars(GTK_EDITABLE(key), 2);
		gtk_entry_set_placeholder_text(GTK_ENTRY(key), "key");
		gtk_editable_set_text(GTK_EDITABLE(key), c->key);
		gtk_widget_set_tooltip_text(key, "The key, a single letter or sign");
		g_signal_connect(key, "changed", G_CALLBACK(on_key_changed), c);
		gtk_box_append(GTK_BOX(box), key);
		GtkWidget *word = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(word), "the word it types, e.g. github.com/");
		gtk_editable_set_text(GTK_EDITABLE(word), c->word);
		gtk_widget_set_hexpand(word, TRUE);
		g_signal_connect(word, "changed", G_CALLBACK(on_word_changed), c);
		gtk_box_append(GTK_BOX(box), word);
		GtkWidget *after = gtk_button_new_with_label("Key after it");
		gtk_widget_add_css_class(after, "flat");
		gtk_widget_set_tooltip_text(after, "A key that types its word after this one");
		g_signal_connect(after, "clicked", G_CALLBACK(on_add_after), c);
		gtk_box_append(GTK_BOX(box), after);
		GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_tooltip_text(remove, "Remove it, and the keys after it");
		g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), c);
		gtk_box_append(GTK_BOX(box), remove);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_append(GTK_LIST_BOX(p->list), row);
		add_rows(p, c, depth + 1);
	}
}

static void rebuild(struct quickweb_page *p) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(p->list))) {
		gtk_list_box_remove(GTK_LIST_BOX(p->list), child);
	}
	if (p->root->children->len == 0) {
		ui_empty_row(p->list, "web-browser-symbolic", "No keys yet",
			"Add one: the key, and the word it types, like g for github.com/.");
		return;
	}
	add_rows(p, p->root, 0);
}

GtkWidget *quickweb_page_new(struct settings *s) {
	struct quickweb_page *p = g_new0(struct quickweb_page, 1);
	p->s = s;
	current = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Quick web",
		"A web address typed a word a key: g types github.com/, and k after it ksmbl/. Keys "
		"without a word type themselves, Shift types the letter itself, Backspace takes a word "
		"back and Enter opens it in the browser.", &content);

	GtkWidget *open = ui_group(content, "Open it", NULL);
	GtkWidget *keys_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->keys_button = gtk_button_new_with_label("Click to record");
	g_signal_connect(p->keys_button, "clicked", G_CALLBACK(on_record), p);
	gtk_box_append(GTK_BOX(keys_box), p->keys_button);
	GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
	gtk_widget_set_tooltip_text(clear, "No shortcut");
	g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_keys), p);
	gtk_box_append(GTK_BOX(keys_box), clear);
	ui_row(open, "Shortcut", "Opens Quick web in both modes; keys used for something else "
		"are taken over", keys_box);
	GtkWidget *try_it = gtk_button_new_with_label("Open it now");
	g_signal_connect(try_it, "clicked", G_CALLBACK(on_try), p);
	ui_row(open, "Try it", "\"panel quickweb\" opens it too", try_it);

	p->list = ui_group(content, "Keys and their words",
		"Indented keys come after the one above them.");
	GtkWidget *add = gtk_button_new_with_label("Add a key");
	gtk_widget_set_halign(add, GTK_ALIGN_START);
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_top), p);
	gtk_box_append(GTK_BOX(content), add);

	load(p);
	struct confdoc *docs[] = { s->windowmode, s->tilemode };
	for (size_t k = 0; k < G_N_ELEMENTS(docs) && !p->keys; k++) {
		find_binding(docs[k], &p->keys);
	}
	show_keys(p);
	rebuild(p);
	return page;
}
