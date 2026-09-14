#!/usr/bin/env python3
"""mkfat32.py -- write a FAT32 volume, with no tools but this file.

    python3 tools/mkfat32.py out.img [SIZE_MB] [name=content ...]

WHY THIS EXISTS. `make fat32.img` needed mkfs.vfat and mcopy, which are Linux
tools; on macOS neither exists, so the FAT32 test could not be built and
docs/TODO.md recorded it as a host gap. Everything a FAT32 volume is made of is
bytes this file knows how to lay out -- the same approach tools/mkuefidisk.py
already takes for the EFI system partition -- so the dependency was never
necessary, only convenient.

WHAT IT WRITES: one boot sector plus its backup, an FSInfo sector, two FATs,
and a root directory holding a volume label and the files given on the command
line. Short names only (8.3, upper case): long-name entries are a checksummed
UCS-2 chain, and a test fixture that depends on getting them right is testing
this script rather than the filesystem driver.
"""
import os
import sys

SECTOR = 512


def build_fat32(path, size_mb=32, files=None, label="EMBLINK"):
    """Lay out a FAT32 volume containing `files` ({name: bytes}) in its root."""
    files = files or {}
    SECTORS = (size_mb * 1024 * 1024) // SECTOR

    RESERVED = 32
    FATS = 2
    SEC_PER_CLUS = 8                          # 4 KiB clusters
    data_sectors = SECTORS - RESERVED
    # Solve for the FAT size: each cluster needs 4 bytes of FAT per copy.
    clusters = data_sectors // (SEC_PER_CLUS + (4 * FATS) / float(SECTOR))
    fat_sectors = int((int(clusters) * 4 + SECTOR - 1) // SECTOR) + 1
    img = bytearray(SECTORS * SECTOR)

    # ---- boot sector ----
    bs = img
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"EMBLINK "
    bs[11:13] = (SECTOR).to_bytes(2, "little")
    bs[13] = SEC_PER_CLUS
    bs[14:16] = (RESERVED).to_bytes(2, "little")
    bs[16] = FATS
    bs[17:19] = (0).to_bytes(2, "little")     # root entries: 0 on FAT32
    bs[19:21] = (0).to_bytes(2, "little")     # small sector count: 0
    bs[21] = 0xF8                             # fixed disk
    bs[22:24] = (0).to_bytes(2, "little")     # FAT16 size: 0 on FAT32
    bs[24:26] = (63).to_bytes(2, "little")
    bs[26:28] = (255).to_bytes(2, "little")
    bs[28:32] = (0).to_bytes(4, "little")     # hidden sectors
    bs[32:36] = (SECTORS).to_bytes(4, "little")
    bs[36:40] = (fat_sectors).to_bytes(4, "little")
    bs[40:42] = (0).to_bytes(2, "little")     # flags: mirrored
    bs[42:44] = (0).to_bytes(2, "little")     # version
    bs[44:48] = (2).to_bytes(4, "little")     # root cluster
    bs[48:50] = (1).to_bytes(2, "little")     # FSInfo sector
    bs[50:52] = (6).to_bytes(2, "little")     # backup boot sector
    bs[64] = 0x80
    bs[66] = 0x29                             # extended boot signature
    bs[67:71] = (0x1234ABCD).to_bytes(4, "little")
    bs[71:82] = label.ljust(11)[:11].encode()
    bs[82:90] = b"FAT32   "
    bs[510:512] = b"\x55\xAA"
    img[6 * SECTOR:6 * SECTOR + SECTOR] = bs[0:SECTOR]   # backup

    # ---- FSInfo ----
    fi = 1 * SECTOR
    img[fi:fi + 4] = b"RRaA"
    img[fi + 484:fi + 488] = b"rrAa"
    img[fi + 488:fi + 492] = (0xFFFFFFFF).to_bytes(4, "little")
    img[fi + 492:fi + 496] = (0xFFFFFFFF).to_bytes(4, "little")
    img[fi + 508:fi + 512] = b"\x00\x00\x55\xAA"

    # ---- FATs: cluster 0/1 reserved, 2 = root (end), 3 = the file (end) ----
    def put_fat(entry, value):
        for f in range(FATS):
            off = (RESERVED + f * fat_sectors) * SECTOR + entry * 4
            img[off:off + 4] = (value & 0x0FFFFFFF).to_bytes(4, "little")

    put_fat(0, 0x0FFFFFF8)
    put_fat(1, 0x0FFFFFFF)
    put_fat(2, 0x0FFFFFFF)

    data_start = RESERVED + FATS * fat_sectors
    def clus_off(n):
        return (data_start + (n - 2) * SEC_PER_CLUS) * SECTOR

    # ---- root directory: a volume label, then the file ----
    root = clus_off(2)
    ent = bytearray(32)
    ent[0:11] = label.ljust(11)[:11].encode()
    ent[11] = 0x08                             # volume label
    img[root:root + 32] = ent

    # ---- the files, one cluster each ----
    #
    # ONE CLUSTER PER FILE is a real limit and it is stated rather than hidden:
    # a file longer than the cluster size would need a FAT chain, and this
    # writes fixtures, not archives. 4 KiB is comfortably more than any test
    # marker and the builder refuses anything larger instead of truncating.
    slot = 32
    cluster = 3
    for fname, body_bytes in files.items():
        if len(body_bytes) > SEC_PER_CLUS * SECTOR:
            raise ValueError("%s is %d bytes; this builder writes one cluster "
                             "per file (max %d)"
                             % (fname, len(body_bytes), SEC_PER_CLUS * SECTOR))
        base, _, ext = fname.partition(".")
        ent = bytearray(32)
        ent[0:8] = base.upper().ljust(8)[:8].encode()
        ent[8:11] = ext.upper().ljust(3)[:3].encode()
        ent[11] = 0x20                             # archive
        ent[20:22] = ((cluster >> 16) & 0xFFFF).to_bytes(2, "little")
        ent[26:28] = (cluster & 0xFFFF).to_bytes(2, "little")
        ent[28:32] = (len(body_bytes)).to_bytes(4, "little")
        img[root + slot:root + slot + 32] = ent
        put_fat(cluster, 0x0FFFFFFF)
        img[clus_off(cluster):clus_off(cluster) + len(body_bytes)] = body_bytes
        slot += 32
        cluster += 1

    with open(path, "wb") as f:
        f.write(img)
    return path




def main(argv):
    if not argv:
        print(__doc__)
        return 2
    out = argv[0]
    size = int(argv[1]) if len(argv) > 1 and argv[1].isdigit() else 32
    files = {}
    for a in argv[1:]:
        if "=" in a:
            k, _, v = a.partition("=")
            files[k] = (v + "\n").encode()
    build_fat32(out, size, files)
    print("mkfat32: wrote %s (%d MiB FAT32, %d file(s): %s)"
          % (out, size, len(files), ", ".join(files) or "none"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
