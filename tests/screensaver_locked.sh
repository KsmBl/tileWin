#!/bin/sh
# The screen saver over the lock screen, in a nested tileWin with the real
# tilewin-lock. With "idle_timeout screensaver 3" and the Blank saver the test
# checks that
#  - locked, the lock screen shows until the three seconds are up;
#  - then the screen saver covers the lock screen (the screen turns black);
#  - a key ends it and brings back the lock screen, still locked: the lock
#    screen keeps running, the desktop does not show, and a second lock screen
#    cannot lock the session (it is locked already);
#  - the screen saver comes again after the next three seconds without input.
#
# usage: screensaver_locked.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
saver=$build/screensaver/tilewin-screensaver
lock=$build/lock/tilewin-lock
msg=$build/swaymsg/tilewinmsg
keyboard=$build/tests/keyboard-tool
count=$build/tests/count-content

for needed in "$compositor" "$saver" "$lock" "$msg" "$keyboard" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session; do
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
sock=$XDG_RUNTIME_DIR/tw-saver-locked-$$.sock
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
	[ -n "${lock_pid:-}" ] && kill "$lock_pid" 2>/dev/null
	rm -rf "$work" "$sock"
}
trap cleanup EXIT INT TERM

cat > "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
screensaver {
	name blank
}
EOF

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
animations off
idle_timeout screensaver 3
screensaver_command $saver
lock_command true
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

# how bright the screen is: the teal desktop is about 85, black 0
brightness() {
	grim "$work/shot.png" 2>/dev/null
	"$count" --mean "$work/shot.png" 0 0 1280 720 | awk '{ print int(($1 + $2 + $3) / 3) }'
}
running() {
	for pid in $(pgrep -f "^$saver" 2>/dev/null); do
		# only the one of this nested session
		if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null |
				grep -qx "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"; then
			return 0
		fi
	done
	return 1
}
# still locked: the lock screen runs, and another one cannot lock the session
still_locked() {
	kill -0 "$lock_pid" 2>/dev/null || return 1
	timeout 5 "$lock" > /dev/null 2>&1
	[ $? -eq 2 ]
}

desktop=$(brightness)
echo "the desktop: brightness $desktop"

"$lock" > "$work/lock.log" 2>&1 &
lock_pid=$!
sleep 1
locked=$(brightness)
echo "locked, after a second: brightness $locked"
[ "$locked" -gt 5 ] && [ "$locked" -lt "$desktop" ] ||
	fail "the lock screen does not show"
running && fail "the screen saver started too early"

sleep 4
dark=$(brightness)
echo "locked, after five seconds: brightness $dark"
running || fail "tilewin-screensaver is not running after the idle time while locked"
[ "$dark" -lt 3 ] || fail "the screen saver does not cover the lock screen"

"$keyboard" key none escape >/dev/null 2>&1
sleep 1
back=$(brightness)
echo "after a key: brightness $back"
running && fail "the key did not end the screen saver"
[ "$back" -gt 5 ] || fail "the lock screen did not come back"
[ "$back" -lt "$desktop" ] || fail "the desktop shows after the key"
still_locked || fail "the session is not locked any more after the key"

sleep 4
again=$(brightness)
echo "four seconds later: brightness $again"
running || fail "the screen saver did not come again while locked"
[ "$again" -lt 3 ] || fail "the screen saver did not cover the lock screen again"
"$keyboard" key none escape >/dev/null 2>&1
sleep 1
still_locked || fail "the session is not locked any more after the second key"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
