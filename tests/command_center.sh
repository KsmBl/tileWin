#!/bin/bash
# The Command Center of modernCC: "panel command" opens it; the title of a window
# typed and Enter switches to that window; an action (">dark") is done; Alt+1 on
# a window closes it; and what was used is kept for the next time.
#
# usage: command_center.sh <build dir> <source dir>
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
if ! python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' 2>/dev/null; then
	echo "no GTK 3 for Python to open windows with: skipping"
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
apps=()
cleanup() {
	for a in "${apps[@]}"; do kill "$a" 2>/dev/null; done
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/window.py" <<'EOT'
import sys, gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk
w = Gtk.Window(title=sys.argv[1])
w.set_default_size(300, 200)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
EOT
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
ipc theme moderncc >/dev/null 2>&1

focused() { # the title of the focused window
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
    if n.get("focused") and n.get("pid"):
        print(n.get("name")); return True
    return any(walk(c) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin))'
}
has_window() { ipc -t get_tree | grep -q "\"name\": \"$1\""; }
GDK_BACKEND=wayland python3 "$work/window.py" "Alpha notes" & apps+=($!)
wait_until 10 has_window "Alpha notes" || fail "the first window did not open"
GDK_BACKEND=wayland python3 "$work/window.py" "Beta draft" & apps+=($!)
wait_until 10 has_window "Beta draft" || fail "the second window did not open"
sleep 1

screen() { grim -g "300,100 680x500" - 2>/dev/null | md5sum; }
closed=$(screen)
ipc panel command >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(grim -g '300,100 680x500' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "panel command opened nothing"

# a window, by its title
"$keys" text "alpha" >/dev/null 2>&1
sleep 0.5
"$keys" key none return >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$($msg -s $sock -t get_tree | python3 -c '
import json, sys
def walk(n):
    if n.get(\"focused\") and n.get(\"pid\"):
        print(n.get(\"name\")); return True
    return any(walk(c) for c in n.get(\"nodes\", []) + n.get(\"floating_nodes\", []))
walk(json.load(sys.stdin))')\" = 'Alpha notes' ]" || fail "Enter did not switch to the window typed ($(focused))"

# an action, with the sign of the actions
scheme_before=$(cat "$XDG_CONFIG_HOME/tileWin/color-scheme" 2>/dev/null || echo light)
ipc panel command >/dev/null 2>&1
sleep 0.8
# ">": the sign of the actions (the tool types letters; > is Shift and the full stop)
"$keys" key shift period >/dev/null 2>&1
"$keys" text "dark" >/dev/null 2>&1
sleep 0.5
"$keys" key none return >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(cat '$XDG_CONFIG_HOME/tileWin/color-scheme' 2>/dev/null || echo light)\" != '$scheme_before' ]" ||
	fail "the action did not switch the colour scheme"

# Alt+1 on a window: Close
ipc panel command >/dev/null 2>&1
sleep 0.8
"$keys" text "beta" >/dev/null 2>&1
sleep 0.5
"$keys" key alt 1 >/dev/null 2>&1
wait_until 8 sh -c "! $msg -s $sock -t get_tree | grep -q '\"name\": \"Beta draft\"'" ||
	fail "Alt+1 did not close the window"

# what was used is kept
grep -q "act:dark" "$XDG_STATE_HOME/tileWin/command-center" 2>/dev/null ||
	fail "the history did not keep the action"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
