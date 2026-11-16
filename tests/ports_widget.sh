#!/bin/bash
# The ports widget: on the taskbar how many ports are open, drawn again when a
# program starts listening; on the desktop the List style draws something in
# every theme; a click opens its flyout, and the panel survives all of it.
#
# usage: ports_widget.sh <build dir> <source dir>
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
listener=
cleanup() {
	[ -n "$listener" ] && kill "$listener" 2>/dev/null
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

sed -e 's/^\tright tray keyboard volume/\tright ports tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOT
theme_layout no
widget ports {
	interval 1
}
desktop_widgets {
	ports { style list; column 0; row 0 }
}
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

bar() { grim -g "640,680 640x40" - 2>/dev/null | md5sum; }
wait_still 15 bar
before=$(bar)
# a program starts listening, only for this computer
python3 -c "
import socket, time
s = socket.socket()
s.bind(('127.0.0.1', 0))
s.listen()
time.sleep(600)" &
listener=$!
wait_until 10 sh -c "[ \"\$(grim -g '640,680 640x40' - 2>/dev/null | md5sum)\" != '$before' ]" ||
	fail "the taskbar did not show the new port"

count_drawn() { # x y w h: the pixels that are not the wallpaper
	grim -g "$1,$2 $3x$4" "$work/card.png" 2>/dev/null &&
		python3 -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
print(sum(1 for p in im.getdata() if abs(p[0] - 0x20) + abs(p[1] - 0x40) + abs(p[2] - 0x70) > 40))" \
			"$work/card.png" 2>/dev/null || echo 0
}
if python3 -c 'import PIL' 2>/dev/null; then
	for theme in win95 winxp win7 win11 atari console; do
		ipc theme "$theme" >/dev/null 2>&1
		sleep 3
		drawn=$(count_drawn 14 14 290 290)
		[ "${drawn:-0}" -gt 2000 ] || fail "$theme: the list shows next to nothing ($drawn pixels)"
	done
fi

# a click on the widget opens its flyout (Windows 10: the widget is first on the right)
ipc theme win10 >/dev/null 2>&1
sleep 3
above() { grim -g "600,150 680x520" - 2>/dev/null | md5sum; }
wait_still 5 above
closed=$(above)
"$pointer" 1280 720 click 1000 700 >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(grim -g '600,150 680x520' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a click on the widget opened no flyout"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
