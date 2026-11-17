#!/usr/bin/env python3
"""Draws the wallpapers of the modernCC theme.

    tools/gen-moderncc-wallpaper.py   writes themes/moderncc/wallpaper.svg and
                                      wallpaper-dark.svg

Soft fields of colour that run into each other, calm enough to work in front
of: indigo, teal and a warm peach on light grey, deeper on near black. Original
artwork; the files are committed, so building tileWin does not need Python.
"""
import os

W, H = 1920, 1080

SCHEMES = {
    'wallpaper.svg': ('#e9ecf5', [
        (380, 260, 760, '#8b97ff', 0.55), (1500, 220, 680, '#6fd3c4', 0.45),
        (1180, 900, 820, '#ffc7a8', 0.50), (320, 980, 600, '#c3a6ff', 0.40),
        (960, 520, 520, '#ffffff', 0.55)]),
    'wallpaper-dark.svg': ('#0d0e14', [
        (380, 260, 760, '#3a45b8', 0.60), (1500, 220, 680, '#0f6e66', 0.50),
        (1180, 900, 820, '#7a3f52', 0.45), (320, 980, 600, '#4b2f7a', 0.45),
        (960, 520, 520, '#1b1e2b', 0.60)]),
}


def svg(base, blobs):
    defs, shapes = [], []
    for i, (x, y, r, color, alpha) in enumerate(blobs):
        defs.append('<radialGradient id="b%d" cx="0.5" cy="0.5" r="0.5">'
                    '<stop offset="0" stop-color="%s" stop-opacity="%.2f"/>'
                    '<stop offset="0.55" stop-color="%s" stop-opacity="%.2f"/>'
                    '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
                    % (i, color, alpha, color, alpha * 0.45, color))
        shapes.append('<ellipse cx="%d" cy="%d" rx="%d" ry="%d" fill="url(#b%d)"/>'
                      % (x, y, r * 1.25, r, i))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
            '<title>tileWin modernCC wallpaper: soft fields of colour (original artwork)</title>\n'
            '<defs>%s</defs>\n<rect width="%d" height="%d" fill="%s"/>\n%s\n</svg>\n'
            % (W, H, W, H, ''.join(defs), W, H, base, '\n'.join(shapes)))


root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for name, (base, blobs) in SCHEMES.items():
    path = os.path.join(root, 'themes', 'moderncc', name)
    with open(path, 'w') as f:
        f.write(svg(base, blobs))
    print('themes/moderncc/' + name)
