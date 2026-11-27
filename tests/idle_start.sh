#!/bin/bash
# "Start in the background when idle", in a nested tileWin with a /proc of the
# test's own (TILEWIN_IDLE_START_PROC) and short times:
#  - the window menu's item puts the app on the list and takes it off;
#  - while the processor is busy, nothing is started;
#  - once it is quiet, the app is started: its window opens minimized, and the
#    window that had the focus keeps it;
#  - it is written down for the session, and not started again.
#
# usage: idle_start.sh <build dir> <source dir>
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
for needed in xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-idle-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_IDLE_START_PROC=$work/proc TILEWIN_IDLE_START_TIMES="300 1 2"
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications" "$work/proc"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	[ -n "${ticker:-}" ] && kill "$ticker" 2>/dev/null
	ipc exit >/dev/null 2>&1
	sleep 0.5
	rm -f "$sock"
	[ -n "${IDLE_TEST_KEEP:-}" ] && echo "files in $work" || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# the app: a terminal of a title of its own, behind sh so that the terminal of
# the test (the same program) does not count as it running already
cat > "$XDG_DATA_HOME/applications/tw-idle-test.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=Idle test
Exec=sh -c "exec xfce4-terminal --disable-server --title=behind"
EOT
list=$XDG_CONFIG_HOME/tileWin/idle-start

# the /proc of the test: busy while $work/busy is there, quiet without it
touch "$work/busy"
(
	t=0
	while :; do
		t=$((t + 100))
		if [ -e "$work/busy" ]; then
			printf 'cpu %d 0 0 %d 0 0 0 0\n' $((t * 9 / 10)) $((t / 10)) > "$work/proc/stat.new"
		else
			printf 'cpu %d 0 0 %d 0 0 0 0\n' $((t / 50)) $((t - t / 50)) > "$work/proc/stat.new"
		fi
		mv "$work/proc/stat.new" "$work/proc/stat"
		printf '   8       0 sda 1 0 0 0 0 0 0 0 0 100 0\n' > "$work/proc/diskstats"
		sleep 0.1
	done
) &
ticker=$!

cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat > "$work/tilewin.conf" <<EOT
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOT

# the window menu's item, before the session: the list as it writes it
start_session() {
	rm -f "$work/display"
	env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
		WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
		"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
	if ! wait_for_file 15 "$work/display"; then
		echo "the nested compositor never came up"
		tail -n 3 "$work/log"
		exit 1
	fi
	export WAYLAND_DISPLAY=$(cat "$work/display")
}
start_session
ipc mode window >/dev/null 2>&1

wait_until 10 sh -c "'$msg' -s '$sock' panel idlestart on tw-idle-test.desktop >/dev/null 2>&1 && [ -f '$list' ]" ||
	fail "on: the app is not on the list"
grep -qx tw-idle-test.desktop "$list" || fail "the list does not name the app"
ipc panel idlestart toggle tw-idle-test.desktop >/dev/null 2>&1
sleep 0.5
! grep -qx tw-idle-test.desktop "$list" || fail "toggle: the app is still on the list"
ipc panel idlestart toggle tw-idle-test.desktop >/dev/null 2>&1
sleep 0.5
grep -qx tw-idle-test.desktop "$list" || fail "toggle again: the app is not on the list"

# a new session: the taskbar reads the list when it starts
ipc exit >/dev/null 2>&1
wait_until 10 sh -c "! '$msg' -s '$sock' -t get_version >/dev/null 2>&1"
start_session
ipc mode window >/dev/null 2>&1

# a field of the window whose title starts so: pid, focused, minimized
cat > "$work/window" <<EOT
#!/bin/sh
"$msg" -s "$sock" -t get_tree | python3 -c '
import json, sys
want, field = sys.argv[1], sys.argv[2]
def walk(n):
    if n.get("pid") and n.get("name", "").startswith(want):
        v = n.get(field)
        print(str(v).lower() if isinstance(v, bool) else v)
        sys.exit(0)
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(json.load(sys.stdin))
print("")' "\$1" "\$2"
EOT
chmod +x "$work/window"
window() { "$work/window" "$@"; }

ipc exec "xfce4-terminal --disable-server --title=front" >/dev/null
wait_until 15 sh -c "[ -n \"\$('$work/window' front pid)\" ]" ||
	fail "the test's own terminal did not open"
wait_until 10 sh -c "[ \"\$('$work/window' front focused)\" = true ]" ||
	fail "the test's own terminal has no focus"

# busy: nothing is started
sleep 5
[ -z "$(window behind pid)" ] || fail "started while the processor was busy"

# quiet: started in the background
rm "$work/busy"
wait_until 20 sh -c "[ -n \"\$('$work/window' behind pid)\" ]" ||
	fail "not started once it was quiet"
sleep 1
[ "$(window behind minimized)" = true ] || fail "its window did not open minimized"
[ "$(window front focused)" = true ] || fail "the window that had the focus lost it"
ls "$XDG_RUNTIME_DIR"/tilewin-idle-started-* >/dev/null 2>&1 &&
	grep -qx tw-idle-test.desktop $(grep -l tw-idle-test.desktop "$XDG_RUNTIME_DIR"/tilewin-idle-started-* 2>/dev/null | head -1) 2>/dev/null ||
	fail "not written down for the session"
done_file=$(grep -l tw-idle-test.desktop "$XDG_RUNTIME_DIR"/tilewin-idle-started-* 2>/dev/null | head -1)

# the taskbar restarts in the same session: not started a second time
ipc '[title="^behind"] kill' >/dev/null 2>&1
wait_until 10 sh -c "[ -z \"\$('$work/window' behind pid)\" ]"
pkill -f "^$panel" 2>/dev/null
wait_until 10 pgrep -f "^$panel" >/dev/null || fail "the taskbar did not come back"
sleep 5
[ -z "$(window behind pid)" ] || fail "started again in the same session"

[ -n "$done_file" ] && rm -f "$done_file"
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
