#!/bin/bash
# The AI agent usage widget with two accounts, a Claude Code folder and a Codex
# folder (no real agent is run or touched):
#  - on the taskbar each account with its name and its values;
#  - a click on one opens the flyout of that account, listing the others,
#    and a click on another's row shows that one instead;
#  - "accounts" leaves out the accounts not named;
#  - the claude widget of an older config is the same widget, of its account.
# What is shown is read with tesseract, which the snipping tool uses as well.
#
# usage: agents_usage.sh <build dir> <source dir>
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
for needed in grim tesseract python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import PIL' 2>/dev/null; then
	echo "python3 has no PIL: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
claude=$work/claude-folder
codex=$work/codex-folder
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 TILEWIN_NO_AGENT_SCAN=1 GSETTINGS_BACKEND=memory
export CLAUDE_CONFIG_DIR=$claude CODEX_HOME=$codex
day=$(date +%Y/%m/%d)
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$claude" "$codex/sessions/$day"
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf

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
# Claude Code: connected, its status line has shown 37% and 52%
printf '{"oauthAccount": {"emailAddress": "me@example.org"}}\n' > "$claude/.claude.json"
printf '{}\n' > "$claude/settings.json"
"$tool" connect "$claude" >/dev/null || fail "connect failed"
command=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["statusLine"]["command"])' \
	"$claude/settings.json")
printf '{"rate_limits":{"five_hour":{"used_percentage":37,"resets_at":%d},"seven_day":{"used_percentage":52,"resets_at":%d}}}' \
	$((now + 3600)) $((now + 3 * 86400)) | sh -c "$command" >/dev/null
# Codex: its session log has 23% and 41%
{
	printf '{"timestamp":"2026-01-01T00:00:00.000Z","type":"session_meta","payload":{}}\n'
	printf '{"timestamp":"%s","type":"event_msg","payload":{"type":"token_count","info":null,"rate_limits":{"limit_id":"codex","primary":{"used_percent":23,"window_minutes":300,"resets_at":%d},"secondary":{"used_percent":41,"window_minutes":10080,"resets_at":%d},"plan_type":"plus"}}}\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%S.000Z)" $((now + 3600)) $((now + 3 * 86400))
} > "$codex/sessions/$day/rollout-test.jsonl"

sed -e 's/^\tright tray keyboard volume/\tright agents tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$conf"
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

# the words in a part of the screen, each with where it is: "word x y" (enlarged
# three times for tesseract, the places in screen pixels)
words() { # x y w h
	grim -g "$1,$2 ${3}x$4" "$work/part.png" 2>/dev/null || return 1
	python3 - "$work/part.png" "$1" "$2" <<'PY'
import subprocess, sys
from PIL import Image
path, ox, oy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(path).convert("L")
im = im.resize((im.width * 3, im.height * 3), Image.LANCZOS)
im.save(path)
tsv = subprocess.run(["tesseract", path, "-", "tsv"], capture_output=True, text=True).stdout
for line in tsv.splitlines()[1:]:
    f = line.split("\t")
    if len(f) == 12 and f[11].strip():
        x = ox + (int(f[6]) + int(f[8]) // 2) // 3
        y = oy + (int(f[7]) + int(f[9]) // 2) // 3
        print(f[11].strip(), x, y)
PY
}
bar() { words 500 684 780 36; }
above() { words 600 0 680 680; }
has() { grep -q "^$1 " "$work/seen"; }
# until the taskbar shows (or, with !, no longer shows) a word, for up to $2 seconds
bar_until() { # [!] word seconds
	local not=false
	[ "$1" = "!" ] && { not=true; shift; }
	for _ in $(seq 1 "$2"); do
		bar > "$work/seen"
		if $not; then grep -q "^$1" "$work/seen" || return 0; else grep -q "^$1" "$work/seen" && return 0; fi
		sleep 1
	done
	return 1
}
where() { grep -m1 "^$1 " "$work/seen" | cut -d' ' -f2,3; }

# ---- on the taskbar: both accounts, each with its name and values
bar_until "Codex " 20
has Claude || fail "the taskbar does not name Claude: $(tr '\n' ' ' < "$work/seen")"
has Codex || fail "the taskbar does not name Codex: $(tr '\n' ' ' < "$work/seen")"
grep -q "^37" "$work/seen" && grep -q "^23" "$work/seen" ||
	fail "the taskbar does not show both accounts' values: $(tr '\n' ' ' < "$work/seen")"

# ---- a click on Codex opens its flyout, the others listed as accounts
set -- $(where Codex)
if [ $# -eq 2 ]; then
	"$pointer" 1280 720 click "$1" "$2" >/dev/null 2>&1
	sleep 1.5
	above > "$work/seen"
	has Accounts || fail "the flyout lists no accounts: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	grep -q "^23" "$work/seen" || fail "the flyout of Codex does not show its 23%"
	# a click on the row of Claude Code shows that account instead, the flyout open
	set -- $(grep "^Claude " "$work/seen" | sort -k3 -n | tail -n 1 | cut -d' ' -f2,3)
	if [ $# -eq 2 ]; then
		"$pointer" 1280 720 click "$1" "$2" >/dev/null 2>&1
		sleep 1.5
		above > "$work/seen"
		grep -q "^37" "$work/seen" || fail "a click on Claude Code's row did not show it"
		has Accounts || fail "the flyout closed at the click on an account"
	else
		fail "no row of Claude Code in the flyout"
	fi
	"$pointer" 1280 720 click 100 300 >/dev/null 2>&1 # elsewhere: it closes
	sleep 0.5
else
	fail "no Codex on the taskbar to click"
fi

# ---- "accounts" names Codex alone: Claude is gone from the taskbar
codex_id=codex:$codex
printf '\nwidget agents {\n\taccounts %s\n}\n' "$codex_id" >> "$conf"
ipc panel reload >/dev/null 2>&1
# (until the taskbar is back from reloading: Codex's values there, Claude not)
for _ in $(seq 1 15); do
	bar > "$work/seen"
	grep -qE "^(23|41)" "$work/seen" && ! grep -q "^Claude " "$work/seen" && break
	sleep 1
done
has Claude && fail "with accounts $codex_id the taskbar still shows Claude"
grep -qE "^(23|41)" "$work/seen" ||
	fail "with accounts $codex_id Codex is gone too: $(tr '\n' ' ' < "$work/seen")"

# ---- the claude widget of an older config: the same widget, of its account alone
sed -i -e 's/\tright agents tray/\tright claude tray/' -e '/^widget agents {/,/^}/d' "$conf"
ipc panel reload >/dev/null 2>&1
bar_until "37" 10 || # (tesseract reads the % sign as 9 at times)
	fail "the claude widget of an older config does not show Claude Code's 37%: $(tr '\n' ' ' < "$work/seen")"
has Codex && fail "the claude widget of an older config shows Codex too"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
