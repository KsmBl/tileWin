#!/usr/bin/env python3
"""A GTK 3 app asking "Save changes?" with a message dialog, for the tests of
tileWin's dialogs. Writes the response of the dialog to <result file>.

usage: gtk3_dialog_app.py <result file> [plain|checkbox]
  plain     only texts and buttons: tileWin shows it as its own
  checkbox  with a check box the app put in: GTK's own dialog stays
"""
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

result, kind = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "plain"


def ask(window):
    dialog = Gtk.MessageDialog(transient_for=window, modal=True,
                               message_type=Gtk.MessageType.QUESTION,
                               text='Save changes to "notes.txt" before closing?')
    dialog.format_secondary_text("If you don't save, your changes will be lost.")
    dialog.add_button("Do_n't Save", Gtk.ResponseType.NO)
    dialog.add_button("_Cancel", Gtk.ResponseType.CANCEL)
    dialog.add_button("_Save", Gtk.ResponseType.YES)
    dialog.set_default_response(Gtk.ResponseType.YES)
    if kind == "checkbox":
        check = Gtk.CheckButton(label="Don't ask again")
        check.show()
        dialog.get_message_area().pack_start(check, False, False, 0)
    response = dialog.run()
    dialog.destroy()
    with open(result + ".new", "w") as f:
        f.write("%d\n" % response)
    import os
    os.rename(result + ".new", result)
    Gtk.main_quit()
    return False


GLib.set_prgname("tilewin-test-editor")  # its app id
window = Gtk.Window(title="Editor")
window.set_default_size(500, 350)
window.connect("destroy", Gtk.main_quit)
window.show_all()
GLib.timeout_add(500, ask, window)
Gtk.main()
