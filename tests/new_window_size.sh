#!/bin/sh
# New windows about as big as the screen open maximized, not a few pixels
# short of it, in a nested tileWin (1280x720, no taskbar):
#  - an X11 window of 1250x700 opens maximized, and restoring it gives a
#    window of the usual size (60% by 65% of the screen);
#  - an X11 window of 700x400 opens as it is;
#  - a Wayland (GTK 3) window of 1270x710 opens maximized.
#
# usage: new_window_size.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$msg"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session Xwayland python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import Xlib' 2>/dev/null; then
	echo "python-xlib is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-newsize-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
apps=
cleanup() {
	for app in $apps; do
		kill "$app" 2>/dev/null
	done
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command true
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$DISPLAY" > $work/xdisplay; printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
if [ ! -s "$work/display" ] || [ ! -s "$work/xdisplay" ]; then
	echo "the nested compositor (with Xwayland) never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display") DISPLAY=$(cat "$work/xdisplay")
ipc() { "$msg" -s "$sock" "$@"; }

# the focused window: x y width height maximized
focused() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("focused"):
			r = c["rect"]
			print(r["x"], r["y"], r["width"], r["height"], int(bool(c.get("maximized"))))
		walk(c)
walk(json.load(sys.stdin))'
}
# opens an X11 window of that size and waits for it
x11_window() {
	python3 "$source_dir/tests/x11_titlebar.py" "$1" "$2" < /dev/zero > /dev/null 2>&1 &
	apps="$apps $!"
	sleep 1.5
}

x11_window 1250 700
set -- $(focused)
[ "${5:-}" = 1 ] || fail "an X11 window of 1250x700 did not open maximized: $*"
ipc maximize disable >/dev/null
sleep 0.5
set -- $(focused)
[ "${3:-}" = 768 ] && [ "${4:-}" = 468 ] && [ "${5:-}" = 0 ] ||
	fail "restoring it did not give the usual size (768x468): $*"

x11_window 700 400
set -- $(focused)
[ "${3:-}" = 700 ] && [ "${4:-}" = 400 ] && [ "${5:-}" = 0 ] ||
	fail "an X11 window of 700x400 did not open as it is: $*"

if python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' \
		2>/dev/null; then
	GDK_BACKEND=wayland python3 -c '
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="big")
w.set_default_size(1270, 710)
w.show_all()
Gtk.main()' > /dev/null 2>&1 &
	apps="$apps $!"
	sleep 2
	set -- $(focused)
	[ "${5:-}" = 1 ] || fail "a Wayland window of 1270x710 did not open maximized: $*"
else
	echo "GTK 3 for python is not installed: the Wayland window is not tried"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
