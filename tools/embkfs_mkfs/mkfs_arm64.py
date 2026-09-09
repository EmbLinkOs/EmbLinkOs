#!/usr/bin/env python3
"""Build the MINIMAL aarch64 root filesystem -- docs/ARM64.md phase A6.

    usage: mkfs_arm64.py <out.img> <user-elf-dir> [size-MiB]

WHY A SECOND SCRIPT AND NOT A FLAG ON THE FIRST ONE.

mkfs_embkfs.py builds the x86 boot image, and almost everything it packs is
either x86 MACHINE CODE (every .elf, libembk.so, libtcc1.o, the newlib libc.a
staged at /system/abi for on-OS compilation) or content that only means
something once there is a desktop to show it (icons, wallpapers, music, the
fixture .pkg bundles). None of that exists for aarch64 yet. Teaching that
script to skip three-quarters of itself would make the x86 image's recipe
harder to read in exchange for nothing.

What it does NOT do is reimplement the filesystem. The on-disk format comes
from make_image()/build_root_items() in mkfs_embkfs.py -- the same B-tree
builder, the same superblock, the same crc32c -- so an EMBKFS change cannot
silently apply to one architecture and not the other. This file only decides
WHICH FILES go in, and that list is short and honest.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import layout as L
from mkfs_embkfs import make_image, _read_font

# /system/bin is the sealed system tree (docs/USERSPACE.md): the kernel spawns
# out of it and userland cannot write to it. Anything else discovered in the
# build directory lands under /data/apps/<name>/, which is the same placement
# rule _elf_dest() applies on x86.
_SYSTEM_BIN = {"init.elf", "hello.elf", "shell.elf"}


def _dest(name: str) -> bytes:
    if name in _SYSTEM_BIN:
        return b"system/bin/" + name.encode()
    return f"data/apps/{name[:-4]}/{name}".encode()


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())

    out_path = argv[1]
    user_dir = argv[2]
    size_mib = int(argv[3]) if len(argv) > 3 else 16

    if not os.path.isdir(user_dir):
        sys.exit(f"mkfs_arm64: {user_dir} is not a directory -- build the "
                 f"userland first (make ARCH=aarch64 arm64-user)")

    elves = sorted(f for f in os.listdir(user_dir) if f.endswith(".elf"))
    if not elves:
        sys.exit(f"mkfs_arm64: no *.elf in {user_dir} -- nothing to pack")

    objects = []
    for name in elves:
        with open(os.path.join(user_dir, name), "rb") as fh:
            data = fh.read()

        # Refuse to pack the WRONG ARCHITECTURE. This is the failure this
        # script exists to make impossible: build/ holds x86 objects, the two
        # trees sit side by side, and an x86 binary on the arm image would
        # get all the way to the kernel's ELF loader before anything noticed
        # -- where it fails as "bad machine type", a long way from the cause.
        #   e_machine is a 2-byte LE field at offset 18; 0xB7 == EM_AARCH64.
        if len(data) < 20 or data[:4] != b"\x7fELF":
            sys.exit(f"mkfs_arm64: {name} is not an ELF file")
        machine = data[18] | (data[19] << 8)
        if machine != 0xB7:
            sys.exit(f"mkfs_arm64: {name} is e_machine 0x{machine:x}, "
                     f"not 0xb7 (EM_AARCH64) -- that is an x86 binary")

        objects.append((_dest(name), L.DT_REG, L.S_IFREG | 0o755, data))

    # The SEALED ABI. /system/lib/libembk.so is the ELF loader's ONE hardwired
    # library path -- every dynamic app's DT_NEEDED resolves there and nowhere
    # else (kernel/loader/elf.c) -- so an EmUI app without this file fails at
    # load with "not found" rather than at link.
    so_path = os.path.join(user_dir, "libembk.so")
    if os.path.exists(so_path):
        with open(so_path, "rb") as fh:
            objects.append((b"system/lib/libembk.so", L.DT_REG,
                            L.S_IFREG | 0o755, fh.read()))

    # The UI fonts, from the HOST's DejaVu install, exactly as the x86 image
    # gets them (_read_font is the same finder, so the two images cannot end up
    # with different faces). The toolkit renders no text without these -- a
    # window comes up empty rather than failing, which is a worse symptom than
    # a missing file, so say so at pack time.
    for host_name, image_name in (("DejaVuSans.ttf", b"system/fonts/font.ttf"),
                                  ("DejaVuSansMono.ttf", b"system/fonts/mono.ttf")):
        data = _read_font(host_name)
        if data is None:
            print(f"mkfs_arm64: WARNING: {host_name} not found on this host -- "
                  f"EmUI text will not render")
            continue
        objects.append((image_name, L.DT_REG, L.S_IFREG | L.PERM_FILE, data))

    # The writable tree. Created EMPTY rather than left out: userland expects
    # these to exist (syscalls.c resolves relative paths against /, and a
    # program that opens /data/... for writing should find a directory there,
    # not ENOENT). An explicit DT_DIR object is how build_root_items() is told
    # to make a directory with nothing in it.
    for d in (b"data", b"data/tmp", b"data/apps", b"system", b"system/bin",
              b"system/lib", b"system/fonts"):
        objects.append((d, L.DT_DIR, L.S_IFDIR | L.PERM_DIR, b""))

    make_image(out_path, size_bytes=size_mib * 1024 * 1024, objects=objects)

    print(f"mkfs_arm64: {out_path}  ({size_mib} MiB)")
    for name in elves:
        print(f"    /{_dest(name).decode():<28} "
              f"{os.path.getsize(os.path.join(user_dir, name)):>9} bytes")


if __name__ == "__main__":
    main(sys.argv)
