"""Post-import fixes for the EasyEDA -> KiCad conversion of the CARDio schematic.

KiCad only imports EasyEDA schematics through the GUI (Schematic Editor ->
File -> Import -> Non-KiCad Schematic), and on this design the result has
broken connectivity: EasyEDA treats a wire ending on the middle of another
wire as connected, KiCad needs a junction dot there, and the importer only
placed 19 of the 51 needed. Thirteen nets (GND, VCC, 1V8, +5V, SDA, the
crystal and antenna networks...) came out split.

This script, run on a freshly imported CARDio.kicad_sch:

  * adds the missing junctions;
  * marks genuinely unused pins with no-connect flags;
  * renames the power LED to PWR, the designator the PCB and BOM use.

    python fix_easyeda_schematic.py ../kicad/CARDio.kicad_sch

Then check with:  kicad-cli sch erc  and  kicad-cli pcb drc --schematic-parity
"""

import math
import os
import re
import sys
import uuid

# Pins that are unused by design (pin numbers per the imported symbols).
NO_CONNECT = {
    "U1": ["18", "19", "20", "21", "22", "23", "24"],  # VDD_SPI + SPI pins: wired internally to the embedded flash
    "U2": ["1", "5", "6", "7", "8", "14"],               # MAX30102 N.C. pins
    "U4": ["4"],                                        # AMS1117 tab (same as VOUT, connected through pin 2)
    "USB1": ["3", "9", "13", "14"],                     # SBU1/2 unused; shell tabs
    "LED14": ["1"],                                     # DO of the last LED in the chain
}


def tokenize(text):
    return re.findall(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+', text)


def parse(text):
    stack, cur = [], []
    for t in tokenize(text):
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def first(node, key):
    r = find(node, key)
    return r[0] if r else None


def placed_pins(sch):
    """(ref, pin number, x, y) for every pin of every placed symbol."""
    libs = {}
    for sym in find(first(sch, "lib_symbols"), "symbol"):
        pins = []

        def walk(n):
            for c in n:
                if isinstance(c, list) and c:
                    if c[0] == "pin":
                        at, num = first(c, "at"), first(c, "number")
                        pins.append((num[1], float(at[1]), float(at[2])))
                    elif c[0] == "symbol":
                        walk(c)

        walk(sym)
        libs[sym[1]] = pins

    out = []
    for sym in find(sch, "symbol"):
        at = first(sym, "at")
        X, Y = float(at[1]), float(at[2])
        rot = math.radians(-float(at[3])) if len(at) > 3 else 0.0
        mirror = first(sym, "mirror")
        ref = next((p[2] for p in find(sym, "property") if p[1] == "Reference"), "?")
        for num, px, py in libs.get(first(sym, "lib_id")[1], []):
            py = -py  # library coordinates are y-up
            if mirror and mirror[1] == "x":
                py = -py
            if mirror and mirror[1] == "y":
                px = -px
            x = px * math.cos(rot) - py * math.sin(rot)
            y = px * math.sin(rot) + py * math.cos(rot)
            out.append((ref, num, round(X + x, 4), round(Y + y, 4)))
    return out


def key(p):
    return (round(p[0], 2), round(p[1], 2))


def on_middle(p, seg):
    (x1, y1), (x2, y2) = seg
    if key(p) in (key(seg[0]), key(seg[1])):
        return False
    cross = (x2 - x1) * (p[1] - y1) - (y2 - y1) * (p[0] - x1)
    return (abs(cross) < 1e-3 and min(x1, x2) - 1e-3 <= p[0] <= max(x1, x2) + 1e-3
            and min(y1, y2) - 1e-3 <= p[1] <= max(y1, y2) + 1e-3)


def main(path):
    text = open(path, encoding="utf-8").read()
    sch = parse(text)
    segs = [[(float(p[1]), float(p[2])) for p in first(w, "pts")[1:]] for w in find(sch, "wire")]
    segs = [(s[0], s[-1]) for s in segs]
    have = {key((float(first(j, "at")[1]), float(first(j, "at")[2]))) for j in find(sch, "junction")}
    pins = placed_pins(sch)

    # 1. Junctions wherever a wire end or a pin lands on the middle of a wire.
    candidates = [p for s in segs for p in s] + [(x, y) for *_, x, y in pins]
    need = sorted({key(p) for p in candidates if key(p) not in have and any(on_middle(p, s) for s in segs)})
    items = [f'\t(junction\n\t\t(at {x:g} {y:g})\n\t\t(diameter 0)\n\t\t(color 0 0 0 0)\n\t\t(uuid "{uuid.uuid4()}")\n\t)'
             for x, y in need]

    # 2. No-connect flags on pins that are unused by design.
    by_pin = {(r, n): (x, y) for r, n, x, y in pins}
    have_nc = {key((float(first(n, "at")[1]), float(first(n, "at")[2]))) for n in find(sch, "no_connect")}
    nc = 0
    for ref, nums in NO_CONNECT.items():
        for n in nums:
            xy = by_pin.get((ref, n))
            if xy and key(xy) not in have_nc:
                items.append(f'\t(no_connect\n\t\t(at {xy[0]:g} {xy[1]:g})\n\t\t(uuid "{uuid.uuid4()}")\n\t)')
                nc += 1

    # Insert before the sheet_instances / embedded_fonts block at the end of the file.
    anchor = re.search(r"\n\t\((?:sheet_instances|embedded_fonts)", text)
    pos = anchor.start() if anchor else text.rstrip().rfind(")")
    text = text[:pos] + "\n" + "\n".join(items) + text[pos:]

    # 3. The PCB (and the BOM that was ordered) call the power LED "PWR".
    text, renamed = re.subn(r'\(property "Reference" "LED11"', '(property "Reference" "PWR"', text)
    text = re.sub(r'\(reference "LED11"\)', '(reference "PWR")', text)
    print(f"added {len(need)} junctions, {nc} no-connect flags; renamed LED11 -> PWR: {bool(renamed)}")

    text = set_pin_types(text)
    text = add_pwr_flag(text, by_pin[("L3", "2")])
    text = exclude_page_frame(text)
    text = build_symbol_library(text, path)
    text = link_footprints(text, path)
    open(path, "w", encoding="utf-8").write(text)


# --- Electrical pin types ---------------------------------------------------------
# EasyEDA doesn't record pin types, so the importer made every passive pin an
# "input" and every IC pin "unspecified", and ERC can't tell what drives what.

PASSIVE_PREFIXES = ("CC0603", "CL", "GJM", "GRM", "RC0", "LQ", "L327", "USBLC6", "WIFI_ANTENNA", "A-SP192")


def pin_type(symbol, name, number):
    s = re.sub(r"_\d+$", "", symbol)
    if s.startswith(PASSIVE_PREFIXES):
        return "passive"
    if s.startswith("AMS1117"):
        # One power output per supply net; the duplicate VOUT/TAB pin is passive.
        if name in ("VIN", "GND", "ADJ(GND)"):
            return "power_in"
        return "power_out" if number == "2" else "passive"
    if s.startswith("TYPE-C"):
        # The USB socket is where the board's power comes from.
        if (name, number) in (("VBUS", "2"), ("GND", "1")):
            return "power_out"
        return "passive"
    if s.startswith("ESP32-C3"):
        if name.startswith(("VDD3P3", "VDDA")) or name == "GND":
            return "power_in"
        if name == "CHIP_EN":
            return "input"
        if name == "EP":  # exposed pad = ground
            return "power_in"
        if name in ("LNA_IN", "VDD_SPI") or name.startswith(("XTAL", "SPI")):
            return "passive"
        return "bidirectional"
    if s.startswith("MAX30102"):
        return {"N.C.": "no_connect", "SCL": "input", "SDA": "bidirectional", "INT#": "open_collector",
                "VLED+": "power_in", "VDD": "power_in", "GND": "power_in", "PGND": "power_in"}.get(name, "passive")
    if s.startswith("WS2812B"):
        return {"VDD": "power_in", "GND": "power_in", "DI": "input", "DO": "output"}[name]
    return None  # power symbols etc.: leave as imported


def set_pin_types(text):
    lines = text.split("\n")
    symbol, pending, changed = None, None, 0
    name = None
    for i, line in enumerate(lines):
        m = re.match(r'^\t\t\(symbol "([^"]+)"$', line)
        if m:
            symbol = m.group(1)
        m = re.match(r"^(\t+)\(pin (\w+) (\w+)$", line)
        if m and symbol:
            pending, name = i, None
            continue
        if pending is not None:
            m = re.search(r'\(name "([^"]*)"', line)
            if m:
                name = m.group(1)
            m = re.search(r'\(number "([^"]*)"', line)
            if m:
                new = pin_type(symbol, name, m.group(1))
                indent, old, style = re.match(r"^(\t+)\(pin (\w+) (\w+)$", lines[pending]).groups()
                if new and new != old:
                    lines[pending] = f"{indent}(pin {new} {style}"
                    changed += 1
                pending = None
    print(f"set electrical types on {changed} pins")
    return "\n".join(lines)


KICAD_POWER_LIB = [
    os.path.expandvars(r"%LOCALAPPDATA%/Programs/KiCad/10.0/share/kicad/symbols/power.kicad_sym"),
    r"C:/Program Files/KiCad/10.0/share/kicad/symbols/power.kicad_sym",
    "/usr/share/kicad/symbols/power.kicad_sym",
    "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/power.kicad_sym",
]


def add_pwr_flag(text, xy):
    """The ESP32's RF supply (VDD3P3) is VCC through an LC filter (L3/C7): no
    power-output pin drives it, so ERC needs a PWR_FLAG to know it's powered."""
    lib = next((p for p in KICAD_POWER_LIB if os.path.exists(p)), None)
    if lib is None:
        print("KiCad's power.kicad_sym not found; skipping PWR_FLAG")
        return text
    src = open(lib, encoding="utf-8").read()
    s = src.index('\t(symbol "PWR_FLAG"\n')
    block = src[s:src.index("\n\t)\n", s) + len("\n\t)")]
    block = re.sub(r"^", "\t", block, flags=re.M)  # lib_symbols entries sit one level deeper
    end = text.index("\n\t)\n", text.index("\t(lib_symbols"))
    text = text[:end] + "\n" + block + text[end:]

    root = re.search(r'\(path "(/[0-9a-f-]+)"', text).group(1)
    x, y = xy
    prop = '\t\t(property "{k}" "{v}"\n\t\t\t(at {x:g} {y:g} 0)\n{hide}\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)'
    instance = "\n".join([
        "\t(symbol", '\t\t(lib_id "PWR_FLAG")', f"\t\t(at {x:g} {y:g} 0)", "\t\t(unit 1)",
        "\t\t(exclude_from_sim no)", "\t\t(in_bom yes)", "\t\t(on_board yes)", "\t\t(dnp no)",
        f'\t\t(uuid "{uuid.uuid4()}")',
        prop.format(k="Reference", v="#FLG01", x=x, y=y - 1.905, hide="\t\t\t(hide yes)\n"),
        prop.format(k="Value", v="PWR_FLAG", x=x, y=y - 3.81, hide=""),
        prop.format(k="Footprint", v="", x=x, y=y, hide="\t\t\t(hide yes)\n"),
        f'\t\t(pin "1"\n\t\t\t(uuid "{uuid.uuid4()}")\n\t\t)',
        f'\t\t(instances\n\t\t\t(project "CARDio"\n\t\t\t\t(path "{root}"\n\t\t\t\t\t(reference "#FLG01")\n'
        "\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)",
        "\t)",
    ])
    anchor = re.search(r"\n\t\((?:sheet_instances|embedded_fonts)", text)
    text = text[:anchor.start()] + "\n" + instance + text[anchor.start():]
    print(f"placed a PWR_FLAG on the ESP32 RF supply net (L3 pin 2 at {x:g}, {y:g})")
    return text


def exclude_page_frame(text):
    # EasyEDA's sheet frame comes in as a pin-less symbol called "A"; it's not a part.
    m = re.search(r'\(lib_id "Unknown_0_-1480"\)', text)
    if m:
        block_end = text.index('(property "Reference"', m.end())
        head = text[m.end():block_end]
        head = head.replace("(in_bom yes)", "(in_bom no)").replace("(on_board yes)", "(on_board no)")
        head = head.replace("(in_pos_files yes)", "(in_pos_files no)")
        text = text[:m.end()] + head + text[block_end:]
        print("excluded the EasyEDA page frame from the BOM and board")
    return text


# --- Project symbol library ---------------------------------------------------------

def top_level_symbols(text):
    """(name, start, end) of each symbol directly inside lib_symbols."""
    start = text.index("\t(lib_symbols")
    out = []
    for m in re.finditer(r'^\t\t\(symbol "([^"]+)"\n', text[start:], re.M):
        s = start + m.start()
        e = text.index("\n\t\t)\n", s) + len("\n\t\t)")
        out.append((m.group(1), s, e))
        if text.startswith("\t)", e + 1):
            break
    return out


def build_symbol_library(text, sch_path):
    # The importer gives every instance its own copy of a symbol (R..._1, R..._2);
    # fold identical copies together and write them to CARDio.kicad_sym.
    syms = top_level_symbols(text)
    blocks = {name: text[s:e] for name, s, e in syms}

    def canonical(name, block):
        # Same symbol if only its name and default reference differ.
        block = re.sub(r'\(symbol "' + re.escape(name), '(symbol "@', block)
        return re.sub(r'\(property "Reference" "[^"]*"', '(property "Reference" ""', block)

    keep, alias = {}, {}
    for name, block in blocks.items():
        base = re.sub(r"_\d+$", "", name) if re.sub(r"_\d+$", "", name) in blocks else name
        if base != name and canonical(name, block) == canonical(base, blocks[base]):
            alias[name] = base
        else:
            keep[name] = block
    lib_name = {name: re.sub(r"[^A-Za-z0-9_.+\-]", "_", name).replace("Unknown_0_-1480", "Page_Frame") for name in keep}

    # Library file
    lib = ['(kicad_symbol_lib\n\t(version 20241209)\n\t(generator "cardio_fix_easyeda_schematic")\n\t(generator_version "1.0")']
    for name, block in keep.items():
        b = block.replace(f'(symbol "{name}', f'(symbol "{lib_name[name]}')
        lib.append(re.sub(r"^\t", "", b, flags=re.M))
    lib_file = re.sub(r"[^/\\]+$", "CARDio.kicad_sym", sch_path)
    open(lib_file, "w", encoding="utf-8").write("\n".join(lib) + "\n)\n")
    table = re.sub(r"[^/\\]+$", "sym-lib-table", sch_path)
    open(table, "w", encoding="utf-8").write(
        '(sym_lib_table\n\t(version 7)\n\t(lib (name "CARDio") (type "KiCad") (uri "${KIPRJMOD}/CARDio.kicad_sym") '
        '(options "") (descr "CARDio symbols (LCSC/EasyEDA parts used on the board)"))\n)\n')

    # Schematic: drop the duplicate copies and point everything at CARDio:<name>.
    for name, s, e in reversed(syms):
        if name in alias:
            text = text[:s] + text[e + 1:]
    for name in keep:
        text = text.replace(f'\t\t(symbol "{name}"\n', f'\t\t(symbol "CARDio:{lib_name[name]}"\n', 1)
    # An instance uses the cached symbol named by lib_name if it has one, else by lib_id.
    def relink(m):
        cached = m.group(2) or m.group(3)
        return f'(lib_id "CARDio:{lib_name[alias.get(cached, cached)]}")'

    text = re.sub(r'(\(lib_name "([^"]+)"\)\n\t\t)?\(lib_id "([^"]+)"\)', relink, text)
    # sub-unit names inside the cached copies must match the new top-level names
    for name in keep:
        if lib_name[name] != name:
            text = text.replace(f'(symbol "{name}_', f'(symbol "{lib_name[name]}_')
    print(f"wrote {len(keep)} symbols to CARDio.kicad_sym (merged {len(alias)} duplicate copies)")
    return text


def link_footprints(text, sch_path):
    # Fill each symbol's Footprint field from the board, which has the real parts.
    pcb_path = re.sub(r"\.kicad_sch$", ".kicad_pcb", sch_path)
    pcb = open(pcb_path, encoding="utf-8").read()
    fp_of = {}
    for m in re.finditer(r'\(footprint "([^"]+)".*?\(property "Reference" "([^"]+)"', pcb, re.S):
        fp_of[m.group(2)] = m.group(1)
    count = 0

    def fill(m):
        nonlocal count
        block = m.group(0)
        ref = re.search(r'\(property "Reference" "([^"]+)"', block)
        if ref and ref.group(1) in fp_of:
            new, n = re.subn(r'\(property "Footprint" "[^"]*"', f'(property "Footprint" "{fp_of[ref.group(1)]}"', block, count=1)
            count += n
            return new
        return block

    text = re.sub(r'\n\t\(symbol\n.*?\n\t\)', fill, text, flags=re.S)
    print(f"linked {count} symbols to their footprints")
    return text


if __name__ == "__main__":
    main(sys.argv[1])
