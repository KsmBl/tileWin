#!/bin/sh
# The settings app has its icon on its taskbar button: its window (app id
# org.tilewin.Settings) is found by the StartupWMClass of its desktop entry,
# and where the theme has no icon of its own (the sway theme) its own icon,
# blue tiles with a gear, is taken from hicolor.
#
# usage: settings_icon.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
settings=$build/settings/tilewin-settings

for needed in "$compositor" "$msg" "$panel" "$settings"; do
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
sock=$XDG_RUNTIME_DIR/tw-sicon-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
# the desktop entry and the icon as they are installed
export XDG_DATA_DIRS=$work/data:/usr/share
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$work/data/applications" "$work/data/icons/hicolor/scalable/apps"
cp "$source_dir/data/tilewin-settings.desktop" "$work/data/applications/"
cp "$source_dir/data/icons/org.tilewin.Settings.svg" "$work/data/icons/hicolor/scalable/apps/"
[ -f /usr/share/icons/hicolor/index.theme ] &&
	cp /usr/share/icons/hicolor/index.theme "$work/data/icons/hicolor/"
echo sway > "$XDG_CONFIG_HOME/tileWin/current-theme"

cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
session_restore no
remember_windows no
animations off
panel_command $panel
mode tile
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec $settings
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
sleep 8

# the taskbar of the sway theme is at the top in tile mode: the blue of the
# icon (#1f6fc9 to #4aa3f0) beside the title of the focused window, right of
# the workspaces (whose small screens are blue too)
grim -g "165,0 475x40" -t ppm "$work/bar.ppm" 2>/dev/null
blue=$(python3 - "$work/bar.ppm" <<'EOP'
import sys
data = open(sys.argv[1], "rb").read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
pixels = data[pos + 1:]
count = 0
for i in range(0, len(pixels) - 2, 3):
    r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
    if 20 <= r <= 90 and 100 <= g <= 175 and 190 <= b <= 250:
        count += 1
print(count)
EOP
)
echo "$blue pixels of the icon's blue on the taskbar"
if [ "${blue:-0}" -lt 70 ]; then # without it: about 35
	echo "FAIL: the settings app has no icon on its taskbar button"
	exit 1
fi
echo "all checks passed"
