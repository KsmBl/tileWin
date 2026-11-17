#!/bin/bash
# Quick web (panel/quickweb.c): a key types its word from quickweb.conf, the
# next key one of the words after it, a key without a word and a Shifted key
# type themselves, Backspace takes a word back, and Enter opens the address,
# https:// before it, in the browser (here a stand-in that writes it down).
#
# usage: quick_web.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
keyboard=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$keyboard"; do
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications"

cat > "$XDG_CONFIG_HOME/tileWin/quickweb.conf" <<'EOT'
key g "github.com/" {
	key k "ksmbl/" {
		key t "tileWin"
	}
}
key y "youtube.com"
EOT
mkdir -p "$work/bin"
printf '#!/bin/sh\necho "$@" >> %s/opened\n' "$work" > "$work/bin/xdg-open"
chmod +x "$work/bin/xdg-open"
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
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

cat > "$work/tilewin.conf" <<EOT
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOT
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
sleep 2

opened() { [ -s "$work/opened" ] && tail -n 1 "$work/opened"; }
type_and_open() {
	: > "$work/opened"
	ipc panel quickweb >/dev/null 2>&1
	sleep 1
	"$keyboard" "$@" key none return >/dev/null 2>&1
	wait_until 5 test -s "$work/opened"
}

# a word a key, the next one of those after it
type_and_open text gkt
[ "$(opened)" = "https://github.com/ksmbl/tileWin" ] || fail "g k t did not open github.com/ksmbl/tileWin: $(opened)"

# Backspace takes a whole word back, and its key can be pressed again
type_and_open text gk key none backspace text k
[ "$(opened)" = "https://github.com/ksmbl/" ] || fail "Backspace did not take the word back: $(opened)"

# a key without a word types itself, and from there on all are letters
type_and_open text yxg
[ "$(opened)" = "https://youtube.comxg" ] || fail "keys after a letter of their own were taken as words: $(opened)"

# Shift types the letter itself
type_and_open key shift g text it
[ "$(opened)" = "https://Git" ] || fail "Shift did not type the letter itself: $(opened)"

# Escape closes without opening
: > "$work/opened"
ipc panel quickweb >/dev/null 2>&1
sleep 1
"$keyboard" text g key none escape >/dev/null 2>&1
sleep 1
[ ! -s "$work/opened" ] || fail "Escape opened something: $(opened)"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
