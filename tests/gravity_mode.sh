#!/bin/sh
# Gravity mode: a window let go of while it is still moving carries on sliding
# and comes back off the edges of the screen. There is no pull downwards, so
# the checks are that a thrown window travels further than the throw itself,
# that it is still travelling a moment later, and that it does none of this
# when it was put down gently or when the mode is off.
#
# usage: gravity_mode.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session xfce4-terminal; do
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
	pkill -f "tilewin -c $work/" 2>/dev/null
	sleep 0.3
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

sock=$work/ipc.sock
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
window_gravity enable
window_gravity_drag 1.5
window_gravity_bounce 0.6
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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

ipc mode window >/dev/null 2>&1
sleep 1
ipc exec "xfce4-terminal --disable-server" >/dev/null 2>&1
attempt=0
while [ $attempt -lt 40 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
	ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"' && break
done
if ! ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"'; then
	echo "no window came up to throw"
	exit 1
fi
ipc floating enable >/dev/null 2>&1
ipc resize set 300 200 >/dev/null 2>&1
sleep 1

# The tree reports the content area; the title bar to grab is above it.
left_edge() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
    if isinstance(n, dict):
        if n.get("app_id") == "xfce4-terminal":
            print(n["rect"]["x"])
            return True
        for c in n.get("nodes", []) + n.get("floating_nodes", []):
            if walk(c):
                return True
    return False
walk(json.load(sys.stdin))'
}
top_edge() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
    if isinstance(n, dict):
        if n.get("app_id") == "xfce4-terminal":
            print(n["rect"]["y"])
            return True
        for c in n.get("nodes", []) + n.get("floating_nodes", []):
            if walk(c):
                return True
    return False
walk(json.load(sys.stdin))'
}

# throws the window leftwards from wherever it is, and says where it landed
throw_left() {
	ipc move position 700 300 >/dev/null 2>&1
	sleep 1
	bar=$(( $(top_edge) - 15 ))
	grab=$(( $(left_edge) + 60 ))
	"$pointer" 1280 720 throw "$grab" "$bar" "$(( grab - 300 ))" "$bar" >/dev/null 2>&1
}

# ---------- thrown: it goes further than the throw and is still going ----------
throw_left
landed=$(left_edge)
sleep 1
later=$(left_edge)
sleep 2
settled=$(left_edge)
echo "thrown: let go at $landed, $later a second later, $settled after three"
# the sweep itself only carried it 300px from x=700, so anything well past that
# came from the slide
if [ "$landed" -gt 300 ]; then
	fail "the window did not slide on after it was let go of (landed at $landed)"
fi
if [ "$landed" = "$later" ] && [ "$later" = "$settled" ]; then
	fail "the window stopped the instant it was let go of"
fi

# ---------- put down gently: it stays ----------
ipc move position 700 300 >/dev/null 2>&1
sleep 1
bar=$(( $(top_edge) - 15 ))
grab=$(( $(left_edge) + 60 ))
"$pointer" 1280 720 click "$grab" "$bar" >/dev/null 2>&1
sleep 1
"$pointer" 1280 720 move $(( grab - 200 )) "$bar" >/dev/null 2>&1
gentle=$(left_edge)
sleep 2
if [ "$gentle" != "$(left_edge)" ]; then
	fail "a window put down without moving slid away by itself"
fi

# ---------- with the mode off, a throw is just a move ----------
ipc window_gravity disable >/dev/null 2>&1
sleep 0.5
throw_left
off_landed=$(left_edge)
sleep 2
if [ "$off_landed" != "$(left_edge)" ]; then
	fail "the window slid although gravity mode was off"
fi
if [ "$off_landed" -lt 300 ]; then
	fail "with the mode off the window went further than the throw itself"
fi
echo "with the mode off it stayed at $off_landed"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "a thrown window slides on, and only when it should"
exit 0
