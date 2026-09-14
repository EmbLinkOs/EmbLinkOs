#!/usr/bin/env python3
"""Write a minimal ISO 9660 image, with no external tools.

WHY THIS EXISTS. Testing the optical path needs a disc, and a disc needs a
filesystem builder. `mkisofs`/`genisoimage` is a Linux package; macOS has
`hdiutil`, which produces a hybrid HFS+/ISO image whose ISO half is not what
a plain reader sees first. Depending on either makes the test run on one
developer's machine and skip on another's -- which is exactly the failure
tools/mkfat32.py was written to avoid for FAT32.

WHAT IT WRITES. The smallest thing that is really ISO 9660: 16 empty system
blocks, a primary volume descriptor, a terminator, a path table, and one root
directory holding the files given on the command line. No Joliet, no Rock
Ridge, no El Torito -- this is a filesystem to read, not a disc to boot.

THE FORMAT'S ONE ODDITY is that every multi-byte number is stored TWICE, once
little-endian and once big-endian, back to back. The helpers below are named
for that: both16/both32.
"""
import sys, os, struct

BLOCK = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def strA(s, n):
    """A space-padded identifier field. The standard's character set is a
    subset of ASCII; callers here only pass upper-case names."""
    b = s.encode("ascii", "replace")[:n]
    return b + b" " * (n - len(b))


def dec_datetime(pad_zero=True):
    """The 17-byte 'YYYYMMDDHHMMSSttZ' form the volume descriptor uses. A
    fixed value, because a reproducible image is worth more here than a real
    timestamp: two runs of the build must produce identical bytes."""
    return b"2026091400000000" + bytes([0])


def dir_record(name, lba, size, is_dir, seq=1):
    """One directory record. `name` is bytes: b'\\x00' for '.', b'\\x01' for
    '..', otherwise the stored name (which for a FILE carries the version
    suffix ';1' -- part of the format, part of no name anyone types)."""
    ident = name
    ident_len = len(ident)
    rec_len = 33 + ident_len
    pad = rec_len % 2          # records are padded to an even length
    rec_len += pad
    r = bytearray()
    r.append(rec_len)
    r.append(0)                            # extended attribute length
    r += both32(lba)
    r += both32(size)
    # 7-byte recording date: years since 1900, month, day, h, m, s, GMT offset
    r += bytes([126, 9, 14, 0, 0, 0, 0])
    r.append(0x02 if is_dir else 0x00)     # file flags
    r.append(0)                            # file unit size
    r.append(0)                            # interleave gap
    r += both16(seq)                       # volume sequence number
    r.append(ident_len)
    r += ident
    if pad:
        r.append(0)
    assert len(r) == rec_len, (len(r), rec_len)
    return bytes(r)


def build(out_path, entries, label="EMBLINK"):
    """entries: list of (name, bytes). Names are upper-cased and get ';1'."""
    # --- lay the image out before writing any of it ---------------------
    # 0..15  system area (zero)
    # 16     primary volume descriptor
    # 17     volume descriptor set terminator
    # 18     L path table
    # 19     M path table
    # 20     root directory
    # 21..   file data, one block-aligned run each
    root_lba = 20
    files = []
    lba = 21
    for name, data in entries:
        files.append((name.upper(), lba, len(data), data))
        lba += (len(data) + BLOCK - 1) // BLOCK or 1
    total_blocks = lba

    # --- the root directory ---------------------------------------------
    root = bytearray()
    root += dir_record(b"\x00", root_lba, BLOCK, True)      # '.'
    root += dir_record(b"\x01", root_lba, BLOCK, True)      # '..'
    for name, flba, size, _ in files:
        root += dir_record((name + ";1").encode("ascii"), flba, size, False)
    if len(root) > BLOCK:
        raise SystemExit("mkiso: more files than fit in one root block")
    root_size = BLOCK
    root += b"\x00" * (BLOCK - len(root))

    # --- the path table (one entry: the root) ---------------------------
    # Required by the standard even when nothing reads it; a reader that
    # checks the PVD's pointers and finds zeroes is entitled to refuse.
    def path_table(endian):
        e = "<" if endian == "L" else ">"
        t = bytearray()
        t.append(1)                     # directory identifier length
        t.append(0)                     # extended attribute length
        t += struct.pack(e + "I", root_lba)
        t += struct.pack(e + "H", 1)    # parent directory number
        t += b"\x00"                    # the root's identifier is one NUL
        t += b"\x00"                    # pad to even
        return bytes(t)

    lpt, mpt = path_table("L"), path_table("M")
    pt_size = len(lpt)

    # --- the primary volume descriptor ----------------------------------
    pvd = bytearray(BLOCK)
    pvd[0] = 1                                     # type: primary
    pvd[1:6] = b"CD001"                            # THE identifier
    pvd[6] = 1                                     # version
    pvd[8:40] = strA("", 32)                       # system identifier
    pvd[40:72] = strA(label.upper(), 32)           # volume identifier
    pvd[80:88] = both32(total_blocks)              # volume space size
    # ONE FIELD PER SLICE. A bytearray slice assignment of the wrong length
    # RESIZES the array instead of failing, so writing two 4-byte fields into
    # one 4-byte slice silently pushes every later field four bytes along and
    # produces an image whose volume descriptor is subtly, invisibly wrong.
    pvd[120:124] = both16(1)                       # volume set size
    pvd[124:128] = both16(1)                       # volume sequence number
    pvd[128:132] = both16(BLOCK)                   # logical block size
    pvd[132:140] = both32(pt_size)                 # path table size
    pvd[140:144] = struct.pack("<I", 18)           # L path table location
    pvd[144:148] = struct.pack("<I", 0)            # optional L path table
    pvd[148:152] = struct.pack(">I", 19)           # M path table location
    pvd[152:156] = struct.pack(">I", 0)            # optional M path table
    rootrec = dir_record(b"\x00", root_lba, root_size, True)
    # THE ROOT RECORD IS EXACTLY 34 BYTES HERE. The field is fixed-width and a
    # record that does not fill it leaves the reader parsing whatever follows.
    assert len(rootrec) == 34, len(rootrec)
    pvd[156:190] = rootrec
    pvd[190:318] = strA("", 128)                   # volume set identifier
    pvd[318:446] = strA("EMBLINKOS", 128)          # publisher
    pvd[446:574] = strA("", 128)                   # data preparer
    pvd[574:702] = strA("TOOLS/MKISO.PY", 128)     # application
    for off in (813, 830, 847, 864):               # the four date fields
        pvd[off:off + 17] = dec_datetime()
    pvd[881] = 1                                   # file structure version
    assert len(pvd) == BLOCK, "the volume descriptor grew: a slice was resized"

    term = bytearray(BLOCK)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    term[6] = 1

    # --- write it ---------------------------------------------------------
    with open(out_path, "wb") as f:
        f.write(b"\x00" * (BLOCK * 16))
        f.write(bytes(pvd))
        f.write(bytes(term))
        f.write(lpt + b"\x00" * (BLOCK - len(lpt)))
        f.write(mpt + b"\x00" * (BLOCK - len(mpt)))
        f.write(bytes(root))
        for _, _, size, data in files:
            f.write(data)
            pad = (-len(data)) % BLOCK
            if pad or not data:
                f.write(b"\x00" * (pad if data else BLOCK))
    return total_blocks


def main():
    if len(sys.argv) < 2:
        print("usage: mkiso.py OUT.iso [NAME=CONTENT | path ...]", file=sys.stderr)
        return 2
    out = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        if "=" in arg and not os.path.exists(arg):
            name, _, content = arg.partition("=")
            entries.append((name, content.encode()))
        else:
            with open(arg, "rb") as f:
                entries.append((os.path.basename(arg), f.read()))
    if not entries:
        entries = [("HELLO.TXT", b"Hello from an ISO 9660 disc.\n")]
    n = build(out, entries)
    print("mkiso: %s, %d blocks, %d file(s)" % (out, n, len(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
