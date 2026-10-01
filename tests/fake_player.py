#!/usr/bin/env python3
"""A window that is a media player, for the tests of the media buttons.

It owns org.mpris.MediaPlayer2.tilewintest on the session bus, as players
do, and writes every Previous, PlayPause and Next it is asked for to the
log file given; PlayPause toggles between playing and paused and says so
with PropertiesChanged, as a real player would.

usage: fake_player.py <log file>
"""
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk  # noqa: E402

XML = """
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Previous"/>
    <method name="Next"/>
    <method name="PlayPause"/>
    <method name="Play"/>
    <method name="Pause"/>
    <method name="Stop"/>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
"""

log_path = sys.argv[1]
state = {"status": "Playing"}
connection = None


def log(line):
    with open(log_path, "a") as f:
        f.write(line + "\n")


def properties(interface):
    if interface == "org.mpris.MediaPlayer2":
        return {
            "Identity": GLib.Variant("s", "tileWin test player"),
            "DesktopEntry": GLib.Variant("s", "tilewin-fakeplayer"),
            "CanQuit": GLib.Variant("b", False),
            "CanRaise": GLib.Variant("b", False),
        }
    return {
        "PlaybackStatus": GLib.Variant("s", state["status"]),
        "CanGoNext": GLib.Variant("b", True),
        "CanGoPrevious": GLib.Variant("b", True),
        "CanPlay": GLib.Variant("b", True),
        "CanPause": GLib.Variant("b", True),
        "CanControl": GLib.Variant("b", True),
    }


def method_call(conn, sender, path, interface, method, params, invocation):
    log(method)
    if method == "PlayPause":
        state["status"] = "Paused" if state["status"] == "Playing" else "Playing"
        conn.emit_signal(None, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
            GLib.Variant("(sa{sv}as)", ("org.mpris.MediaPlayer2.Player",
                {"PlaybackStatus": GLib.Variant("s", state["status"])}, [])))
    invocation.return_value(None)


def get_property(conn, sender, path, interface, name):
    return properties(interface)[name]


def bus_acquired(conn, name):
    for info in Gio.DBusNodeInfo.new_for_xml(XML).interfaces:
        conn.register_object("/org/mpris/MediaPlayer2", info, method_call, get_property, None)


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title="Test player")
    window.set_default_size(320, 200)
    window.present()
    Gio.bus_own_name(Gio.BusType.SESSION, "org.mpris.MediaPlayer2.tilewintest",
        Gio.BusNameOwnerFlags.NONE, bus_acquired, lambda *a: log("ready"), None)


app = Gtk.Application(application_id="tilewin-fakeplayer",
    flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run([])
