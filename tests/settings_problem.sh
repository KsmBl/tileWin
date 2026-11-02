#!/bin/sh
# When something fails the settings say so in a banner over the page, with the
# next step. On the DNS page, with a helper that refuses the first time:
#  - with the About page on view, the Network page fails in the background with
#    NetworkManager gone, and that is not said over the About page;
#  - turning the service off fails and the banner shows;
#  - Try again runs the same thing once more, and the banner goes;
#  - after another failure × closes it without running anything.
#
# usage: settings_problem.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
settings=$build/settings/tilewin-settings
pointer=$build/tests/pointer-tool

for needed in "$compositor" "$msg" "$settings" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-daemon grim python3; do
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
sock=$XDG_RUNTIME_DIR/tw-problem-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_DNS_CONF=$work/dns.conf TILEWIN_DNS_APPLY=$work/helper TILEWIN_DNS_RUN=$work/run
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$work/run"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"
echo win95 > "$XDG_CONFIG_HOME/tileWin/current-theme"
printf 'servers 1.1.1.1 8.8.8.8\nfastest 2\ntest_interval 3600\n' > "$work/dns.conf"

cat > "$work/helper" <<EOS
#!/bin/sh
echo "\$*" >> "$work/calls"
[ -e "$work/refuse" ] || exit 0
rm -f "$work/refuse"
echo boom >&2
exit 1
EOS
chmod +x "$work/helper"
touch "$work/refuse"
# NetworkManager answers as ever, but cannot tell about the network
mkdir "$work/bin"
real_nmcli=$(command -v nmcli || echo true)
cat > "$work/bin/nmcli" <<EOS
#!/bin/sh
case "\$*" in
*"connection show uuid"*)
	if [ ! -e "$work/nm-back" ]; then
		echo "NetworkManager is not running" >&2
		exit 1
	fi
	;;
esac
exec "$real_nmcli" "\$@"
EOS
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
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
panel_command none
animations off
output HEADLESS-1 mode --custom 1280x800 pos 0 0
exec $settings --page=about
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
attempt=0
while ! ipc -t get_tree | grep -q '"name": "tileWin Settings"' && [ $attempt -lt 50 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
sleep 4
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 1.5

# is the banner up? its red, where nothing else on the page is red
banner_up() {
	grim -g "600,20 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | python3 -c '
import sys
r, g, b = (int(x) for x in sys.stdin.read().split())
sys.exit(0 if r > 170 and g < 80 and b < 60 else 1)'
}
calls() { grep -c "" "$work/calls" 2>/dev/null || echo 0; }
: > "$work/calls"

banner_up && fail "what failed on a page in the background shows over another"

# now the DNS page, with NetworkManager back
touch "$work/nm-back"
ipc '[title="tileWin Settings"] kill' >/dev/null
sleep 0.5
"$settings" --page=dns >/dev/null 2>&1 &
sleep 4
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 1.5
banner_up && fail "a banner shows before anything failed"
# the window is new: the pointer has to come in before a click counts
"$pointer" 1280 800 move 640 400
sleep 1
"$pointer" 1280 800 click 1211 175
sleep 1
banner_up || fail "a refused helper shows no banner"
[ "$(grep -c disable "$work/calls")" = 1 ] || fail "the helper was not asked once: $(cat "$work/calls")"

"$pointer" 1280 800 click 1190 24
sleep 1
banner_up && fail "the banner stayed after it worked"
[ "$(grep -c disable "$work/calls")" = 2 ] || fail "Try again did not ask the helper again: $(cat "$work/calls")"

touch "$work/refuse"
: > "$work/calls"
# with the banner gone and the service off the page is back where it was
"$pointer" 1280 800 click 1211 175
sleep 1
banner_up || fail "the second refusal shows no banner"
before=$(calls)
"$pointer" 1280 800 click 1251 24
sleep 0.8
banner_up && fail "× did not close the banner"
[ "$(calls)" = "$before" ] || fail "× ran something: $(cat "$work/calls")"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
