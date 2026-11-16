#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 TLG PC port
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the Xbox pad picture of the game's button hints (issue #61): gpu/assets/pad_xbox.dds.

The game draws every button hint from one 256x128 picture of a DualShock 4 and puts a glow on the
button to press, at places listed in its own script (game/scripts/guide/guidesystem.lua,
GuideSystem._Const.BUTTON). With "Button icons: Xbox" the port shows this picture instead
(vk_texture_pack.cpp, swapped on the PC side, the game files stay as they are) and moves the glow
places (tlg_lua.cpp, ButtonIcons). Both come from the launcher's own Xbox drawing
(launcher/pad_drawing.py XBOX_SVG), so the hint and the Controller page show the same pad.

Run after a change of the drawing (needs Pillow):
    python scripts/make_xbox_pad.py           writes the DDS, prints the glow table for tlg_lua.cpp
    python scripts/make_xbox_pad.py --png x   also writes a preview PNG

The idea of swapping the hint picture and moving the glow comes from SulphurS16/TLG-Port (pc_prompts);
the drawing and this code are our own.
"""
import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'launcher'))
import pad_drawing  # noqa: E402

GAME_W, GAME_H = 256, 128           # the game's picture; the glow places are in these pixels
SCALE, OX, OY = 0.46, 36.0, 9.0      # drawing (400x250) -> game picture
DETAIL = 2                          # our picture: 512x256, sharp on a 4K screen
SUPER = 8                           # drawn this much larger, then scaled down (smooth edges)
LINE_GREY = 179                     # the game's line colour; its fill is opaque black
LINE_WIDTH = 0.8                    # in game pixels
FILLED = ('MainOutline', 'LeftShoulder', 'RightShoulder', 'L2Trigger', 'R2Trigger')
GLOW_SHIFT = (16.0, 16.5)           # glow place = button centre - this (from the game's own table)
# Wider than 16:9 the game stretches the glow sideways from its left edge: tlg_lua.cpp (ButtonIcons)
# moves it back by half the extra width.

# Game hint button -> centre in the drawing. Share and the touchpad click are both the View button
# on an Xbox pad (runtime_pad.c pad_map).
GLOW = {
    'R1': (300, 33), 'R2': (303, 14), 'L1': (100, 33), 'L2': (97, 14),
    'Left': (135, 140), 'Right': (173, 140), 'Up': (154, 121), 'Down': (154, 159),
    'Square': (266, 88), 'Cross': (288, 110), 'Circle': (310, 88), 'Triangle': (288, 66),
    'StickBaseL': (112, 88), 'StickBaseR': (246, 140),
    'Push': (174, 88), 'PS': (200, 62), 'Select': (174, 88), 'Start': (226, 88),
}


# The Controls screen (Options > Controls) is a second picture of the game: a panel, the pad and the
# lines to the labels in one 1920x1088 picture (game/textures/ui/gamemenu/
# sys_option_item_guide_padimage); the label texts are placed by the game, so our lines end where
# the game's end. Scale and place: the two sticks sit above "Move" and "Move Camera".
C_W, C_H, C_ROWS = 1920, 1080, 1088          # the last 8 rows are black filler, as in the game's
C_SCALE, C_OX, C_OY = 184 / 134, 455 - 112 * 184 / 134, 180.0
C_SUPER = 4
C_PANEL = (48, 63, 1092, 716)                # outer box of the panel; 3 px edge, corner 15
C_PANEL_ALPHA, C_EDGE_ALPHA = 92, 184
C_BODY, C_PART, C_LINE, C_LEAD = 24, 52, 190, 255   # greys: pad, raised parts, pad lines, label lines
C_RAISED = ('Joystick', 'ButtonBorder', 'DpadButton', 'PSButton', 'ShareButton', 'OptionsButton')
# Label lines: from a point of the drawing (x, y, start distance from it) through corners in the
# picture to the place where the game's own line ends.
C_LEADS = [
    ((226, 88, 6), ['u175', 'x597']),              # Menu: Pause
    ((288, 66, 11), ['r835', 'y255', 'x849']),     # Y: Jump
    ((310, 88, 11), ['r868', 'y333']),             # B: Pick Up/Grab
    ((288, 110, 11), ['d460', 'x849']),            # A: Crouch
    ((266, 88, 11), ['d518', 'x776']),             # X: Tackle/Throw (crosses the right stick)
    ((112, 88, 0), ['d546']),                     # left stick: Move
    ((246, 140, 0), ['d546']),                    # right stick: Move Camera
]


def shapes(scale=SCALE, ox=OX, oy=OY):
    """(id, [polylines], circle or None) of every part, in game pixels."""
    P = lambda x, y: (ox + x * scale, oy + y * scale)
    for el in ET.fromstring(pad_drawing.XBOX_SVG).iter():
        tag = el.tag.split('}')[-1]
        if tag == 'path':
            yield el.get('id'), [[P(*p) for p in pl] for pl in pad_drawing.path_polylines(el.get('d'))], None
        elif tag == 'circle':
            cx, cy, r = (float(el.get(k)) for k in ('cx', 'cy', 'r'))
            yield el.get('id'), [], (*P(cx, cy), r * scale)


def render_controls():
    """The Controls screen picture: see-through black panel, the pad, white lines to the labels."""
    k = C_SUPER
    size = (C_W * k, C_ROWS * k)
    grey, alpha = Image.new('L', size, 0), Image.new('L', size, 0)
    dg, da = ImageDraw.Draw(grey), ImageDraw.Draw(alpha)
    x0, y0, x1, y1 = (v * k for v in C_PANEL)
    da.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius=15 * k, fill=C_EDGE_ALPHA)
    da.rounded_rectangle((x0 + 3 * k, y0 + 3 * k, x1 - 1 - 3 * k, y1 - 1 - 3 * k), radius=12 * k,
                         fill=C_PANEL_ALPHA)
    width = max(1, round(1.6 * k))

    def both(method, shape, **kw):
        getattr(dg, method)(shape, **kw)
        kw.update({key: 255 for key in ('fill', 'outline') if key in kw})
        getattr(da, method)(shape, **kw)

    parts = [(eid, [[(x * k, y * k) for x, y in pl] for pl in pls],
              tuple(v * k for v in circle) if circle else None)
             for eid, pls, circle in shapes(C_SCALE, C_OX, C_OY)]
    for eid, polylines, _ in parts:          # the pad's body first, everything else lies on it
        if eid in FILLED:
            for pts in polylines:
                both('polygon', pts, fill=C_BODY)
    for eid, polylines, circle in parts:
        raised = eid.startswith(C_RAISED)
        for pts in polylines:
            if raised:
                both('polygon', pts, fill=C_PART)
            both('line', pts, fill=C_LINE, width=width, joint='curve')
        if circle:
            x, y, r = circle
            box = (x - r, y - r, x + r, y + r)
            both('ellipse', box, fill=C_PART if raised else None, outline=C_LINE, width=width)
    lead = 2 * k
    for (bx, by, gap), steps in C_LEADS:
        x, y = C_OX + bx * C_SCALE, C_OY + by * C_SCALE
        if not gap:                          # a stick: a dot in its middle
            both('ellipse', ((x - 4) * k, (y - 4) * k, (x + 4) * k, (y + 4) * k), fill=C_LEAD)
        first, gap = steps[0][0], gap * C_SCALE
        x, y = x + gap * ((first == 'r') - (first == 'l')), y + gap * ((first == 'd') - (first == 'u'))
        pts = [(x, y)]
        for step in steps:
            if step[0] in 'xrl':
                x = float(step[1:])
            else:
                y = float(step[1:])
            pts.append((x, y))
        for a, b in zip(pts, pts[1:]):       # square corners: every piece one pixel-exact box
            box = (min(a[0], b[0]) * k - lead // 2, min(a[1], b[1]) * k - lead // 2,
                   max(a[0], b[0]) * k + lead // 2 - 1, max(a[1], b[1]) * k + lead // 2 - 1)
            both('rectangle', box, fill=C_LEAD)
    both('rectangle', (0, C_H * k, C_W * k, C_ROWS * k), fill=0)
    small = (C_W, C_ROWS)
    grey, alpha = grey.resize(small, Image.BOX), alpha.resize(small, Image.BOX)
    return Image.merge('RGBA', (grey, grey, grey, alpha))


def render():
    """The 512x256 RGBA picture: opaque black pad, grey lines, the line colour around it (alpha 0)."""
    k = DETAIL * SUPER
    size = (GAME_W * k, GAME_H * k)
    fill, line = Image.new('L', size, 0), Image.new('L', size, 0)
    df, dl = ImageDraw.Draw(fill), ImageDraw.Draw(line)
    width = max(1, round(LINE_WIDTH * k))
    for eid, polylines, circle in shapes():
        for pl in polylines:
            pts = [(x * k, y * k) for x, y in pl]
            if eid in FILLED:
                df.polygon(pts, fill=255)
            dl.line(pts, fill=255, width=width, joint='curve')
        if circle:
            x, y, r = (v * k for v in circle)
            dl.ellipse((x - r, y - r, x + r, y + r), outline=255, width=width)
    small = (GAME_W * DETAIL, GAME_H * DETAIL)
    fill, line = fill.resize(small, Image.BOX), line.resize(small, Image.BOX)
    out = Image.new('RGBA', small)
    pixels = []
    for f, l in zip(fill.tobytes(), line.tobytes()):
        alpha = max(f, l)
        grey = LINE_GREY if not alpha else min(LINE_GREY, round(LINE_GREY * l / alpha))
        pixels.append((grey, grey, grey, alpha))
    out.putdata(pixels)
    return out


# BC7 (the pack loader's colour format). Two of its block modes are enough for a grey picture with
# alpha: mode 5 (colour and alpha picked separately) and mode 6 (one line through all four); the
# one closer to the pixels is kept.
W2 = (0, 21, 43, 64)
W4 = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)


def _mix(a, b, w):
    return ((64 - w) * a + w * b + 32) >> 6


def _pick(values, lo, hi, weights):
    """Per value the closest step between lo and hi; returns (indices, squared error)."""
    steps = [_mix(lo, hi, w) for w in weights]
    indices, error = [], 0
    for v in values:
        best = min(range(len(steps)), key=lambda i: abs(steps[i] - v))
        indices.append(best)
        error += (steps[best] - v) ** 2
    return indices, error


class _Bits:
    def __init__(self):
        self.value, self.count = 0, 0

    def put(self, v, n):
        self.value |= (v & ((1 << n) - 1)) << self.count
        self.count += n

    def bytes(self):
        assert self.count == 128
        return self.value.to_bytes(16, 'little')


def _mode5(grey, alpha):
    g0, g1 = min(grey) >> 1, (max(grey) + 1) >> 1
    g1 = min(g1, 127)
    e0, e1 = (g0 << 1) | (g0 >> 6), (g1 << 1) | (g1 >> 6)
    a0, a1 = min(alpha), max(alpha)
    ci, ce = _pick(grey, e0, e1, W2)
    ai, ae = _pick(alpha, a0, a1, W2)
    if ci[0] & 2:
        g0, g1, ci = g1, g0, [3 - i for i in ci]
    if ai[0] & 2:
        a0, a1, ai = a1, a0, [3 - i for i in ai]
    b = _Bits()
    b.put(1 << 5, 6)
    b.put(0, 2)
    for _ in range(3):
        b.put(g0, 7)
        b.put(g1, 7)
    b.put(a0, 8)
    b.put(a1, 8)
    for indices in (ci, ai):
        b.put(indices[0], 1)
        for i in indices[1:]:
            b.put(i, 2)
    return b.bytes(), 3 * ce + ae


def _mode6(grey, alpha):
    # Ends: the pixels with the lowest and highest alpha (grey as the tie-break).
    order = sorted(range(16), key=lambda i: (alpha[i], grey[i]))
    lo, hi = order[0], order[-1]
    best = None
    for p0 in (0, 1):
        for p1 in (0, 1):
            q = lambda v, p: min(127, max(0, (v - p + 1) >> 1))
            g0, a0, g1, a1 = q(grey[lo], p0), q(alpha[lo], p0), q(grey[hi], p1), q(alpha[hi], p1)
            eg0, ea0, eg1, ea1 = (g0 << 1) | p0, (a0 << 1) | p0, (g1 << 1) | p1, (a1 << 1) | p1
            indices, error = [], 0
            for g, a in zip(grey, alpha):
                costs = [3 * (_mix(eg0, eg1, w) - g) ** 2 + (_mix(ea0, ea1, w) - a) ** 2 for w in W4]
                i = min(range(16), key=costs.__getitem__)
                indices.append(i)
                error += costs[i]
            if best is None or error < best[0]:
                best = (error, g0, a0, g1, a1, p0, p1, indices)
    error, g0, a0, g1, a1, p0, p1, indices = best
    if indices[0] & 8:
        g0, a0, p0, g1, a1, p1 = g1, a1, p1, g0, a0, p0
        indices = [15 - i for i in indices]
    b = _Bits()
    b.put(1 << 6, 7)
    for _ in range(3):
        b.put(g0, 7)
        b.put(g1, 7)
    b.put(a0, 7)
    b.put(a1, 7)
    b.put(p0, 1)
    b.put(p1, 1)
    b.put(indices[0], 3)
    for i in indices[1:]:
        b.put(i, 4)
    return b.bytes(), error


def bc7(image):
    w, h = image.size
    data = image.tobytes()
    out = bytearray()
    known = {}                               # most blocks of a drawing are one flat colour
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            grey, alpha = [], []
            for y in range(by, by + 4):
                for x in range(bx, bx + 4):
                    o = (min(y, h - 1) * w + min(x, w - 1)) * 4
                    grey.append(data[o])
                    alpha.append(data[o + 3])
            key = (bytes(grey), bytes(alpha))
            if key not in known:
                known[key] = min(_mode5(grey, alpha), _mode6(grey, alpha), key=lambda r: r[1])[0]
            out += known[key]
    return bytes(out)


def dds(image):
    """BC7 DDS with a DX10 header and a full mip chain: what vk_texture_pack.cpp reads."""
    w, h = image.size
    levels, mips = [], image
    while True:
        levels.append(bc7(mips))
        if mips.size == (1, 1):
            break
        mips = mips.resize((max(1, mips.width // 2), max(1, mips.height // 2)), Image.BOX)
    header = struct.pack('<4s7I44x', b'DDS ', 124, 0x000A1007, h, w, len(levels[0]), 0, len(levels))
    header += struct.pack('<2I4s20x', 32, 4, b'DX10')      # pixel format
    header += struct.pack('<I16x', 0x00401008)             # caps: texture, mipmap, complex
    header += struct.pack('<5I', 98, 3, 0, 1, 0)           # DXGI_FORMAT_BC7_UNORM, 2D, 1 image
    assert len(header) == 4 + 124 + 20
    return header + b''.join(levels)


def glow_table():
    rows = []
    for name, (x, y) in GLOW.items():
        rows.append(f'{name}={{{OX + x * SCALE - GLOW_SHIFT[0]:.1f},{OY + y * SCALE - GLOW_SHIFT[1]:.1f}}}')
    return ','.join(rows)


def main():
    args = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    args.add_argument('--out', default=str(ROOT / 'gpu' / 'assets' / 'pad_xbox.dds'))
    args.add_argument('--png', help='also write a preview PNG here')
    args = args.parse_args()
    image = render()
    if args.png:
        image.save(args.png)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(dds(image))
    print(f'{out}: {out.stat().st_size} bytes, {image.width}x{image.height}')
    image = render_controls()
    if args.png:
        image.save(str(Path(args.png).with_name(Path(args.png).stem + '_controls.png')))
    out = out.with_name(out.stem + '_controls.dds')
    out.write_bytes(dds(image))
    print(f'{out}: {out.stat().st_size} bytes, {image.width}x{image.height}')
    print('Glow table (tlg_lua.cpp XBOXPAD):')
    print(glow_table())


if __name__ == '__main__':
    main()
