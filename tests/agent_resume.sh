#!/bin/bash
# End-to-end auto-resume test for the AI agent usage widget:
#  - opens TWO fake terminal windows running real foreground PTY children named
#    "claude" (matching CLAUDE_CONFIG_DIR):
#      1) one whose session log stopped on error:"rate_limit" ("hit your 5-hour limit")
#      2) one whose session log ended normally (idle, NOT affected by usage limits)
#  - opens a third non-terminal window that currently holds keyboard focus;
#  - enables auto_resume with resume_delay 2 and resume_command continue;
#  - feeds a 100% 5-hour usage window resetting in 8 seconds;
#  - verifies ONLY the rate-limited terminal receives "continue" + Enter after
#    resets_at + 2s, the idle terminal stays untouched, and focus returns to the
#    previously focused non-terminal window.
#
# usage: agent_resume.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
tool=$build/claude/tilewin-claude-usage
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel

for needed in "$tool" "$compositor" "$msg" "$panel"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
if ! python3 -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk' 2>/dev/null; then
	echo "python3 GTK4 bindings unavailable: skipping"
	exit 77
fi
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi
. "$(dirname "$0")/wait.sh"

work=$(mktemp -d)
sock=$work/tilewin.sock
claude=$work/claude-folder
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 TILEWIN_NO_AGENT_SCAN=1 GSETTINGS_BACKEND=memory
export CLAUDE_CONFIG_DIR=$claude
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
	"$claude/sessions" "$claude/projects/testproj" "$work/bin"

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

# Session 1: stopped cleanly on a usage limit
cat > "$claude/projects/testproj/11111111-1111-1111-1111-111111111111.jsonl" <<'EOF'
{"type":"user","message":{"role":"user","content":"work on task"}}
{"type":"assistant","isApiErrorMessage":true,"error":"rate_limit","message":{"model":"<synthetic>","content":[{"type":"text","text":"You've hit your 5-hour limit · resets soon"}]}}
EOF

# Session 2: sat idle after a normal assistant answer
cat > "$claude/projects/testproj/22222222-2222-2222-2222-222222222222.jsonl" <<'EOF'
{"type":"user","message":{"role":"user","content":"say hi"}}
{"type":"assistant","message":{"model":"claude-opus","content":[{"type":"text","text":"Hi!"}]}}
EOF

# Other non-terminal window that holds focus before and after auto-resume
cat > "$work/other_window.py" <<'PY'
import sys
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk

out_path = sys.argv[1]

def changed(entry):
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(entry.get_text())

def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="Other app")
    win.set_default_size(400, 120)
    entry = Gtk.Entry()
    entry.connect("changed", changed)
    win.set_child(entry)
    win.present()
    entry.grab_focus()
    open(out_path, "w").close()

app = Gtk.Application(application_id="org.tilewin.OtherApp", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run([])
PY

# Fake terminal window (app_id contains "terminal") that forks a fake interactive "claude"
# process (argv[0] = ".../claude") on a real PTY, registers its session file, and records
# typed text + Enter activation.
cat > "$work/fake_terminal.py" <<'PY'
import json, os, pty, signal, sys
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk

app_id, out_path, agent_bin, claude_dir, session_id = sys.argv[1:6]

child_pid, master_fd = pty.fork()
if child_pid == 0:
    os.environ["CLAUDE_CONFIG_DIR"] = claude_dir
    os.execv(sys.executable, [agent_bin, "-c", "import time; time.sleep(120)", "--resume", session_id])
    os._exit(127)

with open(os.path.join(claude_dir, "sessions", f"{child_pid}.json"), "w") as f:
    json.dump({"sessionId": session_id}, f)

def write_state(entry, submitted):
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"{entry.get_text()}|{'ENTER' if submitted else 'TYPED'}\n")

def activate(app):
    win = Gtk.ApplicationWindow(application=app, title=f"claude - {session_id[:8]}")
    win.set_default_size(400, 120)
    entry = Gtk.Entry()
    entry.connect("changed", lambda e: write_state(e, False))
    entry.connect("activate", lambda e: write_state(e, True))
    win.set_child(entry)
    win.present()
    entry.grab_focus()
    open(out_path, "w").close()

app = Gtk.Application(application_id=app_id, flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
try:
    app.run([])
finally:
    try:
        os.kill(child_pid, signal.SIGTERM)
    except OSError:
        pass
    try:
        os.close(master_fd)
    except OSError:
        pass
PY

# Connect fake Claude Code folder
printf '{"oauthAccount": {"emailAddress": "me@example.org"}}\n' > "$claude/.claude.json"
printf '{}\n' > "$claude/settings.json"
"$tool" connect "$claude" >/dev/null || fail "connect failed"
status_cmd=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["statusLine"]["command"])' \
	"$claude/settings.json")

# Configure agents widget with auto_resume enabled, 2s delay, "continue" command
conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
sed -e 's/^\tright tray keyboard volume/\tright agents tray keyboard volume/' \
	"$source_dir/config/taskbar.conf" > "$conf"
cat >> "$conf" <<EOF

widget agents {
	accounts claude:$claude
	auto_resume yes
	resume_delay 2
	resume_command continue
}
EOF

# Symlink panel into $work/bin so paths with spaces work with sway's "exec %s"
ln -s "$panel" "$work/bin/tilewin-panel"

# Start compositor with panel
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #204070
session_restore no
animations off
panel_command $work/bin/tilewin-panel -d
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	"$(dirname "$0")/session.sh" "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
if ! wait_for_file 15 "$work/display"; then
	echo "the nested compositor never came up"
	tail -n 20 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
unset DISPLAY
export GDK_BACKEND=wayland

# Launch limited terminal, idle terminal, and focused non-terminal window via compositor exec
ipc exec "python3 '$work/fake_terminal.py' org.tilewin.LimitedTerminal '$work/limited_out' '$work/bin/claude' '$claude' 11111111-1111-1111-1111-111111111111" >/dev/null
wait_until 15 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.LimitedTerminal\"'" ||
	fail "limited terminal window never appeared"

ipc exec "python3 '$work/fake_terminal.py' org.tilewin.IdleTerminal '$work/idle_out' '$work/bin/claude' '$claude' 22222222-2222-2222-2222-222222222222" >/dev/null
wait_until 15 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.IdleTerminal\"'" ||
	fail "idle terminal window never appeared"

ipc exec "python3 '$work/other_window.py' '$work/other_out'" >/dev/null
wait_until 15 sh -c "'$msg' -s '$sock' -t get_tree | grep -q '\"app_id\": \"org.tilewin.OtherApp\"'" ||
	fail "other window never appeared"
ipc '[app_id="org.tilewin.OtherApp"] focus' >/dev/null 2>&1

# Seed a 100% 5h usage window resetting in 8 seconds so the panel sees it armed before reset
now=$(date +%s)
reset_at=$((now + 8))
printf '{"rate_limits":{"five_hour":{"used_percentage":100,"resets_at":%d},"seven_day":{"used_percentage":40,"resets_at":%d}}}' \
	"$reset_at" $((now + 3 * 86400)) | sh -c "$status_cmd" >/dev/null

# Wait until the rate-limited terminal receives "continue|ENTER" after reset_at + 2s
if ! wait_until 25 sh -c "grep -qx 'continue|ENTER' '$work/limited_out' 2>/dev/null"; then
	fail "limited terminal did not receive 'continue' + Enter after reset (got: '$(cat "$work/limited_out" 2>/dev/null)')"
	grep -i "auto-resume\|resume" "$work/log" | tail -n 20 || true
fi

# Give any stray queued jobs a moment to run, then verify idle terminal & other window stayed untouched
sleep 1
if [ -s "$work/idle_out" ]; then
	fail "idle terminal was erroneously sent text: '$(cat "$work/idle_out")'"
fi
if [ -s "$work/other_out" ]; then
	fail "non-terminal window received text: '$(cat "$work/other_out")'"
fi
wait_until 5 sh -c "'$msg' -s '$sock' -t get_tree | python3 -c '
import json, sys
def find(n):
    if n.get(\"app_id\") == \"org.tilewin.OtherApp\" and n.get(\"focused\"):
        return True
    return any(find(c) for k in (\"nodes\", \"floating_nodes\") for c in n.get(k, []))
sys.exit(0 if find(json.load(sys.stdin)) else 1)
'" || fail "focus did not return to the previously focused window"

ipc -t get_version >/dev/null 2>&1 || fail "tileWin no longer answers"
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
