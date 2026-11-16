#!/bin/bash
# The password dialog of tilewin-polkit when the only screen goes and comes
# back, as on a switch to another console (Ctrl+Alt+F3) and back: the dialog
# is on the screen that came, not only the darkening, and it has the keyboard
# (Escape answers No). Uses --preview, so it needs no polkit.
#
# usage: polkit_screen_back.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
agent=$build/polkit/tilewin-polkit
keys=$build/tests/keyboard-tool
count=$build/tests/count-content

for needed in "$compositor" "$msg" "$agent" "$keys" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
agent_pid=
cleanup() {
	[ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
wallpaper solid #204070
session_restore no
animations off
polkit_agent disable
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOT
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display") SWAYSOCK=$sock TILEWINSOCK=$sock

"$agent" --preview > "$work/agent.log" 2>&1 &
agent_pid=$!
sleep 2
# the only screen goes, and a new one comes
ipc output HEADLESS-1 unplug >/dev/null 2>&1
sleep 1
ipc create_output >/dev/null 2>&1
sleep 2
screen=$(ipc -t get_outputs | grep -m1 '"name"' | cut -d'"' -f4)
grim -o "$screen" "$work/back.png"
size=$(ipc -t get_outputs | grep -m1 -A3 '"current_mode"' | grep -o '[0-9]\+' | head -2 | tr '\n' ' ')
set -- $size
w=${1:-1920} h=${2:-1080}
dialog=$("$count" "$work/back.png" $((w / 2 - 200)) $((h / 2 - 150)) 400 300)
echo "dialog pixels in the middle of $screen: $dialog"
[ "${dialog:-0}" -gt 10000 ] || fail "the screen that came back is dark, without the dialog"

"$keys" key none escape >/dev/null 2>&1
wait_until 5 sh -c "! kill -0 $agent_pid 2>/dev/null" || fail "Escape did not reach the dialog"
wait "$agent_pid" 2>/dev/null
status=$?
agent_pid=
[ "$status" -eq 1 ] || fail "the dialog did not answer No (exit $status)"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
