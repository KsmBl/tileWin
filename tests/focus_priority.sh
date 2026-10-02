#!/bin/sh
# focus_priority in a nested tileWin, with two terminals of their own processes:
#  - the command takes off, raised, high, highest and nice values from -20 to 0,
#    and refuses others;
#  - where tileWin may lower nice values (CAP_SYS_NICE), the focused terminal's
#    process runs at -10 and the other at its own value, and the other way round
#    when the focus moves; off gives it back;
#  - where it may not (as when run from the build directory), nothing changes,
#    it says so once in its log, and it goes on working.
#
# usage: focus_priority.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
# TILEWIN_COMPOSITOR=$(which tilewin) tries the installed one, which has CAP_SYS_NICE
compositor=${TILEWIN_COMPOSITOR:-$build/sway/tilewin}
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$msg"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-prio-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

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
trap cleanup EXIT INT TERM

cat > "$work/tilewin.conf" <<EOF
session_restore no
panel_command true
animations off
focus_priority high
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

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
ipc() { "$msg" -s "$sock" "$@"; }

# the values it takes, and those it does not
for good in off raised high highest -20 0 -3; do
	ipc "focus_priority $good" >/dev/null 2>&1 || fail "focus_priority $good is refused"
done
for bad in 5 -21 fast; do
	ipc "focus_priority $bad" >/dev/null 2>&1 && fail "focus_priority $bad is taken"
done
ipc focus_priority high >/dev/null

# two terminals, each a process of its own, with titles to tell them apart
ipc exec "xfce4-terminal --disable-server --title=one" >/dev/null
ipc exec "xfce4-terminal --disable-server --title=two" >/dev/null
pids() {
	ipc -t get_tree | python3 -c '
import json, sys
found = {}
def walk(n):
    if n.get("pid") and n.get("name") in ("one", "two"):
        found[n["name"]] = n["pid"]
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(json.load(sys.stdin))
print(found.get("one", 0), found.get("two", 0))'
}
attempt=0
while [ $attempt -lt 40 ]; do
	set -- $(pids)
	[ "$1" != 0 ] && [ "$2" != 0 ] && break
	sleep 0.5
	attempt=$((attempt + 1))
done
one=$1 two=$2
if [ "$one" = 0 ] || [ "$two" = 0 ]; then
	echo "the terminals did not come up"
	exit 1
fi
nice_of() { ps -o ni= -p "$1" | tr -d ' '; }
before_one=$(nice_of "$one")
before_two=$(nice_of "$two")

# can this compositor lower nice values at all?
tw_pid=$(pgrep -f "^$compositor -c $work/tilewin.conf" | head -n 1)
caps=$(awk '/^CapEff:/ { print $2 }' "/proc/$tw_pid/status")
can=$(( (0x$caps >> 23) & 1 ))

ipc '[title="one"] focus' >/dev/null
sleep 0.3
n1=$(nice_of "$one") n2=$(nice_of "$two")
echo "focused one: nice $n1 and $n2 (before $before_one and $before_two), CAP_SYS_NICE $can"
if [ "$can" = 1 ]; then
	[ "$n1" = -10 ] || fail "the focused terminal does not run at -10"
	[ "$n2" = "$before_two" ] || fail "the other terminal does not keep its own value"
	ipc '[title="two"] focus' >/dev/null
	sleep 0.3
	n1=$(nice_of "$one") n2=$(nice_of "$two")
	echo "focused two: nice $n1 and $n2"
	[ "$n2" = -10 ] || fail "the newly focused terminal does not run at -10"
	[ "$n1" = "$before_one" ] || fail "the terminal focused before does not get its value back"
	ipc focus_priority off >/dev/null
	sleep 0.3
	[ "$(nice_of "$two")" = "$before_two" ] || fail "off does not give the value back"
else
	[ "$n1" = "$before_one" ] && [ "$n2" = "$before_two" ] ||
		fail "without CAP_SYS_NICE a nice value changed"
	ipc '[title="two"] focus' >/dev/null
	ipc '[title="one"] focus' >/dev/null
	sleep 0.3
	warnings=$(grep -c "focus_priority needs CAP_SYS_NICE" "$work/log")
	echo "without CAP_SYS_NICE: $warnings warning(s) in the log"
	[ "$warnings" = 1 ] || fail "the missing capability is not said once in the log"
fi
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
