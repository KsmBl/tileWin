#!/bin/sh
# Checks that the clipboard history lives in tilewin-clipboard and not in the
# taskbar: that copies turn up over its socket, that the commands the list sends
# do what they say, and that the bytes of a copied picture land in that process
# while the taskbar stays where it was.
#
# usage: clipboard_split.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
panel=$build/panel/tilewin-panel
clipboard=$build/clipboard/tilewin-clipboard
probe=$source_dir/tests/clipboard_probe.py

for needed in "$compositor" "$panel" "$clipboard"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in wl-copy python3 dbus-run-session; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

# The socket lives in XDG_RUNTIME_DIR and a unix path is only 108 bytes, so the
# run gets a short directory of its own rather than one under the build tree.
work=$(mktemp -d "$XDG_RUNTIME_DIR/twclip.XXXXXX") || exit 77
state=$work/state
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$state" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

cleanup() {
	[ -n "${clip_pid:-}" ] && kill "$clip_pid" 2>/dev/null
	[ -n "${panel_pid:-}" ] && kill "$panel_pid" 2>/dev/null
	[ -n "${comp_pid:-}" ] && kill "$comp_pid" 2>/dev/null
	# killing the shell that started it does not kill the compositor, and one
	# left behind holds on to an output; the config path is unique to this run
	pkill -f "tilewin -c $work/" 2>/dev/null
	sleep 0.3
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# the compositor says which display it took, rather than the test guessing from
# the sockets lying about in XDG_RUNTIME_DIR
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	SWAYSOCK="$work/ipc.sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/compositor.log" 2>&1 &
comp_pid=$!

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/compositor.log"
	exit 1
fi
display=$(cat "$work/display")
export WAYLAND_DISPLAY=$display

# A tilewin-clipboard left over from an earlier run on the same display holds
# the lock, so the one started here would exit at once and the test would go on
# measuring a process that is no longer there.
# the name is over fifteen characters, so /proc reports it cut short
for stale in $(pgrep -x tilewin-clipboa 2>/dev/null); do
	if tr '\0' '\n' < "/proc/$stale/environ" 2>/dev/null |
			grep -qx "WAYLAND_DISPLAY=$display"; then
		kill "$stale" 2>/dev/null
	fi
done
sleep 1

"$clipboard" > "$work/clipboard.log" 2>&1 &
clip_pid=$!
sleep 2
if ! kill -0 "$clip_pid" 2>/dev/null; then
	echo "tilewin-clipboard stopped as soon as it started"
	tail -n 3 "$work/clipboard.log"
	exit 1
fi
attempt=0
while [ ! -S "$XDG_RUNTIME_DIR/tilewin-clipboard-$display.sock" ] && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
if [ ! -S "$XDG_RUNTIME_DIR/tilewin-clipboard-$display.sock" ]; then
	echo "tilewin-clipboard never opened its socket"
	tail -n 3 "$work/clipboard.log"
	exit 1
fi

list() { python3 "$probe" list; }

# ---------- a copied text turns up ----------
printf 'hello from the test' | wl-copy
sleep 1
if ! list | grep -q "hello from the test"; then
	fail "a copied text did not turn up in the history"
	list
fi

# ---------- a second copy goes on top, and the first is still there ----------
printf 'the newer one' | wl-copy
sleep 1
if [ "$(list | head -n 1 | cut -d' ' -f4-)" != "the newer one" ]; then
	fail "the newest copy is not at the top"
	list
fi
if [ "$(list | wc -l)" != "2" ]; then
	fail "expected two entries, got $(list | wc -l)"
fi

# ---------- only a preview of a long text is handed out ----------
python3 -c "print('x' * 200000, end='')" | wl-copy
sleep 1
longest=$(list | awk '{ print length($0) }' | sort -n | tail -n 1)
if [ "$longest" -gt 1024 ]; then
	fail "the whole text was handed to the list ($longest bytes), not a preview"
fi

# ---------- a copied picture turns up with a thumbnail ----------
python3 "$probe" png "$work/picture.png" 800 600
wl-copy --type image/png < "$work/picture.png"
sleep 1.5
image_id=$(list | awk '$2 == 1 { print $1; exit }')
if [ -z "$image_id" ]; then
	fail "a copied picture did not turn up in the history"
	list
else
	# a thumbnail fits in 300x120, and 800x600 runs out of height first
	size=$(python3 "$probe" thumb "$image_id")
	case "$size" in
	"160 120") ;;
	*) fail "the thumbnail of an 800x600 picture came out as '$size', wanted 160x120" ;;
	esac
fi

# ---------- the memory of a big picture belongs to tilewin-clipboard ----------
rss() { awk '/VmRSS/ { print $2 }' "/proc/$1/status" 2>/dev/null; }
panel_before=0
clip_before=$(rss "$clip_pid")
"$panel" > "$work/panel.log" 2>&1 &
panel_pid=$!
sleep 3
panel_before=$(rss "$panel_pid")
# noisy, so the file is about twelve megabytes rather than a compressible few
python3 "$probe" png "$work/big.png" 2000 2000 noisy
wl-copy --type image/png < "$work/big.png"
sleep 2.5
# open and close the list so the taskbar has fetched everything it draws
"$build/swaymsg/tilewinmsg" -s "$work/ipc.sock" panel clipboard >/dev/null 2>&1
sleep 1.5
"$build/swaymsg/tilewinmsg" -s "$work/ipc.sock" panel close >/dev/null 2>&1
sleep 0.5
panel_after=$(rss "$panel_pid")
clip_after=$(rss "$clip_pid")
picture_kb=$(( $(wc -c < "$work/big.png") / 1024 ))
clip_growth=$(( clip_after - clip_before ))
panel_growth=$(( panel_after - panel_before ))
echo "picture ${picture_kb} kB: tilewin-clipboard grew ${clip_growth} kB, the taskbar ${panel_growth} kB"
# the program that keeps it must hold the bytes; the taskbar draws a thumbnail
# and opens a window for the list, which is worth a little but nothing like this
if [ "$clip_growth" -lt "$(( picture_kb / 2 ))" ]; then
	fail "tilewin-clipboard did not take the picture (${clip_growth} kB for a ${picture_kb} kB copy)"
fi
if [ "$panel_growth" -ge "$(( picture_kb / 4 ))" ]; then
	fail "the taskbar grew by ${panel_growth} kB for a ${picture_kb} kB copy: it is keeping the data"
fi
kill "$panel_pid" 2>/dev/null

# ---------- pinning, removing and clearing ----------
first_id=$(list | head -n 1 | cut -d' ' -f1)
python3 "$probe" send pin "$first_id"
sleep 0.5
if [ "$(list | awk -v id="$first_id" '$1 == id { print $3 }')" != "1" ]; then
	fail "pinning an entry did not take"
fi
count_before=$(list | wc -l)
python3 "$probe" send clear
sleep 0.5
count_after=$(list | wc -l)
if [ "$count_after" != "1" ]; then
	fail "clearing left $count_after entries instead of the one pinned (was $count_before)"
fi
python3 "$probe" send remove "$first_id"
sleep 0.5
if [ "$(list | wc -l)" != "0" ]; then
	fail "removing the last entry left it in the list"
fi

# ---------- a pinned entry comes back after a restart ----------
printf 'pinned across restarts' | wl-copy
sleep 1
pin_id=$(list | head -n 1 | cut -d' ' -f1)
python3 "$probe" send pin "$pin_id"
sleep 0.5
kill "$clip_pid" 2>/dev/null
wait "$clip_pid" 2>/dev/null
sleep 0.5
"$clipboard" >> "$work/clipboard.log" 2>&1 &
clip_pid=$!
attempt=0
while [ ! -S "$XDG_RUNTIME_DIR/tilewin-clipboard-$display.sock" ] && [ $attempt -lt 40 ]; do
	sleep 0.25
	attempt=$((attempt + 1))
done
if ! list | grep -q "pinned across restarts"; then
	fail "a pinned entry did not come back after a restart"
	list
fi

# ---------- only one runs at a time ----------
"$clipboard" > "$work/second.log" 2>&1
if [ -S "$XDG_RUNTIME_DIR/tilewin-clipboard-$display.sock" ]; then
	list >/dev/null 2>&1 || fail "a second tilewin-clipboard took the socket away from the first"
else
	fail "a second tilewin-clipboard removed the socket"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "the clipboard history lives in its own process"
exit 0
