#!/bin/sh
# "tilewinmsg restart" should keep the windows open. Wayland gives a client no
# way to survive the compositor it talks to, so the only restart that can keep
# them is one where the process stays: tileWin therefore only ends the session
# when the compositor binary itself has been replaced.
#
# The test opens a window, restarts, and checks that the same window with the
# same process is still there; then it replaces the binary on disk and checks
# that a restart does end the session, which is what picks up an update.
#
# usage: restart_keeps_windows.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
msg=$build/swaymsg/tilewinmsg

if [ ! -x "$build/sway/tilewin" ] || [ ! -x "$msg" ]; then
	echo "tileWin was not built: skipping"
	exit 77
fi
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

work=$(mktemp -d) || exit 77
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

# a copy of its own, so that replacing it cannot disturb the build directory
cp "$build/sway/tilewin" "$work/tilewin"
sock=$work/ipc.sock
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$sock" \
	dbus-run-session -- "$work/tilewin" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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

compositor_pid() {
	for p in $(pgrep -f "tilewin -c $work/tilewin.conf"); do
		[ "$(cat /proc/$p/comm 2>/dev/null)" = tilewin ] && echo "$p" && return
	done
}
panel_pid() {
	pgrep -P "$(compositor_pid)" -x tilewin-panel 2>/dev/null | head -n 1
}
window_pid() {
	ipc -t get_tree 2>/dev/null | python3 -c '
import json, sys
def walk(n):
    if isinstance(n, dict):
        if n.get("app_id") == "xfce4-terminal":
            print(n.get("pid") or "")
            return True
        for c in n.get("nodes", []) + n.get("floating_nodes", []):
            if walk(c):
                return True
    return False
walk(json.load(sys.stdin))'
}

ipc exec "xfce4-terminal --disable-server" >/dev/null 2>&1
attempt=0
while [ -z "$(window_pid)" ] && [ $attempt -lt 40 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
before_window=$(window_pid)
before_compositor=$(compositor_pid)
before_panel=$(panel_pid)
if [ -z "$before_window" ]; then
	echo "no window came up to keep"
	exit 1
fi

# ---------- a restart with nothing replaced keeps the window ----------
ipc restart >/dev/null 2>&1
sleep 4
after_window=$(window_pid)
after_compositor=$(compositor_pid)
if [ "$after_compositor" != "$before_compositor" ]; then
	fail "restart ended the session although nothing had been replaced"
elif [ -z "$after_window" ]; then
	fail "the window is gone after a restart"
elif [ "$after_window" != "$before_window" ]; then
	fail "the window came back as a new process ($before_window -> $after_window)"
else
	echo "restart kept window $after_window open, compositor $after_compositor stayed"
fi

# the taskbar is still started afresh, so an updated one is picked up
if [ -n "$before_panel" ] && [ "$(panel_pid)" = "$before_panel" ]; then
	fail "the taskbar was not started again, so an updated one would not be picked up"
fi

# ---------- a restart after the binary changed does end the session ----------
# tilewin-session is what starts it again; here it only has to stop with the
# code that asks for that.
touch "$work/tilewin"
sleep 1
ipc restart >/dev/null 2>&1
sleep 4
if [ -n "$(compositor_pid)" ]; then
	fail "restart kept running although the binary on disk had changed"
else
	echo "restart after a new binary ended the session, as it must"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "restart keeps the windows whenever it can"
exit 0
