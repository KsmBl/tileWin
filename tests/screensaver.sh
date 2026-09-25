#!/bin/sh
# The screen saver in a nested tileWin. With "idle_timeout screensaver 3" the
# test checks that
#  - nothing covers the desktop before the three seconds are up;
#  - then tilewin-screensaver covers it (the screen turns dark, from teal);
#  - moving the mouse ends it, brings the desktop back and, with
#    "screensaver_lock yes", runs the lock command;
#  - "screensaver start" starts it at once and is not ended by input in the
#    first moment (the keys that ran the command), but by input after that.
#
# usage: screensaver.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
saver=$build/screensaver/tilewin-screensaver
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool
count=$build/tests/count-content

for needed in "$compositor" "$saver" "$msg" "$pointer" "$count"; do
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
sock=$XDG_RUNTIME_DIR/tw-saver-$$.sock
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
	rm -rf "$work" "$sock"
}
trap cleanup EXIT INT TERM

cat > "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
screensaver {
	name mystify
}
EOF

# a fake lock screen that only notes it was asked for
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
animations off
idle_timeout screensaver 3
screensaver_command $saver
screensaver_lock yes
lock_command echo locked >> $work/locked
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

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
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }

# how bright the screen is: the teal desktop is about 85, black about 0
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
# the pointer of a new virtual device loses its first motion: move twice
nudge() {
	"$pointer" 1280 720 move "$1" 300 move $(($1 + 40)) 340 move $(($1 + 80)) 380 \
		>/dev/null 2>&1
}

sleep 1
early=$(brightness)
echo "after a second: brightness $early"
[ "$early" -gt 60 ] || fail "something covers the desktop before the screen saver is due"
running && fail "the screen saver started too early"

sleep 4
dark=$(brightness)
echo "after five seconds: brightness $dark"
running || fail "tilewin-screensaver is not running after the idle time"
[ "$dark" -lt 30 ] || fail "the screen saver does not cover the desktop"

nudge 300
sleep 1
back=$(brightness)
echo "after moving the mouse: brightness $back"
running && fail "moving the mouse did not end the screen saver"
[ "$back" -gt 60 ] || fail "the desktop did not come back"
grep -q locked "$work/locked" 2>/dev/null ||
	fail "coming back did not run the lock command (screensaver_lock yes)"

# by hand: the input right after the command does not count
ipc idle_timeout screensaver never >/dev/null
# two moves: the first is lost, the second comes some 400 ms after the start
ipc screensaver start >/dev/null
"$pointer" 1280 720 move 500 300 move 560 360 >/dev/null 2>&1
sleep 0.3
running || fail "input right after 'screensaver start' ended it"
sleep 1
nudge 700
sleep 1
running && fail "input a moment after 'screensaver start' did not end it"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
