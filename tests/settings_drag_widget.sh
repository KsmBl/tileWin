#!/bin/sh
# Drags a widget of the Taskbar page by its handle and reads back the order
# taskbar.conf was given. A handle that can be picked up but moves nothing
# looks fine in a screenshot, so the file is what tells.
#
# usage: settings_drag_widget.sh <build dir>
set -u
. "$(dirname "$0")/wait.sh"

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
tool=$build/tests/pointer-tool
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$settings" "$tool" "$msg"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if ! command -v grim >/dev/null 2>&1; then
	echo "grim is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
cleanup() {
	pkill -f "^[^ ]*/tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# a session of its own, so the test neither reads nor writes the real config
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
export TILEWIN_NO_APP_TWEAKS=1 # leaves the real desktop and home alone
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

# a layout of its own, so where the rows are does not depend on any theme
cat > "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
theme_layout no
layout window {
	position bottom
	left start search taskbar
	right tray clock
}
EOF

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec "$settings" --page=taskbar
EOF

env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	SWAYSOCK="$work/tilewin.sock" TILEWINSOCK="$work/tilewin.sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
starter=$!

if ! wait_for_file 20 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 2 "$work/log"
	exit 1
fi
display=$(cat "$work/display")
# the settings app has to be up and laid out before it is clicked: its window
# is there, and the screen has stopped changing
mapped() { "$msg" -s "$work/tilewin.sock" -t get_tree | grep -Eq '"app_id": ?"org.tilewin.Settings"'; }
screen() { WAYLAND_DISPLAY=$display grim -t ppm - | md5sum; }
if ! wait_until 60 mapped; then
	echo "the settings app never opened"
	tail -n 5 "$work/log"
	exit 1
fi
wait_still 30 screen

# maximize the window, click the page so the wheel reaches it, and scroll down
# to the Left section: its rows are then
# start at y 434, search at 491 and taskbar at 548, with the handles at x 283
WAYLAND_DISPLAY=$display "$tool" 1280 720 click 1008 15 click 690 100 scroll 690 400 7 >/dev/null 2>&1
wait_still 30 screen
# the start button, dropped onto the lower half of the window buttons
WAYLAND_DISPLAY=$display "$tool" 1280 720 drag 283 434 400 560 >/dev/null 2>&1
saved() { grep -q "^[[:space:]]*left search taskbar start$" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"; }
wait_until 10 saved
kill "$starter" 2>/dev/null
pkill -f "^[^ ]*/tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/taskbar.conf
if grep -q "^[[:space:]]*left search taskbar start$" "$written"; then
	echo "the dragged widget was saved at its new place"
	exit 0
fi
echo "dragging the start button below the window buttons did not save that order."
echo "Either dragging no longer moves a widget, or the Taskbar page has been"
echo "rearranged and the rows are no longer where this test looks for them."
grep -n "left" "$written" || echo "(taskbar.conf has no left section)"
exit 1
