#!/usr/bin/env python3
"""A window of one color, for tests that look at pictures of windows.

usage: color_window.py <title> <#rrggbb>
"""
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gio, Gtk  # noqa: E402

title, color = sys.argv[1], sys.argv[2]


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title=title)
    window.set_default_size(400, 300)
    css = Gtk.CssProvider()
    css.load_from_string("window, box { background: %s; }" % color)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css,
        Gtk.STYLE_PROVIDER_PRIORITY_USER)
    window.set_child(Gtk.Box())
    window.present()


app = Gtk.Application(application_id="tilewin-colorwindow", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run([])
