#!/usr/bin/env python3
"""Draws the wallpaper of the Starship Console theme.

    tools/gen-console-wallpaper.py      writes themes/console/wallpaper.svg

Deep space with stars, a faint nebula and the glowing limb of a planet; the
windows bring the bars, and the moving wallpaper of the taskbar (panel/livewall.c)
the sensor and the readouts. An original design; the numbers are
random. The file is committed, so building tileWin does not need Python.
"""
import math
import os
import random

W, H = 1920, 1080
COLORS = ['#a5a6ff', '#ffaa90', '#cc99cc', '#6688cc', '#99ccff', '#ffcc99', '#cc6699']
FONT = "Antonio, Oswald, 'Nimbus Sans Narrow', 'Liberation Sans Narrow', sans-serif"
rng = random.Random(1701)
out = []


def block(x, y, w, h, color, left=False, right=False):
    r = h / 2
    if left and right:
        out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s"/>'
                   % (x, y, w, h, r, color))
        return
    d = 'M%.1f %.1f' % (x + (r if left else 0), y)
    d += ' H%.1f' % (x + w - (r if right else 0))
    d += (' A%.1f %.1f 0 0 1 %.1f %.1f' % (r, r, x + w - r, y + h)) if right else \
        ' V%.1f' % (y + h)
    d += ' H%.1f' % (x + (r if left else 0))
    d += (' A%.1f %.1f 0 0 1 %.1f %.1f' % (r, r, x + r, y)) if left else ' V%.1f' % y
    out.append('<path d="%s Z" fill="%s"/>' % (d, color))


def text(x, y, s, size, color='#000000', anchor='end', weight='bold'):
    out.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="%d" font-weight="%s" '
               'fill="%s" text-anchor="%s">%s</text>' % (x, y, FONT, size, weight, color, anchor, s))


# deep space
out.append('<defs>'
           '<radialGradient id="space" cx="0.65" cy="0.55" r="0.9">'
           '<stop offset="0" stop-color="#0b0d1e"/><stop offset="1" stop-color="#000000"/>'
           '</radialGradient>'
           '<radialGradient id="planet" cx="0.35" cy="0.25" r="0.85">'
           '<stop offset="0" stop-color="#2c3a6e"/><stop offset="0.6" stop-color="#121a36"/>'
           '<stop offset="1" stop-color="#05040a"/></radialGradient>'
           '<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5">'
           '<stop offset="0.86" stop-color="#6688cc" stop-opacity="0"/>'
           '<stop offset="0.93" stop-color="#99ccff" stop-opacity="0.35"/>'
           '<stop offset="1" stop-color="#6688cc" stop-opacity="0"/></radialGradient>'
           '<radialGradient id="nebula" cx="0.5" cy="0.5" r="0.5">'
           '<stop offset="0" stop-color="#cc99cc" stop-opacity="0.16"/>'
           '<stop offset="0.5" stop-color="#6688cc" stop-opacity="0.06"/>'
           '<stop offset="1" stop-color="#000000" stop-opacity="0"/></radialGradient>'
           '</defs>')
out.append('<rect width="%d" height="%d" fill="url(#space)"/>' % (W, H))
# a faint nebula, high on the left
out.append('<ellipse cx="560" cy="300" rx="620" ry="300" fill="url(#nebula)" '
           'transform="rotate(-14 560 300)"/>')
for _ in range(420):
    x, y = rng.uniform(0, W), rng.uniform(0, H)
    r = rng.choice([0.6, 0.8, 1, 1, 1.3, 1.7])
    out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#ffffff" opacity="%.2f"/>'
               % (x, y, r, rng.uniform(0.25, 0.95)))
# the planet, low on the right, its atmosphere glowing
cx, cy, pr = 1640, 1320, 700
out.append('<circle cx="%d" cy="%d" r="%d" fill="url(#glow)"/>' % (cx, cy, pr * 1.12))
out.append('<circle cx="%d" cy="%d" r="%d" fill="url(#planet)"/>' % (cx, cy, pr))
out.append('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#a5a6ff" stroke-opacity="0.4" '
           'stroke-width="2"/>' % (cx, cy, pr))

svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
       '<title>tileWin Starship Console wallpaper: space, a planet and the bars of a console '
       '(original artwork)</title>\n%s\n</svg>\n' % (W, H, W, H, '\n'.join(out)))
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(root, 'themes', 'console', 'wallpaper.svg'), 'w') as f:
    f.write(svg)
print('themes/console/wallpaper.svg')
