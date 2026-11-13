#!/bin/sh
# The settings of the claude widget: what it shows, ticked and in order. On
# the Taskbar page its gear opens the dialog; ticking "Time until the 5-hour
# limit resets" adds reset_5h after the two shown by default, its arrow up puts
# it between them, and taskbar.conf has show "usage_5h reset_5h usage_7d" for
# the widget; the settings are still running after it (the rows are made again
# on every change).
#
# usage: settings_claude.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$settings" "$pointer"; do
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
export GIO_USE_VFS=local CLAUDE_CONFIG_DIR=$work/claude
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/claude"
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
sed -e 's/^\tright tray keyboard volume/\tright claude tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$conf"

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

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
panel_command true
output * mode --custom 1280x3000
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec "$settings" --page=taskbar
EOF
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
wait_until 20 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.Settings\"'" ||
	fail "the settings never came up"
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
wait_still 15 screen
click() { "$pointer" 1280 3000 click "$1" "$2" >/dev/null 2>&1; }
shows() { sed -n '/^widget claude {/,/^}/p' "$conf" | sed -n 's/^[[:space:]]*show //p' | tr -d '"'; }

click 1166 1284 # the gear of "Claude Code usage" in the right section
wait_until 10 sh -c "'$msg' -s '$sock' -t get_tree | grep -q 'Claude Code usage settings'" ||
	fail "the settings of the widget did not open"
wait_still 5 screen
click 382 1565 # tick: Time until the 5-hour limit resets
wait_until 5 sh -c "[ \"\$(sed -n '/^widget claude {/,/^}/p' '$conf' | grep -c reset_5h)\" -gt 0 ]" ||
	fail "ticking wrote nothing: $(shows)"
[ "$(shows)" = "usage_5h usage_7d reset_5h" ] || fail "after the tick it shows '$(shows)'"
wait_still 5 screen
click 847 1565 # its arrow up
wait_until 5 sh -c "sed -n '/^widget claude {/,/^}/p' '$conf' | grep -q 'usage_5h reset_5h usage_7d'" ||
	fail "the arrow did not move it up: '$(shows)'"
"$msg" -s "$sock" -t get_tree | grep -q '"app_id": "org.tilewin.Settings"' ||
	fail "the settings are gone after the changes"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
