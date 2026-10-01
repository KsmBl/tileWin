#!/bin/sh
# The Windows 3 theme's own ways, in a nested tileWin with the taskbar:
#  - the Start menu is the Program Manager: its program groups, and a group
#    opened as a window whose icons start the apps;
#  - a minimized window lies as an icon at the bottom of the desktop, and a
#    double click on it restores the window;
#  - "minimized_icons no" in taskbar.conf leaves them on the taskbar only.
#
# usage: win3_extras.sh <build dir> <source dir>
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
for needed in dbus-run-session grim xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-win3-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
# only the apps of this test, so the groups are known
export XDG_DATA_DIRS=$work/nodata
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications" "$work/nodata"
cat > "$XDG_DATA_HOME/applications/testcalc.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=Test Calc
Exec=touch $work/launched
Icon=accessories-calculator
Categories=Utility;
EOD
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
panel_command $panel
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
pixel() { grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'; }
# the mean brightness of a part of the screen
mean() {
	grim -g "$1,$2 $3x$4" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
	while data[pos:pos + 1].isspace():
		pos += 1
	start = pos
	while not data[pos:pos + 1].isspace():
		pos += 1
	fields.append(data[start:pos])
pixels = data[pos + 1:]
print(sum(pixels) // max(1, len(pixels)))'
}
minimized() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "Terminal" and c.get("pid"):
			print("yes" if c.get("minimized") else "no")
		walk(c)
walk(json.load(sys.stdin))' | head -n 1
}

ipc theme win3 >/dev/null
sleep 2.5

# the Program Manager, at the bottom left over the taskbar
ipc panel startmenu toggle >/dev/null
sleep 1.2
[ "$(pixel 60 362)" = "0 0 128" ] || fail "the Start menu is not the Program Manager (navy title: $(pixel 60 362))"
# the one group there is (Accessories) as an icon, opened as a window
"$pointer" 1280 720 move 58 430 wait 300 click 58 430 >/dev/null 2>&1
sleep 0.8
[ "$(pixel 60 416)" = "0 0 128" ] || fail "the program group does not open as a window ($(pixel 60 416))"
# its one app, in the first cell of the group window
"$pointer" 1280 720 move 66 462 wait 300 click 66 462 >/dev/null 2>&1
sleep 1
[ -e "$work/launched" ] || fail "the icon in the program group does not start the app"

# a minimized window as an icon at the bottom left of the desktop
ipc exec "xfce4-terminal --disable-server -T Terminal" >/dev/null
sleep 3
empty=$(mean 4 610 80 70)
ipc '[title=Terminal] minimize enable' >/dev/null
sleep 1.2
[ "$(minimized)" = yes ] || fail "the window did not minimize"
[ "$(mean 4 610 80 70)" != "$empty" ] || fail "the minimized window has no icon on the desktop"
"$pointer" 1280 720 move 46 640 wait 300 doubleclick 46 640 >/dev/null 2>&1
sleep 1
[ "$(minimized)" = no ] || fail "a double click on the icon does not restore the window"

# switched off in taskbar.conf
echo "minimized_icons no" >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
sleep 2
ipc '[title=Terminal] minimize enable' >/dev/null
sleep 1.2
[ "$(mean 4 610 80 70)" = "$empty" ] || fail "minimized_icons no still shows the icon"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
