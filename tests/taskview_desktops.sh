#!/bin/sh
# The desktops of the task view (Win+Tab), in a nested tileWin:
#  - a desktop dragged along the strip lands where it is dropped, with its
#    windows, and a name stays with the desktop it was given to;
#  - a click on the name of a desktop renames it: typing replaces the name,
#    Return keeps it, Escape leaves the old one.
#
# usage: taskview_desktops.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$pointer" "$keys"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session xfce4-terminal python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-desks-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
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
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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
# "<workspace>=<window>" for every window, sorted
windows() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, ws, out):
	if n.get("type") == "workspace":
		ws = n["name"]
	if n.get("pid") and n.get("name"):
		out.append(ws + "=" + n["name"])
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		walk(c, ws, out)
out = []
walk(json.load(sys.stdin), None, out)
print(" ".join(sorted(out)))'
}

for n in One Two Three; do
	case $n in One) ws=1 ;; Two) ws=2 ;; Three) ws=3 ;; esac
	ipc workspace $ws >/dev/null
	ipc exec "xfce4-terminal --disable-server -T $n" >/dev/null
	sleep 2
done
ipc workspace 1 >/dev/null
[ "$(windows)" = "1=One 2=Two 3=Three" ] || fail "the desktops did not come up ($(windows))"

# the strip of a 1280x720 screen: desktops 1 to 3 at x 304, 528 and 752,
# "New desktop" at 976, their names at y 686
ipc taskview >/dev/null
sleep 1
"$pointer" 1280 720 drag 304 615 790 615 >/dev/null 2>&1
sleep 0.8
[ "$(windows)" = "1=Two 2=Three 3=One" ] ||
	fail "the first desktop dragged to the end did not land there ($(windows))"

"$pointer" 1280 720 click 528 686 >/dev/null 2>&1
sleep 0.4
"$keys" text mail key none return >/dev/null 2>&1
sleep 0.6
[ "$(windows)" = "1=Two 2:mail=Three 3=One" ] ||
	fail "clicking the name and typing did not rename the desktop ($(windows))"

"$pointer" 1280 720 click 304 686 >/dev/null 2>&1
sleep 0.4
"$keys" text junk key none escape >/dev/null 2>&1
sleep 0.6
[ "$(windows)" = "1=Two 2:mail=Three 3=One" ] || fail "Escape did not keep the old name ($(windows))"
# the name goes along with its desktop
"$pointer" 1280 720 drag 528 615 250 615 >/dev/null 2>&1
sleep 0.8
[ "$(windows)" = "1:mail=Three 2=Two 3=One" ] ||
	fail "the renamed desktop did not keep its name when dragged ($(windows))"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
