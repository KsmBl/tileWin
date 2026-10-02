#!/bin/sh
# The previews of windows above the taskbar show the windows themselves, in
# their colors, whichever renderer the compositor draws with: the software
# one hands the pictures over as XRGB, a GPU as XBGR. A red window has to come
# out red, not as its app icon and not blue.
#
# usage: thumbnail_colors.sh <build dir> <source dir>
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
for needed in dbus-run-session grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-thumbs-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk' \
		>/dev/null 2>&1; then
	echo "python3 has no GTK 4: skipping"
	exit 77
fi
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


# the middle of the preview over the first taskbar button (Windows 10)
preview_color() {
	grim -g "317,560 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'
}

run() {
	renderer=$1
	rm -f "$work/display"
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
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER="$renderer" \
		WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
		dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
	attempt=0
	while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	if [ ! -s "$work/display" ]; then
		fail "$renderer: the nested compositor never came up"
		return
	fi
	WAYLAND_DISPLAY=$(cat "$work/display")
	export WAYLAND_DISPLAY
	ipc theme win10 >/dev/null
	sleep 2
	ipc exec "python3 $source_dir/tests/color_window.py Red '#d02020'" >/dev/null
	sleep 3
	"$pointer" 1280 720 move 372 700 wait 2500 >/dev/null 2>&1 &
	held=$!
	sleep 2
	set -- $(preview_color)
	wait "$held"
	[ "${1:-0}" -gt 150 ] && [ "${3:-255}" -lt 90 ] ||
		fail "$renderer: the preview of a red window is not red: $*"
	ipc exit >/dev/null 2>&1
	sleep 1
}

ipc() { "$msg" -s "$sock" "$@"; }

run pixman
if ls /dev/dri/renderD* >/dev/null 2>&1; then
	run gles2
else
	echo "no GPU to draw with: only the software renderer was tried"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
