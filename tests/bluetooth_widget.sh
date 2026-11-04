#!/bin/sh
# The Bluetooth icon of the taskbar follows what BlueZ says. In a nested tileWin
# with a stand-in for BlueZ on a system bus of its own:
#  - without bluetoothd the icon takes no room
#  - with an adapter it shows, and the picture differs between off, on and a
#    connected device, each without restarting the taskbar
#  - clicking it opens the Bluetooth flyout
#  - without an adapter it takes no room again
#
# usage: bluetooth_widget.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
count=$build/tests/count-content
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$count" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon gdbus grim python3 cmp; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -I -c 'import gi; from gi.repository import Gio' 2>/dev/null; then
	echo "python3-gobject is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-bt-widget-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo "${BT_WIDGET_THEME:-win10}" > "$XDG_CONFIG_HOME/tileWin/current-theme"
cat > "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
layout window {
	position bottom
	left start
	right bluetooth showdesktop
}
EOF

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
bus_pid=
fake_pid=
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	[ -n "$fake_pid" ] && kill "$fake_pid" 2>/dev/null
	[ -n "$bus_pid" ] && kill "$bus_pid" 2>/dev/null
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# the system bus of this test
dbus-daemon --session --nofork --print-address=1 > "$work/bus" 2>/dev/null &
bus_pid=$!
attempt=0
while [ ! -s "$work/bus" ] && [ $attempt -lt 50 ]; do
	sleep 0.1
	attempt=$((attempt + 1))
done
if [ ! -s "$work/bus" ]; then
	echo "no private bus"
	exit 1
fi
export DBUS_SYSTEM_BUS_ADDRESS=$(cat "$work/bus")

# BlueZ as far as the taskbar sees it, driven by org.tilewin.test.Set
cat > "$work/fake_bluez.py" <<'EOF'
import sys
from gi.repository import Gio, GLib

ADAPTER, DEVICE = "/org/bluez/hci0", "/org/bluez/hci0/dev_00_11_22_33_44_55"
XML = """<node>
<interface name='org.freedesktop.DBus.ObjectManager'>
  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>
  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>
  <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>
</interface>
<interface name='org.tilewin.test'>
  <method name='Set'><arg type='s' direction='in'/></method>
</interface>
</node>"""
state = {"adapter": False, "powered": False, "connected": False}
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)


def adapter_props():
    return {"org.bluez.Adapter1": {"Powered": GLib.Variant("b", state["powered"]),
                                   "Discovering": GLib.Variant("b", False)}}


def device_props():
    return {"org.bluez.Device1": {"Alias": GLib.Variant("s", "Test Headphones"),
                                  "Address": GLib.Variant("s", "00:11:22:33:44:55"),
                                  "Paired": GLib.Variant("b", True),
                                  "Connected": GLib.Variant("b", state["connected"])}}


def emit(path, iface, member, signature, *args):
    bus.emit_signal(None, path, iface, member, GLib.Variant(signature, args))


def changed(path, iface, name, value):
    emit(path, "org.freedesktop.DBus.Properties", "PropertiesChanged", "(sa{sv}as)",
         iface, {name: GLib.Variant("b", value)}, [])


def apply(what):
    if what == "adapter":
        state["adapter"] = True
        emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "(oa{sa{sv}})",
             ADAPTER, adapter_props())
        emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "(oa{sa{sv}})",
             DEVICE, device_props())
    elif what == "no-adapter":
        state.update(adapter=False, powered=False, connected=False)
        emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "(oas)",
             DEVICE, ["org.bluez.Device1"])
        emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "(oas)",
             ADAPTER, ["org.bluez.Adapter1"])
    elif what in ("on", "off"):
        state["powered"] = what == "on"
        changed(ADAPTER, "org.bluez.Adapter1", "Powered", state["powered"])
    elif what in ("connected", "disconnected"):
        state["connected"] = what == "connected"
        changed(DEVICE, "org.bluez.Device1", "Connected", state["connected"])


def call(conn, sender, path, iface, method, params, invocation):
    if method == "GetManagedObjects":
        objects = {}
        if state["adapter"]:
            objects = {ADAPTER: adapter_props(), DEVICE: device_props()}
        invocation.return_value(GLib.Variant("(a{oa{sa{sv}}})", (objects,)))
    else:
        apply(params.unpack()[0])
        invocation.return_value(None)


node = Gio.DBusNodeInfo.new_for_xml(XML)
for i in node.interfaces:
    bus.register_object("/", i, call, None, None)


def acquired(*_):
    print("up", flush=True)


Gio.bus_own_name_on_connection(bus, "org.bluez", 0, acquired, None)
GLib.MainLoop().run()
EOF
python3 -I "$work/fake_bluez.py" > "$work/fake.out" 2>"$work/fake.err" &
fake_pid=$!
attempt=0
while [ ! -s "$work/fake.out" ] && [ $attempt -lt 50 ]; do
	sleep 0.1
	attempt=$((attempt + 1))
done
if [ ! -s "$work/fake.out" ]; then
	echo "the stand-in for BlueZ never came up"
	cat "$work/fake.err"
	exit 1
fi
bluez() {
	gdbus call -a "$DBUS_SYSTEM_BUS_ADDRESS" -d org.bluez -o / -m org.tilewin.test.Set "$1" >/dev/null ||
		fail "the stand-in for BlueZ did not take $1"
	sleep 0.7
}

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
polkit_agent disable
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$DBUS_SESSION_BUS_ADDRESS" > $work/dbus; printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display") DBUS_SESSION_BUS_ADDRESS=$(cat "$work/dbus")
sleep 3

# the right half of the taskbar, where the widget is
shot() {
	grim -g "640,680 640x40" "$work/$1.png"
	if [ -n "${BT_WIDGET_SHOTS:-}" ]; then
		mkdir -p "$BT_WIDGET_SHOTS" && cp "$work/$1.png" "$BT_WIDGET_SHOTS/"
	fi
}
differs() {
	if cmp -s "$work/$1.png" "$work/$2.png"; then
		fail "$3"
	fi
}
same() {
	cmp -s "$work/$1.png" "$work/$2.png" || fail "$3"
}

# the stand-in has no adapter yet, as with no bluetoothd
shot none
bluez adapter
shot off
differs none off "the icon does not appear when an adapter does"
bluez on
shot on
differs off on "switching Bluetooth on does not change the icon"
bluez connected
shot connected
differs on connected "connecting a device does not change the icon"
bluez disconnected
shot on2
same on on2 "disconnecting does not bring back the icon of Bluetooth on"

# a click on the icon, found as what appeared between "none" and "on", opens
# a flyout above the taskbar
grim "$work/before_flyout.png"
x=$(python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
a = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1])
b = GdkPixbuf.Pixbuf.new_from_file(sys.argv[2])
n, stride, pa, pb = a.get_n_channels(), a.get_rowstride(), a.get_pixels(), b.get_pixels()
# the bell or show desktop move too: the icon is what lies left of them
for x in range(a.get_width()):
    for y in range(a.get_height()):
        i = y * stride + x * n
        if pa[i:i + 3] != pb[i:i + 3]:
            print(640 + x + 12)
            sys.exit()
' "$work/none.png" "$work/connected.png")
if [ -z "$x" ]; then
	fail "the icon was not found on the taskbar"
else
	"$pointer" 1280 720 move "$x" 700 wait 300 click "$x" 700 >/dev/null 2>&1
	sleep 1
	grim "$work/after_flyout.png"
	if [ -n "${BT_WIDGET_SHOTS:-}" ]; then
		cp "$work/after_flyout.png" "$BT_WIDGET_SHOTS/"
	fi
	before=$("$count" "$work/before_flyout.png" 640 200 640 450)
	after=$("$count" "$work/after_flyout.png" 640 200 640 450)
	echo "above the taskbar: $before pixels before the click, $after after"
	[ "$after" -gt $((before + 2000)) ] || fail "clicking the icon opened no flyout"
fi
"$msg" -s "$sock" panel close >/dev/null 2>&1
sleep 0.5

bluez no-adapter
shot none2
same none none2 "the icon stays after the adapter is gone"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
