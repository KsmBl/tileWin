#!/bin/sh
# The search of the start menu and of the launcher finds more than apps, in a
# nested tileWin with the taskbar, with stand-ins that write down how they were
# called:
#  - "dark mode" opens the Theme page of the settings;
#  - a sum shows its value, which Enter copies;
#  - a file in a folder of the home folder is found by a part of its name and
#    opened, with spaces, quotes and other marks in its name; one in a hidden
#    folder is not found;
#  - in the launcher a word that is no app still runs as a command with Enter,
#    even though settings have that word (dig: Digital clocks on the Desktop
#    page, the Diggers screen saver).
#
# usage: start_search.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$keys"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in dbus-run-session find timeout; do
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
sock=$XDG_RUNTIME_DIR/tw-search-$$.sock
export HOME=$work/home
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$HOME/Documents/old" "$HOME/.secret" "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" \
	"$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$work/bin"
cp "$source_dir/config/taskbar.conf" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
tricky="$HOME/Documents/old/zebra it's; 100% \$mod.txt"
: > "$tricky"
: > "$HOME/.secret/hiddenzebra.txt"

# stand-ins: each writes its arguments, one to a line
for name in xdg-open tilewin-settings wl-copy dig; do
	cat > "$work/bin/$name" <<EOS
#!/bin/sh
for a in "\$@"; do printf '%s\n' "\$a"; done > "$work/$name.called"
EOS
	chmod +x "$work/bin/$name"
done
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
remember_windows no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
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
sleep 1.5

# waits for a stand-in to have been called, and says what with
called() {
	attempt=0
	while [ ! -e "$work/$1.called" ] && [ $attempt -lt 25 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	cat "$work/$1.called" 2>/dev/null
	rm -f "$work/$1.called"
}

for theme in win10 win7 win11; do
	ipc theme $theme >/dev/null
	sleep 1

	ipc panel startmenu toggle >/dev/null
	sleep 0.5
	"$keys" text dark key none space text mode key none return
	got=$(called tilewin-settings)
	[ "$got" = "--page=theme" ] || fail "$theme: dark mode opened '$got', not the Theme page"
	sleep 0.5

	ipc panel startmenu toggle >/dev/null
	sleep 0.5
	"$keys" text 6 key shift 8 text 7 key none return
	got=$(called wl-copy | tail -n 1)
	[ "$got" = "42" ] || fail "$theme: 6*7 copied '$got', not 42"
	sleep 0.5

	ipc panel startmenu toggle >/dev/null
	sleep 0.5
	"$keys" text zebra
	sleep 1.2
	"$keys" key none return
	got=$(called xdg-open)
	[ "$got" = "$tricky" ] || fail "$theme: zebra opened '$got', not '$tricky'"
	sleep 0.5
done

# hidden folders stay out: the only other zebra is not found
rm -f "$tricky"
ipc panel startmenu toggle >/dev/null
sleep 0.5
"$keys" text hiddenzebra
sleep 1.2
"$keys" key none return
sleep 0.8
[ ! -e "$work/xdg-open.called" ] || fail "a file in a hidden folder was found: $(cat "$work/xdg-open.called")"
"$keys" key none escape key none escape
sleep 0.5

# the launcher: a command comes before a setting of that name
ipc panel launcher >/dev/null
sleep 0.5
"$keys" text dig key none return
sleep 0.2
attempt=0
while [ ! -e "$work/dig.called" ] && [ $attempt -lt 25 ]; do
	sleep 0.2
	attempt=$((attempt + 1))
done
[ -e "$work/dig.called" ] || fail "Enter in the launcher did not run the command dig"
[ -z "$(called tilewin-settings)" ] || fail "the launcher opened a setting for the command dig"
sleep 0.5
ipc panel launcher >/dev/null
sleep 0.5
"$keys" text dig key none down key none return
got=$(called tilewin-settings)
echo "the row below the command: $got"
case $got in
--page=*) ;;
*) fail "the row below the command opened '$got', not a settings page" ;;
esac

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
