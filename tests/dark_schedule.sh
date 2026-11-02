#!/bin/sh
# Dark mode by the clock, in a nested tileWin with a clock of its own:
#  - a session that starts at night starts dark, one in the day light;
#  - it turns at the start and the end of the time;
#  - a change by hand lasts until the next of them;
#  - dark_schedule off leaves it as it is; a nonsense schedule is refused.
#
# usage: dark_schedule.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$msg"; do
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
sock=$XDG_RUNTIME_DIR/tw-darkclock-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"
echo 22:00 > "$work/clock"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
panel_command none
animations off
dark_schedule 21:00 07:00
output HEADLESS-1 mode --custom 800x600 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
	TILEWIN_SCHEDULE_CLOCK="$work/clock" TILEWIN_BATTERY_POLL_MS=200 \
	TILEWIN_SYS_POWER="$work/nopower" \
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

scheme() {
	ipc -t get_tilewin | python3 -c 'import json, sys; print(json.load(sys.stdin)["color_scheme"])'
}
at() {
	echo "$1" > "$work/clock"
	sleep 0.8
}
sleep 0.8
[ "$(scheme)" = dark ] || fail "a session started at 22:00 is $(scheme)"
at 23:30
[ "$(scheme)" = dark ] || fail "at 23:30 it is $(scheme)"
at 07:00
[ "$(scheme)" = light ] || fail "at the end of the time it is $(scheme)"
at 12:00
[ "$(scheme)" = light ] || fail "at noon it is $(scheme)"
at 21:00
[ "$(scheme)" = dark ] || fail "at the start of the time it is $(scheme)"

# by hand: lasts until the next start or end
ipc color_scheme light >/dev/null
at 23:00
[ "$(scheme)" = light ] || fail "light by hand went back to $(scheme) in the same night"
at 08:00
[ "$(scheme)" = light ] || fail "after the night it is $(scheme)"
ipc color_scheme dark >/dev/null
at 09:00
[ "$(scheme)" = dark ] || fail "dark by hand went back to $(scheme) in the same day"
at 22:00
at 08:00
[ "$(scheme)" = light ] || fail "the next end did not bring light back"

# a new schedule in the config applies at once, off leaves it
sed -i 's/^dark_schedule .*/dark_schedule 06:00 09:00/' "$work/tilewin.conf"
ipc reload >/dev/null
sleep 1
[ "$(scheme)" = dark ] || fail "a new schedule that says dark now left $(scheme)"
sed -i 's/^dark_schedule .*/dark_schedule off/' "$work/tilewin.conf"
ipc reload >/dev/null
sleep 0.5
at 12:00
[ "$(scheme)" = dark ] || fail "with the schedule off it turned $(scheme)"
ipc dark_schedule 25:00 07:00 2>&1 | grep -q Expected || fail "25:00 was taken for a time"
ipc dark_schedule 07:00 07:00 2>&1 | grep -q Expected || fail "a time of no length was taken"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
