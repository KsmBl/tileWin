#!/bin/sh
# The taskbar and the desktop on two screens, in a nested tileWin:
#  - a selection rectangle dragged on the desktop of the screen without the
#    icons is drawn on that screen, not on the main one;
#  - "outputs main" in taskbar.conf gives only the main display a taskbar,
#    and the default gives every screen one;
#  - "panel identify" shows the number of each screen on it;
#  - the Super key ("panel startmenu toggle") opens the Start menu on the main
#    display, also while another screen has the focus.
#
# usage: multi_screen_panel.sh <build dir> <source dir>
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
for needed in dbus-run-session grim; do
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
sock=$XDG_RUNTIME_DIR/tw-panels-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/Desktop"
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
trap cleanup EXIT
trap 'exit 1' INT TERM

cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
main_output HEADLESS-1
output HEADLESS-1 mode --custom 1280x720 pos 0 0
output HEADLESS-2 mode --custom 1280x720 pos 1280 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman \
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
pixel() { grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'; }
wall="32 64 112"
sleep 3

# a taskbar on each screen, at the bottom
[ "$(pixel 600 710)" != "$wall" ] || fail "the main display has no taskbar"
[ "$(pixel 1880 710)" != "$wall" ] || fail "the other screen has no taskbar"

# the selection rectangle, on the screen without icons
"$pointer" 2560 720 down 1600 150 move 1700 250 move 1900 400 wait 2000 up >/dev/null 2>&1 &
drag=$!
sleep 1.4
[ "$(pixel 1750 300)" != "$wall" ] || fail "the selection rectangle is not on the screen dragged on"
[ "$(pixel 470 300)" = "$wall" ] || fail "the selection rectangle is drawn on the main display"
wait "$drag"
sleep 0.3
[ "$(pixel 1750 300)" = "$wall" ] || fail "the selection rectangle stays after letting go"

# identify
ipc panel identify HEADLESS-1=1 HEADLESS-2=2 >/dev/null
sleep 0.8
[ "$(pixel 640 300)" != "$wall" ] || fail "identify shows nothing on the first screen"
[ "$(pixel 1920 300)" != "$wall" ] || fail "identify shows nothing on the second screen"
sleep 3
[ "$(pixel 1920 300)" = "$wall" ] || fail "the numbers of identify do not go away"

# the Start menu from the keyboard, on the main display
ipc focus output HEADLESS-2 >/dev/null
ipc panel startmenu toggle >/dev/null
sleep 1
[ "$(pixel 60 400)" != "$wall" ] || fail "the Start menu does not open on the main display"
[ "$(pixel 1340 400)" = "$wall" ] || fail "the Start menu opens on the focused screen"
ipc panel startmenu close >/dev/null
sleep 0.5

# only the main display gets a taskbar
sed -i 's/^# outputs \*.*/outputs main/' "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
sleep 2
[ "$(pixel 600 710)" != "$wall" ] || fail "with outputs main the main display lost its taskbar"
[ "$(pixel 1880 710)" = "$wall" ] || fail "with outputs main the other screen keeps a taskbar"
ipc main_output HEADLESS-2 >/dev/null
sleep 2
[ "$(pixel 1880 710)" != "$wall" ] || fail "the taskbar does not follow the main display"
[ "$(pixel 600 710)" = "$wall" ] || fail "the screen that is no longer main keeps its taskbar"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
