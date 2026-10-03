#!/bin/sh
# tilewin-dns-apply on computers made up for the test: every file below a
# directory of its own (TILEWIN_DNS_ROOT), and systemctl, nmcli, networkctl
# and ss replaced by scripts that say which services run and log what they
# are told. On each kind of computer, turning the DNS service on has to send
# its DNS there, and turning it off has to leave it as it was:
#  - systemd-resolved with NetworkManager (which drops a loopback server from
#    its DNS for all networks, so resolved itself gets the service);
#  - systemd-resolved with systemd-networkd;
#  - a plain /etc/resolv.conf, with NetworkManager, turned on twice;
#  - a resolv.conf that is a link, without NetworkManager;
#  - one wired by the version before, which set NetworkManager's DNS.
#
# usage: dns_apply.sh <helper>
set -u

helper=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $*"
	failures=$((failures + 1))
}
ok() {
	echo "ok   $*"
}

mkdir -p "$work/bin"
cat > "$work/bin/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$TILEWIN_DNS_ROOT/calls"
case "$1" in
is-active)
	for unit; do :; done
	grep -qx "$unit" "$TILEWIN_DNS_ROOT/active" 2>/dev/null ;;
*) exit 0 ;;
esac
EOF
for tool in nmcli networkctl; do
	printf '#!/bin/sh\necho "%s $*" >> "$TILEWIN_DNS_ROOT/calls"\n' "$tool" > "$work/bin/$tool"
done
printf '#!/bin/sh\necho "UNCONN 0 0 127.0.0.153:53 0.0.0.0:*"\n' > "$work/bin/ss"
chmod +x "$work/bin/"*

# A computer: the services that run, then files as name=content lines below.
computer() {
	root=$work/$1
	shift
	mkdir -p "$root/etc" "$root/run/systemd/resolve" "$root/var/lib"
	: > "$root/active"
	for unit in "$@"; do
		echo "$unit" >> "$root/active"
	done
}

apply() {
	TILEWIN_DNS_ROOT=$root TILEWIN_DNS_PATH=$work/bin:/usr/bin:/bin bash "$helper" "$@" \
		< /dev/null > "$root/out" 2>&1 || fail "$root: '$*' failed: $(cat "$root/out")"
}

has() { # file pattern what
	if grep -q -- "$2" "$root/$1" 2>/dev/null; then ok "$3"; else fail "$3"; fi
}

gone() { # file what
	if [ -e "$root/$1" ]; then fail "$2"; else ok "$2"; fi
}

echo "== resolved and NetworkManager"
computer resolved-nm systemd-resolved.service NetworkManager.service tilewin-dnsd.service
printf 'nameserver 127.0.0.53\n' > "$root/run/systemd/resolve/stub-resolv.conf"
printf 'nameserver 192.168.0.1\nnameserver 1.1.1.1\n' > "$root/run/systemd/resolve/resolv.conf"
ln -s "$root/run/systemd/resolve/stub-resolv.conf" "$root/etc/resolv.conf"
apply enable
has etc/systemd/resolved.conf.d/tilewin-dns.conf '^DNS=127.0.0.153$' "resolved asks the service"
has etc/systemd/resolved.conf.d/tilewin-dns.conf '^Domains=~\.$' "for all names"
has etc/NetworkManager/conf.d/zz-tilewin-dns.conf '^dns=none$' \
	"NetworkManager gives resolved no servers of the networks beside it"
has var/lib/tilewin-dns/network-resolv.conf '192.168.0.1' "the network's servers are kept"
has var/lib/tilewin-dns/wired '^resolved$' "it says how it was wired"
[ -L "$root/etc/resolv.conf" ] && ok "resolv.conf stays resolved's link" ||
	fail "resolv.conf was replaced although resolved answers"
# resolved last, so it forgets what NetworkManager gave it before
if grep -n "" "$root/calls" | grep -E "nmcli general reload|systemctl try-restart systemd-resolved" |
		tail -1 | grep -q "systemd-resolved"; then
	ok "resolved is restarted after NetworkManager is reloaded"
else
	fail "resolved is not restarted after NetworkManager is reloaded"
fi
apply disable
gone etc/systemd/resolved.conf.d/tilewin-dns.conf "off: resolved's file is gone"
gone etc/NetworkManager/conf.d/zz-tilewin-dns.conf "off: NetworkManager's file is gone"
has calls 'systemctl disable --now tilewin-dnsd.service' "off: the service is stopped"

echo "== resolved and systemd-networkd"
computer resolved-networkd systemd-resolved.service systemd-networkd.service
mkdir -p "$root/etc/systemd/network" "$root/usr/lib/systemd/network"
printf '[Match]\nName=en*\n' > "$root/etc/systemd/network/20-wired.network"
printf '[Match]\nName=wl*\n' > "$root/usr/lib/systemd/network/80-wifi.network"
printf 'hosts: mymachines resolve [!UNAVAIL=return] files dns\n' > "$root/etc/nsswitch.conf"
printf 'nameserver 10.0.0.1\n' > "$root/etc/resolv.conf" # not the stub: nss-resolve says it
apply enable
has etc/systemd/resolved.conf.d/tilewin-dns.conf '^DNS=127.0.0.153$' \
	"with nss-resolve, resolved asks the service"
has etc/systemd/network/20-wired.network.d/zz-tilewin-dns.conf '^DNSDefaultRoute=no$' \
	"a network of /etc is no default route for DNS"
has etc/systemd/network/80-wifi.network.d/zz-tilewin-dns.conf '^DNSDefaultRoute=no$' \
	"a network of /usr/lib is none either, by a drop-in in /etc"
gone etc/NetworkManager/conf.d/zz-tilewin-dns.conf "no file for a NetworkManager that is not there"
has calls 'networkctl reload' "systemd-networkd reloads"
apply disable
gone etc/systemd/network/20-wired.network.d "off: the drop-ins are gone"
gone etc/systemd/network/80-wifi.network.d "off: also those for /usr/lib"
[ -f "$root/usr/lib/systemd/network/80-wifi.network" ] && ok "off: the networks themselves stay" ||
	fail "a network file was removed"

echo "== a plain resolv.conf, with NetworkManager"
computer plain-nm NetworkManager.service
printf '# Generated by NetworkManager\nnameserver 10.1.1.1\n' > "$root/etc/resolv.conf"
apply enable
has etc/resolv.conf '^nameserver 127.0.0.153$' "resolv.conf names the service"
has var/lib/tilewin-dns/resolv.conf.before 'nameserver 10.1.1.1' "the old one is put aside"
has var/lib/tilewin-dns/network-resolv.conf 'nameserver 10.1.1.1' "its servers are kept for the service"
has etc/NetworkManager/conf.d/zz-tilewin-dns.conf '^rc-manager=unmanaged$' \
	"NetworkManager no longer writes resolv.conf"
apply enable # again, as an update does
has var/lib/tilewin-dns/resolv.conf.before 'nameserver 10.1.1.1' \
	"turned on twice, what is put aside is still the old one"
has etc/resolv.conf '^nameserver 127.0.0.153$' "and resolv.conf names the service"
apply disable
has etc/resolv.conf 'nameserver 10.1.1.1' "off: the old resolv.conf is back"
gone var/lib/tilewin-dns/resolv.conf.before "off: nothing is left aside"

echo "== a resolv.conf that is a link, nothing else"
computer plain-link
mkdir -p "$root/etc/resolvconf"
printf 'nameserver 10.2.2.2\n' > "$root/etc/resolvconf/resolv.conf"
ln -s "$root/etc/resolvconf/resolv.conf" "$root/etc/resolv.conf"
apply enable
[ -L "$root/etc/resolv.conf" ] && fail "the link was written through" ||
	ok "the link is put aside, not written through"
has etc/resolvconf/resolv.conf 'nameserver 10.2.2.2' "the file it pointed at is untouched"
apply disable
[ -L "$root/etc/resolv.conf" ] && ok "off: resolv.conf is the link again" ||
	fail "off: the link did not come back"

echo "== wired by the version before"
computer old-version systemd-resolved.service NetworkManager.service
ln -s "$root/run/systemd/resolve/stub-resolv.conf" "$root/etc/resolv.conf"
mkdir -p "$root/etc/NetworkManager/conf.d"
printf '[global-dns-domain-*]\nservers=127.0.0.153\n' > "$root/etc/NetworkManager/conf.d/zz-tilewin-dns.conf"
apply enable
if grep -q 'global-dns' "$root/etc/NetworkManager/conf.d/zz-tilewin-dns.conf"; then
	fail "the DNS for all networks of the version before is still set"
else
	ok "the version before's setting is replaced"
fi
has etc/systemd/resolved.conf.d/tilewin-dns.conf '^DNS=127.0.0.153$' "resolved asks the service now"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
