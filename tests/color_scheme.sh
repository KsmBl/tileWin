#!/bin/sh
# tilewin-color-scheme gives GTK 3 apps such as Thunar the dark look, also on a
# system with nothing but GTK's own Adwaita and no settings.ini yet:
#  - dark: the GTK theme becomes Adwaita-dark, made from the dark Adwaita
#    built into GTK, and settings.ini (made when missing) prefers dark
#  - light: back to Adwaita, not preferring dark
#  - an Adwaita-dark folder of the user's own is left alone
#  - GTK 3 draws a window of the made theme dark (when PyGObject has GTK 3)
# gsettings is a stand-in that keeps its values in a folder.
#
# usage: color_scheme.sh <build dir>
set -u

script=$1/tilewin-color-scheme
if [ ! -f "$script" ]; then
	echo "$script was not built: skipping"
	exit 77
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work/bin" "$work/gs"
cat > "$work/bin/gsettings" <<'GS'
#!/bin/sh
case "$1" in
list-keys) exit 0 ;;
get) cat "$FAKE_GS/$3" 2>/dev/null || echo "'Adwaita'" ;;
set) echo "'$4'" > "$FAKE_GS/$3" ;;
esac
GS
chmod +x "$work/bin/gsettings"

failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
run() {
	PATH=$work/bin:$PATH FAKE_GS=$work/gs HOME=$work/home \
		XDG_CONFIG_HOME=$work/config XDG_DATA_HOME=$work/data sh "$script" "$1"
}
setting() { tr -d "'" < "$work/gs/$1"; }
prefer() { sed -n 's/^gtk-application-prefer-dark-theme=//p' "$work/config/gtk-3.0/settings.ini"; }

run dark || fail "dark: the script failed"
echo "dark: gtk-theme $(setting gtk-theme), color-scheme $(setting color-scheme), prefer $(prefer)"
[ "$(setting gtk-theme)" = Adwaita-dark ] || fail "dark: the GTK theme is not Adwaita-dark"
[ "$(setting color-scheme)" = prefer-dark ] || fail "dark: color-scheme is not prefer-dark"
[ "$(prefer)" = true ] || fail "dark: settings.ini was not made preferring dark"
grep -q 'gtk-contained-dark.css' "$work/data/themes/Adwaita-dark/gtk-3.0/gtk.css" 2>/dev/null ||
	fail "dark: no Adwaita-dark theme for GTK 3 was made"

if python3 -c 'import gi; gi.require_version("Gtk", "3.0")' 2>/dev/null &&
		[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
	bg=$(XDG_DATA_HOME=$work/data GTK_THEME=Adwaita-dark python3 -I -W ignore -c '
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
c = Gtk.Window().get_style_context().get_background_color(Gtk.StateFlags.NORMAL)
print(int((c.red + c.green + c.blue) / 3 * 255))' 2>/dev/null)
	echo "GTK 3 window background with it: $bg of 255"
	[ -n "$bg" ] && [ "$bg" -lt 100 ] || fail "GTK 3 does not draw the made theme dark"
else
	echo "(no GTK 3 for Python or no display: the look itself is not checked)"
fi

run light || fail "light: the script failed"
echo "light: gtk-theme $(setting gtk-theme), color-scheme $(setting color-scheme), prefer $(prefer)"
[ "$(setting gtk-theme)" = Adwaita ] || fail "light: the GTK theme is not Adwaita again"
[ "$(setting color-scheme)" = default ] || fail "light: color-scheme is not default"
[ "$(prefer)" = false ] || fail "light: settings.ini still prefers dark"

# a folder of the user's own is not written over
rm -rf "$work/data/themes/Adwaita-dark"
mkdir -p "$work/data/themes/Adwaita-dark/gtk-3.0"
echo '/* mine */' > "$work/data/themes/Adwaita-dark/gtk-3.0/gtk.css"
run dark || fail "dark again: the script failed"
grep -q 'mine' "$work/data/themes/Adwaita-dark/gtk-3.0/gtk.css" ||
	fail "the user's own Adwaita-dark was written over"

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all passed"
