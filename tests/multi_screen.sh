#!/bin/sh
# Several screens in window mode. Two screens of different sizes side by side,
# the small one on the left:
#  - a maximized window taken to the other screen with "move output" stays
#    maximized there, keeps the focus, and restores onto that screen
#  - Win+Right on a window snapped to the right half carries it on to the left
#    half of the screen to the right
#  - carrying a window over the edge between the screens does not snap it,
#    the outer edge still does
#  - a screen that goes away hands its windows to the desktop on view on the
#    other screen, still maximized and snapped, and gets them back, where they
#    were, when it returns
#  - show desktop minimizes the windows of every screen
#
# usage: multi_screen.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-screens-$$.sock
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
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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

open_on() {
	ipc focus output "$1" >/dev/null 2>&1
	ipc exec "xfce4-terminal --disable-server -T $2" >/dev/null 2>&1
	attempt=0
	while [ -z "$(window "$2")" ] && [ $attempt -lt 40 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
	done
	sleep 1
	if [ -z "$(window "$2")" ]; then
		echo "window $2 never came up"
		exit 1
	fi
}

open_on HEADLESS-2 wide
ipc "[title=wide] maximize enable" >/dev/null
sleep 0.5
ipc "[title=wide] focus" >/dev/null
ipc move output left >/dev/null
sleep 0.5
set -- $(window wide)
echo "maximized window after moving it left: $*"
[ "$1" = HEADLESS-1 ] || fail "the window did not go to the left screen"
[ "$4" = 1280 ] && [ "$6" = 1 ] || fail "it did not stay maximized on the smaller screen"
[ "$8" = 1 ] || fail "it lost the focus on the way"
ipc "[title=wide] maximize disable" >/dev/null
sleep 0.5
set -- $(window wide)
echo "restored: $*"
if [ "$2" -lt 0 ] || [ $(($2 + $4)) -gt 1280 ]; then
	fail "it restored off the left screen"
fi

ipc "[title=wide] snap right" >/dev/null
ipc "[title=wide] snap right" >/dev/null
sleep 0.5
set -- $(window wide)
echo "snapped right twice: $*"
[ "$1" = HEADLESS-2 ] && [ "$2" = 1280 ] && [ "$4" = 960 ] ||
	fail "Win+Right did not carry the snapped window on to the left half of the next screen"

# carrying it over the edge between the screens and holding it there
ipc "[title=wide] snap restore" >/dev/null
ipc "[title=wide] move position 1500 300" >/dev/null
sleep 0.5
x=$(field wide 2); y=$(field wide 3); w=$(field wide 4)
"$pointer" 3200 1080 drag $((x + w / 2)) $((y - 10)) 1279 500 >/dev/null 2>&1
sleep 0.5
set -- $(window wide)
echo "let go on the edge between the screens: $*"
[ "$6" = 0 ] && [ "$4" != 640 ] && [ "$4" != 960 ] ||
	fail "the edge between the screens snapped the window"
x=$(field wide 2); y=$(field wide 3); w=$(field wide 4)
"$pointer" 3200 1080 drag $((x + w / 2)) $((y - 10)) 0 400 >/dev/null 2>&1
sleep 0.5
set -- $(window wide)
echo "let go on the outer left edge: $*"
[ "$1" = HEADLESS-1 ] && [ "$2" = 0 ] && [ "$4" = 640 ] ||
	fail "the outer edge did not snap the window any more"

# windows on both screens, then the right one goes away and comes back
ipc "[title=wide] move output right" >/dev/null
ipc "[title=wide] maximize enable" >/dev/null
open_on HEADLESS-2 half
ipc "[title=half] snap right" >/dev/null
open_on HEADLESS-1 left
sleep 0.5
wide_before=$(window wide | cut -d' ' -f1-6)
half_before=$(window half | cut -d' ' -f1-6)
ipc output HEADLESS-2 disable >/dev/null
sleep 1
echo "with the right screen gone: $(window wide) / $(window half) / $(window left)"
set -- $(window wide)
[ "$1" = HEADLESS-1 ] && [ "$4" = 1280 ] && [ "$6" = 1 ] ||
	fail "the maximized window did not stay maximized on the remaining screen"
set -- $(window half)
[ "$1" = HEADLESS-1 ] && [ "$2" = 640 ] && [ "$4" = 640 ] ||
	fail "the snapped window did not stay snapped on the remaining screen"
visible=$(ipc -t get_workspaces | python3 -c '
import json, sys
print(sum(1 for w in json.load(sys.stdin) if w["visible"]))')
shown=$(ipc -t get_workspaces | python3 -c '
import json, sys
print([w["name"] for w in json.load(sys.stdin) if w["visible"]][0])')
ws_of() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, ws):
    if n.get("type") == "workspace":
        ws = n["name"]
    if n.get("name") == sys.argv[1] and n.get("pid"):
        print(ws)
        return True
    return any(walk(c, ws) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin), None)' "$1"
}
[ "$(ws_of wide)" = "$shown" ] && [ "$(ws_of half)" = "$shown" ] ||
	fail "the windows of the screen that went away are not on the desktop on view"

ipc output HEADLESS-2 enable >/dev/null
sleep 1.5
wide_after=$(window wide | cut -d' ' -f1-6)
half_after=$(window half | cut -d' ' -f1-6)
echo "back: $wide_after / $half_after / $(window left)"
[ "$wide_after" = "$wide_before" ] || fail "the maximized window did not go back ($wide_before)"
[ "$half_after" = "$half_before" ] || fail "the snapped window did not go back ($half_before)"
[ "$(field left 1)" = HEADLESS-1 ] || fail "the window of the other screen moved as well"

ipc showdesktop >/dev/null
sleep 0.5
minimized=0
for title in wide half left; do
	minimized=$((minimized + $(field $title 7)))
done
[ "$minimized" = 3 ] || fail "show desktop left windows on one of the screens ($minimized of 3 minimized)"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
