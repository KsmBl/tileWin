#!/bin/sh
# The settings window takes the look of the theme that is on, and follows when
# the theme or the colour scheme changes while it is open. In a nested tileWin
# with the settings on the Screen saver page, filling the screen:
#  - with Windows 95 the page is its grey face, with Windows 2000 its warmer
#    grey, with XP the beige of Luna and the task pane on the left is blue,
#    with Windows 7 white, with Windows 11 the grey of its ground, and dark
#    with the dark colour scheme;
#  - every theme there is gives a style sheet GTK takes without a complaint.
#
# usage: settings_theme_look.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-settings-look-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo win95 > "$XDG_CONFIG_HOME/tileWin/current-theme"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #008080
session_restore no
panel_command none
animations off
output HEADLESS-1 mode --custom 1280x800 pos 0 0
exec $settings --page=screensaver
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

# the settings write what GTK says to their own log
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

settings_open() {
	ipc -t get_tree | grep -q '"name": "tileWin Settings"'
}
attempt=0
while ! settings_open && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
settings_open || { echo "the settings never opened"; exit 1; }
sleep 1
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 1

# "#rrggbb" of the screen at x y
pixel() {
	grim -g "$1,$2 1x1" "$work/pixel.png" &&
		python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
p = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1]).get_pixels()
print("#%02x%02x%02x" % (p[0], p[1], p[2]))' "$work/pixel.png"
}
# the page right of everything on it, and the left pane below its list
page() { pixel 1200 300; }
pane() { pixel 8 760; }
# switches the theme and waits for the page to be that colour
expect() {
	theme=$1 want=$2
	if [ "$theme" != - ]; then
		ipc theme "$theme" window >/dev/null 2>&1
	fi
	attempt=0
	while [ "$(page)" != "$want" ] && [ $attempt -lt 20 ]; do
		sleep 0.25
		attempt=$((attempt + 1))
	done
	got=$(page)
	echo "$theme: the page is $got"
	[ "$got" = "$want" ] || fail "with $theme the page is $got, not $want"
}

expect - "#c0c0c0"
expect win2000 "#d4d0c8"
expect winxp "#ece9d8"
side=$(pane)
python3 -I -c '
import sys
v = int(sys.argv[1][1:], 16)
r, g, b = v >> 16, (v >> 8) & 0xff, v & 0xff
sys.exit(0 if b > r + 60 and b > g + 30 else 1)' "$side" ||
	fail "the task pane of XP is not blue ($side)"
expect win7 "#ffffff"
expect win11 "#f3f3f3"
ipc color_scheme dark >/dev/null 2>&1
expect - "#202020"
ipc color_scheme light >/dev/null 2>&1
expect - "#f3f3f3"

# every theme, light and dark: GTK takes each style sheet
for scheme in light dark; do
	ipc color_scheme $scheme >/dev/null 2>&1
	for dir in "$source_dir"/themes/*/; do
		ipc theme "$(basename "$dir")" window >/dev/null 2>&1
		sleep 0.6
	done
done
settings_open || fail "the settings closed while the themes changed"
if grep -i "theme parser\|css.*error\|Gtk-CRITICAL" "$work/log" | grep -q .; then
	fail "GTK complained about a style sheet: $(grep -i -m 2 "theme parser\|css.*error\|Gtk-CRITICAL" "$work/log")"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the settings window looks like the theme"
