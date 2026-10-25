"""Check the KiCad board against the Gerbers the cards were actually made from.

Rasterises each layer of both Gerber sets at 20 px/mm, aligns them on the board
outline and reports the area that differs. Writes an overlay PNG per layer:
red = only in the original, green = only in KiCad, yellow = both.

    pip install gerbonara pillow
    # unzip ../fabrication/CARDio_Rev3.0_gerbers.zip into orig/, then:
    kicad-cli pcb export gerbers --no-protel-ext -o kicad/ ../kicad/CARDio.kicad_pcb
    python compare_gerbers.py orig/ kicad/ diff/
"""
import math
import sys
from pathlib import Path

import gerbonara
from gerbonara import graphic_primitives as gp
from gerbonara.utils import MM
from PIL import Image, ImageChops, ImageDraw

orig_dir, kicad_dir, out_dir = map(Path, sys.argv[1:4])
out_dir.mkdir(exist_ok=True)
PX = 20  # px per mm

PAIRS = [
    ("outline", "Gerber_BoardOutlineLayer.GKO", "CARDio-BoardOutLine.gbr"),
    ("top copper", "Gerber_TopLayer.GTL", "CARDio-TopLayer.gbr"),
    ("inner 1", "Gerber_InnerLayer1.G1", "CARDio-Inner1.gbr"),
    ("inner 2", "Gerber_InnerLayer2.G2", "CARDio-Inner2.gbr"),
    ("bottom copper", "Gerber_BottomLayer.GBL", "CARDio-BottomLayer.gbr"),
    ("top mask", "Gerber_TopSolderMaskLayer.GTS", "CARDio-TopSolderMaskLayer.gbr"),
    ("bottom mask", "Gerber_BottomSolderMaskLayer.GBS", "CARDio-BottomSolderMaskLayer.gbr"),
    ("top paste", "Gerber_TopPasteMaskLayer.GTP", "CARDio-TopPasteMaskLayer.gbr"),
    ("bottom paste", "Gerber_BottomPasteMaskLayer.GBP", "CARDio-BottomPasteMaskLayer.gbr"),
    ("top silk", "Gerber_TopSilkscreenLayer.GTO", "CARDio-TopSilkLayer.gbr"),
    ("bottom silk", "Gerber_BottomSilkscreenLayer.GBO", "CARDio-BottomSilkLayer.gbr"),
]


def prims(path):
    f = gerbonara.rs274x.GerberFile.open(path)
    out = []
    for obj in f.objects:
        for p in obj.to_primitives(MM):
            out.append((p, getattr(obj, "polarity_dark", True)))
    return out


def bbox(path):
    f = gerbonara.rs274x.GerberFile.open(path)
    (x0, y0), (x1, y1) = f.bounding_box(MM)
    return x0, y0, x1, y1


def raster(path, origin, size):
    ox, oy = origin
    w, h = size
    img = Image.new("L", (int(w * PX) + 1, int(h * PX) + 1), 0)
    d = ImageDraw.Draw(img)

    def P(x, y):
        return ((x - ox) * PX, (h - (y - oy)) * PX)

    for p, dark in prims(path):
        col = 255 if dark else 0
        if isinstance(p, gp.Circle):
            cx, cy = P(p.x, p.y)
            r = p.r * PX
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
        elif isinstance(p, gp.Line):
            a, b = P(p.x1, p.y1), P(p.x2, p.y2)
            wd = max(1, p.width * PX)
            d.line([a, b], fill=col, width=int(round(wd)))
            for c in (a, b):
                r = wd / 2
                d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=col)
        elif isinstance(p, gp.Arc):
            pts = []
            cx, cy = p.cx, p.cy  # gerbonara arc centres are absolute
            a1 = math.atan2(p.y1 - cy, p.x1 - cx)
            a2 = math.atan2(p.y2 - cy, p.x2 - cx)
            if p.clockwise:
                while a2 > a1:
                    a2 -= 2 * math.pi
            else:
                while a2 < a1:
                    a2 += 2 * math.pi
            r0 = math.hypot(p.x1 - cx, p.y1 - cy)
            n = max(8, int(abs(a2 - a1) * r0 * PX / 2))
            for i in range(n + 1):
                a = a1 + (a2 - a1) * i / n
                pts.append(P(cx + r0 * math.cos(a), cy + r0 * math.sin(a)))
            wd = max(1, p.width * PX)
            d.line(pts, fill=col, width=int(round(wd)), joint="curve")
        elif isinstance(p, gp.Rectangle):
            corners = []
            c, s = math.cos(p.rotation), math.sin(p.rotation)
            for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                lx, ly = dx * p.w / 2, dy * p.h / 2
                corners.append(P(p.x + lx * c - ly * s, p.y + lx * s + ly * c))
            d.polygon(corners, fill=col)
        elif isinstance(p, gp.ArcPoly):
            pts = [P(x, y) for x, y in p.outline]
            if len(pts) >= 3:
                d.polygon(pts, fill=col)
        else:
            pass
    return img


# Align both exports on their board outlines (EasyEDA and KiCad use different origins).
ob = bbox(orig_dir / PAIRS[0][1])
kb = bbox(kicad_dir / PAIRS[0][2])
w, h = ob[2] - ob[0], ob[3] - ob[1]
print(f"outline orig {w:.3f} x {h:.3f} mm | kicad {kb[2]-kb[0]:.3f} x {kb[3]-kb[1]:.3f} mm")
for name, o, k in PAIRS:
    a = raster(orig_dir / o, ob[:2], (w, h))
    b = raster(kicad_dir / k, kb[:2], (w, h))
    b = b.resize(a.size)
    diff = ImageChops.difference(a, b).point(lambda v: 255 if v > 128 else 0)
    # ignore 1-2 px edge noise from rasterisation
    from PIL import ImageFilter
    diff = diff.filter(ImageFilter.MinFilter(5))
    nd = sum(1 for v in diff.get_flattened_data() if v)
    na = max(1, sum(1 for v in a.get_flattened_data() if v))
    print(f"{name:14s} differing area {nd/PX/PX:8.2f} mm^2  ({100*nd/na:5.2f}% of original's {na/PX/PX:8.1f} mm^2)")
    rgb = Image.merge("RGB", (a, b, Image.new("L", a.size, 0)))  # red=orig only, green=kicad only, yellow=both
    rgb.thumbnail((1400, 1400))
    rgb.save(out_dir / f"{name.replace(' ', '_')}.png")
