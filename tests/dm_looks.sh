#!/bin/sh
# The login screen of each theme looks as that Windows did. Without a screen, with
# the picture the greeter renders for the settings:
#  - every theme gives a picture, and no two are alike;
#  - some things only that Windows had are where they belong: the blue bands of
#    XP and its orange line, the grey dialog of Windows 95 on its teal, the white
#    dialog of Windows 3, the red power button of Windows 7;
#  - card_style plain turns it all into a card, on any theme.
#
# usage: dm_looks.sh <build dir> <source dir>
set -u

greeter=$1/dm/tilewin-dm-greeter
source_dir=$2
if [ ! -x "$greeter" ]; then
	echo "$greeter was not built: skipping"
	exit 77
fi
if ! command -v python3 >/dev/null 2>&1 || ! python3 -I -c 'import PIL' 2>/dev/null; then
	echo "python3 with PIL is not there: skipping"
	exit 77
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
fail() {
	echo "FAIL: $1"
	failures=$((failures + 1))
}
export TILEWIN_DATADIR=$source_dir TILEWIN_DM_STATE=$work/none TILEWIN_DM_SESSIONS=$work/none
export HOME=$work
mkdir -p "$work/config/tileWin"
export XDG_CONFIG_HOME=$work/config
printf 'show_users no\ndefault_user sam\ncard_title "Log in"\n' > "$work/conf"

render() { # <theme> <conf> <picture>
	echo "$1" > "$work/config/tileWin/current-theme"
	echo light > "$work/config/tileWin/color-scheme"
	"$greeter" --render "$3" --size 1280x720 --config "$2" || fail "$1 gave no picture"
}

for dir in "$source_dir"/themes/*/; do
	theme=$(basename "$dir")
	render "$theme" "$work/conf" "$work/$theme.png"
done

python3 -I - "$work" <<'PY' || failures=$((failures + 1))
import glob, os, sys
from PIL import Image
work = sys.argv[1]
pictures = {os.path.basename(p)[:-4]: Image.open(p).convert('RGB') for p in glob.glob(work + '/*.png')}
bad = 0
def fail(text):
    global bad
    print('FAIL:', text)
    bad += 1
names = sorted(pictures)
for i, a in enumerate(names):
    for b in names[i + 1:]:
        if pictures[a].tobytes() == pictures[b].tobytes():
            fail(a + ' and ' + b + ' are the same picture')
def near(pixel, want, slack=24):
    return all(abs(x - y) <= slack for x, y in zip(pixel, want))
def count(name, want, slack=12):
    return sum(1 for p in pictures[name].getdata() if near(p, want, slack))
# Windows XP: the dark blue bands above and below, and the orange line
xp = pictures['winxp']
if not near(xp.getpixel((640, 20)), (0, 48, 153)) or not near(xp.getpixel((640, 700)), (0, 48, 153)):
    fail('Windows XP has no blue bands: %s' % (xp.getpixel((640, 20)),))
if not near(xp.getpixel((640, 623)), (255, 173, 64), 50):
    fail('Windows XP has no orange line: %s' % (xp.getpixel((640, 623)),))
# Windows 95: a grey dialog on the teal, with the navy of its title bar
w95 = pictures['win95']
if not near(w95.getpixel((20, 20)), (0, 128, 128)):
    fail('Windows 95 is not on teal: %s' % (w95.getpixel((20, 20)),))
if not near(w95.getpixel((640, 460)), (192, 192, 192)) and not near(w95.getpixel((440, 420)), (192, 192, 192)):
    fail('Windows 95 has no grey dialog')
if count('win95', (0, 0, 128), 20) < 200:
    fail('Windows 95 has no navy title bar')
# Windows 2000: the same, with the banner on top that 95 does not have
if count('win2000', (13, 33, 107), 30) < 100:
    fail('Windows 2000 has no banner')
# Windows 3: a white dialog with a black frame
if count('win3', (255, 255, 255), 2) < 40000:
    fail('Windows 3 has no white dialog')
# Windows 7: the red of its power button, which nothing else there has
if count('win7', (201, 51, 33), 40) < 60:
    fail('Windows 7 has no red power button')
if count('win10', (201, 51, 33), 40) > 60:
    fail('Windows 10 has a red power button')
sys.exit(1 if bad else 0)
PY

# a card, whichever the theme
printf 'show_users no\ndefault_user sam\ncard_title "Log in"\ncard_style plain\nclock none\n' > "$work/plain"
render win7 "$work/plain" "$work/card7.tmp"
render winxp "$work/plain" "$work/cardxp.tmp"
python3 -I - "$work/card7.tmp" "$work/winxp.png" "$work/cardxp.tmp" <<'PY' || failures=$((failures + 1))
import sys
from PIL import Image
xp = Image.open(sys.argv[2]).convert('RGB')
card = Image.open(sys.argv[3]).convert('RGB')
if card.getpixel((640, 20)) == xp.getpixel((640, 20)):
    print('FAIL: card_style plain on Windows XP still has its bands')
    sys.exit(1)
PY

if [ "$failures" -gt 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
echo "all checks passed"
