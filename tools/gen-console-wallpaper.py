#!/usr/bin/env python3
"""Draws the wallpaper of the Starship Console theme.

    tools/gen-console-wallpaper.py      writes themes/console/wallpaper.svg

Deep space with stars and the glowing limb of a planet, a panel of readouts in
a corner and the faint rings of a sensor; the windows bring the bars. An original design; the numbers are
random. The file is committed, so building tileWin does not need Python.
"""
import math
import os
import random

W, H = 1920, 1080
COLORS = ['#ff9c3a', '#ffcf99', '#c896d8', '#8f9bff', '#8fc8ff', '#e8c070', '#d4707a']
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


def number():
    return '%02d-%04d' % (rng.randrange(100), rng.randrange(10000))


# deep space
out.append('<defs>'
           '<radialGradient id="space" cx="0.65" cy="0.55" r="0.9">'
           '<stop offset="0" stop-color="#0b0d1e"/><stop offset="1" stop-color="#000000"/>'
           '</radialGradient>'
           '<radialGradient id="planet" cx="0.35" cy="0.25" r="0.85">'
           '<stop offset="0" stop-color="#3a2f5e"/><stop offset="0.6" stop-color="#17132a"/>'
           '<stop offset="1" stop-color="#05040a"/></radialGradient>'
           '<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5">'
           '<stop offset="0.86" stop-color="#8f9bff" stop-opacity="0"/>'
           '<stop offset="0.93" stop-color="#8fc8ff" stop-opacity="0.35"/>'
           '<stop offset="1" stop-color="#8f9bff" stop-opacity="0"/></radialGradient>'
           '</defs>')
out.append('<rect width="%d" height="%d" fill="url(#space)"/>' % (W, H))
for _ in range(420):
    x, y = rng.uniform(0, W), rng.uniform(0, H)
    r = rng.choice([0.6, 0.8, 1, 1, 1.3, 1.7])
    out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#ffffff" opacity="%.2f"/>'
               % (x, y, r, rng.uniform(0.25, 0.95)))
# the planet, low on the right, its atmosphere glowing
cx, cy, pr = 1640, 1320, 700
out.append('<circle cx="%d" cy="%d" r="%d" fill="url(#glow)"/>' % (cx, cy, pr * 1.12))
out.append('<circle cx="%d" cy="%d" r="%d" fill="url(#planet)"/>' % (cx, cy, pr))
out.append('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#ffcf99" stroke-opacity="0.35" '
           'stroke-width="2"/>' % (cx, cy, pr))

# readouts on the right, under the top bar
rx, ry = 1490, 90
for row in range(8):
    xx = rx
    for col in range(5):
        s = '%0*d' % (rng.choice([2, 3, 4]), rng.randrange(10000))
        text(xx, ry + row * 24, s, 17, COLORS[rng.randrange(7)], anchor='start', weight='normal')
        xx += 78
# a small row of blocks above them, as a button bar
bx = 1490
for i in range(4):
    block(bx, 56, 86, 16, COLORS[(i + 3) % 7], left=i == 0, right=i == 3)
    bx += 92

# the rings of a sensor display, faint, on the left
sx, sy = 440, 690
for r in (60, 120, 180, 240):
    out.append('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#c896d8" stroke-opacity="0.18" '
               'stroke-width="2"/>' % (sx, sy, r))
out.append('<path d="M%d %d L%.1f %.1f" stroke="#ff9c3a" stroke-opacity="0.35" stroke-width="3"/>'
           % (sx, sy, sx + 240 * math.cos(-0.6), sy + 240 * math.sin(-0.6)))
for _ in range(9):
    a, r = rng.uniform(0, 2 * math.pi), rng.uniform(30, 230)
    out.append('<circle cx="%.1f" cy="%.1f" r="4" fill="#ffcf99" opacity="0.5"/>'
               % (sx + r * math.cos(a), sy + r * math.sin(a)))

svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
       '<title>tileWin Starship Console wallpaper: space, a planet and the bars of a console '
       '(original artwork)</title>\n%s\n</svg>\n' % (W, H, W, H, '\n'.join(out)))
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(root, 'themes', 'console', 'wallpaper.svg'), 'w') as f:
    f.write(svg)
print('themes/console/wallpaper.svg')
