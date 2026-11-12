#!/bin/sh
# The chart of the last 24 hours in the battery flyout, in a nested tileWin
# with the taskbar, a fake /sys/class/power_supply (TILEWIN_SYS_POWER) and a
# history written beforehand with a shutdown, a time charging and a
# hibernation in it:
#  - the taskbar takes the battery down in the history file while it runs;
#  - the flyout draws the chart: a line in the color of the charts and one in
#    green where it charged, across the whole width, and over the times the
#    machine was off or hibernating a straight line, never a drop to the bottom;
#  - without a battery the flyout has no chart, and is as high as it was before.
# BATTERY_SHOTS=<folder> keeps the screenshots.
#
# usage: battery_chart.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-batchart-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME/tileWin" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME" "$work/power/BAT0" "$work/power/AC" "$work/nopower"
echo win10 > "$XDG_CONFIG_HOME/tileWin/current-theme"
echo Battery > "$work/power/BAT0/type"
echo Mains > "$work/power/AC/type"
echo 58 > "$work/power/BAT0/capacity"
echo Discharging > "$work/power/BAT0/status"
echo 0 > "$work/power/AC/online"

# The day before: discharging, shut down for 8 hours, charging, discharging,
# hibernated for 50 minutes, and 58% now. Lowest is 38%.
history=$XDG_STATE_HOME/tileWin/battery-history
python3 -I - "$history" <<'EOF'
import sys, time
now = int(time.time())
h = 3600
lines = ["# unix time, battery percent, c when charging", "%d 95 -" % (now - 30 * h)]
for i in range(37): # 22 h ago to 16 h ago, 85% down to 40%
    lines.append("%d %d -" % (now - 22 * h + i * 600, 85 - i * 45 // 36))
lines.append("%d 38 c" % (now - 8 * h)) # after the shutdown
for i in range(1, 19): # charging for three hours
    lines.append("%d %d c" % (now - 8 * h + i * 600, 38 + i * 57 // 18))
for i in range(1, 25): # discharging for four hours
    lines.append("%d %d -" % (now - 5 * h + i * 600, 95 - i * 33 // 24))
lines.append("%d 60 -" % (now - 600)) # after hibernating
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
EOF
before=$(wc -l < "$history")

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
stop() {
	ipc exit >/dev/null 2>&1
	attempt=0
	while [ -S "$sock" ] && ipc -t get_version >/dev/null 2>&1 && [ $attempt -lt 30 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	sleep 0.5
}
cleanup() {
	stop
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
polkit_agent disable
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

start() { # the power supply folder
	rm -f "$work/display"
	env -u WAYLAND_DISPLAY -u DISPLAY \
		TILEWIN_SYS_POWER="$1" TILEWIN_BATTERY_POLL_MS=1000 \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
		WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
		"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
	attempt=0
	while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	if [ ! -s "$work/display" ]; then
		echo "the nested compositor never came up"
		tail -n 3 "$work/log"
		exit 1
	fi
	export WAYLAND_DISPLAY=$(cat "$work/display")
}

# The pixels of a screenshot in ppm: python helpers read by the checks below.
ppm_lib='
import sys
def load(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    w, h = int(fields[1]), int(fields[2])
    px = data[pos + 1:]
    return w, h, lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
def like(c, want, t):
    return all(abs(a - b) <= t for a, b in zip(c, want))
'
# The box of the flyout: "x y w h" of where a screenshot with it differs from
# one without, above the taskbar.
flyout_box() { # closed.ppm open.ppm
	python3 -I -c "$ppm_lib"'
w, h, a = load(sys.argv[1])
_, _, b = load(sys.argv[2])
xs, ys = [], []
for y in range(0, h - 60):
    for x in range(0, w, 2):
        if a(x, y) != b(x, y):
            xs.append(x)
            ys.append(y)
if xs:
    print(min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
' "$1" "$2"
}
# What the chart in the flyout box looks like: "<columns of line> <holes>
# <green pixels> <line pixels> <lowest line above the bottom of the chart>".
# The line is in the chart color of win10 dark, green where it charged; the
# bottom of the chart is the lowest row of the fill under the line, and only
# the 88 rows of the chart above it are looked at.
chart_scan() { # open.ppm x y w h
	python3 -I -c "$ppm_lib"'
w, h, get = load(sys.argv[1])
fx, fy, fw, fh = map(int, sys.argv[2:6])
line, green, fill = (118, 185, 237), (108, 203, 95), (67, 85, 100)
bottom = 0
for y in range(fy, fy + fh):
    for x in range(fx, fx + fw, 4):
        if like(get(x, y), fill, 6):
            bottom = y
cols, greens, lines, lowest = set(), 0, 0, 0
for y in range(max(fy, bottom - 92), bottom + 2 if bottom else fy):
    for x in range(fx, fx + fw):
        c = get(x, y)
        if like(c, line, 40) or like(c, green, 40):
            cols.add(x)
            lowest = max(lowest, y)
            if like(c, green, 40):
                greens += 1
            else:
                lines += 1
holes = sum(1 for x in range(min(cols), max(cols)) if x not in cols) if cols else 0
print(len(cols), holes, greens, lines, bottom - lowest)
' "$@"
}

# Waits until the taskbar is drawn: the bottom of the screen is no longer the
# wallpaper alone.
wait_taskbar() {
	attempt=0
	while [ $attempt -lt 150 ]; do
		grim -g "0,700 1280x20" -t ppm "$work/bottom.ppm" 2>/dev/null
		drawn=$(python3 -I -c "$ppm_lib"'
w, h, get = load(sys.argv[1])
print(sum(1 for y in range(h) for x in range(0, w, 8) if not like(get(x, y), (32, 64, 112), 4)))
' "$work/bottom.ppm" 2>/dev/null)
		[ "${drawn:-0}" -gt 20 ] && return 0
		sleep 0.2
		attempt=$((attempt + 1))
	done
	fail "the taskbar never came up"
}

# ---- with a battery
start "$work/power"
wait_taskbar
attempt=0
while [ "$(wc -l < "$history")" -le "$before" ] && [ $attempt -lt 150 ]; do # the taskbar is up
	sleep 0.2
	attempt=$((attempt + 1))
done
sleep 1
after=$(wc -l < "$history")
[ "$after" -gt "$before" ] || fail "the taskbar takes nothing down in the history ($before lines, $after now)"
tail -n 1 "$history" | grep -q " 58 -$" || fail "the last line of the history is no 58% discharging: $(tail -n 1 "$history")"
ipc color_scheme dark >/dev/null
sleep 1
grim -t ppm "$work/closed.ppm"
ipc panel power >/dev/null
sleep 1.5
grim -t ppm "$work/open.ppm"
[ -n "${BATTERY_SHOTS:-}" ] && grim "$work/open.png"
box=$(flyout_box "$work/closed.ppm" "$work/open.ppm")
height_battery=0
if [ -z "$box" ]; then
	fail "panel power opens no flyout"
else
	echo "the flyout is at $box"
	height_battery=$(echo "$box" | cut -d' ' -f4)
	set -- $(chart_scan "$work/open.ppm" $box)
	echo "chart: line over $1 columns with $2 missing, $3 pixels green, $4 not," \
		"lowest $5 px above the bottom"
	[ "$1" -ge 300 ] || fail "the line does not go across the chart ($1 columns)"
	[ "$2" -le 2 ] || fail "the line has holes ($2 columns), a gap is not bridged"
	[ "$3" -ge 20 ] || fail "the time charging is not green ($3 pixels)"
	[ "$4" -ge 200 ] || fail "the line is missing ($4 pixels)"
	# 38% at the lowest, of a chart 88 pixels high: about 33 above its bottom
	[ "$5" -ge 25 ] || fail "the line drops to the bottom ($5 px above it): a gap drawn as empty?"
	ipc panel power >/dev/null
	sleep 1
fi
if [ -n "${BATTERY_SHOTS:-}" ]; then
	mkdir -p "$BATTERY_SHOTS" && cp "$work/open.png" "$BATTERY_SHOTS/"
fi
stop
grep -q " 58 -$" "$history" || fail "the history lost its readings"

# ---- without a battery: no chart, and the flyout as high as before it
start "$work/nopower"
wait_taskbar
sleep 1
ipc color_scheme dark >/dev/null
sleep 1
grim -t ppm "$work/closed.ppm"
ipc panel power >/dev/null
sleep 1.5
grim -t ppm "$work/open.ppm"
box=$(flyout_box "$work/closed.ppm" "$work/open.ppm")
if [ -z "$box" ]; then
	fail "panel power opens no flyout without a battery"
else
	height=$(echo "$box" | cut -d' ' -f4)
	echo "without a battery the flyout is at $box"
	[ "$height_battery" -eq 0 ] || [ $((height_battery - height)) -eq 146 ] ||
		fail "the chart does not make the flyout 146 px higher ($height_battery against $height)"
	set -- $(chart_scan "$work/open.ppm" $box)
	[ "$4" -lt 20 ] || fail "a chart without a battery ($4 pixels of line)"
fi
stop

ipc -t get_version >/dev/null 2>&1 && fail "tileWin did not stop"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
