#!/usr/bin/env python3
"""Generates the icon sets of the built-in tileWin themes.

    tools/gen-icons.py            writes themes/<theme>/icons/*.svg and aliases

Every icon is described once as simple shapes; each theme turns the shapes
into its own look (Windows 95: pixel grid with black outlines, XP: glossy
diagonal gradients, 7: glass highlights, 10: flat, 11: soft fluent
gradients). The generated files are committed, so building tileWin does not
need Python. All artwork is original.
"""
import math
import os
import re
import sys

THEMES = ['win95', 'winxp', 'win7', 'win10', 'win11']

STYLES = {
    'win95': dict(k=32 / 48, crisp=True, round=0, outline='black', grad=None,
                  shadow=False, gloss=False, stroke=1),
    'winxp': dict(k=1, crisp=False, round=1.0, outline='dark', grad='diag',
                  shadow=True, gloss=False, stroke=1.2),
    'win7': dict(k=1, crisp=False, round=0.7, outline='dark', grad='vert',
                 shadow=True, gloss=True, stroke=0.9),
    'win10': dict(k=1, crisp=False, round=0, outline=None, grad=None,
                  shadow=False, gloss=False, stroke=0),
    'win11': dict(k=1, crisp=False, round=1.4, outline='soft', grad='soft',
                  shadow=False, gloss=False, stroke=0.8),
}


# ---------------------------------------------------------------- colors

def rgb(c):
    c = c.lstrip('#')
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def hexc(t):
    return '#%02x%02x%02x' % tuple(max(0, min(255, round(v))) for v in t)


def mix(a, b, t):
    ra, rb = rgb(a), rgb(b)
    return hexc([ra[i] + (rb[i] - ra[i]) * t for i in range(3)])


def lighten(c, t):
    return mix(c, '#ffffff', t)


def darken(c, t):
    return mix(c, '#000000', t)


# ---------------------------------------------------------------- canvas

NUM = re.compile(r'-?\d+(?:\.\d+)?')


class Icon:
    def __init__(self, theme, tray=False):
        self.t = theme
        self.p = dict(STYLES[theme])
        # Windows 95 tray icons are 16 px pixel art
        self.size = 48
        if self.p['crisp']:
            self.size = 16 if tray else 32
            self.p['k'] = self.size / 48
        self.defs = []
        self.els = []
        self.ids = 0

    # coordinates -----------------------------------------------------
    def n(self, v, half=False):
        v = v * self.p['k']
        if self.p['crisp']:
            return round(v) + (0.5 if half else 0)
        return round(v, 2)

    def pts(self, points, half=False):
        return ' '.join('%s,%s' % (self.n(x, half), self.n(y, half)) for x, y in points)

    def uid(self, prefix):
        self.ids += 1
        return '%s%d' % (prefix, self.ids)

    # paint -----------------------------------------------------------
    def fill(self, color, flat=False, grad=None):
        if color is None:
            return 'none'
        g = grad if grad is not None else self.p['grad']
        if flat or not g:
            return color
        gid = self.uid('g')
        if g == 'diag':
            stops = [(0, lighten(color, .5)), (.5, color), (1, darken(color, .22))]
            coords = 'x1="0" y1="0" x2="1" y2="1"'
        elif g == 'vert':
            stops = [(0, lighten(color, .45)), (.48, color), (1, darken(color, .3))]
            coords = 'x1="0" y1="0" x2="0" y2="1"'
        else:
            stops = [(0, lighten(color, .22)), (1, darken(color, .08))]
            coords = 'x1="0" y1="0" x2="0" y2="1"'
        self.defs.append('<linearGradient id="%s" %s>%s</linearGradient>' % (
            gid, coords, ''.join('<stop offset="%s" stop-color="%s"/>' % s for s in stops)))
        return 'url(#%s)' % gid

    def stroke(self, color, outline=True, width=None):
        mode = self.p['outline']
        if not outline or not mode or color is None:
            return ''
        if mode == 'black':
            c, op = '#000000', 1
        elif mode == 'dark':
            c, op = darken(color, .55), 1 if self.t == 'winxp' else .8
        else:
            c, op = darken(color, .4), .5
        w = width if width is not None else self.p['stroke']
        return ' stroke="%s" stroke-width="%s" stroke-opacity="%s" stroke-linejoin="round"' % (c, w, op)

    def common(self, color, outline, flat, grad, opacity, clip):
        s = self.stroke(color, outline)
        attrs = ' fill="%s"%s' % (self.fill(color, flat, grad), s)
        if opacity != 1:
            attrs += ' opacity="%s"' % opacity
        if clip:
            attrs += ' clip-path="url(#%s)"' % clip
        return attrs, bool(s)

    # shapes ----------------------------------------------------------
    def rect(self, x, y, w, h, color, r=0, outline=True, flat=False, grad=None,
             opacity=1, clip=None):
        attrs, stroked = self.common(color, outline, flat, grad, opacity, clip)
        if self.p['crisp']:
            x0, y0 = self.n(x), self.n(y)
            x1, y1 = self.n(x + w), self.n(y + h)
            if stroked:
                x0, y0, x1, y1 = x0 + .5, y0 + .5, x1 - .5, y1 - .5
            geo = 'x="%s" y="%s" width="%s" height="%s"' % (x0, y0, max(1, x1 - x0), max(1, y1 - y0))
        else:
            geo = 'x="%s" y="%s" width="%s" height="%s"' % (self.n(x), self.n(y), self.n(w), self.n(h))
            rr = r * self.p['round']
            if rr:
                geo += ' rx="%s"' % round(rr, 2)
        self.els.append('<rect %s%s/>' % (geo, attrs))

    def poly(self, points, color, outline=True, flat=False, grad=None, opacity=1, clip=None):
        attrs, stroked = self.common(color, outline, flat, grad, opacity, clip)
        self.els.append('<polygon points="%s"%s/>' % (self.pts(points, stroked), attrs))

    def circle(self, cx, cy, r, color, outline=True, flat=False, grad=None, opacity=1, clip=None):
        self.ellipse(cx, cy, r, r, color, outline, flat, grad, opacity, clip)

    def ellipse(self, cx, cy, rx, ry, color, outline=True, flat=False, grad=None,
                opacity=1, clip=None):
        attrs, _ = self.common(color, outline, flat, grad, opacity, clip)
        self.els.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s"%s/>' % (
            self.n(cx), self.n(cy), self.n(rx), self.n(ry), attrs))

    def path(self, d, color, outline=True, flat=False, grad=None, opacity=1, clip=None):
        attrs, stroked = self.common(color, outline, flat, grad, opacity, clip)
        d = NUM.sub(lambda m: str(self.n(float(m.group()), stroked)), d)
        self.els.append('<path d="%s"%s/>' % (d, attrs))

    def line(self, points, color, width, cap='round', opacity=1, clip=None):
        w = max(1, round(width * self.p['k'])) if self.p['crisp'] else width
        extra = ' clip-path="url(#%s)"' % clip if clip else ''
        if opacity != 1:
            extra += ' opacity="%s"' % opacity
        odd = self.p['crisp'] and w % 2 == 1
        self.els.append(
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="%s" '
            'stroke-linecap="%s" stroke-linejoin="round"%s/>' % (
                self.pts(points, odd), color, w, 'square' if self.p['crisp'] else cap, extra))

    def arc(self, cx, cy, r, a0, a1, color, width, steps=24, cap='round'):
        pts = []
        for i in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * i / steps)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        self.line(pts, color, width, cap)
        return pts

    # effects ---------------------------------------------------------
    def clip_rect(self, x, y, w, h, r=0):
        cid = self.uid('c')
        rr = r * self.p['round']
        self.defs.append('<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>' % (
            cid, self.n(x), self.n(y), self.n(w), self.n(h), round(rr, 2)))
        return cid

    def clip_circle(self, cx, cy, r):
        cid = self.uid('c')
        self.defs.append('<clipPath id="%s"><circle cx="%s" cy="%s" r="%s"/></clipPath>' % (
            cid, self.n(cx), self.n(cy), self.n(r)))
        return cid

    def clip_poly(self, points):
        cid = self.uid('c')
        self.defs.append('<clipPath id="%s"><polygon points="%s"/></clipPath>' % (cid, self.pts(points)))
        return cid

    def shadow(self, cx=24, cy=43.5, rx=18, ry=3):
        if not self.p['shadow']:
            return
        fid = self.uid('f')
        self.defs.append('<filter id="%s" x="-50%%" y="-200%%" width="200%%" height="500%%">'
                         '<feGaussianBlur stdDeviation="1.4"/></filter>' % fid)
        self.els.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#000" opacity="0.28" '
                        'filter="url(#%s)"/>' % (cx, cy, rx, ry, fid))

    def gloss(self, x, y, w, h, r=0, force=False):
        """White highlight over the upper half of a box (Windows 7 glass, XP gel)."""
        if not (self.p['gloss'] or force) or self.p['crisp']:
            return
        gid = self.uid('g')
        self.defs.append('<linearGradient id="%s" x1="0" y1="0" x2="0" y2="1">'
                         '<stop offset="0" stop-color="#fff" stop-opacity="0.75"/>'
                         '<stop offset="1" stop-color="#fff" stop-opacity="0.08"/></linearGradient>' % gid)
        rr = r * self.p['round']
        self.els.append('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="url(#%s)"/>' % (
            x + 1, y + 1, w - 2, h * 0.48, round(rr, 2), gid))

    def svg(self):
        size = self.size
        head = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                'viewBox="0 0 %d %d"%s>' % (size, size, size, size,
                                           ' shape-rendering="crispEdges"' if self.p['crisp'] else ''))
        defs = '<defs>%s</defs>' % ''.join(self.defs) if self.defs else ''
        return head + defs + ''.join(self.els) + '</svg>\n'


# ---------------------------------------------------------------- palette per theme

def pick(ic, **by_theme):
    return by_theme.get(ic.t, by_theme.get('default'))


ACCENT = dict(win95='#000080', winxp='#2a64d6', win7='#2f6fc0', win10='#0078d4', win11='#1a73d9')
GREEN = dict(win95='#008000', winxp='#3aa336', win7='#3c9a3c', win10='#16a34a', win11='#2fb36a')
RED = dict(win95='#c00000', winxp='#d8401f', win7='#c8322c', win10='#e81123', win11='#e5484d')
PAPER = dict(win95='#ffffff', winxp='#ffffff', win7='#fbfbfb', win10='#ffffff', win11='#fdfdfd')
PAPER_EDGE = dict(win95='#c0c0c0', winxp='#d6dbe6', win7='#d0d6de', win10='#d8d8d8', win11='#dcdcdc')
METAL = dict(win95='#c0c0c0', winxp='#c9cfd8', win7='#b9c3cf', win10='#9aa0a6', win11='#aab3bd')
DARK = dict(win95='#000000', winxp='#1d2533', win7='#1e2a38', win10='#1f1f1f', win11='#2b2f36')


def T(ic, table):
    return table[ic.t]


# ---------------------------------------------------------------- emblems
# Small pictures placed on folders and documents; (cx, cy) is the center,
# s the size of their square.

def emblem_lines(ic, cx, cy, s, color='#7a869a'):
    for i in range(4):
        y = cy - s * .3 + i * s * .2
        ic.rect(cx - s * .35, y, s * (.7 if i < 3 else .45), max(1.5, s * .07), color,
                outline=False, flat=True)


def emblem_page(ic, cx, cy, s):
    ic.rect(cx - s * .34, cy - s * .44, s * .68, s * .88, T(ic, PAPER))
    emblem_lines(ic, cx, cy, s * .8)


def emblem_picture(ic, cx, cy, s):
    x, y, w, h = cx - s * .5, cy - s * .38, s, s * .76
    ic.rect(x, y, w, h, '#ffffff' if ic.t != 'win10' else '#f3f3f3')
    cl = ic.clip_rect(x + s * .09, y + s * .09, w - s * .18, h - s * .18)
    ic.rect(x + s * .09, y + s * .09, w - s * .18, h - s * .18,
            pick(ic, win95='#00c0ff', default='#6ab7ff'), outline=False)
    ic.circle(x + w * .7, y + h * .34, s * .09, pick(ic, win95='#ffff00', default='#ffd23f'),
              outline=False, flat=True)
    ic.poly([(x, y + h), (x + w * .38, y + h * .45), (x + w * .62, y + h * .72),
             (x + w * .78, y + h * .58), (x + w, y + h)],
            pick(ic, win95='#008000', default='#3fa34d'), outline=False, clip=cl)


def emblem_music(ic, cx, cy, s):
    c = pick(ic, win95='#000080', winxp='#3b4fc4', win7='#2a4a8c', win10='#0063b1', win11='#7b5cd6')
    ic.poly([(cx - s * .22, cy - s * .28), (cx + s * .34, cy - s * .44), (cx + s * .34, cy - s * .3),
             (cx - s * .22, cy - s * .14)], c, outline=False, flat=True)
    ic.rect(cx - s * .22, cy - s * .28, s * .09, s * .62, c, outline=False, flat=True)
    ic.rect(cx + s * .25, cy - s * .44, s * .09, s * .62, c, outline=False, flat=True)
    ic.ellipse(cx - s * .3, cy + s * .34, s * .16, s * .12, c, outline=False)
    ic.ellipse(cx + s * .17, cy + s * .18, s * .16, s * .12, c, outline=False)


def emblem_film(ic, cx, cy, s):
    x, y, w, h = cx - s * .46, cy - s * .36, s * .92, s * .72
    ic.rect(x, y, w, h, pick(ic, win95='#000000', default='#2d3440'))
    for i in range(4):
        hx_ = x + s * .08 + i * s * .22
        ic.rect(hx_, y + s * .05, s * .1, s * .1, '#ffffff', outline=False, flat=True)
        ic.rect(hx_, y + h - s * .15, s * .1, s * .1, '#ffffff', outline=False, flat=True)
    ic.rect(x + s * .1, y + h * .3, w - s * .2, h * .4,
            pick(ic, win95='#0000ff', default='#5aa2ff'), outline=False)


def emblem_download(ic, cx, cy, s):
    c = pick(ic, win95='#008000', winxp='#3aa336', win7='#2f8f2f', win10='#0078d4', win11='#1a73d9')
    ic.poly([(cx - s * .16, cy - s * .46), (cx + s * .16, cy - s * .46), (cx + s * .16, cy),
             (cx + s * .4, cy), (cx, cy + s * .44), (cx - s * .4, cy), (cx - s * .16, cy)], c)


def emblem_monitor(ic, cx, cy, s, screen=None):
    x, y, w, h = cx - s * .5, cy - s * .42, s, s * .64
    ic.rect(x, y, w, h, pick(ic, win95='#c0c0c0', winxp='#d8d4c8', default='#2b2f36'), r=1)
    ic.rect(x + s * .08, y + s * .08, w - s * .16, h - s * .16,
            screen or pick(ic, win95='#008080', winxp='#3a7bd5', default='#2f8cea'), outline=False)
    ic.rect(cx - s * .22, y + h, s * .44, s * .16, pick(ic, win95='#808080', default='#6b7280'),
            outline=False)


def emblem_house(ic, cx, cy, s):
    roof = pick(ic, win95='#c00000', winxp='#d9542b', win7='#c0443a', win10='#0078d4', win11='#e36a4f')
    wall = pick(ic, win95='#ffffc0', winxp='#f7e7c1', win7='#f2e4c9', win10='#f3f3f3', win11='#fbf3e6')
    ic.rect(cx - s * .34, cy - s * .08, s * .68, s * .5, wall)
    ic.poly([(cx - s * .5, cy - s * .02), (cx, cy - s * .48), (cx + s * .5, cy - s * .02)], roof)
    ic.rect(cx - s * .1, cy + s * .14, s * .2, s * .28, pick(ic, win95='#808000', default='#8a5a2b'),
            outline=False)


def emblem_globe(ic, cx, cy, s):
    r = s * .5
    ic.circle(cx, cy, r, pick(ic, win95='#0000ff', winxp='#2f7ae5', win7='#2d78d0', win10='#0078d4',
                                win11='#2b8ae6'))
    cl = ic.clip_circle(cx, cy, r)
    land = pick(ic, win95='#00ff00', default='#5cc15c')
    k = s / 36
    ic.path('M%s,%s C%s,%s %s,%s %s,%s C%s,%s %s,%s %s,%s C%s,%s %s,%s %s,%s Z' % (
        cx - 12 * k, cy - 11 * k, cx - 4 * k, cy - 14 * k, cx + 1 * k, cy - 8 * k, cx - 3 * k, cy - 3 * k,
        cx - 7 * k, cy + 2 * k, cx - 1 * k, cy + 7 * k, cx - 6 * k, cy + 15 * k,
        cx - 13 * k, cy + 10 * k, cx - 20 * k, cy - 2 * k, cx - 12 * k, cy - 11 * k),
        land, outline=False, flat=ic.t in ('win95', 'win10'), clip=cl)
    ic.path('M%s,%s C%s,%s %s,%s %s,%s C%s,%s %s,%s %s,%s Z' % (
        cx + 5 * k, cy - 14 * k, cx + 16 * k, cy - 12 * k, cx + 18 * k, cy + 2 * k, cx + 9 * k, cy + 4 * k,
        cx + 4 * k, cy + 6 * k, cx + 1 * k, cy - 6 * k, cx + 5 * k, cy - 14 * k),
        land, outline=False, flat=ic.t in ('win95', 'win10'), clip=cl)
    if ic.t in ('win10', 'win11'):
        white = '#ffffff'
        ic.line([(cx - r, cy), (cx + r, cy)], white, max(1, s * .04), opacity=.7)
        ic.ellipse(cx, cy, r * .45, r, None, outline=False)
        ic.els[-1] = ic.els[-1].replace('fill="none"', 'fill="none" stroke="#fff" stroke-opacity="0.7" '
                                        'stroke-width="%s"' % max(1, round(s * .04, 2)))
    ic.gloss(cx - r, cy - r, 2 * r, 2 * r, r)


def gear_points(cx, cy, ro, ri, teeth, phase=0):
    pts = []
    for i in range(teeth * 4):
        a = math.radians(phase + i * 360 / (teeth * 4))
        r = ro if i % 4 in (1, 2) else ri
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def emblem_gear(ic, cx, cy, s, color=None):
    c = color or pick(ic, win95='#808080', winxp='#7d8aa0', win7='#6f7f94', win10='#5f6b7a', win11='#6b7b8c')
    ic.poly(gear_points(cx, cy, s * .5, s * .38, 8, 11), c)
    ic.circle(cx, cy, s * .16, pick(ic, win95='#c0c0c0', default='#e8ecf2'))


def emblem_window(ic, cx, cy, s):
    x, y, w, h = cx - s * .5, cy - s * .42, s, s * .84
    ic.rect(x, y, w, h, pick(ic, win95='#c0c0c0', default='#ffffff'), r=1)
    ic.rect(x + 1 * (s / 20), y + 1 * (s / 20), w - 2 * (s / 20), h * .24, T(ic, ACCENT), outline=False)


def emblem_chevrons(ic, cx, cy, s, color=None):
    c = color or pick(ic, win95='#000080', default='#3b6fd8')
    w = max(2, s * .12)
    ic.line([(cx - s * .15, cy - s * .3), (cx - s * .45, cy), (cx - s * .15, cy + s * .3)], c, w)
    ic.line([(cx + s * .15, cy - s * .3), (cx + s * .45, cy), (cx + s * .15, cy + s * .3)], c, w)


def emblem_prompt(ic, cx, cy, s, color='#c0c0c0'):
    w = max(2, s * .12)
    ic.line([(cx - s * .42, cy - s * .28), (cx - s * .12, cy), (cx - s * .42, cy + s * .28)], color, w)
    ic.rect(cx - s * .02, cy + s * .2, s * .42, max(2, s * .12), color, outline=False, flat=True)


def emblem_palette(ic, cx, cy, s):
    ic.ellipse(cx, cy, s * .5, s * .4, pick(ic, win95='#ffffc0', default='#f1dcb5'))
    for (dx, dy, c) in ((-.24, -.12, RED), (0, -.22, GREEN), (.24, -.1, ACCENT)):
        ic.circle(cx + s * dx, cy + s * dy, s * .09, T(ic, c), outline=False, flat=True)
    ic.circle(cx + s * .18, cy + s * .16, s * .09, pick(ic, win95='#ffff00', default='#f5c518'),
              outline=False, flat=True)


def emblem_flask(ic, cx, cy, s):
    ic.poly([(cx - s * .12, cy - s * .48), (cx + s * .12, cy - s * .48), (cx + s * .12, cy - s * .14),
             (cx + s * .42, cy + s * .42), (cx - s * .42, cy + s * .42), (cx - s * .12, cy - s * .14)],
            pick(ic, win95='#ffffff', default='#e8f4ff'))
    ic.poly([(cx - s * .26, cy + s * .14), (cx + s * .26, cy + s * .14), (cx + s * .4, cy + s * .4),
             (cx - s * .4, cy + s * .4)], T(ic, GREEN), outline=False)


def emblem_cards(ic, cx, cy, s):
    ic.rect(cx - s * .46, cy - s * .38, s * .5, s * .7, '#ffffff', r=1)
    ic.rect(cx - s * .04, cy - s * .3, s * .5, s * .7, '#ffffff', r=1)
    ic.circle(cx + s * .21, cy + s * .02, s * .1, T(ic, RED), outline=False, flat=True)
    ic.circle(cx - s * .21, cy - s * .06, s * .08, '#000000', outline=False, flat=True)


def emblem_chart(ic, cx, cy, s):
    for i, (hgt, c) in enumerate(((.5, ACCENT), (.8, GREEN), (.35, RED))):
        ic.rect(cx - s * .42 + i * s * .3, cy + s * .4 - s * hgt, s * .22, s * hgt, T(ic, c))


def emblem_pencil(ic, cx, cy, s):
    c = pick(ic, win95='#ffff00', default='#f5b82e')
    ic.poly([(cx + s * .32, cy - s * .48), (cx + s * .48, cy - s * .32), (cx - s * .22, cy + s * .38),
             (cx - s * .38, cy + s * .22)], c)
    ic.poly([(cx - s * .38, cy + s * .22), (cx - s * .22, cy + s * .38), (cx - s * .48, cy + s * .48)],
            pick(ic, win95='#808080', default='#3a3a3a'), outline=False)


def emblem_magnifier(ic, cx, cy, s):
    ic.line([(cx + s * .12, cy + s * .12), (cx + s * .46, cy + s * .46)],
            pick(ic, win95='#000000', winxp='#6b4a2b', default='#3d4450'), s * .16)
    ic.circle(cx - s * .12, cy - s * .12, s * .32, pick(ic, win95='#00ffff', default='#bfe3ff'),
              opacity=1 if ic.t == 'win95' else .92)
    ic.gloss(cx - s * .44, cy - s * .44, s * .64, s * .64, 8, force=ic.t == 'winxp')


def emblem_star(ic, cx, cy, s):
    pts = []
    for i in range(10):
        a = math.radians(-90 + i * 36)
        r = s * (.5 if i % 2 == 0 else .22)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    ic.poly(pts, pick(ic, win95='#ffff00', default='#ffc928'))


def emblem_plus(ic, cx, cy, s):
    c = T(ic, GREEN)
    ic.circle(cx, cy, s * .5, c)
    ic.rect(cx - s * .3, cy - s * .08, s * .6, s * .16, '#ffffff', outline=False, flat=True)
    ic.rect(cx - s * .08, cy - s * .3, s * .16, s * .6, '#ffffff', outline=False, flat=True)


def emblem_box(ic, cx, cy, s):
    c = pick(ic, win95='#c08040', winxp='#d7a15a', win7='#c99452', win10='#c8935a', win11='#d6a46a')
    ic.rect(cx - s * .46, cy - s * .2, s * .92, s * .64, c)
    ic.poly([(cx - s * .46, cy - s * .2), (cx - s * .3, cy - s * .46), (cx + s * .3, cy - s * .46),
             (cx + s * .46, cy - s * .2)], lighten(c, .25))
    ic.rect(cx - s * .08, cy - s * .2, s * .16, s * .64, pick(ic, win95='#ffffff', default='#f4e3c1'),
            outline=False, flat=True)


EMBLEMS = {
    'page': emblem_page, 'picture': emblem_picture, 'music': emblem_music, 'film': emblem_film,
    'download': emblem_download, 'monitor': emblem_monitor, 'house': emblem_house,
    'globe': emblem_globe, 'gear': emblem_gear, 'window': emblem_window, 'chevrons': emblem_chevrons,
    'prompt': emblem_prompt, 'palette': emblem_palette, 'flask': emblem_flask, 'cards': emblem_cards,
    'chart': emblem_chart, 'pencil': emblem_pencil, 'magnifier': emblem_magnifier,
    'star': emblem_star, 'plus': emblem_plus, 'box': emblem_box,
}


# ---------------------------------------------------------------- base shapes

def folder(ic, emblem=None, band=None, emblem_scale=1.0):
    t = ic.t
    back, front = {
        'win95': ('#c0c000', '#ffff80'),
        'winxp': ('#e0ad3a', '#fcd976'),
        'win7': ('#d7a13d', '#f5d27f'),
        'win10': ('#e3a21a', '#ffc83d'),
        'win11': ('#eba01c', '#ffd35a'),
    }[t]
    ic.shadow()
    if t in ('winxp', 'win7'):
        ic.poly([(3, 8), (17, 8), (20, 11), (39, 11), (39, 38), (3, 38)], back)
        ic.rect(7, 13, 30, 22, '#ffffff', outline=True)
        if t == 'win7':
            ic.rect(10, 11, 28, 20, '#f2f6fb')
            emblem_lines(ic, 24, 20, 14, '#b9c3d0')
        front_pts = [(9, 18), (46, 18), (40, 42), (3, 42)]
        ic.poly(front_pts, front)
        if t == 'win7':
            ic.poly([(9.5, 18.5), (45.4, 18.5), (44.4, 23), (8.3, 23)], '#ffffff', outline=False,
                    flat=True, opacity=.45)
        ecx, ecy = 24.5, 30
    else:
        ic.poly([(4, 9), (18, 9), (21, 13), (44, 13), (44, 40), (4, 40)], back)
        if t == 'win11':
            ic.rect(7, 14, 34, 20, '#ffffff', outline=False)
        ic.rect(4, 17, 40, 23, front, r=2)
        ecx, ecy = 24, 28.5
    if band:
        if t in ('winxp', 'win7'):
            ic.poly([(6.3, 32), (43.3, 32), (41.1, 41), (3.9, 41)], band, outline=False)
        else:
            ic.rect(4, 33, 40, 7, band, r=2, outline=t == 'win95')
    if emblem:
        EMBLEMS[emblem](ic, ecx, ecy, 15 * emblem_scale)


def page(ic, emblem=None, band=None, emblem_scale=1.0):
    ic.shadow(rx=13, cy=44.5)
    body = [(9, 3), (30, 3), (39, 12), (39, 45), (9, 45)]
    ic.poly(body, T(ic, PAPER), grad='vert' if ic.t in ('win7', 'winxp') else None)
    ic.poly([(30, 3), (30, 12), (39, 12)], T(ic, PAPER_EDGE))
    if band:
        cl = ic.clip_poly(body)
        ic.rect(9, 36, 30, 9, band, outline=False, clip=cl)
    if emblem == 'lines':
        emblem_lines(ic, 23.5, 25, 22)
    elif emblem:
        EMBLEMS[emblem](ic, 24, 26, 18 * emblem_scale)


def window(ic, content='#ffffff', title=None, x=3, y=6, w=42, h=36):
    t = ic.t
    ic.shadow(rx=18)
    if t == 'win95':
        ic.rect(x, y, w, h, '#c0c0c0')
        ic.rect(x + 2, y + 2, w - 4, 6, '#000080', outline=False)
        ic.rect(x + 3, y + 10, w - 6, h - 13, content)
        return (x + 3, y + 10, w - 6, h - 13)
    if t == 'winxp':
        ic.rect(x, y, w, h, '#0a4fd6', r=4)
        ic.rect(x + 1, y + 1, w - 2, 8, '#2f7cf6', r=3, outline=False, grad='vert')
        ic.rect(x + w - 8, y + 2.5, 5, 5, '#e2553a', r=1, outline=False)
        ic.rect(x + 3, y + 10, w - 6, h - 13, content, outline=False, flat=True)
        return (x + 3, y + 10, w - 6, h - 13)
    if t == 'win7':
        ic.rect(x, y, w, h, '#7fa3c8', r=4)
        ic.gloss(x, y, w, 18, 4)
        ic.rect(x + w - 11, y + 1.5, 9, 5, '#c7433a', r=1, outline=False)
        ic.rect(x + 3, y + 9, w - 6, h - 12, content, outline=True)
        return (x + 3, y + 9, w - 6, h - 13)
    if t == 'win10':
        ic.rect(x, y, w, h, title or '#2b2b2b')
        ic.rect(x + 1.5, y + 8, w - 3, h - 9.5, content)
        return (x + 1.5, y + 8, w - 3, h - 9.5)
    ic.rect(x, y, w, h, title or '#e9edf2', r=3)
    ic.rect(x + 2, y + 8, w - 4, h - 10, content, r=2, outline=False)
    for i, c in enumerate(('#9aa3ad', '#9aa3ad', '#e5484d')):
        ic.circle(x + w - 13 + i * 4.5, y + 4, 1.2, c, outline=False, flat=True)
    return (x + 2, y + 8, w - 4, h - 10)


def monitor(ic, screen_fn=None):
    t = ic.t
    ic.shadow(rx=15)
    if t in ('win95', 'winxp'):
        case = '#c0c0c0' if t == 'win95' else '#e6e1d3'
        ic.rect(12, 36, 24, 6, case)
        ic.rect(18, 32, 12, 5, darken(case, .12), outline=t == 'win95')
        ic.rect(4, 4, 40, 30, case, r=3)
        sx, sy, sw, sh = 8, 8, 32, 22
        ic.rect(sx, sy, sw, sh, '#008080' if t == 'win95' else '#3f7fe0', r=1)
        if t == 'win95':
            ic.rect(38, 31, 3, 1.5, '#00ff00', outline=False)
    else:
        bezel = {'win7': '#1b2330', 'win10': '#2b2b2b', 'win11': '#30343b'}[t]
        ic.rect(15, 41, 18, 3, pick(ic, win7='#4b5563', win10='#5a5a5a', win11='#8b939c'), r=1)
        ic.rect(21, 33, 6, 9, pick(ic, win7='#6b7280', win10='#6e6e6e', win11='#a2aab3'), outline=False)
        ic.rect(3, 6, 42, 29, bezel, r=2)
        sx, sy, sw, sh = 5, 8, 38, 24
        screen = pick(ic, win7='#2f78c8', win10='#0078d4', win11='#4a9df0')
        ic.rect(sx, sy, sw, sh, screen, r=1, outline=False)
        if t == 'win7':
            ic.poly([(sx, sy), (sx + sw * .55, sy), (sx + sw * .3, sy + sh), (sx, sy + sh)], '#ffffff',
                    outline=False, flat=True, opacity=.16)
    if screen_fn:
        screen_fn(sx, sy, sw, sh)
    return sx, sy, sw, sh


def bin_(ic, full=False):
    t = ic.t
    ic.shadow(rx=14)
    body = pick(ic, win95='#c0c0c0', winxp='#9cc2e6', win7='#a9bfd4', win10='#d6d6d6', win11='#c7d3e0')
    pts = [(11, 14), (37, 14), (34, 44), (14, 44)]
    if full:
        paper = T(ic, PAPER)
        ic.poly([(13, 13), (19, 4), (27, 9), (22, 16)], paper)
        ic.poly([(22, 12), (31, 5), (37, 13), (29, 16)], pick(ic, win95='#ffffc0', default='#fff3c4'))
    ic.poly(pts, body, opacity=1 if t in ('win95', 'win10') else .96)
    rib = darken(body, .35)
    if t == 'win95':
        for x in (17, 21, 25, 29, 33):
            ic.line([(x, 17), (x - (x - 24) * .1, 42)], '#808080', 1)
    elif t in ('winxp', 'win7'):
        cl = ic.clip_poly(pts)
        ic.gloss(11, 14, 12, 60, 0, force=True)
        # recycling arrows
        g = T(ic, GREEN)
        ic.arc(24, 30, 6.5, 200, 320, g, 2.2)
        ic.arc(24, 30, 6.5, 330, 450, g, 2.2)
        ic.arc(24, 30, 6.5, 90, 180, g, 2.2)
        _ = cl
    else:
        for x in (19, 24, 29):
            ic.line([(x, 19), (x, 39)], rib, 1.6)
    ic.rect(9, 11, 30, 4, lighten(body, .1) if t != 'win95' else '#c0c0c0', r=1)


def envelope(ic):
    t = ic.t
    ic.shadow(rx=17)
    c = pick(ic, win95='#ffffc0', winxp='#fff6d8', win7='#f3f6fa', win10='#0078d4', win11='#4a9df0')
    ic.rect(4, 11, 40, 28, c, r=2)
    flap = pick(ic, win95='#000000', winxp='#c79e4a', win7='#8aa0b8', win10='#ffffff', win11='#ffffff')
    ic.line([(5, 12.5), (24, 28), (43, 12.5)], flap, 1.8 if t != 'win95' else 1)
    if t in ('winxp', 'win95'):
        ic.rect(34, 14, 7, 8, T(ic, RED), outline=t == 'win95')


def circle_badge(ic, color, glyph):
    ic.shadow(rx=15)
    ic.circle(24, 24, 19, color)
    ic.gloss(5, 5, 38, 38, 19, force=ic.t == 'winxp')
    glyph()


# ---------------------------------------------------------------- icons

ICONS = {}


def icon(*names):
    def deco(fn):
        for name in names:
            ICONS[name] = fn
        return fn
    return deco


def folder_icon(emblem=None, scale=1.0):
    return lambda ic: folder(ic, emblem, emblem_scale=scale)


for _name, _emblem in (('folder', None), ('folder-documents', 'page'), ('folder-pictures', 'picture'),
                       ('folder-music', 'music'), ('folder-videos', 'film'),
                       ('folder-download', 'download'), ('user-desktop', 'monitor'),
                       ('folder-remote', 'globe'), ('folder-new', 'star')):
    ICONS[_name] = folder_icon(_emblem)

for _name, _emblem in (('applications-accessories', 'pencil'), ('applications-development', 'chevrons'),
                       ('applications-games', 'cards'), ('applications-graphics', 'palette'),
                       ('applications-internet', 'globe'), ('applications-multimedia', 'film'),
                       ('applications-office', 'chart'), ('applications-science', 'flask'),
                       ('applications-system', 'gear'), ('applications-other', 'window')):
    ICONS[_name] = folder_icon(_emblem)


@icon('user-home')
def user_home(ic):
    ic.shadow(rx=16)
    emblem_house(ic, 24, 25, 38)


@icon('system-file-manager')
def file_manager(ic):
    t = ic.t
    if t in ('win95', 'winxp'):
        folder(ic)
        ic.shadow(cx=34, cy=44, rx=9, ry=2)
        emblem_magnifier(ic, 30, 30, 22)
    elif t == 'win7':
        folder(ic)
        ic.rect(4, 37, 40, 8, '#4b5d73', r=2)
        ic.gloss(4, 37, 40, 8, 2)
    else:
        folder(ic, band=T(ic, ACCENT))


@icon('utilities-terminal')
def terminal(ic):
    content = pick(ic, win95='#000000', default='#101418')
    x, y, w, h = window(ic, content, title=pick(ic, win10='#3a3a3a', win11='#3b3f46'))
    emblem_prompt(ic, x + w * .42, y + h * .5, h * .62,
                  pick(ic, win95='#c0c0c0', winxp='#e8e8e8', win7='#dfe6ee', default='#f2f2f2'))


@icon('web-browser')
def browser(ic):
    t = ic.t
    ic.shadow(rx=16)
    emblem_globe(ic, 24, 24, 36)
    if t in ('winxp', 'win7', 'win95'):
        ring = pick(ic, win95='#ffff00', winxp='#f2b233', win7='#e8a83a')
        ic.els.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="none" stroke="%s" stroke-width="%s" '
                      'transform="rotate(-28 %s %s)"/>' % (
                          ic.n(24), ic.n(24), ic.n(23), ic.n(8.5), ring, ic.n(3.2) if t != 'win95' else 2,
                          ic.n(24), ic.n(24)))


@icon('accessories-text-editor')
def text_editor(ic):
    t = ic.t
    ic.shadow(rx=14)
    ic.rect(8, 5, 32, 40, T(ic, PAPER), r=1)
    ic.rect(8, 5, 32, 7, pick(ic, win95='#000080', winxp='#3d7be0', win7='#4a86c8', win10='#0078d4',
                              win11='#3a8ee6'), r=1)
    if t in ('win95', 'winxp', 'win7'):
        for x in (13, 19, 25, 31, 37):
            ic.rect(x - 1, 3, 2, 6, T(ic, METAL), outline=t == 'win95')
    for i in range(6):
        ic.rect(12, 16 + i * 4.5, 24 if i % 3 != 2 else 16, 1.5,
                pick(ic, win95='#000000', default='#9aa6b8'), outline=False, flat=True)
    if t in ('win10', 'win11'):
        emblem_pencil(ic, 36, 36, 16)


@icon('accessories-calculator')
def calculator(ic):
    t = ic.t
    ic.shadow(rx=13)
    body = pick(ic, win95='#c0c0c0', winxp='#d4d0c8', win7='#5d6b7c', win10='#2b2b2b', win11='#3b4048')
    ic.rect(9, 3, 30, 42, body, r=3)
    ic.rect(12, 6, 24, 9, pick(ic, win95='#ffffff', winxp='#dfe8c8', win7='#cfe3c0', win10='#3c3c3c',
                               win11='#e8f0f8'), r=1)
    keys = pick(ic, win95='#c0c0c0', winxp='#f4f2ec', win7='#e6eaef', win10='#4a4a4a', win11='#5a616b')
    for row in range(4):
        for col in range(3):
            last = row == 3 and col == 2
            ic.rect(12 + col * 8.5, 18 + row * 6.6, 6.5, 5, T(ic, ACCENT) if last and t != 'win95' else keys,
                    r=1)


@icon('image-viewer')
def image_viewer(ic):
    ic.shadow(rx=17)
    emblem_picture(ic, 24, 24, 40)


@icon('utilities-system-monitor')
def system_monitor(ic):
    x, y, w, h = window(ic, pick(ic, win95='#000000', default='#0f1a14'),
                        title=pick(ic, win10='#3a3a3a', win11='#3b3f46'))
    grid = pick(ic, win95='#008000', default='#1f5f3a')
    for i in range(1, 4):
        ic.line([(x, y + h * i / 4), (x + w, y + h * i / 4)], grid, .8)
    ic.line([(x + 1, y + h * .8), (x + w * .2, y + h * .55), (x + w * .38, y + h * .7), (x + w * .55, y + h * .25),
             (x + w * .72, y + h * .5), (x + w - 1, y + h * .35)],
            pick(ic, win95='#00ff00', default='#3ddc84'), 2)


@icon('preferences-system', 'preferences-desktop')
def control_panel(ic):
    t = ic.t
    if t in ('win95', 'winxp', 'win7'):
        monitor(ic, lambda sx, sy, sw, sh: emblem_gear(ic, sx + sw / 2, sy + sh / 2, sh * .8,
                                                        pick(ic, win95='#c0c0c0', default='#e5e9ef')))
    else:
        ic.poly(gear_points(24, 24, 20, 15.5, 8, 11), pick(ic, win10='#5d6b7a', win11='#6f7f90'))
        ic.circle(24, 24, 7, pick(ic, win10='#ffffff', win11='#f2f5f8'), outline=False)


@icon('preferences-desktop-theme')
def theme_icon(ic):
    ic.shadow(rx=16)
    emblem_palette(ic, 24, 25, 40)
    if ic.t != 'win95':
        emblem_pencil(ic, 35, 14, 14)


@icon('preferences-desktop-wallpaper')
def wallpaper_icon(ic):
    monitor(ic, lambda sx, sy, sw, sh: emblem_picture(ic, sx + sw / 2, sy + sh / 2, sw * .8))


@icon('computer')
def computer(ic):
    t = ic.t

    def screen(sx, sy, sw, sh):
        if t == 'win95':
            return
        ic.rect(sx + 2, sy + sh - 5, sw - 4, 3, pick(ic, winxp='#2a55c4', win7='#1d4f8f', default='#1a1a1a'),
                outline=False, opacity=.7)
    monitor(ic, screen)


@icon('network-workgroup', 'network-server')
def network_places(ic):
    ic.shadow(rx=16)
    emblem_globe(ic, 18, 18, 28)
    emblem_monitor(ic, 32, 33, 24)


@icon('drive-harddisk')
def harddisk(ic):
    t = ic.t
    ic.shadow(rx=19)
    body = pick(ic, win95='#c0c0c0', winxp='#d9dde4', win7='#b7c2cf', win10='#6b6b6b', win11='#9aa5b1')
    if t in ('winxp', 'win7'):
        ic.poly([(8, 15), (44, 15), (40, 22), (4, 22)], lighten(body, .25))
    ic.rect(4, 21, 40, 16, body, r=2)
    ic.rect(8, 28, 22, 2, darken(body, .4), outline=False, flat=True)
    ic.rect(36, 28, 4, 3, pick(ic, win95='#00ff00', default='#35d04e'), outline=False, flat=True)


@icon('drive-removable-media', 'drive-removable-media-usb', 'media-removable')
def usb_stick(ic):
    ic.shadow(rx=10)
    ic.rect(18, 4, 12, 11, T(ic, METAL), r=1)
    ic.rect(21, 7, 2, 3, '#555555', outline=False, flat=True)
    ic.rect(25, 7, 2, 3, '#555555', outline=False, flat=True)
    ic.rect(14, 14, 20, 30, pick(ic, win95='#000080', winxp='#3b6fd6', win7='#2c4f7c', win10='#0078d4',
                                 win11='#3a8ee6'), r=3)
    ic.gloss(14, 14, 20, 30, 3)


@icon('user-trash', 'user-trash-empty', 'trash-empty')
def trash(ic):
    bin_(ic)


@icon('user-trash-full', 'trash-full')
def trash_full(ic):
    bin_(ic, full=True)


@icon('internet-mail', 'mail-client')
def mail(ic):
    envelope(ic)


@icon('multimedia-player', 'applications-multimedia-player')
def media_player(ic):
    color = pick(ic, win95='#008080', winxp='#2f6fd6', win7='#2c6cc0', win10='#0078d4', win11='#e8743b')

    def glyph():
        ic.poly([(19, 14), (35, 24), (19, 34)], '#ffffff', outline=ic.t == 'win95', flat=True)
    circle_badge(ic, color, glyph)


@icon('help-browser', 'help-contents', 'help-about')
def help_icon(ic):
    def glyph():
        w = 4.5 if ic.t != 'win95' else 4.5
        ic.line([(18, 18), (19, 13.5), (24, 11.5), (29, 13.5), (30, 18), (26.5, 22), (24, 25), (24, 28)],
                '#ffffff', w)
        ic.circle(24, 35, 2.8, '#ffffff', outline=False, flat=True)
    circle_badge(ic, T(ic, ACCENT), glyph)


@icon('system-run')
def run_icon(ic):
    x, y, w, h = window(ic, '#ffffff', title=pick(ic, win10='#3a3a3a', win11='#e9edf2'), x=3, y=12, w=32, h=30)
    g = T(ic, GREEN)
    ic.poly([(26, 6), (44, 6), (44, 24), (38, 18), (26, 30), (20, 24), (32, 12)], g)


@icon('system-search', 'edit-find')
def search_icon(ic):
    ic.shadow(cx=30, rx=12)
    emblem_magnifier(ic, 22, 22, 40)


def power_glyph(ic, cx, cy, r, color, w):
    ic.arc(cx, cy, r, -60, 240, color, w)
    ic.line([(cx, cy - r - 2), (cx, cy + 1)], color, w)


def square_badge(ic, color):
    ic.shadow(rx=16)
    ic.rect(5, 5, 38, 38, color, r=6)
    ic.gloss(5, 5, 38, 38, 6, force=ic.t == 'winxp')


@icon('system-shutdown')
def shutdown(ic):
    square_badge(ic, T(ic, RED))
    power_glyph(ic, 24, 25.5, 10, '#ffffff', 4 if ic.t != 'win95' else 4.5)


@icon('system-reboot', 'view-refresh')
def reboot(ic):
    name_color = T(ic, GREEN)
    ic.shadow(rx=15)
    ic.circle(24, 24, 19, name_color)
    ic.gloss(5, 5, 38, 38, 19, force=ic.t == 'winxp')
    pts = ic.arc(24, 24, 11, -60, 230, '#ffffff', 4)
    ex, ey = pts[-1]
    ic.poly([(ex - 6, ey - 1), (ex + 5, ey - 4), (ex + 2, ey + 7)], '#ffffff', outline=False, flat=True)


@icon('system-log-out')
def log_out(ic):
    square_badge(ic, pick(ic, win95='#808000', winxp='#e6962c', win7='#d7892b', win10='#ca5010', win11='#e8743b'))
    ic.rect(12, 12, 13, 24, '#ffffff', outline=False, flat=True, opacity=.35)
    ic.line([(19, 12), (12, 12), (12, 36), (19, 36)], '#ffffff', 3.2)
    ic.line([(19, 24), (35, 24)], '#ffffff', 3.6)
    ic.poly([(31, 17), (38, 24), (31, 31)], '#ffffff', outline=False, flat=True)


@icon('system-lock-screen')
def lock(ic):
    ic.shadow(rx=13)
    metal = T(ic, METAL)
    ic.arc(24, 20, 9, 180, 360, metal if ic.t != 'win95' else '#808080', 4.5, cap='butt')
    ic.line([(15, 20), (15, 24)], metal if ic.t != 'win95' else '#808080', 4.5, cap='butt')
    ic.line([(33, 20), (33, 24)], metal if ic.t != 'win95' else '#808080', 4.5, cap='butt')
    body = pick(ic, win95='#ffff00', winxp='#e7b43a', win7='#d9a530', win10='#ffb900', win11='#f5b82e')
    ic.rect(9, 23, 30, 21, body, r=3)
    ic.gloss(9, 23, 30, 21, 3, force=ic.t == 'winxp')
    ic.circle(24, 31, 3, '#3a3a3a', outline=False, flat=True)
    ic.rect(23, 32, 2, 7, '#3a3a3a', outline=False, flat=True)


@icon('text-x-generic', 'text-plain', 'text-x-log', 'text-markdown', 'text-x-readme')
def text_file(ic):
    page(ic, 'lines')


@icon('image-x-generic')
def image_file(ic):
    page(ic, 'picture')


@icon('audio-x-generic')
def audio_file(ic):
    page(ic, 'music')


@icon('video-x-generic')
def video_file(ic):
    page(ic, 'film')


@icon('package-x-generic', 'application-x-archive', 'application-zip')
def archive_file(ic):
    ic.shadow(rx=17)
    emblem_box(ic, 24, 26, 40)


@icon('application-x-executable', 'application-x-desktop', 'exec', 'application-default-icon')
def executable(ic):
    window(ic, '#ffffff', title=pick(ic, win10='#3a3a3a'))


@icon('application-pdf')
def pdf_file(ic):
    page(ic, 'lines', band=T(ic, RED))


@icon('x-office-document', 'application-vnd.oasis.opendocument.text')
def office_document(ic):
    page(ic, 'lines', band=T(ic, ACCENT))


@icon('x-office-spreadsheet', 'application-vnd.oasis.opendocument.spreadsheet')
def office_spreadsheet(ic):
    page(ic, 'chart', band=T(ic, GREEN), emblem_scale=.9)


@icon('x-office-presentation', 'application-vnd.oasis.opendocument.presentation')
def office_presentation(ic):
    page(ic, 'picture', band=pick(ic, win95='#c06000', default='#e8743b'), emblem_scale=.9)


@icon('text-html')
def html_file(ic):
    page(ic, 'globe', emblem_scale=.95)


@icon('text-x-script', 'application-x-shellscript', 'text-x-python')
def script_file(ic):
    page(ic)
    ic.rect(13, 16, 22, 18, pick(ic, win95='#000000', default='#1f2630'), r=1)
    emblem_prompt(ic, 23, 25, 11, '#e6e6e6')


@icon('unknown')
def unknown_file(ic):
    page(ic)


@icon('document-new')
def document_new(ic):
    page(ic, 'lines')
    emblem_star(ic, 36, 36, 16)


@icon('emblem-symbolic-link')
def link_emblem(ic):
    ic.rect(6, 6, 36, 36, '#ffffff', r=3)
    ic.arc(30, 32, 14, 180, 270, pick(ic, win95='#000000', default='#2a64d6'), 5, cap='butt')
    ic.poly([(26, 10), (38, 18), (26, 26)], pick(ic, win95='#000000', default='#2a64d6'), outline=False,
            flat=True)


@icon('edit-delete')
def edit_delete(ic):
    c = T(ic, RED)
    if ic.t in ('win10', 'win11'):
        bin_(ic)
        return
    ic.shadow(rx=13)
    ic.line([(12, 12), (36, 36)], c, 8)
    ic.line([(36, 12), (12, 36)], c, 8)


@icon('edit-rename')
def edit_rename(ic):
    ic.shadow(rx=17)
    ic.rect(4, 14, 34, 18, '#ffffff', r=1)
    ic.rect(8, 21, 16, 4, pick(ic, win95='#000080', default='#316ac5'), outline=False, flat=True)
    emblem_pencil(ic, 34, 26, 22)


# ---------------------------------------------------------------- tray icons
# Shown at 16-24 px in the notification area and larger in the flyouts.
# Windows 10 and 11 use white shapes that the panel tints with the text color
# ("tray { icons symbolic }"); the older themes use colored icons.

SYMBOLIC = ('win10', 'win11')
WHITE = '#ffffff'


def outlined_arc(ic, cx, cy, r, a0, a1, width):
    """A white arc with a dark rim, readable on light and dark backgrounds."""
    if ic.t == 'win95':
        ic.arc(cx, cy, r, a0, a1, '#000000', width, steps=10)
        return
    ic.arc(cx, cy, r, a0, a1, pick(ic, winxp='#0c3a8c', default='#1c2733'), width + 2.4, steps=12)
    ic.arc(cx, cy, r, a0, a1, WHITE, width, steps=12)


def badge_off(ic, cx=36, cy=36, r=10):
    if ic.t in SYMBOLIC:
        ic.line([(cx - r * .6, cy - r * .6), (cx + r * .6, cy + r * .6)], WHITE, 3.6)
        ic.line([(cx + r * .6, cy - r * .6), (cx - r * .6, cy + r * .6)], WHITE, 3.6)
        return
    ic.circle(cx, cy, r, T(ic, RED))
    w = 3.4 if ic.t != 'win95' else 3
    ic.line([(cx - r * .45, cy - r * .45), (cx + r * .45, cy + r * .45)], WHITE, w)
    ic.line([(cx + r * .45, cy - r * .45), (cx - r * .45, cy + r * .45)], WHITE, w)


def tray_speaker(ic, waves, muted):
    t = ic.t
    if t in SYMBOLIC:
        shape = [(4, 17), (13, 17), (24, 7), (24, 41), (13, 31), (4, 31)]
        if t == 'win11':
            ic.poly(shape, WHITE, outline=False, flat=True)
        else:
            ic.line(shape + [shape[0]], WHITE, 3.4)
        if muted:
            ic.line([(31, 18), (43, 30)], WHITE, 3.6)
            ic.line([(43, 18), (31, 30)], WHITE, 3.6)
        for i in range(waves):
            ic.arc(24, 24, 8 + 7 * i, -48, 48, WHITE, 3.6, steps=12)
        return
    ic.rect(3, 16, 11, 16, pick(ic, win95='#c0c0c0', winxp='#dfe6f1', win7='#e8eef5'))
    ic.poly([(14, 16), (26, 5), (26, 43), (14, 32)], pick(ic, win95='#ffff00', winxp='#f2c84b',
                                                        win7='#d4dde8'))
    for i in range(waves):
        outlined_arc(ic, 26, 24, 8 + 7 * i, -48, 48, 3.4)
    if muted:
        badge_off(ic, 37, 34, 10)


def tray_wireless(ic, level, offline=False):
    t = ic.t
    if t == 'win10':
        for i in range(4):
            on = not offline and i < level
            if i == 0:
                ic.circle(24, 40, 3.6, WHITE, outline=False, flat=True, opacity=1 if on else .35)
                continue
            pts = []
            for k in range(13):
                a = math.radians(-135 + 90 * k / 12)
                pts.append((24 + 11 * i * math.cos(a), 41 + 11 * i * math.sin(a)))
            ic.line(pts, WHITE, 4, opacity=1 if on else .35)
    elif t == 'win11':
        bands = [(0, 9), (13, 21), (25, 32), (36, 42)]
        for i, (r0, r1) in enumerate(bands):
            on = not offline and i < level
            outer = [(24 + r1 * math.cos(math.radians(a)), 44 + r1 * math.sin(math.radians(a)))
                     for a in range(-135, -44, 5)]
            inner = [(24 + r0 * math.cos(math.radians(a)), 44 + r0 * math.sin(math.radians(a)))
                     for a in range(-45, -136, -5)] if r0 else [(24, 44)]
            ic.poly(outer + inner, WHITE, outline=False, flat=True, opacity=1 if on else .35)
    else:
        for i in range(4):
            on = not offline and i < level
            h = 10 + i * 10
            color = (pick(ic, win95='#00c000', winxp='#56d33c', win7='#ffffff') if on else
                     pick(ic, win95='#808080', winxp='#a9bfdf', win7='#6f7c8a'))
            ic.rect(4 + i * 11, 44 - h, 8, h, color, r=1)
    if offline:
        badge_off(ic, 37, 36, 10)


def tray_wired(ic, offline=False):
    if ic.t in SYMBOLIC:
        ic.line([(5, 8), (43, 8), (43, 32), (5, 32), (5, 8)], WHITE, 3.4)
        ic.line([(24, 33), (24, 40)], WHITE, 3.4)
        ic.line([(14, 41), (34, 41)], WHITE, 3.4)
    else:
        emblem_monitor(ic, 17, 19, 28)
        emblem_monitor(ic, 31, 31, 28)
    if offline:
        badge_off(ic, 37, 36, 10)


def tray_battery(ic, level, charging):
    t = ic.t
    fill_w = 29 * level / 100
    if t in SYMBOLIC:
        right = 30 if charging else 40
        ic.line([(4, 15), (right, 15), (right, 33), (4, 33), (4, 15)], WHITE, 3.2)
        ic.rect(right + 2, 20, 4, 8, WHITE, outline=False, flat=True)
        inner = (right - 11) * level / 100
        if inner > 0:
            ic.rect(8, 19, inner, 10, WHITE, outline=False, flat=True)
        if charging:
            ic.poly([(42, 8), (35, 25), (40, 25), (37, 40), (46, 21), (41, 21)], WHITE, outline=False,
                    flat=True)
        return
    ic.rect(3, 13, 37, 22, pick(ic, win95='#c0c0c0', winxp='#f1f4f9', win7='#e4e9ef'), r=2)
    ic.rect(40, 19, 5, 10, pick(ic, win95='#808080', winxp='#9aa6b6', win7='#8792a0'), r=1)
    color = T(ic, GREEN) if level >= 40 else pick(ic, win95='#ffff00', default='#f2b322') if level >= 20 \
        else T(ic, RED)
    if fill_w > 0:
        ic.rect(7, 17, fill_w, 14, color, r=1, outline=False)
    if charging:
        ic.poly([(26, 3), (15, 25), (22, 25), (18, 45), (32, 20), (25, 20)],
                pick(ic, win95='#ffff00', default='#ffd23f'))


def tray_brightness(ic):
    color = WHITE if ic.t in SYMBOLIC else pick(ic, win95='#ffff00', default='#ffc928')
    for i in range(8):
        a = math.radians(i * 45)
        ic.line([(24 + 13 * math.cos(a), 24 + 13 * math.sin(a)), (24 + 20 * math.cos(a), 24 + 20 * math.sin(a))],
                color if ic.t in SYMBOLIC else pick(ic, win95='#000000', default='#e8962a'), 3.6)
    ic.circle(24, 24, 8.5, color, flat=ic.t in SYMBOLIC, outline=ic.t not in SYMBOLIC)


TRAY_ICONS = {
    'tray-volume-muted': lambda ic: tray_speaker(ic, 0, True),
    'tray-volume-off': lambda ic: tray_speaker(ic, 0, False),
    'tray-volume-low': lambda ic: tray_speaker(ic, 1, False),
    'tray-volume-medium': lambda ic: tray_speaker(ic, 2, False),
    'tray-volume-high': lambda ic: tray_speaker(ic, 3, False),
    'tray-network-wireless-offline': lambda ic: tray_wireless(ic, 0, True),
    'tray-network-wired': lambda ic: tray_wired(ic),
    'tray-network-wired-offline': lambda ic: tray_wired(ic, True),
    'tray-brightness': tray_brightness,
}
for _level in range(5):
    TRAY_ICONS['tray-network-wireless-%d' % _level] = (lambda lv: lambda ic: tray_wireless(ic, lv))(_level)
for _level in range(0, 101, 20):
    for _charging in (False, True):
        TRAY_ICONS['tray-battery-%d%s' % (_level, '-charging' if _charging else '')] = \
            (lambda lv, ch: lambda ic: tray_battery(ic, lv, ch))(_level, _charging)


# ---------------------------------------------------------------- aliases

# canonical icon: other icon names and desktop entry @Categories that use it
ALIASES = """
system-file-manager org.xfce.thunar thunar Thunar org.gnome.Nautilus nautilus org.kde.dolphin dolphin nemo org.cinnamon.Nemo pcmanfm pcmanfm-qt caja io.elementary.files foilebrowser file-manager @FileManager
utilities-terminal org.xfce.terminal xfce4-terminal terminal Terminal org.gnome.Terminal gnome-terminal org.gnome.Console kgx org.kde.konsole konsole Alacritty alacritty foot footclient kitty wezterm org.wezfurlong.wezterm xterm urxvt rxvt-unicode st lxterminal terminator tilix com.gexperts.Tilix ghostty com.mitchellh.ghostty @TerminalEmulator
web-browser firefox org.mozilla.firefox firefox-esr chromium google-chrome brave-browser com.brave.Browser librewolf io.gitlab.librewolf-community epiphany org.gnome.Epiphany vivaldi falkon org.kde.falkon qutebrowser zen-browser browser internet-web-browser @WebBrowser
accessories-text-editor org.xfce.mousepad mousepad gedit org.gnome.gedit org.gnome.TextEditor gnome-text-editor kate org.kde.kate kwrite org.kde.kwrite featherpad leafpad xed pluma gvim text-editor @TextEditor
accessories-calculator galculator org.gnome.Calculator gnome-calculator kcalc org.kde.kcalc qalculate-gtk io.github.Qalculate xpcalc calculator @Calculator
image-viewer org.xfce.ristretto ristretto eog org.gnome.eog org.gnome.Loupe gwenview org.kde.gwenview feh imv nomacs sxiv nsxiv viewnior
utilities-system-monitor htop btop gnome-system-monitor org.gnome.SystemMonitor ksysguard org.kde.plasma-systemmonitor xfce4-taskmanager org.xfce.taskmanager lxtask system-monitor
preferences-system tilewin-settings xfce4-settings-manager org.xfce.settings.manager gnome-control-center org.gnome.Settings systemsettings preferences-system-windows
internet-mail thunderbird org.mozilla.Thunderbird evolution org.gnome.Evolution geary org.gnome.Geary kmail org.kde.kmail2 claws-mail mail-send-receive @Email
multimedia-player vlc mpv io.mpv.Mpv celluloid io.github.celluloid_player.Celluloid totem org.gnome.Totem parole org.xfce.parole smplayer haruna org.kde.haruna audacious rhythmbox org.gnome.Rhythmbox3 strawberry elisa org.kde.elisa multimedia-video-player @Player
user-home folder-home
folder-download folder-downloads
user-desktop desktop
folder-documents folder-templates
""".strip()


def aliases_text():
    lines = ['# tileWin theme icons: <icon file> <icon names and @DesktopCategories using it>',
             '# Generated by tools/gen-icons.py']
    lines += ALIASES.splitlines()
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- output

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    only = sys.argv[1:]
    canonical = {}
    for name, fn in ICONS.items():
        canonical.setdefault(fn, name)
    for theme in THEMES:
        out = os.path.join(root, 'themes', theme, 'icons')
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(out):
            if name.endswith('.svg') and not os.path.isdir(os.path.join(out, name)):
                os.remove(os.path.join(out, name))
        extra = []
        for fn, name in canonical.items():
            if only and name not in only:
                continue
            ic = Icon(theme)
            fn(ic)
            with open(os.path.join(out, name + '.svg'), 'w') as f:
                f.write(ic.svg())
        for name, fn in TRAY_ICONS.items():
            if only and name not in only:
                continue
            ic = Icon(theme, tray=True)
            fn(ic)
            with open(os.path.join(out, name + '.svg'), 'w') as f:
                f.write(ic.svg())
        # names drawn by the same function become aliases of the first one
        for name, fn in ICONS.items():
            if canonical[fn] != name:
                extra.append((canonical[fn], name))
        with open(os.path.join(out, 'aliases'), 'w') as f:
            f.write(aliases_text())
            for target, name in extra:
                f.write('%s %s\n' % (target, name))
    print('%d icons and %d tray icons x %d themes' % (len(canonical), len(TRAY_ICONS), len(THEMES)))


if __name__ == '__main__':
    main()
