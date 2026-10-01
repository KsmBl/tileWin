#!/bin/sh
# The Snap layouts of the Window behavior page: adding one, removing one and
# going back to the defaults each write taskbar.conf, which the taskbar reads
# its layouts from.
#
# usage: settings_snap_layouts.sh <build dir>
set -u

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
tool=$build/tests/pointer-tool

for needed in "$compositor" "$settings" "$tool"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
cleanup() {
	pkill -f "tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# a session of its own, so the test neither reads nor writes the real config
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
export TILEWIN_NO_APP_TWEAKS=1 # leaves the real desktop and home alone
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
output * mode --custom 1280x1400
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec "$settings" --page=windows
EOF

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

before=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	SWAYSOCK="$work/tilewin.sock" TILEWINSOCK="$work/tilewin.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
starter=$!

display=
attempt=0
while [ -z "$display" ] && [ $attempt -lt 40 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
	after=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
	display=$(echo "$after" | grep -vxF "$before" | head -n 1)
done
if [ -z "$display" ]; then
	echo "the nested compositor never came up"
	tail -n 2 "$work/log"
	exit 1
fi
sleep 6 # the settings app has to be up and laid out before it is clicked

written=$XDG_CONFIG_HOME/tileWin/taskbar.conf
layouts() { sed -n '/^snap_layouts {/,/^}/p' "$written" 2>/dev/null | grep -c '^\s*layout '; }
first() { sed -n '/^snap_layouts {/,/^}/p' "$written" 2>/dev/null | grep -m1 '^\s*layout ' | xargs; }

# the page is ready once its blue "Add layout" button is drawn
attempt=0
while [ $attempt -lt 60 ]; do
	rgb=$(WAYLAND_DISPLAY=$display grim -g "1178,1200 1x1" -t ppm - 2>/dev/null | tail -c 3 |
		od -An -tu1 | tr -s ' ' | sed 's/^ //')
	set -- $rgb
	[ "${3:-0}" -gt 150 ] && [ "${1:-255}" -lt 120 ] && break
	sleep 0.5
	attempt=$((attempt + 1))
done
command -v grim >/dev/null 2>&1 || { echo "grim is not installed: skipping"; exit 77; }

# "Add layout" with what the dropdowns start at: two side by side, two thirds
WAYLAND_DISPLAY=$display "$tool" 1280 1400 click 1178 1200 >/dev/null 2>&1
sleep 1.5
[ "$(layouts)" = 6 ] || fail "adding a layout did not write six layouts ($(layouts))"
sed -n '/^snap_layouts {/,/^}/p' "$written" | grep -q 'layout columns 0.66$' ||
	fail "the new layout is not two side by side at two thirds"

# the remove button of the first one, the halves
WAYLAND_DISPLAY=$display "$tool" 1280 1400 click 1215 708 >/dev/null 2>&1
sleep 1.5
[ "$(layouts)" = 5 ] || fail "removing a layout did not leave five ($(layouts))"
[ "$(first)" = "layout columns 0.66" ] || fail "the halves are still first: $(first)"

# back to the defaults: no list of its own
WAYLAND_DISPLAY=$display "$tool" 1280 1400 click 1005 1200 >/dev/null 2>&1
sleep 1.5
grep -q '^snap_layouts' "$written" && fail "the default layouts still left a list in taskbar.conf"

kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
