#!/bin/sh
# GTK 3's message dialogs as tileWin's own: in a nested tileWin with the GTK 3
# module and tilewin-dialog of this build, a GTK 3 app asks "Save changes?"
# with Don't Save, Cancel and Save (the default):
#  - tileWin's dialog comes (app id org.tilewin.Dialog) with the app's title,
#    floating over the app, and GTK's own does not;
#  - Return answers Save, Escape answers as closing the dialog (-4), and a
#    click on Don't Save answers No: the app gets each as the dialog's response;
#  - a dialog with a check box the app put in stays GTK's own;
#  - TILEWIN_NO_GTK_DIALOGS=1 keeps GTK's own for an app.
#
# usage: gtk3_dialogs.sh <build dir> <source dir>
set -u

build=$(cd "$1" && pwd) # GTK3_MODULES takes only a whole path
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
keys=$build/tests/keyboard-tool
pointer=$build/tests/pointer-tool
module=$build/dialog/libtilewin-gtk3-dialogs.so
helper=$build/dialog/tilewin-dialog

for needed in "$compositor" "$msg" "$keys" "$pointer" "$helper"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if [ ! -f "$module" ]; then
	echo "the GTK 3 module was not built: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
if ! python3 -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' \
		2>/dev/null; then
	echo "GTK 3 for Python is not installed: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
# GIO without gvfs, whose daemon the nested session's bus may keep waiting
export GIO_USE_VFS=local
export TILEWIN_GTK3_DIALOGS=$module TILEWIN_DIALOG=$helper
unset GTK3_MODULES TILEWIN_NO_GTK_DIALOGS
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

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

{
	echo 'wallpaper solid #204070'
	echo 'session_restore no'
	echo 'remember_windows no'
	echo 'animations off'
	echo 'panel_command true'
	echo 'output HEADLESS-1 mode --custom 1280x720 pos 0 0'
	echo "exec sh -c 'printf %s \"\$WAYLAND_DISPLAY\" > $work/display'"
} > "$work/tilewin.conf"

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
ipc mode tile >/dev/null 2>&1

# where a window is: "floating|tiled x y width height", by app id
place() {
	ipc -t get_tree | python3 -c '
import json, sys
def walk(n, floating):
	for c in n.get("nodes", []):
		walk(c, False)
	for c in n.get("floating_nodes", []):
		walk(c, True)
	if n.get("app_id") == sys.argv[1] and n.get("pid"):
		r = n["rect"]
		print("floating" if floating else "tiled", r["x"], r["y"], r["width"], r["height"],
			n.get("name"))
walk(json.load(sys.stdin), False)' "$1"
}
dialog_up() { [ -n "$(place org.tilewin.Dialog)" ]; }
gtk_dialog_up() {
	# GTK's own: a second window of the app, the dialog
	[ "$(place tilewin-test-editor | wc -l)" -ge 2 ]
}
screen() { grim -t ppm - 2>/dev/null | md5sum; }

# <kind> [env]: the app asks; its answer lands in $work/answer
ask() {
	rm -f "$work/answer"
	ipc exec "${2:-} python3 $source_dir/tests/gtk3_dialog_app.py $work/answer $1 >> $work/app.log 2>&1" >/dev/null
}
answer() {
	wait_for_file 5 "$work/answer" && cat "$work/answer"
}
app_gone() { [ -z "$(place tilewin-test-editor)" ]; }

# ---- Return: the default, Save
ask plain
if wait_until 15 dialog_up; then
	set -- $(place org.tilewin.Dialog)
	[ "$1" = floating ] || fail "tileWin's dialog is not floating in tile mode: $*"
	[ "${6:-}" = Editor ] || fail "tileWin's dialog has the title '${6:-}', not the app's"
	gtk_dialog_up && fail "GTK's own dialog came as well"
	wait_still 5 screen
	[ -n "${DIALOG_SHOTS:-}" ] && grim "$DIALOG_SHOTS/dialog.png"
	"$keys" key none return
	[ "$(answer)" = -8 ] || fail "Return answered '$(cat "$work/answer" 2>/dev/null)', not Save (-8)"
else
	fail "no dialog of tileWin came for the app's question"
	place tilewin-test-editor
	tail -n 5 "$work/app.log"
fi
wait_until 5 app_gone

# ---- Escape: as closing it
ask plain
if wait_until 15 dialog_up; then
	wait_still 5 screen
	"$keys" key none escape
	[ "$(answer)" = -4 ] || fail "Escape answered '$(cat "$work/answer" 2>/dev/null)', not -4"
else
	fail "no dialog of tileWin came the second time"
fi
wait_until 5 app_gone

# ---- Alt+N, the access key of Don't Save
ask plain
if wait_until 15 dialog_up; then
	wait_still 5 screen
	"$keys" key alt n
	[ "$(answer)" = -9 ] || fail "Alt+N answered '$(cat "$work/answer" 2>/dev/null)', not Don't Save (-9)"
else
	fail "no dialog of tileWin came the third time"
fi
wait_until 5 app_gone

# ---- with a check box of the app's: GTK's own
ask checkbox
if wait_until 15 gtk_dialog_up; then
	dialog_up && fail "tileWin's dialog came for one with a check box"
	wait_still 5 screen
	"$keys" key none escape
	answer >/dev/null || fail "GTK's own dialog did not answer"
else
	fail "GTK's own dialog with the check box never came"
fi
wait_until 5 app_gone

# ---- TILEWIN_NO_GTK_DIALOGS=1
ask plain TILEWIN_NO_GTK_DIALOGS=1
if wait_until 15 gtk_dialog_up; then
	dialog_up && fail "tileWin's dialog came with TILEWIN_NO_GTK_DIALOGS=1"
	wait_still 5 screen
	"$keys" key none escape
	answer >/dev/null
else
	fail "GTK's own dialog never came with TILEWIN_NO_GTK_DIALOGS=1"
fi

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
