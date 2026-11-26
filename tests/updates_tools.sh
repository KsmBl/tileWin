#!/bin/bash
# tilewin-updates and tilewin-updates-root, the two scripts behind the updates
# widget, with stand-ins for checkupdates, pacman, paccache, apt, dnf,
# systemctl, pkexec and notify-send that write down what they were asked:
#
#  - checking: the updates found and counted, none, a failure, pacman without
#    pacman-contrib; apt and dnf read as well
#  - notifying once of updates not seen before, not with --quiet
#  - installing: the bar from pacman's lines, checking again afterwards, a
#    restart asked for when the kernel was updated; the password not given; a
#    failure with pacman's reason
#  - the package cache: its size, cleaning it, the weekly clean-up on and off
#  - the root helper doing nothing but its commands; one at a time
#
# usage: updates_tools.sh <tilewin-updates> <tilewin-updates-root>
set -u

tool=$1
root_tool=$2
for needed in flock awk du cksum; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done

work=$(mktemp -d)
[ -n "${UPDATES_TEST_KEEP:-}" ] && echo "files in $work" || trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
ok() { echo "ok   $1"; }
check() { # description, condition (shell, evaluated here)
	if eval "$2"; then ok "$1"; else fail "$1"; fi
}

export HOME=$work/home XDG_STATE_HOME=$work/home/state
mkdir -p "$HOME"
state=$XDG_STATE_HOME/tileWin/updates
bin=$work/bin
asked=$work/asked
mkdir -p "$bin" "$work/cache"
: > "$asked"

# ---------- stand-ins ----------

stub() { # name, body
	printf '#!/bin/bash\n%s\n' "$2" > "$bin/$1"
	chmod +x "$bin/$1"
}
# what checkupdates answers comes from a file: its exit code on the first line
answer() { # exit code, lines...
	local rc=$1
	shift
	{ echo "$rc"; printf '%s\n' "$@"; } > "$work/checkupdates.answer"
}
stub checkupdates "rc=\$(head -1 '$work/checkupdates.answer'); tail -n +2 '$work/checkupdates.answer'; exit \$rc"
stub pacman "echo \"pacman \$*\" >> '$asked'
if [ -f '$work/pacman.fail' ]; then echo 'error: failed to commit transaction (conflicting files)'; exit 1; fi
echo ':: Synchronizing package databases...'
echo ':: Retrieving packages...'
echo '(1/2) upgrading linux'
echo '(2/2) upgrading firefox'
echo 2 > '$work/checkupdates.answer'"
stub paccache "echo \"paccache \$*\" >> '$asked'; rm -f '$work/cache/'*"
stub systemctl "echo \"systemctl \$*\" >> '$asked'
case \"\$*\" in
	'enable --now paccache.timer') touch '$work/timer.on' ;;
	'disable --now paccache.timer') rm -f '$work/timer.on' ;;
	*is-enabled*) [ -f '$work/timer.on' ] ;;
esac"
stub notify-send "echo \"notify \$*\" >> '$asked'"
# pkexec: runs the helper as it is, or says the password was not given
stub pkexec "[ -f '$work/pkexec.dismiss' ] && exit 126; exec bash \"\$@\""

export PATH=$bin:$PATH
export TILEWIN_UPDATES_ROOT=$root_tool TILEWIN_UPDATES_TEST_PATH=$bin
export TILEWIN_UPDATES_MANAGER=pacman TILEWIN_UPDATES_CACHE=$work/cache
head -c 3000 /dev/zero > "$work/cache/firefox-133.0-1-x86_64.pkg.tar.zst"

value() { # key: from the status file
	sed -n "s/^$1=//p" "$state/status"
}
asked_for() { grep -qF -- "$1" "$asked"; }
notifications() { grep -c '^notify' "$asked"; }

# ---------- checking ----------

answer 0 "linux 7.3.5.arch1-1 -> 7.3.6.arch1-1" "firefox 133.0-1 -> 134.0-1"
bash "$tool" check
check "two updates found" '[ "$(value count)" = 2 ]'
check "the list: name, now, new" 'grep -qx $'"'"'firefox\t133.0-1\t134.0-1'"'"' "$state/list"'
check "idle after the check, with when" '[ "$(value state)" = idle ] && [ -n "$(value checked)" ]'
check "the size of the package cache" '[ "$(value cache)" -ge 3000 ]'
check "the weekly clean-up is off" '[ "$(value weekly)" = off ]'
check "notified of them" '[ "$(notifications)" = 1 ] && asked_for "2 updates are available"'
bash "$tool" check
check "not notified of the same updates again" '[ "$(notifications)" = 1 ]'
answer 0 "firefox 133.0-1 -> 134.0-2"
bash "$tool" check --quiet
check "--quiet: no notification of new updates" '[ "$(notifications)" = 1 ]'
answer 2
bash "$tool" check
check "none: counted as 0" '[ "$(value count)" = 0 ] && [ ! -s "$state/list" ]'
answer 1 "==> ERROR: Cannot fetch updates"
bash "$tool" check
check "a failed check is an error, with pacman's words" '[ "$(value state)" = error ] && [ "$(value message)" = "==> ERROR: Cannot fetch updates" ]'
mv "$bin/checkupdates" "$bin/checkupdates.off"
bash "$tool" check
check "without pacman-contrib it says what is missing" 'grep -q '"'"'message=pacman-contrib is needed'"'"' "$state/status"'
mv "$bin/checkupdates.off" "$bin/checkupdates"

# apt and dnf
stub apt "echo 'Listing...'; echo 'firefox-esr/stable-security 128.6.0esr-1~deb13u1 amd64 [upgradable from: 128.5.0esr-1~deb13u1]'"
TILEWIN_UPDATES_MANAGER=apt bash "$tool" check --quiet
check "apt: the upgradable packages" 'grep -qx $'"'"'firefox-esr\t128.5.0esr-1~deb13u1\t128.6.0esr-1~deb13u1'"'"' "$state/list"'
stub dnf "echo; echo 'firefox.x86_64          134.0-1.fc41          updates'; echo 'kernel-core.x86_64   6.12.9-200.fc41   updates'; exit 100"
TILEWIN_UPDATES_MANAGER=dnf bash "$tool" check --quiet
check "dnf: the packages without their architecture" '[ "$(value count)" = 2 ] && grep -qx $'"'"'kernel-core\t\t6.12.9-200.fc41'"'"' "$state/list"'

# ---------- installing ----------

answer 0 "linux 7.3.5.arch1-1 -> 7.3.6.arch1-1" "firefox 133.0-1 -> 134.0-1"
bash "$tool" check --quiet
: > "$asked"
bash "$tool" install
check "pacman -Syu --noconfirm, through pkexec" 'asked_for "pacman -Syu --noconfirm"'
check "what pacman printed is in the log" 'grep -q '"'"'(2/2) upgrading firefox'"'"' "$state/log"'
check "checked again afterwards: up to date" '[ "$(value count)" = 0 ] && [ "$(value state)" = idle ]'
check "the installation is noted" '[ -n "$(value installed)" ]'
check "the kernel was updated: a restart is asked for" '[ "$(value restart)" = yes ] && [ "$(value restart_boot)" = "$(cat /proc/sys/kernel/random/boot_id)" ]'
check "notified that a restart finishes it" 'asked_for "Restart the computer to finish"'
check "no bar left behind" '[ -z "$(value done)" ] && [ -z "$(value total)" ]'

answer 0 "firefox 133.0-1 -> 134.0-1"
bash "$tool" check --quiet
touch "$work/pkexec.dismiss"
bash "$tool" install
check "password not given: nothing done, no error" '[ "$(value state)" = idle ] && grep -q '"'"'message=Not installed: the password was not given'"'"' "$state/status"'
rm "$work/pkexec.dismiss"
touch "$work/pacman.fail"
bash "$tool" install
check "a failed installation is an error, with pacman's reason" '[ "$(value state)" = error ] && grep -q '"'"'message=error: failed to commit transaction'"'"' "$state/status"'
check "notified of the failure" 'asked_for "The updates could not be installed"'
rm "$work/pacman.fail"

# ---------- the package cache ----------

head -c 3000 /dev/zero > "$work/cache/firefox-133.0-1-x86_64.pkg.tar.zst"
: > "$asked"
bash "$tool" clean
check "cleaning keeps two versions, none of removed packages" 'asked_for "paccache -rk2" && asked_for "paccache -ruk0"'
check "the cache size after cleaning" '[ "$(value cache)" -lt 3000 ]'
bash "$tool" weekly on
check "weekly on: paccache.timer enabled" 'asked_for "systemctl enable --now paccache.timer"'
check "weekly on in the status" '[ "$(value weekly)" = on ]'
bash "$tool" weekly off
check "weekly off: disabled again" 'asked_for "systemctl disable --now paccache.timer" && [ "$(value weekly)" = off ]'

# ---------- the root helper and the lock ----------

TILEWIN_UPDATES_TEST_PATH=$bin bash "$root_tool" rm -rf / >/dev/null 2>&1
rc=$?
check "the root helper refuses what it does not know" '[ $rc = 2 ]'
TILEWIN_UPDATES_TEST_PATH=$bin bash "$root_tool" weekly sometimes >/dev/null 2>&1
rc=$?
check "weekly takes only on or off" '[ $rc != 0 ]'
(
	exec 9> "$state/lock"
	flock 9
	bash "$tool" check 2>&1 | grep -q 'already at work'
) && ok "a second one at the same time does nothing" || fail "a second one at the same time does nothing"

echo
if [ $failures -gt 0 ]; then
	echo "$failures failed"
	exit 1
fi
echo "all passed"
