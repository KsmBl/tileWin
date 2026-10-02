#!/bin/sh
# The battery saver, in a nested tileWin with the taskbar and a fake
# /sys/class/power_supply (TILEWIN_SYS_POWER) read every 300 ms:
#  - on battery above the level (20% by default) it stays off;
#  - at or below it, it comes on (get_tilewin "battery_saver") and the taskbar
#    tells so in a notification;
#  - plugged in, it goes off again;
#  - "battery_saver off" keeps it off however low the battery is, and a level
#    that is no number is refused.
#
# usage: battery_saver.sh <build dir> <source dir>
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
for needed in dbus-run-session python3; do
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
sock=$XDG_RUNTIME_DIR/tw-battery-$$.sock
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
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

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
ipc() { "$msg" -s "$sock" "$@"; }
saver() {
	ipc -t get_tilewin | python3 -c '
import json, sys
print("on" if json.load(sys.stdin).get("battery_saver") else "off")'
}
sleep 2

[ "$(saver)" = off ] || fail "the battery saver is on at 50%"
battery 15 Discharging 0
[ "$(saver)" = on ] || fail "the battery saver does not come on at 15%"
sleep 3
grep -q "Battery saver is on" "$XDG_STATE_HOME/tileWin/notifications.json" 2>/dev/null ||
	fail "the taskbar shows no notification that the battery saver is on"
battery 15 Charging 1
[ "$(saver)" = off ] || fail "the battery saver stays on when plugged in"

battery 5 Discharging 0
ipc battery_saver off >/dev/null
sleep 1
[ "$(saver)" = off ] || fail "battery_saver off does not keep it off"
ipc battery_saver lots 2>/dev/null | grep -q '"success": false' ||
	fail "a level that is no number is taken"
ipc battery_saver 10 >/dev/null
sleep 1
[ "$(saver)" = on ] || fail "battery_saver 10 does not turn it on at 5%"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
