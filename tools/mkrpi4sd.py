#!/usr/bin/env python3
"""mkrpi4sd.py -- a whole Raspberry Pi 4 SD card image: docs/RPI4.md P3.

    python3 tools/mkrpi4sd.py <out.img> <boot-dir> <rootfs.img>

    LBA 0        MBR
    partition 1  FAT32 (type 0x0C), 128 MiB at 1 MiB: every file in <boot-dir>,
                 subdirectories included -- what the Pi's firmware boots from
    partition 2  <rootfs.img> verbatim (type 0x83, as tools/mkbootdisk.sh
                 uses): the EMBKFS root the kernel mounts

The whole image is rounded up to a power of two, because QEMU refuses any
other size for an SD card; a real card does not care.

WHY A FAT WRITER OF ITS OWN, when tools/mkfat32.py exists. Two things the Pi's
firmware needs that the kernel's own test fixtures never did:

  * LONG NAMES AND SUBDIRECTORIES. The firmware looks for
    `bcm2711-rpi-4-b.dtb` and `overlays/disable-bt.dtbo` by exactly those
    names. mkfat32.py writes 8.3 names in the root only.
  * A VOLUME THAT IS FAT32 BY THE SPECIFICATION'S DEFINITION. The FAT type is
    decided by the CLUSTER COUNT, not by the "FAT32" string in the boot
    sector: fewer than 65525 clusters is FAT16, whatever the label says.
    mkfat32.py's 32 MiB of 4 KiB clusters is ~8000 -- fine for this kernel's
    driver, which trusts the label, and read as FAT16 by anything that follows
    the specification. Which the firmware does. 128 MiB of 1 KiB clusters is
    ~130000, comfortably FAT32 to every reader.

Checked on macOS by `fsck_msdos -n` on partition 1 and by mounting it with
hdiutil and comparing every file (`make ... rpi4-sdcard` does not need
either; they are how this file was verified).

Deterministic: fixed timestamps, directory entries in sorted order, so the
same inputs give the same bytes.
"""
import os
import struct
import sys

SECTOR = 512
PART1_LBA = 2048                      # 1 MiB: what every SD partitioner uses
BOOT_MB = 128
SEC_PER_CLUS = 2                      # 1 KiB clusters -> ~130k: FAT32 for real
RESERVED = 32
NUM_FATS = 2
EOC = 0x0FFFFFFF
FIXED_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1      # 2026-01-01
FIXED_TIME = 0
ATTR_DIR, ATTR_ARCHIVE, ATTR_VOLUME, ATTR_LFN = 0x10, 0x20, 0x08, 0x0F
SHORT_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


# --- names --------------------------------------------------------------------

def fits_83(name):
    """True when `name` IS its own short name: upper case, 8.3, legal chars."""
    if name in (".", ".."):
        return True
    base, dot, ext = name.partition(".")
    if "." in ext or not base or len(base) > 8 or len(ext) > 3:
        return False
    return all(c in SHORT_OK for c in base + ext)


def short_entry_name(name):
    base, _, ext = name.partition(".")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def make_short_name(name, taken):
    """A unique BASIS~N.EXT for a name that needs a long entry."""
    base, dot, ext = name.rpartition(".") if "." in name[1:] else (name, "", "")
    clean = lambda s: "".join(c for c in s.upper() if c in SHORT_OK)
    b, e = clean(base) or "FILE", clean(ext)[:3]
    for n in range(1, 1000000):
        tail = "~%d" % n
        cand = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if cand not in taken:
            taken.add(cand)
            return cand.encode("ascii")
    raise SystemExit("mkrpi4sd: ran out of short names for %s" % name)


def lfn_checksum(short11):
    s = 0
    for byte in short11:
        s = (((s & 1) << 7) | (s >> 1)) + byte
        s &= 0xFF
    return s


def lfn_entries(name, short11):
    """The long-name entries for `name`, in on-disk order (last piece first)."""
    units = list(name.encode("utf-16-le"))
    chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    chars.append(0x0000)                               # terminator...
    while len(chars) % 13:
        chars.append(0xFFFF)                           # ...then padding
    pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    csum = lfn_checksum(short11)
    out = []
    for seq, piece in enumerate(pieces, 1):
        order = seq | (0x40 if seq == len(pieces) else 0)
        e = bytearray(32)
        e[0] = order
        struct.pack_into("<5H", e, 1, *piece[0:5])
        e[11], e[12], e[13] = ATTR_LFN, 0, csum
        struct.pack_into("<6H", e, 14, *piece[5:11])
        struct.pack_into("<2H", e, 28, *piece[11:13])
        out.append(bytes(e))
    return list(reversed(out))


def dirent(short11, attr, cluster, size):
    return struct.pack("<11sBBBHHHHHHHI", short11, attr, 0, 0,
                       FIXED_TIME, FIXED_DATE, FIXED_DATE,
                       cluster >> 16, FIXED_TIME, FIXED_DATE,
                       cluster & 0xFFFF, size)


# --- the volume ---------------------------------------------------------------

class Fat32:
    def __init__(self, sectors, hidden):
        self.sectors, self.hidden = sectors, hidden
        # Solve for the FAT size: it must hold an entry for every cluster the
        # rest of the volume turns into, and it takes space from that volume.
        fat = 1
        while True:
            data = sectors - RESERVED - NUM_FATS * fat
            clusters = data // SEC_PER_CLUS
            need = (clusters + 2) * 4
            if fat * SECTOR >= need:
                break
            fat = (need + SECTOR - 1) // SECTOR
        if clusters < 65525:
            raise SystemExit("mkrpi4sd: %d clusters is FAT16 territory" % clusters)
        self.fat_sectors, self.clusters = fat, clusters
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, EOC
        self.next = 2
        self.img = bytearray(sectors * SECTOR)

    def cluster_offset(self, n):
        return (RESERVED + NUM_FATS * self.fat_sectors + (n - 2) * SEC_PER_CLUS) * SECTOR

    def alloc(self, nbytes):
        """A contiguous chain long enough for `nbytes` (at least one cluster)."""
        per = SEC_PER_CLUS * SECTOR
        n = max(1, (nbytes + per - 1) // per)
        first = self.next
        if first + n > self.clusters + 2:
            raise SystemExit("mkrpi4sd: the boot partition is full")
        for c in range(first, first + n):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = EOC
        self.next += n
        return first

    def write_chain(self, first, data):
        off = self.cluster_offset(first)
        self.img[off:off + len(data)] = data          # chains are contiguous

    def add_dir(self, path, cluster, parent_cluster):
        """Write directory `path` (whose chain starts at `cluster`) and,
        recursively, everything under it."""
        entries = []
        if parent_cluster is None:                    # the root: a label
            entries.append(dirent(b"EMBLINKBOOT", ATTR_VOLUME, 0, 0))
        else:
            entries.append(dirent(b".          ", ATTR_DIR, cluster, 0))
            entries.append(dirent(b"..         ", ATTR_DIR, parent_cluster, 0))

        taken, later = set(), []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            is_dir = os.path.isdir(full)
            if fits_83(name):
                short = short_entry_name(name)
                taken.add(short.decode())
                longs = []
            else:
                short = make_short_name(name, taken)
                longs = lfn_entries(name, short)
            if is_dir:
                child = self.alloc(self.dir_bytes(full))
                later.append((full, child))
                entries += longs + [dirent(short, ATTR_DIR, child, 0)]
            else:
                data = open(full, "rb").read()
                first = self.alloc(len(data)) if data else 0
                if data:
                    self.write_chain(first, data)
                entries += longs + [dirent(short, ATTR_ARCHIVE, first, len(data))]

        blob = b"".join(entries)
        self.write_chain(cluster, blob)               # the rest stays zero: end
        for full, child in later:
            # ".." of a directory whose parent is the ROOT is cluster 0, by
            # rule -- not the root's real cluster.
            self.add_dir(full, child, 0 if parent_cluster is None else cluster)

    def dir_bytes(self, path):
        """An upper bound on the bytes a directory's entries take."""
        n = 3                                         # ".", "..", end
        for name in os.listdir(path):
            n += 1 + (0 if fits_83(name) else (len(name) + 13) // 13)
        return n * 32

    def finish(self, label=b"EMBLINKBOOT"):
        b = self.img
        # Boot sector (BPB + FAT32 extension).
        b[0:3] = b"\xEB\x58\x90"
        b[3:11] = b"EMBLINK "
        struct.pack_into("<HBHBHHBHHHII", b, 11,
                         SECTOR, SEC_PER_CLUS, RESERVED, NUM_FATS,
                         0, 0,                         # root entries, total16: FAT32
                         0xF8, 0, 63, 255,             # media, fat16 size, CHS
                         self.hidden, self.sectors)
        struct.pack_into("<IHHIHH", b, 36,
                         self.fat_sectors, 0, 0,       # FAT size, flags, version
                         2, 1, 6)                      # root cluster, FSInfo, backup
        b[64], b[66] = 0x80, 0x29                      # drive number, ext. signature
        struct.pack_into("<I", b, 67, 0x0EB7B0D7)      # serial: fixed
        b[71:82] = label
        b[82:90] = b"FAT32   "
        b[510:512] = b"\x55\xAA"
        # FSInfo.
        fs = SECTOR
        struct.pack_into("<I", b, fs, 0x41615252)
        struct.pack_into("<I", b, fs + 484, 0x61417272)
        struct.pack_into("<II", b, fs + 488, self.clusters + 2 - self.next, self.next)
        struct.pack_into("<I", b, fs + 508, 0xAA550000)
        # Backups of both, at 6 and 7.
        b[6 * SECTOR:8 * SECTOR] = b[0:2 * SECTOR]
        # The FATs.
        raw = struct.pack("<%dI" % len(self.fat), *self.fat)
        for i in range(NUM_FATS):
            off = (RESERVED + i * self.fat_sectors) * SECTOR
            b[off:off + len(raw)] = raw
        return bytes(b)


def build_boot_partition(boot_dir, sectors, hidden):
    v = Fat32(sectors, hidden)
    root = v.alloc(v.dir_bytes(boot_dir))
    assert root == 2
    v.add_dir(boot_dir, root, None)
    return v.finish()


def mbr_entry(ptype, start, count):
    # CHS fields set to the "use LBA" sentinel; every reader since 1995 does.
    return struct.pack("<B3sB3sII", 0x00, b"\xFE\xFF\xFF", ptype, b"\xFE\xFF\xFF",
                       start, count)


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    out, boot_dir, rootfs = argv
    boot_secs = BOOT_MB * 1024 * 1024 // SECTOR
    root_bytes = open(rootfs, "rb").read()
    if len(root_bytes) % SECTOR:
        raise SystemExit("mkrpi4sd: %s is not a whole number of sectors" % rootfs)
    part2_lba = PART1_LBA + boot_secs
    root_secs = len(root_bytes) // SECTOR

    total = (part2_lba + root_secs) * SECTOR
    size = 1
    while size < total:
        size <<= 1                                     # QEMU: a power of two

    mbr = bytearray(SECTOR)
    struct.pack_into("<I", mbr, 440, 0x454D424B)       # disk signature "EMBK"
    mbr[446:462] = mbr_entry(0x0C, PART1_LBA, boot_secs)
    mbr[462:478] = mbr_entry(0x83, part2_lba, root_secs)
    mbr[510:512] = b"\x55\xAA"

    with open(out, "wb") as f:
        f.write(mbr)
        f.seek(PART1_LBA * SECTOR)
        f.write(build_boot_partition(boot_dir, boot_secs, PART1_LBA))
        f.seek(part2_lba * SECTOR)
        f.write(root_bytes)
        f.truncate(size)
    print("mkrpi4sd: %s -- %d MiB: boot FAT32 %d MiB at 1 MiB, root %d MiB"
          % (out, size >> 20, BOOT_MB, len(root_bytes) >> 20))


if __name__ == "__main__":
    main(sys.argv[1:])
