#!/bin/sh
# The apps behind the windows, in a nested tileWin:
#  - an app that stops answering (stopped here with SIGSTOP) turns
#    "(Not Responding)" some seconds after its window is focused, and goes back
#    when it answers again; closing it while it hangs offers to end it (the
#    "not_responding" event the taskbar shows its dialog for), and end_task
#    ends it;
#  - a minimized app stops after pause_minimized, goes on when it is shown,
#    and is written down for a tileWin after a crash; a terminal and an app in
#    pause_minimized_except keep running;
#  - an app opens where its window was closed last (remember_windows);
#  - peek hides the other windows and shows the desktop, and peek off brings
#    them back;
#  - the magnifier zooms around the pointer, with Alt and the wheel as well as
#    with "magnify", past the screen's edge it is dark, and it goes back.
#
# usage: app_windows.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool
keyboard=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$pointer" "$keyboard"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session xfce4-terminal mousepad grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-apps-$$.sock
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
	[ -n "${events:-}" ] && kill "$events" 2>/dev/null
	for pid in ${stopped:-}; do
		kill -CONT "$pid" 2>/dev/null
	done
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
panel_command true
animations off
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

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

# a field of the window with that title (or app id): pid, name, not_responding, paused, rect
window() {
	ipc -t get_tree | python3 -c '
import json, sys
want, field = sys.argv[1], sys.argv[2]
def walk(n):
    if n.get("pid") and (n.get("name", "").startswith(want) or n.get("app_id") == want):
        v = n.get(field)
        if field == "rect":
            v = "%d %d %d %d" % (v["x"], v["y"], v["width"], v["height"])
        print(str(v).lower() if isinstance(v, bool) else v)
        sys.exit(0)
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(json.load(sys.stdin))
print("")' "$1" "$2"
}
wait_for() { # <title> <field> <value> <seconds>
	attempt=0
	while [ "$(window "$1" "$2")" != "$3" ] && [ $attempt -lt $(($4 * 5)) ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	[ "$(window "$1" "$2")" = "$3" ]
}
state_of() { awk '{ print $3 }' "/proc/$1/stat" 2>/dev/null; }
# red, green and blue of one pixel of the screen
pixel() { grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'; }
near() { # <r g b> <r g b> <tolerance>
	set -- $1 $2 $3
	d() { [ $(( $1 > $2 ? $1 - $2 : $2 - $1 )) -le $3 ]; }
	d "$1" "$4" "$7" && d "$2" "$5" "$7" && d "$3" "$6" "$7"
}
tilewin_state() { ipc -t get_tilewin | python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }

ipc exec "xfce4-terminal --disable-server --title=one" >/dev/null
ipc exec "xfce4-terminal --disable-server --title=two" >/dev/null
wait_for one visible true 20
wait_for two visible true 20
one=$(window one pid) two=$(window two pid)
if [ -z "$one" ] || [ -z "$two" ]; then
	echo "the terminals did not come up"
	exit 1
fi
sleep 1

# ---------- not responding ----------
ipc -t subscribe -m '["tilewin"]' > "$work/events" 2>&1 &
events=$!
sleep 0.3
kill -STOP "$one"
stopped=$one
sleep 2.2 # (an app is asked at most every two seconds; both were asked when they opened)
ipc '[title="^two"] focus' >/dev/null
ipc '[title="^one"] focus' >/dev/null # asks it whether it still answers
if wait_for one not_responding true 9; then
	case "$(window one name)" in
	*"(Not Responding)") ;;
	*) fail "the title of a hanging window does not say it is not responding" ;;
	esac
else
	fail "a window whose app hangs is not shown as not responding"
fi
kill -CONT "$one"
stopped=
wait_for one not_responding false 4 || fail "an app that answers again is still not responding"
[ "$(window one name)" = "one" ] || fail "the title does not come back: $(window one name)"

kill -STOP "$one"
stopped=$one
sleep 2.2
ipc '[title="^two"] focus' >/dev/null
ipc '[title="^one"] focus' >/dev/null
wait_for one not_responding true 9 || fail "a window that hangs again is not shown so"
ipc '[title="^one"] kill' >/dev/null
sleep 0.5
grep -q '"not_responding"' "$work/events" ||
	fail "closing a hanging window does not offer to end its app"
ipc '[title="^one"] end_task' >/dev/null || fail "end_task was refused"
sleep 1
if kill -0 "$one" 2>/dev/null && [ "$(state_of "$one")" != Z ]; then
	fail "end_task did not end the app"
fi
stopped=
grep -q '"responding"' "$work/events" || fail "the offer to end the app is not taken back"
kill "$events" 2>/dev/null
events=

# ---------- minimized apps stop ----------
ipc exec "mousepad --disable-server" >/dev/null
attempt=0
while [ -z "$(window org.xfce.mousepad pid)" ] && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
pad=$(window org.xfce.mousepad pid)
if [ -z "$pad" ]; then
	fail "mousepad did not come up"
else
	sleep 1
	ipc pause_minimized 2 >/dev/null
	ipc pause_minimized_sound pause >/dev/null # (the test session has no sound server)
	ipc '[app_id="org.xfce.mousepad"] minimize enable' >/dev/null
	ipc '[title="^two"] minimize enable' >/dev/null
	sleep 3.5
	[ "$(state_of "$pad")" = T ] || fail "a minimized app did not stop ($(state_of "$pad"))"
	[ "$(window org.xfce.mousepad paused)" = true ] || fail "the tree does not say it is paused"
	[ "$(state_of "$two")" != T ] || fail "a minimized terminal stopped"
	grep -q "^$pad " "$XDG_RUNTIME_DIR/tilewin-paused-$WAYLAND_DISPLAY" 2>/dev/null ||
		fail "the stopped app is not written down for after a crash"
	ipc '[app_id="org.xfce.mousepad"] minimize disable' >/dev/null
	sleep 0.5
	[ "$(state_of "$pad")" != T ] || fail "a shown app does not go on"
	[ -e "$XDG_RUNTIME_DIR/tilewin-paused-$WAYLAND_DISPLAY" ] &&
		fail "the list of stopped apps is left behind"
	ipc pause_minimized_except org.xfce.mousepad.desktop >/dev/null
	ipc '[app_id="org.xfce.mousepad"] minimize enable' >/dev/null
	sleep 3.5
	[ "$(state_of "$pad")" != T ] || fail "an app in pause_minimized_except stopped"
	ipc '[app_id="org.xfce.mousepad"] minimize disable' >/dev/null
	ipc '[title="^two"] minimize disable' >/dev/null
	ipc pause_minimized off >/dev/null

	# ---------- it opens where it was ----------
	ipc '[app_id="org.xfce.mousepad"] resize set 640 420' >/dev/null
	ipc '[app_id="org.xfce.mousepad"] move position 150 120' >/dev/null
	sleep 0.8
	before=$(window org.xfce.mousepad rect)
	ipc '[app_id="org.xfce.mousepad"] kill' >/dev/null
	attempt=0
	while kill -0 "$pad" 2>/dev/null && [ $attempt -lt 25 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	grep -q "^org.xfce.mousepad	" "$XDG_STATE_HOME/tileWin/window-places" 2>/dev/null ||
		fail "the place of a closed window is not kept"
	ipc exec "mousepad --disable-server" >/dev/null
	attempt=0
	while [ -z "$(window org.xfce.mousepad pid)" ] && [ $attempt -lt 50 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	sleep 0.8
	after=$(window org.xfce.mousepad rect)
	[ "$before" = "$after" ] ||
		fail "the app does not open where it was closed: $before, now $after"
	ipc '[app_id="org.xfce.mousepad"] kill' >/dev/null
	sleep 0.5
fi

# ---------- peek ----------
ipc '[title="^one"] kill' >/dev/null 2>&1
ipc exec "xfce4-terminal --disable-server --title=three" >/dev/null
wait_for three visible true 20
for w in two three; do
	ipc "[title=\"^$w\"] resize set 400 300" >/dev/null
done
ipc '[title="^two"] move position 20 20' >/dev/null
ipc '[title="^three"] move position 700 300' >/dev/null
sleep 0.8
wall=$(pixel 1200 50)
in_two=$(pixel 220 200)
in_three=$(pixel 900 450)
near "$in_two" "$wall" 12 && fail "the test cannot tell a window from the desktop"
ipc "peek $(window three id)" >/dev/null || fail "peek at a window was refused"
sleep 0.4
[ "$(tilewin_state peek)" = True ] || fail "peek is not on"
near "$(pixel 220 200)" "$wall" 30 || fail "peeking at a window still shows the other one"
near "$(pixel 900 450)" "$in_three" 12 || fail "peeking at a window does not show it"
ipc peek desktop >/dev/null
sleep 0.4
near "$(pixel 900 450)" "$wall" 30 || fail "peeking at the desktop still shows a window"
ipc peek off >/dev/null
sleep 0.4
near "$(pixel 220 200)" "$in_two" 12 || fail "the windows do not come back after peeking"

# ---------- the magnifier ----------
"$pointer" 1280 720 move 5 5 >/dev/null 2>&1
ipc magnify 4 >/dev/null
sleep 0.8
near "$(pixel 200 120)" "5 5 8" 6 || fail "zoomed at the corner, the part past the screen is not dark"
ipc magnify off >/dev/null
sleep 0.8
[ "$(tilewin_state magnify)" = 1.0 ] || fail "magnify off does not end zooming"
near "$(pixel 1200 50)" "$wall" 12 || fail "the screen does not come back after zooming"
"$keyboard" hold alt 2500 >/dev/null 2>&1 &
held=$!
sleep 1
"$pointer" 1280 720 scroll 640 360 -4 >/dev/null 2>&1
wait "$held"
sleep 0.5
level=$(tilewin_state magnify)
python3 -c "import sys; sys.exit(0 if float('$level') > 2 else 1)" ||
	fail "Alt and the wheel do not zoom (level $level)"
"$pointer" 1280 720 scroll 640 360 3 >/dev/null 2>&1
sleep 0.5
[ "$(tilewin_state magnify)" != 1.0 ] ||
	fail "the wheel without Alt changed the zoom"
ipc magnify off >/dev/null
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	tail -n 20 "$work/log"
	exit 1
fi
echo "all checks passed"
