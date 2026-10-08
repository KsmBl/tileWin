#!/bin/sh
# "tilewinmsg restart" starts the helpers of tileWin that run for the whole
# session again when an update replaced their file, so their fixes come in
# with the windows open, and leaves the others running:
#  - restarted with nothing replaced, tilewin-nightlight is the same process
#  - its file replaced (a new file in its place, as an install does) and
#    restarted, the old process is gone and a new one runs the new file
#
# usage: restart_helpers.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
nightlight=$build/nightlight/tilewin-nightlight

for needed in "$compositor" "$msg" "$nightlight"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-daemon >/dev/null 2>&1; then
	echo "dbus-daemon is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-helpers-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 TILEWIN_NO_NOTIFY=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
mkdir -p "$work/bin"
cp "$nightlight" "$work/bin/tilewin-nightlight"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	for pid in $(helper_pids); do
		kill "$pid" 2>/dev/null
	done
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# the nightlight helpers started from the copy in this test's folder
helper_pids() {
	for exe in /proc/[0-9]*/exe; do
		target=$(readlink "$exe" 2>/dev/null) || continue
		case "$target" in
		"$work/bin/tilewin-nightlight"*)
			pid=${exe#/proc/}
			echo "${pid%/exe}"
			;;
		esac
	done
}

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
panel_command none
polkit_agent disable
animations off
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY PATH="$work/bin:$PATH" \
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
sleep 1.5

first=$(helper_pids)
echo "nightlight at the start: ${first:-none}"
if [ -z "$first" ]; then
	echo "tilewin-nightlight did not stay up here: skipping"
	exit 77
fi

"$msg" -s "$sock" restart >/dev/null
sleep 2
same=$(helper_pids)
echo "after a restart with nothing replaced: $same"
[ "$same" = "$first" ] || fail "a helper nobody replaced was started again"

# an update: a new file in the old one's place
cp "$nightlight" "$work/bin/tilewin-nightlight.new"
mv "$work/bin/tilewin-nightlight.new" "$work/bin/tilewin-nightlight"
"$msg" -s "$sock" restart >/dev/null
sleep 2.5
after=$(helper_pids)
echo "after a restart with its file replaced: ${after:-none}"
[ -n "$after" ] || fail "the replaced helper was not started again"
for pid in $after; do
	[ "$pid" != "$first" ] || fail "the replaced helper still runs the old file"
	case "$(readlink "/proc/$pid/exe")" in
	*"(deleted)") fail "the helper runs a file that is gone" ;;
	esac
done
[ "$(echo "$after" | wc -w)" -le 1 ] || fail "more than one nightlight helper runs"
"$msg" -s "$sock" -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
