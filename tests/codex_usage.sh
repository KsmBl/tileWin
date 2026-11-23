#!/bin/sh
# The codex widget on the taskbar, with a Codex folder of the test's own as
# $CODEX_HOME (no real Codex is run or touched):
#  - it draws the limits of the newest session log without being connected;
#  - it draws again when Codex writes new limits into the log;
#  - its history is filled from the session logs;
#  - a click on it opens its flyout.
#
# usage: codex_usage.sh <build dir> <source dir>
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
codex=$work/codex-folder
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 TILEWIN_NO_AGENT_SCAN=1 GSETTINGS_BACKEND=memory
export CODEX_HOME=$codex
day=$(date +%Y/%m/%d)
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$codex/sessions/$day"
log=$codex/sessions/$day/rollout-test.jsonl

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

now=$(date +%s)
# a token_count line as Codex writes it: <5h used> <week used>
limits() {
	printf '{"timestamp":"%s","type":"event_msg","payload":{"type":"token_count","info":null,"rate_limits":{"limit_id":"codex","primary":{"used_percent":%s,"window_minutes":300,"resets_at":%d},"secondary":{"used_percent":%s,"window_minutes":10080,"resets_at":%d},"plan_type":"plus"}}}\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%S.000Z)" "$1" $((now + 3600)) "$2" $((now + 3 * 86400))
}
printf '{"timestamp":"2026-01-01T00:00:00.000Z","type":"session_meta","payload":{}}\n' > "$log"
limits 23 41 >> "$log"

sed -e 's/^\tright tray keyboard volume/\tright codex tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
bar() { grim -g "600,680 680x40" "$work/bar.png" 2>/dev/null && md5sum < "$work/bar.png"; }
wait_still 15 bar

# ---- the history, filled from the session log
history=$(ls "$XDG_STATE_HOME"/tileWin/codex-usage/*.history 2>/dev/null | head -n 1)
[ -n "$history" ] || fail "no history was written"
grep -q " 23.0 $((now + 3600)) 41.0 $((now + 3 * 86400))\$" "$history" 2>/dev/null ||
	fail "the history does not have the values of the log: $(cat "$history" 2>/dev/null)"

# ---- new limits in the log: drawn again
before=$(bar)
limits 97 88 >> "$log"
# the widget looks again every five seconds
wait_until 12 sh -c "[ \"\$(grim -g '600,680 680x40' - 2>/dev/null | md5sum)\" != '$before' ]" ||
	fail "the taskbar did not show the new values"

# ---- a click opens the flyout above the taskbar
above() { grim -g "600,200 680x470" - 2>/dev/null | md5sum; }
wait_still 5 above
closed=$(above)
"$pointer" 1280 720 click 925 700 >/dev/null 2>&1 # the widget, first in the right section
wait_until 5 sh -c "[ \"\$(grim -g '600,200 680x470' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a click on the widget opened no flyout"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
