#!/bin/bash
# The guard of the ports: a program of the user's that listens on all
# addresses is asked about once (tilewin-dialog), the answer is kept in
# ports.conf for the program, the ports of the programs allowed go to the
# helper, those of programs blocked do not; started again on another port, an
# allowed program is not asked about again. The helper and the dialog are
# stand-ins that write down what they got, so nothing touches the firewall.
#
# usage: port_guard.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel

for needed in "$compositor" "$msg" "$panel"; do
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
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
# off by default: switched on as the Network page of the settings does
echo "guard yes" > "$XDG_CONFIG_HOME/tileWin/ports.conf"

# the helper: writes down its arguments
cat > "$work/helper" <<EOT
#!/bin/sh
echo "\$*" >> $work/applied
EOT
# the dialog: keeps the question, answers what $work/answer says
cat > "$work/dialog" <<EOT
#!/bin/sh
cat > $work/question.\$\$
cat $work/answer
EOT
chmod +x "$work/helper" "$work/dialog"
export TILEWIN_PORTS_GUARD=$work/helper TILEWIN_DIALOG=$work/dialog

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ipc() { "$msg" -s "$sock" "$@"; }
servers=()
cleanup() {
	for s in "${servers[@]}"; do kill "$s" 2>/dev/null; done
	ipc exit >/dev/null 2>&1
	wait_until 5 sh -c "! pgrep -f '^[^ ]*/tilewin -c $work/' >/dev/null"
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# a server on all addresses, on a free port; prints the port
cat > "$work/server.py" <<'EOT'
import socket, sys, time
s = socket.socket()
s.bind(('0.0.0.0', 0))
s.listen()
print(s.getsockname()[1], flush=True)
time.sleep(600)
EOT
cp "$work/server.py" "$work/other.py"
serve() { # script: starts it, sets port
	python3 "$1" > "$work/port.out" &
	servers+=($!)
	wait_for_file 5 "$work/port.out"
	port=$(cat "$work/port.out")
	rm -f "$work/port.out"
}
questions() { ls "$work"/question.* 2>/dev/null | wc -l; }

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

# the table goes up at once, nothing allowed
wait_until 10 grep -q '^apply - -$' "$work/applied" || fail "the guard did not start with nothing allowed"

echo 1 > "$work/answer"
serve "$work/server.py"
first=$port
wait_until 10 grep -q "^apply $first -\$" "$work/applied" ||
	fail "the port of the program allowed did not go to the helper"
[ "$(questions)" -eq 1 ] || fail "not asked once about the new program ($(questions))"
grep -q "Allow python3" "$work"/question.* || fail "the question does not name the program"
grep -q "^allow .*python3.* $work/server.py\$" "$XDG_CONFIG_HOME/tileWin/ports.conf" ||
	fail "the answer was not kept for the script"

# stopped and started again, on another port: not asked again
kill "${servers[0]}"
wait_until 10 sh -c "tail -n 1 '$work/applied' | grep -q '^apply - -\$'" ||
	fail "the port stayed open after the program ended"
serve "$work/server.py"
second=$port
wait_until 10 grep -q "^apply $second -\$" "$work/applied" ||
	fail "the program allowed before was not let through on its new port"
[ "$(questions)" -eq 1 ] || fail "asked again about a program allowed before"

# another script with the same interpreter is another program: blocked
echo 2 > "$work/answer"
serve "$work/other.py"
blocked=$port
wait_until 10 sh -c "[ \$(ls $work/question.* | wc -l) -eq 2 ]" ||
	fail "not asked about the other script"
wait_until 10 grep -q "^block .* $work/other.py\$" "$XDG_CONFIG_HOME/tileWin/ports.conf" ||
	fail "the block was not kept"
sleep 3
grep -q "$blocked" "$work/applied" && fail "the port of the program blocked went to the helper"

# switched off: the table goes
sed -i 's/^guard yes$/guard no/' "$XDG_CONFIG_HOME/tileWin/ports.conf"
wait_until 10 grep -q '^stop$' "$work/applied" || fail "switching the guard off did not stop it"

pgrep -f "^$panel" >/dev/null || fail "the panel is gone"
if [ "$failures" -gt 0 ]; then
	cat "$work/applied"
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
