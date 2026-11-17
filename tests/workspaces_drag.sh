#!/bin/bash
# The workspaces widget on the taskbar (panel/widgets/workspaces.c): a window
# is dragged from one desktop's small screen to another's and goes to that
# desktop, as on KDE; a press without moving is still a click.
#
# usage: workspaces_drag.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' \
		2>/dev/null; then
	echo "python3 with GTK 3 is not installed: skipping"
	exit 77
fi
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications"

sed -e "s/^\tleft start search taskbar/\tleft start workspaces taskbar/" \
	"$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
echo "theme_layout no" >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat > "$work/win.py" <<'PY'
import gi, sys
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title=sys.argv[1])
w.set_default_size(300, 200)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
PY

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	pkill -f "$work/win.py" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
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
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc mode window >/dev/null 2>&1
ipc theme win10 >/dev/null 2>&1

# the desktop a window of that title is on
desk_of() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, ws=None):
    if n.get("type") == "workspace":
        ws = n["name"]
    if n.get("name") == sys.argv[1]:
        print(ws)
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c, ws)
walk(json.load(sys.stdin))' "$1"
}
ipc workspace 2 >/dev/null 2>&1
python3 "$work/win.py" Other >/dev/null 2>&1 &
wait_until 10 sh -c "true" && sleep 2
ipc workspace 1 >/dev/null 2>&1
python3 "$work/win.py" Draggable >/dev/null 2>&1 &
sleep 2.5
[ "$(desk_of Draggable)" = 1 ] || fail "the window did not open on desktop 1: $(desk_of Draggable)"

# the window's outline on desktop 1's small screen, carried to desktop 2's
"$pointer" 1280 720 down 79 697 move 95 697 move 115 697 move 141 699 wait 300 up \
	>/dev/null 2>&1
moved() { [ "$(desk_of Draggable)" = 2 ]; }
wait_until 5 moved || fail "the dragged window did not go to desktop 2: $(desk_of Draggable)"
[ "$(desk_of Other)" = 2 ] || fail "the other window moved too"

# a press without moving is a click: it goes to the desktop
ipc workspace 1 >/dev/null 2>&1
python3 "$work/win.py" Third >/dev/null 2>&1 &
sleep 2
"$pointer" 1280 720 click 141 697 >/dev/null 2>&1
focused2() { ipc -t get_workspaces | grep -q '"name": "2",[^}]*"focused": true' ||
	ipc -t get_workspaces | python3 -c 'import json,sys; sys.exit(0 if any(w["focused"] and w["name"]=="2" for w in json.load(sys.stdin)) else 1)'; }
wait_until 5 focused2 || fail "a click on desktop 2 did not go there"
[ "$(desk_of Third)" = 1 ] || fail "a click moved a window"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
