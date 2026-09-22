#!/bin/sh
# A desktop keeps its place by its number, so a name given to one has to live
# beside that number rather than replace it. The checks are that a name can be
# given and taken away, that it travels with the desktop when the desktop is
# moved, and that a workspace named by hand in the config is left alone.
#
# usage: named_desktops.sh <build dir> <source dir>
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
	pkill -f "tilewin -c $work/" 2>/dev/null
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

# "<number>:<name>" for every desktop, in the order the numbers put them
names() {
	ipc -t get_workspaces | python3 -c '
import json, sys
print(" ".join(n for _, n in sorted((w["num"], w["name"]) for w in json.load(sys.stdin))))'
}

ipc mode window >/dev/null 2>&1
sleep 1
# an empty desktop is thrown away as soon as it is left, so two are made: both
# are kept, and the second is the one in front
ipc desktop new >/dev/null 2>&1
sleep 1
ipc desktop new >/dev/null 2>&1
sleep 1
if [ "$(names)" != "1 2" ]; then
	echo "two desktops were expected before naming one, got '$(names)'"
	exit 1
fi

# which of the two is in front decides what the names should read afterwards
focused=$(ipc -t get_workspaces | python3 -c '
import json, sys
print(next(w["num"] for w in json.load(sys.stdin) if w["focused"]))')
if [ "$focused" = "1" ]; then
	named="1:Work 2"
	moved="1 2:Work"
	direction=right
else
	named="1 2:Work"
	moved="1:Work 2"
	direction=left
fi

# ---------- a name can be given ----------
ipc desktop rename Work >/dev/null 2>&1
sleep 0.5
if [ "$(names)" != "$named" ]; then
	fail "naming the desktop in front gave '$(names)', wanted '$named'"
fi

# ---------- and the number still says where it is ----------
number=$(ipc -t get_workspaces | python3 -c '
import json, sys
print(max(w["num"] for w in json.load(sys.stdin) if w["name"].endswith(":Work")))')
if [ "$number" != "$focused" ]; then
	fail "the named desktop lost its number (got '$number', wanted '$focused')"
fi

# ---------- moving it takes the name along ----------
ipc desktop move $direction >/dev/null 2>&1
sleep 0.5
if [ "$(names)" != "$moved" ]; then
	fail "moving the named desktop gave '$(names)', wanted '$moved'"
fi

# ---------- and it can be taken away again ----------
ipc desktop rename >/dev/null 2>&1
sleep 0.5
if [ "$(names)" != "1 2" ]; then
	fail "clearing the name gave '$(names)', wanted '1 2'"
fi

# ---------- a workspace named by hand keeps the name it was given ----------
ipc workspace "scratch" >/dev/null 2>&1
sleep 1
if ipc desktop rename Renamed 2>&1 | grep -q '"success": true'; then
	fail "a workspace named by hand was renamed, and its name is how it is reached"
fi
if ! ipc -t get_workspaces | grep -q '"name": "scratch"'; then
	fail "the workspace named by hand lost its name"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "desktops can be named, and the name stays with the desktop"
exit 0
