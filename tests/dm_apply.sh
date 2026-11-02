#!/bin/sh
# tilewin-dm-apply with stand-ins for systemctl and the daemon's check:
#  - a good config replaces the old one, a bad one changes nothing;
#  - enable makes this the display manager and notes which one it was;
#  - disable puts that one back;
#  - status says which it is.
#
# usage: dm_apply.sh <build dir>
set -u

helper=$1/dm/tilewin-dm-apply
if [ ! -e "$helper" ]; then
	echo "tilewin-dm-apply was not built: skipping"
	exit 77
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

mkdir -p "$work/bin"
echo lightdm.service > "$work/current"
cat > "$work/bin/systemctl" <<EOS
#!/bin/sh
echo "systemctl \$*" >> "$work/calls"
case "\$*" in
"show -P Id display-manager.service") cat "$work/current" ;;
"enable --force "*) echo "\$3" > "$work/current" ;;
esac
exit 0
EOS
cat > "$work/bin/systemd-sysusers" <<EOS
#!/bin/sh
echo "sysusers \$*" >> "$work/calls"
EOS
cat > "$work/bin/check" <<'EOS'
#!/bin/sh
if grep -q nonsense "$2"; then
	echo "line 1: nonsense is no setting"
	exit 1
fi
EOS
chmod +x "$work/bin/"*

run() {
	# the helper sets its own PATH: the stand-ins come in through a copy of it
	sed "s|^export PATH=.*|export PATH=$work/bin:\$PATH|" "$helper" > "$work/helper"
	chmod +x "$work/helper"
	TILEWIN_DM_CONF=$work/etc/display-manager.conf TILEWIN_DM_STATE_DIR=$work/state \
		TILEWIN_DM_DAEMON=$work/bin/check "$work/helper" "$@"
}

conf=$work/etc/display-manager.conf
echo "theme win95" | run write || fail "a good config was refused"
[ "$(cat "$conf" 2>/dev/null)" = "theme win95" ] || fail "a good config did not land"
[ "$(stat -c %a "$conf")" = 644 ] || fail "the greeter cannot read the config"

if out=$(echo "nonsense 1" | run write 2>&1); then
	fail "a bad config was taken"
fi
case $out in
*"no setting"*) ;;
*) fail "the helper does not say what is wrong: $out" ;;
esac
[ "$(cat "$conf")" = "theme win95" ] || fail "a bad config changed the file"
[ "$(find "$work/etc" -type f | wc -l)" = 1 ] || fail "a temporary file was left behind"

[ "$(run status)" = disabled ] || fail "status with lightdm in use"
run enable || fail "enable failed"
[ "$(cat "$work/current")" = tilewin-display-manager.service ] || fail "enable did not take over"
[ "$(cat "$work/state/previous-display-manager")" = lightdm.service ] || fail "the one before is not noted"
grep -q "sysusers tilewin-dm.conf" "$work/calls" || fail "the user of the greeter was not made"
[ "$(run status)" = enabled ] || fail "status with this in use"

# again changes nothing of what is noted
run enable || fail "a second enable failed"
[ "$(cat "$work/state/previous-display-manager")" = lightdm.service ] || fail "a second enable lost the one before"

run disable || fail "disable failed"
[ "$(cat "$work/current")" = lightdm.service ] || fail "disable did not put the old one back"

if run nonsense 2>/dev/null; then
	fail "a nonsense word was taken"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
