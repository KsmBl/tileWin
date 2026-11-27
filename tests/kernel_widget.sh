#!/bin/bash
# The kernel widget, on files of the automatic updates of its own
# (TILEWIN_KERNEL_AUTO_ROOT): on the taskbar how far a build is, drawn again
# as it gets further while a run holds its lock; the state when it is done; a
# new time of the last check is shown at once (Update now that finds nothing
# new takes a second); a click opens its flyout; on the desktop the Card,
# List, Ring and Bar styles draw something in every theme, and the panel
# survives all of it.
#
# usage: kernel_widget.sh <build dir> <source dir>
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
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_KERNEL_AUTO_ROOT=$work/ka
ka=$work/ka/var/lib/tilewin/kernel-auto
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/ka/etc/tilewin" "$ka/build" "$work/ka/run"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
lockpid=
cleanup() {
	[ -n "$lockpid" ] && kill "$lockpid" 2>/dev/null
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

printf 'enabled=yes\nprofile=running\nseries=end\n' > "$work/ka/etc/tilewin/kernel-auto.conf"
status() { # state next
	printf 'state=%s\nrunning=7.3.4-tilewin1\nrunning_auto=yes\nnext=%s\nfixes=7.3.5\nchecked=%s\n' \
		"$1" "$2" "$(date +%s)" > "$ka/status"
}
building() { # percent
	printf 'kind=kernel\nphase=compile\npercent=%s\nversion=7.3.6\nupdated=%s\n' "$1" "$(date +%s)" \
		> "$ka/build/kernel"
}
# a run that holds its lock, as tilewin-kernel-root auto-run does
python3 -c "
import fcntl, time
f = open('$work/ka/run/tilewin-kernel-auto.lock', 'w')
fcntl.flock(f, fcntl.LOCK_EX)
time.sleep(600)" &
lockpid=$!
status building ""
building 30

sed -e 's/^\tright tray keyboard volume/\tright kernel tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOF
theme_layout no
desktop_widgets {
	kernel { style card; column 0; row 0 }
	kernel:list { style list; column 3; row 0 }
	kernel:ring { style ring; column 6; row 0 }
	kernel:bar { style bar; column 0; row 2 }
}
EOF
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
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
ipc mode window >/dev/null 2>&1

bar() { grim -g "640,680 640x40" - 2>/dev/null | md5sum; }
# the cells of the desktop the four cards take; the empty desktop beside them
cards() { grim -g "0,0 820x320" - 2>/dev/null | md5sum; }
wait_still 15 bar
at30=$(bar)
building 70
wait_until 10 sh -c "[ \"\$(grim -g '640,680 640x40' - 2>/dev/null | md5sum)\" != '$at30' ]" ||
	fail "the taskbar did not show the build getting further"
cards_busy=$(cards)

kill "$lockpid" 2>/dev/null
lockpid=
status idle 7.3.6-tilewin1
building 100
# idle, the widget looks every fifteen seconds
wait_until 25 sh -c "[ \"\$(grim -g '0,0 820x320' - 2>/dev/null | md5sum)\" != '$cards_busy' ]" ||
	fail "the desktop cards did not show the kernel waiting for the restart"

# "Update now" that finds nothing new: only the time it looked changes, in a
# second, and the cards show it at once, not at the next quarter of a minute
sed -i "s/^checked=.*/checked=$(($(date +%s) - 10800))/" "$ka/status"
wait_until 10 sh -c "[ \"\$(grim -g '0,0 820x320' - 2>/dev/null | md5sum)\" != '$(cards)' ]"
wait_still 5 cards
three_hours=$(cards)
sed -i "s/^checked=.*/checked=$(date +%s)/" "$ka/status"
wait_until 5 sh -c "[ \"\$(grim -g '0,0 820x320' - 2>/dev/null | md5sum)\" != '$three_hours' ]" ||
	fail "a new time of the last check was not shown at once"

# every style draws something, in every theme
count_drawn() { # x y w h: the pixels that are not the wallpaper
	grim -g "$1,$2 $3x$4" "$work/card.png" 2>/dev/null &&
		python3 -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
print(sum(1 for p in im.getdata() if abs(p[0] - 0x20) + abs(p[1] - 0x40) + abs(p[2] - 0x70) > 40))" \
			"$work/card.png" 2>/dev/null || echo 0
}
if python3 -c 'import PIL' 2>/dev/null; then
	for theme in win95 winxp win7 win11; do
		ipc theme "$theme" >/dev/null 2>&1
		sleep 3
		for card in "card 14 14 290 190" "list 314 14 290 190" "ring 614 14 190 190" "bar 14 214 290 90"; do
			set -- $card
			drawn=$(count_drawn "$2" "$3" "$4" "$5")
			[ "${drawn:-0}" -gt 2000 ] || fail "$theme: the $1 card shows next to nothing ($drawn pixels)"
		done
	done
fi

# a click on the widget opens its flyout (Windows 10: the widget is first on the right)
ipc theme win10 >/dev/null 2>&1
sleep 3
above() { grim -g "600,300 680x370" - 2>/dev/null | md5sum; }
wait_still 5 above
closed=$(above)
"$pointer" 1280 720 click 965 700 >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(grim -g '600,300 680x370' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a click on the widget opened no flyout"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
