#!/bin/sh
# Properties and Open with of desktop icons. The Properties dialog opens from
# the icon menu's command and with Alt+Enter on a selected icon, for a link to
# a big folder (counted on a thread, also when the dialog is closed while it
# counts), a file, a link to a phone and a program shortcut, and closes with
# Escape and Enter. "Open with" starts the program it names with the file, or
# with the folder a link points to; a program shortcut opens with nothing.
# The programs are stand-ins that write down what they were given.
#
# usage: desktop_properties.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$msg" "$panel" "$pointer" "$keys"; do
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
sock=$XDG_RUNTIME_DIR/tw-props-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export LANG=C LC_ALL=C
desk=$work/Desktop
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications" "$desk" "$work/bin" "$work/Music"
printf 'XDG_DESKTOP_DIR="%s"\n' "$desk" > "$XDG_CONFIG_HOME/user-dirs.dirs"

for tool in fakefiles fakeedit; do
	cat > "$work/bin/$tool" <<EOS
#!/bin/sh
printf '%s %s\n' $tool "\$*" >> $work/opened
EOS
	chmod +x "$work/bin/$tool"
	cat > "$XDG_DATA_HOME/applications/$tool.desktop" <<EOS
[Desktop Entry]
Type=Application
Name=$tool
Exec=$work/bin/$tool %U
MimeType=$([ $tool = fakefiles ] && echo inode/directory || echo text/plain);
EOS
done
cat > "$XDG_CONFIG_HOME/mimeapps.list" <<EOS
[Default Applications]
inode/directory=fakefiles.desktop
text/plain=fakeedit.desktop
EOS

# a folder big enough to be still counting when the dialog closes again
for d in 1 2 3 4 5 6 7 8 9 10; do
	mkdir -p "$work/Music/album$d"
	(cd "$work/Music/album$d" && seq 1 400 | xargs touch)
done
# by name: 0 Music, 1 notes.txt, 2 Phone, 3 Text Editor
printf '[Desktop Entry]\nType=Link\nName=Music\nURL=file://%s/Music\nIcon=folder-music\n' \
	"$work" > "$desk/Music.desktop"
echo hello > "$desk/notes.txt"
printf '[Desktop Entry]\nType=Link\nName=Phone\nURL=mtp://Pixel_8/\n' > "$desk/Phone.desktop"
printf '[Desktop Entry]\nType=Application\nName=Text Editor\nExec=%s/bin/fakeedit %%U\n' \
	"$work" > "$desk/Editor.desktop"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
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
ipc() { "$msg" -s "$sock" "$@"; }
ipc mode window >/dev/null 2>&1
sleep 3

# the dialog sits in the middle of the screen, on the solid wallpaper
middle() {
	grim -g "560,250 160x100" -t ppm - 2>/dev/null | python3 -c '
import sys
data = sys.stdin.buffer.read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
px = data[pos + 1:]
print(sum(px) // max(1, len(px)))'
}
plain=$(middle)
shown() { now=$(middle); [ $((now - plain)) -gt 30 ] || [ $((plain - now)) -gt 30 ]; }

props() { # number, name, key that closes it
	ipc panel desktop properties "$1" >/dev/null
	sleep 1.2
	if shown; then
		echo "ok   the properties of $2 are shown"
	else
		fail "no properties dialog for $2"
	fi
	"$keys" key none "$3" >/dev/null 2>&1
	sleep 0.8
	shown && fail "$3 did not close the properties of $2"
}
# closed while still counting the folder, then once more
ipc panel desktop properties 0 >/dev/null
sleep 0.1
"$keys" key none escape >/dev/null 2>&1
sleep 0.5
props 0 "a link to a folder" escape
props 1 "a file" return
props 2 "a link to a phone" escape
props 3 "a program shortcut" return

# Alt+Enter on the selected icon
"$pointer" 1280 720 click 60 145 >/dev/null 2>&1
"$keys" key alt return >/dev/null 2>&1
sleep 1.2
if shown; then
	echo "ok   Alt+Enter shows the properties of the selected icon"
else
	fail "Alt+Enter showed no properties"
fi
"$keys" key none escape >/dev/null 2>&1
sleep 0.8

open_with() { # number, desktop id, what should have been started
	: > "$work/opened"
	ipc panel desktop openwith "$1" "$2" >/dev/null
	attempt=0
	while [ ! -s "$work/opened" ] && [ $attempt -lt 15 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	got=$(cat "$work/opened")
	if [ "$got" = "$3" ]; then
		echo "ok   open with: ${3:-nothing}"
	else
		fail "open with $2 on icon $1 should start \"$3\", started \"$got\""
	fi
}
open_with 1 fakeedit.desktop "fakeedit file://$desk/notes.txt"
open_with 0 fakefiles.desktop "fakefiles file://$work/Music"
open_with 3 fakeedit.desktop ""

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
