#!/bin/bash
# App popups (sway/wm/appopup.c): "popup toggle <name>" of popups.conf, bound
# to a key, starts the app and shows its window at its place and size over a
# darkened screen, sliding in; the key again hides it, it comes back without starting
# again, a click on the darkened rest hides it, and a window of it that was
# open already is taken in.
#
# usage: app_popup.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keyboard=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$pointer" "$keyboard"; do
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

# a pretend mail app: a window with an app_id of its own, which counts its starts
cat > "$work/mailapp.py" <<'PY'
import gi, sys
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib
GLib.set_prgname(sys.argv[1])
with open(sys.argv[2], "a") as f:
    f.write("started\n")
w = Gtk.Window(title=sys.argv[1])
w.set_default_size(300, 200)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
PY
cat > "$XDG_DATA_HOME/applications/tw-test-mail.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=Test Mail
Exec=python3 $work/mailapp.py twtestmail $work/starts
StartupWMClass=twtestmail
EOT
cat > "$XDG_DATA_HOME/applications/tw-test-notes.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=Test Notes
Exec=python3 $work/mailapp.py twtestnotes $work/notes-starts
StartupWMClass=twtestnotes
EOT
cat > "$XDG_CONFIG_HOME/tileWin/popups.conf" <<EOT
popup mail {
	app tw-test-mail.desktop
	position right
	width 70%
	height 100%
	dim 50
}
popup notes {
	app tw-test-notes.desktop
	position top
	width 50%
	height 40%
	dim 0
	animation none
}
EOT
for mode in windowmode tilemode; do
	cp "$source_dir/config/$mode.conf" "$XDG_CONFIG_HOME/tileWin/$mode.conf"
	echo 'bindsym $mod+Shift+m popup toggle mail' >> "$XDG_CONFIG_HOME/tileWin/$mode.conf"
done

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	pkill -f "$work/mailapp.py" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
include $XDG_CONFIG_HOME/tileWin/windowmode.conf
wallpaper solid #204070
session_restore no
animations on
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
sleep 1

# "x y width height focused visible" of the window of that app_id, if there is one
window() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, scratch):
    scratch = scratch or n.get("name") == "__i3_scratch"
    if n.get("app_id") == sys.argv[1]:
        r = n["rect"]
        print(r["x"], r["y"], r["width"], r["height"], n.get("focused"),
              n.get("visible") and not scratch)
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c, scratch)
walk(json.load(sys.stdin), False)' "$1"
}
shown() { window twtestmail | grep -q "True True$"; }
hidden() { window twtestmail | grep -q "False$"; }
darkened() {
	# the corner far from the popup: the wallpaper #204070 at half
	local px
	px=$(grim -g "20,20 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ')
	[ "$px" = " 16 32 56" ] || [ "$px" = " 16 32 57" ] || [ "$px" = " 17 32 56" ]
}

# the shortcut starts it, and it shows at the right, 70% wide and as high as the work area
"$keyboard" key super+shift m >/dev/null 2>&1
wait_until 15 shown || fail "the shortcut did not start and show the app"
read -r x y w h _ <<< "$(window twtestmail)"
[ "${x:-0}" -gt 300 ] && [ "${w:-0}" -gt 800 ] && [ "${w:-0}" -lt 920 ] ||
	fail "it is not 70% wide at the right: $x $y $w $h"
[ "${h:-0}" -gt 550 ] || fail "it is not as high as the screen: $h"
wait_until 3 darkened || fail "the rest of the screen is not darkened"

# the shortcut again hides it, and once more it comes back without starting again
"$keyboard" key super+shift m >/dev/null 2>&1
wait_until 5 hidden || fail "the shortcut did not hide it"
darkened && fail "the screen stayed dark after hiding"
"$keyboard" key super+shift m >/dev/null 2>&1
wait_until 5 shown || fail "the shortcut did not bring it back"
[ "$(wc -l < "$work/starts")" -eq 1 ] || fail "it was started again"

# a click on the darkened rest hides it
"$pointer" 1280 720 click 100 300 >/dev/null 2>&1
wait_until 5 hidden || fail "a click beside it did not hide it"

# a window that is open already is taken in, at its own place and size; no sheet with dim 0
python3 "$work/mailapp.py" twtestnotes "$work/notes-starts" >/dev/null 2>&1 &
wait_until 10 sh -c "[ -s '$work/notes-starts' ]"
sleep 1
ipc popup toggle notes >/dev/null 2>&1
notes_shown() { window twtestnotes | grep -q "True True$"; }
wait_until 5 notes_shown || fail "the open window was not shown as the popup"
read -r x y w h _ <<< "$(window twtestnotes)"
[ "${y:-99}" -lt 60 ] && [ "${w:-0}" -gt 580 ] && [ "${w:-0}" -lt 700 ] ||
	fail "notes is not 50% wide at the top: $x $y $w $h"
[ "$(wc -l < "$work/notes-starts")" -eq 1 ] || fail "notes was started although it ran"
darkened && fail "dim 0 darkened the screen"

ipc popup toggle nothing 2>&1 | grep -q '"success": false' ||
	fail "an unknown popup did not fail"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
