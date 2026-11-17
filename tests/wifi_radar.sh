#!/bin/bash
# The Wi-Fi radar widget (panel/widgets/wifiradar.c, panel/wifiradar.c) on the
# desktop: its sweep goes round, a click on a saved network connects to it
# at once, on an open one too, on a new one with a password opens the
# network flyout for it, and with Wi-Fi off a click on the radar turns it on.
# NetworkManager is a pretend one with networks made up for the test.
#
# usage: wifi_radar.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keyboard=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
cat >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf" <<EOT
desktop_widgets {
	wifiradar { style radar; size 4x4; column 3; row 0 }
}
EOT

export FAKE_NM=$work/nm
mkdir -p "$work/bin" "$FAKE_NM"
cat > "$work/bin/nmcli" <<'FAKE'
#!/bin/sh
# A pretend NetworkManager: fictional networks, one saved, connecting takes a moment.
st=$FAKE_NM
[ -f "$st/active" ] || echo "Home" > "$st/active"
active=$(cat "$st/active")
echo "$*" >> "$st/calls"
nets='Home:86:5180:WPA2
Studio Three:72:5500:WPA2 WPA3
Cafe Lumen:64:2437:
Office Guest:57:2412:WPA2
Library:49:5240:WPA2 802.1X
Garden:41:2462:WPA2
Attic:33:2437:WPA2
Printer Setup:27:2412:
Long Hall:18:5745:WPA3'
case "$*" in
"radio wifi") cat "$st/radio" 2>/dev/null || echo enabled ;;
"radio wifi on") echo enabled > "$st/radio" ;;
*"-f DEVICE,TYPE device"*) echo "wlan0:wifi"; echo "lo:loopback" ;;
*"-f DEVICE,TYPE,STATE,CONNECTION device"*) echo "wlan0:wifi:connected:$active" ;;
*"-f DEVICE,TYPE,STATE device"*) echo "wlan0:wifi:connected" ;;
*"-f IN-USE,SIGNAL,FREQ,SECURITY,SSID device wifi list"*)
	echo "$nets" | while IFS=: read -r n s f sec; do
		u=" "; [ "$n" = "$active" ] && u="*"
		printf '%s:%s:%s MHz:%s:%s\n' "$u" "$s" "$f" "$sec" "$n"; done ;;
*"-f IN-USE,SIGNAL,SECURITY,SSID,BSSID,FREQ,RATE,BANDWIDTH device wifi list"*)
	i=0; echo "$nets" | while IFS=: read -r n s f sec; do i=$((i+1))
		u=" "; [ "$n" = "$active" ] && u="*"
		printf '%s:%s:%s:%s:AA\\:BB\\:CC\\:00\\:00\\:%02d:%s MHz:270 Mbit/s:80 MHz\n' "$u" "$s" "$sec" "$n" "$i" "$f"; done ;;
*"-f UUID,TYPE connection show"*) echo "uuid-home:802-11-wireless"; echo "uuid-garden:802-11-wireless" ;;
*"-g UUID connection show"*) echo uuid-home; echo uuid-garden ;;
*"802-11-wireless.ssid connection show uuid uuid-home"*) echo Home ;;
*"802-11-wireless.ssid connection show uuid uuid-garden"*) echo Garden ;;
"connection up uuid uuid-garden") sleep 2; echo Garden > "$st/active"; echo "Connection successfully activated" ;;
"connection up uuid uuid-home") sleep 2; echo Home > "$st/active"; echo "Connection successfully activated" ;;
"device wifi connect Cafe Lumen") sleep 2; echo "Cafe Lumen" > "$st/active"; echo "Device 'wlan0' successfully activated" ;;
"device wifi connect Printer Setup") sleep 1; echo "Error: Connection activation failed: IP configuration could not be reserved."; exit 4 ;;
"device wifi connect"*) sleep 1; echo "Error: Connection activation failed: Secrets were required, but not provided."; exit 4 ;;
*) ;;
esac
exit 0
FAKE
chmod +x "$work/bin/nmcli"
export PATH=$work/bin:$PATH

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOT
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc mode window >/dev/null 2>&1
ipc theme win11 >/dev/null 2>&1

called() { grep -qxF -e "$1" "$FAKE_NM/calls" 2>/dev/null; }
radar() { grim -g "315,15 390x390" - 2>/dev/null | md5sum; }
moving() {
	local a b
	a=$(radar)
	sleep 0.4
	b=$(radar)
	[ "$a" != "$b" ]
}
wait_until 15 moving || fail "the sweep of the radar does not go round"
wait_until 10 called "-t -f IN-USE,SIGNAL,FREQ,SECURITY,SSID device wifi list --rescan auto" ||
	fail "the radar never asked for a scan"
sleep 1

# a saved network connects at once, by its connection
"$pointer" 1280 720 click 425 260 >/dev/null 2>&1
wait_until 6 called "connection up uuid uuid-garden" ||
	fail "a click on the saved network did not connect to it"
wait_until 6 grep -qx Garden "$FAKE_NM/active" || fail "Garden was not connected to"

# an open one too
sleep 2
"$pointer" 1280 720 click 469 265 >/dev/null 2>&1
wait_until 6 called "device wifi connect Cafe Lumen" ||
	fail "a click on the open network did not connect to it"

# a new one with a password: the network flyout, with the field for it, and no try without
sleep 3
flyout() { grim -g "720,150 400x480" - 2>/dev/null | md5sum; }
closed=$(flyout)
"$pointer" 1280 720 click 470 167 >/dev/null 2>&1
wait_until 6 sh -c "[ \"\$(grim -g '720,150 400x480' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a click on the network with a password opened no flyout"
grep -q "device wifi connect Studio Three" "$FAKE_NM/calls" &&
	fail "the network with a password was connected to without one"
ipc panel close >/dev/null 2>&1

# a right click on a network: its menu; what it offers reaches NetworkManager
menu() { grim -g "320,20 400x380" - 2>/dev/null | md5sum; }
closed=$(menu)
"$pointer" 1280 720 rightclick 425 260 >/dev/null 2>&1
wait_until 5 sh -c "[ \"\$(grim -g '320,20 400x380' - 2>/dev/null | md5sum)\" != '$closed' ]" ||
	fail "a right click on a network opened no menu"
"$keyboard" key none escape >/dev/null 2>&1
hex=$(printf %s Garden | od -An -tx1 | tr -d ' \n')
ipc panel radar autoconnect-off "$hex" >/dev/null 2>&1
wait_until 5 called "connection modify uuid uuid-garden connection.autoconnect no" ||
	fail "Connect automatically did not reach NetworkManager"
ipc panel radar forget "$hex" >/dev/null 2>&1
wait_until 5 called "connection delete uuid uuid-garden" ||
	fail "Forget this network did not reach NetworkManager"

# Wi-Fi off: a click on the radar turns it on
# (the first click sends a ping, so the radar looks again and finds it off)
echo disabled > "$FAKE_NM/radio"
for try in 1 2 3; do
	"$pointer" 1280 720 click 510 210 >/dev/null 2>&1
	wait_until 3 called "radio wifi on" && break
done
called "radio wifi on" || fail "a click on the radar with Wi-Fi off did not turn it on"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
