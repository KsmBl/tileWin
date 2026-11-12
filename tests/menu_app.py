#!/usr/bin/env python3
# A GTK 3 window with a menu bar, as Thunar has, for the test of the title bar
# buttons while one of its menus is open. Says "closed" when it is closed.
#
#   menu_app.py <title>
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk  # noqa: E402

win = Gtk.Window(title=sys.argv[1])
win.set_default_size(400, 300)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
bar = Gtk.MenuBar()
for top in ("File", "Edit", "View"):
    item = Gtk.MenuItem(label=top)
    menu = Gtk.Menu()
    for label in ("Cut", "Copy", "Paste"):
        menu.append(Gtk.MenuItem(label=label))
    item.set_submenu(menu)
    bar.append(item)
box.pack_start(bar, False, False, 0)
win.add(box)


def closed(*args):
    print("closed", flush=True)
    Gtk.main_quit()


win.connect("destroy", closed)
win.show_all()
print("ready", flush=True)
Gtk.main()
