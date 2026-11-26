#!/bin/bash
# The updates widget, from the state tilewin-updates writes: on the taskbar
# while updates wait, gone while all is up to date, drawn again while they are
# installed; a click opens its flyout, every theme draws it, and the panel
# survives all of it. Nothing is checked or installed: no tilewin-updates runs.
#
# usage: updates_widget.sh <build dir> <source dir>
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
for needed in grim python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import PIL' 2>/dev/null; then
	echo "python-pillow is not installed: skipping"
	exit 77
fi
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
state=$XDG_STATE_HOME/tileWin/updates
mkdir -p "$state"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	[ -n "${UPDATES_TEST_KEEP:-}" ] && echo "files in $work" || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

status() { # key=value...: the status file, as tilewin-updates writes it
	printf '%s\n' "manager=pacman" "checked=$(date +%s)" "cache=734003200" "weekly=off" "$@" \
		> "$state/.status" && mv "$state/.status" "$state/status"
}
printf '%s\t%s\t%s\n' linux 7.3.5.arch1-1 7.3.6.arch1-1 firefox 133.0-1 134.0-1 \
	mesa 1:25.3.1-1 1:25.3.2-1 > "$state/list"
status state=idle count=3

cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
echo "theme_layout no" >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
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

bar() { grim -g "0,680 1280x40" - 2>/dev/null | md5sum; }
wait_still 15 bar
grim -g "0,680 1280x40" "$work/with.png"

# up to date: the widget hides
status state=idle count=0
: > "$state/list"
wait_until 10 sh -c "[ \"\$(grim -g '0,680 1280x40' - 2>/dev/null | md5sum)\" != '$(bar)' ]" ||
	fail "up to date, the widget did not hide"
wait_still 5 bar
grim -g "0,680 1280x40" "$work/without.png"

# where it was: the pixels that differ
box=$(python3 -c "
import sys
from PIL import Image, ImageChops
d = ImageChops.difference(Image.open(sys.argv[1]).convert('RGB'), Image.open(sys.argv[2]).convert('RGB'))
b = d.getbbox()
print('%d %d %d' % (b[0], b[2], b[2] - b[0]) if b else '')" "$work/with.png" "$work/without.png")
[ -n "$box" ] || fail "the widget drew nothing with three updates waiting"

# installing: drawn again, with how far it is
status state=installing done=1 total=3 current=firefox phase=installing count=3
printf '%s\t%s\t%s\n' linux 7.3.5.arch1-1 7.3.6.arch1-1 firefox 133.0-1 134.0-1 \
	mesa 1:25.3.1-1 1:25.3.2-1 > "$state/list"
wait_until 10 sh -c "[ \"\$(grim -g '0,680 1280x40' - 2>/dev/null | md5sum)\" != '$(bar)' ]" ||
	fail "an installation did not show on the taskbar"

# a click on it opens the flyout with the updates
status state=idle count=3
wait_still 5 bar
if [ -n "$box" ]; then
	read -r left right width <<< "$box"
	above() { grim -g "500,150 780x520" - 2>/dev/null | md5sum; }
	wait_still 5 above
	closed=$(above)
	"$pointer" 1280 720 click $(((left + right) / 2)) 700 >/dev/null 2>&1
	wait_until 5 sh -c "[ \"\$(grim -g '500,150 780x520' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
		fail "a click on the widget opened no flyout"
	sleep 1
	grim "$work/flyout.png"
fi

# every theme draws it, and the panel lives on
for theme in win95 winxp win7 win11 atari console; do
	ipc theme "$theme" >/dev/null 2>&1
	sleep 2
done

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
