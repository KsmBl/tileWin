#!/bin/sh
# The speaker on the taskbar button of a window playing sound, in a nested
# tileWin with the taskbar and a fake pactl (sink inputs from a file, commands
# logged; the real one would change the sound of this computer):
#  - a sink input of the window's process puts a speaker on its button;
#  - a click on the speaker mutes it, a second one unmutes it;
#  - the wheel on the speaker changes that app's volume;
#  - paused (corked) or gone, the speaker goes away.
#
# usage: taskbar_sound.sh <build dir> <source dir>
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
for needed in dbus-run-session grim xfce4-terminal python3; do
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
sock=$XDG_RUNTIME_DIR/tw-sound-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/bin"
echo '[]' > "$work/inputs.json"
# the fake pactl: "subscribe" reports a change whenever the number in
# $work/trigger goes up (every subscriber sees it: the volume widgets listen too)
echo 0 > "$work/trigger"
cat > "$work/bin/pactl" <<EOP
#!/bin/sh
case "\$*" in
subscribe)
	seen=\$(cat "$work/trigger")
	# ends with the taskbar that started it, like the real one
	while kill -0 \$PPID 2>/dev/null; do
		now=\$(cat "$work/trigger")
		if [ "\$now" != "\$seen" ]; then
			seen=\$now
			echo "Event 'change' on sink-input #7"
		fi
		sleep 0.2
	done ;;
"-f json list sink-inputs") cat "$work/inputs.json" ;;
*) echo "\$*" >> "$work/pactl.log" ;;
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
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }

# sink inputs: index 7 of the given process, corked or not, muted or not
inputs() {
	printf '[{"index":7,"mute":%s,"corked":%s,"volume":{"front-left":{"value_percent":"60%%"}},"properties":{"application.process.id":"%s","application.name":"Terminal"}}]\n' \
		"$3" "$2" "$1" > "$work/inputs.json"
	echo $(($(cat "$work/trigger") + 1)) > "$work/trigger"
	sleep 1
}
shot() {
	grim -g "0,680 1280x40" "$1" 2>/dev/null
}
# the box of the pixels that differ between two shots: "x y" of its middle
changed_middle() {
	python3 - "$@" <<'EOP'
import struct, sys, zlib
def load(path):
	data = open(path, "rb").read()
	pos, chunks = 8, {}
	idat = b""
	while pos < len(data):
		length, kind = struct.unpack(">I4s", data[pos:pos + 8])
		body = data[pos + 8:pos + 8 + length]
		if kind == b"IHDR":
			w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
		elif kind == b"IDAT":
			idat += body
		pos += 12 + length
	raw = zlib.decompress(idat)
	bpp = 4 if ctype == 6 else 3
	rows, prev, i = [], bytearray(w * bpp), 0
	for _ in range(h):
		f = raw[i]; i += 1
		line = bytearray(raw[i:i + w * bpp]); i += w * bpp
		for x in range(len(line)):
			a = line[x - bpp] if x >= bpp else 0
			b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
			if f == 1: line[x] = (line[x] + a) & 255
			elif f == 2: line[x] = (line[x] + b) & 255
			elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
			elif f == 4:
				p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
				line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
		rows.append(line); prev = line
	return w, h, bpp, rows
w, h, bpp, a = load(sys.argv[1])
_, _, _, b = load(sys.argv[2])
# optionally only within x0 y0 x1 y1 (screen coordinates)
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (0, 680, w, 680 + h)
xs, ys = [], []
for y in range(max(0, y0 - 680), min(h, y1 - 680)):
	for x in range(max(0, x0), min(w, x1)):
		if a[y][x * bpp:x * bpp + 3] != b[y][x * bpp:x * bpp + 3]:
			xs.append(x); ys.append(y)
if xs:
	print((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2 + 680)
EOP
}

ipc theme win10 >/dev/null
sleep 2
ipc exec "xfce4-terminal --disable-server -T Terminal" >/dev/null
sleep 3
pid=$(ipc -t get_tree | python3 -c '
import json, sys
def walk(n):
	for c in n.get("nodes", []) + n.get("floating_nodes", []):
		if c.get("name") == "Terminal" and c.get("pid"):
			print(c["pid"])
		walk(c)
walk(json.load(sys.stdin))' | head -n 1)
[ -n "$pid" ] || { echo "the terminal did not open"; exit 1; }
ipc '[title=Terminal] minimize enable' >/dev/null
sleep 0.5
shot "$work/before.png"
inputs "$pid" false false
shot "$work/playing.png"
middle=$(changed_middle "$work/before.png" "$work/playing.png")
[ -n "$middle" ] || { echo "FAIL: no speaker on the button of the window playing sound"; exit 1; }
set -- $middle
sx=$1 sy=$2

"$pointer" 1280 720 move "$sx" "$sy" wait 300 click "$sx" "$sy" >/dev/null 2>&1
sleep 0.5
grep -q "set-sink-input-mute 7 1" "$work/pactl.log" 2>/dev/null ||
	fail "a click on the speaker does not mute the app"
inputs "$pid" false true
shot "$work/muted.png"
[ -n "$(changed_middle "$work/playing.png" "$work/muted.png")" ] ||
	fail "the speaker does not show it is muted"
"$pointer" 1280 720 move "$sx" "$sy" wait 300 click "$sx" "$sy" >/dev/null 2>&1
sleep 0.5
grep -q "set-sink-input-mute 7 0" "$work/pactl.log" 2>/dev/null ||
	fail "a second click does not unmute it"
"$pointer" 1280 720 move "$sx" "$sy" wait 300 scroll "$sx" "$sy" -1 >/dev/null 2>&1
sleep 0.5
grep -q "set-sink-input-volume 7 [+-]5%" "$work/pactl.log" 2>/dev/null ||
	fail "the wheel on the speaker does not change the volume"

inputs "$pid" true false
shot "$work/paused.png"
[ -z "$(changed_middle "$work/before.png" "$work/paused.png" $((sx - 10)) $((sy - 10)) \
	$((sx + 10)) $((sy + 10)))" ] ||
	fail "the speaker stays while the sound is paused"

# The sound server goes away and comes back (restarted, or swapped for
# another): every "pactl subscribe" ends, and each one that listened has to
# listen again, or its icon stays as it was until the taskbar restarts.
listening() { pgrep -f "$work/bin/pactl subscribe" | wc -l; }
before=$(listening)
pkill -f "$work/bin/pactl subscribe"
sleep 0.5
[ "$(listening)" -lt "$before" ] || fail "the subscriptions could not be ended for the test"
attempt=0
while [ "$(listening)" -lt "$before" ] && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
[ "$(listening)" -ge "$before" ] ||
	fail "after the sound server went away only $(listening) of $before listen again"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
