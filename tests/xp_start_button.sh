#!/bin/sh
# The Start button of the Windows XP themes, drawn like the real one: the
# green of its glossy band near the top and of its lower half where the real
# button has them, and the waving flag on its left (its red panel above, its
# yellow one below), in Windows XP and in Windows XP (dark).
#
# usage: xp_start_button.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-xpstart-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

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
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

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

pixel() { grim -g "$1,$2 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ' | sed 's/^ //'; }
for theme in winxp winxp-dark; do
	ipc theme $theme >/dev/null
	sleep 2.5
	# the taskbar of XP is 30 high at the bottom of a 1280x720 screen; the
	# real button is #89be8b in its glossy band and #3a933a in its lower half
	band=$(pixel 60 692)
	lower=$(pixel 90 711)
	[ "$(python3 -c "import sys; a=[int(v) for v in sys.argv[1:4]]; b=bytes.fromhex('89be8b'); print(sum(abs(x-y) for x,y in zip(a,b)) <= 70)" $band)" = True ] ||
		fail "$theme: the glossy band of the start button is $band, not near #89be8b"
	[ "$(python3 -c "import sys; a=[int(v) for v in sys.argv[1:4]]; b=bytes.fromhex('3a933a'); print(sum(abs(x-y) for x,y in zip(a,b)) <= 70)" $lower)" = True ] ||
		fail "$theme: the lower half of the start button is $lower, not near #3a933a"
	# the flag: red above, yellow below on its left side
	found=$(grim -g "6,694 24x24" -t ppm - 2>/dev/null | python3 -c '
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
w = int(fields[1])
px = data[pos + 1:]
red = yellow = None
for i in range(0, len(px) - 2, 3):
	r, g, b = px[i], px[i + 1], px[i + 2]
	y = (i // 3) // w
	if r > 190 and g < 120 and b < 80 and red is None:
		red = y
	if r > 200 and g > 160 and b < 90:
		yellow = y
print("yes" if red is not None and yellow is not None and red < yellow else "no")')
	[ "$found" = yes ] || fail "$theme: no flag with its red panel above its yellow one"
done

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
