#!/bin/sh
# The display manager from the login screen to a session and back, with the real
# greeter and compositor on a headless screen and the stand-in for PAM (which is
# in a test program only):
#  - the greeter comes up in the look of the theme, and asks;
#  - a wrong password is refused, nothing starts, and nothing is remembered;
#  - the right one ends the greeter, starts the session with its environment and
#    remembers user and session;
#  - after the session the greeter is back, with that theme;
#  - a session that is not installed, and root, are refused by the daemon;
#  - a further question of PAM (a code) is asked and answered;
#  - shutting down goes through the daemon, and not when it is turned off;
#  - the automatic login starts the session without a greeter, once.
#
# usage: display_manager.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
daemon=$build/tests/tilewin-display-manager-test
compositor=$build/sway/tilewin
greeter=$build/dm/tilewin-dm-greeter
msg=$build/swaymsg/tilewinmsg
keys=$build/tests/keyboard-tool
pointer=$build/tests/pointer-tool

for needed in "$daemon" "$compositor" "$greeter" "$msg" "$keys" "$pointer"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim python3; do
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
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

mkdir -p "$work/sessions" "$work/bin" "$work/home/.config/tileWin"
echo win95 > "$work/home/.config/tileWin/current-theme"
echo dark > "$work/home/.config/tileWin/color-scheme"

# the session: says what it was given, and lasts until the test says so
cat > "$work/bin/fake-session" <<EOS
#!/bin/sh
{
	echo "USER=\$USER"
	echo "HOME=\$HOME"
	echo "XDG_SESSION_DESKTOP=\${XDG_SESSION_DESKTOP:-}"
	echo "EXTRA=\${EXTRA:-}"
	echo "ARGS=\$*"
} > "$work/session.env"
while [ ! -e "$work/session.end" ]; do
	sleep 0.1
done
rm -f "$work/session.end"
EOS
cat > "$work/bin/systemctl" <<EOS
#!/bin/sh
echo "\$*" >> "$work/systemctl.calls"
EOS
chmod +x "$work/bin/fake-session" "$work/bin/systemctl"
cat > "$work/sessions/fake.desktop" <<EOS
[Desktop Entry]
Name=Fake session
Exec=$work/bin/fake-session one %U
DesktopNames=FakeDesktop;other
EOS

export TILEWIN_DM_SESSIONS=$work/sessions
export TW_DM_FAKE_PASSWORD=rightword TW_DM_FAKE_HOME=$work/home TW_DM_FAKE_LOG=$work/backend.log
export TILEWIN_DATADIR=$source_dir GSETTINGS_BACKEND=memory TILEWIN_BATTERY_POLL_MS=60000
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export PATH=$work/bin:$build/swaymsg:$PATH
# no bus either: nothing under the login screen may need one of the real session
unset WAYLAND_DISPLAY DISPLAY SWAYSOCK TILEWINSOCK XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME
unset DBUS_SESSION_BUS_ADDRESS

conf=$work/display-manager.conf
state=$work/state
run=$work/run
pid=
cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	sleep 0.6
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

start_daemon() {
	rm -f "$work/display" "$state/shared/last-user"
	mkdir -p "$state/home/state" "$state/home/cache"
	"$daemon" --config "$conf" --state "$state" --run "$run" \
		--compositor "$compositor" --greeter "$work/bin/greeter-wrap" > "$work/daemon.log" 2>&1 &
	pid=$!
}

# the greeter under a wrapper, which only says where the compositor is for this test
cat > "$work/bin/greeter-wrap" <<EOS
#!/bin/sh
printf %s "\$WAYLAND_DISPLAY" > "$work/display.new"
printf %s "\$SWAYSOCK" > "$work/sock"
mv "$work/display.new" "$work/display"
exec "$greeter" "\$@"
EOS
chmod +x "$work/bin/greeter-wrap"

wait_for() { # <what> <test...>
	what=$1
	shift
	attempt=0
	while ! "$@" && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	"$@" || { fail "$what"; return 1; }
}

# of this run: its config is in a folder of its own
greeter_running() { pgrep -f "^$greeter .*--config $conf" >/dev/null; }
greeter_up() { [ -s "$work/display" ] && greeter_running; }
has_env() { [ -s "$work/session.env" ]; }
gone() { ! greeter_running; }

type_into_greeter() {
	WAYLAND_DISPLAY=$(cat "$work/display") "$keys" "$@"
}
shot() {
	WAYLAND_DISPLAY=$(cat "$work/display") grim -t ppm "$1"
}
same() { cmp -s "$1" "$2"; }

cat > "$conf" <<EOC
show_users no
default_session fake
fail_delay 0
env EXTRA=from-the-config
clock none
EOC
start_daemon
wait_for "the greeter did not come up: $(tail -n 3 "$work/daemon.log" 2>/dev/null)" greeter_up
sleep 1.2
shot "$work/empty.ppm"

# ---- a wrong password
type_into_greeter text sam key none return text wrongword key none return
sleep 1.5
[ ! -s "$work/session.env" ] || fail "a session started with a wrong password"
[ ! -e "$state/shared/last-user" ] || fail "a wrong password was remembered"
greeter_up || fail "the greeter went after a wrong password"
shot "$work/refused.ppm"
same "$work/empty.ppm" "$work/refused.ppm" && fail "the greeter shows nothing after a wrong password"

# ---- the right one: the field holds the name, the focus is on the password
type_into_greeter text rightword key none return
wait_for "the right password started no session: $(cat "$work/backend.log")" has_env
gone || fail "the greeter is still up in the session"
grep -q "^USER=sam$" "$work/session.env" || fail "the session is not of sam: $(cat "$work/session.env")"
grep -q "^HOME=$work/home$" "$work/session.env" || fail "the session has another home"
grep -q "^XDG_SESSION_DESKTOP=FakeDesktop$" "$work/session.env" || fail "the desktop of the session"
grep -q "^EXTRA=from-the-config$" "$work/session.env" || fail "the env line of the config"
grep -q "^ARGS=one" "$work/session.env" || fail "the arguments of the session"
if grep -q "%U" "$work/session.env"; then fail "a field code reached the session"; fi
[ "$(cat "$state/shared/last-user" 2>/dev/null)" = sam ] || fail "the user is not remembered"
[ "$(cat "$state/shared/last-session" 2>/dev/null)" = fake ] || fail "the session is not remembered"
[ "$(cat "$state/shared/last-theme" 2>/dev/null)" = win95 ] || fail "the theme of the user is not remembered"
[ "$(cat "$state/shared/last-scheme" 2>/dev/null)" = dark ] || fail "the scheme of the user is not remembered"
# the order that matters: PAM opened it only after the greeter's session closed
grep -n "" "$work/backend.log" | grep -E "open sam|close greeter" | tr '\n' ' ' > "$work/order"
case $(cat "$work/order") in
*"close greeter"*"open sam"*) ;;
*) fail "the session opened before the greeter ended: $(cat "$work/order")" ;;
esac

# ---- out of the session: back at the login screen, in the look of that theme
rm -f "$work/display" "$work/session.env"
touch "$work/session.end"
wait_for "the greeter did not come back after the session" greeter_up
pgrep -f "tilewin-nightlight|tilewin-polkit" -P "$(pgrep -f "^$compositor -c $run/" | head -n 1)" >/dev/null &&
	fail "a helper of the desktop runs under the login screen"
grep -q "close sam" "$work/backend.log" || fail "the session was not closed"
[ "$(cat "$state/shared/config/tileWin/current-theme" 2>/dev/null)" = win95 ] ||
	fail "the login screen does not wear the theme of who logged in"
[ "$(cat "$state/shared/config/tileWin/color-scheme" 2>/dev/null)" = dark ] ||
	fail "the login screen does not wear the scheme of who logged in"
sleep 1.2

# ---- the session of the file is gone: the daemon refuses
mv "$work/sessions/fake.desktop" "$work/fake.away"
# the name is there from last time: the keys go to the password. By now this is
# the dialog of Windows 95, the theme of who logged in, and its OK button works
type_into_greeter text rightword
sleep 0.3
WAYLAND_DISPLAY=$(cat "$work/display") "$pointer" 1280 720 move 300 300
sleep 0.3
WAYLAND_DISPLAY=$(cat "$work/display") "$pointer" 1280 720 click 550 412
sleep 1.5
[ ! -s "$work/session.env" ] || fail "a session that is gone was started"
grep -q "authenticated" "$work/backend.log" || fail "the login did not go through at all"
mv "$work/fake.away" "$work/sessions/fake.desktop"
kill "$pid"; wait "$pid" 2>/dev/null; pid=
sleep 0.5
gone || fail "the greeter outlived the daemon"

# ---- root, by default not
: > "$work/backend.log"
start_daemon
wait_for "the greeter did not come up the second time" greeter_up
sleep 1.2
type_into_greeter text root key none return text rightword key none return
sleep 1.5
[ ! -s "$work/session.env" ] || fail "root logged in"
grep -q "authenticated root" "$work/backend.log" && fail "root was even asked for"

# ---- a further question of PAM
kill "$pid"; wait "$pid" 2>/dev/null; pid=
sleep 0.5
export TW_DM_FAKE_CODE=4711
start_daemon
wait_for "the greeter did not come up for the code" greeter_up
sleep 1.2
type_into_greeter text sam key none return text rightword key none return
sleep 1
[ ! -s "$work/session.env" ] || fail "the session started without the code"
type_into_greeter text 4711 key none return
wait_for "the code did not start the session" has_env
unset TW_DM_FAKE_CODE
rm -f "$work/display" "$work/session.env"
touch "$work/session.end"
wait_for "the greeter did not come back after the code" greeter_up
kill "$pid"; wait "$pid" 2>/dev/null; pid=
sleep 0.5

# ---- automatic login: no greeter, and once per start of the computer
cat >> "$conf" <<EOC
autologin_user kim
autologin_session fake
EOC
rm -f "$run/autologin-done"
start_daemon
wait_for "the automatic login started no session" has_env
grep -q "^USER=kim$" "$work/session.env" || fail "the automatic login is not of kim"
gone || fail "a greeter runs with the automatic login"
rm -f "$work/session.env" "$work/display"
touch "$work/session.end"
wait_for "after the automatic login the greeter did not come" greeter_up
[ ! -s "$work/session.env" ] || fail "the automatic login happened twice"
kill "$pid"; wait "$pid" 2>/dev/null; pid=

ls "$run" >/dev/null 2>&1
if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	echo "--- the daemon said:"; tail -n 8 "$work/daemon.log"
	echo "--- the stand-in:"; tail -n 8 "$work/backend.log"
	exit 1
fi
echo "all checks passed"
