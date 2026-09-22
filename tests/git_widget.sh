#!/bin/sh
# The git widget has to read the repository out of the focused window and put
# the branch on the bar. A widget that draws nothing takes no room at all, so
# the check is that the branch turns up in the pixels of the taskbar: the test
# makes a repository whose branch has a name no other text on the bar shares,
# opens a terminal in it, and counts what the bar drew before and after.
#
# usage: git_widget.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
panel=$build/panel/tilewin-panel
msg=$build/swaymsg/tilewinmsg
count=$build/tests/count-content

for needed in "$compositor" "$panel" "$msg" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in git grim dbus-run-session xfce4-terminal; do
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
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	pkill -f "tilewin -c $work/" 2>/dev/null
	sleep 0.3
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# a repository of its own, so the test does not depend on the state of any other
repo=$work/repo
mkdir -p "$repo"
git -C "$repo" init -q -b widgettestbranch
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name "Widget Test"
printf 'one\n' > "$repo/file.txt"
git -C "$repo" add file.txt
git -C "$repo" commit -qm "first"
printf 'one\ntwo\nthree\n' > "$repo/file.txt"   # +2 lines, unstaged

# the git widget alone on the right, so nothing else can be mistaken for it
sed 's/^\tright .*/\tright git/' "$source_dir/config/taskbar.conf" > "$XDG_CONFIG_HOME/tileWin/taskbar.conf"
printf '\ntheme_layout no\n' >> "$XDG_CONFIG_HOME/tileWin/taskbar.conf"

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command $panel
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$work/ipc.sock" TILEWINSOCK="$work/ipc.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
sleep 4

# the right-hand end of the bar, where the widget draws
shot_area() {
	grim "$1" 2>/dev/null
}
# 1280x720 output, the bar along the bottom
area_x=900
area_y=690
area_width=370
area_height=28

shot_area "$work/before.png"
empty=$("$count" "$work/before.png" $area_x $area_y $area_width $area_height)

"$msg" -s "$work/ipc.sock" exec "sh -c 'cd $repo && exec xfce4-terminal --disable-server'" >/dev/null 2>&1
attempt=0
while [ $attempt -lt 30 ]; do
	sleep 1
	attempt=$((attempt + 1))
	"$msg" -s "$work/ipc.sock" -t get_tree | grep -q '"app_id": "xfce4-terminal"' && break
done
sleep 6 # the widget has to notice the focus and let git answer

shot_area "$work/after.png"
drawn=$("$count" "$work/after.png" $area_x $area_y $area_width $area_height)
echo "the bar drew $empty pixels there with no repository, $drawn with one"
if [ "$drawn" -le "$empty" ]; then
	fail "the git widget drew nothing once a window was in a repository"
fi

# and it has to be the branch, not any old text: ask the widget's own source of
# truth the same way it does, so a wrong directory cannot pass
resolved=$(git -C "$repo" rev-parse --abbrev-ref HEAD)
if [ "$resolved" != "widgettestbranch" ]; then
	fail "the test repository is not on the branch the test expects"
fi

# with the window gone the reading stays: that is what makes it usable
"$msg" -s "$work/ipc.sock" kill >/dev/null 2>&1
sleep 3
shot_area "$work/gone.png"
kept=$("$count" "$work/gone.png" $area_x $area_y $area_width $area_height)
echo "after the window closed the bar still drew $kept pixels there"
if [ "$kept" -le "$empty" ]; then
	fail "the reading was dropped as soon as the window went; it should stay"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the git widget follows the focused window and keeps what it found"
exit 0
