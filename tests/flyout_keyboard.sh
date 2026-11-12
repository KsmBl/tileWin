#!/bin/sh
# The flyouts work without a mouse, in a nested tileWin with the taskbar, with
# stand-ins for nmcli and pactl that write down how they were called:
#  - quick settings: Tab puts a ring on the first button, Down and Right walk to
#    night light, Enter turns it on, Space off again;
#  - quick settings: Left and Right on the volume slider change the volume and
#    Enter does not jump it to where its middle is;
#  - network: the arrows reach a network, Enter opens it, Tab and Enter press
#    Connect, and a password is typed with spaces and arrows going to the field;
#  - network: a password too short for WPA is not tried; one the network turns
#    down opens the field again, the connection nmcli saved with it goes, and
#    the one typed then connects; a saved network whose password is wrong asks
#    for a new one and keeps it in the connection;
#  - network: the network connected to has Disconnect on its row, which
#    disconnects without the row being opened;
#  - power: Tab and Enter reach the Keep awake switch.
# Nothing shows until a key is used: no ring on a flyout just opened.
#
# usage: flyout_keyboard.sh <build dir> <source dir>
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
for needed in dbus-run-session grim; do
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
sock=$XDG_RUNTIME_DIR/tw-flykeys-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME/tileWin" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$work/bin"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

# Testnet takes the password "right pw2"; a wrong one is turned down the way
# NetworkManager does it, and the connection made for it is kept, as nmcli does.
# With $work/known there is a saved connection for it, whose password is wrong;
# with $work/active it is connected.
denied='Error: Connection activation failed: (7) Secrets were required, but not provided.'
cat > "$work/bin/nmcli" <<EOS
#!/bin/sh
echo "\$*" >> "$work/nmcli.calls"
case "\$*" in
"radio wifi") echo enabled ;;
"-t -f WIFI radio") echo enabled ;;
"-t -f DEVICE,TYPE,STATE,CONNECTION device")
	if [ -f "$work/active" ]; then echo 'wlan0:wifi:connected:Testnet'; else echo 'wlan0:wifi:disconnected:'; fi ;;
*"device wifi list"*) [ -f "$work/active" ] && printf '*' || printf ' '
	printf ':80:WPA2:Testnet:AA\\\\:BB\\\\:CC\\\\:DD\\\\:EE\\\\:FF:5180 MHz:540 Mbit/s:80 MHz\\n' ;;
"-t -f UUID,TYPE connection show") [ -f "$work/known" ] && echo 'cccc-3333:802-11-wireless' ;;
"-g UUID connection show") cat "$work/saved" 2>/dev/null; [ -f "$work/known" ] && echo cccc-3333 ;;
"-g 802-11-wireless.ssid connection show uuid "*) echo Testnet ;;
"device wifi connect Testnet password right pw2") ;;
"device wifi connect Testnet"*) echo bbbb-2222 > "$work/saved"; echo "$denied"; exit 4 ;;
"connection delete uuid bbbb-2222") rm -f "$work/saved" ;;
"connection up uuid cccc-3333") echo "$denied"; exit 4 ;;
esac
exit 0
EOS
cat > "$work/bin/pactl" <<EOS
#!/bin/sh
echo "\$*" >> "$work/pactl.calls"
case "\$*" in
"get-sink-volume @DEFAULT_SINK@") echo 'Volume: front-left: 26214 /  40% / -23.88 dB' ;;
"get-sink-mute @DEFAULT_SINK@") echo 'Mute: no' ;;
esac
exit 0
EOS
cat > "$work/bin/rfkill" <<EOS
#!/bin/sh
echo "\$*" >> "$work/rfkill.calls"
EOS
chmod +x "$work/bin/nmcli" "$work/bin/pactl" "$work/bin/rfkill"
export PATH=$work/bin:$PATH

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

# ---- quick settings: first button, down, right: night light
ipc panel quicksettings >/dev/null
sleep 0.8
grim -t ppm "$work/plain.ppm"
"$keys" key none tab
sleep 0.4
grim -t ppm "$work/ring.ppm"
cmp -s "$work/plain.ppm" "$work/ring.ppm" && fail "no ring shows after Tab"
"$keys" key none down key none right key none return
sleep 0.5
night=$(cat "$XDG_STATE_HOME/tileWin/nightlight" 2>/dev/null)
[ "$night" = on ] || fail "Enter on the night light button left it '$night'"
"$keys" key none space
sleep 0.5
night=$(cat "$XDG_STATE_HOME/tileWin/nightlight" 2>/dev/null)
[ "$night" = off ] || fail "Space on the night light button left it '$night'"

# ---- the volume slider: down from the row of buttons
: > "$work/pactl.calls"
"$keys" key none down
sleep 0.2
"$keys" key none right key none right
sleep 0.5
grep -q 'set-sink-volume @DEFAULT_SINK@ 45%' "$work/pactl.calls" ||
	fail "Right on the volume slider did not go from 40% to 45%: $(cat "$work/pactl.calls")"
: > "$work/pactl.calls"
"$keys" key none return
sleep 0.4
if grep -q 'set-sink-volume' "$work/pactl.calls"; then
	fail "Enter on the slider set the volume: $(cat "$work/pactl.calls")"
fi
"$keys" key none escape
sleep 0.4

# ---- network: the row, then Connect, then the password
ipc panel network >/dev/null 2>&1 || true
sleep 1.2
: > "$work/nmcli.calls"
# from the end, since the addresses of this computer may come first: the last
# stop is Refresh, the one above it the network
"$keys" key none up key none up key none return
sleep 0.5
"$keys" key none down key none return
sleep 0.5
# too short for WPA: not tried, and the field keeps it
"$keys" text ab key none space text cd key none return
sleep 0.8
grep -q 'connect Testnet' "$work/nmcli.calls" && fail "a password of 5 characters was tried"
"$keys" key none backspace key none backspace key none backspace key none backspace key none backspace
# a wrong one: turned down, and the field is there to type the next one
"$keys" text wrong key none space text pw1 key none return
sleep 1.2
grep -q 'connect Testnet password wrong pw1' "$work/nmcli.calls" ||
	fail "the keyboard did not connect: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
grep -q 'connection delete uuid bbbb-2222' "$work/nmcli.calls" ||
	fail "the connection saved with the wrong password stayed: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
grep -q 'connection delete uuid cccc-3333' "$work/nmcli.calls" && fail "a connection that was there before went"
"$keys" text right key none space text pw2 key none return
sleep 1.2
grep -q 'connect Testnet password right pw2' "$work/nmcli.calls" ||
	fail "no new password could be typed after the wrong one: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
"$keys" key none escape key none escape
sleep 0.4

# ---- network: a saved network whose password is no longer right
ipc panel close >/dev/null 2>&1
touch "$work/known"
ipc panel network >/dev/null 2>&1 || true
sleep 1.2
: > "$work/nmcli.calls"
"$keys" key none up key none up key none return
sleep 0.5
# Show password comes first in a saved network, Connect to the right of it
"$keys" key none down key none right key none return
sleep 1.2
grep -q 'connection up uuid cccc-3333' "$work/nmcli.calls" ||
	fail "the saved network was not connected: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
"$keys" text fresh key none space text pw3 key none return
sleep 1.2
grep -q 'connection modify uuid cccc-3333 802-11-wireless-security.psk fresh pw3' "$work/nmcli.calls" ||
	fail "the new password of a saved network was not kept: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
[ "$(grep -c 'connection up uuid cccc-3333' "$work/nmcli.calls")" = 2 ] ||
	fail "it did not connect again with the new password"
"$keys" key none escape key none escape
sleep 0.4

# ---- network: Disconnect on the row of the network connected to
ipc panel close >/dev/null 2>&1
touch "$work/active"
ipc panel network >/dev/null 2>&1 || true
sleep 1.2
: > "$work/nmcli.calls"
# from the end: Refresh, then the Disconnect of the row
"$keys" key none up key none up key none return
sleep 1.0
grep -q 'device disconnect wlan0' "$work/nmcli.calls" ||
	fail "Disconnect on the row did not disconnect: $(tr '\n' '|' < "$work/nmcli.calls" | tail -c 500)"
"$keys" key none escape key none escape
sleep 0.4

# ---- power: walk until the keep awake switch answers
ipc panel close >/dev/null 2>&1
ipc panel power >/dev/null 2>&1
sleep 0.8
awake() { ipc -t get_tilewin | grep -c '"keep_awake": *true'; }
for step in 1 2 3 4 5; do
	"$keys" key none tab key none return
	sleep 0.4
	[ "$(awake)" = 1 ] && break
done
[ "$(awake)" = 1 ] || fail "Tab and Enter did not reach the keep awake switch"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
