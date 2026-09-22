#!/bin/sh
# The Apps page can be given a program that has no entry of its own: Other...
# asks for a command, and what is typed there has to end up in common.conf. A
# dialog that opens and closes without saving looks perfectly healthy, so the
# only way to tell is to work it and read the file back.
#
# usage: settings_custom_app.sh <build dir>
set -u

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
pointer=$build/tests/pointer-tool
keys=$build/tests/keyboard-tool

for needed in "$compositor" "$settings" "$pointer" "$keys"; do
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
exec "$settings" --page=apps
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	SWAYSOCK="$work/tilewin.sock" TILEWINSOCK="$work/tilewin.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
starter=$!

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 2 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
sleep 6 # the settings app has to be up and laid out before it is clicked

# The Task manager row: its Other... button, then the command, then the button
# that keeps it. That row writes $taskmanager to common.conf, which is the
# half of the dialog that can be read back from a file.
"$pointer" 1280 720 click 988 460 >/dev/null 2>&1
sleep 2
# Return in the field is what keeps it, so the test need not know where the
# button sits: that moves with the length of the description above it.
"$keys" key ctrl a text htop key none return >/dev/null 2>&1
sleep 2
kill "$starter" 2>/dev/null
pkill -f "tilewin -c $work/" 2>/dev/null

written=$XDG_CONFIG_HOME/tileWin/common.conf
if [ ! -f "$written" ]; then
	echo "the settings app wrote no common.conf at all"
	exit 1
fi
if grep -q '^set \$taskmanager htop$' "$written"; then
	echo "the program typed into Other... was saved"
	exit 0
fi
echo "the command typed into Other... was not saved."
echo "Either the dialog no longer writes the variable, the entry no longer takes"
echo "the keyboard or Return, or the Apps page has been rearranged and the"
echo "Other... button of the task manager is no longer at 988,460."
grep -n 'taskmanager' "$written" || echo "(common.conf has no taskmanager line)"
exit 1
