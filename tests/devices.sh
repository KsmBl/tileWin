#!/bin/bash
# The Device manager on a computer of its own: a sysfs, a udev database and a
# kernel log below a folder, read by tilewin-settings --list-devices and
# changed by tilewin-devices-root.
#
#  - the devices in their categories, named from udev's database, each with
#    its driver; the hidden ones only when asked for
#  - problems: a PCI device without the driver it needs is an unknown device,
#    a USB device whose function has no driver too, missing firmware
#  - what is never disabled: the disk Linux runs from (through LUKS/LVM) and
#    the controllers it hangs on, the graphics of a connected screen
#  - disabling: a udev rule that keeps it off, the driver let go of now (a USB
#    device unauthorized); enabling takes both back
#
# usage: devices.sh <tilewin-settings> <tilewin-devices-root>
set -u

settings=$1
helper=$2
work=$(mktemp -d)
[ -n "${DEVICES_TEST_KEEP:-}" ] && echo "files in $work" || trap 'rm -rf "$work"' EXIT
failures=0
fail() { echo "FAIL: $1"; failures=$((failures + 1)); }
ok() { echo "ok   $1"; }
check() { local what=$1; shift; if "$@"; then ok "$what"; else fail "$what"; fi; }

R=$work/root
S=$R/sys
export TILEWIN_DEVICES_ROOT=$R TILEWIN_DEVICES_KLOG=$work/klog

# a device: its folder, uevent, subsystem link, the bus or class link to it
dev() { # path below /sys/devices, subsystem (bus/pci, class/net, ...)
	mkdir -p "$S/devices/$1"
	touch "$S/devices/$1/uevent"
	ln -sfn "$S/$2" "$S/devices/$1/subsystem"
	case $2 in
	bus/*) mkdir -p "$S/$2/devices"; ln -sfn "$S/devices/$1" "$S/$2/devices/$(basename "$1")" ;;
	class/*) mkdir -p "$S/$2"; ln -sfn "$S/devices/$1" "$S/$2/$(basename "$1")" ;;
	esac
}
attr() { mkdir -p "$(dirname "$S/devices/$1")"; printf '%s\n' "$2" > "$S/devices/$1"; }
driver() { # path, bus, driver, module
	mkdir -p "$S/bus/$2/drivers/$3"
	ln -sfn "$S/bus/$2/drivers/$3" "$S/devices/$1/driver"
	if [ -n "${4:-}" ]; then
		mkdir -p "$S/module/$4"
		ln -sfn "$S/module/$4" "$S/bus/$2/drivers/$3/module"
	fi
}
udevdb() { mkdir -p "$R/run/udev/data"; printf 'E:%s\n' "${@:2}" > "$R/run/udev/data/$1"; }

P=pci0000:00
# graphics with the screen of the laptop on it
dev $P/0000:00:02.0 bus/pci; attr $P/0000:00:02.0/class 0x030000; driver $P/0000:00:02.0 pci i915 i915
attr $P/0000:00:02.0/vendor 0x8086; attr $P/0000:00:02.0/device 0xa788
dev $P/0000:00:02.0/drm/card1 class/drm
dev $P/0000:00:02.0/drm/card1/card1-eDP-1 class/drm; attr $P/0000:00:02.0/drm/card1/card1-eDP-1/status connected
udevdb +pci:0000:00:02.0 'ID_MODEL_FROM_DATABASE=Raptor Lake-S UHD Graphics' 'ID_VENDOR_FROM_DATABASE=Intel Corporation'
# Wi-Fi without its firmware
dev $P/0000:00:14.3 bus/pci; attr $P/0000:00:14.3/class 0x028000; driver $P/0000:00:14.3 pci iwlwifi iwlwifi
dev $P/0000:00:14.3/net/wlan0 class/net; attr $P/0000:00:14.3/net/wlan0/ifindex 3
udevdb +pci:0000:00:14.3 'ID_MODEL_FROM_DATABASE=Wi-Fi 6E AX211'
# a bridge that needs no driver, a SATA controller that has none
dev $P/0000:00:1f.0 bus/pci; attr $P/0000:00:1f.0/class 0x060100
udevdb +pci:0000:00:1f.0 'ID_MODEL_FROM_DATABASE=LPC Controller'
dev $P/0000:00:17.0 bus/pci; attr $P/0000:00:17.0/class 0x010601
udevdb +pci:0000:00:17.0 'ID_MODEL_FROM_DATABASE=SATA Controller'
# the disk Linux runs from, through LUKS and LVM, behind a root port
dev $P/0000:00:1a.0 bus/pci; attr $P/0000:00:1a.0/class 0x060400; driver $P/0000:00:1a.0 pci pcieport
NV=$P/0000:00:1a.0/0000:02:00.0
dev $NV bus/pci; attr $NV/class 0x010802; driver $NV pci nvme nvme
udevdb +pci:0000:02:00.0 'ID_MODEL_FROM_DATABASE=NVMe Controller'
dev $NV/nvme/nvme0/nvme0n1 class/block; attr $NV/nvme/nvme0/nvme0n1/size 7814037168
attr $NV/nvme/nvme0/nvme0n1/dev 259:0; attr $NV/nvme/nvme0/model 'Fast Disk 4TB'
ln -sfn "$S/devices/$NV/nvme/nvme0" "$S/devices/$NV/nvme/nvme0/nvme0n1/device"
dev $NV/nvme/nvme0/nvme0n1/nvme0n1p2 class/block; attr $NV/nvme/nvme0/nvme0n1/nvme0n1p2/partition 2
dev virtual/block/dm-0 class/block; attr virtual/block/dm-0/dm/name cryptlvm; attr virtual/block/dm-0/size 100
mkdir -p "$S/devices/virtual/block/dm-0/slaves"; ln -sfn "$S/devices/$NV/nvme/nvme0/nvme0n1/nvme0n1p2" "$S/devices/virtual/block/dm-0/slaves/nvme0n1p2"
dev virtual/block/dm-1 class/block; attr virtual/block/dm-1/dm/name Vg-root; attr virtual/block/dm-1/size 100
mkdir -p "$S/devices/virtual/block/dm-1/slaves"; ln -sfn "$S/devices/virtual/block/dm-0" "$S/devices/virtual/block/dm-1/slaves/dm-0"
mkdir -p "$R/proc"; echo "/dev/mapper/Vg-root / ext4 rw 0 0" > "$R/proc/mounts"
# a card reader, of the "other" class
dev $P/0000:00:1c.0/0000:6d:00.0 bus/pci; attr $P/0000:00:1c.0/0000:6d:00.0/class 0xff0000
driver $P/0000:00:1c.0/0000:6d:00.0 pci rtsx_pci rtsx_pci
dev $P/0000:00:1c.0 bus/pci; attr $P/0000:00:1c.0/class 0x060400; driver $P/0000:00:1c.0 pci pcieport
udevdb +pci:0000:6d:00.0 'ID_MODEL_FROM_DATABASE=RTS5260 Card Reader'
# USB: the controller, its root hub, a fingerprint reader without a driver, a receiver
X=$P/0000:00:14.0
dev $X bus/pci; attr $X/class 0x0c0330; driver $X pci xhci_hcd xhci_pci
dev $X/usb1 bus/usb; attr $X/usb1/idVendor 1d6b; attr $X/usb1/speed 480; attr $X/usb1/authorized 1
attr $X/usb1/bDeviceClass 09; attr $X/usb1/dev 189:0
dev $X/usb1/1-0:1.0 bus/usb; driver $X/usb1/1-0:1.0 usb hub
dev $X/usb1/1-5 bus/usb; attr $X/usb1/1-5/idVendor 0a5c; attr $X/usb1/1-5/idProduct 5843
attr $X/usb1/1-5/product 58200; attr $X/usb1/1-5/authorized 1; attr $X/usb1/1-5/dev 189:4
udevdb c189:4 'ID_MODEL_FROM_DATABASE=BCM58200 ControlVault 3' 'ID_VENDOR_FROM_DATABASE=Broadcom Corp.'
dev $X/usb1/1-5/1-5:1.0 bus/usb
dev $X/usb1/1-6 bus/usb; attr $X/usb1/1-6/idVendor 046d; attr $X/usb1/1-6/product 'USB Receiver'
attr $X/usb1/1-6/manufacturer Logitech; attr $X/usb1/1-6/authorized 1; attr $X/usb1/1-6/dev 189:5
dev $X/usb1/1-6/1-6:1.0 bus/usb; driver $X/usb1/1-6/1-6:1.0 usb usbhid usbhid
H=$X/usb1/1-6/1-6:1.0/0003:046D:C52B.0001
dev $H bus/hid; driver $H hid hid-generic; printf 'HID_NAME=Logitech USB Receiver\n' > "$S/devices/$H/uevent"
dev $H/input/input5 class/input; attr $H/input/input5/name 'Logitech Mouse'
dev $H/input/input5/event5 class/input; attr $H/input/input5/event5/dev 13:69
udevdb c13:69 ID_INPUT=1 ID_INPUT_MOUSE=1
# the keyboard of the laptop, and the lid
dev platform/i8042/serio0 bus/serio; driver platform/i8042/serio0 serio atkbd
dev platform/i8042/serio0/input/input3 class/input; attr platform/i8042/serio0/input/input3/name 'AT keyboard'
dev platform/i8042/serio0/input/input3/event3 class/input; attr platform/i8042/serio0/input/input3/event3/dev 13:67
udevdb c13:67 ID_INPUT=1 ID_INPUT_KEY=1 ID_INPUT_KEYBOARD=1
dev LNXSYSTM:00/PNP0C0D:00 bus/acpi; driver LNXSYSTM:00/PNP0C0D:00 acpi button
dev LNXSYSTM:00/PNP0C0D:00/input/input0 class/input; attr LNXSYSTM:00/PNP0C0D:00/input/input0/name 'Lid Switch'
attr LNXSYSTM:00/PNP0C0D:00/input/input0/phys PNP0C0D/button/input0
# the loopback network, two processors, the machine
dev virtual/net/lo class/net; attr virtual/net/lo/ifindex 1
for c in 0 1; do mkdir -p "$S/devices/system/cpu/cpu$c"; done
printf 'processor\t: 0\nmodel name\t: Test CPU 9000\n\nprocessor\t: 1\nmodel name\t: Test CPU 9000\n' > "$R/proc/cpuinfo"
mkdir -p "$S/class/dmi/id"; echo 'Test Inc.' > "$S/class/dmi/id/sys_vendor"; echo 'Laptop 7' > "$S/class/dmi/id/product_name"
echo '1.2.3' > "$S/class/dmi/id/bios_version"
cat > "$work/klog" <<'EOF'
{"_KERNEL_DEVICE":"+pci:0000:00:14.3","MESSAGE":"iwlwifi 0000:00:14.3: Direct firmware load for iwlwifi-test-90.ucode failed with error -2","PRIORITY":"3","__REALTIME_TIMESTAMP":"1793033204383887"}
{"_KERNEL_DEVICE":"+pci:0000:00:02.0","MESSAGE":"i915 0000:00:02.0: [drm] GT0: GuC firmware i915/tgl_guc_70.bin version 70.49.4","PRIORITY":"6","__REALTIME_TIMESTAMP":"1793033204383887"}
EOF

list() { "$settings" --list-devices "$@"; }
row() { list --hidden | awk -F'\t' -v n="$1" '$2 == n' ; }
field() { row "$1" | cut -f"$2"; }

# ---------- what is found ----------

check "a GPU is a display adapter, named from udev's database" test "$(field 'Raptor Lake-S UHD Graphics' 1)" = display
check "  with its driver" test "$(field 'Raptor Lake-S UHD Graphics' 4)" = i915
check "Wi-Fi is a network adapter" test "$(field 'Wi-Fi 6E AX211' 1)" = network
check "  whose missing firmware is a problem" test "$(field 'Wi-Fi 6E AX211' 3)" = warning
field 'Wi-Fi 6E AX211' 6 > "$work/status"
check "  that names the file" grep -q iwlwifi-test-90.ucode "$work/status"
check "  and its interface is not a second adapter" test -z "$(row wlan0)"
check "the firmware a driver loaded is named" test "$(field 'Raptor Lake-S UHD Graphics' 7)" = i915/tgl_guc_70.bin
check "a bridge without a driver needs none" test "$(field 'LPC Controller' 1):$(field 'LPC Controller' 3)" = system:ok
check "a SATA controller without one is an unknown device" test "$(field 'SATA Controller' 1):$(field 'SATA Controller' 3)" = other:warning
check "the disk, by its model and size" bash -c "'$settings' --list-devices | grep -q '^disks.Fast Disk 4TB (4.0.TB)'"
check "a card reader is a memory technology device" test "$(field 'RTS5260 Card Reader' 1)" = mtd
check "the root hub is a USB controller, with the hub driver" test "$(field 'USB Root Hub (USB 2.0)' 1):$(field 'USB Root Hub (USB 2.0)' 4)" = usbctl:hub
check "a USB device whose function has no driver is an unknown device" \
	test "$(field 'BCM58200 ControlVault 3' 1):$(field 'BCM58200 ControlVault 3' 3)" = other:warning
check "  named from the database when its own name says little (58200)" test -n "$(row 'BCM58200 ControlVault 3')"
check "a USB receiver is a USB device, named as it names itself" test "$(field 'USB Receiver' 1):$(field 'USB Receiver' 4)" = usb:usbhid
check "its mouse is a pointing device" test "$(field 'Logitech Mouse' 1)" = mice
check "  that is disabled with its USB device, not the controller" test "$(field 'Logitech Mouse' 5)" = devices/$X/usb1/1-6
check "the laptop's keyboard is a keyboard, disabled on its serio port" \
	test "$(field 'AT keyboard' 1):$(field 'AT keyboard' 5)" = keyboards:devices/platform/i8042/serio0
check "the lid is a system device, that cannot be disabled" test "$(field 'Lid Switch' 1):$(field 'Lid Switch' 5)" = system:-
check "the screen is a monitor" test "$(field 'Built-in display' 1)" = monitors
check "every processor" test "$(list | grep -c $'^processors\tTest CPU 9000')" = 2
check "the computer and its firmware" test -n "$(row 'Test Inc. Laptop 7')" -a -n "$(row 'System Firmware 1.2.3')"
check "the loopback network is hidden" bash -c "! '$settings' --list-devices | grep -q '^network.lo'"
check "  but there with the hidden devices" bash -c "'$settings' --list-devices --hidden | grep -q '^network.lo'"

# ---------- what may be disabled ----------

mkdir -p "$work/bin"
printf '#!/bin/sh\necho "udevadm $*" >> %s/udevadm.calls\n' "$work" > "$work/bin/udevadm"
chmod +x "$work/bin/udevadm"
export PATH="$work/bin:$PATH"
Hc() { bash "$helper" "$@" 2>&1; }

check "the disk Linux runs from (through LUKS and LVM) stays" bash -c "bash '$helper' check pci 0000:02:00.0 | grep -q 'runs from a disk'"
check "  and the root port it hangs on" bash -c "bash '$helper' check pci 0000:00:1a.0 | grep -q 'runs from a disk'"
check "the graphics of a connected screen stays" bash -c "bash '$helper' check pci 0000:00:02.0 | grep -q 'screen in use'"
check "a card reader may go" test "$(Hc check pci 0000:6d:00.0)" = ok
check "a root hub is not disabled on its own" bash -c "bash '$helper' check usb usb1 | grep -q 'root hub'"
check "a name with a path in it is refused" bash -c "bash '$helper' check pci ../../x | grep -q 'not one'"
Hc disable pci 0000:02:00.0 > "$work/out"
check "disabling the disk is refused" bash -c "grep -q 'runs from a disk' '$work/out' && [ ! -e '$R/etc/udev/rules.d/90-tilewin-disabled.rules' ]"

rules=$R/etc/udev/rules.d/90-tilewin-disabled.rules
Hc disable pci 0000:6d:00.0 > "$work/out"
check "disabling takes the device from its driver now" grep -qx 0000:6d:00.0 "$S/bus/pci/drivers/rtsx_pci/unbind"
check "  and a rule keeps it off after a restart" grep -q 'ACTION=="bind", SUBSYSTEM=="pci", KERNEL=="0000:6d:00.0"' "$rules"
check "  which udev reads at once" grep -q 'udevadm control --reload' "$work/udevadm.calls"
check "  and the list says it is disabled" test "$(field 'RTS5260 Card Reader' 3)" = disabled
Hc disable usb 1-6 > "$work/out"
check "a USB device is disabled with its authorized switch" grep -qx 0 "$S/devices/$X/usb1/1-6/authorized"
check "  and a rule of its own" grep -q 'KERNEL=="1-6", ATTR{authorized}="0"' "$rules"
check "  and both are kept" test "$(grep -c '^# tilewin-disabled' "$rules")" = 2
rm "$S/devices/$P/0000:00:1c.0/0000:6d:00.0/driver" # as the kernel does on unbind
Hc enable pci 0000:6d:00.0 > "$work/out"
check "enabling lets the kernel find its driver again" grep -qx 0000:6d:00.0 "$S/bus/pci/drivers_probe"
check "  and takes only its rule away" bash -c "! grep -q 6d:00.0 '$rules' && grep -q 'KERNEL==\"1-6\"' '$rules'"
Hc enable usb 1-6 > "$work/out"
check "a USB device is enabled again" grep -qx 1 "$S/devices/$X/usb1/1-6/authorized"
check "  and without rules left the file goes" test ! -e "$rules"
Hc restart usb 1-6 > "$work/out"
check "restarting a USB device plugs it in anew" bash -c "grep -qx 1 '$S/devices/$X/usb1/1-6/authorized' && grep -q restarted '$work/out'"

[ "$failures" -eq 0 ] && echo "all passed" || { echo "$failures failed"; exit 1; }
