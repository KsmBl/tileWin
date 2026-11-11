#!/bin/bash
# install.sh with stand-ins for apt, sudo, pkg-config and meson, on a system
# without pacman and dnf:
#  - Debian 13, without wlroots 0.20: apt gets no name Debian lacks in the list
#    that must not fail (libwlroots-0.20-dev), the lock screen's PAM is there,
#    and what building wlroots takes; wlroots and the libraries it needs newer
#    are downloaded into subprojects/ and linked in statically, the keyboard
#    layouts looked for where the system has them, and their files left out of
#    the installation;
#  - a Debian that has libwlroots-0.20-dev: it is installed, and tileWin is
#    built against it as on Arch, unless wlroots is in subprojects/ from before:
#    meson.build takes it from there, so it stays static;
#  - every subproject install.sh downloads has its wrap.
#
# usage: install_debian.sh <source dir>
set -u

source_dir=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ok() { echo "ok   $1"; }
check() { # description, command...
	local what=$1
	shift
	if "$@"; then ok "$what"; else fail "$what"; fi
}

# the programs of this computer, but none of the package managers or of what
# the stand-ins are for
mkdir -p "$work/usr" "$work/bin"
find /usr/bin -maxdepth 1 ! -type d ! -name pacman ! -name dnf ! -name apt-get ! -name apt-cache \
	! -name sudo ! -name pkg-config ! -name meson ! -name ninja -print0 |
	xargs -0 ln -sft "$work/usr" 2>/dev/null
log=$work/log
cat > "$work/bin/sudo" <<'EOF'
#!/bin/sh
exec "$@"
EOF
cat > "$work/bin/apt-get" <<EOF
#!/bin/sh
echo "apt-get \$*" >> "$log"
EOF
# apt-cache show: whether this Debian has the package
cat > "$work/bin/apt-cache" <<EOF
#!/bin/sh
[ "\$2" = libwlroots-0.20-dev ] && [ -n "\${HAS_WLROOTS:-}" ]
EOF
# pkg-config: wlroots 0.20 only when this Debian has it, the layouts of xkb-data
cat > "$work/bin/pkg-config" <<'EOF'
#!/bin/sh
case "$*" in
*wlroots-0.20*) [ -n "${HAS_WLROOTS:-}" ] ;;
"--variable=xkb_base xkeyboard-config") echo /usr/share/X11/xkb ;;
*) exit 0 ;;
esac
EOF
cat > "$work/bin/meson" <<EOF
#!/bin/sh
echo "meson \$*" >> "$log"
if [ "\$1" = setup ]; then
	mkdir -p "\$2" && touch "\$2/build.ninja"
fi
EOF
printf '#!/bin/sh\nexit 0\n' > "$work/bin/ninja"
chmod +x "$work/bin"/*

# install.sh runs in a copy of the source of its own, without the subprojects a
# developer may have downloaded
src=$work/src
mkdir -p "$src/subprojects"
cp "$source_dir/install.sh" "$src/"
cp "$source_dir"/subprojects/*.wrap "$src/subprojects/"

install() { # build dir
	rm -f "$log"
	PATH="$work/bin:$work/usr" HOME=$work/home TILEWIN_BUILD_DIR=$1 \
		bash "$src/install.sh" --destdir "$work/stage" --no-user-config > "$work/out" 2>&1
}

# ---------- Debian 13: no wlroots 0.20 ----------
install "$work/build"
check "install.sh goes through on Debian 13" grep -q "tileWin is installed" "$work/out"
first=$(grep -m1 "^apt-get install" "$log")
check "  the list that has to install has no wlroots in it" bash -c "[[ '$first' != *wlroots* ]]"
check "  it has PAM, for the lock screen" bash -c "[[ ' $first ' == *' libpam0g-dev '* ]]"
check "  pkexec and polkitd, which Debian 13 has apart" bash -c "[[ ' $first ' == *' pkexec '* && ' $first ' == *' polkitd '* ]]"
check "  libwlroots-0.20-dev is not asked for" bash -c "! grep -q libwlroots-0.20-dev '$log'"
check "  what building wlroots takes is installed" \
	grep -q "^apt-get install -y libseat-dev libdisplay-info-dev libliftoff-dev hwdata .* bison$" "$log"
check "  wlroots and its newer libraries are downloaded into subprojects/" \
	grep -qx "meson subprojects download wlroots wayland wayland-protocols libdrm libxkbcommon pixman" "$log"
setup=$(grep "^meson setup" "$log")
check "  and linked in statically" bash -c "[[ '$setup' == *' --default-library=static '* ]]"
check "  libxkbcommon finds the layouts of the system" \
	bash -c "[[ '$setup' == *' -Dlibxkbcommon:xkb-config-root=/usr/share/X11/xkb '* ]]"
check "  pixman comes from subprojects/ from the start, the system's is too old for wlroots" \
	bash -c "[[ '$setup' == *' --force-fallback-for=pixman-1'* ]]"
check "  and its locales" bash -c "[[ '$setup' == *' -Dlibxkbcommon:x-locale-root=/usr/share/X11/locale'* ]]"
check "  the files of the subprojects are not installed" grep -q "^meson install .*--skip-subprojects" "$log"

install "$work/build"
check "a second run builds in the same set-up" bash -c "! grep -q '^meson setup' '$log'"

# ---------- a Debian with wlroots 0.20 ----------
export HAS_WLROOTS=1
install "$work/build-system"
check "with libwlroots-0.20-dev in Debian it is installed" grep -qx "apt-get install -y libwlroots-0.20-dev" "$log"
check "  nothing is downloaded" bash -c "! grep -q 'subprojects download' '$log'"
check "  and tileWin is linked against it" bash -c "! grep '^meson setup' '$log' | grep -q 'default-library'"
mkdir -p "$src/subprojects/wlroots-0.20.2"
install "$work/build-before"
check "wlroots in subprojects/ from before stays static" grep -q "^meson setup .* --default-library=static" "$log"
unset HAS_WLROOTS

# ---------- the wraps ----------
names=$(sed -n 's/.*"\$MESON" subprojects download \([a-z0-9 -]*\) .*/\1/p' "$source_dir/install.sh")
check "install.sh names the subprojects it downloads" test -n "$names"
for name in $names; do
	wrap=$source_dir/subprojects/$name.wrap
	check "  $name has a wrap with a checksum" grep -Eq '^source_hash = [0-9a-f]{64}$' "$wrap"
done

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
