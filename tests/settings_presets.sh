#!/bin/sh
# Picks a choice from a dropdown of the settings app and looks at what it wrote:
# the settings are chosen from lists instead of typed in, and a list that shows
# the new choice without saving it looks perfectly healthy. The Launcher page is
# opened from the command line, so the sidebar has to have opened its folder
# for the page to be there at all.
#
# usage: settings_presets.sh <build dir>
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
exec "$settings" --page=launcher
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

# Screenshot under Default programs: open its list, then pick the second
# choice, "A rectangle (tileWin)", which writes "set $screenshot tilewin-snip
# area" to common.conf a second later.
WAYLAND_DISPLAY=$display "$tool" 1280 1500 click 1142 932 >/dev/null 2>&1
sleep 1
WAYLAND_DISPLAY=$display "$tool" 1280 1500 click 1139 1015 >/dev/null 2>&1
sleep 3
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/common.conf
if grep -q '^set \$screenshot tilewin-snip area$' "$written" 2>/dev/null; then
	echo "the choice was saved"
	exit 0
fi
echo "picking a screenshot tool from its list saved nothing."
echo "Either the list no longer writes its setting, or the Launcher page has"
echo "been rearranged and the list is no longer at 1142,932."
grep -n "screenshot" "$written" 2>/dev/null || echo "(common.conf has no \$screenshot line)"
exit 1
