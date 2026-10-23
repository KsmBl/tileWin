#!/bin/sh
# Desktop links, the Type=Link .desktop files a file manager puts on the desktop
# ("Create Link on Desktop"): a file:// link opens the folder it names, with
# its %20 decoded; a scheme that has a program of its own (smb here) goes to
# xdg-open, which starts that program; trash, KDE's short trash:/ and mtp, which only a file manager
# knows, open with the default file manager. Renaming such an icon changes
# the Name= of the file (and drops its translations), not the file name, and
# keeps it executable. xdg-open and the two programs are stand-ins that write down
# what they were given.
#
# usage: desktop_links.sh <build dir> <source dir>
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
if ! command -v dbus-daemon >/dev/null 2>&1; then
	echo "dbus-daemon is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
sock=$XDG_RUNTIME_DIR/tw-links-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
export LANG=C LC_ALL=C
desk=$work/Desktop
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" \
	"$XDG_DATA_HOME/applications" "$desk" "$work/bin" "$work/My Music"
printf 'XDG_DESKTOP_DIR="%s"\n' "$desk" > "$XDG_CONFIG_HOME/user-dirs.dirs"

# the stand-ins
for tool in xdg-open fakefiles fakesmb; do
	cat > "$work/bin/$tool" <<EOS
#!/bin/sh
printf '%s %s\n' $tool "\$*" >> $work/opened
EOS
	chmod +x "$work/bin/$tool"
done
for app in fakefiles fakesmb; do
	cat > "$XDG_DATA_HOME/applications/$app.desktop" <<EOS
[Desktop Entry]
Type=Application
Name=$app
Exec=$work/bin/$app %U
EOS
done
cat > "$XDG_CONFIG_HOME/mimeapps.list" <<EOS
[Default Applications]
inode/directory=fakefiles.desktop
x-scheme-handler/smb=fakesmb.desktop
EOS
export PATH="$work/bin:$PATH"

link() { # file name url [extra lines]
	printf '[Desktop Entry]\nVersion=1.0\nType=Link\nName=%s\nURL=%s\nIcon=folder\n%s' \
		"$1" "$2" "${3:-}" > "$desk/$1.desktop"
	chmod 755 "$desk/$1.desktop"
}
# sorted by name, so their numbers are 0 to 4
music_url="file://$(printf '%s' "$work" | sed 's/ /%20/g')/My%20Music"
link Alpha "$music_url"
link Bravo "trash:///" 'Name[de]=Papierkorb
'
link Charlie "mtp://Pixel_8_ABC123/"
link Delta "smb://server/share/"
link Echo "trash:/"

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

opens() { # number, what should have been started
	: > "$work/opened"
	ipc panel desktop open "$1" >/dev/null
	attempt=0
	while [ ! -s "$work/opened" ] && [ $attempt -lt 20 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	got=$(cat "$work/opened")
	if [ "$got" = "$2" ]; then
		echo "ok   $2"
	else
		fail "link $1 should start \"$2\", started \"$got\""
	fi
}
opens 0 "xdg-open $work/My Music"
opens 1 "fakefiles trash:///"
opens 2 "fakefiles mtp://Pixel_8_ABC123/"
opens 3 "xdg-open smb://server/share/"
opens 4 "fakefiles trash:///"

# rename Bravo: the dialog holds its shown name, all of it selected
ipc panel desktop rename 1 >/dev/null
sleep 1
"$keys" text bin key none return >/dev/null 2>&1
sleep 1
file=$desk/Bravo.desktop
if [ ! -f "$file" ]; then
	fail "renaming a link renamed its file: $(ls "$desk" | tr '\n' ' ')"
else
	grep -qx 'Name=bin' "$file" || fail "renaming a link did not set Name=: $(grep Name "$file")"
	grep -q '^Name\[' "$file" && fail "the translated names of a renamed link are still there"
	grep -qx 'URL=trash:///' "$file" || fail "renaming a link lost its URL"
	[ -x "$file" ] || fail "a renamed link is no longer executable"
	[ $failures -eq 0 ] && echo "ok   renaming a link changes its Name="
fi
[ "$(ls "$desk" | wc -l)" -eq 5 ] || fail "the desktop folder changed: $(ls "$desk" | tr '\n' ' ')"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
