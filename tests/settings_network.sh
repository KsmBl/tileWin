#!/bin/sh
# Works the Network and DNS pages of the settings app against a NetworkManager
# and a DNS helper made up for the test, and looks at what they were told:
# showing the Wi-Fi password asks NetworkManager for the secret, a fixed
# address is saved with its subnet and gateway, and choosing how the servers
# for all networks are asked writes the config of the DNS service.
#
# usage: settings_network.sh <build dir>
set -u

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
tool=$build/tests/pointer-tool

for needed in "$compositor" "$settings" "$tool"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
cleanup() {
	pkill -f "^[^ ]*/tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
export TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$work/run" "$work/nmconf"

# NetworkManager: a connected Wi-Fi network with a lease
cat > "$work/nmcli" <<'EON'
#!/bin/sh
echo "$*" >> "$FAKE_NM_LOG"
case "$*" in
*"NAME,UUID,TYPE,DEVICE,ACTIVE connection show"*)
	echo 'Home\: upstairs:aaaa-1111:802-11-wireless:wlan0:yes'
	echo 'lo:cccc:loopback:lo:yes' ;;
*"-s -g 802-11-wireless-security.psk"*)
	echo 'correct horse battery' ;;
*"connection show uuid aaaa-1111"*)
	printf '%s\n' connection.type:802-11-wireless GENERAL.DEVICES:wlan0 ipv4.method:auto \
		ipv4.addresses: ipv4.gateway: ipv4.dns: ipv4.ignore-auto-dns:no ipv6.dns: \
		802-11-wireless-security.key-mgmt:wpa-psk 'IP4.ADDRESS[1]:192.168.1.129/24' \
		IP4.GATEWAY:192.168.1.1 'IP4.DNS[1]:192.168.1.1' ;;
esac
exit 0
EON
# the helper that would run as root: it only keeps the config
cat > "$work/apply" <<'EOA'
#!/bin/sh
echo "$*" >> "$FAKE_DNS_LOG"
[ "$1" = config ] && cat > "$TILEWIN_DNS_CONF"
exit 0
EOA
chmod +x "$work/nmcli" "$work/apply"
printf 'servers 1.1.1.1 9.9.9.9 8.8.8.8\nfastest 2\nprefetch_skip ads.test\n' > "$work/dns.conf"
export TILEWIN_NMCLI=$work/nmcli TILEWIN_DNS_APPLY=$work/apply TILEWIN_DNS_CONF=$work/dns.conf \
	TILEWIN_DNS_RUN=$work/run TILEWIN_NM_CONF_DIR=$work/nmconf \
	FAKE_NM_LOG=$work/nm.log FAKE_DNS_LOG=$work/dns.log
: > "$work/nm.log"
: > "$work/dns.log"

# Opens a page in a nested tileWin of the given height; sets $display.
start() {
	page=$1
	height=$2
	cat > "$work/tw-$page.conf" <<EOC
wallpaper solid #008080
session_restore no
panel_command true
animations off
output * mode --custom 1280x$height
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec "$settings" --page=$page
EOC
	before=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
	env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
		WLR_RENDERER=pixman SWAYSOCK="$work/$page.sock" TILEWINSOCK="$work/$page.sock" \
		"$(dirname "$0")/session.sh" "$compositor" -c "$work/tw-$page.conf" > "$work/tw-$page.log" 2>&1 &
	display=
	attempt=0
	while [ -z "$display" ] && [ $attempt -lt 40 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
		after=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
		display=$(echo "$after" | grep -vxF "$before" | head -n 1)
	done
	if [ -z "$display" ]; then
		echo "the nested compositor never came up"
		exit 1
	fi
	sleep 6
	[ -n "${SHOT_DIR:-}" ] && WAYLAND_DISPLAY=$display grim "$SHOT_DIR/$page.png"
}

click() {
	WAYLAND_DISPLAY=$display "$tool" 1280 "$height" click "$1" "$2" >/dev/null 2>&1
	sleep 1
}

failed=0
fail() {
	echo "FAIL: $*"
	failed=1
}

start network 1500
click 1116 463 # Show, beside the password
grep -q -- '-s -g 802-11-wireless-security.psk connection show uuid aaaa-1111' "$work/nm.log" ||
	fail "showing the password did not ask NetworkManager for it"
click 1135 255 # Get the address: ...
click 1105 338 # ... Fixed address, filled in from the lease
click 1194 480 # Apply
sleep 1
grep -q 'connection modify uuid aaaa-1111 ipv4.method manual ipv4.addresses 192.168.1.129/24 ipv4.gateway 192.168.1.1' \
	"$work/nm.log" || fail "the fixed address was not saved with its subnet and gateway"
grep -q 'connection up uuid aaaa-1111' "$work/nm.log" ||
	fail "the connected network did not connect again with the new address"
pkill -f "^[^ ]*/tilewin -c $work/tw-network.conf" 2>/dev/null
sleep 2 # its socket name is free again before the next one starts

start dns 2600
click 1110 326 # Ask: ...
click 1100 409 # ... These servers, in this order
sleep 1
grep -q '^config' "$work/dns.log" || fail "the DNS page never handed its config to the helper"
grep -q '^fastest 0$' "$work/dns.conf" || fail "\"these servers, in order\" did not write fastest 0"
grep -q '^servers 1.1.1.1 9.9.9.9 8.8.8.8$' "$work/dns.conf" || fail "the servers were lost"
grep -q '^prefetch_skip ads.test$' "$work/dns.conf" || fail "a name never to prefetch was lost"

if [ $failed -ne 0 ]; then
	echo "--- nmcli was asked:"
	cat "$work/nm.log"
	echo "--- the DNS config:"
	cat "$work/dns.conf"
	echo "If a page was rearranged, the clicks above may miss their controls."
	exit 1
fi
echo "the Network and DNS pages said the right things"
