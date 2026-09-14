#!/usr/bin/env python3
"""mkgpt.py -- write a GPT disk image with a chosen LOGICAL BLOCK SIZE.

    python3 tools/mkgpt.py OUT.img [--block 4096] [--mib 64] [--parts 2]

WHY THIS EXISTS. Everything in GPT is counted in logical blocks: the header is
in block 1, the entry array starts at a block number, and how many entries fit
in one read is the block size divided by the entry size. A parser that assumes
512 works on every image anyone here has ever built -- because every tool that
builds them assumes 512 too -- and fails on a 4Kn drive, which many NVMe parts
are or can be formatted as.

So the point of this tool is to produce the image nothing else here can: the
same layout with a different block size, so the difference is the only
variable. Each partition's first block carries a signature the guest reads
back, because "a partition was registered" and "the partition addresses the
right sectors" are different claims.
"""
import os
import struct
import sys

# The CRC32 GPT uses is the ordinary IEEE one; zlib has it.
from zlib import crc32

EFI_SYSTEM = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")     # C12A7328-...
BASIC_DATA = bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7")     # EBD0A0A2-...


def guid_for(i):
    """A distinct, stable unique-GUID per partition. Stable so two runs of the
    build produce identical bytes."""
    return bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                  0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x10 + i])


def build(path, block=512, total_mib=64, nparts=2):
    bs = block
    total_blocks = (total_mib * 1024 * 1024) // bs
    if total_blocks < 128:
        raise SystemExit("mkgpt: too small for a GPT at this block size")

    entries_bytes = 128 * 128                     # 128 entries of 128 bytes
    entry_blocks = (entries_bytes + bs - 1) // bs
    first_usable = 2 + entry_blocks               # protective MBR, header, array
    last_usable = total_blocks - 2 - entry_blocks

    # Split the usable range evenly. Each partition's LAST block is INCLUSIVE,
    # which is the single most common thing to get wrong about a GPT entry.
    span = (last_usable - first_usable + 1) // nparts
    parts = []
    for i in range(nparts):
        start = first_usable + i * span
        end = start + span - 1
        parts.append((start, end))

    entries = bytearray(entries_bytes)
    for i, (start, end) in enumerate(parts):
        off = i * 128
        entries[off:off + 16] = EFI_SYSTEM if i == 0 else BASIC_DATA
        entries[off + 16:off + 32] = guid_for(i)
        struct.pack_into("<QQQ", entries, off + 32, start, end, 0)
        name = ("EMBK%d" % i).encode("utf-16-le")
        entries[off + 56:off + 56 + len(name)] = name
    entries_crc = crc32(bytes(entries)) & 0xFFFFFFFF

    def header(my_lba, alt_lba, entry_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<III", h, 8, 0x00010000, 92, 0)   # rev, size, crc=0
        struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, first_usable, last_usable)
        h[56:72] = bytes.fromhex("0102030405060708090a0b0c0d0e0f10")
        struct.pack_into("<QIII", h, 72, entry_lba, 128, 128, entries_crc)
        struct.pack_into("<I", h, 16, crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h)

    with open(path, "wb") as f:
        f.truncate(total_blocks * bs)

        # THE PROTECTIVE MBR IS STILL 512 BYTES AT THE START OF BLOCK 0, whatever
        # a block is on this disk. It exists so that a tool which only speaks
        # MBR sees one partition covering everything and leaves the disk alone.
        mbr = bytearray(bs)
        e = 446
        mbr[e + 0] = 0x00
        mbr[e + 4] = 0xEE                                  # GPT protective
        struct.pack_into("<I", mbr, e + 8, 1)              # first LBA
        span32 = min(total_blocks - 1, 0xFFFFFFFF)
        struct.pack_into("<I", mbr, e + 12, span32)
        mbr[510] = 0x55
        mbr[511] = 0xAA
        f.seek(0); f.write(bytes(mbr))

        f.seek(1 * bs); f.write(header(1, total_blocks - 1, 2).ljust(bs, b"\x00"))
        f.seek(2 * bs); f.write(bytes(entries))

        # The backup: entry array first, then the header in the very last block.
        alt_entries_lba = total_blocks - 1 - entry_blocks
        f.seek(alt_entries_lba * bs); f.write(bytes(entries))
        f.seek((total_blocks - 1) * bs)
        f.write(header(total_blocks - 1, 1, alt_entries_lba).ljust(bs, b"\x00"))

        # A signature in each partition's first block, so the guest can prove it
        # is addressing the sectors the table named rather than merely counting
        # entries.
        for i, (start, _end) in enumerate(parts):
            f.seek(start * bs)
            f.write(("EMBLINK-PART%d" % i).encode() + b"\x00")

    print("mkgpt: %s, %d-byte blocks, %d blocks, %d partition(s)"
          % (path, bs, total_blocks, nparts))
    for i, (start, end) in enumerate(parts):
        print("       part%d: blocks %d..%d (%d MiB)"
              % (i, start, end, ((end - start + 1) * bs) >> 20))
    return parts


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    block, mib, nparts = 512, 64, 2
    args = sys.argv[2:]
    while args:
        a = args.pop(0)
        if a == "--block": block = int(args.pop(0))
        elif a == "--mib": mib = int(args.pop(0))
        elif a == "--parts": nparts = int(args.pop(0))
        else: raise SystemExit("mkgpt: unknown argument %s" % a)
    build(out, block, mib, nparts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
