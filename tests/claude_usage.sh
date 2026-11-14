#!/bin/sh
# The usage limits of Claude Code for the claude widget, with a Claude Code
# folder of the test's own (no real Claude Code is run or touched):
#  - "tilewin-claude-usage connect" puts itself before the folder's status line
#    and keeps the rest of settings.json; a settings.json it cannot read stays;
#  - the status line it becomes writes down rate_limits of the JSON Claude Code
#    hands it, runs the old status line with the same input and shows its output;
#  - a window not in the input keeps what was written down before;
#  - "disconnect" gives back the old status line and forgets the values;
#  - the widget on the taskbar draws the values, and again when they change;
#  - a click on it opens its flyout.
#
# usage: claude_usage.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
tool=$build/claude/tilewin-claude-usage
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool

for needed in "$tool" "$compositor" "$msg" "$panel" "$pointer"; do
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
claude=$work/claude-folder
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export CLAUDE_CONFIG_DIR=$claude
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$claude"

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

json() { python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$@"; }
printf '{"oauthAccount": {"emailAddress": "me@example.org"}}\n' > "$claude/.claude.json"
cat > "$claude/settings.json" <<'EOF'
{
  "model": "opus",
  "statusLine": { "type": "command", "command": "printf 'old:'; cat", "padding": 2 }
}
EOF

# ---- connect
"$tool" connect "$claude" >/dev/null || fail "connect failed"
command=$(json "$claude/settings.json" 'd["statusLine"]["command"]')
case "$command" in
*tilewin-claude-usage*statusline*) ;;
*) fail "the status line is not tileWin's: $command" ;;
esac
[ "$(json "$claude/settings.json" 'd["model"]')" = opus ] || fail "connect lost the rest of settings.json"
[ "$(json "$claude/settings.json" 'd["statusLine"]["padding"]')" = 2 ] ||
	fail "connect lost the padding of the status line"
"$tool" connect "$claude" | grep -q 'connected already' || fail "a second connect did not see the first"

# ---- the status line: values written down, the old one shown
now=$(date +%s)
input=$(printf '{"model":{"display_name":"Opus"},"rate_limits":{"five_hour":{"used_percentage":23,"resets_at":%d},"seven_day":{"used_percentage":41,"resets_at":%d}}}' \
	$((now + 3600)) $((now + 3 * 86400)))
shown=$(printf '%s' "$input" | sh -c "$command")
case "$shown" in
old:*rate_limits*) ;;
*) fail "the old status line did not get the input or its output was not shown: $shown" ;;
esac
"$tool" list > "$work/list"
grep -q 'me@example.org' "$work/list" || fail "list does not name the account: $(cat "$work/list")"
grep -q '5h 23%' "$work/list" || fail "the 5-hour value was not written down: $(cat "$work/list")"
grep -q '7d 41%' "$work/list" || fail "the weekly value was not written down: $(cat "$work/list")"
# a window missing from the input keeps what was there
printf '{"rate_limits":{"five_hour":{"used_percentage":30,"resets_at":%d}}}' $((now + 3600)) |
	sh -c "$command" >/dev/null
"$tool" list > "$work/list"
grep -q '5h 30%' "$work/list" && grep -q '7d 41%' "$work/list" ||
	fail "a missing window was not kept: $(cat "$work/list")"

# ---- a settings.json it cannot read stays as it is
mkdir -p "$work/broken"
echo '{ this is not json' > "$work/broken/settings.json"
"$tool" connect "$work/broken" >/dev/null 2>&1 && fail "connect took a settings.json it cannot read"
[ "$(cat "$work/broken/settings.json")" = '{ this is not json' ] || fail "a broken settings.json was changed"

# ---- the widget draws the values, and again when they change
sed -e 's/^\tright tray keyboard volume/\tright claude tray keyboard volume/' \
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
before=$(bar)
printf '{"rate_limits":{"five_hour":{"used_percentage":97,"resets_at":%d},"seven_day":{"used_percentage":88,"resets_at":%d}}}' \
	$((now + 3600)) $((now + 3 * 86400)) | sh -c "$command" >/dev/null
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

# ---- disconnect
"$tool" disconnect "$claude" >/dev/null || fail "disconnect failed"
[ "$(json "$claude/settings.json" 'd["statusLine"]["command"]')" = "printf 'old:'; cat" ] ||
	fail "disconnect did not give back the old status line: $(cat "$claude/settings.json")"
"$tool" list | grep -q '5h' && fail "disconnect left the values behind"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
