#!/usr/bin/env python3
"""mkpkg.py -- generate a package manifest (docs/PACKAGING_AND_SDK.md §3) from a
built EMBX. The manifest is a VIEW of the binary: its build_id, abi and caps are
read straight out of the EMBX header/cap-table, so the human-readable declaration
is consistent with the binary by construction. Namespace lines are supplied by
the caller (in PK2 they will come from build.ebm; here, from --ns flags).

    mkpkg.py <in.embx> <out.pkg> --name N --version V [--ns "ro /system"]...
"""
import sys, struct

CAP_NAMES = {1:"filesystem",2:"network",3:"gpu",4:"audio",5:"camera",
             6:"usb",7:"serial",8:"rawdisk",9:"kernel-ext",10:"debug"}

def main():
    inp, outp = sys.argv[1], sys.argv[2]
    name = version = None
    ns = []
    i = 3
    while i < len(sys.argv):
        a = sys.argv[i]
        if   a == "--name":    name = sys.argv[i+1]; i += 2
        elif a == "--version": version = sys.argv[i+1]; i += 2
        elif a == "--ns":      ns.append(sys.argv[i+1]); i += 2
        else: sys.exit(f"mkpkg: unknown arg {a}")
    if not name or not version:
        sys.exit("mkpkg: --name and --version are required")

    img = open(inp, "rb").read()
    if img[:5] != b"\x7fEMBX":
        sys.exit(f"mkpkg: {inp} is not an EMBX")
    abi        = struct.unpack_from("<I", img, 0x14)[0]
    cap_off    = struct.unpack_from("<I", img, 0x40)[0]
    cap_count  = struct.unpack_from("<H", img, 0x44)[0]
    build_id   = img[0x50:0x70].hex()
    caps = []
    for k in range(cap_count):
        cid = struct.unpack_from("<I", img, cap_off + k*16)[0]
        caps.append(CAP_NAMES.get(cid, f"cap{cid}"))

    provides = inp.rsplit("/", 1)[-1]
    lines = [f"name:     {name}",
             f"version:  {version}",
             f"abi:      {abi}",
             f"build_id: {build_id}",
             f"caps:     {' '.join(caps)}" if caps else "caps:",
             "namespace:"]
    for n in ns:
        lines.append(f"  {n}")
    lines.append(f"provides: {provides}")
    open(outp, "w").write("\n".join(lines) + "\n")
    print(f"mkpkg: {outp}  (name={name} v{version} abi={abi} caps=[{','.join(caps)}] ns={len(ns)})")

if __name__ == "__main__":
    main()
