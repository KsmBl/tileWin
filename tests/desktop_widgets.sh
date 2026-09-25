#!/bin/sh
# Widgets on the desktop. taskbar.conf puts the memory widget and the disk
# space widget on the desktop, and the test checks that
#  - their cards turn up where the config puts them, drawn over the wallpaper;
#  - a card dragged with a pointer of its own moves there and keeps the place
#    it was dragged to in the state file, next to what the config said then;
#  - clicking the disk space card opens its flyout (the widgets that had no
#    flyout of their own got one) and the taskbar survives all of it;
#  - the drag is forgotten once the config gives the card another place.
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
# an empty desktop folder of its own, so no icons sit where the cards go
mkdir -p "$work/Desktop"
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
	memory { x 100; y 100 }
	storage { x 100; y 300 }
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
# the pointer of a new virtual device loses its first motion: move twice first
point() {
	x=$1 y=$2
	shift 2
	"$pointer" 1280 720 move "$x" "$y" move "$((x + 1))" "$y" "$@" >/dev/null 2>&1
}

started=$(panel_pid)
memory_card=$(drawn 100 100 120 60)
empty=$(drawn 700 100 120 60)
echo "memory card: $memory_card pixels drawn, an empty spot: $empty"
[ "$memory_card" -gt 200 ] || fail "the memory widget is not on the desktop"
[ "$empty" -lt 50 ] || fail "something is drawn where the desktop should be empty"

# carry the memory card 500 pixels to the right
point 140 130 drag 140 130 640 130
sleep 1
moved=$(drawn 600 100 120 60)
left=$(drawn 100 100 60 60)
echo "after the drag: $moved pixels at the new place, $left at the old one"
[ "$moved" -gt 200 ] || fail "the dragged card is not where it was dragged to"
[ "$left" -lt 50 ] || fail "the dragged card is still at its old place"
state=$XDG_STATE_HOME/tileWin/desktop-widgets
if ! grep -q "^600 100 100 100 memory " "$state" 2>/dev/null; then
	fail "the place it was dragged to was not kept: $(cat "$state" 2>/dev/null)"
fi

# the disk space card opens a flyout with the drives
before=$(drawn 100 420 360 250)
point 140 330 click 140 330
sleep 2
after=$(drawn 100 420 360 250)
echo "below the disk space card: $before pixels before the click, $after after"
[ "$after" -gt $((before + 2000)) ] || fail "clicking the disk space card opened no flyout"
"$build/tests/keyboard-tool" key none escape >/dev/null 2>&1
sleep 0.5

# a new place in the config wins over the old drag
sed -i 's/memory { x 100; y 100 }/memory { x 100; y 500 }/' "$conf"
sleep 2
config_place=$(drawn 100 500 120 60)
drag_place=$(drawn 600 100 120 60)
echo "after the config moved it: $config_place pixels at its new place, $drag_place where it was dragged"
[ "$config_place" -gt 200 ] && [ "$drag_place" -lt 50 ] ||
	fail "the config did not win over the old drag"

[ "$(panel_pid)" = "$started" ] || fail "the taskbar died and was started again"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
