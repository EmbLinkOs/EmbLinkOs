#!/usr/bin/env python3
"""Is this stick image the one the build produced -- and whole?

A stick image nearly went to a laptop carrying a root filesystem that was not
mkfs's: several targets boot the master embkfs.img writable, the kernel's
boot-time commits land in it, and the stick image is then packed from
whatever master is lying there. It was caught by reading the image's embedded
superblock before handing it over. This is that reading, made a step of
`make stick`, so it cannot be skipped by being in a hurry.

Checks, on the EMBKFS partition inside the GPT image:
  - the superblock is valid and at generation 1 (pristine: no commit has
    touched it since mkfs);
  - the root mirror header (spec v2.3 section 6) is present and names a
    block that is byte-identical to the root;
  - the ESP carries /EFI/BOOT/BOOTX64.EFI (the loader the boot log looks for).
Then prints the sha256 and size, which is what goes into the handover.

Run: python3 tools/stick_check.py uefi-usb.img      (make stick does)
"""
import hashlib
import struct
import sys

SB_OFFSET = 65536
BLOCK = 4096
MIRROR_HDR = 320
EMBKFS_MAGIC = struct.unpack("<Q", b"EMBKFS17")[0]
MIRROR_MAGIC = struct.unpack("<Q", b"EMBKMIR1")[0]


def gpt_partitions(f):
    f.seek(512)
    hdr = f.read(92)
    if hdr[:8] != b"EFI PART":
        raise SystemExit("stick_check: no GPT header")
    pe_lba = struct.unpack_from("<Q", hdr, 72)[0]
    n = struct.unpack_from("<I", hdr, 80)[0]
    esz = struct.unpack_from("<I", hdr, 84)[0]
    f.seek(pe_lba * 512)
    ents = f.read(n * esz)
    out = []
    for i in range(n):
        e = ents[i * esz:(i + 1) * esz]
        if e[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        out.append((first, last))
    return out


def fat_has_loader(f, part_lba):
    """Walk the ESP's FAT32 root -> EFI -> BOOT for BOOTX64.EFI."""
    base = part_lba * 512
    f.seek(base)
    bs = f.read(512)
    bps = struct.unpack_from("<H", bs, 11)[0]
    spc = bs[13]
    rsv = struct.unpack_from("<H", bs, 14)[0]
    nfat = bs[16]
    spf = struct.unpack_from("<I", bs, 36)[0]
    root = struct.unpack_from("<I", bs, 44)[0]
    data = base + (rsv + nfat * spf) * bps

    def cluster(n):
        f.seek(data + (n - 2) * spc * bps)
        return f.read(spc * bps)

    def find(dir_cluster, name11):
        b = cluster(dir_cluster)
        for o in range(0, len(b), 32):
            e = b[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or (e[11] & 0x0F) == 0x0F:
                continue
            if e[0:11] == name11:
                return (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
        return None

    efi = find(root, b"EFI        ")
    if efi is None:
        return False
    boot = find(efi, b"BOOT       ")
    if boot is None:
        return False
    return find(boot, b"BOOTX64 EFI") is not None


def main(path):
    fails = []
    with open(path, "rb") as f:
        parts = gpt_partitions(f)
        if len(parts) < 2:
            raise SystemExit("stick_check: expected an ESP and a root partition, found %d" % len(parts))
        esp_lba, root_lba = parts[0][0], parts[1][0]

        if fat_has_loader(f, esp_lba):
            print("stick_check: ESP has /EFI/BOOT/BOOTX64.EFI")
        else:
            fails.append("the ESP has no /EFI/BOOT/BOOTX64.EFI")

        base = root_lba * 512
        f.seek(base + SB_OFFSET)
        sb = f.read(512)
        magic = struct.unpack_from("<Q", sb, 0)[0]
        if magic != EMBKFS_MAGIC:
            fails.append("partition 2 has no EMBKFS superblock at +65536")
        else:
            gen = struct.unpack_from("<Q", sb, 80)[0]
            total = struct.unpack_from("<Q", sb, 48)[0]
            root_blk, root_csum, root_gen, _ = struct.unpack_from("<QQQQ", sb, 88)
            print("stick_check: EMBKFS generation %d, root block %d, %d blocks" % (gen, root_blk, total))
            if gen != 1:
                fails.append("the root filesystem is at generation %d -- not mkfs's output; the "
                             "master drifted (rm -f embkfs.img && make && make uefi-usb.img)" % gen)
            mmagic = struct.unpack_from("<Q", sb, MIRROR_HDR)[0]
            mblk = struct.unpack_from("<Q", sb, MIRROR_HDR + 8)[0]
            if mmagic != MIRROR_MAGIC:
                fails.append("no root mirror header in the superblock")
            else:
                f.seek(base + root_blk * BLOCK)
                root = f.read(BLOCK)
                f.seek(base + mblk * BLOCK)
                mirr = f.read(BLOCK)
                if root == mirr and 0 < mblk < total and mblk != root_blk:
                    print("stick_check: root mirror at block %d, byte-identical to block %d" % (mblk, root_blk))
                else:
                    fails.append("the root mirror at block %d does not match the root" % mblk)

    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
            size += len(b)
    for x in fails:
        print("  FAIL: %s" % x)
    verdict = "OK" if not fails else "FAIL"
    print("stick_check: %s  sha256 %s  %d bytes  %s" % (path, h.hexdigest(), size, verdict))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "uefi-usb.img"))
