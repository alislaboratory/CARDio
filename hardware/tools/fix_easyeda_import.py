"""Post-import fixes for the EasyEDA -> KiCad conversion of the CARDio PCB.

KiCad's EasyEDA Std importer gets the routing exactly right, but it turns the
filled regions (SOLIDREGION) inside EasyEDA footprints into footprint zones,
which KiCad never plots. On this board that silently loses:

  * the copper of the ANT1 inverted-F Wi-Fi/BLE antenna (no antenna at all!);
  * the solder-paste apertures of every LCSC part that draws custom paste
    shapes (all the passives, the ESP32-C3 and the USB-C socket).

It also shows reference/value labels EasyEDA kept hidden, and lays out text in
Source Code Pro with different glyph spacing. This script fixes all of that,
plus one deliberate change (a copper label that sat 0.026 mm from a GND track).
Verified by plotting Gerbers and diffing them against the original JLCPCB set:
copper, mask, paste and outline match; see hardware/README.md.

Run with KiCad's bundled Python (it provides `pcbnew`):

    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" fix_easyeda_import.py \
        ../easyeda/CARDio_pcb.json ../kicad/CARDio.kicad_pcb
"""

import json
import os
import re
import sys

import pcbnew

EASYEDA_UNIT_MM = 0.254  # EasyEDA Std works in 10 mil units


def easyeda_footprints(path):
    data = json.load(open(path, encoding="utf-8"))
    for shape in data["shape"]:
        if not shape.startswith("LIB"):
            continue
        parts = shape.split("#@$")
        ref = next((p.split("~")[10] for p in parts if p.startswith("TEXT~P")), None)
        yield ref, parts


def signature(fp):
    """What makes two footprints the same part: pads and graphics, in footprint coordinates."""
    def mm(v):
        return tuple(round(pcbnew.ToMM(c), 3) for c in (v.x, v.y))
    pads = sorted((p.GetNumber(), mm(p.GetPosition()), mm(p.GetSize(pcbnew.F_Cu)), p.GetShape(pcbnew.F_Cu),
                   round(p.GetOrientationDegrees(), 2), p.GetLayerSet().FmtHex()) for p in fp.Pads())
    gfx = sorted((g.GetLayer(), mm(g.GetBoundingBox().GetOrigin()), mm(g.GetBoundingBox().GetEnd()))
                 for g in fp.GraphicalItems() if g.GetClass() == "PCB_SHAPE")
    return pads, gfx


def parse_path(d):
    nums = [float(n) for n in re.findall(r"-?\d+(?:\.\d+)?", d)]
    return list(zip(nums[0::2], nums[1::2]))


def main(json_path, pcb_path):
    board = pcbnew.LoadBoard(pcb_path)
    fps = dict(easyeda_footprints(json_path))

    # --- 1. Restore the antenna copper ----------------------------------------
    parts = fps["ANT1"]
    region = next(p for p in parts if p.startswith("SOLIDREGION~1~"))  # layer 1 = top copper
    outline = parse_path(region.split("~")[3])
    ee_pad = next(p for p in parts if p.startswith("PAD~") and p.split("~")[8] == "ANT").split("~")
    ee_pad_xy = (float(ee_pad[2]), float(ee_pad[3]))

    fp = board.FindFootprintByReference("ANT1")
    pad = next(p for p in fp.Pads() if p.GetNumber() == "ANT")
    pad_xy = pcbnew.ToMM(pad.GetPosition())
    # EasyEDA and KiCad differ only by scale and a fixed offset; anchor it on the pad.
    off = (pad_xy[0] - ee_pad_xy[0] * EASYEDA_UNIT_MM, pad_xy[1] - ee_pad_xy[1] * EASYEDA_UNIT_MM)
    board_pts = [(x * EASYEDA_UNIT_MM + off[0], y * EASYEDA_UNIT_MM + off[1]) for x, y in outline]

    # An inverted-F antenna is deliberately shorted to ground at one end, so its
    # copper joins the ANT and GND pads. KiCad models that as a net-tie
    # footprint: copper shapes inside it may connect the listed pads. A
    # footprint shape (rather than a custom pad) also keeps the antenna under
    # solder mask with no paste, as on the original.
    shape = pcbnew.PCB_SHAPE(fp)
    shape.SetShape(pcbnew.SHAPE_T_POLY)
    shape.SetLayer(pcbnew.F_Cu)
    shape.SetFilled(True)
    shape.SetWidth(0)
    shape.SetPolyPoints([pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)) for x, y in board_pts])
    fp.Add(shape)
    fp.AddNetTiePadGroup("ANT, GND")
    # The importer did keep the region, but as a footprint zone, which KiCad
    # never plots; drop that unplotted duplicate.
    for zone in list(fp.Zones()):
        if zone.GetLayer() == pcbnew.F_Cu:
            fp.Remove(zone)

    # Sanity check: the antenna must touch its feed pad.
    if not shape.GetEffectiveShape().Collide(pad.GetEffectiveShape(pcbnew.F_Cu), 0):
        sys.exit("antenna polygon misplaced: it doesn't reach the ANT pad")
    print(f"ANT1: restored {len(board_pts)}-point antenna copper (net tie ANT/GND)")

    # --- 2. Hide reference/value text EasyEDA kept hidden ----------------------
    hidden = 0
    for ref, parts in fps.items():
        fp = board.FindFootprintByReference(ref) if ref else None
        if not fp:
            continue
        for p in parts:
            f = p.split("~")
            if f[0] == "TEXT" and f[1] in ("P", "N"):
                # field 12 is EasyEDA's display flag ("none" = hidden)
                visible = not (len(f) > 12 and f[12] == "none")
                field = fp.Reference() if f[1] == "P" else fp.Value()
                if not visible and field.IsVisible():
                    field.SetVisible(False)
                    hidden += 1
    print(f"hid {hidden} reference/value labels that were hidden in EasyEDA")

    # --- 3. Solder paste (and a stray silkscreen region) ----------------------
    # Many LCSC footprints draw custom (notched) paste apertures as regions on
    # the paste layer. The importer turns those into zones inside the footprint
    # and disables the pads' own paste -- but KiCad doesn't plot footprint
    # zones on paste layers, so the stencil would have no openings for these
    # parts at all. Turn each one into a filled paste shape instead.
    converted = 0
    for fp in board.GetFootprints():
        for zone in list(fp.Zones()):
            layer = zone.GetLayer()
            if layer not in (pcbnew.F_Paste, pcbnew.B_Paste, pcbnew.F_SilkS, pcbnew.B_SilkS):
                continue
            outline = zone.Outline().Outline(0)
            shape = pcbnew.PCB_SHAPE(fp)
            shape.SetShape(pcbnew.SHAPE_T_POLY)
            shape.SetLayer(layer)
            shape.SetFilled(True)
            shape.SetWidth(0)
            shape.SetPolyPoints([outline.CPoint(i) for i in range(outline.PointCount())])
            fp.Add(shape)
            fp.Remove(zone)
            converted += 1
            # The importer suppressed these pads' own paste with a -100 mm
            # margin; take the paste layer off instead (same stencil, no DRC noise).
            for pad in fp.Pads():
                margin = pad.GetLocalSolderPasteMargin()
                if margin is not None and margin <= pcbnew.FromMM(-50):
                    ls = pad.GetLayerSet()
                    ls.RemoveLayer(pcbnew.F_Paste)
                    ls.RemoveLayer(pcbnew.B_Paste)
                    pad.SetLayerSet(ls)
                    pad.SetLocalSolderPasteMargin(None)
    print(f"converted {converted} footprint paste/silk zones into plotted shapes")

    # --- 4. Text in Source Code Pro ---------------------------------------------
    # EasyEDA and KiCad space glyphs differently, so the same font at the same
    # nominal size comes out wider. Fit each text to the outline EasyEDA stored.
    data = json.load(open(json_path, encoding="utf-8"))
    targets = {}
    for s in data["shape"]:
        f = s.split("~")
        if f[0] == "TEXT" and len(f) > 14 and f[14] == "Source Code Pro SemiBold":
            pts = parse_path(f[11])
            xs = [x * EASYEDA_UNIT_MM + off[0] for x, _ in pts]
            ys = [y * EASYEDA_UNIT_MM + off[1] for _, y in pts]
            targets[f[10]] = (min(xs), min(ys), max(xs), max(ys))

    for item in board.GetDrawings():
        if not isinstance(item, pcbnew.PCB_TEXT) or item.GetText() not in targets:
            continue
        x0, y0, x1, y1 = targets[item.GetText()]
        for _ in range(3):  # converges in one or two passes
            bb = item.GetBoundingBox()
            w, h = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
            size = item.GetTextSize()
            item.SetTextSize(pcbnew.VECTOR2I(int(size.x * (x1 - x0) / w), int(size.y * (y1 - y0) / h)))
            bb = item.GetBoundingBox()
            item.Move(pcbnew.VECTOR2I(pcbnew.FromMM(x0) - bb.GetLeft(), pcbnew.FromMM(y0) - bb.GetTop()))
        bb = item.GetBoundingBox()
        err = max(abs(pcbnew.ToMM(bb.GetLeft()) - x0), abs(pcbnew.ToMM(bb.GetRight()) - x1),
                  abs(pcbnew.ToMM(bb.GetTop()) - y0), abs(pcbnew.ToMM(bb.GetBottom()) - y1))
        print(f"fitted text {item.GetText()!r} to the original outline (max edge error {err:.3f} mm)")

    # Hidden value labels with an 'Ω' were given a CJK font that isn't installed
    # anywhere; use KiCad's built-in font instead.
    for fp in board.GetFootprints():
        for field in fp.GetFields():
            if field.GetFontName() == "NotoSerifCJKsc-Medium":
                field.SetFont(None)

    # --- 5. The one deliberate layout change -------------------------------------
    # The floating "3V3" copper label sat 0.026 mm from a GND track on the
    # original (electrically harmless, but below any fab's clearance). Nudge it
    # the smallest distance that clears the 0.15 mm rule.
    label = next(d for d in board.GetDrawings() if isinstance(d, pcbnew.PCB_TEXT) and d.GetText() == "3V3")
    copper = [t for t in board.GetTracks() if t.IsOnLayer(label.GetLayer())]

    def collides(clearance_mm):
        shape = label.GetEffectiveShape(label.GetLayer())
        return any(shape.Collide(t.GetEffectiveShape(label.GetLayer()), pcbnew.FromMM(clearance_mm)) for t in copper)

    if collides(0.15):
        best = None
        for step in range(1, 41):  # up to 0.4 mm, in 10 um steps
            d = pcbnew.FromMM(step * 0.01)
            for dx, dy in ((0, -d), (0, d), (-d, 0), (d, 0)):
                label.Move(pcbnew.VECTOR2I(dx, dy))
                ok = not collides(0.16)
                label.Move(pcbnew.VECTOR2I(-dx, -dy))
                if ok:
                    best = (dx, dy)
                    break
            if best:
                break
        if not best:
            sys.exit("couldn't find a clear spot for the 3V3 label")
        label.Move(pcbnew.VECTOR2I(*best))
        print(f"moved the '3V3' copper label {pcbnew.ToMM(max(abs(best[0]), abs(best[1]))):.2f} mm to clear a GND track")

    # --- 6. Project footprint library ----------------------------------------------
    # The importer embeds footprints with no library name. Save one copy of each
    # into CARDio.pretty next to the board and point every footprint at it, so
    # the project is self-contained and the schematic can reference them.
    lib_dir = os.path.join(os.path.dirname(os.path.abspath(pcb_path)), "CARDio.pretty")
    io = pcbnew.PCB_IO_KICAD_SEXPR()
    if os.path.isdir(lib_dir):
        for f in os.listdir(lib_dir):
            os.remove(os.path.join(lib_dir, f))
    else:
        io.CreateLibrary(lib_dir)
    protos = {}  # library name -> normalised footprint
    # Build each library copy from an unrotated, front-side instance where there is one.
    order = sorted(board.GetFootprints(),
                   key=lambda f: (f.IsFlipped(), f.GetOrientationDegrees() % 360 != 0, f.GetReference()))
    for fp in order:
        proto = pcbnew.FOOTPRINT(fp)  # a copy, normalised to the origin, front side, 0 deg
        if proto.IsFlipped():
            proto.Flip(proto.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        proto.SetOrientationDegrees(0)
        proto.SetPosition(pcbnew.VECTOR2I(0, 0))
        proto.SetReference("REF**")
        base = fp.GetFPID().GetLibItemName().wx_str().replace(" ", "_")
        # EasyEDA lets different LCSC parts share a footprint name with slightly
        # different geometry; keep each distinct variant rather than merging them.
        name, n = base, 1
        while name in protos and signature(protos[name]) != signature(proto):
            n += 1
            name = f"{base}_v{n}"
        if name not in protos:
            proto.SetFPID(pcbnew.LIB_ID("", name))
            io.FootprintSave(lib_dir, proto)
            protos[name] = proto
        fp.SetFPID(pcbnew.LIB_ID("CARDio", name))
    saved = protos

    # Re-instantiate every footprint from its library copy (what "Update
    # Footprints from Library" does), so rotated instances don't carry the
    # importer's cosmetic artefacts. Position, side, rotation, nets and labels
    # are kept; the pads and graphics were already checked identical above.
    for fp in list(board.GetFootprints()):
        lib_fp = pcbnew.FOOTPRINT(protos[fp.GetFPID().GetLibItemName().wx_str()])
        lib_fp.SetFPID(fp.GetFPID())
        if fp.IsFlipped():
            lib_fp.Flip(lib_fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        lib_fp.SetOrientation(fp.GetOrientation())
        lib_fp.SetPosition(fp.GetPosition())
        for new_field, old_field in ((lib_fp.Reference(), fp.Reference()), (lib_fp.Value(), fp.Value())):
            new_field.SetText(old_field.GetText())
            new_field.SetVisible(old_field.IsVisible())
            new_field.SetPosition(old_field.GetPosition())
            new_field.SetTextAngle(old_field.GetTextAngle())
        for pad in lib_fp.Pads():
            old = fp.FindPadByNumber(pad.GetNumber())
            if old:
                pad.SetNet(old.GetNet())
        lib_fp.SetPath(fp.GetPath())
        board.Remove(fp)
        board.Add(lib_fp)
    table = os.path.join(os.path.dirname(lib_dir), "fp-lib-table")
    with open(table, "w", encoding="utf-8") as f:
        f.write('(fp_lib_table\n\t(version 7)\n\t(lib (name "CARDio") (type "KiCad") '
                '(uri "${KIPRJMOD}/CARDio.pretty") (options "") '
                '(descr "CARDio footprints (LCSC/EasyEDA parts used on the board)"))\n)\n')
    print(f"saved {len(saved)} footprints to CARDio.pretty and linked the board to it")

    board.EmbedFonts()  # so the board renders the same on machines without the font
    board.Save(pcb_path)

    # --- 6. Physical stackup -------------------------------------------------------
    # EasyEDA doesn't export one. Record what the cards were made with: 4 layers,
    # 1.6 mm, black solder mask, white silkscreen, lead-free HASL. (KiCad's Python
    # API doesn't expose the stackup, so it goes straight into the file.)
    text = open(pcb_path, encoding="utf-8").read()
    if "(stackup" not in text:
        layers = [
            ("F.SilkS", "Top Silk Screen", None, "White"),
            ("F.Paste", "Top Solder Paste", None, None),
            ("F.Mask", "Top Solder Mask", 0.01, "Black"),
            ("F.Cu", "copper", 0.035, None),
            ("dielectric 1", "prepreg", 0.1, None),
            ("In1.Cu", "copper", 0.035, None),
            ("dielectric 2", "core", 1.24, None),
            ("In2.Cu", "copper", 0.035, None),
            ("dielectric 3", "prepreg", 0.1, None),
            ("B.Cu", "copper", 0.035, None),
            ("B.Mask", "Bottom Solder Mask", 0.01, "Black"),
            ("B.Paste", "Bottom Solder Paste", None, None),
            ("B.SilkS", "Bottom Silk Screen", None, "White"),
        ]
        out = ["\t\t(stackup"]
        for name, kind, thickness, color in layers:
            item = f'\t\t\t(layer "{name}"\n\t\t\t\t(type "{kind}")'
            if color:
                item += f'\n\t\t\t\t(color "{color}")'
            if thickness:
                item += f"\n\t\t\t\t(thickness {thickness})"
            if kind in ("prepreg", "core"):
                item += '\n\t\t\t\t(material "FR4")\n\t\t\t\t(epsilon_r 4.5)\n\t\t\t\t(loss_tangent 0.02)'
            out.append(item + "\n\t\t\t)")
        out.append('\t\t\t(copper_finish "HAL lead-free")\n\t\t\t(dielectric_constraints no)\n\t\t)')
        text = text.replace("\t(setup\n", "\t(setup\n" + "\n".join(out) + "\n", 1)
        open(pcb_path, "w", encoding="utf-8").write(text)
        print("added the board stackup: 4 layers, 1.6 mm, black mask, white silkscreen")


if __name__ == "__main__":
    main(*sys.argv[1:3])
