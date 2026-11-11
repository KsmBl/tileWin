#!/bin/sh
# tilewin-pam-setup on a /etc/pam.d of its own:
#  - Arch: the services that include system-login and system-local-login;
#  - Debian: the ones that include common-auth and the other common-*, and the
#    greeter's, which includes nothing, as on Arch;
#  - a service that is there stays, unless it includes what the system does not
#    have (one of Arch on Debian): then it is put there anew;
#  - the services of Debian include only what Debian has.
#
# usage: pam_setup.sh <source dir>
set -u

source_dir=$1
setup=$source_dir/scripts/tilewin-pam-setup
data=$source_dir/data

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ok() { echo "ok   $1"; }
check() { # description, command...
	what=$1
	shift
	if "$@"; then ok "$what"; else fail "$what"; fi
}
services="tilewin-lock tilewin-dm tilewin-dm-greeter tilewin-dm-autologin"

# ---------- Arch ----------
arch=$work/arch
mkdir -p "$arch"
touch "$arch/system-login" "$arch/system-local-login" "$arch/login"
sh "$setup" "$data" "$arch" > "$work/out" 2>&1 || fail "it failed on Arch: $(cat "$work/out")"
for s in $services; do
	check "Arch: $s is the one of Arch" cmp -s "$arch/$s" "$data/$s.pam"
done
check "  the lock screen includes system-local-login" grep -q "include system-local-login" "$arch/tilewin-lock"

# ---------- Debian ----------
debian=$work/debian
mkdir -p "$debian"
touch "$debian/common-auth" "$debian/common-account" "$debian/common-session" \
	"$debian/common-password" "$debian/login"
sh "$setup" "$data" "$debian" > "$work/out" 2>&1 || fail "it failed on Debian: $(cat "$work/out")"
for s in tilewin-lock tilewin-dm tilewin-dm-autologin; do
	check "Debian: $s is the one of Debian" cmp -s "$debian/$s" "$data/pam-debian/$s.pam"
done
check "  the greeter's is the same as on Arch" cmp -s "$debian/tilewin-dm-greeter" "$data/tilewin-dm-greeter.pam"
check "  no service includes system-login" sh -c "! grep -hv '^#' '$debian'/tilewin-* | grep -q 'system-login\|system-local-login'"
check "  the lock screen checks the password with common-auth" grep -qx "@include common-auth" "$debian/tilewin-lock"

# every include of the services of Debian is one Debian has
for f in "$data"/pam-debian/*.pam; do
	sed -n 's/^@include[[:space:]]*//p; s/^[^#@][^[:space:]]*[[:space:]]*\(include\|substack\)[[:space:]]*//p' "$f" |
	while read -r included; do
		[ -e "$debian/$included" ] || echo "$(basename "$f") includes $included"
	done
done > "$work/includes"
check "the services of Debian include only what Debian has" test ! -s "$work/includes"

# ---------- what is there already ----------
echo "auth include common-auth # by hand" > "$debian/tilewin-lock"
cp "$data/tilewin-dm.pam" "$debian/tilewin-dm"
sh "$setup" "$data" "$debian" > "$work/out" 2>&1
check "a service of the administrator's own stays" grep -q "by hand" "$debian/tilewin-lock"
check "one of Arch on Debian is put there anew" cmp -s "$debian/tilewin-dm" "$data/pam-debian/tilewin-dm.pam"
check "  which it says" grep -q "tilewin-dm again" "$work/out"
check "  and the others are left alone" sh -c "! grep -q 'tilewin-lock\|autologin\|greeter' '$work/out'"

check "no /etc/pam.d, nothing to do" sh "$setup" "$data" "$work/none"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
