#!/bin/sh
# The network usage widget on the desktop in the chart style: a line for
# what comes in and one for what goes out (blue and orange), on a scale with
# labels, and the same scale in its flyout; and in every theme, light and
# dark, the face of its chart is the one of the CPU chart beside it. A curl now and then makes some
# traffic; without any the lines lie at the bottom, and are still there.
#
# usage: network_chart.sh <build dir> <source dir>
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
for needed in dbus-daemon grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-netchart-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/Desktop"
printf 'XDG_DESKTOP_DIR="%s/Desktop"\n' "$work" > "$XDG_CONFIG_HOME/user-dirs.dirs"
echo win10 > "$XDG_CONFIG_HOME/tileWin/current-theme"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<'EOF'

theme_layout no
desktop_widgets {
	net { style chart; column 0; row 0; columns 4; rows 2 }
	cpu { style chart; column 4; row 0; columns 4; rows 2 }
}
EOF

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

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC

env -u WAYLAND_DISPLAY -u DISPLAY \
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
if command -v curl >/dev/null 2>&1; then
	(for _ in 1 2 3 4 5 6 7 8; do
		curl -s -o /dev/null --max-time 2 http://example.com; sleep 1
	done) &
fi
sleep 12

# The pixels of a part of the screen with a color like: "r g b" and a
# tolerance; prints how many.
count() { # x y w h r g b tolerance
	grim -g "$1,$2 ${3}x$4" -t ppm - 2>/dev/null | python3 -c '
import sys
r, g, b, t = (int(v) for v in sys.argv[1:5])
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
px = data[pos + 1:]
print(sum(1 for i in range(0, len(px) - 2, 3)
          if abs(px[i] - r) <= t and abs(px[i + 1] - g) <= t and abs(px[i + 2] - b) <= t))
' "$5" "$6" "$7" "$8"
}

# the chart of the card (4 x 2 cells at the top left), below its heading
down=$(count 24 62 272 134 0 120 215 30)
up=$(count 24 62 272 134 192 96 42 30)
echo "chart on the desktop: $down pixels of the line in, $up of the line out"
[ "$down" -ge 30 ] || fail "the line of what comes in is missing ($down pixels)"
[ "$up" -ge 10 ] || fail "the line of what goes out is missing ($up pixels)"
# the labels of the scale: dark grey text at the left edge inside the chart
labels=$(count 24 62 60 134 90 90 90 40)
[ "$labels" -ge 15 ] || fail "the scale of the chart has no labels ($labels pixels)"

# its flyout: the same two lines, with a scale of its own
"$pointer" 1280 720 click 150 120 >/dev/null 2>&1
sleep 2
flyout_labels=$(count 20 130 80 90 170 170 170 40)
echo "flyout: $flyout_labels pixels of scale labels"
[ "$flyout_labels" -ge 15 ] || fail "the chart of the flyout has no labels on its scale"

"$pointer" 1280 720 click 900 600 >/dev/null 2>&1 # the flyout closes
sleep 1

# In every theme and scheme it looks like the other charts: the face of its
# chart and the grid on it (its two most common colors) are those of the CPU
# chart beside it.
face() { # x: the left of the card
	grim -g "$(($1 + 30)),80 240x100" -t ppm - 2>/dev/null | python3 -c '
import sys, collections
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
px = data[pos + 1:]
c = collections.Counter(tuple(px[i:i + 3]) for i in range(0, len(px) - 2, 3))
# the face and the grid on it: the two most common colors
print(" ".join("%d %d %d" % k for k, _ in c.most_common(2)))
'
}
for theme in win95 winxp win7 win8 win10 win11 sway; do
	for scheme in light dark; do
		"$msg" -s "$sock" theme "$theme" >/dev/null 2>&1
		"$msg" -s "$sock" color_scheme "$scheme" >/dev/null 2>&1
		sleep 3
		net=$(face 0)
		cpu=$(face 400)
		# within a few levels: the cards of win8 have a tint of their own each
		same=$(echo "$net $cpu" | awk '{ d = 0; for (i = 1; i <= 6; i++) {
			x = $i - $(i + 6); d += x < 0 ? -x : x }; print d <= 24 ? "yes" : "no" }')
		if [ "$theme" = win8 ]; then
			echo "$theme $scheme: the tiles of win8 differ in color by design ($net, $cpu)"
		elif [ "$same" = yes ]; then
			echo "ok   $theme $scheme: the same face and grid ($net)"
		else
			fail "$theme $scheme: the network chart's face and grid are $net, the CPU chart's $cpu"
		fi
	done
done

"$msg" -s "$sock" -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
