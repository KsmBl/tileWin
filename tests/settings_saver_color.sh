#!/bin/sh
# The colour of the Matrix screen saver is picked with a colour button on the
# Screen saver page of the settings, not from a list. In a nested tileWin with
# the settings on that page, the screen saver on and Matrix picked with
# "matrix_color red", the name of the list before:
#  - the page shows a colour button in that red, and the preview rains red;
#  - a click on it opens the colour dialog, and a blue picked there and
#    selected is written to taskbar.conf as "#rrggbb";
#  - the button and the preview turn blue.
#
# usage: settings_saver_color.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-saver-color-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
printf 'idle_timeout screensaver 600\n' > "$XDG_CONFIG_HOME/tileWin/common.conf"
printf 'screensaver {\n\tname matrix\n\tmatrix_color red\n}\n' > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# a tall screen, so the settings show the page down to the saver's own settings
cat > "$work/tilewin.conf" <<EOC
wallpaper solid #008080
session_restore no
panel_command none
animations off
output HEADLESS-1 mode --custom 1280x1400 pos 0 0
exec $settings --page=screensaver
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

# "<x> <y> <width> <height>" of the window with that title, nothing without one
window() {
	ipc -t get_tree | python3 -I -c '
import json, sys
def walk(n):
    if n.get("pid") and n.get("name") == sys.argv[1]:
        r = n["rect"]
        print(r["x"], r["y"], r["width"], r["height"])
        return True
    return any(walk(c) for c in n.get("nodes", []) + n.get("floating_nodes", []))
walk(json.load(sys.stdin))' "$1"
}
wait_window() {
	attempt=0
	while [ -z "$(window "$1")" ] && [ $attempt -lt 50 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	sleep 1
}
# the middle of the pixels of a screenshot in the right part of the page that
# are that colour, near enough: "<x> <y> <count>", nothing for none
find_color() {
	python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
p = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1])
want = int(sys.argv[2][1:], 16)
wr, wg, wb = want >> 16, (want >> 8) & 0xff, want & 0xff
n, stride, data = p.get_n_channels(), p.get_rowstride(), p.get_pixels()
xs, ys = [], []
for y in range(0, p.get_height()):
    for x in range(1000, p.get_width()):
        i = y * stride + x * n
        if abs(data[i] - wr) + abs(data[i + 1] - wg) + abs(data[i + 2] - wb) < 12:
            xs.append(x)
            ys.append(y)
if len(xs) > 50:
    print((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2, len(xs))' "$@"
}
# the coloured pixels of the preview with each colour the strongest: "<red> <green> <blue>"
preview_tint() {
	python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
p = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1])
n, stride, data = p.get_n_channels(), p.get_rowstride(), p.get_pixels()
most = [0, 0, 0]
for y in range(150, 500):
    for x in range(400, 900):
        i = y * stride + x * n
        c = data[i:i + 3]
        if max(c) - min(c) >= 40:
            most[c.index(max(c))] += 1
print(*most)' "$1"
}
# the colour written for the Matrix
written() {
	sed -n 's/^[[:space:]]*matrix_color[[:space:]]*//p' "$XDG_CONFIG_HOME/tileWin/taskbar.conf" |
		tr -d '"'
}

wait_window "tileWin Settings"
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 2
grim "$work/red.png"
set -- $(preview_tint "$work/red.png") 0 0 0
echo "preview with red: $1 red, $2 green, $3 blue pixels"
[ "$1" -gt 100 ] && [ "$1" -gt $((10 * ($2 + $3))) ] || fail "the preview does not rain red"
button=$(find_color "$work/red.png" "#ff4d4d")
if [ -z "$button" ]; then
	fail "no colour button in the red of the old \"matrix_color red\""
else
	set -- $button
	bx=$1 by=$2
	echo "the colour button is at $bx,$by"
	"$pointer" 1280 1400 move "$bx" "$by" wait 300 click "$bx" "$by" >/dev/null 2>&1
	wait_window "Pick a Color"
	dialog=$(window "Pick a Color")
	if [ -z "$dialog" ]; then
		fail "a click on the colour button opens no colour dialog"
	else
		set -- $dialog
		dx=$1 dy=$2
		# the third blue of the palette, then Select at the top right
		"$pointer" 1280 1400 move $((dx + 32)) $((dy + 151)) wait 300 \
			click $((dx + 32)) $((dy + 151)) wait 300 \
			move $((dx + 436)) $((dy + 39)) wait 300 click $((dx + 436)) $((dy + 39)) \
			wait 300 move 640 1350 >/dev/null 2>&1
		sleep 2
		[ -z "$(window "Pick a Color")" ] || fail "the colour dialog is still open after Select"
		color=$(written)
		echo "written: matrix_color $color"
		case $color in
		\#[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])
			python3 -I -c '
import sys
v = int(sys.argv[1][1:], 16)
r, g, b = v >> 16, (v >> 8) & 0xff, v & 0xff
sys.exit(0 if b > r + 60 and b > g else 1)' "$color" ||
				fail "the colour written, $color, is not the blue picked"
			grim "$work/blue.png"
			[ -n "$(find_color "$work/blue.png" "$color")" ] ||
				fail "the colour button does not show the blue picked"
			set -- $(preview_tint "$work/blue.png") 0 0 0
			echo "preview with $color: $1 red, $2 green, $3 blue pixels"
			[ "$3" -gt 100 ] && [ "$3" -gt $((10 * ($1 + $2))) ] ||
				fail "the preview does not rain in the blue picked"
			;;
		*)
			fail "the colour picked is not written as #rrggbb (\"$color\")"
			;;
		esac
	fi
fi
grep -q "CRITICAL" "$work/log" && fail "GTK complained: $(grep -m 1 CRITICAL "$work/log")"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the colour of the Matrix is picked with a colour button"
