#!/usr/bin/env python3
"""mkswap.py -- write an EMBKSWAP header onto a raw image.

    python3 tools/mkswap.py build/swap.img 128        # 128 MiB

The image is created sparse if it does not exist. Block 0 becomes the header
(magic, version, page size, slot count, a uuid); every other 4 KiB block is a
swap slot. Nothing else is written: the kernel owns the slots.
"""
import os, struct, sys, uuid
path = sys.argv[1]; mib = int(sys.argv[2]) if len(sys.argv) > 2 else 128
size = mib << 20
with open(path, "wb") as f:
    f.truncate(size)
    hdr = b"EMBKSWAP" + struct.pack("<II", 1, 4096) + struct.pack("<Q", size // 4096) + uuid.uuid4().bytes
    f.seek(0); f.write(hdr.ljust(4096, b"\0"))
print("mkswap: %s: %d slots (%d MiB)" % (path, size // 4096 - 1, mib))
