#!/bin/sh
# The settings of the AI agent usage widget: what it shows, ticked and in
# order. A config from before it, with the claude and codex widget on the
# taskbar and the desktop, has them replaced when the settings open: "agents"
# once in the layout, on the desktop its entry and none of theirs, their widget
# blocks gone. On the Taskbar page its gear opens the dialog; ticking "Time
# until the 5-hour limit resets" adds reset_5h after the two shown by default,
# its arrow up puts it between them, and taskbar.conf has show "usage_5h
# reset_5h usage_7d" for the widget; the settings are still running after it
# (the rows are made again on every change).
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
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 TILEWIN_NO_AGENT_SCAN=1 GSETTINGS_BACKEND=memory
export GIO_USE_VFS=local CLAUDE_CONFIG_DIR=$work/claude CODEX_HOME=$work/no-codex
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/claude"
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
sed -e 's/^\tright tray keyboard volume/\tright claude codex:2 tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$conf"
printf 'widget claude {\n\taccount ~/.claude\n}\nwidget codex:2 {\n\taccount ~/.codex\n}\n' >> "$conf"
printf 'desktop_widgets {\n\tclaude {\n\t\tstyle list\n\t}\n\tcodex {\n\t\tstyle week\n\t}\n}\n' >> "$conf"

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
shows() { sed -n '/^widget agents {/,/^}/p' "$conf" | sed -n 's/^[[:space:]]*show //p' | tr -d '"'; }

grep -q '^	right agents tray keyboard volume' "$conf" ||
	fail "the claude and codex widget on the taskbar did not become one agents: $(grep -m1 '^	right' "$conf")"
grep -qE '^widget (claude|codex)' "$conf" && fail "the widget blocks of claude and codex are still there"
desk=$(sed -n '/^desktop_widgets {/,/^}/p' "$conf")
[ "$(printf '%s\n' "$desk" | grep -cE '^	(agents|claude|codex)')" = 1 ] &&
	printf '%s\n' "$desk" | grep -q '^	agents {' && printf '%s\n' "$desk" | grep -q 'style list' ||
	fail "the desktop has not one agents with the looks of the first: $(printf '%s' "$desk" | tr '\n' ' ')"

click 1166 1119 # the gear of the AI agent usage widget in the right section
wait_until 10 sh -c "'$msg' -s '$sock' -t get_tree | grep -q 'AI agent usage settings'" ||
	fail "the settings of the widget did not open"
wait_still 5 screen
click 382 1750 # tick: Time until the 5-hour limit resets (under the accounts)
wait_until 5 sh -c "[ \"\$(sed -n '/^widget agents {/,/^}/p' '$conf' | grep -c reset_5h)\" -gt 0 ]" ||
	fail "ticking wrote nothing: $(shows)"
[ "$(shows)" = "usage_5h usage_7d reset_5h" ] || fail "after the tick it shows '$(shows)'"
wait_still 5 screen
click 847 1750 # its arrow up
wait_until 5 sh -c "sed -n '/^widget agents {/,/^}/p' '$conf' | grep -q 'usage_5h reset_5h usage_7d'" ||
	fail "the arrow did not move it up: '$(shows)'"
"$msg" -s "$sock" -t get_tree | grep -q '"app_id": "org.tilewin.Settings"' ||
	fail "the settings are gone after the changes"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
