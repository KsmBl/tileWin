#!/bin/sh
# Picks a number from a dropdown of the settings app and looks at what it
# wrote: numbers are chosen from lists of sensible ones instead of typed into
# spin buttons, which the lists only drive, so a list that shows the new
# number without the spin button saving it looks perfectly healthy.
#
# usage: settings_numbers.sh <build dir>
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
export TILEWIN_NO_APP_TWEAKS=1 # leaves the real desktop and home alone
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
output * mode --custom 1280x1500
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec "$settings" --page=keyboard
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

# Repeat delay under Typing: open its list (600 ms is chosen), then pick
# "300 ms", which writes "repeat_delay 300" to the input block of common.conf.
WAYLAND_DISPLAY=$display "$tool" 1280 1500 click 1185 596 >/dev/null 2>&1
sleep 1
WAYLAND_DISPLAY=$display "$tool" 1280 1500 click 1180 747 >/dev/null 2>&1
sleep 3
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/common.conf
if grep -q '^[[:space:]]*repeat_delay 300$' "$written" 2>/dev/null; then
	echo "the number was saved"
	exit 0
fi
echo "picking a repeat delay from its list saved nothing."
echo "Either the list no longer sets its spin button, or the Keyboard page has"
echo "been rearranged and the list is no longer at 1185,596."
grep -n "repeat" "$written" 2>/dev/null || echo "(common.conf has no repeat line)"
exit 1
