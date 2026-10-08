#!/usr/bin/env python3
# A GTK 4 app for the tests of where windows and dialogs open.
#
#   dialog_app.py window <title>
#       a plain window
#   dialog_app.py lone <title>
#       a window of a fixed size naming no parent: a dialog of no window
#   dialog_app.py parent <title> <dir>
#       a window that opens, once <dir>/dialog exists, a dialog of it titled
#       "<title>-dialog", and once <dir>/lone exists, a window of a fixed size
#       naming no parent titled "<title>-lone"
import os
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402


def fixed_window(title, parent=None):
    win = Gtk.Window(title=title)
    win.set_default_size(360, 160)
    win.set_resizable(False)
    if parent is not None:
        win.set_transient_for(parent)
        win.set_modal(True)
    win.set_child(Gtk.Label(label=title))
    win.present()
    return win


def main():
    mode, title = sys.argv[1], sys.argv[2]
    app = Gtk.Application(application_id=None)
    keep = []

    def activate(app):
        if mode == "lone":
            win = fixed_window(title)
            app.add_window(win)
            return
        win = Gtk.ApplicationWindow(application=app, title=title)
        win.set_default_size(800, 500)
        win.set_child(Gtk.Label(label=title))
        win.present()
        if mode != "parent":
            return
        folder = sys.argv[3]
        opened = set()

        def poll():
            for name, make in (
                ("dialog", lambda: fixed_window(title + "-dialog", win)),
                ("lone", lambda: fixed_window(title + "-lone")),
            ):
                if name not in opened and os.path.exists(os.path.join(folder, name)):
                    opened.add(name)
                    keep.append(make())
            return True

        GLib.timeout_add(100, poll)

    app.connect("activate", activate)
    app.run([])


main()
