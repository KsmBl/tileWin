#!/bin/sh
# Qt apps follow the dark color scheme: Qt knows no desktop called tileWin and
# would fall back to a theme that never asks whether the session is dark, so
# tileWin gives the programs it starts QT_QPA_PLATFORMTHEME=xdgdesktopportal,
# and leaves a theme of the user's own (such as qt6ct) alone:
#  - started without it, a program tileWin runs has xdgdesktopportal
#  - started with qt6ct, it keeps qt6ct
#  - tilewin-session sets it the same way
#
# usage: qt_platform_theme.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$msg"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-daemon >/dev/null 2>&1 || [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no dbus-daemon or XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-qt-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

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

# what a program tileWin starts gets, with QT_QPA_PLATFORMTHEME as given (or unset)
theme_given() { # [value]
	rm -f "$work/seen"
	cat > "$work/tilewin.conf" <<EOF
session_restore no
panel_command none
polkit_agent disable
exec sh -c 'printf "%s" "\${QT_QPA_PLATFORMTHEME-unset}" > $work/seen'
EOF
	if [ $# -gt 0 ]; then
		set -- env -u WAYLAND_DISPLAY -u DISPLAY "QT_QPA_PLATFORMTHEME=$1"
	else
		set -- env -u WAYLAND_DISPLAY -u DISPLAY -u QT_QPA_PLATFORMTHEME
	fi
	"$@" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
		WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
		"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
	attempt=0
	while [ ! -e "$work/seen" ] && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	cat "$work/seen" 2>/dev/null || echo "nothing"
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.8
}

unset_seen=$(theme_given)
echo "started without it, a program gets: $unset_seen"
[ "$unset_seen" = xdgdesktopportal ] || fail "Qt apps get no platform theme that follows dark mode"

own=$(theme_given qt6ct)
echo "started with qt6ct, a program gets: $own"
[ "$own" = qt6ct ] || fail "the user's own Qt platform theme was replaced"

session=$build/tilewin-session
if [ -f "$session" ]; then
	grep -q 'QT_QPA_PLATFORMTHEME="${QT_QPA_PLATFORMTHEME:-xdgdesktopportal}"' "$session" ||
		fail "tilewin-session does not set the Qt platform theme"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
