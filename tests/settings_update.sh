#!/bin/sh
# The Updates group of the About page, with a stand-in for tilewin-update:
#  - it says what the newest release is when the page is shown;
#  - Build runs "source", Update runs "release";
#  - a failure shows in the banner, and Try again runs the same way once more;
#  - with a package manager owning the files neither button is there.
#
# usage: settings_update.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-update-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_UPDATE_COMMAND=$work/update
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"
echo win95 > "$XDG_CONFIG_HOME/tileWin/current-theme"

cat > "$work/update" <<EOS
#!/bin/sh
echo "\$1" >> "$work/calls"
case \$1 in
check)
	echo "installed 0.0"
	[ ! -e "$work/managed" ] || echo "managed yes"
	echo "release 99.0"
	echo "archive yes"
	;;
*)
	echo "==> working"
	if [ -e "$work/refuse" ]; then
		rm -f "$work/refuse"
		echo "boom" >&2
		exit 1
	fi
	echo "==> done"
	;;
esac
EOS
chmod +x "$work/update"

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
panel_command none
animations off
output HEADLESS-1 mode --custom 1280x800 pos 0 0
exec $settings --page=theme
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
attempt=0
while ! ipc -t get_tree | grep -q '"name": "tileWin Settings"' && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
sleep 2.5
[ ! -e "$work/calls" ] || fail "GitHub was asked before the page was shown: $(cat "$work/calls")"
ipc '[title="tileWin Settings"] kill' >/dev/null
sleep 0.5
"$settings" --page=about >/dev/null 2>&1 &
sleep 3
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 2

grep -q '^check$' "$work/calls" 2>/dev/null || fail "the page did not ask for the newest release"

banner_up() {
	grim -g "600,20 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | python3 -c '
import sys
r, g, b = (int(x) for x in sys.stdin.read().split())
sys.exit(0 if r > 170 and g < 80 and b < 60 else 1)'
}

# the buttons of a window that fills the screen
release_x=1200 release_y=340
build_x=1207 build_y=395

: > "$work/calls"
touch "$work/refuse"
"$pointer" 1280 800 click "$build_x" "$build_y"
sleep 1.5
grep -q '^source$' "$work/calls" || fail "Build did not run the source way: $(cat "$work/calls")"
banner_up || fail "a failed update shows no banner"

"$pointer" 1280 800 click 1190 24
sleep 1.5
[ "$(grep -c '^source$' "$work/calls")" = 2 ] || fail "Try again did not run it again: $(cat "$work/calls")"
banner_up && fail "the banner stayed after it worked"

: > "$work/calls"
"$pointer" 1280 800 click "$release_x" "$release_y"
sleep 1.5
grep -q '^release$' "$work/calls" || fail "Update did not run the release way: $(cat "$work/calls")"
banner_up && fail "a good update shows a banner"

# owned by a package manager: nothing to press
touch "$work/managed"
ipc '[title="tileWin Settings"] kill' >/dev/null
sleep 0.5
"$settings" --page=about >/dev/null 2>&1 &
sleep 3
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 2
: > "$work/calls"
"$pointer" 1280 800 click "$release_x" "$release_y"
sleep 0.5
"$pointer" 1280 800 click "$build_x" "$build_y"
sleep 1
if grep -qE '^(source|release)$' "$work/calls"; then
	fail "with a package manager a button still ran: $(cat "$work/calls")"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
