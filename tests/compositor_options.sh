#!/bin/sh
# An option of the compositor that is easy to get wrong and hard to see:
#
#  - $taskmanager naming a program nobody has installed must fall back to one
#    that is there, or the taskbar menu and Ctrl+Shift+Escape open nothing.
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

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the compositor options behave"
exit 0
