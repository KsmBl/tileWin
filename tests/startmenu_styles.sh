#!/bin/sh
# Every style of Start menu in a theme that brings another one has to wear
# that theme's colors, not those of the Windows the style comes from: the
# colors the menu is mostly drawn in have to be those of the theme's menus (as
# the classic menu shows them), and the start screen of tiles is in the color
# of its title bars.
#
# usage: startmenu_styles.sh <build dir> <source dir>
set -u
. "$(dirname "$0")/wait.sh"

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
sock=$XDG_RUNTIME_DIR/tw-smstyles-$$.sock
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
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command $panel --debug 2>>$work/panel.log
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

# a checksum of the screen, and how often the taskbar has loaded its config and
# theme (it says so in its log)
screen() { grim -t ppm - 2>/dev/null | md5sum; }
loads() { grep -c 'Reloading panel config and theme' "$work/panel.log" 2>/dev/null; }
more_loads() { [ "$(loads)" -gt "$1" ]; }
changed_from() { [ "$(screen)" != "$1" ]; }
# the taskbar and the screen at rest: no load waiting to be done, nothing
# moving on the screen
rest_state() { loads; screen; }
at_rest() { wait_still 10 rest_state; }
# runs a command that makes the taskbar load its config and theme again, and
# waits until it has and has drawn everything anew
loaded() {
	_loads=$(loads)
	"$@" >/dev/null
	wait_until 10 more_loads "$_loads"
	at_rest
}
# runs an ipc command that changes the screen, and waits for it to be done
shown() {
	_before=$(screen)
	ipc "$@" >/dev/null
	wait_until 10 changed_from "$_before"
	at_rest
}
# the taskbar is drawn, at the top or the bottom: there is more than wallpaper
strip() { grim -g "0,$1 1280x40" -t ppm - 2>/dev/null | md5sum; }
panel_up() {
	_middle=$(strip 340)
	[ "$(strip 0)" != "$_middle" ] || [ "$(strip 680)" != "$_middle" ]
}
if ! wait_until 30 panel_up; then
	echo "the taskbar never came up"
	tail -n 3 "$work/log"
	exit 1
fi
at_rest
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

conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
# the color most of the start menu is drawn in: the most common one among the
# pixels that changed when it opened
dominant() {
	python3 - "$1" "$2" <<'PY'
import sys, collections
def read(path):
	data = open(path, 'rb').read()
	fields, pos = [], 0
	while len(fields) < 4:
		while data[pos:pos + 1].isspace():
			pos += 1
		start = pos
		while not data[pos:pos + 1].isspace():
			pos += 1
		fields.append(data[start:pos])
	return data[pos + 1:]
a, b = read(sys.argv[1]), read(sys.argv[2])
count = collections.Counter()
for i in range(0, min(len(a), len(b)) - 2, 3 * 7):
	if a[i:i + 3] != b[i:i + 3]:
		count[b[i:i + 3]] += 1
color = count.most_common(1)[0][0] if count else b'\0\0\0'
print('%02x%02x%02x' % tuple(color))
PY
}
close() { python3 -c "import sys; a, b = (int(x, 16) for x in sys.argv[1:]); print(sum(abs(((a >> s) & 255) - ((b >> s) & 255)) for s in (0, 8, 16)) <= ${3:-24})" "$1" "$2"; }
relayout() {
	sed -i "s/^\t#\? \?layout [a-z]*$/\tlayout $1/" "$conf"
	ipc panel reload
}
show() {
	loaded relayout "$1"
	grim -t ppm "$work/without.ppm"
	shown panel startmenu toggle
	grim -t ppm "$work/with.ppm"
	shown panel startmenu toggle
	dominant "$work/without.ppm" "$work/with.ppm"
}

for theme in win1 win95 win11; do
	loaded ipc theme $theme
	menu=$(show classic)
	case $theme in
		win1) styles="twocolumn list centered progman" ;;
		win95) styles="twocolumn list centered" ;;
		win11) styles="twocolumn list" ;;
	esac
	for style in $styles; do
		got=$(show $style)
		[ "$(close "$menu" "$got")" = True ] ||
			fail "$theme: the $style menu is mostly #$got, not the #$menu of its menus"
	done
done

# the start screen of tiles in the blue of the title bars of Windows 1
loaded ipc theme win1
got=$(show tiles)
[ "$(close 5555ff "$got")" = True ] || fail "win1: the start screen is #$got, not the blue #5555ff"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
