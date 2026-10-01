#!/usr/bin/env python3
"""An app that shows progress and a count on its taskbar button, the way
Firefox or a mail app does: it sends com.canonical.Unity.LauncherEntry.Update
on the session bus and stays on the bus until told to quit.

usage: launcher_entry.py <desktop id>
Commands on stdin, one per line:
  progress <0-1>    show that much progress
  count <n>         show that number
  clear             show nothing
  quit
"""
import sys
from gi.repository import Gio, GLib

desktop_id = sys.argv[1]
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def update(props):
	bus.emit_signal(None, "/com/canonical/unity/launcherentry/1",
		"com.canonical.Unity.LauncherEntry", "Update",
		GLib.Variant("(sa{sv})", ("application://" + desktop_id, props)))
	bus.flush_sync(None)


print("ready", flush=True)
for line in sys.stdin:
	words = line.split()
	if not words:
		continue
	if words[0] == "progress":
		update({"progress": GLib.Variant("d", float(words[1])),
			"progress-visible": GLib.Variant("b", True)})
	elif words[0] == "count":
		update({"count": GLib.Variant("x", int(words[1])),
			"count-visible": GLib.Variant("b", True)})
	elif words[0] == "clear":
		update({"progress-visible": GLib.Variant("b", False),
			"count-visible": GLib.Variant("b", False)})
	elif words[0] == "quit":
		break
	print("done", flush=True)
