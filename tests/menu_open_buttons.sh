#!/bin/sh
# The title bar buttons of a window whose menu is open, in a nested tileWin
# with a GTK 3 app with a menu bar (tests/menu_app.py, as Thunar has):
#  - with the Edit menu open, the X closes the window: the menu goes on the
#    press, as GTK takes the close of no window but the menu's while it is open;
#  - with it open, Maximize maximizes the window and the menu goes, so the X
#    then closes it.
#
# usage: menu_open_buttons.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' 2>/dev/null; then
	echo "GTK 3 for Python is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-menubtn-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
app=
cleanup() {
	[ -n "$app" ] && kill "$app" 2>/dev/null
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command true
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }

# where the window is: the content's x y, its title bar's x y width height,
# and whether it is maximized
window() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "Menuapp":
			r, d = c["rect"], c["deco_rect"]
			print(r["x"], r["y"], d["x"], d["y"], d["width"], d["height"],
				int(bool(c.get("maximized"))))
		walk(c)
walk(json.load(sys.stdin))'
}

# starts the app; the places of Edit (the second item of the menu bar) and of
# the buttons (46 wide, from the right) in edit_x ..., from window()
start_app() {
	: > "$work/out"
	GDK_BACKEND=wayland python3 "$source_dir/tests/menu_app.py" Menuapp > "$work/out" 2>&1 &
	app=$!
	attempt=0
	while ! grep -q ready "$work/out" 2>/dev/null && [ $attempt -lt 50 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	sleep 1
	places || { echo "the window did not open"; cat "$work/out"; exit 1; }
}
places() {
	set -- $(window)
	[ $# -eq 7 ] || return 1
	edit_x=$(($1 + 53)) edit_y=$(($2 + 13))
	max_x=$(($3 + $5 - 69)) close_x=$(($3 + $5 - 23)) button_y=$(($4 + $6 / 2))
	maximized=$7
}

# ---- the X with the Edit menu open
start_app
"$pointer" 1280 720 click "$edit_x" "$edit_y" wait 600 >/dev/null 2>&1
"$pointer" 1280 720 move "$close_x" "$button_y" wait 200 click "$close_x" "$button_y" wait 800 >/dev/null 2>&1
grep -q closed "$work/out" || fail "the X did nothing while the menu was open"
[ -z "$(window)" ] || fail "the window is still there: $(window)"
kill "$app" 2>/dev/null
sleep 0.5

# ---- Maximize with the Edit menu open
start_app
"$pointer" 1280 720 click "$edit_x" "$edit_y" wait 600 >/dev/null 2>&1
"$pointer" 1280 720 move "$max_x" "$button_y" wait 200 click "$max_x" "$button_y" wait 600 >/dev/null 2>&1
places
[ "$maximized" = 1 ] || fail "Maximize with the menu open: $(window)"
# and its menu went: the X works at once
"$pointer" 1280 720 move "$close_x" "$button_y" wait 200 click "$close_x" "$button_y" wait 800 >/dev/null 2>&1
grep -q closed "$work/out" || fail "the menu stayed open after Maximize"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
