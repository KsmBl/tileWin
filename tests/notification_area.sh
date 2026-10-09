#!/bin/sh
# The notification bell belongs to the notification area of the taskbar. On
# the XP theme that area is a light blue band from the tray to the right end;
# the bell, which shows only while there are new notifications, used to cut
# it short at the clock, leaving the bell and show desktop on the dark taskbar.
# In a nested tileWin with the taskbar and the XP theme:
#  - after two notifications the right end of the taskbar is the band's blue,
#    as it was before them, not the darker taskbar
#  - the bell shows with a red-orange count, which stands out on the blue
#
# usage: notification_area.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
count=$build/tests/count-content

for needed in "$compositor" "$msg" "$panel" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon grim notify-send python3; do
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
sock=$XDG_RUNTIME_DIR/tw-notif-area-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo winxp > "$XDG_CONFIG_HOME/tileWin/current-theme"
cat > "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
layout window {
	position bottom
	left start taskbar
	right tray volume network battery clock notifications showdesktop
}
EOF

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
polkit_agent disable
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$DBUS_SESSION_BUS_ADDRESS" > $work/dbus; printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
export WAYLAND_DISPLAY=$(cat "$work/display") DBUS_SESSION_BUS_ADDRESS=$(cat "$work/dbus")
sleep 3

# the blue of the taskbar's background under the bell, a row near
# its bottom where no icon is: "<blue> <brightness>"
band() {
	grim "$work/$1.png"
	if [ -n "${NOTIF_SHOTS:-}" ]; then
		mkdir -p "$NOTIF_SHOTS" && cp "$work/$1.png" "$NOTIF_SHOTS/"
	fi
	"$count" --mean "$work/$1.png" 1232 712 10 3 | awk '{ printf "%d %d", $3, ($1 + $2 + $3) / 3 }'
}
# red-orange pixels at the right end of the taskbar: the count on the bell
badge() {
	python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
p = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1])
n, stride, data = p.get_n_channels(), p.get_rowstride(), p.get_pixels()
found = 0
for y in range(680, 720):
    for x in range(1000, 1280):
        i = y * stride + x * n
        r, g, b = data[i], data[i + 1], data[i + 2]
        if r > 180 and g < 120 and b < 90:
            found += 1
print(found)' "$work/$1.png"
}

set -- $(band before)
before_blue=$1 before_light=$2
notify-send -a Test "Hello" "A notification"
sleep 0.5
notify-send -a Test "Second" "Another one"
sleep 7 # the toasts have gone
set -- $(band after)
after_blue=$1 after_light=$2
red=$(badge after)
echo "right end of the taskbar: blue $before_blue, brightness $before_light before;" \
	"blue $after_blue, brightness $after_light with notifications; badge pixels $red"
[ "$after_light" -ge $((before_light - 4)) ] ||
	fail "the notification area stops at the clock: the bell sits on the darker taskbar"
[ "$red" -gt 20 ] || fail "no red-orange count on the bell"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
