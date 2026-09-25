#!/bin/sh
# Drags a widget of the Taskbar page by its handle and reads back the order
# taskbar.conf was given. A handle that can be picked up but moves nothing
# looks fine in a screenshot, so the file is what tells.
#
# usage: settings_drag_widget.sh <build dir>
set -u

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
tool=$build/tests/pointer-tool

for needed in "$compositor" "$settings" "$tool"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
cleanup() {
	pkill -f "tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# a session of its own, so the test neither reads nor writes the real config
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
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
exec "$settings" --page=taskbar
EOF

before=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	SWAYSOCK="$work/tilewin.sock" TILEWINSOCK="$work/tilewin.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
starter=$!

display=
attempt=0
while [ -z "$display" ] && [ $attempt -lt 40 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
	after=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
	display=$(echo "$after" | grep -vxF "$before" | head -n 1)
done
if [ -z "$display" ]; then
	echo "the nested compositor never came up"
	tail -n 2 "$work/log"
	exit 1
fi
sleep 6 # the settings app has to be up and laid out before it is clicked

# maximize the window and scroll down to the Left section: its rows are then
# start at y 506, search at 563 and taskbar at 620, with the handles at x 283
WAYLAND_DISPLAY=$display "$tool" 1280 720 click 1008 15 scroll 690 400 10 >/dev/null 2>&1
sleep 1
# the start button, dropped onto the lower half of the window buttons
WAYLAND_DISPLAY=$display "$tool" 1280 720 drag 283 506 400 632 >/dev/null 2>&1
sleep 2
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

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
