# SPDX-License-Identifier: GPL-3.0-only
# Fluvi brand: regenerates fluvi-mark.svg, fluvi-wordmark.svg, fluvi-icon.svg, fluvi-mark-256.png and fluvi.ico.
# Usage: uv run --no-project --with skia-python --with fonttools python make-brand.py <out dir> <BricolageGrotesqueCondensed-ExtraBold.ttf> <preview dir>
import sys, os, io, struct
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.boundsPen import BoundsPen
import skia

out, font_path, shots = sys.argv[1], sys.argv[2], sys.argv[3]
AMBER, FOAM, BED, TILE = '#FFB547', '#EEF5F2', '#0A2228', '#10313A'

# The mark: a meander (integral-like S) that is a lowercase italic "f"; the crossbar is a small wave.
def mark_paths(w):
    return (f'<path d="M47.5 13.5C45.6 9 41.7 6.5 37 6.5C30.6 6.5 27 11 27 18.5V43.5C27 51.5 22.6 57.5 15 57.5" '
            f'fill="none" stroke="{AMBER}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="M15 28.5Q20.5 24 26 28.5T38 28.5" fill="none" stroke="{AMBER}" stroke-width="{w}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')

def mark_svg(w=6.5):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">{mark_paths(w)}</svg>\n'

def icon_svg(w):
    # App icon: the mark on a river-bed tile, so it reads on light and dark taskbars.
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">'
            f'<rect x="1" y="1" width="62" height="62" rx="15" fill="{BED}"/>'
            f'<rect x="1.5" y="1.5" width="61" height="61" rx="14.5" fill="none" stroke="{TILE}" stroke-width="1"/>'
            f'<g transform="translate(32 32) scale(0.86) translate(-31 -32)">{mark_paths(w)}</g></svg>\n')

# Wordmark: "Fluvi" outlines; the dot of the i is the lantern (amber).
f = TTFont(font_path)
gs = f.getGlyphSet(); cmap = f.getBestCmap(); hmtx = f['hmtx']
upm = f['head'].unitsPerEm
track = -0.01 * upm
x = 0; parts = []; ymax = [0]
for ch in 'Fluvi':
    g = cmap[ord(ch)]
    rec = DecomposingRecordingPen(gs); gs[g].draw(rec)
    # split contours
    contours, cur = [], []
    for op, args in rec.value:
        cur.append((op, args))
        if op in ('closePath', 'endPath'):
            contours.append(cur); cur = []
    for i, c in enumerate(contours):
        pen = SVGPathPen(gs)
        for op, args in c: getattr(pen, op)(*args)
        bp = BoundsPen(gs)
        for op, args in c: getattr(bp, op)(*args)
        is_dot = ch == 'i' and bp.bounds and bp.bounds[1] > 0.6 * f['OS/2'].sCapHeight
        parts.append((x, pen.getCommands(), AMBER if is_dot else FOAM))
        if bp.bounds: ymax[0] = max(ymax[0], bp.bounds[3])
    x += hmtx[g][0] + track
asc = f['OS/2'].sCapHeight
# glyph y-up -> svg y-down; crop to cap height (+ small margin), no descenders in "Fluvi"
top = ymax[0] + 4
height = top + 4
width = x - track
paths = ''.join(f'<path transform="translate({px:.1f} {top:.1f}) scale(1 -1)" d="{d}" fill="{col}"/>' for px, d, col in parts)
wm = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height:.0f}" '
      f'width="{width/10:.1f}" height="{height/10:.1f}">{paths}</svg>\n')

def write(name, text):
    with open(os.path.join(out, name), 'w', encoding='utf-8', newline='\n') as fh: fh.write(text)

write('fluvi-mark.svg', mark_svg())
write('fluvi-wordmark.svg', wm)
write('fluvi-icon.svg', icon_svg(6.5))

def raster(svg_text, px):
    stream = skia.MemoryStream(svg_text.encode('utf-8'))
    dom = skia.SVGDOM.MakeFromStream(stream)
    dom.setContainerSize(skia.Size(px, px))
    surf = skia.Surface(px, px)
    with surf as canvas:
        canvas.clear(skia.ColorTRANSPARENT)
        canvas.scale(px / 64, px / 64)
        dom.render(canvas)
    return bytes(surf.makeImageSnapshot().encodeToData())

png256 = raster(icon_svg(6.5), 256)
open(os.path.join(out, 'fluvi-mark-256.png'), 'wb').write(png256)
# ICO: PNG entries; small sizes get a heavier stroke so the mark survives 16 px.
sizes = [16, 24, 32, 48, 64, 128, 256]
images = [raster(icon_svg(9 if s <= 24 else 7.5 if s <= 48 else 6.5), s) for s in sizes]
hdr = struct.pack('<HHH', 0, 1, len(sizes)); off = 6 + 16 * len(sizes); dirs = b''
for s, data in zip(sizes, images):
    dirs += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), off); off += len(data)
open(os.path.join(out, 'fluvi.ico'), 'wb').write(hdr + dirs + b''.join(images))

# Preview sheet for review.
sheet = skia.Surface(900, 300)
with sheet as c:
    c.clear(skia.Color(0x0A, 0x22, 0x28))
    for i, (s, data) in enumerate(zip(sizes, images)):
        img = skia.Image.MakeFromEncoded(skia.Data.MakeWithCopy(data))
        c.drawImage(img, 20 + sum(sizes[:i]) + 12 * i, 20)
    dom = skia.SVGDOM.MakeFromStream(skia.MemoryStream(mark_svg().encode()))
    c.save(); c.translate(20, 160); c.scale(2, 2); dom.render(c); c.restore()
    dom2 = skia.SVGDOM.MakeFromStream(skia.MemoryStream(wm.encode()))
    c.save(); c.translate(180, 170); c.scale(2.5, 2.5); dom2.render(c); c.restore()
sheet.makeImageSnapshot().save(os.path.join(shots, 'brand-sheet.png'), skia.kPNG)
print('ok', width, height)
