#!/bin/sh
# Clicks a switch of the settings app and looks at what it wrote. A switch that
# moves on screen without saving anything looks perfectly healthy, so the only
# way to tell is to work it and read the file back.
#
# usage: settings_switches.sh <build dir>
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

# "Let the theme bring its own layout", the one switch of the Taskbar page that
# is in view without scrolling. Every switch of that page and of the Mouse page
# is made by the same helper, so one of them standing in for the rest is enough
# to catch a helper that has stopped saving.
WAYLAND_DISPLAY=$display "$tool" 1280 720 click 1005 595 >/dev/null 2>&1
sleep 2
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/taskbar.conf
if [ ! -f "$written" ]; then
	echo "the settings app wrote no taskbar.conf at all"
	exit 1
fi
if grep -q "^theme_layout no" "$written"; then
	echo "the switch saved what it was set to"
	exit 0
fi
echo "clicking the switch saved nothing."
echo "Either a switch no longer writes its setting, or the Taskbar page has"
echo "been rearranged and the switch is no longer at 1005,595."
grep -n "theme_layout" "$written" || echo "(taskbar.conf has no theme_layout line)"
exit 1
