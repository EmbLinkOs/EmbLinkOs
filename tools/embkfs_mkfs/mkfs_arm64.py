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
from mkfs_embkfs import (make_image, _read_font, _tree_objects,
                          FIXTURE_OBJECTS, _prog_meta,
                          _SYSTEM_BIN as _X86_SYSTEM_BIN)

# /system/bin is the sealed system tree (docs/USERSPACE.md): the kernel spawns
# out of it and userland cannot write to it. Everything else lands under
# /data/apps/<name>/.
#
# IMPORTED from the x86 packer, not retyped, because it is not a preference --
# it is a CONTRACT WITH init.c, which hardcodes "/system/bin/home.elf",
# "/system/bin/login.elf" and "/system/bin/setup.elf". A second list here that
# drifted from that one would put the desktop somewhere init cannot spawn it,
# and the symptom would be init reporting a failure to launch a file that is
# plainly on the image.
#
# hello.elf and posixdemo.elf are the additions: both are boot WITNESSES,
# spawned by the kernel itself rather than by init, and they belong in the
# sealed tree for the same reason init does.
# lockdemo.elf joins the two the ARM boot self-test already spawns by absolute
# path: the futex case in early.c runs "/system/bin/lockdemo.elf", and a
# program the kernel names has to be where the kernel looks.
_SYSTEM_BIN = _X86_SYSTEM_BIN | {"hello.elf", "posixdemo.elf", "lockdemo.elf",
                                "jitter.elf"}

# Must match DEV_USER in user/system/init/init.c -- init auto-logs in as this name and
# binds /home/<name> into the desktop's namespace.
DEV_USER = b"yves"


def _dest(name: str) -> bytes:
    if name in _SYSTEM_BIN:
        return b"system/bin/" + name.encode()
    return f"data/apps/{name[:-4]}/{name}".encode()


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())

    out_path = argv[1]
    user_dir = argv[2]
    # 64 MiB. It was 16 while the image held two programs; thirty EmUI apps at
    # ~700 KB each plus two fonts went straight past it, and the way that
    # announces itself is a struct.error deep inside pack_superblock_body(),
    # because free_blocks went NEGATIVE and 'Q' will not take it. The main
    # mkfs records the same growth story for the same reason. The image is
    # sparse on disk and the kernel reads total_blocks from the superblock, so
    # over-sizing costs nothing. 64 -> 128 when /system/images joined: the icon
    # and wallpaper tree is ~20 MiB on its own.
    size_mib = int(argv[3]) if len(argv) > 3 else 128

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

        # The THREE per-app manifests, packed beside the binary exactly as the
        # x86 image packs them. They are not decoration:
        #   <app>.ns    what the app may NAME  (namespace binds)
        #   <app>.caps  what the app may DO    (capability classes)
        #   <app>.app   how it presents itself (display name + icon)
        # The desktop reads .app to build the launcher instead of hard-coding
        # each app's name, so without them the log fills with `"files.app" not
        # found` and the launcher comes up empty -- which is what happened the
        # first time the real session ran here.
        base = name[:-4]
        dest = _dest(name)
        if dest.startswith(b"data/apps/"):
            for suffix in ("ns", "caps", "app"):
                # _prog_meta, SHARED with the x86 image builder, because a
                # program's manifests live in the program's own directory now
                # (user/apps/<name>/, user/tests/<name>/, ...) and only the NAME
                # is known here. Composing the path from parts locally is how
                # this broke: a tree-wide rewrite of "user/bin/..." literals
                # could not see it, so the aarch64 launcher came up empty and
                # the x86 one did not. One function, one place to be wrong.
                side = _prog_meta(base, suffix)
                if os.path.exists(side):
                    with open(side, "rb") as sf:
                        objects.append((f"data/apps/{base}/{base}.{suffix}".encode(),
                                        L.DT_REG, L.S_IFREG | L.PERM_FILE, sf.read()))

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

    # ICONS AND THE WALLPAPER. /system/images is what the desktop draws itself
    # from: the dock's app icons (.eic) and the backdrop (.ppm). Without it the
    # session still comes up -- menu bar, clock, dock -- but on flat black with
    # an empty dock, which is what the first real session here looked like.
    # Same helper and same destination as the x86 image, so the two cannot
    # disagree about where an icon lives.
    objects.extend(_tree_objects("system/images", b"system/images/",
                                 (".ppm", ".pam", ".eic")))

    # The on-disk FORMAT fixtures the x86 image also carries, IMPORTED rather
    # than retyped: /hello.txt, a symlink, and the two files whose names share a
    # CRC32C hash (wgyehkb/illoeuw -> one dir-entry item, the collision-chain
    # regression). posixdemo reads /hello.txt by relative and absolute path, so
    # without them its filesystem section fails for want of a file rather than
    # for want of a working filesystem.
    objects.extend(FIXTURE_OBJECTS)

    # The writable tree, and it is a CONTRACT rather than tidiness. A directory
    # that does not exist cannot be granted: the kernel resolves an ns-bind
    # prefix in the PARENT's namespace at spawn time, so an app cannot create
    # what it was never given. init spawns the desktop with `rw /home/<user>`
    # and `rw /run`, so both must be on the image before init runs.
    #
    # This is the same set the x86 packer commits to (D4 §5, D3 §4.1), and it
    # was learned the same way: with /home missing, init's mkdir loop failed
    # every time and the serial log filled with `"home" not found` while it
    # retried forever. An explicit DT_DIR object is how build_root_items() is
    # told to make a directory with nothing in it.
    dirs = [b"data", b"data/tmp", b"data/apps", b"system", b"system/bin",
            b"system/lib", b"system/fonts", b"run", b"home", b"etc",
            b"home/" + DEV_USER]
    dirs += [b"home/" + DEV_USER + b"/" + d for d in
             (b"Desktop", b"Documents", b"Downloads", b"Music", b"Pictures",
              b"Videos", b"Trash", b".vellum")]
    for d in dirs:
        objects.append((d, L.DT_DIR, L.S_IFDIR | L.PERM_DIR, b""))

    # init reads /etc/shadow to decide whether this is a first boot. Present and
    # EMPTY is the honest state for an image with no accounts; with
    # DEV_AUTOLOGIN it is not consulted, but its absence would send a
    # non-DEV build into the setup flow on every boot.
    objects.append((b"etc/passwd", L.DT_REG, L.S_IFREG | 0o644, b""))
    objects.append((b"etc/shadow", L.DT_REG, L.S_IFREG | 0o600, b""))
    objects.append((b"home/" + DEV_USER + b"/readme.txt", L.DT_REG,
                    L.S_IFREG | L.PERM_FILE,
                    b"Welcome to EmbLink on aarch64.\n"))

    # Say it in words BEFORE struct.pack says it in hex. A rough block count is
    # enough: this is a guard against "the image is too small", not an
    # allocator.
    need = sum((len(o[3]) + 4095) // 4096 for o in objects) + 64
    have = (size_mib * 1024 * 1024) // 4096
    if need > have:
        sys.exit(f"mkfs_arm64: content needs ~{need} blocks but the image has "
                 f"{have} ({size_mib} MiB). Pass a bigger size as argv[3], or "
                 f"raise the default in this file.")

    make_image(out_path, size_bytes=size_mib * 1024 * 1024, objects=objects)

    print(f"mkfs_arm64: {out_path}  ({size_mib} MiB)")
    for name in elves:
        print(f"    /{_dest(name).decode():<28} "
              f"{os.path.getsize(os.path.join(user_dir, name)):>9} bytes")


if __name__ == "__main__":
    main(sys.argv)
