#!/bin/sh
# Opens every page of the settings app in a nested tileWin that draws into
# memory, and looks at what came out. A page that builds without a word of
# complaint can still lay itself out to nothing, which is what this catches.
#
# The pages are split into groups, so that several runs of the script take a
# share of them each, side by side: run <group> of <groups> takes the pages
# whose place in the list (counted from 0) leaves <group> - 1 when divided by
# <groups>. Without them it takes every page.
#
# usage: render_pages.sh <build dir> <source dir> [<group> <groups>]
set -u

build=$1
group=${3:-1}
groups=${4:-1}
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
count_content=$build/tests/count-content
. "$(dirname "$0")/wait.sh"

for needed in "$compositor" "$msg" "$settings" "$count_content"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-daemon pkill cksum; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

# every page of the settings app (the pages[] of settings/main.c), once
all_pages="theme wallpaper desktop animations windows appwindows screen screensaver sound datetime bluetooth network dns taskbar startmenu launcher keyboard mouse apps account login backup kernel devices about"
pages=
index=0
for page in $all_pages; do
	[ $((index % groups)) -eq $((group - 1)) ] && pages="$pages $page"
	index=$((index + 1))
done

work=$(mktemp -d)
bus_pid=
fake_pid=
# killing the shell that starts the compositor does not kill the compositor, and
# a left over one holds on to an output; the config path is unique to this run
cleanup() {
	pkill -f "^[^ ]*/tilewin -c $work/" 2>/dev/null
	[ -n "$fake_pid" ] && kill "$fake_pid" 2>/dev/null
	[ -n "$bus_pid" ] && kill "$bus_pid" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# A session of its own, or it reads the config of whoever runs the test and
# brings back the apps their last session had open.
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
export TILEWIN_NO_APP_TWEAKS=1 # leaves the real desktop and home alone
# The bus of each session starts an accessibility bus of its own, and they all
# put their socket at the same place in XDG_RUNTIME_DIR; with several sessions
# at once GTK finds the one another session has just taken away, and complains.
# What is looked at here is what the pages draw.
export GTK_A11Y=none NO_AT_BRIDGE=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

# The desktop behind the window is one flat color, so that a window that never
# turned up cannot be mistaken for a page that drew itself.
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
exec "$settings" --page=PAGE
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

# The Bluetooth page with no BlueZ (no adapter, or the service off) is a single
# line that says so, and rightly draws little more. So that the page itself is
# drawn, it gets a stand-in for BlueZ on a system bus of its own: one powered
# adapter and a pair of headphones paired with it.
# Returns 1 when it cannot be had here, 2 when it did not come up.
start_fake_bluez() {
	if ! python3 -I -c 'import gi; from gi.repository import Gio' 2>/dev/null; then
		echo "bluetooth: python3-gobject is not installed for a stand-in for BlueZ: not looked at"
		return 1
	fi
	dbus-daemon --session --nofork --print-address=1 > "$work/bus" 2>/dev/null &
	bus_pid=$!
	if ! wait_for_file 10 "$work/bus"; then
		echo "bluetooth: no system bus of its own"
		return 2
	fi
	cat > "$work/fake_bluez.py" <<'EOF'
from gi.repository import Gio, GLib

ROOT_XML = """<node><interface name='org.freedesktop.DBus.ObjectManager'>
  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>
  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>
  <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>
</interface></node>"""
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)


def root_call(conn, sender, path, iface, method, params, invocation):
    objects = {
        "/org/bluez/hci0": {"org.bluez.Adapter1": {
            "Powered": GLib.Variant("b", True), "Discovering": GLib.Variant("b", False)}},
        "/org/bluez/hci0/dev_AA_11_22_33_44_55": {"org.bluez.Device1": {
            "Alias": GLib.Variant("s", "Test Headphones"),
            "Icon": GLib.Variant("s", "audio-headset"),
            "Paired": GLib.Variant("b", True), "Connected": GLib.Variant("b", True)}},
    }
    invocation.return_value(GLib.Variant("(a{oa{sa{sv}}})", (objects,)))


bus.register_object("/", Gio.DBusNodeInfo.new_for_xml(ROOT_XML).interfaces[0], root_call,
                    None, None)
Gio.bus_own_name_on_connection(bus, "org.bluez", 0, lambda *_: print("up", flush=True), None)
GLib.MainLoop().run()
EOF
	DBUS_SYSTEM_BUS_ADDRESS=$(cat "$work/bus") python3 -I "$work/fake_bluez.py" \
		> "$work/fake.out" 2> "$work/fake.err" &
	fake_pid=$!
	if ! wait_for_file 10 "$work/fake.out"; then
		echo "bluetooth: the stand-in for BlueZ never came up"
		cat "$work/fake.err"
		return 2
	fi
}

# The page sits right of the list of pages and below the heading. The area is
# kept well inside it, so that neither the desktop nor the window's own frame
# can be counted as something the page drew.
area_x=420
area_y=140
area_width=560
area_height=420
# The emptiest page still draws a few thousand pixels of rows, borders and
# text; a flat desktop, or a page that laid itself out to nothing, draws none
# at all, so anywhere in between tells the two apart.
least_content=1500

window_there() {
	"$msg" -s "$sock" -t get_tree | grep -q '"app_id": "org.tilewin.Settings"'
}
# what the screen shows, as a number that stays the same while nothing is drawn
screen_sum() {
	WAYLAND_DISPLAY=$display grim -t ppm - | cksum
}

failures=0
for page in $pages; do
	system_bus=
	if [ "$page" = bluetooth ]; then
		start_fake_bluez
		case $? in
		1) continue ;;
		2) failures=$((failures + 1)); continue ;;
		esac
		system_bus=$(cat "$work/bus")
	fi
	sed "s|--page=PAGE|--page=$page|" "$work/tilewin.conf" > "$work/$page.conf"
	rm -f "$work/display"
	shot=$work/$page.png
	log=$work/$page.log
	sock=$work/$page.sock
	env -u WAYLAND_DISPLAY -u DISPLAY \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
		SWAYSOCK="$sock" TILEWINSOCK="$sock" ${system_bus:+DBUS_SYSTEM_BUS_ADDRESS=$system_bus} \
		"$(dirname "$0")/session.sh" "$compositor" -c "$work/$page.conf" > "$log" 2>&1 &
	starter=$!

	wait_for_file 30 "$work/display"
	display=$(cat "$work/display" 2>/dev/null)
	if [ -z "$display" ]; then
		echo "$page: the nested compositor never came up"
		tail -n 2 "$log"
		failures=$((failures + 1))
		kill "$starter" 2>/dev/null
		pkill -f "^[^ ]*/tilewin -c $work/$page.conf" 2>/dev/null
		continue
	fi

	# One shot, once the window is there and the screen has stopped changing:
	# it has come in and its page has laid itself out. Looking again until
	# something turns up would accept a half-drawn frame as a page.
	content=0
	if ! wait_until 30 window_there; then
		echo "$page: the settings window never opened"
	elif ! wait_still 20 screen_sum; then
		echo "$page: the screen never stopped changing; looked at all the same"
	fi
	WAYLAND_DISPLAY=$display grim "$shot" 2>>"$log"
	if [ -s "$shot" ]; then
		content=$("$count_content" "$shot" $area_x $area_y $area_width $area_height)
	fi
	kill "$starter" 2>/dev/null
	pkill -f "^[^ ]*/tilewin -c $work/$page.conf" 2>/dev/null
	wait "$starter" 2>/dev/null

	if [ ! -s "$shot" ]; then
		echo "$page: no screenshot was taken"
		failures=$((failures + 1))
		continue
	fi
	if [ "$content" -lt "$least_content" ]; then
		echo "$page: only $content pixel(s) of the page area were drawn on"
		failures=$((failures + 1))
	else
		echo "$page: $content pixels drawn"
	fi
	if grep -q "CRITICAL\|assertion .* failed" "$log"; then
		echo "$page: GTK complained:"
		grep -m 3 "CRITICAL\|assertion" "$log"
		failures=$((failures + 1))
	fi
done

if [ $failures -gt 0 ]; then
	echo "$failures page(s) did not come out right"
	exit 1
fi
echo "the pages drew themselves:$pages"
