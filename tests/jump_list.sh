#!/bin/sh
# Jump lists in the right-click menu of a taskbar button, in a nested tileWin
# with the taskbar (Windows 10 theme): for a window whose app has a desktop
# entry with actions and files in recently-used.xbel
#  - the menu starts with the recent files, newest first, leaving out files
#    that are gone, and a click opens the file with that app;
#  - then the tasks (desktop actions), and a click runs the task.
#
# usage: jump_list.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$panel" "$pointer"; do
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
sock=$XDG_RUNTIME_DIR/tw-jump-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications" "$work/docs"

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

# the app: its entry opens files and runs its tasks with a script that notes them
printf '#!/bin/sh\necho "$@" >> %s/opened.log\n' "$work" > "$work/opener"
chmod +x "$work/opener"
cat > "$XDG_DATA_HOME/applications/xfce4-terminal.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=Terminal Test
Exec=$work/opener %f
Icon=utilities-terminal
Actions=preferences;newtab;

[Desktop Action preferences]
Name=Preferences
Exec=$work/opener --prefs

[Desktop Action newtab]
Name=New Tab
Exec=$work/opener --tab
EOD
echo notes > "$work/docs/notes.txt"
echo plan > "$work/docs/plan.md"
bookmark() {
	cat <<EOB
  <bookmark href="file://$1" added="$2" modified="$2" visited="$2">
    <info><metadata owner="http://freedesktop.org"><mime:mime-type type="text/plain"/>
      <bookmark:applications><bookmark:application name="Terminal Test" exec="&apos;opener %f&apos;" modified="$2" count="1"/></bookmark:applications>
    </metadata></info>
  </bookmark>
EOB
}
{
	echo '<?xml version="1.0" encoding="UTF-8"?>'
	echo '<xbel version="1.0" xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks" xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">'
	bookmark "$work/docs/notes.txt" 2026-09-30T10:00:00Z
	bookmark "$work/docs/gone.txt" 2026-09-30T12:00:00Z
	bookmark "$work/docs/plan.md" 2026-09-30T11:00:00Z
	echo '</xbel>'
} > "$XDG_DATA_HOME/recently-used.xbel"

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
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }

ipc theme win10 >/dev/null
sleep 2
ipc exec "xfce4-terminal --disable-server -T Terminal" >/dev/null
sleep 3
id=$(ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "Terminal" and c.get("pid"):
			print(c["id"])
		walk(c)
walk(json.load(sys.stdin))' | head -n 1)
[ -n "$id" ] || { echo "the terminal did not open"; exit 1; }

# the menu at 600,400: Recent, plan.md, notes.txt, a line, Tasks, Preferences,
# New Tab, ... (rows of 32 pixels from y 111)
pick() {
	ipc panel window_menu "$id" 600 400 >/dev/null
	sleep 1
	"$pointer" 1280 720 move 650 "$1" wait 300 click 650 "$1" >/dev/null 2>&1
	sleep 1
}
pick 159
grep -q "docs/plan.md" "$work/opened.log" 2>/dev/null ||
	fail "the first recent file is not the newest one, opened with the app"
pick 191
grep -q "docs/notes.txt" "$work/opened.log" 2>/dev/null ||
	fail "the second recent file does not open"
grep -q "gone.txt" "$work/opened.log" 2>/dev/null && fail "a file that is gone is listed"
pick 266
grep -q -- "--prefs" "$work/opened.log" 2>/dev/null || fail "the first task does not run"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	cat "$work/opened.log" 2>/dev/null
	exit 1
fi
echo "all checks passed"
