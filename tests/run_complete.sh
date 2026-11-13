#!/bin/sh
# Autocompletion in the Run dialog (Win+R), in a nested tileWin with programs,
# a history and folders of the test's own:
#  - "twtesta" and Tab takes the program twtestalpha of $PATH, Enter runs it;
#  - "twtestb" and Tab takes the earlier entry "twtestbeta --flag" of the
#    history, arguments and all, before the program of the same name;
#  - a path completes from its folder: Tab takes the one folder that fits and
#    lists what is in it, Tab takes the file, and Enter opens it;
#  - Escape closes the list first and the dialog only with the next.
#
# usage: run_complete.sh <build dir> <source dir>
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
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME/tileWin" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME" "$work/bin" "$work/files/Documents" "$work/files/Music"
touch "$work/files/Documents/notes.txt"

# programs that only say they ran, and an xdg-open that says what it opened
for name in twtestalpha twtestbeta; do
	printf '#!/bin/sh\necho "$*" > %s/%s.ran\n' "$work" "$name" > "$work/bin/$name"
	chmod +x "$work/bin/$name"
done
printf '#!/bin/sh\necho "$*" > %s/opened\n' "$work" > "$work/bin/xdg-open"
chmod +x "$work/bin/xdg-open"
export PATH=$work/bin:$PATH
printf 'twtestbeta --flag\nfirefox\n' > "$XDG_STATE_HOME/tileWin/run-history"

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

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
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
screen() { grim "$work/shot.png" 2>/dev/null && md5sum < "$work/shot.png"; }
wait_still 10 screen
key() { "$keys" key "$@" >/dev/null 2>&1; }
type_text() { "$keys" text "$1" >/dev/null 2>&1; }
open_run() { # [text]
	ipc panel run "$@" >/dev/null
	wait_still 5 screen
}
first_history() { head -n 1 "$XDG_STATE_HOME/tileWin/run-history"; }

# ---- a program of $PATH
open_run
key ctrl a # the last command is in the field, selected: typing replaces it
type_text twtesta
wait_still 5 screen
key none tab
key none return
wait_for_file 5 "$work/twtestalpha.ran" || fail "Tab and Enter after twtesta did not run twtestalpha"
[ "$(first_history)" = twtestalpha ] || fail "the history did not get twtestalpha: $(first_history)"

# ---- the history first, with its arguments
open_run
type_text twtestb
wait_still 5 screen
key none tab
key none return
wait_for_file 5 "$work/twtestbeta.ran" || fail "Tab and Enter after twtestb ran nothing"
[ "$(cat "$work/twtestbeta.ran" 2>/dev/null)" = --flag ] ||
	fail "the earlier entry was not taken with its arguments: '$(cat "$work/twtestbeta.ran")'"

# ---- a path: the one folder that fits, then the file in it
open_run "$work/files/Doc"
key none tab # Documents/, the only one: taken at once, its files listed
wait_still 5 screen
key none tab # notes.txt
key none return
wait_for_file 5 "$work/opened" || fail "the completed path was not opened"
[ "$(cat "$work/opened" 2>/dev/null)" = "$work/files/Documents/notes.txt" ] ||
	fail "the completed path is '$(cat "$work/opened" 2>/dev/null)'"

# ---- Escape: the list first, then the dialog
open_run
type_text twtest
wait_still 5 screen
key none escape
wait_still 5 screen
listless=$(screen)
key none escape
wait_still 5 screen
[ "$(screen)" != "$listless" ] || fail "the second Escape did not close the Run dialog"
rm -f "$work"/*.ran
key none return
sleep 0.5 # a negative: with the dialog closed Enter runs nothing
ls "$work"/*.ran >/dev/null 2>&1 && fail "Enter ran something after the dialog was closed"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
