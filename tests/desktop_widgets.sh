#!/bin/sh
# Widgets on the desktop, in the cells of the icon grid. taskbar.conf puts an
# analog clock, a memory widget, a disk space ring and a CPU chart on the
# desktop, and the test checks that
#  - each card takes the cells the config gives it, and one without a place
#    goes to the upper right corner;
#  - the desktop icons make room: the icon that would sit in the first cell
#    moves below the clock that covers it;
#  - a card dragged with a pointer of its own lands on whole cells and keeps
#    them in the state file, while one dropped onto another card goes back;
#  - clicking the disk space ring opens its flyout, and the taskbar survives;
#  - a new place or style in the config wins over the old drag.
#
# The grid is the default one: cells of 100 pixels and a margin of 10, so the
# card of column c and row r starts at 14 + 100 c, 14 + 100 r, with 12 columns
# and 6 rows above the taskbar of the default theme.
#
# usage: desktop_widgets.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
panel=$build/panel/tilewin-panel
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool
count=$build/tests/count-content

for needed in "$compositor" "$panel" "$msg" "$pointer" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session; do
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
sock=$XDG_RUNTIME_DIR/tw-desk-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
# a desktop folder of its own with one file, whose icon would take the first cell
mkdir -p "$work/Desktop"
touch "$work/Desktop/notes.txt"
printf 'XDG_DESKTOP_DIR="%s/Desktop"\n' "$work" > "$XDG_CONFIG_HOME/user-dirs.dirs"

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

conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
cp "$source_dir/config/taskbar.conf" "$conf"
cat >> "$conf" <<EOF

desktop_widgets {
	clock { style analog; column 0; row 0 }
	memory { column 3; row 0 }
	storage { style ring; column 3; row 2 }
	cpu { style chart }
}
EOF

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command $panel
animations off
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
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
sleep 4

panel_pid() {
	ipc -t get_tilewin | sed -n 's/.*"panel_pid": *\([0-9]*\).*/\1/p'
}
drawn() {
	grim "$work/shot.png" 2>/dev/null
	"$count" "$work/shot.png" "$1" "$2" "$3" "$4"
}
# the pixels drawn in columns x rows cells from column $1, row $2
cell() {
	drawn $((14 + 100 * $1)) $((14 + 100 * $2)) $((100 * $3 - 8)) $((100 * $4 - 8))
}
# the pointer of a new virtual device loses its first motion: move twice first
point() {
	x=$1 y=$2
	shift 2
	"$pointer" 1280 720 move "$x" "$y" move "$((x + 1))" "$y" "$@" >/dev/null 2>&1
}
state=$XDG_STATE_HOME/tileWin/desktop-widget-cells

started=$(panel_pid)
clock=$(cell 0 0 2 2)
memory=$(cell 3 0 1 1)
storage=$(cell 3 2 2 2)
chart=$(cell 9 0 3 2)
empty=$(cell 6 3 2 2)
echo "clock $clock, memory $memory, disk space $storage, chart $chart, empty cells $empty"
[ "$clock" -gt 300 ] || fail "the analog clock is not in column 0, row 0"
[ "$memory" -gt 200 ] || fail "the memory widget is not in column 3, row 0"
[ "$storage" -gt 300 ] || fail "the disk space ring is not in column 3, row 2"
[ "$chart" -gt 300 ] || fail "the CPU chart without a place is not in the upper right corner"
[ "$empty" -lt 50 ] || fail "something is drawn in cells no widget has"

# the clock covers the first cells, so the icon flows to the next free one
icon=$(cell 0 2 1 1)
echo "the icon below the clock: $icon pixels"
[ "$icon" -gt 200 ] || fail "the icon did not make room for the clock"

# carry the memory card three cells to the right, a little off the grid
point 360 60 drag 360 60 668 72
sleep 1
moved=$(cell 6 0 1 1)
left=$(cell 3 0 1 1)
echo "after the drag: $moved pixels in column 6, $left in column 3"
[ "$moved" -gt 200 ] || fail "the dragged card did not land on the cells it was dropped on"
[ "$left" -lt 50 ] || fail "the dragged card is still in its old cells"
if ! grep -q "^6 0 3,0,, memory " "$state" 2>/dev/null; then
	fail "the cells it was dragged to were not kept: $(cat "$state" 2>/dev/null)"
fi

# dropped onto the chart, the disk space ring goes back where it was
point 410 310 drag 410 310 1010 110
sleep 1
back=$(cell 3 2 2 2)
echo "the ring dropped onto the chart: $back pixels at its old place"
[ "$back" -gt 300 ] || fail "a card dropped onto another one did not go back"
if grep -q " storage " "$state" 2>/dev/null; then
	fail "a drop onto another card was kept: $(cat "$state")"
fi

# the disk space ring opens a flyout with the drives
before=$(drawn 0 0 1280 660)
point 410 310 click 410 310
sleep 2
after=$(drawn 0 0 1280 660)
echo "the whole screen: $before pixels before the click, $after after"
[ "$after" -gt $((before + 2000)) ] || fail "clicking the disk space ring opened no flyout"
"$build/tests/keyboard-tool" key none escape >/dev/null 2>&1
sleep 0.5

# a new place in the config wins over the old drag, a new style brings its size
sed -i 's/memory { column 3; row 0 }/memory { column 3; row 5 }/' "$conf"
sed -i 's/cpu { style chart }/cpu { style ring }/' "$conf"
sleep 2
config_place=$(cell 3 5 1 1)
drag_place=$(cell 6 0 1 1)
ring=$(cell 10 0 2 2)
chart_left=$(cell 9 0 1 2)
echo "memory: $config_place pixels at its new place, $drag_place where it was dragged;" \
	"cpu ring $ring, left of it $chart_left"
[ "$config_place" -gt 200 ] && [ "$drag_place" -lt 50 ] ||
	fail "the config did not win over the old drag"
[ "$ring" -gt 300 ] && [ "$chart_left" -lt 50 ] ||
	fail "the CPU widget did not take the two cells of its new style"

[ "$(panel_pid)" = "$started" ] || fail "the taskbar died and was started again"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
