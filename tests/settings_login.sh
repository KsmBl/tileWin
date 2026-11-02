#!/bin/sh
# The Login screen page of the settings, with a stand-in for tilewin-dm-apply:
#  - the picture on top follows a change at once, and nothing is saved by that;
#  - the Save bar is only there with something to save, and Discard goes back;
#  - Save sends what changed through the helper, and what the file said stays;
#  - the switch of the display manager asks first, and does nothing by itself;
#  - a helper that refuses shows in the banner.
#
# usage: settings_login.sh <build dir> <source dir>
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
sock=$XDG_RUNTIME_DIR/tw-login-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export TILEWIN_DM_APPLY=$work/helper TILEWIN_DM_CONF=$work/display-manager.conf
export TILEWIN_DM_STATE=$work/no-state
export PATH=$build/dm:$PATH
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
echo light > "$XDG_CONFIG_HOME/tileWin/color-scheme"
echo win10 > "$XDG_CONFIG_HOME/tileWin/current-theme"

printf 'theme win7\nenv FOO=bar\nvt 4\nclock none\n' > "$work/display-manager.conf"
cat > "$work/helper" <<EOS
#!/bin/sh
echo "\$1" >> "$work/calls"
case \$1 in
status) echo disabled ;;
write)
	cat > "$work/written"
	if [ -e "$work/refuse" ]; then
		echo "line 1: boom" >&2
		exit 1
	fi
	;;
esac
EOS
chmod +x "$work/helper"

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
exec $settings --page=login
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
sleep 2
ipc '[title="tileWin Settings"] fullscreen enable' >/dev/null
sleep 2
"$pointer" 1280 800 move 640 400
sleep 0.5

grep -q '^status$' "$work/calls" 2>/dev/null || fail "the page did not ask whether it is in use"

# the picture on top: how much of it differs from a shot of it before
thumb() { grim -g "261,20 304x171" "$1"; }
differs() { ! cmp -s "$1" "$2"; }
# the Save bar: its blue button, at the bottom when there is something to save
bar_up() {
	grim -g "1186,743 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | python3 -c '
import sys
r, g, b = (int(x) for x in sys.stdin.read().split())
sys.exit(0 if b > 150 and r < 80 else 1)'
}
banner_up() {
	grim -g "600,20 1x1" -t ppm - 2>/dev/null | tail -c 3 | od -An -tu1 | python3 -c '
import sys
r, g, b = (int(x) for x in sys.stdin.read().split())
sys.exit(0 if r > 170 and g < 80 and b < 60 else 1)'
}

bar_up && fail "the Save bar shows with nothing changed"
thumb "$work/before.png"

# the card to the right, and dark: the picture follows at once, nothing is saved yet
"$pointer" 1280 800 click 1197 690
sleep 0.5
"$pointer" 1280 800 click 1199 360
sleep 1
thumb "$work/after.png"
differs "$work/before.png" "$work/after.png" || fail "the picture does not follow the settings"
bar_up || fail "no Save bar after a change"
if grep -q '^write$' "$work/calls"; then
	fail "something was saved before Save"
fi

# Discard: back as it was, the bar goes
"$pointer" 1280 800 click 1128 743
sleep 1
bar_up && fail "the Save bar stayed after Discard"
thumb "$work/discarded.png"
differs "$work/before.png" "$work/discarded.png" && fail "Discard did not bring the picture back"

# Save: what changed, what the file said, and no defaults
"$pointer" 1280 800 click 1197 690
sleep 0.8
"$pointer" 1280 800 click 1213 743
sleep 1.5
grep -q '^write$' "$work/calls" || fail "Save did not reach the helper: $(cat "$work/calls")"
grep -q '^card_position right$' "$work/written" 2>/dev/null || fail "the change was not saved: $(cat "$work/written" 2>/dev/null)"
grep -q '^theme "win7"$' "$work/written" 2>/dev/null || fail "the theme of the file got lost"
grep -q '^vt 4$' "$work/written" 2>/dev/null || fail "a number of the file got lost"
grep -q '^env FOO=bar$' "$work/written" 2>/dev/null || fail "an env line of the file got lost"
if grep -q 'remember_user' "$work/written" 2>/dev/null; then
	fail "a default was written out"
fi
bar_up && fail "the Save bar stayed after saving"

# a helper that refuses: the banner, and the change is still there to save
touch "$work/refuse"
"$pointer" 1280 800 click 1133 690
sleep 0.8
"$pointer" 1280 800 click 1213 743
sleep 1.5
banner_up || fail "a refusal shows no banner"
bar_up || fail "after a refusal the change was taken for saved"
rm -f "$work/refuse"
"$pointer" 1280 800 click 1251 24
sleep 0.6

# the switch: asks, so nothing has gone to the helper after the click
"$pointer" 1280 800 click 609 101
sleep 1.5
if grep -q '^enable$' "$work/calls"; then
	fail "the switch turned it on without asking"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
