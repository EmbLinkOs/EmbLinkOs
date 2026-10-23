#!/usr/bin/env python3
"""mkuefidisk.py -- build a bootable UEFI disk image, with no external tools.

    tools/mkuefidisk.py <BOOTX64.EFI> <out.img> [embkfs.img]

    two arguments   GPT + an EFI System Partition holding the loader
    three arguments the same, plus a second partition carrying a raw EMBKFS --
                    a single self-contained stick: the firmware launches
                    BOOTX64.EFI, the embedded kernel comes up, and
                    embkfs_init() finds its root on the SAME device.

WHY THIS REPLACED A SHELL SCRIPT. The old one called mkfs.vfat, mmd, mcopy,
sfdisk and `stat -c` -- five programs, none of which exists on macOS, where
this OS is developed. So the one image you would write to a USB stick to boot
a real machine could not be built on the only machine available to build it.
That is not a small inconvenience: it is the last step of "put this OS on
hardware" being impossible on the development host.

Everything here is written by hand instead, which this tree already does for
its own filesystem (tools/embkfs_mkfs) and its swap store (tools/mkswap.py):

  * a protective MBR, so tools that predate GPT see one unknown partition
    covering the disk rather than an empty one they might offer to format;
  * primary and backup GPT headers and partition arrays, with the three CRC32s
    the firmware checks (header, entry array, and the backup's own);
  * a real FAT32 filesystem -- BPB, FSInfo, backup boot sector, two FATs, and
    the directory chain /EFI/BOOT/BOOTX64.EFI.

FAT32 rather than FAT16 because that is what firmware expects on an ESP, and
every name in the path (EFI, BOOT, BOOTX64.EFI) fits 8.3, so no long-filename
entries are needed and the directory code stays honest.
"""
import os
import struct
import sys
import uuid
import zlib

SECTOR = 512
ESP_MB = 64
FIRST_LBA = 2048                      # 1 MiB aligned
GPT_ENTRIES = 128
GPT_ENTRY_SZ = 128

ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
DATA_TYPE = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")


def _guid(u: uuid.UUID) -> bytes:
    """GUIDs on disk are MIXED endian: the first three fields little-endian, the
    last two big-endian. Writing all sixteen bytes straight through is the
    classic way to get partition types the firmware does not recognise."""
    b = u.bytes
    return b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:]


# --- FAT32 -------------------------------------------------------------------

def _fat_name(name: str) -> bytes:
    """'BOOTX64.EFI' -> b'BOOTX64 EFI'. 8.3, space padded, upper case."""
    if "." in name:
        stem, ext = name.split(".", 1)
    else:
        stem, ext = name, ""
    if len(stem) > 8 or len(ext) > 3:
        raise ValueError("not an 8.3 name: %r" % name)
    return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")


def _dirent(name: str, attr: int, cluster: int, size: int) -> bytes:
    return (_fat_name(name) + bytes([attr]) + b"\0" * 8 +
            struct.pack("<H", (cluster >> 16) & 0xFFFF) + b"\0" * 4 +
            struct.pack("<HI", cluster & 0xFFFF, size))


def fat32(size_sectors: int, efi_blob: bytes, label: str = "EMBLINKEFI") -> bytes:
    """A FAT32 filesystem of `size_sectors` holding /EFI/BOOT/BOOTX64.EFI."""
    spc = 1                                   # sectors per cluster
    reserved = 32
    nfats = 2

    # sectors_per_fat has to describe a FAT big enough for the clusters that
    # remain once the FATs themselves are subtracted -- solved by iterating
    # rather than by the usual approximation, because an under-sized FAT is a
    # filesystem the firmware mounts and then reads past the end of.
    spf = 1
    while True:
        data = size_sectors - reserved - nfats * spf
        clusters = data // spc
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need <= spf:
            break
        spf = need
    if clusters < 65525:
        raise ValueError("too few clusters for FAT32 (%d)" % clusters)

    img = bytearray(size_sectors * SECTOR)

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"EMBLINK "
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR,      # bytes per sector
                     spc,         # sectors per cluster
                     reserved,    # reserved sectors
                     nfats,       # FATs
                     0,           # root entries (0 on FAT32)
                     0,           # total sectors 16 (0 -> use the 32-bit field)
                     0xF8,        # media
                     0,           # sectors per FAT 16 (0 on FAT32)
                     32,          # sectors per track
                     8,           # heads
                     0,           # hidden sectors
                     size_sectors)
    struct.pack_into("<IHHIHH", boot, 36,
                     spf,         # sectors per FAT 32
                     0,           # ext flags
                     0,           # version
                     2,           # root directory cluster
                     1,           # FSInfo sector
                     6)           # backup boot sector
    boot[64] = 0x80              # drive number
    boot[66] = 0x29              # extended boot signature
    struct.pack_into("<I", boot, 67, 0x454D424B)
    boot[71:82] = label.upper().ljust(11).encode("ascii")[:11]
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"
    img[0:SECTOR] = boot
    img[6 * SECTOR:7 * SECTOR] = boot          # the backup the spec asks for

    fsinfo = bytearray(SECTOR)
    fsinfo[0:4] = b"RRaA"
    fsinfo[484:488] = b"rrAa"
    struct.pack_into("<II", fsinfo, 488, 0xFFFFFFFF, 0xFFFFFFFF)
    fsinfo[510:512] = b"\x55\xAA"
    img[SECTOR:2 * SECTOR] = fsinfo

    # cluster 2 = root, 3 = /EFI, 4 = /EFI/BOOT, 5.. = the loader's data
    file_clusters = max(1, (len(efi_blob) + spc * SECTOR - 1) // (spc * SECTOR))
    chain = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF, 2: 0x0FFFFFFF, 3: 0x0FFFFFFF, 4: 0x0FFFFFFF}
    for i in range(file_clusters):
        c = 5 + i
        chain[c] = 0x0FFFFFFF if i == file_clusters - 1 else c + 1
    if 5 + file_clusters - 1 > clusters + 1:
        raise ValueError("the loader does not fit in the ESP")

    fat = bytearray(spf * SECTOR)
    for c, v in chain.items():
        struct.pack_into("<I", fat, c * 4, v)
    for n in range(nfats):
        off = (reserved + n * spf) * SECTOR
        img[off:off + len(fat)] = fat

    data_start = (reserved + nfats * spf) * SECTOR

    def cluster_off(c):
        return data_start + (c - 2) * spc * SECTOR

    root = _dirent("EFI", 0x10, 3, 0)
    img[cluster_off(2):cluster_off(2) + len(root)] = root

    efidir = (_dirent(".", 0x10, 3, 0) + _dirent("..", 0x10, 0, 0) +
              _dirent("BOOT", 0x10, 4, 0))
    img[cluster_off(3):cluster_off(3) + len(efidir)] = efidir

    bootdir = (_dirent(".", 0x10, 4, 0) + _dirent("..", 0x10, 3, 0) +
               _dirent("BOOTX64.EFI", 0x20, 5, len(efi_blob)))
    img[cluster_off(4):cluster_off(4) + len(bootdir)] = bootdir

    img[cluster_off(5):cluster_off(5) + len(efi_blob)] = efi_blob
    return bytes(img)


# --- GPT ---------------------------------------------------------------------

def gpt_disk(parts, total_sectors: int, payloads) -> bytes:
    """parts: [(first_lba, last_lba, type_guid, name)]. payloads: [(lba, bytes)]."""
    img = bytearray(total_sectors * SECTOR)

    # protective MBR: one 0xEE partition covering the disk (clamped to 32 bits)
    mbr = bytearray(SECTOR)
    span = min(total_sectors - 1, 0xFFFFFFFF)
    mbr[446:462] = (bytes([0x00, 0x00, 0x02, 0x00, 0xEE, 0xFF, 0xFF, 0xFF]) +
                    struct.pack("<II", 1, span))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    entries = bytearray(GPT_ENTRIES * GPT_ENTRY_SZ)
    for i, (first, last, tguid, name) in enumerate(parts):
        e = (_guid(tguid) + _guid(uuid.uuid4()) +
             struct.pack("<QQQ", first, last, 0) +
             name.encode("utf-16-le").ljust(72, b"\0")[:72])
        entries[i * GPT_ENTRY_SZ:(i + 1) * GPT_ENTRY_SZ] = e
    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
    entry_sectors = len(entries) // SECTOR

    disk_guid = _guid(uuid.uuid4())
    backup_lba = total_sectors - 1
    backup_entries_lba = backup_lba - entry_sectors
    first_usable = 2 + entry_sectors
    last_usable = backup_entries_lba - 1

    def header(my_lba, alt_lba, entries_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIII", h, 8, 0x00010000, 92, 0, 0)
        struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, first_usable, last_usable)
        h[56:72] = disk_guid
        struct.pack_into("<QIII", h, 72, entries_lba, GPT_ENTRIES, GPT_ENTRY_SZ, entries_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")

    img[SECTOR:2 * SECTOR] = header(1, backup_lba, 2)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    img[backup_entries_lba * SECTOR:backup_entries_lba * SECTOR + len(entries)] = entries
    img[backup_lba * SECTOR:(backup_lba + 1) * SECTOR] = header(backup_lba, 1, backup_entries_lba)

    for lba, blob in payloads:
        img[lba * SECTOR:lba * SECTOR + len(blob)] = blob
    return bytes(img)


def main(argv):
    if not 3 <= len(argv) <= 4:
        print(__doc__)
        return 2
    efi_path, out_path = argv[1], argv[2]
    embkfs_path = argv[3] if len(argv) > 3 else None
    efi = open(efi_path, "rb").read()

    esp_sectors = ESP_MB * 1024 * 1024 // SECTOR
    esp = fat32(esp_sectors, efi)
    parts = [(FIRST_LBA, FIRST_LBA + esp_sectors - 1, ESP_TYPE, "EFI System")]
    payloads = [(FIRST_LBA, esp)]
    total = FIRST_LBA + esp_sectors + 2048

    if embkfs_path:
        root = open(embkfs_path, "rb").read()
        root_sectors = (len(root) + SECTOR - 1) // SECTOR
        start2 = FIRST_LBA + esp_sectors
        parts.append((start2, start2 + root_sectors - 1, DATA_TYPE, "EMBKFS root"))
        payloads.append((start2, root))
        total = start2 + root_sectors + 2048

    open(out_path, "wb").write(gpt_disk(parts, total, payloads))
    what = "ESP + EMBKFS root, single device" if embkfs_path else "ESP only"
    print("mkuefidisk: wrote %s (%d MiB, GPT: %s, /EFI/BOOT/BOOTX64.EFI)"
          % (out_path, total * SECTOR // (1024 * 1024), what))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
