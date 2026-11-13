#!/usr/bin/env python3
"""A fingerprint reader for the tests: net.reactivated.Fprint on the session
bus (TILEWIN_FPRINT_BUS=session), with one device of 3 enroll stages.

usage: fake_fprintd.py <dir>
  <dir>/fingers   the enrolled fingers, one a line (written by it, read by the test)
  <dir>/touch     written by the test: "match" or "no-match", the next finger
                  laid on the reader while it verifies; taken away once used
  <dir>/calls     every method called, one a line
  <dir>/ready     there once the name is owned
Enrolling passes a stage every 0.3 seconds and completes after the third.
"""
import os
import sys

from gi.repository import Gio, GLib

DIR = sys.argv[1]
DEVICE = "/net/reactivated/Fprint/Device/0"
STAGES = 3

XML = """
<node>
  <interface name="net.reactivated.Fprint.Manager">
    <method name="GetDevices"><arg type="ao" direction="out"/></method>
    <method name="GetDefaultDevice"><arg type="o" direction="out"/></method>
  </interface>
  <interface name="net.reactivated.Fprint.Device">
    <method name="ListEnrolledFingers"><arg type="s" direction="in"/><arg type="as" direction="out"/></method>
    <method name="DeleteEnrolledFingers"><arg type="s" direction="in"/></method>
    <method name="DeleteEnrolledFingers2"/>
    <method name="DeleteEnrolledFinger"><arg type="s" direction="in"/></method>
    <method name="Claim"><arg type="s" direction="in"/></method>
    <method name="Release"/>
    <method name="VerifyStart"><arg type="s" direction="in"/></method>
    <method name="VerifyStop"/>
    <method name="EnrollStart"><arg type="s" direction="in"/></method>
    <method name="EnrollStop"/>
    <signal name="VerifyStatus"><arg type="s"/><arg type="b"/></signal>
    <signal name="EnrollStatus"><arg type="s"/><arg type="b"/></signal>
    <property name="name" type="s" access="read"/>
    <property name="num-enroll-stages" type="i" access="read"/>
    <property name="scan-type" type="s" access="read"/>
    <property name="finger-present" type="b" access="read"/>
    <property name="finger-needed" type="b" access="read"/>
  </interface>
</node>
"""

state = {"claimed": None, "verifying": False, "enrolling": None, "stage": 0}
conn = None


def log(line):
    with open(os.path.join(DIR, "calls"), "a") as f:
        f.write(line + "\n")


def fingers():
    try:
        with open(os.path.join(DIR, "fingers")) as f:
            return [l.strip() for l in f if l.strip()]
    except OSError:
        return []


def save(names):
    with open(os.path.join(DIR, "fingers.new"), "w") as f:
        f.write("".join(n + "\n" for n in names))
    os.rename(os.path.join(DIR, "fingers.new"), os.path.join(DIR, "fingers"))


def emit(name, result, done):
    conn.emit_signal(None, DEVICE, "net.reactivated.Fprint.Device", name,
                     GLib.Variant("(sb)", (result, done)))


def error(inv, name, text):
    inv.return_dbus_error("net.reactivated.Fprint.Error." + name, text)


def enroll_step():
    if not state["enrolling"]:
        return False
    state["stage"] += 1
    if state["stage"] < STAGES:
        emit("EnrollStatus", "enroll-stage-passed", False)
        return True
    names = fingers()
    if state["enrolling"] not in names:
        names.append(state["enrolling"])
    save(names)
    state["enrolling"] = None
    emit("EnrollStatus", "enroll-completed", True)
    return False


def watch_touch():
    path = os.path.join(DIR, "touch")
    if state["verifying"] and os.path.exists(path):
        with open(path) as f:
            what = f.read().strip()
        os.unlink(path)
        state["verifying"] = False
        emit("VerifyStatus", "verify-match" if what == "match" else "verify-no-match", True)
    return True


def method(c, sender, path, iface, name, params, inv):
    args = params.unpack()
    log(name + (" " + " ".join(str(a) for a in args) if args else ""))
    if name == "GetDevices":
        inv.return_value(GLib.Variant("(ao)", ([DEVICE],)))
    elif name == "GetDefaultDevice":
        inv.return_value(GLib.Variant("(o)", (DEVICE,)))
    elif name == "ListEnrolledFingers":
        names = fingers()
        if not names:
            error(inv, "NoEnrolledPrints", "no fingers enrolled")
        else:
            inv.return_value(GLib.Variant("(as)", (names,)))
    elif name == "Claim":
        if state["claimed"] and state["claimed"] != sender:
            error(inv, "AlreadyInUse", "claimed by another")
        else:
            state["claimed"] = sender
            inv.return_value(None)
    elif name == "Release":
        state["claimed"] = None
        state["verifying"] = False
        state["enrolling"] = None
        inv.return_value(None)
    elif state["claimed"] != sender:
        error(inv, "ClaimDevice", "claim the device first")
    elif name == "VerifyStart":
        if not fingers():
            error(inv, "NoEnrolledPrints", "no fingers enrolled")
            return
        state["verifying"] = True
        inv.return_value(None)
    elif name == "VerifyStop":
        state["verifying"] = False
        inv.return_value(None)
    elif name == "EnrollStart":
        state["enrolling"] = args[0]
        state["stage"] = 0
        inv.return_value(None)
        GLib.timeout_add(300, enroll_step)
    elif name == "EnrollStop":
        state["enrolling"] = None
        inv.return_value(None)
    elif name == "DeleteEnrolledFinger":
        names = fingers()
        if args[0] not in names:
            error(inv, "NoEnrolledPrints", "not enrolled")
            return
        names.remove(args[0])
        save(names)
        inv.return_value(None)
    elif name in ("DeleteEnrolledFingers", "DeleteEnrolledFingers2"):
        save([])
        inv.return_value(None)
    else:
        inv.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", name)


def get_property(c, sender, path, iface, name):
    return {
        "name": GLib.Variant("s", "Test Reader"),
        "num-enroll-stages": GLib.Variant("i", STAGES),
        "scan-type": GLib.Variant("s", "press"),
        "finger-present": GLib.Variant("b", False),
        "finger-needed": GLib.Variant("b", False),
    }.get(name)


def acquired(c, name):
    global conn
    conn = c
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    c.register_object("/net/reactivated/Fprint/Manager", node.interfaces[0], method, None, None)
    c.register_object(DEVICE, node.interfaces[1], method, get_property, None)
    open(os.path.join(DIR, "ready"), "w").close()


Gio.bus_own_name(Gio.BusType.SESSION, "net.reactivated.Fprint", Gio.BusNameOwnerFlags.NONE,
                 None, acquired, None)
GLib.timeout_add(100, watch_touch)
GLib.MainLoop().run()
