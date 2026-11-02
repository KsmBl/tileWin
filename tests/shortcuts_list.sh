#!/bin/sh
# The list of keyboard shortcuts and the word after a switch of the mode, in a
# nested tileWin with the taskbar:
#  - get_bindings tells the keys of the current mode, with one added in the
#    config, and they change with the mode;
#  - Super+/ opens the list on the screen, Esc closes it, Super+F1 opens it too;
#  - switching the mode tells how to get back, with the keys the config has for
#    it, three times and no more.
#
# usage: shortcuts_list.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$keys"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-shortcuts-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

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
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
bindsym Mod4+Shift+w wm_mode toggle
bindsym Mod4+k wm_mode toggle
bindsym Mod4+Shift+x opacity plus 0.1
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
sleep 1.5

# a binding with these keys and this command
has_binding() {
	ipc -t get_bindings | python3 -c '
import json, sys
want = (sys.argv[1], sys.argv[2])
sys.exit(0 if any((b["keys"], b["command"]) == want for b in json.load(sys.stdin)) else 1)' "$1" "$2"
}
has_binding Shift+Mod4+x "opacity plus 0.1" || fail "get_bindings has not the binding of the config"
has_binding Mod4+slash "panel shortcuts" || fail "Super+/ is not bound to the list"
has_binding Mod4+F1 "panel shortcuts" || fail "Super+F1 is not bound to the list"

# how much of the screen differs from a picture of it without the list
changed() {
	grim -t ppm "$work/now.ppm" && python3 -c '
import sys
def read(path):
	data = open(path, "rb").read()
	fields, pos = [], 0
	while len(fields) < 4:
		while data[pos:pos + 1].isspace():
			pos += 1
		start = pos
		while not data[pos:pos + 1].isspace():
			pos += 1
		fields.append(data[start:pos])
	return data[pos + 1:]
a, b = read(sys.argv[1]), read(sys.argv[2])
same = sum(a[i:i + 3] == b[i:i + 3] for i in range(0, min(len(a), len(b)) - 2, 3 * 11))
print(100 - 100 * same * 33 // max(1, len(a)))' "$work/without.ppm" "$work/now.ppm"
}
grim -t ppm "$work/without.ppm"
"$keys" key super slash
sleep 0.8
big=$(changed)
[ "$big" -ge 15 ] || fail "the list covers only $big% of the screen"
"$keys" key none escape
sleep 0.6
left=$(changed)
[ "$left" -le 2 ] || fail "$left% of the screen is still different after Esc"

"$keys" key super f1
sleep 0.8
[ "$(changed)" -ge 15 ] || fail "Super+F1 does not open the list"
ipc panel shortcuts >/dev/null
sleep 0.5
[ "$(changed)" -le 2 ] || fail "the same command again does not close the list"

# the hint: three times with the keys of the config, then no more
notes=$XDG_STATE_HOME/tileWin/notifications.json
count_hints() {
	python3 -c '
import re, sys
try:
	text = open(sys.argv[1]).read()
except OSError:
	text = ""
print(len(re.findall(r"goes back to", text)))' "$notes"
}
for round in 1 2 3 4 5 6; do
	ipc wm_mode toggle >/dev/null 2>&1
	sleep 1
done
sleep 0.5
got=$(count_hints)
[ "$got" = 3 ] || fail "the mode told how to get back $got times, not 3"
grep -q "Super+Shift+W goes back" "$notes" 2>/dev/null ||
	fail "the hint does not name the keys of the config: $(cat "$notes" 2>/dev/null | head -c 300)"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
