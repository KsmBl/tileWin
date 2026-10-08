#!/bin/sh
# Which screen things go to, with three screens: two side by side on top and
# one in the middle below them.
#
#   +------------+------------+
#   | HEADLESS-1 | HEADLESS-2 |
#   +-----+------+------+-----+
#         | HEADLESS-3  |
#         +-------------+
#
#  - "move output <direction>" takes a window to the screen that way that
#    faces most of it: up from the bottom screen to the top screen over the
#    middle of the window, not over its corner; right from the top left screen
#    to the top right one, not the bottom one; round to the furthest screen
#    when there is none that way
#  - a dialog opens on the screen of the window it belongs to, over its middle,
#    while another screen has the focus
#  - a window of a fixed size naming no parent opens with its app's window;
#    one of an app with no window open on the main display
#
# usage: screen_choice.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
app="$(cd "$(dirname "$0")" && pwd)/dialog_app.py"

for needed in "$compositor" "$msg"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0")' 2>/dev/null; then
	echo "GTK 4 for Python is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-choice-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT INT TERM

# no taskbar: the usable area of a screen is then the whole screen
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command none
animations off
main_output HEADLESS-2
output HEADLESS-1 mode --custom 1920x1080 pos 0 0
output HEADLESS-2 mode --custom 1920x1080 pos 1920 0
output HEADLESS-3 mode --custom 1920x1080 pos 960 1080
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=3 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
export GDK_BACKEND=wayland GSK_RENDERER=cairo
ipc() { "$msg" -s "$sock" "$@"; }

ipc mode window >/dev/null 2>&1
sleep 1

# "<output> <x> <y> <width> <height>" of a window, its content area
window() {
	ipc -t get_tree | python3 -c '
import json, sys
title = sys.argv[1]
def walk(n, output):
    if n.get("type") == "output":
        output = n["name"]
    if n.get("name") == title and n.get("pid"):
        r = n["rect"]
        print(output, r["x"], r["y"], r["width"], r["height"])
        return True
    return any(walk(c, output) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin), None)' "$1"
}
wait_for() {
	attempt=0
	while [ -z "$(window "$1")" ] && [ $attempt -lt 40 ]; do
		sleep 0.25
		attempt=$((attempt + 1))
	done
	sleep 0.5
	if [ -z "$(window "$1")" ]; then
		echo "window $1 never came up"
		tail -n 5 "$work/log"
		exit 1
	fi
}
start() { # <output> <mode> <title> [args...]
	output=$1
	shift
	ipc focus output "$output" >/dev/null 2>&1
	ipc exec "python3 -I '$app' $*" >/dev/null 2>&1
	wait_for "$2"
}
# moves the window to x y, then to the screen that way; prints where it went
move_from() { # <title> <x> <y> <direction>
	ipc "[title=$1] move absolute position $2 $3" >/dev/null
	ipc "[title=$1] focus" >/dev/null
	sleep 0.3
	ipc move output "$4" >/dev/null
	sleep 0.5
	window "$1" | cut -d' ' -f1
}

start HEADLESS-1 window mover

# up from the bottom screen: the window lies across the line between the two
# top screens, its corner under the left one, most of it under the right one
went=$(move_from mover 1700 1300 up)
echo "up from the bottom screen, most of the window under the right top screen: $went"
[ "$went" = HEADLESS-2 ] || fail "up went to $went, not the top screen over most of the window"
went=$(move_from mover 1000 1300 up)
echo "up from the bottom screen, the window under the left top screen: $went"
[ "$went" = HEADLESS-1 ] || fail "up went to $went, not the top left screen"

# right from the top left screen: the top right one, not the bottom one,
# though that one starts further left
went=$(move_from mover 1000 200 right)
echo "right from the top left screen: $went"
[ "$went" = HEADLESS-2 ] || fail "right went to $went, not the top right screen"

# down from the top right screen: the bottom one
went=$(move_from mover 2400 200 down)
echo "down from the top right screen: $went"
[ "$went" = HEADLESS-3 ] || fail "down went to $went, not the bottom screen"

# right from the top right screen: nothing there, round to the top left one
went=$(move_from mover 2400 200 right)
echo "right from the top right screen: $went"
[ "$went" = HEADLESS-1 ] || fail "right from the last screen went to $went, not round to the first"

# a dialog of a window on the bottom screen, while the top left one has the
# focus: on the bottom screen, over the middle of its window
mkdir -p "$work/app"
start HEADLESS-3 parent owner "$work/app"
ipc "[title=owner] move absolute position 1100 1200" >/dev/null
ipc focus output HEADLESS-1 >/dev/null
sleep 0.5
touch "$work/app/dialog"
wait_for owner-dialog
set -- $(window owner)
owner_mid_x=$(($2 + $4 / 2))
set -- $(window owner-dialog)
echo "dialog of a window on the bottom screen: $*"
[ "$1" = HEADLESS-3 ] || fail "the dialog opened on $1, not on the screen of its window"
dialog_mid_x=$(($2 + $4 / 2))
offset=$((dialog_mid_x - owner_mid_x))
[ "${offset#-}" -le 2 ] || fail "the dialog is not over the middle of its window ($offset px off)"

# a window of a fixed size naming no parent, of the same app: with its window
ipc focus output HEADLESS-1 >/dev/null
sleep 0.5
touch "$work/app/lone"
wait_for owner-lone
went=$(window owner-lone | cut -d' ' -f1)
echo "parentless dialog of an app with a window on the bottom screen: $went"
[ "$went" = HEADLESS-3 ] || fail "it opened on $went, not with its app's window"

# one of an app with no window open, started from outside (no launch on a
# screen to follow), while the top left screen has the focus: on the main display
ipc focus output HEADLESS-1 >/dev/null
python3 -I "$app" lone stray >/dev/null 2>&1 &
wait_for stray
went=$(window stray | cut -d' ' -f1)
echo "parentless dialog of an app with no window: $went"
[ "$went" = HEADLESS-2 ] || fail "it opened on $went, not on the main display"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all passed"
