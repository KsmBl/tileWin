#!/bin/sh
# The desktop styles of the widgets in every theme, light and dark. One
# desktop shows a card of each style: a chart, a gauge, a ring, a bar, the
# taskbar look, a tile, and a digital, an analog and a binary clock. For every
# theme and color scheme the test checks that
#  - each card is drawn, with more on it than a plain background;
#  - a theme with a light and a dark scheme draws the card darker in the dark
#    one (Windows 8 keeps its colored tiles, the sway theme is always dark);
#  - no two themes draw the chart the same.
# The panel must survive all of it.
#
# The grid is the default one: the card of column c and row r starts at
# 14 + 100 c, 14 + 100 r.
#
# usage: desktop_widget_styles.sh <build dir> <source dir>
set -u

build=$1
source_dir=$2
compositor=$build/sway/tilewin
panel=$build/panel/tilewin-panel
msg=$build/swaymsg/tilewinmsg
count=$build/tests/count-content

for needed in "$compositor" "$panel" "$msg" "$count"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session; do
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
sock=$XDG_RUNTIME_DIR/tw-styles-$$.sock
export XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state
export XDG_CACHE_HOME=$work/cache XDG_DATA_HOME=$work/data
export TILEWIN_DATADIR=$source_dir TILEWIN_NO_APP_TWEAKS=1
# switching the color scheme also tells GTK; keep that in memory
export GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
mkdir -p "$work/Desktop"
printf 'XDG_DESKTOP_DIR="%s/Desktop"\n' "$work" > "$XDG_CONFIG_HOME/user-dirs.dirs"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
cleanup() {
	"$msg" -s "$sock" exit >/dev/null 2>&1
	sleep 0.5
	rm -rf "$work" "$sock"
}
trap cleanup EXIT INT TERM

# name, column, row, columns, rows of every card
cards="chart 0 0 3 2
gauge 3 0 2 2
ring 5 0 2 2
bar 7 0 3 1
compact 7 1 2 1
analog 10 0 2 2
digital 0 2 3 2
binary 3 2 3 2
tile 6 2 2 2"

conf=$XDG_CONFIG_HOME/tileWin/taskbar.conf
cp "$source_dir/config/taskbar.conf" "$conf"
cat >> "$conf" <<EOF

theme_layout no
desktop_widgets {
	cpu { style chart; column 0; row 0 }
	memory { style gauge; column 3; row 0 }
	storage { style ring; column 5; row 0 }
	power { style bar; column 7; row 0 }
	gpu { style compact; column 7; row 1 }
	clock { style analog; column 10; row 0 }
	clock:digital { style digital; column 0; row 2 }
	clock:binary { style binary; column 3; row 2 }
	showdesktop { style tile; column 6; row 2 }
}
EOF

cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
panel_command $panel
animations off
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
	WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 SWAYSOCK="$sock" TILEWINSOCK="$sock" \
	dbus-run-session -- "$compositor" -c "$work/tilewin.conf" > "$work/log" 2>&1 &

attempt=0
while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
	sleep 0.5
	attempt=$((attempt + 1))
done
if [ ! -s "$work/display" ]; then
	echo "the nested compositor never came up"
	tail -n 3 "$work/log"
	exit 1
fi
export WAYLAND_DISPLAY=$(cat "$work/display")
ipc() { "$msg" -s "$sock" "$@"; }
ipc mode window >/dev/null 2>&1
sleep 3

panel_pid() {
	ipc -t get_tilewin | sed -n 's/.*"panel_pid": *\([0-9]*\).*/\1/p'
}
area() {
	echo "$((14 + 100 * $1)) $((14 + 100 * $2)) $((100 * $3 - 8)) $((100 * $4 - 8))"
}
started=$(panel_pid)

for theme in win95 winxp win7 win8 win10 win11 sway; do
	for scheme in light dark; do
		ipc theme "$theme" >/dev/null 2>&1
		ipc color_scheme "$scheme" >/dev/null 2>&1
		sleep 3
		shot=$work/$theme-$scheme.png
		grim "$shot"
		echo "$cards" | while read -r name col row columns rows; do
			# shellcheck disable=SC2046
			drawn=$("$count" "$shot" $(area "$col" "$row" "$columns" "$rows"))
			if [ "$drawn" -lt 150 ]; then
				echo "FAIL: $theme $scheme: the $name card shows next to nothing ($drawn pixels)"
			fi
		done > "$work/problems"
		if [ -s "$work/problems" ]; then
			cat "$work/problems"
			failures=$((failures + $(wc -l < "$work/problems")))
		fi
		# shellcheck disable=SC2046
		"$count" --mean "$shot" $(area 0 0 3 2) > "$work/$theme-$scheme.mean"
		echo "$theme $scheme: chart card $(cat "$work/$theme-$scheme.mean")"
	done
	# how bright the chart card is, light against dark
	light=$(awk '{ print int(($1 + $2 + $3) / 3) }' "$work/$theme-light.mean")
	dark=$(awk '{ print int(($1 + $2 + $3) / 3) }' "$work/$theme-dark.mean")
	case $theme in
	win8 | sway) ;;
	*)
		[ "$dark" -lt $((light - 20)) ] ||
			fail "$theme: the dark scheme is not darker ($dark against $light)"
		;;
	esac
done

# every theme has a look of its own
sort "$work"/*-light.mean | uniq -d > "$work/same"
if [ -s "$work/same" ]; then
	fail "two themes draw the chart card alike: $(cat "$work/same")"
fi

[ "$(panel_pid)" = "$started" ] || fail "the taskbar died and was started again"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
