#!/bin/sh
# Alt+F4 in window mode, as on Windows: on a window it closes that window; with
# the desktop in front (the desktop clicked, or no window open) it opens the
# shut down dialog, and closes no window at all (kill without a window used
# to close every window of the desktop). Only Escape is pressed in the
# dialog: nothing here can shut the computer down.
#
# usage: desktop_alt_f4.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$pointer" "$keys"; do
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
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0")' 2>/dev/null; then
	echo "GTK 4 for Python is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-altf4-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/Desktop"
printf 'XDG_DESKTOP_DIR="%s/Desktop"\n' "$work" > "$XDG_CONFIG_HOME/user-dirs.dirs"

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

# the binding of window mode, as installed
binding=$(grep -m1 '^bindsym Mod1+F4 ' "$source_dir/config/windowmode.conf")
cat > "$work/tilewin.conf" <<EOC
$binding
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
ipc() { "$msg" -s "$sock" "$@"; }
ipc mode window >/dev/null 2>&1
sleep 3

windows() { ipc -t get_tree | grep -c '"name": "altf4-'; }
open_window() {
	ipc exec "python3 $source_dir/tests/color_window.py altf4-$1 '#c03030'" >/dev/null
	attempt=0
	while [ "$(windows)" -lt "$2" ] && [ $attempt -lt 40 ]; do
		sleep 0.25
		attempt=$((attempt + 1))
	done
	sleep 1
}
alt_f4() {
	"$keys" key alt f4 >/dev/null 2>&1
	sleep 1.5
}
# the shut down dialog dims the whole screen, or covers its middle
middle() {
	grim -g "440,200 400x300" -t ppm - 2>/dev/null | python3 -c '
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
px = data[pos + 1:]
print(sum(px) // max(1, len(px)))'
}
dialog_shown() { # brightness before
	now=$(middle)
	[ $((now - $1)) -gt 8 ] || [ $(($1 - now)) -gt 8 ]
}

# 1. on a window: that window closes
open_window one 1
alt_f4
[ "$(windows)" -eq 0 ] || fail "Alt+F4 on a window did not close it"

# 2. the desktop clicked, a window still open: the dialog, the window stays
open_window two 1
ipc "[title=altf4-two] move position 700 400" >/dev/null 2>&1
sleep 1
"$pointer" 1280 720 click 200 500 >/dev/null 2>&1 # the desktop
sleep 1
plain=$(middle)
alt_f4
if dialog_shown "$plain"; then
	echo "ok   Alt+F4 on the desktop opened the shut down dialog"
else
	fail "Alt+F4 on the desktop opened no shut down dialog"
fi
[ "$(windows)" -eq 1 ] || fail "Alt+F4 on the desktop closed the window that was open"
"$keys" key none escape >/dev/null 2>&1
sleep 1.5

# 3. no window open at all: the dialog
ipc "[title=altf4-two] kill" >/dev/null # aimed at a window: it closes
sleep 1.5
[ "$(windows)" -eq 0 ] || fail "kill aimed at a window did not close it"
plain=$(middle)
alt_f4
if dialog_shown "$plain"; then
	echo "ok   Alt+F4 with no window open opened the shut down dialog"
else
	fail "Alt+F4 with no window open opened no shut down dialog"
fi
"$keys" key none escape >/dev/null 2>&1
sleep 1

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
