#!/bin/sh
# tilewin-update with stand-ins for GitHub, git, the installer and the password:
#  source:
#  - the repository is pulled into a temporary folder, installed from there with
#    --no-deps, tileWin is restarted after that, and the folder is gone;
#  - when the build fails there is no restart, a failure is reported, and the
#    folder is gone all the same;
#  - without a running tileWin there is nothing to restart;
#  release:
#  - nothing happens when the release is what is installed, --force goes on;
#  - the archive lands in place, checked against its sum, then the restart;
#  - an archive with a wrong sum installs nothing;
#  - an archive this system lacks libraries for, and a release with no archive,
#    are built from the source of that tag;
#  check says what is installed and the newest release;
#  built for a package manager it refuses both ways, and check still works.
#
# usage: update.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
update=$build/tilewin-update
if [ ! -x "$update" ]; then
	echo "tilewin-update was not built: skipping"
	exit 77
fi
version=$(sed -n "s/^VERSION='\(.*\)'/\1/p" "$update")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}

mkdir -p "$work/bin" "$work/tmp" "$work/net"
export TMPDIR=$work/tmp
export TILEWIN_UPDATE_PREFIX=/usr/local
export TILEWIN_UPDATE_API=file://$work/net/latest.json
export TILEWIN_UPDATE_GIT=https://example.invalid/tileWin.git
export TILEWIN_UPDATE_ROOT=$work/root

# git: "clones" a folder holding an install.sh that says how it was called
cat > "$work/bin/git" <<EOS
#!/bin/sh
echo "git \$*" >> "$work/log"
for last; do :; done
mkdir -p "\$last"
cat > "\$last/install.sh" <<'EOI'
#!/bin/sh
echo "install \$PWD \$*" >> "\$UPDATE_LOG"
echo "sudo=\$TILEWIN_SUDO" >> "\$UPDATE_LOG"
[ ! -e "\$UPDATE_FAIL" ]
EOI
chmod +x "\$last/install.sh"
EOS
# tilewinmsg: running or not, and restarts written down
cat > "$work/bin/tilewinmsg" <<EOS
#!/bin/sh
[ -e "$work/running" ] || exit 1
case "\$*" in
"-t get_version") echo '{}' ;;
restart) echo restart >> "$work/log" ;;
esac
EOS
# curl: file:// addresses work as ever; what is asked for is written down
real_curl=$(command -v curl)
if [ -z "$real_curl" ]; then
	echo "curl is not installed: skipping"
	exit 77
fi
cat > "$work/bin/curl" <<EOS
#!/bin/sh
echo "curl \$*" >> "$work/curl.log"
exec "$real_curl" "\$@"
EOS
cat > "$work/bin/ldd" <<EOS
#!/bin/sh
[ -e "$work/no-libs" ] && echo '	libwlroots-0.20.so => not found'
exit 0
EOS
# no terminal here: pkexec is what gets asked, and it runs the thing as it is
cat > "$work/bin/pkexec" <<EOS
#!/bin/sh
echo "pkexec \$1" >> "$work/log"
exec "\$@"
EOS
cat > "$work/bin/sudo" <<EOS
#!/bin/sh
echo "sudo \$1" >> "$work/log"
exec "\$@"
EOS
chmod +x "$work/bin/"*
PATH=$work/bin:$PATH
export PATH UPDATE_LOG=$work/log UPDATE_FAIL=$work/fail

left() { find "$work/tmp" -mindepth 1 -maxdepth 1 | wc -l; }
run() { "$update" "$@" </dev/null > "$work/out" 2>&1; }

# ---- source
touch "$work/running"
: > "$work/log"
run source || fail "a good source update failed: $(cat "$work/out")"
grep -q "^git clone .*--depth 1 .*example.invalid/tileWin.git $work/tmp/tilewin-update\." "$work/log" ||
	fail "the repository did not go into a temporary folder: $(cat "$work/log")"
grep -q "^install $work/tmp/tilewin-update\.[^/]*/tileWin --no-deps --prefix /usr/local" "$work/log" ||
	fail "it was not installed from there: $(cat "$work/log")"
grep -q "^sudo=pkexec" "$work/log" || fail "without a terminal pkexec does not ask: $(cat "$work/log")"
[ "$(tail -n 1 "$work/log")" = restart ] || fail "tilewinmsg restart is not the last thing: $(cat "$work/log")"
[ "$(left)" = 0 ] || fail "the temporary folder is still there: $(ls "$work/tmp")"

touch "$work/fail"
: > "$work/log"
if run source; then
	fail "a failed build counts as a good update"
fi
grep -q restart "$work/log" && fail "tileWin was restarted after a failed build"
[ "$(left)" = 0 ] || fail "after a failed build the temporary folder is still there"
rm -f "$work/fail"

rm -f "$work/running"
: > "$work/log"
run source || fail "without a running tileWin the update failed: $(cat "$work/out")"
grep -q restart "$work/log" && fail "a restart without a running tileWin"
touch "$work/running"
: > "$work/log"
run source --no-restart || fail "--no-restart failed"
grep -q restart "$work/log" && fail "--no-restart restarted"

# ---- release
archive_name=tilewin-99.0-$(uname -m)-linux.tar.gz
make_release() { # <tag> [no-archive]
	tree=$work/net/tree
	rm -rf "$tree"
	mkdir -p "$tree/tilewin/stage/usr/local/bin" "$tree/tilewin/data"
	printf '#!/bin/sh\necho new\n' > "$tree/tilewin/stage/usr/local/bin/tilewin"
	chmod +x "$tree/tilewin/stage/usr/local/bin/tilewin"
	cp "$source_dir/data/tilewin.desktop" "$source_dir/data/"*.pam "$tree/tilewin/data/"
	cp "$source_dir/scripts/tilewin-system-setup" "$tree/tilewin/"
	tar -C "$tree" -czf "$work/net/$archive_name" tilewin
	(cd "$work/net" && sha256sum "$archive_name" > SHA256SUMS)
	{
		printf '{\n  "tag_name": "%s",\n  "assets": [\n' "$1"
		if [ "${2:-}" != no-archive ]; then
			printf '    { "browser_download_url": "file://%s/net/%s" },\n' "$work" "$archive_name"
			printf '    { "browser_download_url": "file://%s/net/SHA256SUMS" }\n' "$work"
		fi
		printf '  ]\n}\n'
	} > "$work/net/latest.json"
}

make_release "v$version"
: > "$work/log"; : > "$work/curl.log"; rm -rf "$work/root"
run release || fail "being up to date is no failure: $(cat "$work/out")"
grep -q "newest release already" "$work/out" || fail "it does not say it is up to date: $(cat "$work/out")"
grep -q "$archive_name" "$work/curl.log" && fail "it downloaded what it has"
[ -e "$work/root" ] && fail "it installed what it has"

run release --force || fail "--force failed: $(cat "$work/out")"
[ -x "$work/root/usr/local/bin/tilewin" ] || fail "--force installed nothing: $(cat "$work/out")"

make_release v99.0
: > "$work/log"; rm -rf "$work/root"
run release || fail "a good release update failed: $(cat "$work/out")"
[ -x "$work/root/usr/local/bin/tilewin" ] || fail "the archive did not land in place: $(cat "$work/out")"
[ -s "$work/root/usr/local/share/tileWin/install-log.txt" ] || fail "the list for --uninstall is missing"
grep -q "^pkexec sh" "$work/log" || fail "the archive was not installed through pkexec: $(cat "$work/log")"
[ "$(tail -n 1 "$work/log")" = restart ] || fail "no restart after the release: $(cat "$work/log")"
[ "$(left)" = 0 ] || fail "after a release the temporary folder is still there"
"$update" check </dev/null > "$work/out" 2>&1
{ grep -q "^installed $version$" "$work/out" && grep -q "^release 99.0$" "$work/out" &&
	grep -q "^archive yes$" "$work/out"; } || fail "check says: $(cat "$work/out")"

# a wrong sum: nothing lands
echo broken >> "$work/net/$archive_name"
: > "$work/log"; rm -rf "$work/root"
if run release; then
	fail "an archive with a wrong sum counts as a good update"
fi
grep -q "checksum" "$work/out" || fail "it does not name the checksum: $(cat "$work/out")"
[ -e "$work/root" ] && fail "an archive with a wrong sum was installed"
grep -q restart "$work/log" && fail "a restart after a wrong sum"
[ "$(left)" = 0 ] || fail "after a wrong sum the temporary folder is still there"

# libraries missing here: the source of that tag
make_release v99.0
touch "$work/no-libs"
: > "$work/log"; rm -rf "$work/root"
run release || fail "the way round through the source failed: $(cat "$work/out")"
grep -q "^git clone .*--branch v99.0 " "$work/log" || fail "it did not take the source of the tag: $(cat "$work/log")"
[ -e "$work/root" ] && fail "an archive that cannot run here was installed"
[ "$(left)" = 0 ] || fail "after the way round the temporary folder is still there"
rm -f "$work/no-libs"

# no archive at all: the same
make_release v99.0 no-archive
: > "$work/log"
run release || fail "a release without an archive failed: $(cat "$work/out")"
grep -q "^git clone .*--branch v99.0 " "$work/log" || fail "without an archive it did not build the tag: $(cat "$work/log")"

# owned by a package manager: it says so, touches nothing, and check still works
sed "s/^MANAGED=.*/MANAGED='yes'/" "$update" > "$work/update-managed"
chmod +x "$work/update-managed"
for way in release source; do
	: > "$work/log"; rm -rf "$work/root"
	if "$work/update-managed" "$way" </dev/null > "$work/out" 2>&1; then
		fail "a packaged install took the $way way"
	fi
	grep -q "package manager" "$work/out" || fail "it does not name the package manager: $(cat "$work/out")"
	[ ! -s "$work/log" ] || fail "a packaged install ran something: $(cat "$work/log")"
	[ -e "$work/root" ] && fail "a packaged install was overwritten"
done
"$work/update-managed" check </dev/null > "$work/out" 2>&1
grep -q "^managed yes$" "$work/out" && grep -q "^release 99.0$" "$work/out" ||
	fail "check of a packaged install says: $(cat "$work/out")"

# nonsense
if "$update" nonsense </dev/null >/dev/null 2>&1; then
	fail "a nonsense argument was taken"
fi
if "$update" </dev/null >/dev/null 2>&1; then
	fail "without a terminal and without a choice it went on"
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
