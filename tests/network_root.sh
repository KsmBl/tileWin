#!/bin/bash
# The helper behind the advanced settings of a network, with a tc, an nmcli
# and a /sys made up for the test: the settings are kept per network, the
# speed limits go onto the device the network is connected on and come off
# when it goes down, NetworkManager's dispatcher puts them back when it comes
# up, and the watch connects the network again once the amount went over it.
#
# usage: network_root.sh <build dir>
set -u

helper=$1/network/tilewin-network-root
run() { bash "$helper" "$@"; }
[ -f "$helper" ] || { echo "$helper was not built: skipping"; exit 77; }

work=$(mktemp -d)
trap 'pkill -f "$work" 2>/dev/null; rm -rf "$work"' EXIT

mkdir -p "$work/bin" "$work/sys/class/net/wlan0/statistics"
for tool in tc nmcli; do
	cat > "$work/bin/$tool" <<EOT
#!/bin/sh
echo "$tool \$*" >> "$work/log"
[ "$tool \$1 \$2" = "nmcli -g GENERAL.DEVICES" ] && echo wlan0
exit 0
EOT
	chmod +x "$work/bin/$tool"
done
for tool in bash awk cat dirname grep mkdir mv chmod readlink rm setsid sleep tr kill; do
	ln -s "$(command -v $tool)" "$work/bin/$tool" 2>/dev/null
done
counters() { echo "$1" > "$work/sys/class/net/wlan0/statistics/rx_bytes"
	echo "$2" > "$work/sys/class/net/wlan0/statistics/tx_bytes"; }
counters 5000000 1000000

export TILEWIN_NETWORK_ROOT=$work TILEWIN_NETWORK_PATH=$work/bin TILEWIN_NETWORK_INTERVAL=0.2
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
logged() { grep -qF -- "$1" "$work/log"; }

# connect again after 3 MB, downloads to 20 Mbit/s, uploads to 5 Mbit/s
: > "$work/log"
run set aaaa-1111 3 20000 5000 || fail "set failed"
grep -q '^aaaa-1111 3 20000 5000$' "$work/etc/tileWin/network.conf" ||
	fail "the settings of the network were not kept"
logged 'tc qdisc replace dev wlan0 root tbf rate 5000kbit' ||
	fail "the uploads of the connected device were not limited"
logged 'tc qdisc replace dev wlan0 handle ffff: ingress' &&
	logged 'police rate 20000kbit' || fail "the downloads of the connected device were not limited"
[ -f "$work/run/tilewin-network/wlan0.watch" ] || fail "nothing watches the amount"

# 2 MB later nothing happens, after 3 MB it connects again
counters 6000000 2000000
sleep 0.6
logged 'nmcli --wait 0 connection up' && fail "it connected again before the amount was reached"
counters 7000000 2500000
sleep 0.6
logged 'nmcli --wait 0 connection up uuid aaaa-1111' ||
	fail "it did not connect again once 3 MB went over the network"

# the network goes down: the limits come off, the watch ends
: > "$work/log"
run set aaaa-1111 3 20000 5000 >/dev/null
watcher=$(cat "$work/run/tilewin-network/wlan0.watch")
run dispatch wlan0 down
logged 'tc qdisc del dev wlan0 root' && logged 'tc qdisc del dev wlan0 ingress' ||
	fail "the limits stayed on the device after the network went down"
sleep 0.3
kill -0 "$watcher" 2>/dev/null && fail "the watch went on after the network went down"

# it comes up again: the dispatcher puts the limits back
: > "$work/log"
CONNECTION_UUID=aaaa-1111 run dispatch wlan0 up
logged 'tbf rate 5000kbit' || fail "the limits did not come back when the network came up"
# another network without settings is left alone
: > "$work/log"
run dispatch wlan0 down
: > "$work/log"
CONNECTION_UUID=bbbb-2222 run dispatch wlan0 up
[ -s "$work/log" ] && fail "a network without settings was touched: $(cat "$work/log")"

# a leading zero is no octal number; all off takes the network out
run set cccc-3333 010 0 0 >/dev/null
grep -q '^cccc-3333 10 0 0$' "$work/etc/tileWin/network.conf" || fail "010 was not kept as 10"
run set aaaa-1111 0 0 0 >/dev/null
grep -q '^aaaa-1111' "$work/etc/tileWin/network.conf" && fail "turning all off kept the network"
grep -q '^cccc-3333' "$work/etc/tileWin/network.conf" || fail "another network was lost"

# what is no network or number is refused
run set 'x;rm' 1 1 1 2>/dev/null && fail "a made-up network name was taken"
run set aaaa-1111 -1 0 0 2>/dev/null && fail "a negative amount was taken"

pkill -f "$work" 2>/dev/null
if [ "$failures" -gt 0 ]; then
	echo "--- what was run:"
	cat "$work/log"
	exit 1
fi
echo "the network helper did what the settings asked"
