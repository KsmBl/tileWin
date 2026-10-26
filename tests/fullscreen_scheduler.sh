#!/bin/sh
# The sched_ext scheduler for windows that fill a screen: with
# fullscreen_scheduler in taskbar.conf, the panel asks scx_loader for it when a
# window goes fullscreen and puts back what ran before once none is, or stops
# it when nothing ran. A scheduler that runs already is left alone. scx_loader
# is a stand-in on the session bus that writes down what it was asked.
#
# usage: fullscreen_scheduler.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel

for needed in "$compositor" "$msg" "$panel"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon python3; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0")' 2>/dev/null; then
	echo "GTK 4 for Python is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-scx-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
cp "$source_dir/config/taskbar.conf" "$conf"
printf '\nfullscreen_scheduler scx_lavd\n' >> "$conf"
log=$work/scx.log
: > "$log"

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

env -u WAYLAND_DISPLAY -u DISPLAY TILEWIN_SCX_BUS=session \
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
ipc mode window >/dev/null 2>&1
sleep 2

loader() { # what runs at first, its mode
	ipc exec "python3 $source_dir/tests/fake_scx_loader.py $log $1 $2" >/dev/null
	attempt=0
	while ! grep -qx ready "$log" && [ $attempt -lt 40 ]; do
		sleep 0.25
		attempt=$((attempt + 1))
	done
	: > "$log"
}
asked() { # what scx_loader should have been asked last
	attempt=0
	while [ "$(tail -n 1 "$log")" != "$1" ] && [ $attempt -lt 20 ]; do
		sleep 0.25
		attempt=$((attempt + 1))
	done
	got=$(tr '\n' '|' < "$log")
	if [ "$(tail -n 1 "$log")" = "$1" ]; then
		echo "ok   $2: $got"
	else
		fail "$2: scx_loader should have been asked \"$1\", was asked \"$got\""
	fi
	: > "$log"
}

loader unknown 0
ipc exec "python3 $source_dir/tests/color_window.py scx-game '#30a030'" >/dev/null
attempt=0
while ! ipc -t get_tree | grep -q '"name": "scx-game"' && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
sleep 1
ipc '[title=scx-game] fullscreen enable' >/dev/null
asked "Start scx_lavd 1" "a window fills the screen: scx_lavd in Gaming mode"
ipc '[title=scx-game] fullscreen disable' >/dev/null
asked "Stop" "it leaves the screen: back to the kernel's own"

pkill -f "fake_scx_loader.py $log" 2>/dev/null
sleep 0.5
: > "$log"
loader scx_bpfland 3
ipc '[title=scx-game] fullscreen enable' >/dev/null
asked "Switch scx_lavd 1" "with scx_bpfland running: switched to scx_lavd"
ipc '[title=scx-game] fullscreen disable' >/dev/null
asked "Switch scx_bpfland 3" "and back to scx_bpfland in its mode"

pkill -f "fake_scx_loader.py $log" 2>/dev/null
sleep 0.5
: > "$log"
loader scx_lavd 1
ipc '[title=scx-game] fullscreen enable' >/dev/null
sleep 2
ipc '[title=scx-game] fullscreen disable' >/dev/null
sleep 2
if [ -s "$log" ]; then
	fail "scx_lavd ran already, yet scx_loader was asked: $(tr '\n' '|' < "$log")"
else
	echo "ok   a scheduler that runs already is left alone"
fi
pkill -f "fake_scx_loader.py $log" 2>/dev/null

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
