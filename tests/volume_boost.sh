#!/bin/sh
# The volume goes on past 100 %, to 130 %, as PipeWire and PulseAudio amplify
# in software. In a nested tileWin with a pactl of its own (and no wpctl):
#  - "tilewin-media volume 120" sets 120 %, 150 sets 130 %, and the volume
#    keys go on past 100 % in their steps, stopping at 130 %;
#  - the volume slider of the quick settings goes past 100 % with the arrow keys
#    and draws differently past it;
#  - the end of the volume flyout's slider is 130 %;
#  - the volume popup shows 125 % differently from 100 %.
#
# usage: volume_boost.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
keys=$build/tests/keyboard-tool
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$keys" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-volume-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/bin"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
volume=$work/volume
echo 100 > "$volume"

# a sound system of the test's own: the volume in a file, any set is logged
cat > "$work/bin/pactl" <<EOS
v=\$(cat "$volume")
case "\$*" in
"get-sink-volume"*) echo "Volume: front-left: 65536 /  \$v% / 0,00 dB,   front-right: 65536 /  \$v% / 0,00 dB" ;;
"get-sink-mute"*) echo "Mute: no" ;;
"get-default-sink") echo test_sink ;;
"-f json list sinks") printf '[{"index":1,"name":"test_sink","description":"Test speakers","mute":false,"volume":{"front-left":{"value_percent":"%s%%"}}}]\n' "\$v" ;;
"-f json list sink-inputs") echo '[]' ;;
subscribe) while kill -0 \$PPID 2>/dev/null; do sleep 1; done ;;
"set-sink-volume "*)
	echo "\$*" >> "$work/pactl.log"
	for arg; do :; done
	case "\$arg" in
	+*) arg=\$((v + \${arg#+})) ;;
	-*) arg=\$((v - \${arg#-})) ;;
	esac
	echo "\${arg%\%}" | tr -d '%+' > "$volume" ;;
*) echo "\$*" >> "$work/pactl.log" ;;
esac
exit 0
EOS
# no PipeWire of the computer: tilewin-media takes pactl
printf '#!/bin/sh\nexit 1\n' > "$work/bin/wpctl"
cp "$source_dir/scripts/tilewin-media.in" "$work/bin/tilewin-media"
ln -s "$msg" "$work/bin/tilewinmsg"
chmod +x "$work/bin/pactl" "$work/bin/wpctl" "$work/bin/tilewin-media"
export PATH=$work/bin:$PATH

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# ---- tilewin-media
tilewin-media --no-popup volume 120 >/dev/null
[ "$(cat "$volume")" = 120 ] || fail "\"volume 120\" set $(cat "$volume")%"
tilewin-media --no-popup volume 150 >/dev/null
[ "$(cat "$volume")" = 130 ] || fail "\"volume 150\" set $(cat "$volume")%, not the 130% it ends at"
tilewin-media --no-popup volume 100 >/dev/null
tilewin-media --no-popup volume up >/dev/null
[ "$(cat "$volume")" = 105 ] || fail "the volume key from 100% set $(cat "$volume")%"
for _ in 1 2 3 4 5 6 7; do
	tilewin-media --no-popup volume up >/dev/null
done
[ "$(cat "$volume")" = 130 ] || fail "the volume key went on to $(cat "$volume")%"
tilewin-media --no-popup volume 100 >/dev/null

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
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
export SWAYSOCK=$sock TILEWINSOCK=$sock
ipc theme win11 >/dev/null 2>&1
screen() { grim -t ppm - 2>/dev/null | md5sum; }
wait_still 10 screen

# ---- the slider of the quick settings: Tab rings Wi-Fi; down and right: night
# light; down: the volume slider
ipc panel quicksettings >/dev/null
wait_still 5 screen
for key in tab down right down; do
	"$keys" key none $key
	sleep 0.2
done
wait_still 5 screen
grim -t ppm "$work/at100.ppm"
"$keys" key none right key none right
wait_until 5 grep -qx 110 "$volume" ||
	fail "two steps right from 100% on the quick settings set $(cat "$volume")%"
wait_still 5 screen
grim -t ppm "$work/at110.ppm"
cmp -s "$work/at100.ppm" "$work/at110.ppm" && fail "the slider looks the same at 110% as at 100%"
"$keys" key none escape
wait_still 5 screen

# ---- the volume flyout: its slider ends at 130 %
tilewin-media --no-popup volume 100 >/dev/null
ipc panel volume >/dev/null
wait_still 5 screen
[ -n "${VOLUME_SHOTS:-}" ] && grim "$VOLUME_SHOTS/flyout.png"
# the right end of its slider (the flyout over the speaker icon, 1280x720)
"$pointer" 1280 720 click "${VOLUME_END_X:-1204}" "${VOLUME_END_Y:-545}" >/dev/null 2>&1
wait_until 5 grep -qx 130 "$volume" ||
	fail "the end of the volume flyout's slider set $(cat "$volume")%"
"$keys" key none escape
wait_still 5 screen

# ---- the popup over 100 %
ipc panel osd volume 100 >/dev/null
wait_still 5 screen
grim -t ppm "$work/osd100.ppm"
ipc panel osd volume 125 >/dev/null
wait_still 5 screen
grim -t ppm "$work/osd125.ppm"
[ -n "${VOLUME_SHOTS:-}" ] && grim "$VOLUME_SHOTS/osd-125.png"
cmp -s "$work/osd100.ppm" "$work/osd125.ppm" && fail "the popup looks the same at 125% as at 100%"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
