#!/bin/sh
# Copying the text in a part of the screen (the "Text" button of the snipping
# toolbar, "tilewin-snip text"), with fake slurp, grim, tesseract, wl-copy and
# notify-send (the real ones would read the screen and fill the clipboard):
#  - the text tesseract reads goes to the clipboard and into a notification,
#    read in English and the session's language when tesseract has it;
#  - without tesseract a notification says to install it;
#  - in a nested tileWin, the Text button of the toolbar runs "tilewin-snip text".
#
# usage: snip_text.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
snip=$source_dir/scripts/tilewin-snip.in
compositor=$build/sway/tilewin
msg=$build/swaymsg/tilewinmsg
panel=$build/panel/tilewin-panel
pointer=$build/tests/pointer-tool

work=$(mktemp -d)
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
sock=
cleanup() {
	[ -n "$sock" ] && "$msg" -s "$sock" exit >/dev/null 2>&1 && sleep 0.5
	rm -rf "$work" ${sock:+"$sock"}
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# the fakes
mkdir -p "$work/bin" "$work/tools" "$work/notess"
cat > "$work/bin/slurp" <<'EOF2'
#!/bin/sh
echo "10,10 100x50"
EOF2
cat > "$work/bin/grim" <<'EOF2'
#!/bin/sh
for last; do :; done
echo picture > "$last"
EOF2
cat > "$work/bin/tesseract" <<EOF2
#!/bin/sh
if [ "\$1" = --list-langs ]; then
	echo "List of available languages (3):"
	printf 'deu\neng\nosd\n'
	exit 0
fi
echo "\$*" >> "$work/tesseract.log"
printf 'Hello world\nsecond line\n\n'
EOF2
cat > "$work/bin/wl-copy" <<EOF2
#!/bin/sh
cat > "$work/clipboard"
EOF2
cat > "$work/bin/notify-send" <<EOF2
#!/bin/sh
echo "\$*" >> "$work/notify.log"
EOF2
chmod +x "$work"/bin/*
# a PATH with only the tools the script needs, for the run without tesseract
for tool in sh sed head cut tr mktemp grep tail sleep rm cat date mkdir; do
	path=$(command -v "$tool") && ln -s "$path" "$work/tools/$tool"
done
for fake in slurp grim wl-copy notify-send; do
	ln -s "$work/bin/$fake" "$work/notess/$fake"
done

PATH=$work/bin:$PATH LANG=de_DE.UTF-8 sh "$snip" text > /dev/null 2>&1
[ "$(cat "$work/clipboard" 2>/dev/null)" = "$(printf 'Hello world\nsecond line')" ] ||
	fail "the text read is not in the clipboard: $(cat "$work/clipboard" 2>/dev/null)"
grep -q -- "-l deu+eng" "$work/tesseract.log" 2>/dev/null ||
	fail "a German session does not read German and English: $(cat "$work/tesseract.log" 2>/dev/null)"
grep -q "Text copied to the clipboard Hello world" "$work/notify.log" 2>/dev/null ||
	fail "no notification with the first line: $(cat "$work/notify.log" 2>/dev/null)"

rm -f "$work/notify.log"
PATH=$work/notess:$work/tools sh "$snip" text > /dev/null 2>&1
grep -q "install tesseract" "$work/notify.log" 2>/dev/null ||
	fail "without tesseract there is no notification to install it"

# the Text button of the toolbar, in a nested tileWin
skip_gui=
for needed in "$compositor" "$msg" "$panel" "$pointer"; do
	[ -x "$needed" ] || skip_gui=yes
done
command -v dbus-run-session >/dev/null 2>&1 || skip_gui=yes
[ -n "${XDG_RUNTIME_DIR:-}" ] || skip_gui=yes
if [ -n "$skip_gui" ]; then
	echo "no nested session: the toolbar is not tried"
else
	sock=$XDG_RUNTIME_DIR/tw-sniptext-$$.sock
	export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
	export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
	export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1 GSETTINGS_BACKEND=memory
	mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
		"$work/snipbin"
	cat > "$work/snipbin/tilewin-snip" <<EOF2
#!/bin/sh
echo "\$*" >> "$work/snip.log"
EOF2
	chmod +x "$work/snipbin/tilewin-snip"
	cat > "$work/tilewin.conf" <<EOC
wallpaper solid #204070
session_restore no
animations off
panel_command $panel
output HEADLESS-1 mode --custom 1280x720 pos 0 0
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOC
	env -u WAYLAND_DISPLAY -u DISPLAY PATH="$work/snipbin:$PATH" \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
		WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
		dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &
	attempt=0
	while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
		sleep 0.2
		attempt=$((attempt + 1))
	done
	if [ ! -s "$work/display" ]; then
		fail "the nested compositor never came up"
	else
		export WAYLAND_DISPLAY=$(cat "$work/display")
		"$msg" -s "$sock" theme win10 >/dev/null
		sleep 2
		"$msg" -s "$sock" panel snip >/dev/null
		sleep 1
		# the fourth button of the toolbar in the middle at the top
		"$pointer" 1280 720 move 733 50 wait 300 click 733 50 >/dev/null 2>&1
		sleep 1
		grep -qx "text" "$work/snip.log" 2>/dev/null ||
			fail "the Text button does not run tilewin-snip text: $(cat "$work/snip.log" 2>/dev/null)"
	fi
fi

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
