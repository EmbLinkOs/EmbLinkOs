#!/usr/bin/env python3
"""tools/kernel_fits.py -- does the kernel still fit where it must be loaded?

THE KERNEL IS NOT RELOCATABLE. It is linked for one physical address
(0x100000) because KP2V hardwires the higher-half window to physical 0, and
both boot paths -- BIOS stage2 and the UEFI loader -- place it there exactly.
So the whole image, .bss included, has to land in memory the firmware has left
free, and firmware reserves low memory wherever it likes: OVMF puts ACPI NVS
at 0x800000, eight megabytes in.

This is what happens when it does not fit, and it is why this check exists:
the loader asks the firmware for the pages, the firmware refuses, and the
machine stops before the kernel's first instruction with nothing on screen but
"EmbBoot FATAL". Measured, on the day the stick was first booted the way a
laptop boots it -- the image had reached 0x801000, ONE PAGE over.

The ceiling is below the lowest reservation seen so far, with room to notice
the problem before a machine does. Growing past it is not forbidden -- it is a
decision, and it needs the per-core fault stacks out of .bss (MAX_CPUS in
kernel/arch/x86_64/cpu/gdt.h) or a relocatable kernel, not a bigger number
here.

Usage: kernel_fits.py <kernel.elf> [ceiling-bytes]
"""
import subprocess, sys

KVB = 0xFFFFFFFF80000000
DEFAULT_CEILING = 0x7C0000          # 7.75 MiB; firmware seen reserving 8.00 MiB


def main():
    elf = sys.argv[1]
    ceiling = int(sys.argv[2], 0) if len(sys.argv) > 2 else DEFAULT_CEILING
    for readelf in ("x86_64-elf-readelf", "readelf", "llvm-readelf"):
        try:
            out = subprocess.run([readelf, "-lW", elf], capture_output=True, text=True).stdout
            if "LOAD" in out:
                break
        except FileNotFoundError:
            continue
    else:
        print("kernel_fits: no readelf available -- check skipped", file=sys.stderr)
        return 0

    lo, hi = None, 0
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "LOAD":
            vaddr, memsz = int(f[2], 16), int(f[5], 16)
            phys = vaddr - KVB if vaddr >= KVB else vaddr
            lo = phys if lo is None else min(lo, phys)
            hi = max(hi, phys + memsz)
    if lo is None:
        print("kernel_fits: %s has no PT_LOAD segments" % elf, file=sys.stderr)
        return 1

    if hi > ceiling:
        print("", file=sys.stderr)
        print("*** THE KERNEL NO LONGER FITS WHERE IT IS LOADED", file=sys.stderr)
        print("***   image: 0x%06X..0x%06X  (%.2f MiB, top at %.3f MiB)"
              % (lo, hi, (hi - lo) / 1048576.0, hi / 1048576.0), file=sys.stderr)
        print("***   ceiling: %.3f MiB -- firmware reserves low memory above it"
              % (ceiling / 1048576.0), file=sys.stderr)
        print("***   A machine would stop at \"EmbBoot FATAL\" before the kernel runs.", file=sys.stderr)
        print("***   Shrink the static footprint (x86_64-elf-nm -S --size-sort -td", file=sys.stderr)
        print("***   kernel/kernel.elf | tail) -- see tools/kernel_fits.py.", file=sys.stderr)
        print("", file=sys.stderr)
        return 1

    print("kernel fits: top at %.3f MiB of %.3f MiB allowed (%.0f KiB spare)"
          % (hi / 1048576.0, ceiling / 1048576.0, (ceiling - hi) / 1024.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
