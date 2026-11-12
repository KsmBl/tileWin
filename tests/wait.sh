# Waiting in the tests: for what is awaited, not for a fixed time, so a test
# goes on as soon as it can and still has time to spare on a busy computer.
# Sourced by the test scripts: . "$(dirname "$0")/wait.sh"
#
# The limits count tries, so a command that takes a while itself only makes
# the wait longer, never shorter.

# wait_until <seconds> <command> [args...]
# Runs the command every tenth of a second until it succeeds; fails when it
# has not within about that many seconds (whole seconds).
wait_until() {
	_wait_tries=$(($1 * 10))
	shift
	while ! "$@" >/dev/null 2>&1; do
		_wait_tries=$((_wait_tries - 1))
		[ "$_wait_tries" -gt 0 ] || return 1
		sleep 0.1
	done
}

# wait_for_file <seconds> <path>: until the file is there and not empty.
wait_for_file() {
	wait_until "$1" test -s "$2"
}

# wait_still <seconds> <command> [args...]
# Until the output of the command (e.g. the checksum of a screenshot) is the
# same twice in a row, a quarter of a second apart: something that has stopped
# drawing. Fails when it keeps changing for about that many seconds.
wait_still() {
	_still_tries=$(($1 * 4))
	shift
	_still_last=
	while [ "$_still_tries" -gt 0 ]; do
		_still_now=$("$@" 2>/dev/null)
		if [ -n "$_still_now" ] && [ "$_still_now" = "$_still_last" ]; then
			return 0
		fi
		_still_last=$_still_now
		_still_tries=$((_still_tries - 1))
		sleep 0.25
	done
	return 1
}
