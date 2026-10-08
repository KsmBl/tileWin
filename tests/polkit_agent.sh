#!/bin/sh
# tilewin-polkit, the password prompt of polkit, in a nested tileWin with two
# screens, the right one the main display:
#  - it registers as the agent of the session
#  - asked for an administrator (pkcheck of org.freedesktop.policykit.exec),
#    it darkens both screens and shows its dialog in the middle of the main one
#  - typing goes into its field; Escape refuses, and the asking program hears
#    that the request was dismissed
#  - then the screens are as before
#  - when the program asking goes away meanwhile, the dialog goes as well
# No password is ever given: a wrong one would count against the account.
# Screenshots are left in $POLKIT_SHOTS when that is set; $POLKIT_THEME picks
# the theme to look at.
#
# usage: polkit_agent.sh <build dir> <source dir>
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
for needed in dbus-daemon grim pkcheck; do
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
sock=$XDG_RUNTIME_DIR/tw-polkit-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
agent_pid=
check_pid=
cleanup() {
	[ -n "$check_pid" ] && kill "$check_pid" 2>/dev/null
	[ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# the agent is started by the test, not by tileWin
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
if [ -n "${POLKIT_THEME:-}" ]; then
	"$msg" -s "$sock" theme "$POLKIT_THEME" >/dev/null 2>&1
	sleep 1
fi

"$agent" > "$work/agent.log" 2>&1 &
agent_pid=$!
sleep 1.5
if ! kill -0 "$agent_pid" 2>/dev/null; then
	cat "$work/agent.log"
	if grep -q "cannot register\|not in a login session" "$work/agent.log"; then
		echo "no login session to be the agent of, or it has an agent: skipping"
		agent_pid=
		exit 77
	fi
	agent_pid=
	fail "the agent did not stay up"
	exit 1
fi

shot() { # <name>: both screens
	grim -o HEADLESS-1 "$work/$1-1.png"
	grim -o HEADLESS-2 "$work/$1-2.png"
	if [ -n "${POLKIT_SHOTS:-}" ]; then
		mkdir -p "$POLKIT_SHOTS"
		cp "$work/$1-1.png" "$work/$1-2.png" "$POLKIT_SHOTS/"
	fi
}
mean() { # <png> <x> <y> <w> <h>: the mean of red, green and blue
	"$count" --mean "$@" | awk '{ printf "%d", ($1 + $2 + $3) / 3 }'
}

shot before
plain=$(mean "$work/before-1.png" 0 0 200 200)

pkcheck --action-id org.freedesktop.policykit.exec --process $$ --allow-user-interaction \
	> "$work/pkcheck.log" 2>&1 &
check_pid=$!
sleep 2.5
shot asking
dimmed_side=$(mean "$work/asking-1.png" 0 0 200 200)
dimmed_corner=$(mean "$work/asking-2.png" 0 0 120 120)
dialog=$("$count" "$work/asking-2.png" 410 160 460 400)
echo "the side screen: $plain before, $dimmed_side while asking; the main one's corner:" \
	"$dimmed_corner; dialog pixels in its middle: $dialog"
[ "$dimmed_side" -lt $((plain * 3 / 4)) ] || fail "the other screen does not darken"
[ "$dimmed_corner" -lt $((plain * 3 / 4)) ] || fail "the main screen does not darken"
[ "$dialog" -gt 20000 ] || fail "no dialog in the middle of the main display"
kill -0 "$check_pid" 2>/dev/null || fail "the request did not wait for an answer"

# typing goes into the field: dots appear in it
"$keys" text abcdef >/dev/null 2>&1
sleep 0.5
shot typed
typed=$(python3 -I -c '
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
a, b = (GdkPixbuf.Pixbuf.new_from_file(f) for f in sys.argv[1:3])
da, db = a.get_pixels(), b.get_pixels()
print(sum(1 for i in range(0, len(da), a.get_n_channels()) if da[i:i + 3] != db[i:i + 3]))
' "$work/asking-2.png" "$work/typed-2.png")
echo "pixels changed by typing: $typed"
[ "$typed" -gt 30 ] || fail "typing does not show in the field"

"$keys" key none escape >/dev/null 2>&1
attempt=0
while kill -0 "$check_pid" 2>/dev/null && [ $attempt -lt 30 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
if kill -0 "$check_pid" 2>/dev/null; then
	fail "Escape did not answer the request"
	status=-1
else
	wait "$check_pid"
	status=$?
fi
check_pid=
echo "pkcheck after Escape: exit $status, $(tr '\n' ' ' < "$work/pkcheck.log")"
# pkcheck: 0 authorized, 1 not, 2 a challenge, 3 dismissed
[ "$status" = 3 ] && grep -q "dismissed" "$work/pkcheck.log" ||
	fail "the request was not dismissed"

sleep 0.5
shot after
after=$(mean "$work/after-1.png" 0 0 200 200)
echo "the side screen afterwards: $after"
[ "$after" -ge $((plain - 4)) ] || fail "the screens stay dark after the dialog went"
kill -0 "$agent_pid" 2>/dev/null || fail "the agent did not stay up for the next request"

# the program asking goes away while it is asked: the dialog goes too
pkcheck --action-id org.freedesktop.policykit.exec --process $$ --allow-user-interaction \
	> "$work/pkcheck2.log" 2>&1 &
check_pid=$!
sleep 2
shot asked-again
again=$(mean "$work/asked-again-1.png" 0 0 200 200)
kill "$check_pid" 2>/dev/null
wait "$check_pid" 2>/dev/null
check_pid=
sleep 1.5
shot gone
gone=$(mean "$work/gone-1.png" 0 0 200 200)
echo "asked again: $again; after the asking program went: $gone"
[ "$again" -lt $((plain * 3 / 4)) ] || fail "the second request shows no dialog"
[ "$gone" -ge $((plain - 4)) ] || fail "the dialog stays after the asking program went"
kill -0 "$agent_pid" 2>/dev/null || fail "the agent did not survive a request polkit gave up on"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
