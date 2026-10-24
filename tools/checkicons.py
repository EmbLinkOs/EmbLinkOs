#!/usr/bin/env python3
"""Check every public symbol's Contour asset or shipped-font fallback.

Both hexadecimal legacy codepoints and decimal private-use IDs are inspected.
A mapped icon must have its reproducible SVG master and a valid EIC ladder.
"""
import os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "ui", "dsl", "em.h")
# The same file mkfs bakes in as /system/fonts/font.ttf -- so it must be found
# the same way, on every host. See mkfs_embkfs.py's FONT_DIRS for the why.
FONT_DIRS = [
    os.environ.get("EMBK_FONT_DIR"),
    "/usr/share/fonts/truetype/dejavu",     # Debian/Ubuntu
    "/usr/share/fonts/dejavu",              # Fedora
    "/usr/share/fonts/TTF",                 # Arch
    os.path.expanduser("~/Library/Fonts"),  # macOS, per-user (brew cask)
    "/Library/Fonts",                       # macOS, system-wide
]
FONT = next((os.path.join(d, "DejaVuSans.ttf") for d in FONT_DIRS
             if d and os.path.exists(os.path.join(d, "DejaVuSans.ttf"))), None)
if FONT is None:
    raise SystemExit(
        "checkicons: DejaVuSans.ttf not found -- searched %s\n"
        "  Debian/Ubuntu: sudo apt install fonts-dejavu\n"
        "  macOS:         brew install --cask font-dejavu\n"
        "  or set EMBK_FONT_DIR=/dir/containing/DejaVuSans.ttf"
        % ", ".join(d for d in FONT_DIRS if d))


def cmap_of(path):
    """Every codepoint the font maps, from its format-4 subtables."""
    d = open(path, "rb").read()
    off = None
    for i in range(struct.unpack(">H", d[4:6])[0]):
        e = 12 + 16 * i
        if d[e:e + 4] == b"cmap":
            off = struct.unpack(">I", d[e + 8:e + 12])[0]
    if off is None:
        raise SystemExit("no cmap table in " + path)
    out = set()
    for i in range(struct.unpack(">H", d[off + 2:off + 4])[0]):
        sub = off + struct.unpack(">I", d[off + 8 + 8 * i:off + 12 + 8 * i])[0]
        if struct.unpack(">H", d[sub:sub + 2])[0] != 4:
            continue
        segx2 = struct.unpack(">H", d[sub + 6:sub + 8])[0]
        sc = segx2 // 2
        ends = [struct.unpack(">H", d[sub + 14 + 2 * j:sub + 16 + 2 * j])[0] for j in range(sc)]
        sp = sub + 16 + segx2
        starts = [struct.unpack(">H", d[sp + 2 * j:sp + 2 + 2 * j])[0] for j in range(sc)]
        for a, b in zip(starts, ends):
            if b != 0xFFFF:
                out |= set(range(a, b + 1))
    return out


def main():
    if not os.path.exists(FONT):
        print("icon-check: %s not present, skipped" % FONT)
        return 0
    have=cmap_of(FONT)
    definitions={m.group(1):int(m.group(2),0) for m in re.finditer(
        r"#define\s+(Icon\w+)\s+(0x[0-9A-Fa-f]+|[0-9]+)",open(HEADER).read())}
    implementation=open(os.path.join(ROOT,"ui/dsl/em.c")).read()
    routes={definitions[m.group(1)]:m.group(2) for m in re.finditer(
        r'case (Icon\w+): path="/system/images/([^"/]+)\.eic";',implementation)}
    errors=[];assets=0
    for name,cp in definitions.items():
        if cp not in routes:
            if cp not in have:errors.append(f"{name}: no asset and no font fallback")
            continue
        stem=routes[cp];master=os.path.join(ROOT,"icons/masters",stem+".svg")
        path=os.path.join(ROOT,"system/images",stem+".eic")
        if not os.path.isfile(master) or not os.path.isfile(path):
            errors.append(f"{name}: missing master or EIC for {stem}");continue
        d=open(path,"rb").read()
        if len(d)<32 or d[:4]!=b'EICO':errors.append(f"{name}: invalid EIC header");continue
        version,count=struct.unpack_from('<HH',d,4)
        if version!=1 or count<3 or 32+16*count>len(d):errors.append(f"{name}: invalid EIC ladder");continue
        for i in range(count):
            w,h,stride,offset,length=struct.unpack_from('<HHIII',d,32+16*i)
            if not w or w!=h or stride!=w*4 or length!=stride*h or offset<32+16*count or offset+length>len(d):
                errors.append(f"{name}: invalid level {i}")
        assets+=1
    if errors:
        print("icon-check: FAIL\n"+"\n".join(errors));return 1
    print(f"icon-check: OK ({len(definitions)} symbols, {assets} Contour assets, valid fallback mappings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
