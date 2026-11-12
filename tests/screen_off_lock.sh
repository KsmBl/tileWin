#!/bin/sh
# Turning the screen off locks it, in a nested tileWin with the real
# tilewin-lock and "idle_timeout screen_off 2" (Power & sleep, "Turn off the
# screen after"). The test checks that
#  - the screen is on and the session unlocked before the two seconds are up;
#  - then the screen goes off (get_outputs) and the session is locked: the lock
#    screen runs and a second one cannot lock the session;
#  - a key turns the screen on again and shows the lock screen, not the
#    desktop, still locked;
#  - the next time the screen goes off while locked, no second lock screen is
#    started.
#
# usage: screen_off_lock.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
lock=$build/lock/tilewin-lock
msg=$build/swaymsg/tilewinmsg
keyboard=$build/tests/keyboard-tool
count=$build/tests/count-content

for needed in "$compositor" "$lock" "$msg" "$keyboard" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session python3; do
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
sock=$XDG_RUNTIME_DIR/tw-screen-off-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	for pid in $(lock_pids); do
		kill "$pid" 2>/dev/null
	done
	rm -rf "$work" "$sock"
}
trap cleanup EXIT INT TERM

# the lock screen notes each start, so a second one shows
cat > "$work/lock.sh" <<EOF
#!/bin/sh
echo started >> $work/lock-starts
exec $lock
EOF
chmod +x "$work/lock.sh"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
animations off
idle_timeout screen_off 2
lock_command $work/lock.sh
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

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

# the lock screens of this nested session
lock_pids() {
	for pid in $(pgrep -f "^$lock" 2>/dev/null); do
		if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null |
				grep -qx "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"; then
			echo "$pid"
		fi
	done
}
# locked: a lock screen runs, and another one cannot lock the session
locked() {
	[ -n "$(lock_pids)" ] || return 1
	timeout 5 "$lock" > /dev/null 2>&1
	[ $? -eq 2 ]
}
screen() {
	ipc -t get_outputs | python3 -I -c '
import json, sys
print("on" if all(o.get("power") for o in json.load(sys.stdin)) else "off")'
}
brightness() {
	grim "$work/shot.png" 2>/dev/null
	"$count" --mean "$work/shot.png" 0 0 1280 720 | awk '{ print int(($1 + $2 + $3) / 3) }'
}

desktop=$(brightness)
echo "the desktop: brightness $desktop"
[ "$(screen)" = on ] || fail "the screen is off before its time"
[ -z "$(lock_pids)" ] || fail "the session is locked before the screen went off"

sleep 3.5
echo "after three and a half seconds: screen $(screen)"
[ "$(screen)" = off ] || fail "the screen did not go off after the idle time"
locked || fail "turning the screen off did not lock the session"

"$keyboard" key none escape >/dev/null 2>&1
sleep 0.5
back=$(brightness)
echo "after a key: screen $(screen), brightness $back"
[ "$(screen)" = on ] || fail "a key did not turn the screen on again"
[ "$back" -lt "$desktop" ] || fail "the desktop shows after the screen came on again"
locked || fail "the session is not locked after the screen came on again"

sleep 3.5
[ "$(screen)" = off ] || fail "the screen did not go off again while locked"
starts=$(cat "$work/lock-starts" 2>/dev/null | wc -l)
echo "lock screens started: $starts"
[ "$starts" -le 1 ] || fail "turning the screen off while locked started another lock screen"
"$keyboard" key none escape >/dev/null 2>&1
sleep 0.5
locked || fail "the session is not locked after the second time"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
