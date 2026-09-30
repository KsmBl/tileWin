#!/bin/sh
# Snapped windows side by side resize together, as on Windows, in a nested
# tileWin (1280x720, no taskbar):
#  - dragging the line between a left and a right half, from either side of
#    it, resizes both, and both stay snapped;
#  - a window snapped next to a half made narrower takes the rest of the width;
#  - the line between two quarters of a column moves both quarters and leaves
#    the half beside them alone;
#  - the line between the column and the half moves all three;
#  - dragging an outer side still unsnaps the window and resizes only it.
#
# usage: snap_resize.sh <build dir> <source dir>
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
for needed in dbus-run-session xfce4-terminal; do
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
sock=$XDG_RUNTIME_DIR/tw-snapsize-$$.sock
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
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command true
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
drag() {
	"$pointer" 1280 720 move "$1" "$2" wait 300 drag "$1" "$2" "$3" "$4" >/dev/null 2>&1
	sleep 0.8
}

open_window one
open_window two
ipc "[title=one] snap left" >/dev/null
ipc "[title=two] snap right" >/dev/null
sleep 0.8
# the title bar is above the content (the rect of a window)
title=$(window one | cut -d' ' -f2)
h=$((720 - title))
[ "$(window one)" = "0 $title 640 $h" ] && [ "$(window two)" = "640 $title 640 $h" ] ||
	{ echo "the halves are not where expected: $(window one) / $(window two)"; exit 1; }

# the line between the halves, grabbed a little left of it
drag 636 300 836 300
[ "$(window one)" = "0 $title 840 $h" ] || fail "the left half after moving the line right: $(window one)"
[ "$(window two)" = "840 $title 440 $h" ] || fail "the right half after moving the line right: $(window two)"
# and from the right half
drag 844 400 444 400
[ "$(window one)" = "0 $title 440 $h" ] || fail "the left half after moving the line left: $(window one)"
[ "$(window two)" = "440 $title 840 $h" ] || fail "the right half after moving the line left: $(window two)"

# snapped again next to it: the rest of the width
ipc "[title=two] snap restore" >/dev/null
sleep 0.3
ipc "[title=two] snap right" >/dev/null
sleep 0.5
[ "$(window two)" = "440 $title 840 $h" ] || fail "snapped next to a narrower half: $(window two)"

# two quarters on the left: the line between them
open_window three
ipc "[title=one] snap topleft" >/dev/null
ipc "[title=three] snap bottomleft" >/dev/null
sleep 0.5
set -- $(window one)
line=$(($2 + $4))
drag 150 $((line + 1)) 150 $((line + 101))
set -- $(window one)
bottom=$(($2 + $4))
[ $bottom -ge $((line + 99)) ] && [ $bottom -le $((line + 101)) ] ||
	fail "the upper quarter did not follow the line: $*"
set -- $(window three)
[ "$1" = 0 ] && [ $(($2 + $4)) = 720 ] && [ "$3" = 440 ] ||
	fail "the lower quarter does not fill the rest of its column: $*"
[ "$2" -gt $((line + 100)) ] && [ "$2" -lt $((line + 140)) ] ||
	fail "the lower quarter did not follow the line: $*"
[ "$(window two)" = "440 $title 840 $h" ] || fail "the half beside the quarters moved: $(window two)"

# the line between the column and the half: all three
drag 440 200 740 200
set -- $(window one)
[ "$3" = 740 ] || fail "the upper quarter after moving the column line: $*"
set -- $(window three)
[ "$3" = 740 ] || fail "the lower quarter after moving the column line: $*"
[ "$(window two)" = "740 $title 540 $h" ] || fail "the half after moving the column line: $(window two)"

# an outer side unsnaps the window and resizes it alone
ipc "[title=three] snap restore" >/dev/null
ipc "[title=one] snap left" >/dev/null
sleep 0.5
drag 0 300 100 300
set -- $(window one)
[ "$1" -ge 95 ] || fail "dragging the outer side did not resize the window: $*"
[ "$(window two)" = "740 $title 540 $h" ] || fail "dragging an outer side moved the other half: $(window two)"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
