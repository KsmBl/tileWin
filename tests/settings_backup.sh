#!/bin/sh
# Backup and restore of the settings, as the Backup page of the settings app
# does them (tilewin-settings --backup/--restore run the same code):
#  - a backup brings back every file as it was, and only those;
#  - a restore keeps the settings it replaces, to go back to;
#  - an archive with files outside its tileWin folder, or with links, is
#    refused without touching anything.
#
# usage: settings_backup.sh <build dir>
set -u

settings=$1/settings/tilewin-settings
if [ ! -x "$settings" ]; then
	echo "$settings was not built: skipping"
	exit 77
fi
if ! command -v tar >/dev/null 2>&1; then
	echo "tar is not installed: skipping"
	exit 77
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export GSETTINGS_BACKEND=memory TILEWIN_NO_APP_TWEAKS=1
# no tileWin to reload: the restore must not need one
export TILEWINSOCK=$work/none.sock SWAYSOCK=$work/none.sock
unset WAYLAND_DISPLAY DISPLAY
conf=$XDG_CONFIG_HOME/tileWin
mkdir -p "$conf/themes/mine" "$XDG_STATE_HOME"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

printf 'set $term foot\n' > "$conf/common.conf"
printf 'font "Noto Sans 10"\n' > "$conf/taskbar.conf"
printf 'name "Mine"\n' > "$conf/themes/mine/theme.conf"
printf 'win95\n' > "$conf/current-theme"
before=$(cd "$conf" && find . -type f | sort | xargs md5sum)

"$settings" --backup "$work/backup.tar.gz" || fail "the backup could not be saved"
tar -tzf "$work/backup.tar.gz" | grep -qx "tileWin/common.conf" ||
	fail "the backup does not hold tileWin/common.conf"

# change everything: edit, remove and add files
printf 'set $term kitty\n' > "$conf/common.conf"
rm -r "$conf/themes"
printf 'new\n' > "$conf/added.conf"
changed=$(cd "$conf" && find . -type f | sort | xargs md5sum)

"$settings" --restore "$work/backup.tar.gz" > "$work/out" 2>&1 ||
	fail "the restore failed: $(cat "$work/out")"
after=$(cd "$conf" && find . -type f | sort | xargs md5sum)
[ "$after" = "$before" ] || fail "the restore did not bring back the settings as they were"

# the settings the restore replaced, to go back to
copy=$(ls "$XDG_STATE_HOME/tileWin/backups/"before-restore-*.tar.gz 2>/dev/null | head -n 1)
if [ -z "$copy" ]; then
	fail "the restore kept no copy of the settings it replaced"
else
	"$settings" --restore "$copy" >/dev/null 2>&1 || fail "going back did not work"
	back=$(cd "$conf" && find . -type f | sort | xargs md5sum)
	[ "$back" = "$changed" ] || fail "going back did not bring back the replaced settings"
fi

# archives that reach outside the tileWin folder, or hold links, are refused
state=$(cd "$conf" && find . -type f | sort | xargs md5sum)
mkdir -p "$work/evil/tileWin" "$work/evil/other"
printf 'x\n' > "$work/evil/tileWin/common.conf"
printf 'x\n' > "$work/evil/other/file"
tar -czf "$work/outside.tar.gz" -C "$work/evil" tileWin other
ln -s /etc/passwd "$work/evil/tileWin/link"
rm -r "$work/evil/other"
tar -czf "$work/link.tar.gz" -C "$work/evil" tileWin
printf 'not an archive\n' > "$work/junk.tar.gz"
for bad in outside link junk; do
	if "$settings" --restore "$work/$bad.tar.gz" >/dev/null 2>&1; then
		fail "the $bad archive was restored"
	fi
done
[ -e "$work/other" ] && fail "a file outside the tileWin folder was unpacked"
[ "$(cd "$conf" && find . -type f | sort | xargs md5sum)" = "$state" ] ||
	fail "a refused archive changed the settings"
[ -z "$(cd "$conf" && find . -type l)" ] || fail "a link was unpacked"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
