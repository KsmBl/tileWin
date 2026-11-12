#!/bin/sh
# The password prompt of tilewin-polkit in the look of every theme, through
# "tilewin-polkit --preview" (a sample request; polkit is not asked, so this
# works while the session has an agent already). For each theme:
#  - the dialog shows on the main display and the other screen is shaded
#  - typing shows in its field
#  - Escape answers No (exit 1) and Return answers Yes (exit 0)
# Screenshots are left in $POLKIT_SHOTS when that is set, one per theme.
# With a part such as 1/2 it works only every second theme from the first,
# so that the parts can run side by side.
#
# usage: polkit_themes.sh <build dir> <source dir> [<part>/<parts>]
set -u
. "$(dirname "$0")/wait.sh"

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

if ! wait_for_file 12 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
export SWAYSOCK=$sock TILEWINSOCK=$sock

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
n = a.get_n_channels()
step, count = n * 1024, 0
for start in range(0, len(da), step): # stretches the same at once, the others pixel by pixel
	ra, rb = da[start:start + step], db[start:start + step]
	if ra != rb:
		count += sum(1 for i in range(0, len(ra), n) if ra[i:i + 3] != rb[i:i + 3])
print(count)
' "$1" "$2"
}
preview() { # starts one; its exit status lands in $work/status
	rm -f "$work/status"
	( "$agent" --preview > "$work/agent.log" 2>&1; echo $? > "$work/status" ) &
	preview_pid=$!
}
answer() { # <key>: presses it and prints the preview's exit status
	"$keys" key none "$1" >/dev/null 2>&1
	if wait_for_file 5 "$work/status"; then
		cat "$work/status"
	else
		echo none
	fi
}

shot() { # <output> <png>: takes it and prints its checksum
	grim -o "$1" "$2" && cksum < "$2"
}
dialog_up() { # the dialog on the main display, drawn to the end, in asking.png
	grim -o HEADLESS-2 "$work/asking.png" &&
		[ "$("$count" "$work/asking.png" 340 120 600 480)" -gt 20000 ] &&
		wait_still 5 shot HEADLESS-2 "$work/asking.png"
}
side_shaded() { # in side.png
	grim -o HEADLESS-1 "$work/side.png" &&
		[ "$(mean "$work/side.png" 0 0 200 200)" -lt $((plain * 3 / 4)) ]
}
typing_shown() { # the typing on the screen, drawn to the end, in typed.png
	grim -o HEADLESS-2 "$work/typed.png" && ! cmp -s "$work/asking.png" "$work/typed.png" &&
		wait_still 5 shot HEADLESS-2 "$work/typed.png"
}

wait_still 5 shot HEADLESS-1 "$work/plain.png"
plain=$(mean "$work/plain.png" 0 0 200 200)
themes=${POLKIT_THEMES:-$(ls "$source_dir/themes")}
part=${3:-1/1}
index=0
for theme in $themes; do
	index=$((index + 1))
	[ $(((index - 1) % ${part#*/})) -eq $((${part%/*} - 1)) ] || continue
	[ -f "$source_dir/themes/$theme/theme.conf" ] || continue
	"$msg" -s "$sock" theme "$theme" >/dev/null 2>&1
	# the preview takes the theme written down; the screens drawn again in it
	wait_until 5 grep -qx "$theme" "$XDG_CONFIG_HOME/tileWin/current-theme"
	wait_still 5 shot HEADLESS-1 "$work/side.png"

	preview
	# (a wait that runs out leaves its screenshot for the checks below to fail on)
	wait_until 10 dialog_up
	wait_until 3 side_shaded
	"$keys" text abc >/dev/null 2>&1
	wait_until 3 typing_shown
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
	wait_until 10 dialog_up
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
