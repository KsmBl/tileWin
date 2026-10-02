#!/bin/sh
# A part of a layout picked with the animations on: the window goes there on
# the first try, not to the plain half, and the window beside it takes the
# rest. A window that was not snapped yet (side by side by "arrange",
# maximized, floating) used to set out with its resize animation for the half
# before the layout's line was known, and land there; only a second try put
# it in its part. Both kinds of animation: a picture of the window stretched,
# and the window resized on every frame ("expensive_calculations yes"); and
# once by a click in Snap Layouts, as a person does it.
#
# usage: snap_animated.sh <build dir> <source dir>
set -u

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
sock=$XDG_RUNTIME_DIR/tw-snapanim-$$.sock
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
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations on
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
open_window() {
	ipc exec "xfce4-terminal --disable-server -T $1" >/dev/null 2>&1
	attempt=0
	while [ -z "$(window "$1")" ] && [ $attempt -lt 40 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
	done
	sleep 1
	[ -n "$(window "$1")" ] || { echo "window $1 never came up"; exit 1; }
}
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

ipc theme win10 >/dev/null
sleep 2
open_window one
open_window two
title=$(ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "one":
			print(c["deco_rect"]["height"])
		walk(c)
walk(json.load(sys.stdin))' | head -n 1)

# where the windows start: snapped as halves, side by side by "arrange", the
# left one maximized or floating; then a part of a layout for the left one
start() {
	ipc '[title=one] snap restore' >/dev/null
	ipc '[title=two] snap restore' >/dev/null
	ipc '[title=one] maximize disable' >/dev/null
	sleep 1
	case $1 in
	halves)
		ipc '[title=one] snap left' >/dev/null
		sleep 1
		ipc '[title=two] snap right' >/dev/null ;;
	arranged)
		ipc arrange horizontal >/dev/null ;;
	maximized)
		ipc '[title=two] snap right' >/dev/null
		sleep 1
		ipc '[title=one] maximize enable' >/dev/null ;;
	floating)
		ipc '[title=two] snap right' >/dev/null
		sleep 1
		ipc '[title=one] move position 50 50, resize set 500 400' >/dev/null ;;
	esac
	sleep 1.5
}

for expensive in no yes; do
	ipc expensive_calculations $expensive >/dev/null
	for from in halves arranged maximized floating; do
		start $from
		set -- $(window one)
		left=$1
		ipc '[title=one] snap left 0.34 0.50' >/dev/null
		sleep 1.5
		set -- $(window one)
		[ "$1 $3" = "0 435" ] ||
			fail "expensive $expensive, from $from: a third did not go to the left window at once: $*"
		set -- $(window two)
		[ "$1 $3" = "435 845" ] ||
			fail "expensive $expensive, from $from: the right window did not get the two thirds left: $*"
		ipc '[title=one] snap left 0.66 0.50' >/dev/null
		sleep 1.5
		set -- $(window one)
		[ "$1 $3" = "0 844" ] ||
			fail "expensive $expensive, from $from: two thirds did not go to the left window at once: $*"
		set -- $(window two)
		[ "$1 $3" = "844 436" ] ||
			fail "expensive $expensive, from $from: the right window did not get the third left: $*"
	done
done

# and as a person does it: from a floating window, a click on the big part of
# the second layout (two thirds on the left) in its Snap Layouts
start floating
ipc '[title=one] move position 100 100, resize set 700 400' >/dev/null
sleep 1.5
set -- $(window one)
bx=$(($1 + $3 - 69)) by=$(($2 - 16))
lx=$(($1 + $3 - 232 + 74)) ly=$(($2 + 36))
"$pointer" 1280 720 move $((bx - 4)) "$by" wait 200 move "$bx" $((by + 1)) wait 900 \
	move $((lx + 30)) $((ly - 20)) wait 100 move "$lx" "$ly" wait 300 click "$lx" "$ly" >/dev/null 2>&1
sleep 1.5
set -- $(window one)
[ "$1 $3" = "0 844" ] || fail "one click on two thirds in Snap Layouts did not give two thirds: $*"
set -- $(window two)
[ "$1 $3" = "844 436" ] || fail "after one click the right window did not get the third left: $*"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
