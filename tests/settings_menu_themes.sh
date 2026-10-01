#!/bin/sh
# A Theme submenu of single "tilewin-theme set" items, as configs from before
# the "themes" line have it, lists only the themes there were back then; the
# taskbar shows every installed theme there anyway, and the menu editor of the
# settings has to show it as that list and save it as "themes".
#
# usage: settings_menu_themes.sh <build dir> <source dir>
set -u

build=$1
source=$2
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
tool=$build/tests/pointer-tool

for needed in "$compositor" "$settings" "$tool"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session is not installed: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

work=$(mktemp -d)
cleanup() {
	pkill -f "tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# a session of its own, so the test neither reads nor writes the real config
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
export TILEWIN_NO_APP_TWEAKS=1 # leaves the real desktop and home alone
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

# the default taskbar.conf, with the theme list of an old one
sed 's/^\t\tthemes .*/\t\titem "Windows 95" exec tilewin-theme set win95\
\t\titem "Windows XP" exec tilewin-theme set winxp\
\t\titem "Windows 7" exec tilewin-theme set win7/' \
	"$source/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
if ! grep -q "tilewin-theme set winxp" "$XDG_CONFIG_HOME/tileWin/taskbar.conf"; then
	echo "the default taskbar.conf has no themes line to replace any more"
	exit 1
fi

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
output * mode --custom 1280x3000
for_window [app_id="org.tilewin.Settings"] fullscreen enable
exec "$settings" --page=taskbar
EOF

before=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	SWAYSOCK="$work/tilewin.sock" TILEWINSOCK="$work/tilewin.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
starter=$!

display=
attempt=0
while [ -z "$display" ] && [ $attempt -lt 40 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
	after=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$')
	display=$(echo "$after" | grep -vxF "$before" | head -n 1)
done
if [ -z "$display" ]; then
	echo "the nested compositor never came up"
	tail -n 2 "$work/log"
	exit 1
fi
sleep 6 # the settings app has to be up and laid out before it is clicked

# The page is tall enough to show the taskbar menu, the last one of the
# right-click menus: open its Theme submenu, then add a separator to it, which
# saves the menu.
WAYLAND_DISPLAY=$display "$tool" 1280 3000 click 1094 2760 >/dev/null 2>&1
sleep 1
WAYLAND_DISPLAY=$display "$tool" 1280 3000 click 904 2368 >/dev/null 2>&1
sleep 3
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/taskbar.conf
theme=$(sed -n '/submenu "Theme"/,/^\t}/p' "$written")
if echo "$theme" | grep -q '^[[:space:]]*themes$' && echo "$theme" | grep -q separator &&
		! grep -v "^[[:space:]]*#" "$written" | grep -q "tilewin-theme set"; then
	echo "the old theme list was saved as every installed theme"
	exit 0
fi
echo "the Theme submenu was not saved as the list of every installed theme."
echo "Either the editor keeps the old items, or the Taskbar page has been"
echo "rearranged and the submenu is no longer at 1094,2760."
echo "$theme"
exit 1
