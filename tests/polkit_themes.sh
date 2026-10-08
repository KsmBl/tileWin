#!/bin/sh
# The password prompt of tilewin-polkit in the look of every theme, through
# "tilewin-polkit --preview" (a sample request; polkit is not asked, so this
# works while the session has an agent already). For each theme:
#  - the dialog shows on the main display and the other screen is shaded
#  - typing shows in its field
#  - Escape answers No (exit 1) and Return answers Yes (exit 0)
# Screenshots are left in $POLKIT_SHOTS when that is set, one per theme.
#
# usage: polkit_themes.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
agent=$build/polkit/tilewin-polkit
keys=$build/tests/keyboard-tool
count=$build/tests/count-content

for needed in "$compositor" "$msg" "$agent" "$keys" "$count"; do
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
sock=$XDG_RUNTIME_DIR/tw-polkit-themes-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
preview_pid=
cleanup() {
	[ -n "$preview_pid" ] && pkill -P "$preview_pid" 2>/dev/null
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #808080
session_restore no
panel_command none
animations off
polkit_agent disable
main_output HEADLESS-2
output HEADLESS-1 mode --custom 1280x720 pos 0 0
output HEADLESS-2 mode --custom 1280x720 pos 1280 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman \
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
export SWAYSOCK=$sock TILEWINSOCK=$sock
sleep 1

mean() { # <png> <x> <y> <w> <h>: the mean of red, green and blue
	"$count" --mean "$@" | awk '{ printf "%d", ($1 + $2 + $3) / 3 }'
}
changed() { # <png> <png>: how many pixels differ
	python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
a, b = (GdkPixbuf.Pixbuf.new_from_file(f) for f in sys.argv[1:3])
da, db = a.get_pixels(), b.get_pixels()
print(sum(1 for i in range(0, len(da), a.get_n_channels()) if da[i:i + 3] != db[i:i + 3]))
' "$1" "$2"
}
preview() { # starts one; its exit status lands in $work/status
	rm -f "$work/status"
	( "$agent" --preview > "$work/agent.log" 2>&1; echo $? > "$work/status" ) &
	preview_pid=$!
}
answer() { # <key>: presses it and prints the preview's exit status
	"$keys" key none "$1" >/dev/null 2>&1
	attempt=0
	while [ ! -s "$work/status" ] && [ $attempt -lt 25 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	if [ -s "$work/status" ]; then
		cat "$work/status"
	else
		echo none
	fi
}

grim -o HEADLESS-1 "$work/plain.png"
plain=$(mean "$work/plain.png" 0 0 200 200)
themes=${POLKIT_THEMES:-$(ls "$source_dir/themes")}
for theme in $themes; do
	[ -f "$source_dir/themes/$theme/theme.conf" ] || continue
	"$msg" -s "$sock" theme "$theme" >/dev/null 2>&1
	sleep 0.5

	preview
	sleep 1.2
	grim -o HEADLESS-1 "$work/side.png"
	grim -o HEADLESS-2 "$work/asking.png"
	"$keys" text abc >/dev/null 2>&1
	sleep 0.4
	grim -o HEADLESS-2 "$work/typed.png"
	if [ -n "${POLKIT_SHOTS:-}" ]; then
		mkdir -p "$POLKIT_SHOTS"
		cp "$work/typed.png" "$POLKIT_SHOTS/$theme.png"
	fi
	side=$(mean "$work/side.png" 0 0 200 200)
	dialog=$("$count" "$work/asking.png" 340 120 600 480)
	typed=$(changed "$work/asking.png" "$work/typed.png")
	no=$(answer escape)
	preview_pid=

	preview
	sleep 1.2
	yes=$(answer return)
	preview_pid=
	echo "$theme: side screen $side (from $plain), dialog pixels $dialog, typing changed" \
		"$typed, Escape $no, Return $yes"
	[ "$side" -lt $((plain * 3 / 4)) ] || fail "$theme: the other screen is not shaded"
	[ "$dialog" -gt 20000 ] || fail "$theme: no dialog on the main display"
	[ "$typed" -gt 15 ] || fail "$theme: typing does not show"
	[ "$no" = 1 ] || fail "$theme: Escape did not answer No"
	[ "$yes" = 0 ] || fail "$theme: Return did not answer Yes"
done

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
