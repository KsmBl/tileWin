/*
 * The Clipboard page, as Windows 11 has it under System: the clipboard history
 * (Win+V) on or off, whether picking an entry pastes it, and Clear to forget
 * what was copied (pinned entries stay). The switches are keys of taskbar.conf;
 * Clear asks tilewin-clipboard over its socket, as the taskbar does.
 */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include "settings.h"

struct clipboard_page {
	struct settings *s;
	GPtrArray *keys;
	GtkWidget *cleared;
};

static struct clipboard_page *current;

void clipboard_page_refresh(struct settings *s) {
	if (current && current->s == s) {
		ui_taskbar_keys_refresh(current->keys);
	}
}

/* The socket of tilewin-clipboard, as clipboard/main.c names it. */
static char *socket_path(void) {
	const char *runtime = g_getenv("XDG_RUNTIME_DIR");
	const char *display = g_getenv("WAYLAND_DISPLAY");
	if (!runtime) {
		return NULL;
	}
	return g_strdup_printf("%s/tilewin-clipboard-%s.sock", runtime,
		display && !strchr(display, '/') ? display : "wayland");
}

static void on_clear(GtkButton *button, gpointer data) {
	struct clipboard_page *p = data;
	char *path = socket_path();
	bool sent = false;
	if (path) {
		GSocketClient *client = g_socket_client_new();
		GSocketAddress *address = g_unix_socket_address_new(path);
		GSocketConnection *conn = g_socket_client_connect(client,
			G_SOCKET_CONNECTABLE(address), NULL, NULL);
		if (conn) {
			GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
			sent = g_output_stream_write_all(out, "clear\n", 6, NULL, NULL, NULL);
			g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
			g_object_unref(conn);
		}
		g_object_unref(address);
		g_object_unref(client);
		g_free(path);
	}
	gtk_label_set_text(GTK_LABEL(p->cleared), sent ? "Cleared: only the pinned entries are left" :
		"The clipboard history is not running");
	gtk_widget_set_visible(p->cleared, TRUE);
}

GtkWidget *clipboard_page_new(struct settings *s) {
	struct clipboard_page *p = g_new0(struct clipboard_page, 1);
	p->s = s;
	p->keys = g_ptr_array_new();
	current = p;
	GtkWidget *content;
	GtkWidget *page = ui_page("Clipboard",
		"What you copy, kept so you can paste it again later. Win+V shows the history.",
		&content);

	GtkWidget *history = ui_group(content, "Clipboard history", NULL);
	ui_taskbar_key(s, p->keys, history, "clipboard_history", "Remember what was copied",
		"Win+V shows the history; passwords marked as secret are never kept", true, 0, 0, 1);
	ui_taskbar_key(s, p->keys, history, "clipboard_paste", "Paste the entry that is picked",
		"Off only copies it back to the clipboard", true, 0, 0, 1);

	GtkWidget *clear_group = ui_group(content, "Clear clipboard data", NULL);
	GtkWidget *clear = gtk_button_new_with_label("Clear");
	g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), p);
	GtkWidget *row = ui_row(clear_group, "Clear the history",
		"Everything but the pinned entries; pin an entry in the Win+V list to keep it", clear);
	p->cleared = gtk_label_new("");
	gtk_widget_add_css_class(p->cleared, "dim-label");
	gtk_widget_set_visible(p->cleared, FALSE);
	gtk_box_prepend(GTK_BOX(ui_row_box(row)), p->cleared);

	ui_taskbar_keys_refresh(p->keys);
	return page;
}
