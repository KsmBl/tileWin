#!/bin/sh
# The media buttons under the preview of a player's window, in a nested
# tileWin with the taskbar: a window that is an MPRIS player (fake_player.py)
# gets previous, play/pause and next under its preview, the buttons reach the
# player, and play/pause shows what the player says it does now.
#
# usage: media_buttons.sh <build dir> <source dir>
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
for needed in dbus-run-session grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-media-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk' \
		>/dev/null 2>&1; then
	echo "python3 has no GTK 4: skipping"
	exit 77
fi
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

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

ipc theme win10 >/dev/null
sleep 2
log=$work/player.log
ipc exec "python3 $source_dir/tests/fake_player.py $log" >/dev/null
attempt=0
while ! grep -q ready "$log" 2>/dev/null && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
grep -q ready "$log" 2>/dev/null || fail "the test player never came up"
sleep 1.5

# rest on the taskbar button of the player (the first after the search box),
# then go up to its preview: the buttons lie in its bottom row, around x 371
without=$(mean 300 636 150 28)
"$pointer" 1280 720 move 372 700 wait 1500 >/dev/null 2>&1
with=$(mean 300 636 150 28)
[ "$with" != "$without" ] || fail "the preview of the player has no media buttons"
"$pointer" 1280 720 move 372 700 wait 1500 click 371 650 wait 300 click 405 650 wait 300 \
	click 337 650 wait 600 >/dev/null 2>&1
calls=$(grep -v ready "$log" | tr '\n' ' ')
[ "$calls" = "PlayPause Next Previous " ] || fail "the buttons did not reach the player ($calls)"

# paused now: the middle button shows play, a triangle, no longer two bars
"$pointer" 1280 720 move 372 700 wait 1500 move 300 640 wait 600 >/dev/null 2>&1
glyph=$(grim -g "364,642 14x16" -t ppm - 2>/dev/null | python3 -c '
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
w = int(fields[1])
px = data[pos + 1:]
# the column between the two bars of pause is dark; a triangle fills it
col = 7
print(max(px[(y * w + col) * 3] for y in range(16)))')
[ "${glyph:-0}" -gt 128 ] || fail "play/pause still shows pause after pausing ($glyph)"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
