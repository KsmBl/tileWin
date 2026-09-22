#!/bin/sh
# Two options of the compositor that are easy to get wrong and hard to see:
#
#  - $taskmanager naming a program nobody has installed must fall back to one
#    that is there, or the taskbar menu and Ctrl+Shift+Escape open nothing.
#  - "expensive_calculations on" must lay a window out again on every frame
#    while it changes size, so the tree shows sizes on the way instead of the
#    window jumping straight to the end.
#
# usage: compositor_options.sh <build dir> <source dir>
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

# A task manager of our own that only says it ran, so the test never opens the
# real one, and a $taskmanager that is deliberately not installed.
mkdir -p "$work/bin"
cat > "$work/bin/xfce4-taskmanager" <<EOF
#!/bin/sh
echo ran > "$work/taskmanager-ran"
EOF
chmod +x "$work/bin/xfce4-taskmanager"
PATH=$work/bin:$PATH
export PATH

sock=$work/ipc.sock
cat > "$work/tilewin.conf" <<EOF
set \$taskmanager tilewin-no-such-task-manager
wallpaper solid #204070
session_restore no
animations enable
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/compositor.log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/compositor.log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
# TILEWINSOCK of the session running the test would win over SWAYSOCK, so the
# socket of the nested compositor is named outright
ipc() { "$msg" -s "$sock" "$@"; }

# ---------- $taskmanager falls back to one that is installed ----------
# The compositor puts the value in before it runs the command, so echoing it
# says what the fallback chose without the program having to be started.
ipc exec "sh -c 'echo \$taskmanager > $work/chosen'" >/dev/null 2>&1
sleep 1.5
chosen=$(cat "$work/chosen" 2>/dev/null)
if [ -z "$chosen" ]; then
	fail "\$taskmanager could not be read back"
elif [ "$chosen" = "tilewin-no-such-task-manager" ]; then
	fail "\$taskmanager was left pointing at a program that is not installed"
else
	program=$(echo "$chosen" | cut -d' ' -f1)
	if ! command -v "$program" >/dev/null 2>&1; then
		fail "\$taskmanager fell back to '$chosen', and '$program' is not installed either"
	else
		echo "\$taskmanager fell back to '$chosen'"
	fi
fi

# ---------- expensive_calculations lays the window out every frame ----------
# With it on the window is given a new size every frame, so the tree shows sizes
# on the way; with it off it jumps straight to the end.
if command -v xfce4-terminal >/dev/null 2>&1; then
	ipc exec "xfce4-terminal --disable-server" >/dev/null 2>&1
	attempt=0
	while [ $attempt -lt 40 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
		ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"' && break
	done
	if ! ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"'; then
		fail "no window came up to resize"
	else
		ipc floating enable >/dev/null 2>&1

		width_of() {
			ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
    if isinstance(n, dict):
        if n.get("app_id") == "xfce4-terminal":
            print(n["rect"]["width"])
            return True
        for c in n.get("nodes", []) + n.get("floating_nodes", []):
            if walk(c):
                return True
    return False
walk(json.load(sys.stdin))'
		}

		# The animation runs when a window is snapped or maximized, not on a
		# plain resize, and it is slowed right down so that reading the tree
		# over the socket is quick enough to see the steps.
		ipc animation_speed 0.15 >/dev/null 2>&1
		count_steps() {
			: > "$work/widths"
			ipc maximize disable >/dev/null 2>&1
			sleep 1
			ipc resize set 400 300 >/dev/null 2>&1
			sleep 1
			( i=0; while [ $i -lt 25 ]; do width_of >> "$work/widths"; i=$((i + 1)); done ) &
			sampler=$!
			ipc maximize enable >/dev/null 2>&1
			wait $sampler 2>/dev/null
			sleep 0.5
			sort -u "$work/widths" | wc -l
		}

		ipc expensive_calculations on >/dev/null 2>&1
		sleep 0.3
		on_count=$(count_steps)
		ipc expensive_calculations off >/dev/null 2>&1
		sleep 0.3
		off_count=$(count_steps)
		echo "different widths while maximizing: $on_count with the option on, $off_count with it off"
		if [ "$on_count" -lt 3 ]; then
			fail "with expensive_calculations on the window did not pass through sizes on the way"
		fi
		if [ "$off_count" -ge "$on_count" ]; then
			fail "with the option off the window passed through as many sizes ($off_count)"
		fi
	fi
else
	echo "xfce4-terminal is not installed: the resize half is skipped"
fi

# a value that is not yes or no must be refused rather than quietly taken
if ipc expensive_calculations 2>&1 | grep -q '"success": true'; then
	fail "expensive_calculations with no argument was accepted"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the compositor options behave"
exit 0
