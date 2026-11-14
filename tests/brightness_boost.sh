#!/bin/sh
# Brightness over 100 %, in a nested tileWin with the night light running, a
# backlight of its own ($TILEWIN_SYS_BACKLIGHT) and a stand-in for brightnessctl:
#  - quick settings: the arrows on the brightness slider go past 100 % to 120 %,
#    which writes the boost into ~/.local/state/tileWin/brightness-boost and
#    leaves the backlight at full; the part over 100 % looks different;
#  - back below 100 % the boost goes and the backlight follows;
#  - tilewin-media (the brightness keys): up stops at 100 %, "brightness 125"
#    goes over it, and down from there stays in the boost;
#  - without the night light the slider ends at 100 % again.
# Screenshots of the quick settings at 120 % are left in $BOOST_SHOTS when
# that is set, one per theme of $BOOST_THEMES (default: win11 and win95).
#
# usage: brightness_boost.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
nightlight=$build/nightlight/tilewin-nightlight
keys=$build/tests/keyboard-tool
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$nightlight" "$keys" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim flock; do
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
sock=$XDG_RUNTIME_DIR/tw-boost-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_SYS_BACKLIGHT=$work/backlight
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME/tileWin" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME" "$work/bin" "$work/backlight/test_bl"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
echo 100 > "$work/backlight/test_bl/max_brightness"
echo 100 > "$work/backlight/test_bl/brightness"
boost_file=$XDG_STATE_HOME/tileWin/brightness-boost

# brightnessctl that sets the backlight of $work/backlight and writes down its calls
cat > "$work/bin/brightnessctl" <<EOS
#!/bin/sh
echo "\$*" >> "$work/brightnessctl.calls"
file=$work/backlight/test_bl/brightness
case "\$*" in
"-m") echo "test_bl,backlight,\$(cat \$file),\$(cat \$file)%,100" ;;
*set*) for arg; do :; done; value=\${arg%\\%}; echo "\$value" > "\$file" ;;
esac
exit 0
EOS
cp "$source_dir/scripts/tilewin-media.in" "$work/bin/tilewin-media"
ln -s "$nightlight" "$work/bin/tilewin-nightlight"
ln -s "$msg" "$work/bin/tilewinmsg"
chmod +x "$work/bin/brightnessctl" "$work/bin/tilewin-media"
export PATH=$work/bin:$PATH

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
export SWAYSOCK=$sock TILEWINSOCK=$sock
lock=$XDG_RUNTIME_DIR/tilewin-nightlight-$WAYLAND_DISPLAY.lock

# tileWin starts the night light (the one of this build, first in PATH)
night_runs() { [ -f "$lock" ] && ! flock -n "$lock" true; }
attempt=0
while ! night_runs && [ $attempt -lt 30 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
night_runs || fail "the night light does not run"
sleep 1

# Tab rings Wi-Fi; down and right: night light; down: the volume slider, then the
# brightness slider below it
open_slider() {
	ipc panel quicksettings >/dev/null
	sleep 0.8
	for key in tab down right down down; do
		"$keys" key none $key
		sleep 0.25
	done
}

# ---- the slider over 100 %
: > "$work/brightnessctl.calls"
open_slider
grim -t ppm "$work/at100.ppm"
"$keys" key none right key none right key none right key none right
sleep 0.6
boost=$(cat "$boost_file" 2>/dev/null)
[ "$boost" = 120 ] || fail "four steps right from 100% left the boost at '$boost'"
[ "$(cat "$work/backlight/test_bl/brightness")" = 100 ] ||
	fail "the backlight is not at full over 100%"
grep -q 'set 1[1-3][0-9]%' "$work/brightnessctl.calls" &&
	fail "brightnessctl was asked for more than 100%: $(cat "$work/brightnessctl.calls")"
grim -t ppm "$work/at120.ppm"
cmp -s "$work/at100.ppm" "$work/at120.ppm" && fail "the slider looks the same at 120% as at 100%"
"$nightlight" boost | grep -qx 120 || fail "tilewin-nightlight boost does not say 120"

shoot() { # <theme>
	[ -n "${BOOST_SHOTS:-}" ] || return 0
	mkdir -p "$BOOST_SHOTS"
	grim "$BOOST_SHOTS/quicksettings-$1.png"
}
themes=${BOOST_THEMES:-win11 win95}
first=1
for theme in $themes; do
	if [ $first = 0 ]; then
		"$keys" key none escape
		ipc theme "$theme" >/dev/null 2>&1
		sleep 1.2
		open_slider
	fi
	first=0
	shoot "$theme"
done

# ---- back below 100 %: the boost goes, the backlight follows
for step in 1 2 3 4 5; do
	"$keys" key none left
done
sleep 0.6
boost=$(cat "$boost_file" 2>/dev/null)
[ "$boost" = 100 ] || fail "five steps left from 120% left the boost at '$boost'"
[ "$(cat "$work/backlight/test_bl/brightness")" = 95 ] ||
	fail "the backlight is $(cat "$work/backlight/test_bl/brightness"), not 95"
"$keys" key none escape
sleep 0.4

# ---- the brightness keys
tilewin-media --no-popup brightness up >/dev/null
[ "$(cat "$work/backlight/test_bl/brightness")" = 100 ] || fail "up from 95% is not 100%"
[ "$(cat "$boost_file")" = 100 ] || fail "up from 95% went over 100%"
# and on past 100 %, as the sliders
tilewin-media --no-popup brightness up >/dev/null
[ "$(cat "$boost_file")" = 105 ] || fail "up from 100% left the boost at $(cat "$boost_file")"
[ "$(cat "$work/backlight/test_bl/brightness")" = 100 ] || fail "the backlight left full over 100%"
tilewin-media brightness 125 >/dev/null
sleep 0.3
[ "$(cat "$boost_file")" = 125 ] || fail "\"brightness 125\" left the boost at $(cat "$boost_file")"
[ -n "${BOOST_SHOTS:-}" ] && grim "$BOOST_SHOTS/osd-125.png"
tilewin-media --no-popup brightness down >/dev/null
[ "$(cat "$boost_file")" = 120 ] || fail "down from 125% left the boost at $(cat "$boost_file")"
tilewin-media --no-popup brightness | grep -q 'Brightness: 120%' ||
	fail "tilewin-media does not show 120%: $(tilewin-media --no-popup brightness)"

# ---- the bottom: 0% shown, the backlight on at 1%, and up again to 5, not 6
tilewin-media --no-popup brightness 10 >/dev/null
tilewin-media --no-popup brightness down >/dev/null
tilewin-media --no-popup brightness down >/dev/null
[ "$(cat "$work/backlight/test_bl/brightness")" = 1 ] ||
	fail "the lowest brightness put the backlight at $(cat "$work/backlight/test_bl/brightness"), not 1"
tilewin-media --no-popup brightness | grep -q 'Brightness: 0%' ||
	fail "the lowest brightness is not shown as 0%: $(tilewin-media --no-popup brightness)"
tilewin-media --no-popup brightness down >/dev/null
[ "$(cat "$work/backlight/test_bl/brightness")" = 1 ] || fail "down from 0% turned the backlight off"
tilewin-media --no-popup brightness up >/dev/null
[ "$(cat "$work/backlight/test_bl/brightness")" = 5 ] ||
	fail "up from 0% gave $(cat "$work/backlight/test_bl/brightness")%, not 5%"

# ---- the slider of the battery flyout goes on past 100 % too
tilewin-media --no-popup brightness 100 >/dev/null
ipc theme win11 >/dev/null 2>&1 # where the flyout and its slider are
sleep 1.2
ipc panel power >/dev/null
sleep 1.5
# the right end of its slider (the flyout over the battery icon, 1280x720)
"$pointer" 1280 720 click 1200 499 >/dev/null 2>&1
sleep 0.6
[ "$(cat "$boost_file")" = 130 ] ||
	fail "the end of the battery flyout's slider left the boost at $(cat "$boost_file")"
[ "$(cat "$work/backlight/test_bl/brightness")" = 100 ] ||
	fail "the battery flyout put the backlight at $(cat "$work/backlight/test_bl/brightness")"
"$keys" key none escape
sleep 0.4

# ---- without the night light: up to 100 % only
# only the night light of this nested tileWin: the one that holds its lock
for proc in /proc/[0-9]*; do
	if ls -l "$proc/fd" 2>/dev/null | grep -qF "$lock"; then
		kill "${proc#/proc/}" 2>/dev/null
	fi
done
attempt=0
while night_runs && [ $attempt -lt 20 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
night_runs && fail "the night light did not end"
tilewin-media --no-popup brightness 125 >/dev/null 2>&1
[ "$(cat "$boost_file")" = 100 ] || fail "over 100% without the night light: $(cat "$boost_file")"
open_slider
"$keys" key none right key none right
sleep 0.6
[ "$(cat "$boost_file")" = 100 ] || fail "the slider went over 100% without the night light"
"$keys" key none escape

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
