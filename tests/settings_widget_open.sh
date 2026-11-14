#!/bin/sh
# "tilewin-settings --widget NAME --desktop", what the Settings button of a
# widget's flyout runs: the settings open with the dialog of that widget, for
# the desktop, on top of the window, and it stays on top when the window is
# clicked (focused): a window raised keeps its dialogs above it.
#
# usage: settings_widget_open.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings

for needed in "$compositor" "$msg" "$settings"; do
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
export GIO_USE_VFS=local
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
panel_command true
output * mode --custom 1280x800
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec "$settings" --widget clock --desktop
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

# the floating windows, bottom first
stack() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	if n.get("type") == "workspace" and n.get("floating_nodes"):
		print(" | ".join(c.get("name") or "" for c in n["floating_nodes"]))
	for c in n.get("nodes", []):
		walk(c)
walk(json.load(sys.stdin))'
}
wait_until 20 sh -c "'$msg' -s '$sock' -t get_tree | grep -q 'Clock settings'" ||
	fail "the dialog of the clock did not open"
sleep 1 # what comes after the dialog: nothing may cover it
[ "$(stack)" = "tileWin Settings | Clock settings" ] ||
	fail "the dialog is not on top of the settings: $(stack)"
ipc '[title="^tileWin Settings$"] focus' >/dev/null
sleep 0.5
[ "$(stack)" = "tileWin Settings | Clock settings" ] ||
	fail "focusing the settings put them over their dialog: $(stack)"
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
