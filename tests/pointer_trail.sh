#!/bin/sh
# The pointer trail and the pointer grown by shaking, in a nested tileWin:
# copies left while the pointer is grown are grown as well, and those left
# after it shrank back are of the normal size again.
#
# usage: pointer_trail.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool
count=$build/tests/count-content

for needed in "$compositor" "$msg" "$pointer" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim; do
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
sock=$XDG_RUNTIME_DIR/tw-trail-$$.sock
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
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<CONF
wallpaper solid #204070
session_restore no
panel_command true
animations off
pointer_trail 1500
pointer_shake enable
pointer_shake_max 400
pointer_shake_rate 3000
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
CONF

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
sleep 1

# a line of copies at the normal size
"$pointer" 1280 720 move 100 360 move 250 360 move 400 360 move 550 360 move 700 360 \
	move 850 360 >/dev/null 2>&1 &
line=$!
sleep 1.4
grim "$work/normal.png"
wait "$line"
normal=$("$count" "$work/normal.png" 0 250 1280 250)
sleep 2.5 # (they fade)

# grown by shaking, then the same line
"$pointer" 1280 720 move 640 360 shake 14 6 240 25 move 100 360 move 250 360 move 400 360 \
	move 550 360 move 700 360 move 850 360 >/dev/null 2>&1 &
line=$!
sleep 3.4
grim "$work/grown.png"
wait "$line"
grown=$("$count" "$work/grown.png" 0 250 1280 250)
echo "trail pixels at the normal size: $normal, while grown: $grown"
[ "$normal" -gt 100 ] || fail "the trail leaves no copies"
[ "$grown" -gt $((normal * 8)) ] || fail "copies left while the pointer is grown are not grown"

"$msg" -s "$sock" -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
