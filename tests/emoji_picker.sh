#!/bin/sh
# The emoji picker (Win+.), in a nested tileWin with the taskbar and the
# clipboard program: a search typed into it finds the emoji by name, Enter
# types it into the window that had the keyboard (text_target.py), the
# clipboard gets back what it held, and the emoji is among the recent ones.
#
# usage: emoji_picker.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool
clipboard=$build/clipboard/tilewin-clipboard

for needed in "$compositor" "$msg" "$panel" "$pointer" "$keys" "$clipboard"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session python3 wl-copy wl-paste; do
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
sock=$XDG_RUNTIME_DIR/tw-emoji-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export PATH="$build/clipboard:$PATH"
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

sleep 3
typed=$work/typed.txt
ipc exec "python3 $source_dir/tests/text_target.py $typed" >/dev/null
attempt=0
while [ ! -f "$typed" ] && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
[ -f "$typed" ] || fail "the window to type into never came up"
sleep 1
printf 'before' | wl-copy
sleep 0.5

ipc panel emoji >/dev/null
sleep 1
"$keys" text rocket >/dev/null 2>&1
sleep 0.5
"$keys" key none return >/dev/null 2>&1
sleep 2
[ "$(cat "$typed" 2>/dev/null)" = "$(printf '\360\237\232\200')" ] ||
	fail "Enter on the rocket did not type it into the window: [$(cat "$typed" 2>/dev/null)]"
[ "$(wl-paste -n 2>/dev/null)" = before ] ||
	fail "the clipboard did not get back what it held: [$(wl-paste -n 2>/dev/null)]"
grep -qx "$(printf '\360\237\232\200')" "$XDG_STATE_HOME/tileWin/emoji-recent" 2>/dev/null ||
	fail "the rocket is not among the recent emoji"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
