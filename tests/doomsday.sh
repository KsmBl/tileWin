#!/bin/sh
# Doomsday in a nested tileWin, over a real window:
#  - tilewin-screensaver --list shows Doomsday, not its kinds;
#  - as a Blizzard, the teal wallpaper turns to dark, cold light at once, snow piles
#    up on the title bar of the window, and frost grows over its glass;
#  - as Hellfire it covers the desktop too, and "name thunderstorm", what the
#    Thunderstorm was called on its own, still starts it;
#  - moving the mouse ends each of them.
#
# usage: doomsday.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
saver=$build/screensaver/tilewin-screensaver
msg=$build/swaymsg/tilewinmsg
pointer=$build/tests/pointer-tool
count=$build/tests/count-content

for needed in "$compositor" "$saver" "$msg" "$pointer" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-doom-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
# a black terminal, on which the white of frost shows
mkdir -p "$XDG_CONFIG_HOME/xfce4/terminal"
printf '[Configuration]\nColorUseTheme=FALSE\nColorBackground=#000000\nColorForeground=#ffffff\n' \
	> "$XDG_CONFIG_HOME/xfce4/terminal/terminalrc"

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
trap cleanup EXIT INT TERM

list=$("$saver" --list)
echo "$list" | grep -q "^doomsday " || fail "--list does not show Doomsday"
echo "$list" | grep -Eq "^(hellfire|thunderstorm|blizzard) " &&
	fail "--list shows a kind of Doomsday as a saver of its own"

saver_conf() {
	printf 'screensaver {\n%s\n}\n' "$1" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
}
saver_conf "name doomsday
doomsday_kind blizzard"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
animations off
screensaver_command $saver
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

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

# a window with room above it for snow
ipc mode window >/dev/null 2>&1
ipc exec "xfce4-terminal --disable-server" >/dev/null 2>&1
attempt=0
while [ $attempt -lt 40 ] && ! ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"'; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if ! ipc -t get_tree | grep -q '"app_id": "xfce4-terminal"'; then
	echo "no window came up"
	exit 1
fi
ipc floating enable >/dev/null 2>&1
ipc resize set 600 320 >/dev/null 2>&1
ipc move position 340 260 >/dev/null 2>&1
sleep 1

# where the window is: x, the top of its title bar, width, and the top of what it draws
read -r wx wtop ww wcontent <<EOF
$(ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
    if n.get("app_id") == "xfce4-terminal":
        r, d = n["rect"], n.get("deco_rect") or {}
        print(r["x"], r["y"] - d.get("height", 0), r["width"], r["y"])
        return True
    return any(walk(c) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin))')
EOF
echo "the window: x $wx, title bar at $wtop, $ww wide, drawing from $wcontent"

# the mean red, green and blue of a part of the screen
mean() {
	grim "$work/shot.png" 2>/dev/null
	"$count" --mean "$work/shot.png" "$@"
}
light() {
	mean "$@" | awk '{ print int(($1 + $2 + $3) / 3) }'
}
running() {
	for pid in $(pgrep -f "^$saver" 2>/dev/null); do
		if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null |
				grep -qx "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"; then
			return 0
		fi
	done
	return 1
}
# ends the saver: a pointer that moves (twice: a new one loses its first motion)
end_saver() {
	"$pointer" 1280 720 move 900 100 move 940 140 move 980 180 >/dev/null 2>&1
	sleep 1
	running && fail "moving the mouse did not end $1"
}

corner_x=$((wx + 4))
corner_y=$((wcontent + 4))
middle_x=$((wx + ww / 2 - 7))
middle_y=$((wcontent + 100))
wall_before=$(mean 20 20 200 150)
echo "before: wallpaper $wall_before"

ipc screensaver start >/dev/null
sleep 4
running || fail "the Blizzard did not start"
wall=$(mean 20 20 200 150)
echo "Blizzard, 4 s: wallpaper $wall"
# teal has no red, and as much blue as green; in the dark, cold light of the
# storm it is grey-blue, bluer than green, and darker
echo "$wall" | awk '{ exit !($1 > 25 && $3 > $2 + 10 && $1 + $2 + $3 < 256) }' ||
	fail "the wallpaper did not turn to the cold light of the storm"

sleep 41
snow=$(light $((wx + 40)) $((wtop - 3)) $((ww - 80)) 3)
corner=$(light "$corner_x" "$corner_y" 14 14)
middle=$(light "$middle_x" "$middle_y" 14 14)
echo "Blizzard, 45 s: just above the title bar $snow; in the window: corner $corner, middle $middle"
[ "$snow" -gt 190 ] || fail "no snow lies on the window"
# the frost grows in from the corners, the middle stays clear longest
[ "$corner" -gt $((middle + 25)) ] || fail "no frost on the glass of the window"
end_saver "the Blizzard"

saver_conf "name doomsday
doomsday_kind hellfire"
ipc screensaver start >/dev/null
sleep 5
running || fail "Hellfire did not start"
wall=$(mean 20 20 200 150)
echo "Hellfire, 5 s: wallpaper $wall"
[ "$wall" != "$wall_before" ] || fail "Hellfire left the wallpaper as it was"
end_saver "Hellfire"

saver_conf "name thunderstorm"
ipc screensaver start >/dev/null
sleep 3
running || fail "the old name thunderstorm does not start the Thunderstorm any more"
dark=$(light 20 20 200 150)
echo "Thunderstorm by its old name, 3 s: wallpaper $dark"
end_saver "the Thunderstorm"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
