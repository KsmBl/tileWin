#!/bin/bash
# A theme that brings its own taskbar layout (Windows 8: no search box) keeps
# the widgets the user put on the taskbar: the updates widget of the layout in
# taskbar.conf shows on the Windows 8 taskbar too, while updates wait, and
# goes while all is up to date. Nothing is checked: no tilewin-updates runs.
#
# usage: theme_layout_widgets.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel

for needed in "$compositor" "$msg" "$panel"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v grim >/dev/null 2>&1; then
	echo "grim is not installed: skipping"
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
	[ -n "${THEME_LAYOUT_TEST_KEEP:-}" ] && echo "files in $work" || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

status() { # count: the status file, as tilewin-updates writes it
	printf '%s\n' "manager=pacman" "checked=$(date +%s)" "state=idle" "count=$1" \
		> "$state/.status" && mv "$state/.status" "$state/status"
}
printf '%s\t%s\t%s\n' firefox 133.0-1 134.0-1 > "$state/list"
status 0

# the layout of taskbar.conf has the updates widget next to the clock
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
grep -q '^	right .* updates clock' "$XDG_CONFIG_HOME/tileWin/taskbar.conf" ||
	fail "taskbar.conf has no updates widget next to the clock"
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
ipc theme win8 >/dev/null 2>&1

# the right of the taskbar, left of the clock (which may turn a minute)
around() { grim -g "700,680 440x40" - 2>/dev/null | md5sum; }
sleep 2
wait_still 15 around
none=$(around)
grim -g "0,680 1280x40" "$work/none.png"

status 3
wait_until 10 sh -c "[ \"\$(grim -g '700,680 440x40' - 2>/dev/null | md5sum)\" != '$none' ]" ||
	fail "Windows 8: the updates widget of taskbar.conf is not on its taskbar"
wait_still 5 around
grim -g "0,680 1280x40" "$work/three.png"

status 0
wait_until 10 sh -c "[ \"\$(grim -g '700,680 440x40' - 2>/dev/null | md5sum)\" = '$none' ]" ||
	fail "Windows 8: up to date, the updates widget did not go again"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
