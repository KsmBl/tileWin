#!/bin/sh
# An X11 app with a title bar of its own (tests/x11_titlebar.py, which acts as
# Bambu Studio does), in a nested tileWin with Xwayland:
#  - a configure request with a new position moves the window, and one that
#    only moves it does not pull it back onto the screen;
#  - dragged by its own title bar, it follows the pointer exactly (the pointer
#    is sent relative to where the window is now, not where it was pressed);
#  - maximize (_NET_WM_STATE) maximizes it, and its _NET_WM_STATE says so;
#  - the restore button (only an activation, as wxWidgets' Restore() on GTK)
#    right after a click in its top strip gives the size back; the same
#    activation without a click there leaves it maximized;
#  - minimize (WM_CHANGE_STATE) minimizes it.
#
# usage: x11_titlebar.sh <build dir> <source dir>
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
for needed in dbus-run-session Xwayland python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import Xlib' 2>/dev/null; then
	echo "python-xlib is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-x11bar-$$.sock
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
	exec 3>&-
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
exec sh -c 'printf %s "\$DISPLAY" > $work/xdisplay; printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
if [ ! -s "$work/display" ] || [ ! -s "$work/xdisplay" ]; then
	echo "the nested compositor (with Xwayland) never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display") DISPLAY=$(cat "$work/xdisplay")
ipc() { "$msg" -s "$sock" "$@"; }

mkfifo "$work/in"
python3 "$source_dir/tests/x11_titlebar.py" < "$work/in" > "$work/out" 2>&1 &
app=$!
exec 3> "$work/in"
attempt=0
while ! grep -q ready "$work/out" 2>/dev/null && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
sleep 1

# x y width height maximized minimized, from tileWin
window() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("window_properties", {}).get("class") == "X11titlebar":
			r = c["rect"]
			print(r["x"], r["y"], r["width"], r["height"], int(bool(c.get("maximized"))),
				int(bool(c.get("minimized"))))
		walk(c)
walk(json.load(sys.stdin))'
}
# what the app itself sees: x y width height states
app_state() {
	echo state >&3
	sleep 0.3
	tail -n 1 "$work/out"
}
send() {
	echo "$*" >&3
	sleep 0.6
}

[ -n "$(window)" ] || { echo "the X11 window did not open"; tail -n 5 "$work/out"; exit 1; }

send move 300 200
[ "$(window)" = "300 200 400 300 0 0" ] || fail "moving itself: $(window), wanted 300 200"
send move 1100 600
[ "$(window)" = "1100 600 400 300 0 0" ] ||
	fail "moving itself partly off the screen: $(window), wanted 1100 600"

# dragged by its own title bar, by 200,150
send move 300 200
"$pointer" 1280 720 move 500 210 wait 300 drag 500 210 700 360 >/dev/null 2>&1
sleep 0.5
[ "$(window)" = "500 350 400 300 0 0" ] || fail "dragged by its title bar: $(window), wanted 500 350"

send maximize
[ "$(window)" = "0 0 1280 720 1 0" ] || fail "maximize: $(window)"
case $(app_state) in
*MAXIMIZED_VERT,MAXIMIZED_HORZ*) ;;
*) fail "the app is not told it is maximized: $(app_state)" ;;
esac

# an activation with no click just before: stays maximized
sleep 0.6
send present
[ "$(window)" = "0 0 1280 720 1 0" ] || fail "an activation alone restored the window: $(window)"
# a press in the top strip, then the activation: the restore button (which
# wxWidgets works on the press)
"$pointer" 1280 720 move 1200 12 wait 300 down 1200 12 wait 1500 up >/dev/null 2>&1 &
held=$!
sleep 1.3
send present
wait "$held"
[ "$(window)" = "500 350 400 300 0 0" ] || fail "the restore button: $(window), wanted 500 350"
case $(app_state) in
*MAXIMIZED*) fail "the app is still told it is maximized: $(app_state)" ;;
esac

send minimize
[ "$(window)" = "500 350 400 300 0 1" ] || fail "minimize: $(window)"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
