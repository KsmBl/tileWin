#!/bin/sh
# The first-run window, in a nested tileWin with the taskbar:
#  - a new user gets it once at the start of the session;
#  - a click on a card changes the theme at once, and Enter walks through the
#    steps to the end, where the window and the program go;
#  - a restart of the taskbar does not bring it back;
#  - someone with a session behind them does not get it after an update.
#
# usage: welcome.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
settings=$build/settings/tilewin-settings
keys=$build/tests/keyboard-tool
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$settings" "$keys" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session; do
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
sock=$XDG_RUNTIME_DIR/tw-welcome-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_WELCOME=1
export PATH=$build/settings:$build/swaymsg:$PATH
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
echo win10 > "$XDG_CONFIG_HOME/tileWin/current-theme"
echo win10 > "$XDG_CONFIG_HOME/tileWin/theme-window"

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
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x800 pos 0 0
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

open() { ipc -t get_tree | grep -q '"name": "Welcome to tileWin"'; }
attempt=0
while ! open && [ $attempt -lt 60 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
open || { echo "FAIL: a new user got no welcome"; exit 1; }
[ -e "$XDG_STATE_HOME/tileWin/welcomed" ] || fail "it is not written down that the welcome was shown"
sleep 1
ipc '[title="Welcome to tileWin"] fullscreen enable' >/dev/null
sleep 1

# the second card of the first row: Windows 3, whatever the theme was before
"$pointer" 1280 800 click 640 280
sleep 1.5
theme=$(cat "$XDG_CONFIG_HOME/tileWin/current-theme" 2>/dev/null)
[ "$theme" != win10 ] || fail "a click on a card left the theme at $theme"
echo "the card chose $theme"

"$keys" key none return
sleep 0.5
"$keys" key none return
sleep 0.5
"$keys" key none return
sleep 0.5
open || fail "the window went before the last step"
"$keys" key none return
attempt=0
while open && [ $attempt -lt 25 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
open && fail "Finish did not close the window"
sleep 0.5
if ipc -t get_tree | grep -q '"name": "tileWin Settings"'; then
	fail "the settings window came up with it"
fi
pgrep -f "$settings --welcome" >/dev/null 2>&1 && fail "the program goes on running without a window"

# not again
ipc restart panel >/dev/null 2>&1
sleep 2.5
open && fail "a restart of the taskbar brought the welcome back"

# not for someone who was here before this came in
rm -f "$XDG_STATE_HOME/tileWin/welcomed"
: > "$XDG_STATE_HOME/tileWin/last-session"
ipc restart panel >/dev/null 2>&1
sleep 2.5
open && fail "someone with a session behind them got the welcome"
[ -e "$XDG_STATE_HOME/tileWin/welcomed" ] || fail "the welcome will still come for them later"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
