#!/bin/bash
# The AI agent usage widget on the desktop with a Claude Code folder and a Codex
# folder with a history (no real agent is run or touched):
#  - the style "together" of before is All agents in one chart (overlay) now:
#    both named, the five-hour quota at the top of the card, the weekly below it;
#  - the chart of the week of one account marks where its 5-hour limit resets;
#  - Antigravity's groups of models (Gemini, Claude/GPT, Argon) are each on the taskbar
#    with their own limits;
#  - the flyout stays the one of an account, its last three 5-hour windows.
# What is shown is read with tesseract.
#
# usage: agents_charts.sh <build dir> <source dir>
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME/tileWin/claude-usage" \
	"$XDG_STATE_HOME/tileWin/codex-usage" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$claude" \
	"$codex/sessions/$day"
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
# Claude Code: on day 6 of its week (it resets in a day), Codex on day 2 (in five days);
# the history of each: readings every hour of the last days, the week before too
history() { # file week_reset five_reset week_used
	python3 - "$1" "$now" "$2" "$3" "$4" <<'PY'
import sys
path, now, week, five, used = sys.argv[1], *map(int, sys.argv[2:5]), float(sys.argv[5])
W, H = 7 * 86400, 5 * 3600
with open(path, "w") as f:
    for t in range(now - 10 * 86400, now - 60, 3600):
        reset = week if t > week - W else week - W  # this week or the one before
        r5 = five - ((five - t - 1) // H) * H       # 5-hour windows one after the other
        used5 = (t - (r5 - H)) / H * 60
        used7 = min(100, used * (t - (reset - W)) / (now - (week - W)))
        f.write(f"{t} {used5:.1f} {r5} {used7:.1f} {reset}\n")
PY
}
history "$XDG_STATE_HOME/tileWin/claude-usage/$(echo "$claude" | tr / -).history" \
	$((now + 86400)) $((now + 3600)) 52
history "$XDG_STATE_HOME/tileWin/codex-usage/$(echo "$codex" | tr / -).history" \
	$((now + 5 * 86400)) $((now + 4 * 3600)) 25
printf '{"oauthAccount": {"emailAddress": "me@example.org"}}\n' > "$claude/.claude.json"
printf '{}\n' > "$claude/settings.json"
"$tool" connect "$claude" >/dev/null || fail "connect failed"
command=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["statusLine"]["command"])' \
	"$claude/settings.json")
printf '{"rate_limits":{"five_hour":{"used_percentage":37,"resets_at":%d},"seven_day":{"used_percentage":52,"resets_at":%d}}}' \
	$((now + 3600)) $((now + 86400)) | sh -c "$command" >/dev/null
{
	printf '{"timestamp":"2026-01-01T00:00:00.000Z","type":"session_meta","payload":{}}\n'
	printf '{"timestamp":"%s","type":"event_msg","payload":{"type":"token_count","info":null,"rate_limits":{"limit_id":"codex","primary":{"used_percent":23,"window_minutes":300,"resets_at":%d},"secondary":{"used_percent":25,"window_minutes":10080,"resets_at":%d},"plan_type":"plus"}}}\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%S.000Z)" $((now + 4 * 3600)) $((now + 5 * 86400))
} > "$codex/sessions/$day/rollout-test.jsonl"

sed -e 's/^\tright tray keyboard volume/\tright agents tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$conf"
printf 'desktop_widgets {\n\tagents {\n\t\tstyle together\n\t\tsize 5x4\n\t}\n}\n' >> "$conf"
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x900 pos 0 0
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

# the words in a part of the screen, each with where it is: "word x y"
words() { # x y w h
	grim -g "$1,$2 ${3}x$4" "$work/part.png" 2>/dev/null || return 1
	python3 - "$work/part.png" "$1" "$2" <<'PY'
import subprocess, sys
from PIL import Image, ImageOps, ImageStat
path, ox, oy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(path).convert("L")
if ImageStat.Stat(im).mean[0] < 128:
    im = ImageOps.invert(im)
im = ImageOps.autocontrast(im)
im = im.resize((im.width * 3, im.height * 3), Image.LANCZOS)
im.save(path)
tsv = subprocess.run(["tesseract", path, "-", "--psm", "11", "tsv"], capture_output=True,
    text=True).stdout
for line in tsv.splitlines()[1:]:
    f = line.split("\t")
    if len(f) == 12 and f[11].strip():
        x = ox + (int(f[6]) + int(f[8]) // 2) // 3
        y = oy + (int(f[7]) + int(f[9]) // 2) // 3
        print(f[11].strip(), x, y)
PY
}
has() { grep -q "^$1 " "$work/seen"; }
count() { grep -c "^$1" "$work/seen"; }
# the flyout of the widget, opened by a click on Claude on the taskbar (its top part read)
open_flyout() {
	local at=""
	for _ in $(seq 1 20); do
		words 500 864 780 36 > "$work/bar"
		at=$(grep -m1 "^Claude " "$work/bar" | cut -d' ' -f2,3)
		[ -n "$at" ] && grep -q "^Codex " "$work/bar" && break
		sleep 1
	done
	[ -n "$at" ] || { fail "no Claude on the taskbar: $(tr '\n' ' ' < "$work/bar")"; return 1; }
	"$pointer" 1280 900 click $at >/dev/null 2>&1
	sleep 1.5
	words 300 0 980 430 > "$work/seen"
}
close_flyout() {
	"$pointer" 1280 900 click 100 300 >/dev/null 2>&1
	sleep 0.5
}

# the card in the top right corner (x y w h of it alone: tesseract reads the small
# labels of the charts only without the wallpaper around), read until it shows both agents
card() {
	for _ in $(seq 1 20); do
		words "$@" > "$work/seen"
		has Codex && has Claude && return 0
		sleep 1
	done
	return 1
}
at() { grep -m1 "^$1 " "$work/seen" | cut -d' ' -f"$2"; }

# ---- "together", the chart of all agents of before, draws the one of now: the
# five-hour and the weekly quota of all accounts, one below the other
card 715 15 490 390 || fail "the card does not name both agents: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
has Five-hour && has Weekly || fail "the card has not both quota charts: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
five_y=$(at Five-hour 3)
week_y=$(at Weekly 3)
[ -n "$five_y" ] && [ -n "$week_y" ] && [ "$week_y" -gt $((five_y + 60)) ] ||
	fail "the weekly chart is not below the five-hour one ($five_y, $week_y)"
has Now || fail "the charts do not mark now: $(tr '\n' ' ' < "$work/seen" | head -c 300)"

# ---- a chart of the week of one account: a dashed line where its 5-hour limit resets next
sed -i 's/^\t\tstyle together/\t\tstyle week/' "$conf"
ipc panel reload >/dev/null 2>&1
sleep 2
card 715 15 490 390 || fail "the week style does not name both agents"
has resets || fail "the chart of the week does not mark where the 5-hour limit resets: $(tr '\n' ' ' < "$work/seen" | head -c 300)"

# ---- the flyout stays the one of the account clicked
if open_flyout; then
	has three || fail "the flyout has not the account's last three 5-hour windows: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	close_flyout
fi

# ---- Antigravity's three groups of models, each with its limits: on the taskbar each of
# its own (Argon has only a weekly one), also Claude/GPT, never used, at 0%
agy=$XDG_STATE_HOME/tileWin/agent-usage/agy
mkdir -p "$agy"
python3 - "$agy/account.jsonl" "$now" <<'PY'
import sys, json, datetime
path, now = sys.argv[1], int(sys.argv[2])
iso = lambda t: datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
with open(path, "w") as f:
    for ago in range(86400, 0, -3600):
        k = 1 - ago / 86400
        q = {"gemini-5h": {"remaining_fraction": 0.8, "reset_time": iso(now + 3600)},
             "gemini-weekly": {"remaining_fraction": 1 - 0.4 * k, "reset_time": iso(now + 2 * 86400)},
             # (Claude/GPT never used: all of it left, no reset)
             "3p-5h": {"remaining_fraction": 1, "reset_time": "1970-01-01T00:00:00Z"},
             "3p-weekly": {"remaining_fraction": 1, "reset_time": "1970-01-01T00:00:00Z"},
             "gemini-argon-wtu-weekly": {"remaining_fraction": 1 - 0.6 * k, "reset_time": iso(now + 86400)}}
        f.write(json.dumps({"at": now - ago, "data": {"quota": q, "pid": 0}}) + "\n")
PY
printf 'widget agents {\n\taccounts claude:%s codex:%s antigravity\n}\n' "$claude" "$codex" >> "$conf"
ipc panel reload >/dev/null 2>&1
for _ in $(seq 1 20); do
	words 0 864 1280 36 > "$work/seen"
	has Argon && break
	sleep 1
done
[ "$(count AG)" -ge 3 ] && has Gemini && has Argon ||
	fail "the taskbar does not show Antigravity's groups each: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
# (Argon: its weekly limit alone, no "5h" after its name)
argon_x=$(grep -m1 "^Argon " "$work/seen" | cut -d' ' -f2)
[ -n "$argon_x" ] && awk -v x="$argon_x" '$1 == "5h" && $2 > x { found = 1 } END { exit !found }' \
	"$work/seen" && fail "Argon shows a 5-hour limit it does not have: $(tr '\n' ' ' < "$work/seen" | head -c 300)"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
