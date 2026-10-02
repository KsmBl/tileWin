#!/bin/sh
# Sticky Notes on the desktop, in a nested tileWin with the taskbar: a note
# made with "panel note new" is typed into, moved by its strip, colored,
# still there after the taskbar starts again, made bigger by its corner and
# thrown away with its x;
# each time the file of the note says so.
#
# usage: sticky_notes.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$pointer" "$keys"; do
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
sock=$XDG_RUNTIME_DIR/tw-notes-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

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
ipc() { "$msg" -s "$sock" "$@"; }
pixel() { grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'; }
# the mean brightness of a part of the screen
mean() {
	grim -g "$1,$2 $3x$4" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
	while data[pos:pos + 1].isspace():
		pos += 1
	start = pos
	while not data[pos:pos + 1].isspace():
		pos += 1
	fields.append(data[start:pos])
pixels = data[pos + 1:]
print(sum(pixels) // max(1, len(pixels)))'
}

notes=$XDG_DATA_HOME/tileWin/notes
note() { ls "$notes"/*.txt 2>/dev/null | head -n 1; }
header() { head -n 1 "$(note)" 2>/dev/null; }
sleep 2
ipc theme win7 >/dev/null
sleep 1.5

ipc panel note new >/dev/null
sleep 1
[ -n "$(note)" ] || fail "panel note new made no note"
# near the top right of a 1280x720 screen: 220x200 at 1000,60
[ "$(header)" = "#note 1000 60 220 200 yellow HEADLESS-1" ] || fail "the new note is $(header)"

"$pointer" 1280 720 click 1100 160 >/dev/null 2>&1
sleep 0.4
"$keys" text milk key none return text eggs >/dev/null 2>&1
sleep 1.5
[ "$(tail -n +2 "$(note)")" = "$(printf 'milk\neggs')" ] ||
	fail "typing into the note did not keep milk and eggs: $(tail -n +2 "$(note)")"

# its strip, dragged 200 to the left and down
"$pointer" 1280 720 drag 1100 70 900 270 >/dev/null 2>&1
sleep 1
set -- $(header)
[ "${2:-0}" -ge 790 ] && [ "${2:-0}" -le 810 ] && [ "${3:-0}" -ge 250 ] && [ "${3:-0}" -le 270 ] ||
	fail "dragging the strip did not move the note to 800,260: $(header)"

# the color, as its menu sets it
ipc "panel note color \"$(note)\" blue" >/dev/null
sleep 0.5
case "$(header)" in *" blue "*) ;; *) fail "the note did not turn blue: $(header)" ;; esac

# the taskbar again: the note is there again, blue paper where it was
ipc restart panel >/dev/null
sleep 3
rgb=$(grim -g "900,400 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //')
set -- $rgb
[ "${3:-0}" -gt 200 ] && [ "${1:-255}" -lt 230 ] && [ "${1:-0}" -gt 150 ] ||
	fail "after the taskbar started again there is no blue note at 900,400 ($rgb)"

# its corner, pulled 80 to the right and 60 down
"$pointer" 1280 720 drag 1015 455 1095 515 >/dev/null 2>&1
sleep 1
set -- $(header)
[ "${4:-0}" -ge 290 ] && [ "${4:-0}" -le 310 ] && [ "${5:-0}" -ge 250 ] && [ "${5:-0}" -le 270 ] ||
	fail "pulling the corner did not make the note 300x260: $(header)"

# its x, at the right end of the strip
"$pointer" 1280 720 move 900 400 wait 200 click $((800 + ${4:-220} - 12)) 271 >/dev/null 2>&1
sleep 1
[ -z "$(note)" ] || fail "the x did not throw the note away"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
