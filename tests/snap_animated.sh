#!/bin/sh
# A part of a layout picked with the animations on: the window goes there on
# the first try, not to the plain half, and the window beside it takes the
# rest. A window that was not snapped yet (side by side by "arrange",
# maximized, floating) used to set out with its resize animation for the half
# before the layout's line was known, and land there; only a second try put
# it in its part. Both kinds of animation: a picture of the window stretched,
# and the window resized on every frame ("expensive_calculations yes"); and
# once by a click in Snap Layouts, as a person does it. Given a kind (no or
# yes), only that one, the click with yes, so that both can run side by side.
#
# usage: snap_animated.sh <build dir> <source dir> [no|yes]
set -u
. "$(dirname "$0")/wait.sh"

build=$1
source_dir=$2
kinds=${3:-no yes}
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

if ! wait_for_file 12 "$work/display"; then
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
	wait_until 20 window_up "$1" || { echo "window $1 never came up"; exit 1; }
	wait_still 5 both
}
window_up() {
	[ -n "$(window "$1")" ]
}
# where both windows are, one after the other
both() {
	window one
	window two
}
# After a command that moves the windows: until they set out (a command may
# leave them where they are, so not for long) and then are still.
moved_from() {
	[ "$(both)" != "$1" ]
}
settle() { # <where both were before the command>
	wait_until 2 moved_from "$1"
	sleep 0.5 # the longest animation is over, as the next command finds them
	wait_still 5 both
}
step() { # <command>: runs it and waits for the windows to settle
	before=$(both)
	ipc "$1" >/dev/null
	settle "$before"
}
# Until the left window has its x and width, and the right one its, and after
# the longest animation still has them; the checks below then look again.
placed() { # <x width of one> <x width of two>
	set -- "$1" "$2" "$(window one)" "$(window two)"
	[ "$(echo "$3" | cut -d' ' -f1,3)" = "$1" ] && [ "$(echo "$4" | cut -d' ' -f1,3)" = "$2" ]
}
land() {
	wait_until 5 placed "$1" "$2"
	sleep 0.5
	wait_still 5 both
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
wait_until 5 grep -qx win10 "$XDG_CONFIG_HOME/tileWin/current-theme"
wait_still 5 sh -c 'grim -t ppm - | cksum'
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
	before=$(both)
	ipc '[title=one] snap restore' >/dev/null
	ipc '[title=two] snap restore' >/dev/null
	ipc '[title=one] maximize disable' >/dev/null
	settle "$before"
	case $1 in
	halves)
		step '[title=one] snap left'
		step '[title=two] snap right' ;;
	arranged)
		step 'arrange horizontal' ;;
	maximized)
		step '[title=two] snap right'
		step '[title=one] maximize enable' ;;
	floating)
		step '[title=two] snap right'
		step '[title=one] move position 50 50, resize set 500 400' ;;
	esac
}

for expensive in $kinds; do
	ipc expensive_calculations $expensive >/dev/null
	for from in halves arranged maximized floating; do
		start $from
		set -- $(window one)
		left=$1
		ipc '[title=one] snap left 0.34 0.50' >/dev/null
		land "0 435" "435 845"
		set -- $(window one)
		[ "$1 $3" = "0 435" ] ||
			fail "expensive $expensive, from $from: a third did not go to the left window at once: $*"
		set -- $(window two)
		[ "$1 $3" = "435 845" ] ||
			fail "expensive $expensive, from $from: the right window did not get the two thirds left: $*"
		ipc '[title=one] snap left 0.66 0.50' >/dev/null
		land "0 844" "844 436"
		set -- $(window one)
		[ "$1 $3" = "0 844" ] ||
			fail "expensive $expensive, from $from: two thirds did not go to the left window at once: $*"
		set -- $(window two)
		[ "$1 $3" = "844 436" ] ||
			fail "expensive $expensive, from $from: the right window did not get the third left: $*"
	done
done

if [ "$expensive" = yes ]; then
	# and as a person does it: from a floating window, a click on the big part of
	# the second layout (two thirds on the left) in its Snap Layouts
	start floating
	step '[title=one] move position 100 100, resize set 700 400'
	# and drawn there, its buttons where the pointer goes
	wait_still 5 sh -c 'grim -t ppm - | cksum'
	set -- $(window one)
	bx=$(($1 + $3 - 69)) by=$(($2 - 16))
	lx=$(($1 + $3 - 232 + 74)) ly=$(($2 + 36))
	"$pointer" 1280 720 move $((bx - 4)) "$by" wait 200 move "$bx" $((by + 1)) wait 900 \
		move $((lx + 30)) $((ly - 20)) wait 100 move "$lx" "$ly" wait 300 click "$lx" "$ly" >/dev/null 2>&1
	land "0 844" "844 436"
	set -- $(window one)
	[ "$1 $3" = "0 844" ] || fail "one click on two thirds in Snap Layouts did not give two thirds: $*"
	set -- $(window two)
	[ "$1 $3" = "844 436" ] || fail "after one click the right window did not get the third left: $*"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
