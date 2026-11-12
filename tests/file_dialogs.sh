#!/bin/sh
# The open and save dialogs: GTK's own, unless failBrauwser is the default file
# manager, then failBrauwser's. In a nested tileWin with a failBrauwser and a
# Thunar of its own (.desktop files, failBrauwser's portals.conf and its GTK 3
# module in GTK3_MODULES):
#  - with failBrauwser the default for folders, the apps tileWin starts get
#    FAILBRAUWSER_NO_CHOOSER=0 and the portal keeps failBrauwser's chooser;
#  - with Thunar the default, they get 1, and a tilewin-portals.conf of
#    tileWin's own gives the portal GTK's chooser, the rest of portals.conf kept;
#  - back to failBrauwser, that file goes again;
#  - a tilewin-portals.conf of the user's own is never touched;
#  - a dialog of failBrauwser (its app id, a title like "Save contents") floats
#    in tile mode, a window of it with a folder's name does not.
#
# usage: file_dialogs.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg

for needed in "$compositor" "$msg"; do
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
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data XDG_DATA_DIRS=$work/sysdata
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
export GTK3_MODULES=$work/lib/libfailbrauwser-chooser.so
unset FAILBRAUWSER_NO_CHOOSER
portals=$XDG_CONFIG_HOME/xdg-desktop-portal
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$portals" \
	"$XDG_DATA_HOME/applications" "$XDG_DATA_DIRS/applications"

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

# a failBrauwser as the "choose application" dialog names it, and a Thunar
cat > "$XDG_DATA_HOME/applications/userapp-failbrauwser-ABC123.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=failBrauwser
Exec=$HOME/.local/bin/failbrauwser %U
NoDisplay=true
EOF
cat > "$XDG_DATA_DIRS/applications/thunar.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Thunar
Exec=thunar %U
EOF
cat > "$portals/portals.conf" <<EOF
# Written by failBrauwser's install.sh
[preferred]
org.freedesktop.impl.portal.FileChooser=failbrauwser
default=gtk
EOF
# the default for folders, written whole as xdg-mime does
default_for_folders() {
	printf '[Default Applications]\ninode/directory=%s\n' "$1" > "$XDG_CONFIG_HOME/mimeapps.list.new"
	mv "$XDG_CONFIG_HOME/mimeapps.list.new" "$XDG_CONFIG_HOME/mimeapps.list"
}
default_for_folders userapp-failbrauwser-ABC123.desktop

{
	echo 'wallpaper solid #204070'
	echo 'session_restore no'
	echo 'remember_windows no'
	echo 'animations off'
	echo 'panel_command true'
	echo 'output HEADLESS-1 mode --custom 1280x720 pos 0 0'
	# the rule as it is shipped
	grep -F 'for_window [app_id="^org\.failbrauwser' "$source_dir/config/common.conf"
	echo "exec sh -c 'printf %s \"\$WAYLAND_DISPLAY\" > $work/display'"
} > "$work/tilewin.conf"
grep -q failbrauwser "$work/tilewin.conf" || fail "no rule for failBrauwser's dialogs in common.conf"

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")

# what an app tileWin starts now has
app_env() {
	rm -f "$work/env"
	ipc exec "env > $work/env.new && mv $work/env.new $work/env" >/dev/null
	wait_for_file 5 "$work/env"
	grep '^FAILBRAUWSER_NO_CHOOSER=' "$work/env" | cut -d= -f2
}

[ "$(app_env)" = 0 ] || fail "failBrauwser the default, yet apps get FAILBRAUWSER_NO_CHOOSER=$(app_env)"
[ -e "$portals/tilewin-portals.conf" ] && fail "failBrauwser the default, yet the portal is sent to GTK"

default_for_folders thunar.desktop
wait_until 5 test -e "$portals/tilewin-portals.conf" ||
	fail "Thunar the default, yet the portal keeps failBrauwser's chooser"
grep -qx 'org.freedesktop.impl.portal.FileChooser=gtk' "$portals/tilewin-portals.conf" 2>/dev/null ||
	fail "the portal file of tileWin does not choose GTK: $(cat "$portals/tilewin-portals.conf" 2>/dev/null)"
grep -qx 'default=gtk' "$portals/tilewin-portals.conf" 2>/dev/null ||
	fail "the rest of portals.conf was not kept"
[ "$(app_env)" = 1 ] || fail "Thunar the default, yet apps get FAILBRAUWSER_NO_CHOOSER=$(app_env)"

default_for_folders userapp-failbrauwser-ABC123.desktop
wait_until 5 test ! -e "$portals/tilewin-portals.conf" ||
	fail "failBrauwser the default again, yet the portal file of tileWin stayed"
[ "$(app_env)" = 0 ] || fail "failBrauwser the default again, yet apps get $(app_env)"

# a file of the user's own stays as it is
printf '[preferred]\ndefault=kde\n' > "$portals/tilewin-portals.conf"
default_for_folders thunar.desktop
sleep 1 # a negative: nothing may happen
grep -qx 'default=kde' "$portals/tilewin-portals.conf" ||
	fail "a tilewin-portals.conf of the user's own was written over"
rm -f "$portals/tilewin-portals.conf"

# failBrauwser's dialog floats in tile mode, a window of a folder does not
if python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' 2>/dev/null; then
	ipc mode tile >/dev/null 2>&1
	window() {
		GDK_BACKEND=wayland python3 -c '
import sys, gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib
GLib.set_prgname("org.failbrauwser.FailBrauwser")
w = Gtk.Window(title=sys.argv[1])
w.set_default_size(500, 300)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()' "$1" >/dev/null 2>&1 &
		windows="$windows $!"
	}
	windows=
	floating() {
		ipc -t get_tree | python3 -c '
import json, sys
def walk(n, floating):
	for c in n.get("nodes", []):
		walk(c, False)
	for c in n.get("floating_nodes", []):
		walk(c, True)
	if n.get("name") == sys.argv[1] and n.get("app_id") == "org.failbrauwser.FailBrauwser":
		print("floating" if floating else "tiled")
walk(json.load(sys.stdin), False)' "$1"
	}
	window "Documents"
	window "Save contents"
	for _ in $(seq 20); do
		[ -n "$(floating 'Save contents')" ] && [ -n "$(floating 'Documents')" ] && break
		sleep 0.5
	done
	[ "$(floating 'Save contents')" = floating ] ||
		fail "failBrauwser's Save dialog does not float in tile mode: $(floating 'Save contents')"
	[ "$(floating 'Documents')" = tiled ] ||
		fail "a window of failBrauwser with a folder's name floats: $(floating 'Documents')"
	kill $windows 2>/dev/null
else
	echo "GTK 3 for Python is not installed: the floating of the dialog is not looked at"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
