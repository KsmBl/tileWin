#!/bin/sh
# A new desktop comes last, to the right of all there are, also after one
# before it was closed: it used to take the first free number, and as the
# numbers sort the desktops it came up in the place of the closed one.
# In a nested tileWin with three desktops named A, B and C:
#  - closing the first and making a new one gives B, C and the new one;
#  - closing the one in the middle and making a new one puts that last too;
#  - the new desktop is the one in front, and it can be moved to the left.
#
# usage: desktop_order.sh <build dir> <source dir>
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
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
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
	pkill -f "^[^ ]*/tilewin -c $work/" 2>/dev/null
	sleep 0.3
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

sock=$work/ipc.sock
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }

# the desktops from left to right, a new one without a name as "new"
order() {
	ipc -t get_workspaces | python3 -c '
import json, sys
print(" ".join(w["name"].partition(":")[2] or "new"
    for w in sorted(json.load(sys.stdin), key=lambda w: w["num"])))'
}
# the name of the desktop in front
front() {
	ipc -t get_workspaces | python3 -c '
import json, sys
print(next(w["name"].partition(":")[2] or "new" for w in json.load(sys.stdin) if w["focused"]))'
}
# switches to the desktop with that name
show() {
	ipc workspace "$(ipc -t get_workspaces | python3 -c '
import json, sys
print(next(w["name"] for w in json.load(sys.stdin) if w["name"].endswith(":" + sys.argv[1])))' "$1")" \
		>/dev/null 2>&1
	sleep 0.5
}

ipc mode window >/dev/null 2>&1
sleep 1
for name in A B C; do
	ipc desktop new >/dev/null 2>&1
	sleep 0.5
	ipc desktop rename $name >/dev/null 2>&1
	sleep 0.5
done
if [ "$(order)" != "A B C" ]; then
	echo "three desktops A, B and C were expected, got '$(order)'"
	exit 1
fi

# ---------- after the first was closed ----------
show A
ipc desktop close >/dev/null 2>&1
sleep 0.5
[ "$(order)" = "B C" ] || fail "closing the first desktop left '$(order)', wanted 'B C'"
ipc desktop new >/dev/null 2>&1
sleep 0.5
[ "$(order)" = "B C new" ] ||
	fail "a new desktop after the first was closed gave '$(order)', wanted 'B C new'"
[ "$(front)" = new ] || fail "the new desktop is not the one in front ($(front))"
ipc desktop rename D >/dev/null 2>&1
sleep 0.5

# ---------- after one in the middle was closed ----------
show C
ipc desktop close >/dev/null 2>&1
sleep 0.5
[ "$(order)" = "B D" ] || fail "closing the desktop in the middle left '$(order)', wanted 'B D'"
ipc desktop new >/dev/null 2>&1
sleep 0.5
[ "$(order)" = "B D new" ] ||
	fail "a new desktop after one in the middle was closed gave '$(order)', wanted 'B D new'"

# ---------- and it moves like any other ----------
ipc desktop move left >/dev/null 2>&1
sleep 0.5
[ "$(order)" = "B new D" ] || fail "moving the new desktop left gave '$(order)', wanted 'B new D'"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "new desktops come last"
