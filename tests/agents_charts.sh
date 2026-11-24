#!/bin/bash
# The charts of the AI agent usage widget's flyout with all accounts in one chart
# (charts together), a Claude Code folder and a Codex folder with a history (no
# real agent is run or touched):
#  - a legend names both accounts, each a line in the chart of the 5-hour and of
#    the weekly limits;
#  - relative by default: each line from the start of its window to its reset,
#    the weeks along "day 1" to "day 7" (Claude Code on day 6, Codex on day 2);
#  - chart_scale absolute: along the clock, the day from midnight ("today") and
#    the week from Monday to Sunday, the wheel back to the week before;
#  - the account shown alone (no charts setting) keeps its last three 5-hour windows.
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
printf 'widget agents {\n\tcharts together\n}\n' >> "$conf"
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

# ---- together, relative by default: a legend of both, the weeks in their days
if open_flyout; then
	# (the legend over the charts, under the headline)
	legend_y=$(grep -m1 "^Codex " "$work/seen" | cut -d' ' -f3)
	[ -n "$legend_y" ] && [ "$legend_y" -lt 130 ] && has Claude ||
		fail "no legend of both accounts over the charts: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	[ "$(count day)" -ge 5 ] ||
		fail "the weekly chart of all accounts is not from start to reset in days: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	has reset || fail "the titles do not say from start to reset"
	close_flyout
fi

# ---- absolute: the day from midnight, the week Monday to Sunday; the wheel goes a week back
sed -i 's/^\tcharts together/&\n\tchart_scale absolute/' "$conf"
ipc panel reload >/dev/null 2>&1
sleep 2
if open_flyout; then
	has Mon && has Sun || fail "the absolute week is not Monday to Sunday: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	has today || fail "the absolute 5-hour chart is not of today"
	[ "$(count day)" -ge 5 ] && fail "the absolute charts still count the days of the windows"
	set -- $(grep -m1 "^Mon " "$work/seen" | cut -d' ' -f2,3)
	if [ $# -eq 2 ]; then
		"$pointer" 1280 900 scroll $(($1 + 100)) $(($2 - 40)) -1 >/dev/null 2>&1
		sleep 1
		words 300 0 980 430 > "$work/seen"
		has back || fail "the wheel over the week did not go back: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	fi
	close_flyout
fi

# ---- the account shown alone, as before
sed -i -e '/^\tcharts together/d' -e '/^\tchart_scale absolute/d' "$conf"
ipc panel reload >/dev/null 2>&1
sleep 2
if open_flyout; then
	has three || fail "the account alone has not its last three 5-hour windows: $(tr '\n' ' ' < "$work/seen" | head -c 300)"
	[ "$(count day)" -ge 5 ] && fail "the account alone is drawn in days of its window"
	close_flyout
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
