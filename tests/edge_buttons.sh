#!/bin/sh
# The title bar buttons of a window against the top of the screen reach up
# to the edge, and against the right edge too the last one into the corner,
# so the pointer pushed there is on them (Fitts's law, as on Windows):
#  - snapped to the right half: the top edge over maximize maximizes it;
#  - maximized, or in the top right quarter: the corner closes it;
#  - snapped to the left half: the top edge over close closes it.
# Both in Windows 95, whose buttons sit inside the frame, and in Windows 10.
#
# usage: edge_buttons.sh <build dir> <source dir>
set -u
. "$(dirname "$0")/wait.sh"

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-edgebuttons-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
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
ipc() { "$msg" -s "$sock" "$@"; }

# the content of a window: x y width height
window() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == sys.argv[1] and c.get("pid"):
			r = c["rect"]
			print(r["x"], r["y"], r["width"], r["height"])
		walk(c)
walk(json.load(sys.stdin))' "$1"
}
# a checksum of the screen: still once everything is drawn where it goes
screen() { grim -t ppm - 2>/dev/null | md5sum; }
there() { [ -n "$(window "$1")" ]; }
open_window() {
	ipc exec "xfce4-terminal --disable-server -T $1" >/dev/null 2>&1
	wait_until 20 there "$1" || { echo "window $1 never came up"; exit 1; }
	wait_still 10 screen
}
# the window is where a snap puts it: right, topright or left
placed() {
	set -- "$1" $(window "$2")
	[ $# = 5 ] || return 1
	case $1 in
	right) [ "$2" -ge 640 ] ;;
	topright) [ "$2" -ge 640 ] && [ $(($3 + $5)) -le 360 ] ;;
	left) [ $(($2 + $4)) -le 640 ] ;;
	esac
}
snap() {
	ipc "[title=^$1$] snap $2" >/dev/null
	wait_until 10 placed "$2" "$1"
	wait_still 10 screen
}
maximized() { set -- $(window "$1"); [ "${3:-0}" = 1280 ]; }
in_theme() { ipc -t get_tilewin | grep -q "\"theme\": *\"$1\""; }
# the mean brightness of a strip of the screen, to tell whether the popup is up
mean() {
	grim -g "$1,$2 $3x$4" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
	while data[pos:pos + 1].isspace():
		pos += 1
	start = pos
	while not data[pos:pos + 1].isspace():
		pos += 1
	fields.append(data[start:pos])
pixels = data[pos + 1:]
print(sum(pixels) // max(1, len(pixels)))'
}

gone() { [ -z "$(window "$1")" ]; }
for theme in win95 win10; do
	ipc theme $theme >/dev/null
	wait_until 10 in_theme $theme
	wait_still 10 screen
	# the maximize button of a window in the right half, from the top edge
	case $theme in win95) max_x=1248 ;; win10) max_x=1210 ;; esac
	open_window r$theme
	snap r$theme right
	"$pointer" 1280 720 click $max_x 0 >/dev/null 2>&1
	wait_until 5 maximized r$theme
	wait_still 10 screen
	set -- $(window r$theme)
	[ "${3:-0}" = 1280 ] || fail "$theme: the top edge over maximize did not maximize the window: $*"
	"$pointer" 1280 720 click 1279 0 >/dev/null 2>&1
	wait_until 5 gone r$theme
	gone r$theme || fail "$theme: the corner did not close the maximized window"

	open_window q$theme
	snap q$theme topright
	"$pointer" 1280 720 click 1279 0 >/dev/null 2>&1
	wait_until 5 gone q$theme
	gone q$theme || fail "$theme: the corner did not close the window in the top right quarter"

	# close of a window in the left half, from the top edge
	case $theme in win95) close_x=626 ;; win10) close_x=617 ;; esac
	open_window l$theme
	snap l$theme left
	"$pointer" 1280 720 click $close_x 0 >/dev/null 2>&1
	wait_until 5 gone l$theme
	gone l$theme || fail "$theme: the top edge over close did not close the left window"
done

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
