#!/bin/sh
# The fingerprint reader, with a fake fprintd on the nested session's bus
# (fake_fprintd.py, TILEWIN_FPRINT_BUS=session):
#  - the Fingerprint page of the settings adds a finger (Claim, EnrollStart,
#    the stages, Release) and removes it again;
#  - the lock screen listens for a finger once one is enrolled: a finger that
#    is not recognised leaves the session locked and says so, and it listens
#    again; the right one unlocks it;
#  - with "Unlock the lock screen with a fingerprint" off, the lock screen
#    leaves the reader alone.
#
# usage: fingerprint.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
lock=$build/lock/tilewin-lock
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$settings" "$lock" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! nm -D "$lock" 2>/dev/null | grep -q sd_bus_open; then
	echo "tilewin-lock was built without sd-bus: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
if ! python3 -c 'from gi.repository import Gio' 2>/dev/null; then
	echo "python3 has no GObject introspection: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
fp=$work/fp
mkdir -p "$fp"
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export GIO_USE_VFS=local TILEWIN_FPRINT_BUS=session
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	pkill -f "fake_fprintd.py $fp" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
animations off
lock_command true
output * mode --custom 1280x900
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec python3 $source_dir/tests/fake_fprintd.py $fp
EOF
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
if ! wait_until 10 test -e "$fp/ready"; then
	echo "the fake fprintd never came up"
	exit 1
fi
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
click() { "$pointer" 1280 900 click "$1" "$2" >/dev/null 2>&1; }
called() { grep -c "^$1" "$fp/calls" 2>/dev/null || echo 0; }

# ---- the Fingerprint page: add the right index finger, then remove it
ipc exec "$settings --page=fingerprint" >/dev/null
wait_until 20 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.Settings\"'" ||
	fail "the settings never came up"
wait_still 15 screen
click 1195 465 # Add...
wait_until 10 grep -qx right-index-finger "$fp/fingers" ||
	fail "Add... enrolled no finger: $(tr '\n' ' ' < "$fp/calls")"
wait_until 5 grep -qx Release "$fp/calls" || fail "the reader was not let go after enrolling"
grep -qx 'EnrollStart right-index-finger' "$fp/calls" || fail "EnrollStart was not asked for the right finger"
wait_still 5 screen
click 803 543 # Done
wait_still 5 screen
click 1187 299 # Remove, on the row of the finger
wait_until 5 sh -c "[ ! -s '$fp/fingers' ]" ||
	fail "Remove left the fingers $(tr '\n' ' ' < "$fp/fingers")"
grep -qx 'DeleteEnrolledFinger right-index-finger' "$fp/calls" ||
	fail "Remove did not ask fprintd to delete the finger"
ipc '[app_id="org.tilewin.Settings"] kill' >/dev/null

# ---- the lock screen
lock_pid() {
	for pid in $(pgrep -f "^$lock" 2>/dev/null); do
		if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null |
				grep -qx "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"; then
			echo "$pid"
		fi
	done
}
lock_gone() { [ -z "$(lock_pid)" ]; }

echo right-index-finger > "$fp/fingers"
: > "$fp/calls"
ipc exec "$lock" >/dev/null
if wait_until 10 grep -qx 'VerifyStart any' "$fp/calls"; then
	echo no-match > "$fp/touch"
	wait_until 5 sh -c "[ \$(grep -c '^VerifyStart' '$fp/calls') -ge 2 ]" ||
		fail "the lock screen did not listen again after a finger that was not recognised"
	lock_gone && fail "a finger that was not recognised unlocked the session"
	echo match > "$fp/touch"
	wait_until 5 lock_gone || fail "the right finger did not unlock the session"
	wait_until 5 grep -qx Release "$fp/calls" || fail "the lock screen did not let go of the reader"
else
	fail "the lock screen never listened for a finger: $(tr '\n' ' ' < "$fp/calls")"
fi

# ---- the switch off: the lock screen leaves the reader alone
echo no > "$XDG_CONFIG_HOME/tileWin/lock-fingerprint"
: > "$fp/calls"
ipc exec "$lock" >/dev/null
wait_until 10 sh -c "[ -n \"\$(pgrep -f '^$lock')\" ]"
sleep 2 # a negative: nothing may be asked
[ -s "$fp/calls" ] && fail "with the switch off the lock screen asked fprintd: $(tr '\n' ' ' < "$fp/calls")"
for pid in $(lock_pid); do
	kill "$pid" 2>/dev/null # the session stays locked: the compositor ends with the test
done

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
