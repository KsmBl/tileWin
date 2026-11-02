#!/bin/sh
# The cards of the Theme page show how each theme looks: over its wallpaper a
# small window and a taskbar in its own colors, here the navy title bar of
# Windows 95 and the blue one of Windows XP.
#
# usage: settings_theme_cards.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings

for needed in "$compositor" "$msg" "$settings"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-cards-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"

ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
panel_command none
animations off
output HEADLESS-1 mode --custom 1900x1500 pos 0 0
exec $settings --page=theme
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

attempt=0
while ! ipc -t get_tree | grep -q '"name": "tileWin Settings"' && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
sleep 1.5
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 2.5
grim -t ppm "$work/shot.ppm"

python3 - "$work/shot.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
pixels = data[pos + 1:]
def count(want, slack=6):
    r, g, b = want
    n = 0
    for i in range(0, len(pixels) - 2, 3):
        if abs(pixels[i] - r) <= slack and abs(pixels[i + 1] - g) <= slack and abs(pixels[i + 2] - b) <= slack:
            n += 1
    return n
checks = {'the navy title bar of Windows 95': (0, 0, 128), 'the blue title bar of Windows XP': (0, 88, 238)}
bad = 0
for what, color in checks.items():
    n = count(color)
    print(what, n)
    if n < 100:
        print('FAIL: no card shows', what)
        bad = 1
sys.exit(bad)
PY
status=$?
[ $status = 0 ] && echo "the cards show the themes"
exit $status
