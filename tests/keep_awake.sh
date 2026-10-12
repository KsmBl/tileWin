#!/bin/sh
# Keep awake, the switch in the taskbar's power flyout (the battery widget)
# for a download or anything else that has to run on, in a nested tileWin with
# the taskbar, a fake /sys/class/power_supply, a screen saver after 2 seconds
# without input and the screen off after 4:
#  - it is off at first, and "keep_awake on" turns it on (get_tilewin
#    "keep_awake"); anything but on, off or toggle is refused;
#  - while it is on no screen saver starts and the screen stays on, and a low
#    battery does not turn the battery saver on;
#  - turned off, the battery saver comes on at once, the screen saver starts
#    and the screen goes off as they did before;
#  - turned on again, the screen comes back, the screen saver ends and the
#    battery saver goes off;
#  - the flyout shows the switch and follows a change made elsewhere, and a
#    click on its row turns it on and the next one off again;
#  - a new session starts without it, however the last one ended.
#
# usage: keep_awake.sh <build dir> <source dir>
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
for needed in dbus-daemon grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-awake-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/power/BAT0" "$work/power/AC"
echo Battery > "$work/power/BAT0/type"
echo Mains > "$work/power/AC/type"
battery() {
	echo "$1" > "$work/power/BAT0/capacity"
	echo "$2" > "$work/power/BAT0/status"
	echo "$3" > "$work/power/AC/online"
	sleep 1
}
battery 50 Discharging 0
# the screen saver: says that it ran and stays until it is ended
cat > "$work/saver.sh" <<EOS
#!/bin/sh
touch "$work/saver-ran"
exec sleep 600
EOS
chmod +x "$work/saver.sh"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
stop() {
	ipc exit >/dev/null 2>&1
	attempt=0
	while [ -S "$sock" ] && ipc -t get_version >/dev/null 2>&1 && [ $attempt -lt 30 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	sleep 0.5
}
cleanup() {
	stop
	pkill -f "$work/saver.sh" 2>/dev/null
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
polkit_agent disable
panel_command $panel
idle_timeout screensaver 2
idle_timeout screen_off 4
screensaver_command $work/saver.sh
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

start() {
	rm -f "$work/display"
	env -u WAYLAND_DISPLAY -u DISPLAY \
		TILEWIN_SYS_POWER="$work/power" TILEWIN_BATTERY_POLL_MS=300 \
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
}
# "on" or "off" for a true or false field of get_tilewin
state() {
	ipc -t get_tilewin | python3 -I -c '
import json, sys
print("on" if json.load(sys.stdin).get(sys.argv[1]) else "off")' "$1"
}
screen() {
	ipc -t get_outputs | python3 -I -c '
import json, sys
print("on" if all(o.get("power") for o in json.load(sys.stdin)) else "off")'
}
saver_running() { pgrep -f "$work/saver.sh" >/dev/null 2>&1; }
# the middle of what differs between two screenshots: "<x> <y>", nothing when
# they are the same; with "<x> <y>" only the row of the flyout around there is
# looked at (a notification fading behind the flyout shines through elsewhere)
changed() {
	python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
a, b = (GdkPixbuf.Pixbuf.new_from_file(p) for p in sys.argv[1:3])
n, stride, w, h = a.get_n_channels(), a.get_rowstride(), a.get_width(), a.get_height()
da, db = a.get_pixels(), b.get_pixels()
xs, ys = [], []
x0, x1, y0, y1 = 0, w, 0, h
if len(sys.argv) > 4:
    cx, cy = int(sys.argv[3]), int(sys.argv[4])
    x0, x1, y0, y1 = cx - 150, cx + 150, cy - 20, cy + 20
for y in range(y0, y1):
    row = y * stride
    if da[row:row + w * n] == db[row:row + w * n]:
        continue
    for x in range(x0, x1):
        i = row + x * n
        if da[i:i + 3] != db[i:i + 3]:
            xs.append(x)
            ys.append(y)
if xs:
    print((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)' "$@"
}

start
ipc keep_awake on >/dev/null
[ "$(state keep_awake)" = on ] || fail "keep_awake on does not turn it on"
ipc keep_awake off >/dev/null
[ "$(state keep_awake)" = off ] || fail "keep_awake off does not turn it off"
ipc keep_awake sometimes 2>/dev/null | grep -q '"success": false' ||
	fail "keep_awake takes something that is not on, off or toggle"
ipc keep_awake toggle >/dev/null
[ "$(state keep_awake)" = on ] || fail "keep_awake toggle does not turn it on"

# on: nothing happens without input, and the low battery saves nothing
rm -f "$work/saver-ran"
battery 15 Discharging 0
sleep 5
[ ! -e "$work/saver-ran" ] || fail "the screen saver starts while kept awake"
[ "$(screen)" = on ] || fail "the screen goes off while kept awake"
[ "$(state battery_saver)" = off ] || fail "the battery saver comes on while kept awake"

# off: everything as before
ipc keep_awake toggle >/dev/null
[ "$(state keep_awake)" = off ] || fail "keep_awake toggle does not turn it off"
sleep 0.5
[ "$(state battery_saver)" = on ] ||
	fail "the battery saver does not come on at 15% once keep awake is off"
sleep 2.5
[ -e "$work/saver-ran" ] || fail "no screen saver after keep awake is off"
sleep 2.5
[ "$(screen)" = off ] || fail "the screen does not go off after keep awake is off"

# on again: back to a screen that is on, with nothing saving power
ipc keep_awake on >/dev/null
sleep 1
[ "$(screen)" = on ] || fail "turning keep awake on does not turn the screen back on"
saver_running && fail "turning keep awake on leaves the screen saver running"
[ "$(state battery_saver)" = off ] ||
	fail "turning keep awake on leaves the battery saver on"

# the switch in the flyout
ipc keep_awake off >/dev/null
ipc idle_timeout screensaver 0 >/dev/null
ipc idle_timeout screen_off 0 >/dev/null
battery 50 Charging 1
"$pointer" 1280 720 move 300 300 >/dev/null 2>&1
sleep 6 # the toast that the battery saver came on goes
grim "$work/closed.png"
ipc panel power >/dev/null
sleep 1.5
grim "$work/off.png"
[ -n "$(changed "$work/closed.png" "$work/off.png")" ] || fail "panel power opens no flyout"
ipc keep_awake on >/dev/null
sleep 1
grim "$work/on.png"
if [ -n "${AWAKE_SHOTS:-}" ]; then
	mkdir -p "$AWAKE_SHOTS" && cp "$work/off.png" "$work/on.png" "$AWAKE_SHOTS/"
fi
middle=$(changed "$work/off.png" "$work/on.png")
if [ -z "$middle" ]; then
	fail "the flyout does not show that keep awake was turned on"
else
	set -- $middle
	echo "the keep awake row of the flyout is around $1,$2"
	"$pointer" 1280 720 move "$1" "$2" wait 300 click "$1" "$2" >/dev/null 2>&1
	sleep 1
	[ "$(state keep_awake)" = off ] || fail "a click on the switch does not turn keep awake off"
	grim "$work/clicked-off.png"
	[ -z "$(changed "$work/off.png" "$work/clicked-off.png" "$1" "$2")" ] ||
		fail "the flyout does not show that the click turned keep awake off"
	"$pointer" 1280 720 click "$1" "$2" >/dev/null 2>&1
	sleep 1
	[ "$(state keep_awake)" = on ] || fail "a second click does not turn keep awake on"
	grim "$work/clicked.png"
	[ -z "$(changed "$work/on.png" "$work/clicked.png" "$1" "$2")" ] ||
		fail "the flyout looks different turned on with a click than turned on by the command"
fi

# a new session, as after a restart of the computer
[ "$(state keep_awake)" = on ] || fail "keep awake is not on before the session ends"
stop
start
sleep 1
[ "$(state keep_awake)" = off ] || fail "a new session starts kept awake"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
