#!/usr/bin/env python3
"""fat32_read.py -- read one file out of the FAT32 EFI partition of a GPT disk
image, for tests that check what the kernel left on the stick.

    python3 tools/fat32_read.py uefi-usb.img /EMBLINK.LOG > log.txt
"""
import struct
import sys


def first_partition_lba(f):
    f.seek(512)
    hdr = f.read(92)
    if hdr[:8] != b"EFI PART":
        raise ValueError("no GPT header")
    pe_lba, n, esz = struct.unpack_from("<Q", hdr, 72)[0], struct.unpack_from("<I", hdr, 80)[0], \
        struct.unpack_from("<I", hdr, 84)[0]
    f.seek(pe_lba * 512)
    ents = f.read(n * esz)
    for i in range(n):
        e = ents[i * esz:(i + 1) * esz]
        if e[:16] != b"\0" * 16:
            return struct.unpack_from("<Q", e, 32)[0]
    raise ValueError("no partitions")


def short_name(component):
    stem, _, ext = component.upper().partition(".")
    return (stem.ljust(8)[:8] + ext.ljust(3)[:3]).encode("ascii")


def read_file(image, path):
    """The file's bytes, or None if it is not there."""
    with open(image, "rb") as f:
        base = first_partition_lba(f) * 512
        f.seek(base)
        bs = f.read(512)
        bps, spc = struct.unpack_from("<H", bs, 11)[0], bs[13]
        rsv, nfat = struct.unpack_from("<H", bs, 14)[0], bs[16]
        spf, root = struct.unpack_from("<I", bs, 36)[0], struct.unpack_from("<I", bs, 44)[0]
        fat_off = base + rsv * bps
        data = base + (rsv + nfat * spf) * bps
        csize = spc * bps

        def chain(start):
            c, seen = start, set()
            while 2 <= c < 0x0ffffff8 and c not in seen:
                seen.add(c)
                yield c
                f.seek(fat_off + c * 4)
                c = struct.unpack("<I", f.read(4))[0] & 0x0fffffff

        def read_chain(start):
            out = bytearray()
            for c in chain(start):
                f.seek(data + (c - 2) * csize)
                out += f.read(csize)
            return bytes(out)

        cluster, size = root, None
        parts = [p for p in path.split("/") if p]
        for i, part in enumerate(parts):
            d, found = read_chain(cluster), None
            for o in range(0, len(d), 32):
                e = d[o:o + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xE5 or (e[11] & 0x0F) == 0x0F:
                    continue
                if e[0:11] == short_name(part):
                    found = e
                    break
            if found is None:
                return None
            cluster = (struct.unpack_from("<H", found, 20)[0] << 16) | struct.unpack_from("<H", found, 26)[0]
            size = struct.unpack_from("<I", found, 28)[0]
        if size is None:
            return None
        return read_chain(cluster)[:size] if size else b""


if __name__ == "__main__":
    content = read_file(sys.argv[1], sys.argv[2])
    if content is None:
        sys.exit("%s: not found" % sys.argv[2])
    sys.stdout.buffer.write(content)
