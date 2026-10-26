#!/bin/bash
# tilewin-kernel and tilewin-kernel-root, the two scripts behind the Kernel
# page, on a kernel tree of their own: a Makefile that does what the targets
# of Linux do to .config, and scripts/config. The root helper works in a
# directory of its own (TILEWIN_KERNEL_ROOT) with stand-ins for mkinitcpio,
# depmod, efibootmgr (a pretend NVRAM), the GRUB tools, systemctl and kexec,
# which write down what they were asked.
#
#  - the releases: newest first, the long-term branches marked; the drivers in
#    use that a kernel built here would miss
#  - a build: the download checked against sha256sums.asc, the profiles, the
#    compression of the kernel and its modules, live patching, the name
#    VERSION-tilewinN, the modules and DKMS files collected; a download that
#    does not match fails; cancelling; deleting
#  - putting a kernel on where GRUB lists unified kernel images: the image on
#    the EFI partition, the modules marked as ours, grub.cfg, grubenv, the
#    kernel of the distribution and the boot order untouched; booting it once
#    through BootNext; loading it with kexec; taking it off again
#  - refusing: a folder that is not ours, the running kernel, an EFI partition
#    that is too full (and nothing left behind), a tool not on the list
#  - GRUB without UKIs: grub.cfg made anew only when grub-script-check passes
#    it, grub-reboot for booting once; systemd-boot's one-shot entry
#
# usage: kernel_tools.sh <tilewin-kernel> <tilewin-kernel-root> <source dir>
set -u

tool=$1
root_tool=$2
source_dir=$3
for needed in python3 xz tar sha256sum flock; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done

work=$(mktemp -d)
# KERNEL_TEST_KEEP=1 keeps the files, to look into a failure
[ -n "${KERNEL_TEST_KEEP:-}" ] && echo "files in $work" || trap 'rm -rf "$work"' EXIT
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

export HOME=$work/home XDG_DATA_HOME=$work/home/data XDG_CACHE_HOME=$work/home/cache
mkdir -p "$HOME"
store=$XDG_DATA_HOME/tileWin/kernels

# ---------- a kernel tree of our own ----------

mk_tree() { # version dir
	local v=$1 t=$2/linux-$1
	[[ $v == *.*.* ]] || v=$v.0 # what the Makefile of 9.10 says: SUBLEVEL = 0
	mkdir -p "$t/scripts" "$t/arch/x86/include" "$t/include/linux"
	cat > "$t/Kconfig" <<'EOF'
config FOO_DRIVER
	tristate "Foo"

config FOO_TEST
	tristate "Foo test"

config BAR_BOOL
	bool "Bar"

menuconfig MD
	bool "Multiple devices"

config DM_INTEGRITY
	tristate "Integrity target"

config BLK_DEV_NVME
	tristate "NVM Express"
EOF
	echo '/* header */' > "$t/include/linux/foo.h"
	# what the fixes of kernel.org change: built (an object beside it) or not
	mkdir -p "$t/fs" "$t/drivers"
	echo one > "$t/fs/a.c"; [ "$v" = 9.9.1 ] && echo fixed1 >> "$t/fs/a.c"
	echo two > "$t/fs/b.c"
	echo three > "$t/drivers/unbuilt.c"
	mkdir -p "$t/Documentation"; echo doc > "$t/Documentation/x.txt"
	touch "$t/fs/a.o" "$t/fs/b.o"
	cat > "$t/scripts/config" <<'EOF'
#!/bin/bash
file=.config
put() { sed -i "/^CONFIG_$1=/d; /^# CONFIG_$1 is not set$/d" "$file"; echo "$2" >> "$file"; }
while [ $# -gt 0 ]; do
	case $1 in
	--file) file=$2; shift 2 ;;
	--enable) put "$2" "CONFIG_$2=y"; shift 2 ;;
	--module) put "$2" "CONFIG_$2=m"; shift 2 ;;
	--disable) put "$2" "# CONFIG_$2 is not set"; shift 2 ;;
	--set-str) put "$2" "CONFIG_$2=\"$3\""; shift 3 ;;
	*) shift ;;
	esac
done
EOF
	chmod +x "$t/scripts/config"
	cat > "$t/allmod.sh" <<'EOF'
#!/bin/bash
# every tristate a module, unless the allconfig file says otherwise
for s in $(awk '/^config/ { s = $2 } /tristate/ { print s }' Kconfig); do
	line=$(grep -E "^(CONFIG_$s=|# CONFIG_$s is not set)" "$KCONFIG_ALLCONFIG" | tail -n 1)
	[ -n "$line" ] || line="CONFIG_$s=m"
	sed -i "/^CONFIG_$s=/d; /^# CONFIG_$s is not set$/d" .config
	echo "$line" >> .config
done
EOF
	chmod +x "$t/allmod.sh"
	mkdir -p "$t/scripts/livepatch"
	cat > "$t/scripts/livepatch/klp-build" <<'KLP'
#!/bin/bash
# a patch with "broken" in it does not build, as one that touches data would
# not, and the error names its file
out= patches=()
[ -n "${TW_KLP_CALLS:-}" ] && echo "klp-build $*" >> "$TW_KLP_CALLS"
while [ $# -gt 0 ]; do
	case $1 in
	-o) out=$2; shift 2 ;;
	-j|-S) shift 2 ;;
	-T) mkdir -p klp-tmp/1-orig; touch klp-tmp/1-orig/.complete; shift ;;
	--no-replace) shift ;;
	*) patches+=("$1"); shift ;;
	esac
done
[ -f .config ] || { echo "error: not a kernel tree"; exit 1; }
for patch in "${patches[@]}"; do
	if grep -q broken "$patch"; then
		f=$(sed -n 's|^+++ b/||p' "$patch" | head -n 1)
		echo "${f:-x.c}:1:1: error: changed data: cannot be patched live"
		exit 1
	fi
done
echo "livepatch module" > "$out"
KLP
	chmod +x "$t/scripts/livepatch/klp-build"
	cat > "$t/Makefile" <<EOF
.PHONY: olddefconfig localmodconfig allmodconfig syncconfig kernelrelease bzImage modules modules_install
olddefconfig:
	@true
syncconfig:
	@mkdir -p include/config && grep '^CONFIG_' .config > include/config/auto.conf || true
localmodconfig:
	@cp "\$(LSMOD)" $work/home/cache/tileWin/kernel/lsmod-used && ./scripts/config --disable UNUSED_DRIVER
allmodconfig:
	@./allmod.sh
kernelrelease:
	@echo $v\$\$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"/\1/p' include/config/auto.conf 2>/dev/null)
bzImage:
	@echo "  CC      init/main.o"; mkdir -p arch/x86/boot; echo kernel-$v > arch/x86/boot/bzImage
	@echo map > System.map; touch Module.symvers
	@[ -z "\$\$TW_FAKE_SLOW" ] || sleep 60
modules:
	@echo "  CC [M]  drivers/foo.o"
modules_install:
	@d=\$(INSTALL_MOD_PATH)/lib/modules/\$\$(\$(MAKE) -s kernelrelease); mkdir -p \$\$d/kernel; \\
		echo ko > \$\$d/kernel/foo.ko; ln -sfn /nowhere \$\$d/build; ln -sfn /nowhere \$\$d/source
EOF
}

mirror=$work/mirror
mkdir -p "$mirror/v9.x" "$mirror/v8.x" "$work/trees"
mk_tree 9.9.1 "$work/trees"
mk_tree 9.10 "$work/trees"
(cd "$work/trees" && tar -cJf "$mirror/v9.x/linux-9.9.1.tar.xz" linux-9.9.1 &&
	tar -cJf "$mirror/v9.x/linux-9.10.tar.xz" linux-9.10)
(cd "$mirror/v9.x" && sha256sum linux-9.9.1.tar.xz linux-9.10.tar.xz > sha256sums.asc &&
	echo "0000000000000000000000000000000000000000000000000000000000000000  linux-9.9.tar.xz" >> sha256sums.asc)
cp "$mirror/v9.x/linux-9.9.1.tar.xz" "$mirror/v9.x/linux-9.9.tar.xz" # a broken checksum
for f in linux-9.10.tar.xz linux-9.9.1.tar.xz linux-9.9.tar.xz linux-9.8.5.tar.xz; do
	echo "<a href=\"$f\">$f</a>"
done > "$mirror/v9.x/index.html"
echo '<a href="linux-8.1.2.tar.xz">linux-8.1.2.tar.xz</a>' > "$mirror/v8.x/index.html"
cat > "$work/releases.json" <<'EOF'
{"latest_stable": {"version": "9.9.1"}, "releases": [
 {"moniker": "stable", "version": "9.9.1", "iseol": false, "source": null},
 {"moniker": "longterm", "version": "8.1.2", "iseol": false, "source": null},
 {"moniker": "linux-next", "version": "next-1", "iseol": false, "source": null}]}
EOF
# the index pages are fetched as v9.x/, which is a folder here
mkdir -p "$work/bin"
cat > "$work/bin/curl" <<EOF
#!/bin/bash
out= url=
while [ \$# -gt 0 ]; do
	case \$1 in -o) out=\$2; shift 2 ;; --) url=\$2; shift 2 ;; -*) shift ;; *) url=\$1; shift ;; esac
done
path=\${url#file://}
path=\${path/#http:\/\/mirror.test/$mirror}
[ -d "\$path" ] && path=\$path/index.html
cp "\$path" "\$out"
EOF
chmod +x "$work/bin/curl"
# programs the check wants that this computer may not have
for t in make gcc ld bc flex bison perl openssl cpio xz zstd gzip curl python3 tar pahole lz4 lzop; do
	command -v "$t" >/dev/null 2>&1 || { printf '#!/bin/sh\nexit 0\n' > "$work/bin/$t"; chmod +x "$work/bin/$t"; }
done
mkdir -p "$work/include/openssl"
touch "$work/include/libelf.h" "$work/include/openssl/ssl.h"
export PATH="$work/bin:$PATH"
export TILEWIN_KERNEL_MIRROR=http://mirror.test TILEWIN_KERNEL_RELEASES=file://$work/releases.json
export TILEWIN_KERNEL_FRAGMENTS=$source_dir/kernel TILEWIN_KERNEL_INCLUDE=$work/include
export TILEWIN_KERNEL_JOBS=2 TILEWIN_KERNEL_NEED_GB=0 TILEWIN_KERNEL_LSMOD=$work/lsmod TILEWIN_KERNEL_CONFIG=$work/base.config
printf 'Module Size Used by\ndm_crypt 1 0\n' > "$work/lsmod"
# what the initramfs of the running kernel holds
echo dm_integrity > "$work/boot-modules"
export TILEWIN_KERNEL_BOOT_MODULES=$work/boot-modules TILEWIN_KERNEL_NO_BOOT_CHECK=1
cat > "$work/base.config" <<'EOF'
CONFIG_BAR_BOOL=y
# CONFIG_FOO_DRIVER is not set
# CONFIG_FOO_TEST is not set
CONFIG_UNUSED_DRIVER=m
CONFIG_KERNEL_GZIP=y
CONFIG_MODULE_COMPRESS=y
CONFIG_MODULE_COMPRESS_GZIP=y
# CONFIG_LIVEPATCH is not set
CONFIG_LOCALVERSION=""
EOF

K() { bash "$tool" "$@"; }

# ---------- the releases ----------

got=$(K versions | tr '\t' '|' | tr '\n' ' ')
want="9.10| 9.9.1|stable, latest 9.9| 9.8.5| 8.1.2|longterm "
[ "$got" = "$want" ] && ok "the releases, newest first: $got" ||
	fail "the releases should be \"$want\", are \"$got\""
[ "$(K latest)" = 9.9.1 ] && ok "latest is 9.9.1" || fail "latest should be 9.9.1"
[ -z "$(K check zstd | grep -v ':optional$')" ] && ok "nothing missing for a build" ||
	fail "missing: $(K check zstd)"
got=$(TILEWIN_KERNEL_INCLUDE=$work/none K check lzo | cut -d: -f2 | tr '\n' ' ')
[[ $got == *libelf* && $got == *openssl* ]] && ok "missing headers are named with their packages" ||
	fail "the check missed the headers: $got"

# a driver of a package for the distribution's kernel, one of DKMS, one of Linux
mkdir -p "$work/sys/module/nvidia" "$work/sys/module/vboxdrv" "$work/sys/module/ext4"
echo OE > "$work/sys/module/nvidia/taint"
echo OE > "$work/sys/module/vboxdrv/taint"
echo > "$work/sys/module/ext4/taint"
printf '#!/bin/sh\ncase $2 in nvidia) echo /usr/lib/modules/x/extramodules/nvidia.ko.zst ;; *) echo /usr/lib/modules/x/updates/dkms/$2.ko.zst ;; esac\n' > "$work/bin/modinfo"
printf '#!/bin/sh\necho nvidia-open\n' > "$work/bin/pacman"
chmod +x "$work/bin/modinfo" "$work/bin/pacman"
got=$(TILEWIN_KERNEL_SYS=$work/sys K foreign | tr '\t\n' ': ')
[ "$got" = "nvidia:nvidia-open " ] && ok "the drivers a new kernel will not have: $got" ||
	fail "the drivers a new kernel will not have should be nvidia of nvidia-open, are \"$got\""
rm "$work/bin/modinfo" "$work/bin/pacman"

# ---------- the modules this computer used ----------

printf 'Module Size Used by\ndm_crypt 1 0\nusb_storage 1 0\n' > "$work/lsmod"
K record-modules
printf 'Module Size Used by\ndm_crypt 1 0\n' > "$work/lsmod"
K record-modules
check "the modules seen are kept, also once they are gone" test "$(K seen | cut -f1)" = 2

# ---------- building ----------

has() { grep -qx -- "$2" "$store/$1/config"; }
K build latest device zstd on > "$work/out" 2>&1
k1=9.9.1-tilewin1
check "a build of the latest release is $k1, built" test "$(cat "$store/$k1/state" 2>/dev/null)" = built
check "  live patching is on" has $k1 CONFIG_LIVEPATCH=y
check "  the kernel is zstd compressed" has $k1 CONFIG_KERNEL_ZSTD=y
check "  and no longer gzip" has $k1 "# CONFIG_KERNEL_GZIP is not set"
check "  the modules are zstd compressed" has $k1 CONFIG_MODULE_COMPRESS_ZSTD=y
check "  it is called -tilewin1" has $k1 'CONFIG_LOCALVERSION="-tilewin1"'
check "  only what is in use (localmodconfig)" has $k1 "# CONFIG_UNUSED_DRIVER is not set"
check "  and what the initramfs needs, loaded or not" grep -qx "dm_integrity 0 0" "$XDG_CACHE_HOME/tileWin/kernel/lsmod-used"
check "  and what was loaded before (usb_storage)" grep -qx "usb_storage 0 0" "$XDG_CACHE_HOME/tileWin/kernel/lsmod-used"
check "  not for this processor only" has $k1 "# CONFIG_X86_NATIVE_CPU is not set"
check "  klp-build can make livepatches for it" grep -qx klp=yes "$store/$k1/info"
check "  and what it takes to start a computer" has $k1 CONFIG_DM_INTEGRITY=m
check "  an option that is a bool is switched on, not made a module" has $k1 CONFIG_MD=y
check "  kexec stays possible" has $k1 CONFIG_KEXEC_FILE=y
check "  the modules are collected" test -f "$store/$k1/modules/lib/modules/$k1/kernel/foo.ko"
check "  without links into the build tree" test ! -e "$store/$k1/modules/lib/modules/$k1/build" -a ! -L "$store/$k1/modules/lib/modules/$k1/build"
check "  the files for DKMS are kept" test -f "$store/$k1/headers/include/linux/foo.h" -a -f "$store/$k1/headers/Kconfig"
check "  with live patching its build tree is kept, for klp-build" test -d "$XDG_CACHE_HOME/tileWin/kernel/build/$k1"
check "  the info names its profile" grep -qx profile=device "$store/$k1/info"

K build 9.9.1 portable xz off on > "$work/out" 2>&1
k2=9.9.1-tilewin2
check "a second build is $k2" test "$(cat "$store/$k2/state" 2>/dev/null)" = built
check "  common hardware is added (NVMe)" has $k2 CONFIG_BLK_DEV_NVME=m
check "  xz for the kernel and its modules" has $k2 CONFIG_MODULE_COMPRESS_XZ=y
check "  live patching is off" has $k2 "# CONFIG_LIVEPATCH is not set"
check "  for this processor only" has $k2 CONFIG_X86_NATIVE_CPU=y
check "  and it says so" grep -qx native=on "$store/$k2/info"
check "  without live patching its build tree goes" test ! -e "$XDG_CACHE_HOME/tileWin/kernel/build/$k2"

# ---------- live fixes ----------

printf 'fix\n' > "$work/Fix-The-Crash.patch"
K patch-build $k1 "$work/Fix-The-Crash.patch" > "$work/out" 2>&1
check "a patch becomes a livepatch module, named after its file" test -s "$store/$k1/livepatches/fix_the_crash.ko"
check "  listed as built" test "$(K livepatches $k1 | cut -f1,2 | tr '\t' :)" = "fix_the_crash:built"
printf 'broken\n' > "$work/data.patch"
K patch-build $k1 "$work/data.patch" > "$work/out" 2>&1
check "a patch klp-build cannot take fails, and says why" \
	grep -q "changed data" "$store/$k1/livepatches/data.progress"
K patch-build $k2 "$work/Fix-The-Crash.patch" > "$work/out" 2>&1
check "a kernel without its build tree cannot be fixed live" grep -q "keeps no build tree" "$work/out"

# the fixes of kernel.org: 9.9 to 9.9.1 and to 9.9.3, as kernel.org has them
mkdir -p "$work/stable/a/fs" "$work/stable/a/drivers" "$work/stable/a/include/linux" \
	"$work/stable/a/Documentation"
(
	cd "$work/stable"
	echo one > a/fs/a.c; echo two > a/fs/b.c; echo three > a/drivers/unbuilt.c
	echo '/* header */' > a/include/linux/foo.h; echo doc > a/Documentation/x.txt
	cp -a a b1; echo fixed1 >> b1/fs/a.c
	cp -a a b3; printf 'fixed1\nfixed3\n' >> b3/fs/a.c; echo broken >> b3/fs/b.c
	echo fixed > b3/drivers/unbuilt.c; echo '/* fixed */' >> b3/include/linux/foo.h
	echo fixed >> b3/Documentation/x.txt
	mv a linux-9.9 && mv b1 linux-9.9.1 && mv b3 linux-9.9.3
	diff -urN linux-9.9 linux-9.9.1 | xz > "$mirror/v9.x/patch-9.9.1.xz"
	diff -urN linux-9.9 linux-9.9.3 | xz > "$mirror/v9.x/patch-9.9.3.xz"
)
cp "$mirror/v9.x/sha256sums.asc" "$work/sums-before"
(cd "$mirror/v9.x" && sha256sum patch-9.9.1.xz >> sha256sums.asc)
K security-build $k1 > "$work/out" 2>&1
check "without a newer release of its series there is nothing to fix" grep -q "no fixes for it yet" "$work/out"
(cd "$mirror/v9.x" && sha256sum patch-9.9.3.xz >> sha256sums.asc)
export TW_KLP_CALLS=$work/klp-calls
: > "$TW_KLP_CALLS"
K security-build $k1 > "$work/out" 2>&1
lp=$store/$k1/livepatches
check "the fixes of kernel.org become a livepatch module, also for a kernel that does not run" \
	test -s "$lp/security_9_9_3.ko" -a "$(cat "$lp/security_9_9_3.state" 2>/dev/null)" = built
check "  what the kernel lacks of them goes in" grep -q "^klp-build .* --no-replace .*/1.patch" "$TW_KLP_CALLS"
check "  only that: what 9.9.1 had already is not in it" \
	bash -c "grep -qx '+fixed3' '$lp/security_9_9_3.patch' && ! grep -q '^+fixed1' '$lp/security_9_9_3.patch'"
check "  a file klp-build refuses is left out, and the rest goes in again" \
	test "$(grep -c . "$TW_KLP_CALLS")" = 2
check "  without building the kernel without them once more" grep -q -- "-S 2" "$TW_KLP_CALLS"
check "  the file it refused is listed" grep -q "^fs/b.c	klp-build: " "$lp/security_9_9_3.left"
check "  and a header" grep -q "^include/linux/foo.h	a header" "$lp/security_9_9_3.left"
check "  but not what is not built into it, nor the documentation" \
	bash -c "! grep -qE 'unbuilt|Documentation' '$lp/security_9_9_3.left'"
check "  and the list says how many" grep -q "1 files live; 2 need a restart" "$lp/security_9_9_3.progress"
check "  its work is gone, and klp-build's" \
	test ! -e "$lp/security_9_9_3.work" -a ! -e "$XDG_CACHE_HOME/tileWin/kernel/build/$k1/klp-tmp"
K security-build $k1 > "$work/out" 2>&1
check "made once" grep -q "made already" "$work/out"
unset TW_KLP_CALLS
check "  listed as built, not at start" test "$(K livepatches $k1 | grep '^security' | cut -f1,2,4 | tr '\t' :)" = "security_9_9_3:built:no"
K security-build $k2 > "$work/out" 2>&1
check "a kernel without its build tree gets no fixes live" grep -q "keeps no build tree" "$work/out"

K build 9.9.1 full lz4 off > "$work/out" 2>&1
k3=9.9.1-tilewin3
check "an \"everything\" build" test "$(cat "$store/$k3/state" 2>/dev/null)" = built
check "  builds the drivers the running kernel leaves off" has $k3 CONFIG_FOO_DRIVER=m
check "  but not the tests" has $k3 "# CONFIG_FOO_TEST is not set"
check "  lz4 leaves the modules uncompressed" has $k3 "# CONFIG_MODULE_COMPRESS is not set"

K build 9.10 device zstd off > "$work/out" 2>&1
check "a release without a third number is called 9.10.0-tilewin1, as its Makefile says" \
	test "$(cat "$store/9.10.0-tilewin1/state" 2>/dev/null)" = built
K delete 9.10.0-tilewin1

K build 9.10 device zstd off off mycustomname > "$work/out" 2>&1
check "a kernel can have a name of its own" \
	test "$(cat "$store/9.10.0-mycustomname/state" 2>/dev/null)" = built
check "  which uname -r gives" grep -qx 'CONFIG_LOCALVERSION="-mycustomname"' "$store/9.10.0-mycustomname/config"
check "  and the list shows" bash -c "bash '$tool' list | cut -f1 | grep -qx 9.10.0-mycustomname"
K build 9.10 device zstd off off mycustomname > "$work/out" 2>&1
check "a name taken already is refused" grep -q "already" "$work/out"
K build 9.10 device zstd off off 'bad/name' > "$work/out" 2>&1
check "a name with a slash in it is refused" grep -q "A name is" "$work/out"
check "  and builds nothing" test ! -e "$store/9.10.0-bad"
K delete 9.10.0-mycustomname

K build 9.9 running zstd off > "$work/out" 2>&1
check "a download that does not match its checksum fails" \
	grep -q "does not match" "$store/9.9.0-tilewin1/progress"
check "  and is not kept" test ! -e "$XDG_CACHE_HOME/tileWin/kernel/src/linux-9.9.tar.xz"
check "bad arguments are refused" bash -c "! bash '$tool' build 9.9.1 nonsense zstd on 2>/dev/null"

TW_FAKE_SLOW=1 K build 9.9.1 device zstd off > "$work/out" 2>&1 &
for _ in $(seq 50); do
	grep -q Compiling "$store/9.9.1-tilewin4/progress" 2>/dev/null && break
	sleep 0.2
done
check "a second build at the same time is refused" bash -c "! bash '$tool' build 9.9.1 device zstd off 2>/dev/null"
K cancel
wait
check "a build can be cancelled" test "$(cat "$store/9.9.1-tilewin4/state" 2>/dev/null)" = cancelled
got=$(K list | cut -f1,2 | sort | tr '\t\n' ': ')
check "the list names them all with their state" test "$got" = "$(printf '%s\n' 9.9.0-tilewin1:failed 9.9.1-tilewin1:built 9.9.1-tilewin2:built 9.9.1-tilewin3:built 9.9.1-tilewin4:cancelled | sort | tr '\n' ' ')"
K delete 9.9.1-tilewin4
check "a built kernel can be deleted" test ! -e "$store/9.9.1-tilewin4"

# ---------- putting it on: GRUB with UKIs ----------

sb=$work/root
esp=$sb/boot
mkdir -p "$esp/EFI/Linux" "$esp/grub" "$sb/etc/kernel" "$sb/usr/lib/modules/7.0.0-arch1" \
	"$sb/sys/firmware/efi/efivars" "$sb/sys/class/block/fakedisk1" "$sb/proc/sys/kernel" "$sb/var/tmp" "$sb/etc/default" \
	"$work/rbin"
echo distro > "$esp/EFI/Linux/arch-linux.efi"
printf 'set default=saved\nuki\n' > "$esp/grub/grub.cfg"
echo grubenv > "$esp/grub/grubenv"
echo 'NAME="Arch Linux"
PRETTY_NAME="Arch Linux"
ID=arch' > "$sb/etc/os-release"
echo "cryptdevice=UUID=x:cryptlvm root=/dev/vg/root rw quiet" > "$sb/etc/kernel/cmdline"
echo 0 > "$sb/proc/sys/kernel/kexec_load_disabled"
echo 1 > "$sb/sys/class/block/fakedisk1/partition"
loaderinfo() { printf '\x06\x00\x00\x00'; printf '%s\0' "$1" | iconv -f UTF-8 -t UTF-16LE; }
loaderinfo "GRUB 2.16" > "$sb/sys/firmware/efi/efivars/LoaderInfo-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"
calls=$work/calls
nvram=$work/nvram
printf 'order=0000\nnext=\n0000=GRUB\n' > "$nvram"
for t in depmod dkms systemctl kexec bootctl grub-reboot lsblk; do
	cat > "$work/rbin/$t" <<EOF
#!/bin/bash
echo "$t \$*" >> $calls
case "$t \$1" in
"bootctl --print-esp-path") exit 1 ;;
"lsblk "*) echo fakedisk ;;
esac
exit 0
EOF
done
cat > "$work/rbin/findmnt" <<EOF
#!/bin/bash
echo /dev/fakedisk1
EOF
cat > "$work/rbin/mkinitcpio" <<EOF
#!/bin/bash
echo "mkinitcpio \$*" >> $calls
while [ \$# -gt 0 ]; do
	case \$1 in
	-g) echo initramfs > "\$2"; shift 2 ;;
	-U) head -c \${TW_UKI_SIZE:-1000} /dev/zero > "\$2"; shift 2 ;;
	--osrelease) cp "\$2" $work/osrelease-used; shift 2 ;;
	*) shift ;;
	esac
done
EOF
cat > "$work/rbin/efibootmgr" <<EOF
#!/bin/bash
# a pretend NVRAM: order=, next= and NUM=LABEL lines
echo "efibootmgr \$*" >> $calls
f=$nvram
get() { sed -n "s/^\$1=//p" "\$f"; }
case \${1:-} in
"")
	echo "BootCurrent: 0000"
	echo "BootOrder: \$(get order)"
	[ -n "\$(get next)" ] && echo "BootNext: \$(get next)"
	grep -E '^[0-9A-F]{4}=' "\$f" | while IFS== read -r n l; do printf 'Boot%s* %s\tHD(1,GPT)/x\n' "\$n" "\$l"; done
	;;
-C)
	label= ; while [ \$# -gt 0 ]; do [ "\$1" = -L ] && label=\$2; shift; done
	n=\$(printf '%04X' \$(( \$(grep -cE '^[0-9A-F]{4}=' "\$f") )))
	echo "\$n=\$label" >> "\$f"
	[ -n "\${TW_FIRMWARE_REORDERS:-}" ] && sed -i "s/^order=.*/order=\$n,\$(get order)/" "\$f"
	;;
-b) sed -i "/^\$2=/d" "\$f" ;;
-n) sed -i "s/^next=.*/next=\$2/" "\$f" ;;
-N) sed -i "s/^next=.*/next=/" "\$f" ;;
-o) sed -i "s/^order=.*/order=\$2/" "\$f" ;;
esac
exit 0
EOF
chmod +x "$work/rbin"/*
export TILEWIN_KERNEL_ROOT=$sb TILEWIN_KERNEL_ESP=$esp TILEWIN_KERNEL_RUNNING=7.0.0-arch1
export TILEWIN_KERNEL_PATH="$work/rbin:/usr/bin:/bin" PKEXEC_UID=$(id -u)
R() { bash "$root_tool" "$@"; }
sums() { sha256sum "$esp/grub/grub.cfg" "$esp/grub/grubenv" "$esp/EFI/Linux/arch-linux.efi" | cut -d' ' -f1; }
before=$(sums)

check "info: GRUB with UKIs, booted once through BootNext" bash -c "bash '$root_tool' info | grep -qx method=uki,bootnext"
check "  Secure Boot is off" bash -c "bash '$root_tool' info | grep -qx secureboot=off"
printf '\x06\x00\x00\x00\x01' > "$sb/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"
check "  or on" bash -c "bash '$root_tool' info | grep -qx secureboot=on"
rm "$sb/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"
: > "$calls"
R install $k1 "$store" > "$work/out" 2>&1
check "install puts the UKI on the EFI partition" test -s "$esp/EFI/Linux/tilewin-$k1.efi"
check "  the modules, marked as ours" test -f "$sb/usr/lib/modules/$k1/.tilewin-kernel" -a -f "$sb/usr/lib/modules/$k1/kernel/foo.ko"
check "  with the kernel, the initramfs for kexec and the DKMS files" \
	test -f "$sb/usr/lib/modules/$k1/vmlinuz" -a -f "$sb/usr/lib/modules/$k1/initramfs-tilewin.img" -a -f "$sb/usr/lib/modules/$k1/build/include/linux/foo.h"
check "  depmod and DKMS ran for it" grep -q "^depmod .*$k1" "$calls"
check "  the image is made with its compression" grep -q -- "-z zstd" "$calls"
check "  and names itself in the boot menu" grep -q 'PRETTY_NAME="Arch Linux (tileWin kernel 9.9.1-tilewin1)"' "$work/osrelease-used"
check "  grub.cfg, grubenv and the distribution's kernel are untouched" test "$(sums)" = "$before"
check "  no file is left half written" bash -c "! ls '$esp/EFI/Linux/'*.new '$sb/usr/lib/modules/'.*.new 2>/dev/null | grep -q ."
check "  the firmware's boot entries are untouched" grep -qx 'order=0000' "$nvram"
check "putting it on twice is refused" bash -c "! bash '$root_tool' install $k1 '$store' 2>/dev/null"

: > "$calls"
R boot $k1 > "$work/out" 2>&1
num=$(sed -n "s/^\([0-9A-F]\{4\}\)=tileWin $k1$/\1/p" "$nvram")
check "boot makes a firmware entry for the UKI" test -n "$num"
check "  points BootNext at it" test -n "$num" -a -n "$(grep -x "next=$num" "$nvram")"
check "  leaves the boot order as it was" grep -qx 'order=0000' "$nvram"
check "  and restarts" grep -qx "systemctl reboot" "$calls"
R boot $k1 > "$work/out" 2>&1
check "booting again takes the same entry" test "$(grep -c "=tileWin $k1$" "$nvram")" = 1
TW_FIRMWARE_REORDERS=1 R boot $k2 "$store" > "$work/out" 2>&1
check "boot with the store puts a kernel on first" test -s "$esp/EFI/Linux/tilewin-$k2.efi"
check "  a firmware that puts the new entry in the boot order gets it taken out again" grep -qx 'order=0000' "$nvram"

: > "$calls"
R load $k1 > "$work/out" 2>&1
check "load starts it with kexec, with the distribution's command line" \
	grep -q "^kexec -s -l $sb/usr/lib/modules/$k1/vmlinuz --initrd=$sb/usr/lib/modules/$k1/initramfs-tilewin.img --command-line=cryptdevice=UUID=x:cryptlvm root=/dev/vg/root rw quiet$" "$calls"
check "  and systemctl kexec" grep -qx "systemctl kexec" "$calls"
# the programs of this computer without kexec, should it have one
mkdir -p "$work/nokexec"
find /usr/bin -maxdepth 1 ! -name kexec ! -name bin -print0 | xargs -0 ln -st "$work/nokexec" 2>/dev/null
find "$work/rbin" -maxdepth 1 -type f ! -name kexec -print0 | xargs -0 ln -sft "$work/nokexec"
TILEWIN_KERNEL_PATH=$work/nokexec R load $k1 > "$work/out" 2>&1
check "load without kexec-tools says so" grep -q "install kexec-tools" "$work/out"

TILEWIN_KERNEL_RUNNING=$k1 R remove $k1 > "$work/out" 2>&1
check "the running kernel is not taken off" test -f "$sb/usr/lib/modules/$k1/.tilewin-kernel"
mkdir -p "$sb/usr/lib/modules/9.9.1-tilewin7"
R remove 9.9.1-tilewin7 > "$work/out" 2>&1
check "a modules folder that is not ours stays" test -d "$sb/usr/lib/modules/9.9.1-tilewin7"
R remove 7.0.0-arch1 > "$work/out" 2>&1
check "the distribution's kernel cannot be named" test -d "$sb/usr/lib/modules/7.0.0-arch1"
R remove $k1 > "$work/out" 2>&1
check "remove takes the UKI off" test ! -e "$esp/EFI/Linux/tilewin-$k1.efi"
check "  the modules" test ! -e "$sb/usr/lib/modules/$k1"
check "  and its firmware entry" bash -c "! grep -q '=tileWin $k1$' '$nvram'"
R remove $k2 > /dev/null 2>&1
check "  a BootNext pointing at an entry taken off goes too" grep -qx 'next=' "$nvram"
check "  but nothing else" test "$(sums)" = "$before" -a -n "$(grep -x 'order=0000' "$nvram")"

# an EFI partition with 20 MB free
printf '#!/bin/bash\necho "Filesystem 1024-blocks Used Available Capacity Mounted on"\necho "/dev/fakedisk1 1000000 979520 20480 98%% /boot"\n' \
	> "$work/rbin/df"
chmod +x "$work/rbin/df"
TW_UKI_SIZE=$((8 * 1024 * 1024)) R install $k1 "$store" > "$work/out" 2>&1
check "a full EFI partition is refused" grep -q "not enough room" "$work/out"
check "  and nothing is left behind" bash -c "[ ! -e '$sb/usr/lib/modules/$k1' ] && ! ls '$esp/EFI/Linux/' | grep -q tilewin"
rm "$work/rbin/df"
check "only build tools are installed" bash -c "bash '$root_tool' tools rm 2>&1 | grep -q 'not a build tool'"

# ---------- live fixes and the scheduler, as root ----------

mkdir -p "$sb/sys/kernel/livepatch"
cat > "$work/rbin/insmod" <<INSMOD
#!/bin/bash
echo "insmod \$*" >> $calls
d=$sb/sys/kernel/livepatch/\$(basename "\$1" .ko)
mkdir -p "\$d"; echo 1 > "\$d/enabled"; echo 0 > "\$d/transition"
INSMOD
cat > "$work/rbin/rmmod" <<RMMOD
#!/bin/bash
echo "rmmod \$*" >> $calls
rm -rf "$sb/sys/kernel/livepatch/\$1"
RMMOD
printf '#!/bin/sh\nexit 0\n' > "$work/rbin/scx_lavd"
chmod +x "$work/rbin"/*
R livepatch-apply $k1 fix_the_crash "$store" > "$work/out" 2>&1
check "a livepatch only goes into the kernel it was made for" grep -q "is not running" "$work/out"
: > "$calls"
TILEWIN_KERNEL_RUNNING=$k1 R livepatch-apply $k1 fix_the_crash "$store" > "$work/out" 2>&1
check "applying one loads it into the running kernel" grep -q "^insmod .*livepatch_fix_the_crash.ko" "$calls"
check "  from a copy of root's own, gone again" bash -c "! ls '$sb/run/tilewin-kernel/' 2>/dev/null | grep -q ."
TILEWIN_KERNEL_RUNNING=$k1 R livepatch-apply $k1 fix_the_crash "$store" > "$work/out" 2>&1
check "  once" grep -q "applied already" "$work/out"
R livepatch-undo fix_the_crash > "$work/out" 2>&1
check "undoing it unloads it" grep -qx "rmmod livepatch_fix_the_crash" "$calls"
check "  and it is gone from the kernel" test ! -e "$sb/sys/kernel/livepatch/livepatch_fix_the_crash"
R livepatch-apply $k1 '../x' "$store" > "$work/out" 2>&1
check "a livepatch name with a path in it is refused" grep -q "not a livepatch" "$work/out"

mkdir -p "$sb/sys/kernel/livepatch/livepatch_security_9_9_2"
echo 0 > "$sb/sys/kernel/livepatch/livepatch_security_9_9_2/transition"
: > "$calls"
TILEWIN_KERNEL_RUNNING=$k1 R livepatch-apply $k1 security_9_9_3 "$store" > "$work/out" 2>&1
check "the newer fixes of kernel.org take the place of the older" \
	grep -qx "rmmod livepatch_security_9_9_2" "$calls"
check "  once they are in" bash -c "grep -n . '$calls' | grep -q '^1:insmod .*security_9_9_3'"
R livepatch-undo security_9_9_3 > /dev/null 2>&1
R livepatch-start $k1 security_9_9_3 "$store" on > "$work/out" 2>&1
check "a fix is set for every start only of a kernel on the computer" grep -q "not on the computer" "$work/out"
mkdir -p "$sb/usr/lib/modules/$k1" && echo "method=uki" > "$sb/usr/lib/modules/$k1/.tilewin-kernel"
: > "$calls"
R livepatch-start $k1 security_9_9_3 "$store" on > "$work/out" 2>&1
check "  then it goes into its modules, running or not" test -f "$sb/usr/lib/modules/$k1/livepatch/livepatch_security_9_9_3.ko"
check "  and the service that applies it is on" grep -qx "systemctl enable tilewin-kernel-livepatch.service" "$calls"
check "  which the list says" test "$(TILEWIN_KERNEL_MODULES=$sb/usr/lib/modules K livepatches $k1 | grep '^security' | cut -f4)" = yes
R livepatch-boot > "$work/out" 2>&1
check "at the start of another kernel nothing is applied" bash -c "! grep -q '^insmod' '$calls'"
TILEWIN_KERNEL_RUNNING=$k1 R livepatch-boot > "$work/out" 2>&1
check "at its own start it is" grep -q "^insmod .*/livepatch/livepatch_security_9_9_3.ko" "$calls"
R livepatch-start $k1 security_9_9_3 "$store" off > "$work/out" 2>&1
check "  and no more once taken off the start" test ! -e "$sb/usr/lib/modules/$k1/livepatch"
rm -rf "$sb/usr/lib/modules/$k1" "$sb/sys/kernel/livepatch"/*

: > "$calls"
R scheduler-default scx_lavd Gaming > "$work/out" 2>&1
check "the scheduler at start goes into scx_loader's config" grep -qx 'default_sched = "scx_lavd"' "$sb/etc/scx_loader/config.toml"
check "  in its mode" grep -qx 'default_mode = "Gaming"' "$sb/etc/scx_loader/config.toml"
check "  and scx_loader starts with the computer" grep -qx "systemctl enable scx_loader.service" "$calls"
R scheduler-default scx_nothere Auto > "$work/out" 2>&1
check "a scheduler that is not installed is refused" grep -q "not installed" "$work/out"
R scheduler-default none Auto > "$work/out" 2>&1
check "none takes the config away again" test ! -e "$sb/etc/scx_loader/config.toml"
printf 'default_sched = "scx_rusty"\n' > "$sb/etc/scx_loader/config.toml"
R scheduler-default scx_lavd Auto > "$work/out" 2>&1
check "a config written by hand stays as it is" grep -qx 'default_sched = "scx_rusty"' "$sb/etc/scx_loader/config.toml"
R scheduler-default none Auto > /dev/null 2>&1
check "  also by none" test -f "$sb/etc/scx_loader/config.toml"

# ---------- GRUB without UKIs ----------

printf 'menuentry "Arch" {\n linux /vmlinuz-linux\n}\n' > "$esp/grub/grub.cfg"
cat > "$work/rbin/grub-mkconfig" <<EOF
#!/bin/bash
echo "grub-mkconfig \$*" >> $calls
out=\$2
{
	echo "submenu 'Advanced' \\\$menuentry_id_option 'gnulinux-advanced-x' {"
	for k in $sb/boot/vmlinuz-*; do
		k=\${k##*/vmlinuz-}
		echo "	menuentry 'Linux \$k' \\\$menuentry_id_option 'gnulinux-\$k-advanced-x' {"
		echo "		linux /vmlinuz-\$k root=x"
		echo "	}"
	done
	echo "}"
	[ -n "\${TW_BROKEN_GRUB:-}" ] && echo "broken {"
} > "\$out"
exit 0
EOF
cat > "$work/rbin/grub-script-check" <<'EOF'
#!/bin/bash
! grep -q '^broken' "$1"
EOF
chmod +x "$work/rbin"/grub-*
mkdir -p "$sb/boot"
check "info: GRUB without UKIs" bash -c "bash '$root_tool' info | grep -qx method=grub,grub-reboot"
old=$(sha256sum "$esp/grub/grub.cfg")
TW_BROKEN_GRUB=1 R install $k1 "$store" > "$work/out" 2>&1
check "a grub.cfg with faults is not taken" test "$(sha256sum "$esp/grub/grub.cfg")" = "$old"
check "  and the kernel is not left half on" test ! -e "$sb/usr/lib/modules/$k1" -a ! -e "$sb/boot/vmlinuz-$k1"
R install $k1 "$store" > "$work/out" 2>&1
check "install puts vmlinuz and initramfs into /boot" test -f "$sb/boot/vmlinuz-$k1" -a -f "$sb/boot/initramfs-$k1.img"
check "  and a new grub.cfg lists it" grep -q "vmlinuz-$k1" "$esp/grub/grub.cfg"
check "  the old one is kept" test -f "$esp/grub/grub.cfg.tilewin-backup"
R boot $k1 > "$work/out" 2>&1
check "booting once needs GRUB_DEFAULT=saved" grep -q "GRUB_DEFAULT=saved" "$work/out"
echo 'GRUB_DEFAULT="saved"' > "$sb/etc/default/grub"
: > "$calls"
R boot $k1 > "$work/out" 2>&1
check "  then grub-reboot gets its entry" grep -qx "grub-reboot gnulinux-advanced-x>gnulinux-$k1-advanced-x" "$calls"
R remove $k1 > "$work/out" 2>&1
check "remove takes it out of /boot and grub.cfg" bash -c "[ ! -e '$sb/boot/vmlinuz-$k1' ] && ! grep -q 'vmlinuz-$k1' '$esp/grub/grub.cfg'"

# ---------- systemd-boot ----------

loaderinfo "systemd-boot 258" > "$sb/sys/firmware/efi/efivars/LoaderInfo-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"
: > "$calls"
R boot $k1 "$store" > "$work/out" 2>&1
check "systemd-boot: a UKI and its one-shot entry" grep -qx "bootctl set-oneshot tilewin-$k1.efi" "$calls"
check "  no firmware entry" bash -c "! grep -q '=tileWin $k1$' '$nvram'"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
