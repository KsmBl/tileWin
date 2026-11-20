#!/bin/bash
# A window focused while another is full screen (Alt+Tab, the taskbar) comes
# over it, as on Windows, instead of the focus being refused or the full screen
# ended; the full screen window stays so behind it and comes back on top when
# it is clicked.
#
# usage: fullscreen_focus.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-fsfocus-$$.sock
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

# the title bar of window two shows: light pixels where it is, as the theme draws
# it, instead of the black terminal of the full screen window
two_shows() {
	set -- $(window two)
	[ $# -eq 4 ] || return 1
	grim -g "$(($1 + 40)),$(($2 - 20)) 100x10" "$work/bar.png" 2>/dev/null || return 1
	python3 -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
sys.exit(0 if sum(im.getdata()) / (im.width * im.height) > 120 else 1)" "$work/bar.png"
}
# name fullscreen_mode focused, of a window
state() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == sys.argv[1] and c.get("pid"):
			print(c.get("fullscreen_mode"), c.get("focused"))
		walk(c)
walk(json.load(sys.stdin))' "$1"
}

if ! python3 -c 'import PIL' 2>/dev/null; then
	echo "python3 has no PIL: skipping"
	exit 77
fi
open_window one
open_window two
ipc '[title="^two$"] move position 300 200, resize set 500 300' >/dev/null
ipc '[title="^one$"] focus, fullscreen enable' >/dev/null
wait_until 5 sh -c '! "$0"' two_shows 2>/dev/null; sleep 0.5
two_shows && fail "a window shows over the full screen one before it is focused"

# Alt+Tab to the other window: it comes over the full screen one, which stays so
ipc alttab next >/dev/null
ipc alttab commit >/dev/null
sleep 1
[ "$(state two)" = "0 True" ] || fail "Alt+Tab did not focus the window behind the full screen one: $(state two)"
[ "$(state one)" = "1 False" ] || fail "Alt+Tab ended the full screen of the other window: $(state one)"
two_shows || fail "the window focused with Alt+Tab is not shown over the full screen one"

# a click on the full screen window puts it on top again
"$pointer" 1280 720 click 100 650 >/dev/null 2>&1
sleep 1
[ "$(state one)" = "1 True" ] || fail "a click did not focus the full screen window: $(state one)"
two_shows && fail "the window stayed over the full screen one after that was clicked"

# focused from the taskbar (a focus command): over it as well, full screen kept
ipc '[title="^two$"] focus' >/dev/null
sleep 1
[ "$(state one)" = "1 False" ] || fail "focusing another window ended the full screen: $(state one)"
two_shows || fail "the window focused from the taskbar is not shown over the full screen one"

exit $((failures > 0))
