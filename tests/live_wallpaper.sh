#!/bin/bash
# The moving wallpaper of the Starship Console (panel/livewall.c): over the
# theme's own wallpaper the sensor's sweep goes round, a click on a row of the
# readouts opens its flyout, and "live_wallpaper no" keeps it all away.
#
# usage: live_wallpaper.sh <build dir> <source dir>
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
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
wallpaper theme
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
ipc theme console >/dev/null 2>&1

# the sensor sits at the left of the bottom, right of the desktop icons
sensor() { grim -g "150,300 302x302" - 2>/dev/null | md5sum; }
moving() {
	local a b
	a=$(sensor)
	sleep 0.6
	b=$(sensor)
	[ "$a" != "$b" ]
}
wait_until 15 moving || fail "the sweep of the sensor does not go round"

# a click on the processor's row of the readouts opens the CPU flyout
flyout() { grim -g "900,0 380x400" - 2>/dev/null | md5sum; }
closed=$(flyout)
"$pointer" 1280 720 move 700 375 click 700 375 >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(grim -g '900,0 380x400' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a click on the readouts opened nothing"
ipc panel close >/dev/null 2>&1

# switched off: nothing moves any more
echo "live_wallpaper no" >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
sleep 3
moving && fail "live_wallpaper no did not stop it"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
