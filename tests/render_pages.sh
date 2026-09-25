#!/bin/sh
# Opens every page of the settings app in a nested tileWin that draws into
# memory, and looks at what came out. A page that builds without a word of
# complaint can still lay itself out to nothing, which is what this catches.
#
# usage: render_pages.sh <build dir> <source dir>
set -u

build=$1
compositor=$build/sway/tilewin
settings=$build/settings/tilewin-settings
count_content=$build/tests/count-content

for needed in "$compositor" "$settings" "$count_content"; do
	if [ ! -x "$needed" ]; then
		echo "$needed was not built: skipping"
		exit 77
	fi
done
for needed in grim dbus-run-session pkill; do
	if ! command -v "$needed" >/dev/null 2>&1; then
		echo "$needed is not installed: skipping"
		exit 77
	fi
done
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	echo "no XDG_RUNTIME_DIR: skipping"
	exit 77
fi

pages="theme wallpaper desktop animations windows screen sound datetime bluetooth taskbar startmenu launcher keyboard mouse apps account about"

work=$(mktemp -d)
# killing the shell that starts the compositor does not kill the compositor, and
# a left over one holds on to an output; the config path is unique to this run
cleanup() {
	pkill -f "tilewin -c $work/" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# A session of its own, or it reads the config of whoever runs the test and
# brings back the apps their last session had open.
XDG_CONFIG_HOME=$work/config
XDG_STATE_HOME=$work/state
XDG_CACHE_HOME=$work/cache
XDG_DATA_HOME=$work/data
export XDG_CONFIG_HOME XDG_STATE_HOME XDG_CACHE_HOME XDG_DATA_HOME
mkdir -p "$XDG_CONFIG_HOME/tileWin" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"

# The desktop behind the window is one flat color, so that a window that never
# turned up cannot be mistaken for a page that drew itself.
cat > "$work/tilewin.conf" <<EOF
wallpaper solid #008080
session_restore no
exec "$settings" --page=PAGE
exec sh -c 'printf %s "\$WAYLAND_DISPLAY" > $work/display'
EOF

# The page sits right of the list of pages and below the heading. The area is
# kept well inside it, so that neither the desktop nor the window's own frame
# can be counted as something the page drew.
area_x=420
area_y=140
area_width=560
area_height=420
# The emptiest page still draws a few thousand pixels of rows, borders and
# text; a flat desktop, or a page that laid itself out to nothing, draws none
# at all, so anywhere in between tells the two apart.
least_content=1500

failures=0
for page in $pages; do
	sed "s|--page=PAGE|--page=$page|" "$work/tilewin.conf" > "$work/$page.conf"
	rm -f "$work/display"
	shot=$work/$page.png
	log=$work/$page.log
	env -u WAYLAND_DISPLAY -u DISPLAY \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
		SWAYSOCK="$XDG_RUNTIME_DIR/tilewin-render-$page.sock" \
		dbus-run-session -- "$compositor" -c "$work/$page.conf" > "$log" 2>&1 &
	starter=$!

	attempt=0
	while [ ! -s "$work/display" ] && [ $attempt -lt 60 ]; do
		sleep 0.5
		attempt=$((attempt + 1))
	done
	display=$(cat "$work/display" 2>/dev/null)
	if [ -z "$display" ]; then
		echo "$page: the nested compositor never came up"
		tail -n 2 "$log"
		failures=$((failures + 1))
		kill "$starter" 2>/dev/null
		pkill -f "tilewin -c $work/$page.conf" 2>/dev/null
		continue
	fi

	# One shot once everything has settled. Looking again until something
	# turns up would accept a half-drawn frame as a page.
	sleep 6
	content=0
	WAYLAND_DISPLAY=$display grim "$shot" 2>>"$log"
	if [ -s "$shot" ]; then
		content=$("$count_content" "$shot" $area_x $area_y $area_width $area_height)
	fi
	kill "$starter" 2>/dev/null
	pkill -f "tilewin -c $work/$page.conf" 2>/dev/null
	wait "$starter" 2>/dev/null
	sleep 1 # let the socket go before the next page asks for a new one

	if [ ! -s "$shot" ]; then
		echo "$page: no screenshot was taken"
		failures=$((failures + 1))
		continue
	fi
	if [ "$content" -lt "$least_content" ]; then
		echo "$page: only $content pixel(s) of the page area were drawn on"
		failures=$((failures + 1))
	else
		echo "$page: $content pixels drawn"
	fi
	if grep -q "CRITICAL\|assertion .* failed" "$log"; then
		echo "$page: GTK complained:"
		grep -m 3 "CRITICAL\|assertion" "$log"
		failures=$((failures + 1))
	fi
done

if [ $failures -gt 0 ]; then
	echo "$failures page(s) did not come out right"
	exit 1
fi
echo "every settings page drew itself"
