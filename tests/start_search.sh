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
. "$(dirname "$0")/wait.sh"

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
for needed in dbus-run-session find timeout grim md5sum; do
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
# the search for files runs find: it writes down each search it finished, so
# the test knows when the files found are in the menu
cat > "$work/bin/find" <<EOS
#!/bin/sh
$(command -v find) "\$@"
status=\$?
printf '%s\n' "\$*" >> "$work/found"
exit \$status
EOS
chmod +x "$work/bin/find"
export PATH=$work/bin:$PATH

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
cleanup() {
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
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

# a checksum of what the screen shows, and of a strip of it across
screen() { grim -t ppm - 2>/dev/null | md5sum; }
strip() { grim -g "0,$1 1280x40" -t ppm - 2>/dev/null | md5sum; }
# the taskbar is drawn, at the top or the bottom: there is more than wallpaper
panel_up() {
	_middle=$(strip 340)
	[ "$(strip 0)" != "$_middle" ] || [ "$(strip 680)" != "$_middle" ]
}
changed_from() { [ "$(screen)" != "$1" ]; }
# runs an ipc command that shows something, and waits until it shows: a menu
# that is on the screen has the keyboard (the keyboard tool takes a moment to
# start typing, and lets the last key take effect before it ends)
shown() {
	_before=$(screen)
	ipc "$@" >/dev/null
	wait_until 10 changed_from "$_before"
}
# runs an ipc command that redraws the screen, then waits until it has been
# redrawn: the screen changed, the taskbar is there, and nothing moves anymore
redrawn() {
	shown "$@"
	wait_until 10 panel_up
	wait_still 10 screen
}
# the start menu opens, ready for the keyboard
open_menu() { shown panel startmenu toggle; }
if ! wait_until 30 panel_up; then
	echo "the taskbar never came up"
	tail -n 3 "$work/log"
	exit 1
fi
wait_still 10 screen
# a search for files has been finished and shown
found() { grep -qF -- "*$1*" "$work/found" 2>/dev/null; }

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
	if ipc -t get_tilewin | grep -q "\"theme\": *\"$theme\""; then
		# already in it: the taskbar loads it again, which shows nothing
		# that could be waited for
		ipc theme $theme >/dev/null
		sleep 1
	else
		redrawn theme $theme
	fi

	open_menu
	"$keys" text dark key none space text mode key none return
	got=$(called tilewin-settings)
	[ "$got" = "--page=theme" ] || fail "$theme: dark mode opened '$got', not the Theme page"

	open_menu
	"$keys" text 6 key shift 8 text 7 key none return
	got=$(called wl-copy | tail -n 1)
	[ "$got" = "42" ] || fail "$theme: 6*7 copied '$got', not 42"

	open_menu
	rm -f "$work/found"
	"$keys" text zebra
	wait_until 10 found zebra
	wait_still 10 screen
	"$keys" key none return
	got=$(called xdg-open)
	[ "$got" = "$tricky" ] || fail "$theme: zebra opened '$got', not '$tricky'"
done

# hidden folders stay out: the only other zebra is not found
rm -f "$tricky" "$work/found"
open_menu
"$keys" text hiddenzebra
wait_until 10 found hiddenzebra || fail "the files were not searched for hiddenzebra"
wait_still 10 screen
"$keys" key none return
sleep 0.8
[ ! -e "$work/xdg-open.called" ] || fail "a file in a hidden folder was found: $(cat "$work/xdg-open.called")"
"$keys" key none escape key none escape
wait_still 10 screen

# the launcher: a command comes before a setting of that name
shown panel launcher
"$keys" text dig key none return
wait_until 5 test -e "$work/dig.called"
[ -e "$work/dig.called" ] || fail "Enter in the launcher did not run the command dig"
# what Enter runs, it runs at once: had it opened a setting as well, that
# would have come with dig
sleep 0.5
[ ! -e "$work/tilewin-settings.called" ] ||
	fail "the launcher opened a setting for the command dig: $(called tilewin-settings)"
shown panel launcher
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
