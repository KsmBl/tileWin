#!/usr/bin/env python3
"""A window with a text field that writes what is in it to a file whenever it
changes, for tests that type into a window (the emoji picker).

usage: text_target.py <file>
"""
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk  # noqa: E402

path = sys.argv[1]


def changed(entry):
    with open(path, "w", encoding="utf-8") as f:
        f.write(entry.get_text())


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title="Text target")
    window.set_default_size(400, 120)
    entry = Gtk.Entry()
    entry.connect("changed", changed)
    window.set_child(entry)
    window.present()
    entry.grab_focus()
    open(path, "w").close()


app = Gtk.Application(application_id="tilewin-texttarget", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run([])
