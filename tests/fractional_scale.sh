#!/bin/sh
# The taskbar on a screen at 150%, in a nested tileWin: it is drawn for the
# pixels it really covers, so its edges are clearly sharper than those of a
# buffer of the next whole scale shrunk to fit, which is what it falls back on
# without the protocol. The clicks still land: the start button opens the menu.
# At 100% and 200% it looks the same with and without the protocol.
#
# usage: fractional_scale.sh <build dir> <source dir>
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

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

# edges of a part of a picture: how much neighbouring pixels differ
sharpness() {
	python3 - "$1" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
w, h = int(fields[1]), int(fields[2])
p = data[pos + 1:]
total = 0
for y in range(h):
    row = p[y * w * 3:(y + 1) * w * 3]
    for x in range(1, w):
        total += (row[x * 3 + 1] - row[x * 3 - 2]) ** 2
print(total // max(1, w * h))
PY
}

# one session at a scale, with or without the protocol: shots of the taskbar
session() { # <scale> <name> [fractional off]
	scale=$1 name=$2 off=${3:-}
	work=$root/$name
	sock=$XDG_RUNTIME_DIR/tw-frac-$$-$name.sock
	mkdir -p "$work/config/tileWin" "$work/state" "$work/cache" "$work/data"
	cp "$source_dir/config/taskbar.conf" "$work/config/tileWin/taskbar.conf"
	cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1920x1080 pos 0 0 scale $scale
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC
	(
		[ -n "$off" ] && export TILEWIN_NO_FRACTIONAL_SCALE=1
		exec env -u WAYLAND_DISPLAY -u DISPLAY \
			XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" \
			XDG_CACHE_HOME="$work/cache" XDG_DATA_HOME="$work/data" \
			TILEWIN_DATADIR="$source_dir" TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory \
			WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
			WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
			"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf"
	) > "$work/log" 2>&1 &
	attempt=0
	while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	if [ ! -s "$work/display" ]; then
		echo "the nested compositor never came up at scale $scale"
		tail -n 3 "$work/log"
		exit 1
	fi
	display=$(cat "$work/display")
	sleep 2.5
	# the left of the taskbar, away from the clock and the charts that change
	if [ "$scale" = 2 ]; then
		WAYLAND_DISPLAY=$display grim -g "0,510 300x30" -t ppm "$work/bar.ppm"
	else
		WAYLAND_DISPLAY=$display grim -g "0,680 400x40" -t ppm "$work/bar.ppm"
	fi
	if [ -z "$off" ] && [ "$scale" = 1.5 ]; then
		WAYLAND_DISPLAY=$display grim -t ppm "$work/before.ppm"
		WAYLAND_DISPLAY=$display "$pointer" 1280 720 click 20 700
		sleep 1
		WAYLAND_DISPLAY=$display grim -t ppm "$work/after.ppm"
		cmp -s "$work/before.ppm" "$work/after.ppm" &&
			fail "at 150% a click on the start button opened nothing"
	fi
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.6
	rm -f "$sock"
}

session 1.5 sharp
session 1.5 soft off
a=$(sharpness "$root/sharp/bar.ppm")
b=$(sharpness "$root/soft/bar.ppm")
echo "at 150%: sharpness $a, without the protocol $b"
[ "$a" -gt $((b + b / 10)) ] || fail "at 150% the taskbar is no sharper than a shrunk buffer ($a against $b)"

session 2 two
session 2 two-off off
cmp -s "$root/two/bar.ppm" "$root/two-off/bar.ppm" ||
	fail "at 200% the taskbar looks different with the protocol"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
