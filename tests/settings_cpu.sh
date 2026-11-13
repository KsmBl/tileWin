#!/bin/sh
# The Processor page and tilewin-cpu-root on a processor of the test's own (a
# folder like /sys/devices/system/cpu with two threads of a performance core
# and one efficient core, TILEWIN_CPU_ROOT):
#  - the helper limits CPUs, keeps the limits in etc/tilewin/cpu-limits and sets
#    them again with "apply", takes a limit at the highest clock away, refuses
#    a clock the CPU cannot run at and what is not a CPU, and "reset" sets
#    every core to full speed and keeps nothing;
#  - on the page, a click on the slider of Core 1 limits both its threads and
#    not the efficient core, the far left of "All cores" limits every core to
#    its lowest, and Full speed takes it all back.
#
# usage: settings_cpu.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
pointer=$build/tests/pointer-tool
helper=$build/cpu/tilewin-cpu-root

for needed in "$compositor" "$msg" "$settings" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if [ ! -f "$helper" ]; then # run with bash: it need not be executable in the build
	echo "$helper was not built: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
cpu_root=$work/root
export TILEWIN_CPU_ROOT=$cpu_root TILEWIN_CPU_ROOT_HELPER=$helper

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

# <cpu> <core id> <highest kHz>
cpu() {
	d=$cpu_root/sys/devices/system/cpu/cpu$1
	mkdir -p "$d/cpufreq" "$d/topology"
	echo "$2" > "$d/topology/core_id"
	echo 0 > "$d/topology/physical_package_id"
	echo 800000 > "$d/cpufreq/cpuinfo_min_freq"
	echo "$3" > "$d/cpufreq/cpuinfo_max_freq"
	echo "$3" > "$d/cpufreq/scaling_max_freq"
	echo 800000 > "$d/cpufreq/scaling_min_freq"
	echo 1200000 > "$d/cpufreq/scaling_cur_freq"
}
cpu 0 0 5100000
cpu 1 0 5100000
cpu 2 8 3800000
mkdir -p "$cpu_root/sys/devices/cpu_core" "$cpu_root/sys/devices/cpu_atom"
echo 0-1 > "$cpu_root/sys/devices/cpu_core/cpus"
echo 2 > "$cpu_root/sys/devices/cpu_atom/cpus"
limits() {
	for n in 0 1 2; do
		cat "$cpu_root/sys/devices/system/cpu/cpu$n/cpufreq/scaling_max_freq"
	done | tr '\n' ' ' | sed 's/ $//'
}
kept=$cpu_root/etc/tilewin/cpu-limits

# ---- the helper
bash "$helper" set 0=2000000 1=2000000 >/dev/null || fail "the helper refused 2 GHz"
[ "$(limits)" = "2000000 2000000 3800000" ] || fail "set 2 GHz on CPUs 0 and 1: $(limits)"
[ "$(cat "$kept" 2>/dev/null | tr '\n' ' ')" = "cpu0 2000000 cpu1 2000000 " ] ||
	fail "the limits were not kept: $(cat "$kept" 2>/dev/null)"
bash "$helper" set 2=9000000 >/dev/null 2>&1 && fail "the helper took 9 GHz for a 3.8 GHz core"
bash "$helper" set "0=1000000;true" >/dev/null 2>&1 && fail "the helper took what is not a clock"
bash "$helper" set 7=1000000 >/dev/null 2>&1 && fail "the helper took a CPU that is not there"
bash "$helper" set 0=5100000 >/dev/null
grep -q '^cpu0 ' "$kept" && fail "a limit at the highest clock was kept"
echo 4000000 > "$cpu_root/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq"
bash "$helper" apply
[ "$(limits)" = "5100000 2000000 3800000" ] || fail "apply did not set the kept limit again: $(limits)"
bash "$helper" reset >/dev/null
[ "$(limits)" = "5100000 5100000 3800000" ] || fail "reset left $(limits)"
[ -e "$kept" ] && fail "reset kept limits"

# ---- the page
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export GIO_USE_VFS=local
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command true
output * mode --custom 1280x900
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
exec "$settings" --page=cpu
EOF
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
wait_until 20 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.Settings\"'" ||
	fail "the settings never came up"
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
wait_still 15 screen
click() { "$pointer" 1280 900 click "$1" "$2" >/dev/null 2>&1; }

# Core 1's slider, a third of the way: both its threads, not the E-core
click 920 312
wait_until 5 sh -c "[ \"\$(cat '$cpu_root/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq')\" != 5100000 ]" ||
	fail "the slider of Core 1 set no limit: $(limits)"
set -- $(limits)
[ "$1" = "$2" ] || fail "the threads of Core 1 got different limits: $*"
[ "$3" = 3800000 ] || fail "the efficient core was limited along with Core 1: $*"
grep -q '^cpu0 ' "$kept" 2>/dev/null || fail "the page's limit was not kept for the next start"

# All cores, at the far left: every core at its lowest
wait_still 5 screen
click 868 177
wait_until 5 sh -c "[ \"\$(cat '$cpu_root/sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq')\" = 800000 ]" ||
	fail "All cores at the left left $(limits)"
[ "$(limits)" = "800000 800000 800000" ] || fail "All cores at the left: $(limits)"

# Full speed
wait_still 5 screen
click 1179 177
wait_until 5 sh -c "[ \"\$(cat '$cpu_root/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq')\" = 5100000 ]" ||
	fail "Full speed left $(limits)"
[ "$(limits)" = "5100000 5100000 3800000" ] || fail "Full speed: $(limits)"
[ -e "$kept" ] && fail "Full speed kept limits for the next start"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
