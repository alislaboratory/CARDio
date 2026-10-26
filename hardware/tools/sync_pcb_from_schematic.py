"""Bring the converted PCB in line with the converted schematic (the scriptable
part of KiCad's "Update PCB from Schematic"), without touching any copper.

After fix_easyeda_import.py and fix_easyeda_schematic.py, the board and the
schematic are connected identically but disagree on names: the board kept
EasyEDA's net names (U3_1, ANT1_ANT...) and the footprints lack the symbols'
part fields (manufacturer part, LCSC number...). This renames the board's nets
to the schematic's and copies the fields across, after checking that every
net groups exactly the same pads in both.

Run with KiCad's bundled Python:

    kicad-cli sch export netlist --format kicadxml -o net.xml ../kicad/CARDio.kicad_sch
    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" sync_pcb_from_schematic.py net.xml ../kicad/CARDio.kicad_pcb
"""

import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

import pcbnew

SKIP_FIELDS = {"Reference", "Value", "Footprint", "Description"}


def main(netlist_path, pcb_path):
    root = ET.parse(netlist_path).getroot()
    board = pcbnew.LoadBoard(pcb_path)

    # --- Nets ------------------------------------------------------------------
    sch_net = {}
    for net in root.iter("net"):
        for node in net.iter("node"):
            sch_net[(node.get("ref"), node.get("pin"))] = net.get("name")
    pads_by_pcb_net = defaultdict(list)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNumber() and pad.GetNetCode() > 0:
                pads_by_pcb_net[pad.GetNetname()].append((fp.GetReference(), pad.GetNumber()))

    renames = {}
    for pcb_name, pads in pads_by_pcb_net.items():
        names = {sch_net.get(p) for p in pads}
        if len(names) != 1 or None in names:
            sys.exit(f"net {pcb_name} doesn't map onto one schematic net: {sorted(map(str, names))}")
        (name,) = names
        if name != pcb_name:
            renames[pcb_name] = name
    if len(set(renames.values())) != len(renames):
        sys.exit("two board nets would merge into one schematic net; aborting")
    for old, new in renames.items():
        board.FindNet(old).SetNetname(new)
    print(f"renamed {len(renames)} board nets to match the schematic")

    # --- Fields ------------------------------------------------------------------
    copied = 0
    for comp in root.iter("comp"):
        fp = board.FindFootprintByReference(comp.get("ref"))
        if not fp:
            continue
        for field in comp.iter("field"):
            name, value = field.get("name"), field.text or ""
            if name in SKIP_FIELDS or not value:
                continue
            if not fp.HasField(name) or fp.GetFieldText(name) != value:
                fp.SetField(name, value)
                f = fp.GetField(name)
                f.SetVisible(False)
                f.SetLayer(pcbnew.F_Fab if not fp.IsFlipped() else pcbnew.B_Fab)
                copied += 1
    print(f"copied {copied} part fields onto footprints")
    board.Save(pcb_path)


if __name__ == "__main__":
    main(*sys.argv[1:3])
