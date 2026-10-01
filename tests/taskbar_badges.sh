#!/bin/sh
# Progress and counts of apps on their taskbar buttons, in a nested tileWin
# with the taskbar: an app sending com.canonical.Unity.LauncherEntry.Update
# (tests/launcher_entry.py, as Firefox or a mail app does)
#  - with progress fills its button green (Windows 10), in navy blocks on
#    Windows 95, and the more progress, the more of the button;
#  - with a count shows a red number on the corner of its icon;
#  - leaving the bus takes both away again.
#
# usage: taskbar_badges.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel

for needed in "$compositor" "$msg" "$panel"; do
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
if ! python3 -c 'from gi.repository import Gio' 2>/dev/null; then
	echo "python-gobject is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-badges-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
app=
cleanup() {
	[ -n "$app" ] && kill "$app" 2>/dev/null
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
exec sh -c 'printf %s "\$DBUS_SESSION_BUS_ADDRESS" > $work/dbus; printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
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
export WAYLAND_DISPLAY=$(cat "$work/display") DBUS_SESSION_BUS_ADDRESS=$(cat "$work/dbus")
ipc() { "$msg" -s "$sock" "$@"; }

# pixels of the taskbar (the bottom 40) that are green, red or navy
count() {
	grim -g "0,680 1280x40" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
# P6 header: magic, width, height, maximum, each followed by white space
fields, pos = [], 0
while len(fields) < 4:
	while data[pos:pos + 1].isspace():
		pos += 1
	start = pos
	while not data[pos:pos + 1].isspace():
		pos += 1
	fields.append(data[start:pos])
pixels = data[pos + 1:]
kind = sys.argv[1]
n = 0
for i in range(0, len(pixels) - 2, 3):
	r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
	if kind == "green" and g > 100 and g > r + 50 and g > b + 50:
		n += 1
	elif kind == "red" and r > 150 and g < 90 and b < 90:
		n += 1
	elif kind == "navy" and r < 20 and g < 20 and b > 100:
		n += 1
print(n)' "$1"
}

ipc theme win10 >/dev/null
sleep 2
ipc exec "xfce4-terminal --disable-server -T Terminal" >/dev/null
sleep 3

mkfifo "$work/in"
python3 "$source_dir/tests/launcher_entry.py" xfce4-terminal.desktop < "$work/in" > "$work/out" 2>&1 &
app=$!
exec 3> "$work/in"
sleep 1
send() {
	echo "$*" >&3
	sleep 0.8
}

green0=$(count green)
red0=$(count red)
send progress 0.3
green30=$(count green)
send progress 0.9
green90=$(count green)
[ "$green30" -gt $((green0 + 100)) ] || fail "progress does not fill the button ($green0 -> $green30)"
[ "$green90" -gt $((green30 + 100)) ] || fail "more progress does not fill more ($green30 -> $green90)"
send count 3
red3=$(count red)
[ "$red3" -gt $((red0 + 40)) ] || fail "the count shows no badge ($red0 -> $red3)"

ipc theme win95 >/dev/null
sleep 2.5
[ "$(count navy)" -gt 60 ] || fail "Windows 95 shows no progress blocks"
ipc theme win10 >/dev/null
sleep 2.5

send quit
exec 3>&-
wait "$app" 2>/dev/null
app=
sleep 1
[ "$(count green)" -le $((green0 + 20)) ] || fail "the progress stays after the app left the bus"
[ "$(count red)" -le $((red0 + 10)) ] || fail "the count stays after the app left the bus"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
