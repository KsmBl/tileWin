#!/bin/sh
# What the taskbar costs while nobody does anything, in a nested tileWin with
# the default taskbar and a fake pactl:
#  - one "pactl subscribe" for everything that listens to the sound, not one
#    for each;
#  - after "restart panel" no helper of the old taskbar is left running;
#  - the idle taskbar wakes up only a few times a second (it used to read the
#    window tree every second for a widget of the other mode's taskbar, and
#    every background reader woke it on its own).
#
# usage: panel_idle.sh <build dir> <source dir>
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
if ! command -v dbus-daemon >/dev/null 2>&1 || [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no dbus-daemon or no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-idle-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/bin"
# a sound server that is there and says nothing. It does not end by itself
# when its taskbar does, so the restart below shows whether the taskbar ends
# its helpers (the cleanup ends what is left)
cat > "$work/bin/pactl" <<EOP
#!/bin/sh
case "\$*" in
subscribe) while :; do sleep 0.5; done ;;
*) ;;
esac
EOP
chmod +x "$work/bin/pactl"
export PATH=$work/bin:$PATH

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	pkill -f "$work/bin/pactl" 2>/dev/null
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
ipc() { "$msg" -s "$sock" "$@"; }
sleep 4

listening() { pgrep -f "$work/bin/pactl subscribe" | wc -l; }
[ "$(listening)" -eq 1 ] ||
	fail "$(listening) pactl subscribe run; the volume and the speakers share one"

taskbar=$(pgrep -f "^$panel" | head -1)
if [ -z "$taskbar" ]; then
	fail "the taskbar is not running"
else
	before=$(awk '/^voluntary_ctxt_switches/{print $2}' /proc/"$taskbar"/status)
	sleep 20
	after=$(awk '/^voluntary_ctxt_switches/{print $2}' /proc/"$taskbar"/status)
	woke=$((after - before))
	echo "the idle taskbar woke $woke times in 20 s"
	# a clock, one reader a second and the sound: about two a second; it was
	# twelve with the git widget and a tile mode taskbar of the workspaces
	[ "$woke" -le 100 ] || fail "the idle taskbar woke $woke times in 20 s (at most 100)"
fi

old=$(pgrep -f "$work/bin/pactl subscribe" | head -1)
ipc restart panel >/dev/null
sleep 3
if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
	fail "the pactl subscribe of the old taskbar still runs after the restart"
fi
[ "$(listening)" -eq 1 ] || fail "after the restart $(listening) pactl subscribe run, not one"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
