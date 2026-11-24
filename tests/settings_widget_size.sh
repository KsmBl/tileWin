#!/bin/sh
# The size of a widget on the desktop, in its settings: a click on the little
# grid makes it that many cells (size 3x2 in its desktop_widgets entry), the
# fields change it on from there, and Default leaves it to the style again
# (no size written).
#
# usage: settings_widget_size.sh <build dir> <source dir>
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
export GIO_USE_VFS=local
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
cp "$source_dir/config/taskbar.conf" "$conf"
printf 'desktop_widgets {\n\tcpu {\n\t\tstyle chart\n\t}\n}\n' >> "$conf"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<CONF
wallpaper solid #204070
session_restore no
panel_command true
output * mode --custom 1280x1400
for_window [title="CPU usage settings"] fullscreen enable
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec "$settings" --widget cpu --desktop
CONF
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
wait_until 20 sh -c "'$msg' -s '$sock' -t get_tree | grep -q 'CPU usage settings'" ||
	fail "the dialog of the CPU widget did not open"
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
wait_still 15 screen
click() { "$pointer" 1280 1400 click "$1" "$2" >/dev/null 2>&1; }
size() { sed -n '/^desktop_widgets {/,/^}/p' "$conf" | sed -n 's/^[[:space:]]*size //p'; }
[ -z "$(size)" ] || fail "a widget without a size of its own has one: $(size)"
click 988 419 # the grid: the cell 5 columns across, 3 rows down
wait_until 5 sh -c "[ \"\$(sed -n '/^desktop_widgets {/,/^}/p' '$conf' | sed -n 's/^[[:space:]]*size //p')\" = 5x3 ]" ||
	fail "a click on the grid did not make it 5x3: '$(size)'"
wait_still 5 screen
click 1082 350 # the + of the columns
wait_until 5 sh -c "[ \"\$(sed -n '/^desktop_widgets {/,/^}/p' '$conf' | sed -n 's/^[[:space:]]*size //p')\" = 6x3 ]" ||
	fail "the + of the columns did not make it 6x3: '$(size)'"
wait_still 5 screen
click 918 350 # Default
wait_until 5 sh -c "[ -z \"\$(sed -n '/^desktop_widgets {/,/^}/p' '$conf' | sed -n 's/^[[:space:]]*size //p')\" ]" ||
	fail "Default did not leave the size to the style: '$(size)'"
"$msg" -s "$sock" -t get_tree | grep -q 'CPU usage settings' || fail "the dialog is gone after the changes"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
