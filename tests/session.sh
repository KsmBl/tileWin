#!/bin/bash
# dbus-run-session for the tests, that takes its bus and the services the bus
# started (xdg-desktop-portal, ...) along when it goes. dbus-run-session itself
# leaves them running when it is killed instead of waiting for its command,
# and the tests stop their compositor either way; hundreds of orphaned buses
# later the user runs out of inotify instances, and file watching stops
# working for everything.
#
# usage: session.sh <command> [args...]
set -u

address=$(mktemp)
dbus-daemon --session --nofork --print-address=3 3> "$address" &
bus=$!
for _ in $(seq 50); do
	[ -s "$address" ] && break
	sleep 0.1
done
DBUS_SESSION_BUS_ADDRESS=$(head -n 1 "$address")
export DBUS_SESSION_BUS_ADDRESS
rm -f "$address"

"$@" &
child=$!
trap 'kill "$child" 2>/dev/null' TERM INT HUP
wait "$child"
status=$?
# a signal only interrupts the first wait: wait for the command to be gone
while kill -0 "$child" 2>/dev/null; do
	wait "$child"
	status=$?
done
pkill -TERM -P "$bus" 2>/dev/null
kill "$bus" 2>/dev/null
wait "$bus" 2>/dev/null
exit $status
