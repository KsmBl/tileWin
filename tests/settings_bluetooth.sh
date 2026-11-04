#!/bin/sh
# Searching on the Bluetooth page of the settings: when the search found a
# device, the signal of BlueZ's object manager reached the page with the device
# in place of the page itself, and the window went down. In a nested tileWin with
# a stand-in for BlueZ on a system bus of its own:
#  - a click on Search asks BlueZ to look for devices
#  - while devices turn up, change and go away the window stays
#  - the devices with a name are listed, so the rows really were built
#  - a click on Stop asks BlueZ to stop, and the window is still there
#
# usage: settings_bluetooth.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
pointer=$build/tests/pointer-tool
count=$build/tests/count-content

for needed in "$compositor" "$msg" "$settings" "$pointer" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-set-bt-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo win10 > "$XDG_CONFIG_HOME/tileWin/current-theme"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() {
	"$msg" -s "$sock" "$@"
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

# BlueZ with one powered adapter. A search brings devices, one after the other
# as a real one does, then changes them and takes one away again.
cat > "$work/fake_bluez.py" <<'EOF'
import sys
from gi.repository import Gio, GLib

ADAPTER = "/org/bluez/hci0"
ROOT_XML = """<node><interface name='org.freedesktop.DBus.ObjectManager'>
  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>
  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>
  <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>
</interface></node>"""
ADAPTER_XML = """<node><interface name='org.bluez.Adapter1'>
  <method name='StartDiscovery'/>
  <method name='StopDiscovery'/>
</interface></node>"""
FOUND = [("AA", "Test Headphones", "audio-headset"), ("BB", "Test Mouse", "input-mouse"),
         ("CC", "CC-11-22-33-44-55", None), ("DD", "Test Phone", "phone"),
         ("EE", "Goes Away Again", None)]
log = open(sys.argv[1], "a", buffering=1)
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
discovering = False
devices = {}


def adapter_props():
    return {"org.bluez.Adapter1": {"Powered": GLib.Variant("b", True),
                                   "Discovering": GLib.Variant("b", discovering)}}


def path_of(key):
    return "%s/dev_%s_11_22_33_44_55" % (ADAPTER, key)


def emit(path, iface, member, signature, *args):
    bus.emit_signal(None, path, iface, member, GLib.Variant(signature, args))


def set_discovering(on):
    global discovering
    discovering = on
    emit(ADAPTER, "org.freedesktop.DBus.Properties", "PropertiesChanged", "(sa{sv}as)",
         "org.bluez.Adapter1", {"Discovering": GLib.Variant("b", on)}, [])


def add_device(found):
    key, name, icon = found
    props = {"Alias": GLib.Variant("s", name), "Paired": GLib.Variant("b", False),
             "Connected": GLib.Variant("b", False), "RSSI": GLib.Variant("n", -60)}
    if icon:
        props["Icon"] = GLib.Variant("s", icon)
    devices[path_of(key)] = {"org.bluez.Device1": props}
    emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "(oa{sa{sv}})",
         path_of(key), devices[path_of(key)])
    log.write("added %s\n" % key)


def change_device(key):
    emit(path_of(key), "org.freedesktop.DBus.Properties", "PropertiesChanged", "(sa{sv}as)",
         "org.bluez.Device1", {"RSSI": GLib.Variant("n", -40)}, [])
    return False


def remove_device(key):
    devices.pop(path_of(key))
    emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "(oas)",
         path_of(key), ["org.bluez.Device1"])
    log.write("removed %s\n" % key)
    return False


def later(ms, fn, *args):
    def run():
        fn(*args)
        return False
    GLib.timeout_add(ms, run)


def root_call(conn, sender, path, iface, method, params, invocation):
    objects = {ADAPTER: adapter_props()}
    objects.update(devices)
    invocation.return_value(GLib.Variant("(a{oa{sa{sv}}})", (objects,)))


def adapter_call(conn, sender, path, iface, method, params, invocation):
    log.write("%s\n" % method)
    set_discovering(method == "StartDiscovery")
    invocation.return_value(None)
    if method == "StartDiscovery":
        for i, found in enumerate(FOUND):
            later(200 + 200 * i, add_device, found)
        later(1500, change_device, "AA")
        later(1700, remove_device, "EE")


bus.register_object("/", Gio.DBusNodeInfo.new_for_xml(ROOT_XML).interfaces[0], root_call, None, None)
bus.register_object(ADAPTER, Gio.DBusNodeInfo.new_for_xml(ADAPTER_XML).interfaces[0], adapter_call,
                    None, None)


def acquired(*_):
    print("up", flush=True)


Gio.bus_own_name_on_connection(bus, "org.bluez", 0, acquired, None)
GLib.MainLoop().run()
EOF
: > "$work/calls"
python3 -I "$work/fake_bluez.py" "$work/calls" > "$work/fake.out" 2>"$work/fake.err" &
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

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
polkit_agent disable
output HEADLESS-1 mode --custom 1280x800 pos 0 0
exec $settings --page=bluetooth > $work/settings.log 2>&1
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
export WAYLAND_DISPLAY=$(cat "$work/display")

window_there() {
	ipc -t get_tree | grep -q '"name": "tileWin Settings"'
}
attempt=0
while ! window_there && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
window_there || { echo "the settings never opened"; tail -n 5 "$work/settings.log"; exit 1; }
sleep 3
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 1.5

shot() {
	grim "$work/$1.png"
	if [ -n "${BT_SETTINGS_SHOTS:-}" ]; then
		mkdir -p "$BT_SETTINGS_SHOTS" && cp "$work/$1.png" "$BT_SETTINGS_SHOTS/"
	fi
}
search_x=1190
search_y=375

shot before
before=$("$count" "$work/before.png" 200 0 1080 800)
# the window is new: the pointer has to come in before a click counts
"$pointer" 1280 800 move "$search_x" "$search_y"
sleep 1
"$pointer" 1280 800 click "$search_x" "$search_y"
sleep 4
window_there || fail "the window went down during the search: $(tail -n 3 "$work/settings.log")"
grep -q StartDiscovery "$work/calls" || fail "the click on Search did not reach BlueZ"
[ "$(grep -c '^added' "$work/calls")" = 5 ] || fail "not all devices turned up: $(cat "$work/calls")"
grep -q '^removed EE' "$work/calls" || fail "the device that goes away did not"

if window_there; then
	shot searching
	searching=$("$count" "$work/searching.png" 200 0 1080 800)
	echo "the page: $before pixels before the search, $searching with the devices"
	[ "$searching" -gt $((before + 500)) ] || fail "the devices found are not listed"
	# there, unchanged by the rows below it
	"$pointer" 1280 800 click "$search_x" "$search_y"
	sleep 1.5
	grep -q StopDiscovery "$work/calls" || fail "the click on Stop did not reach BlueZ"
	window_there || fail "the window went down when the search stopped"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
