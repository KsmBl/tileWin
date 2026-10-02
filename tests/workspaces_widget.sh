#!/bin/sh
# The workspaces widget on the taskbar, in a nested tileWin: every desktop as
# a small screen with its windows where they are, following a window that
# moves; resting on a desktop shows a preview of it with its windows as they
# look, a click on the preview goes there, and so does a click on a desktop.
#
# usage: workspaces_widget.sh <build dir> <source dir>
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
for needed in dbus-run-session grim python3 xfce4-terminal; do
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
sock=$XDG_RUNTIME_DIR/tw-workspaces-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
sed 's/^\tleft start search taskbar$/\tleft start search workspaces taskbar/' \
	"$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
printf 'theme_layout no\nwidget workspaces { labels no }\n' >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

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
# the mean brightness of a part of the screen
mean() {
	grim -g "$1,$2 $3x$4" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
	while data[pos:pos + 1].isspace():
		pos += 1
	start = pos
	while not data[pos:pos + 1].isspace():
		pos += 1
	fields.append(data[start:pos])
pixels = data[pos + 1:]
print(sum(pixels) // max(1, len(pixels)))'
}

windows_of() { ipc -t get_workspaces | python3 -c '
import json, sys
print([w["name"] for w in json.load(sys.stdin) if w["focused"]][0])'; }
sleep 2
ipc theme win10 >/dev/null
sleep 1.5
ipc exec "xfce4-terminal --disable-server -T One" >/dev/null
sleep 2.5
ipc '[title=^One$] snap left' >/dev/null
ipc workspace 2 >/dev/null
ipc exec "python3 $source_dir/tests/color_window.py Red '#d02020'" >/dev/null
sleep 3
ipc '[title=^Red$] snap left' >/dev/null
ipc exec "xfce4-terminal --disable-server -T Two" >/dev/null
sleep 2.5
ipc '[title=^Two$] snap right' >/dev/null
sleep 0.5
ipc workspace 1 >/dev/null
sleep 2

# the widget follows the search box of Windows 10: the first desktop's small
# screen at x 352 to 409, the second's at 413 to 470, y 684 to 716
left=$(mean 356 690 20 20)
right=$(mean 384 690 20 20)
[ "$left" -gt "$right" ] || fail "the window snapped left does not show on the left of its desktop ($left, $right)"

ipc '[title=^One$] snap restore' >/dev/null
ipc '[title=^One$] snap right' >/dev/null
sleep 2.5
left=$(mean 356 690 20 20)
right=$(mean 384 690 20 20)
[ "$right" -gt "$left" ] || fail "the widget did not follow the window to the right ($left, $right)"

# resting on the second desktop: its preview over the taskbar, 400 wide and
# centered on it, the red window filling its left half
"$pointer" 1280 720 move 441 700 wait 2500 >/dev/null 2>&1 &
held=$!
sleep 2
rgb=$(grim -g "300,520 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //')
set -- $rgb
[ "${1:-0}" -gt 150 ] && [ "${3:-255}" -lt 90 ] ||
	fail "the preview of the second desktop does not show its red window ($rgb)"
wait "$held"
"$pointer" 1280 720 move 441 700 wait 1500 move 441 600 wait 300 click 441 600 >/dev/null 2>&1
sleep 1
[ "$(windows_of)" = 2 ] || fail "a click on the preview did not go to its desktop ($(windows_of))"

# and a click on the first desktop in the bar
"$pointer" 1280 720 click 380 700 >/dev/null 2>&1
sleep 1
[ "$(windows_of)" = 1 ] || fail "a click on the first desktop did not go there ($(windows_of))"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
