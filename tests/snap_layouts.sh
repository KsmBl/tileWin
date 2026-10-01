#!/bin/sh
# Snap Layouts, in a nested tileWin with the taskbar (Windows 10 theme):
#  - resting the pointer on a window's maximize button opens the layouts;
#  - a click on the left part of the halves snaps the window to the left half;
#  - snap assist then offers the other window for the right half, and picking
#    it snaps it there;
#  - "snap left 0.66" gives a window two thirds of the screen;
#  - "snap_layouts disable" turns the layouts off.
#
# usage: snap_layouts.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-layouts-$$.sock
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
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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
place_one() {
	ipc '[title=one] snap restore' >/dev/null
	ipc '[title=one] move position 100 100' >/dev/null
	ipc '[title=one] resize set 700 400' >/dev/null
	ipc '[title=one] focus' >/dev/null
	sleep 1
}
place_one
# the height of the title bar, and the screen without the taskbar below the titles
title=$(ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "one":
			print(c["deco_rect"]["height"])
		walk(c)
walk(json.load(sys.stdin))' | head -n 1)
h=$((680 - title))
set -- $(window one)
# the maximize button (Windows 10: 46 wide, the title bar 31 high above the
# content) and the left part of the first layout below it
bx=$(($1 + $3 - 69)) by=$(($2 - 16))
lx=$(($1 + $3 - 232)) ly=$(($2 + 36))
under=$(mean $((lx - 10)) $((ly - 10)) 60 20)
"$pointer" 1280 720 move $((bx - 4)) "$by" wait 200 move "$bx" $((by + 1)) wait 1300 >/dev/null 2>&1 &
held=$!
sleep 1.2
over=$(mean $((lx - 10)) $((ly - 10)) 60 20)
wait "$held"
[ "$over" != "$under" ] || fail "resting on the maximize button shows no layouts"

"$pointer" 1280 720 move $((bx - 4)) "$by" wait 200 move "$bx" $((by + 1)) wait 900 \
	move $((lx + 60)) $((ly - 20)) wait 100 move "$lx" "$ly" wait 300 click "$lx" "$ly" >/dev/null 2>&1
sleep 1.5
[ "$(window one)" = "0 $title 640 $h" ] || fail "the left part did not snap the window there: $(window one)"
# snap assist: "two" in the menu over the right half
"$pointer" 1280 720 move 900 297 wait 300 click 900 297 >/dev/null 2>&1
sleep 1
[ "$(window two)" = "640 $title 640 $h" ] || fail "snap assist did not put the other window into the right half: $(window two)"

ipc '[title=one] snap left 0.66' >/dev/null
sleep 0.5
set -- $(window one)
[ "$3" = 844 ] || fail "snap left 0.66 does not give two thirds: $*"

ipc snap_layouts disable >/dev/null
place_one
set -- $(window one)
bx=$(($1 + $3 - 69)) by=$(($2 - 16))
lx=$(($1 + $3 - 232)) ly=$(($2 + 36))
under=$(mean $((lx - 10)) $((ly - 10)) 60 20)
"$pointer" 1280 720 move $((bx - 4)) "$by" wait 200 move "$bx" $((by + 1)) wait 1300 >/dev/null 2>&1 &
held=$!
sleep 1.2
over=$(mean $((lx - 10)) $((ly - 10)) 60 20)
wait "$held"
[ "$over" = "$under" ] || fail "snap_layouts disable still shows the layouts"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
