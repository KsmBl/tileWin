#!/bin/sh
# Alt+Tab goes through the windows of every screen, not only the one with the
# focus. Two screens side by side, window A on the left one, window B on the
# right one and window C on a desktop of the left screen that is not on view:
#  - from A, Alt+Tab goes to B on the other screen, and the focus moves there;
#  - from B it goes back to A;
#  - backwards it reaches only A and B: C, on a desktop not on view, stays out.
#
# usage: alttab_screens.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session xfce4-terminal python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-alttab-screens-$$.sock
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
output HEADLESS-1 mode --custom 1280x720 pos 0 0
output HEADLESS-2 mode --custom 1920x1080 pos 1280 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman \
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
ipc() { "$msg" -s "$sock" "$@"; }

ipc mode window >/dev/null 2>&1
sleep 1

# "<output> <x> <y> <width> <height> <maximized> <minimized> <focused>" of a window;
# the rect is the content area, the title bar lies above it
window() {
	ipc -t get_tree | python3 -c '
import json, sys
title = sys.argv[1]
def walk(n, output):
    if n.get("type") == "output":
        output = n["name"]
    if n.get("name") == title and n.get("pid"):
        r = n["rect"]
        print(output, r["x"], r["y"], r["width"], r["height"],
              int(bool(n.get("maximized"))), int(bool(n.get("minimized"))),
              int(bool(n.get("focused"))))
        return True
    return any(walk(c, output) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin), None)' "$1"
}
field() { window "$1" | cut -d' ' -f"$2"; }

# the title of the window with the focus and its screen: "<title> <output>"
focused() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, output):
    if n.get("type") == "output":
        output = n["name"]
    if n.get("focused") and n.get("pid"):
        print(n["name"], output)
        return True
    return any(walk(c, output) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin), None)'
}
open_on() {
	ipc focus output "$1" >/dev/null 2>&1
	ipc exec "xfce4-terminal --disable-server -T $2" >/dev/null 2>&1
	attempt=0
	while [ -z "$(window "$2")" ] && [ $attempt -lt 40 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
	done
	sleep 1
	[ -n "$(window "$2")" ] || { echo "window $2 did not open"; exit 1; }
}
# Alt+Tab over IPC: without a modifier held it switches at once
alttab() {
	ipc alttab "$1" >/dev/null 2>&1
	sleep 0.5
}

open_on HEADLESS-1 C
ipc move container to workspace number 7 >/dev/null 2>&1
sleep 0.5
open_on HEADLESS-2 B
open_on HEADLESS-1 A
[ "$(focused)" = "A HEADLESS-1" ] || fail "A does not have the focus at first ($(focused))"

alttab next
[ "$(focused)" = "B HEADLESS-2" ] ||
	fail "Alt+Tab from A does not go to B on the other screen ($(focused))"
alttab next
[ "$(focused)" = "A HEADLESS-1" ] || fail "Alt+Tab from B does not go back to A ($(focused))"
alttab prev
[ "$(focused)" = "B HEADLESS-2" ] ||
	fail "Alt+Shift+Tab from A reaches something else than B ($(focused))"
[ "$(field C 1)" = HEADLESS-1 ] || fail "C left its screen"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "Alt+Tab goes through the windows of every screen"
