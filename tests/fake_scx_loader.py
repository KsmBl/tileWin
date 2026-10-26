#!/usr/bin/env python3
"""scx_loader for the tests of the scheduler for fullscreen windows.

It owns org.scx.Loader on the session bus, with the methods and properties of
the real one, and writes every StartScheduler, SwitchScheduler and
StopScheduler it is asked for to the log file given, as "Start scx_lavd 1".
What runs at first is the second argument ("unknown" for none) in the mode of
the third.

usage: fake_scx_loader.py <log file> [scheduler] [mode]
"""
import sys

from gi.repository import Gio, GLib

XML = """
<node>
  <interface name="org.scx.Loader">
    <property name="CurrentScheduler" type="s" access="read"/>
    <property name="SchedulerMode" type="u" access="read"/>
    <property name="SupportedSchedulers" type="as" access="read"/>
    <method name="StartScheduler">
      <arg name="scx_name" type="s" direction="in"/>
      <arg name="sched_mode" type="u" direction="in"/>
    </method>
    <method name="SwitchScheduler">
      <arg name="scx_name" type="s" direction="in"/>
      <arg name="sched_mode" type="u" direction="in"/>
    </method>
    <method name="StopScheduler"/>
  </interface>
</node>
"""

log_path = sys.argv[1]
state = {
    "current": sys.argv[2] if len(sys.argv) > 2 else "unknown",
    "mode": int(sys.argv[3]) if len(sys.argv) > 3 else 0,
}


def log(line):
    with open(log_path, "a") as f:
        f.write(line + "\n")


def on_call(connection, sender, path, interface, method, params, invocation):
    if method in ("StartScheduler", "SwitchScheduler"):
        name, mode = params.unpack()
        log("%s %s %d" % (method.replace("Scheduler", ""), name, mode))
        state["current"], state["mode"] = name, mode
    elif method == "StopScheduler":
        log("Stop")
        state["current"], state["mode"] = "unknown", 0
    invocation.return_value(None)


def on_get(connection, sender, path, interface, name):
    if name == "CurrentScheduler":
        return GLib.Variant("s", state["current"])
    if name == "SchedulerMode":
        return GLib.Variant("u", state["mode"])
    if name == "SupportedSchedulers":
        return GLib.Variant("as", ["scx_lavd", "scx_bpfland"])
    return None


def on_bus(connection, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
    connection.register_object("/org/scx/Loader", info, on_call, on_get, None)


def on_name(connection, name):
    log("ready")


Gio.bus_own_name(Gio.BusType.SESSION, "org.scx.Loader", Gio.BusNameOwnerFlags.NONE,
                 on_bus, on_name, None)
GLib.MainLoop().run()
