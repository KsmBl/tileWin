#!/bin/sh
# A window opened by a shortcut while the start menu is open closes the start
# menu, as on Windows: in a nested tileWin with a binding of its own
# (Ctrl+Alt+T opens a window of one color), the start menu is opened, the keys
# are pressed, and the corner the start menu covered shows the desktop again,
# with the new window focused.
#
# usage: startmenu_closes.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$keys"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk' \
		2>/dev/null; then
	echo "python3 has no GTK 4: skipping"
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
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
bindsym Ctrl+Mod1+t exec python3 $source_dir/tests/color_window.py Shortcut '#d02020'
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
# the corner of the start menu, away from the windows in the middle
corner() { grim -g "0,420 300x250" - 2>/dev/null | md5sum; }
wait_still 10 screen
desktop=$(corner)
focused_is() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	if n.get("focused") and n.get("name") == sys.argv[1]:
		sys.exit(0)
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		walk(c)
walk(json.load(sys.stdin))
sys.exit(1)' "$1"
}

# <menu> <keys> <window>
check() {
	ipc panel "$1" >/dev/null
	wait_still 5 screen
	[ "$(corner)" != "$desktop" ] || { fail "the $1 did not open"; return; }
	"$keys" key "$2" t >/dev/null 2>&1
	wait_until 15 focused_is "$3" || fail "the window of the shortcut did not get the focus ($1)"
	wait_still 5 screen
	[ "$(corner)" = "$desktop" ] || fail "the $1 stayed open when a window opened"
}
check startmenu ctrl-alt Shortcut
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
